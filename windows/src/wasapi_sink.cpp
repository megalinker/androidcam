#include "wasapi_sink.h"

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <mmreg.h>
#include <cstdio>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <algorithm>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}

namespace {

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

// Wide friendly name -> narrow, for logging + matching.
std::string toNarrow(const wchar_t* w) {
    if (!w) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

// Pick the FFmpeg output sample format that matches the endpoint mix format.
bool mixToAvFormat(const WAVEFORMATEX* wf, AVSampleFormat* out) {
    bool isFloat = wf->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
    if (wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(wf);
        isFloat = IsEqualGUID(ext->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) != 0;
    }
    if (isFloat && wf->wBitsPerSample == 32) { *out = AV_SAMPLE_FMT_FLT; return true; }
    if (!isFloat && wf->wBitsPerSample == 16) { *out = AV_SAMPLE_FMT_S16; return true; }
    if (!isFloat && wf->wBitsPerSample == 32) { *out = AV_SAMPLE_FMT_S32; return true; }
    return false;
}

// Soft limiter: transparent below the knee, then smoothly approaches +/-1.0 so a boosted
// signal compresses instead of hard-clipping into crackle.
inline float softLimit(float x) {
    const float t = 0.8f;
    float a = std::fabs(x);
    if (a <= t) return x;
    float sign = x < 0.0f ? -1.0f : 1.0f;
    return sign * (t + (1.0f - t) * (1.0f - std::exp(-(a - t) / (1.0f - t))));
}

} // namespace

struct WasapiSink::Impl {
    bool comInited = false;
    IMMDeviceEnumerator* enumr = nullptr;
    IMMDevice* device = nullptr;
    IAudioClient* client = nullptr;
    IAudioRenderClient* render = nullptr;
    WAVEFORMATEX* mix = nullptr;

    SwrContext* swr = nullptr;
    AVChannelLayout outLayout{};
    AVSampleFormat outFmt = AV_SAMPLE_FMT_S16;
    int outRate = 48000;
    int outChannels = 2;
    int blockAlign = 4;
    UINT32 bufferFrames = 0;

    uint8_t* buf = nullptr;   // interleaved resample scratch
    int bufSamples = 0;
    bool started = false;
    float gainLinear = 1.0f;   // mic boost (linear), applied with a soft limiter before render
};

WasapiSink::WasapiSink() : p_(new Impl) {}
WasapiSink::~WasapiSink() { Stop(); delete p_; p_ = nullptr; }

bool WasapiSink::Init(const AVCodecContext* dec, const std::string& deviceMatch, float gainDb) {
    Impl& s = *p_;
    s.gainLinear = std::pow(10.0f, gainDb / 20.0f);

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    s.comInited = SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE;

    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                          __uuidof(IMMDeviceEnumerator), (void**)&s.enumr);
    if (FAILED(hr)) { fprintf(stderr, "[audio] MMDeviceEnumerator failed 0x%08lx\n", hr); return false; }

    if (deviceMatch.empty()) {
        hr = s.enumr->GetDefaultAudioEndpoint(eRender, eConsole, &s.device);
        if (FAILED(hr)) { fprintf(stderr, "[audio] no default render endpoint\n"); return false; }
    } else {
        IMMDeviceCollection* coll = nullptr;
        hr = s.enumr->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &coll);
        if (FAILED(hr)) return false;
        UINT count = 0; coll->GetCount(&count);
        std::string want = lower(deviceMatch);
        for (UINT i = 0; i < count && !s.device; ++i) {
            IMMDevice* d = nullptr;
            if (FAILED(coll->Item(i, &d))) continue;
            IPropertyStore* props = nullptr;
            if (SUCCEEDED(d->OpenPropertyStore(STGM_READ, &props))) {
                PROPVARIANT name; PropVariantInit(&name);
                if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &name)) && name.pwszVal) {
                    if (lower(toNarrow(name.pwszVal)).find(want) != std::string::npos) {
                        s.device = d; d = nullptr; // keep
                    }
                }
                PropVariantClear(&name);
                props->Release();
            }
            if (d) d->Release();
        }
        coll->Release();
        if (!s.device) { fprintf(stderr, "[audio] no render endpoint matching \"%s\"\n", deviceMatch.c_str()); return false; }
    }

    {
        IPropertyStore* props = nullptr;
        if (SUCCEEDED(s.device->OpenPropertyStore(STGM_READ, &props))) {
            PROPVARIANT name; PropVariantInit(&name);
            if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &name)) && name.pwszVal)
                fprintf(stderr, "[audio] endpoint: %s\n", toNarrow(name.pwszVal).c_str());
            PropVariantClear(&name);
            props->Release();
        }
    }

    hr = s.device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&s.client);
    if (FAILED(hr)) return false;

    hr = s.client->GetMixFormat(&s.mix);
    if (FAILED(hr) || !s.mix) return false;

    if (!mixToAvFormat(s.mix, &s.outFmt)) {
        fprintf(stderr, "[audio] unsupported mix format (bits=%u tag=%u)\n",
                s.mix->wBitsPerSample, s.mix->wFormatTag);
        return false;
    }
    s.outRate = (int)s.mix->nSamplesPerSec;
    s.outChannels = s.mix->nChannels;
    s.blockAlign = s.mix->nBlockAlign;
    av_channel_layout_default(&s.outLayout, s.outChannels);

    // 200 ms shared-mode buffer.
    const REFERENCE_TIME kBufDuration = 2000000; // 100-ns units
    hr = s.client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, kBufDuration, 0, s.mix, nullptr);
    if (FAILED(hr)) { fprintf(stderr, "[audio] IAudioClient::Initialize 0x%08lx\n", hr); return false; }

    hr = s.client->GetBufferSize(&s.bufferFrames);
    if (FAILED(hr)) return false;

    hr = s.client->GetService(__uuidof(IAudioRenderClient), (void**)&s.render);
    if (FAILED(hr)) return false;

    // Resampler: decoder format -> endpoint mix format (interleaved).
    int ret = swr_alloc_set_opts2(&s.swr,
                                  &s.outLayout,     s.outFmt,       s.outRate,
                                  &dec->ch_layout,  dec->sample_fmt, dec->sample_rate,
                                  0, nullptr);
    if (ret < 0 || swr_init(s.swr) < 0) { fprintf(stderr, "[audio] swr init failed\n"); return false; }

    hr = s.client->Start();
    if (FAILED(hr)) return false;
    s.started = true;
    fprintf(stderr, "[audio] rendering: %d Hz, %d ch, %s (boost %.1f dB)\n",
            s.outRate, s.outChannels, av_get_sample_fmt_name(s.outFmt), 20.0f * std::log10(s.gainLinear));
    return true;
}

bool WasapiSink::WriteFrame(const AVFrame* frame) {
    Impl& s = *p_;
    if (!s.started) return false;

    int outNb = swr_get_out_samples(s.swr, frame->nb_samples);
    if (outNb <= 0) return true;

    if (outNb > s.bufSamples) {
        av_freep(&s.buf);
        int ls;
        if (av_samples_alloc(&s.buf, &ls, s.outChannels, outNb, s.outFmt, 0) < 0) return false;
        s.bufSamples = outNb;
    }

    int got = swr_convert(s.swr, &s.buf, outNb,
                          (const uint8_t* const*)frame->extended_data, frame->nb_samples);
    if (got < 0) return false;

    // Mic boost: gain then soft-limit, in whatever interleaved sample format the endpoint wants.
    if (s.gainLinear != 1.0f && got > 0) {
        int n = got * s.outChannels;
        if (s.outFmt == AV_SAMPLE_FMT_FLT) {
            float* f = reinterpret_cast<float*>(s.buf);
            for (int i = 0; i < n; ++i) f[i] = softLimit(f[i] * s.gainLinear);
        } else if (s.outFmt == AV_SAMPLE_FMT_S16) {
            int16_t* q = reinterpret_cast<int16_t*>(s.buf);
            for (int i = 0; i < n; ++i) {
                float v = softLimit((q[i] / 32768.0f) * s.gainLinear);
                q[i] = (int16_t)std::lrint(v * 32767.0f);
            }
        } else if (s.outFmt == AV_SAMPLE_FMT_S32) {
            int32_t* q = reinterpret_cast<int32_t*>(s.buf);
            for (int i = 0; i < n; ++i) {
                float v = softLimit((float)(q[i] / 2147483648.0) * s.gainLinear);
                q[i] = (int32_t)std::llrint((double)v * 2147483647.0);
            }
        }
    }

    // Push into the WASAPI render buffer, waiting for space as it drains at real time.
    int written = 0, guard = 0;
    while (written < got && guard++ < 1000) {
        UINT32 padding = 0;
        if (FAILED(s.client->GetCurrentPadding(&padding))) return false;
        UINT32 avail = s.bufferFrames - padding;
        if (avail == 0) { Sleep(2); continue; }
        UINT32 chunk = std::min<UINT32>(avail, (UINT32)(got - written));
        BYTE* dst = nullptr;
        if (FAILED(s.render->GetBuffer(chunk, &dst))) return false;
        memcpy(dst, s.buf + (size_t)written * s.blockAlign, (size_t)chunk * s.blockAlign);
        s.render->ReleaseBuffer(chunk, 0);
        written += chunk;
    }
    return true;
}

void WasapiSink::Stop() {
    if (!p_) return;
    Impl& s = *p_;
    if (s.client && s.started) { s.client->Stop(); s.started = false; }
    av_freep(&s.buf); s.bufSamples = 0;
    if (s.swr) { swr_free(&s.swr); }
    av_channel_layout_uninit(&s.outLayout);
    if (s.render) { s.render->Release(); s.render = nullptr; }
    if (s.client) { s.client->Release(); s.client = nullptr; }
    if (s.mix) { CoTaskMemFree(s.mix); s.mix = nullptr; }
    if (s.device) { s.device->Release(); s.device = nullptr; }
    if (s.enumr) { s.enumr->Release(); s.enumr = nullptr; }
    if (s.comInited) { CoUninitialize(); s.comInited = false; }
}
