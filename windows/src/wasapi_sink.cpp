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
#include <cstdlib>
#include <algorithm>
#include <vector>

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

// One RBJ-cookbook biquad. Per-channel state (up to 8 endpoint channels), Direct-Form-II Transposed.
// An EQ is just a std::vector of these — any number of bands the user wants.
struct Biquad {
    float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    float z1[8] = {0}, z2[8] = {0};
    inline float process(float x, int ch) {
        float y = b0 * x + z1[ch];
        z1[ch] = b1 * x - a1 * y + z2[ch];
        z2[ch] = b2 * x - a2 * y;
        return y;
    }
    void set(float B0, float B1, float B2, float A0, float A1, float A2) {
        b0 = B0 / A0; b1 = B1 / A0; b2 = B2 / A0; a1 = A1 / A0; a2 = A2 / A0;
    }
    void peak(float f, float Q, float dB, float Fs) {
        float w = 6.2831853f * f / Fs, c = std::cos(w), al = std::sin(w) / (2 * Q), A = std::pow(10.f, dB / 40);
        set(1 + al * A, -2 * c, 1 - al * A, 1 + al / A, -2 * c, 1 - al / A);
    }
    void highpass(float f, float Q, float Fs) {
        float w = 6.2831853f * f / Fs, c = std::cos(w), al = std::sin(w) / (2 * Q);
        set((1 + c) / 2, -(1 + c), (1 + c) / 2, 1 + al, -2 * c, 1 - al);
    }
    void lowpass(float f, float Q, float Fs) {
        float w = 6.2831853f * f / Fs, c = std::cos(w), al = std::sin(w) / (2 * Q);
        set((1 - c) / 2, 1 - c, (1 - c) / 2, 1 + al, -2 * c, 1 - al);
    }
    void lowshelf(float f, float Q, float dB, float Fs) {
        float w = 6.2831853f * f / Fs, c = std::cos(w), A = std::pow(10.f, dB / 40), sa = 2 * std::sqrt(A) * (std::sin(w) / (2 * Q));
        set(A * ((A + 1) - (A - 1) * c + sa), 2 * A * ((A - 1) - (A + 1) * c), A * ((A + 1) - (A - 1) * c - sa),
            (A + 1) + (A - 1) * c + sa, -2 * ((A - 1) + (A + 1) * c), (A + 1) + (A - 1) * c - sa);
    }
    void highshelf(float f, float Q, float dB, float Fs) {
        float w = 6.2831853f * f / Fs, c = std::cos(w), A = std::pow(10.f, dB / 40), sa = 2 * std::sqrt(A) * (std::sin(w) / (2 * Q));
        set(A * ((A + 1) + (A - 1) * c + sa), -2 * A * ((A - 1) + (A + 1) * c), A * ((A + 1) + (A - 1) * c - sa),
            (A + 1) - (A - 1) * c + sa, 2 * ((A - 1) - (A + 1) * c), (A + 1) - (A - 1) * c - sa);
    }
};

// Named presets expand to a band list; anything else is treated as a band list already.
std::string expandEqPreset(const std::string& name) {
    std::string p = lower(name);
    // Gentle, "tasteful" presets (2-4 dB moves).
    if (p == "clarity") return "hp:80:0.7:0;peak:300:1:-2.5;peak:3500:1:3";
    if (p == "warm")    return "hp:70:0.7:0;ls:200:0.7:3;peak:3000:1:1.5;hs:9000:0.7:-2";
    if (p == "bright")  return "hp:80:0.7:0;peak:4000:1.2:4;hs:10000:0.7:3";
    if (p == "podcast") return "hp:80:0.7:0;peak:250:1:-2;peak:3000:1:2.5;hs:9000:0.7:1.5";
    // Radical "+" variants of each: same character, much bigger moves (5-7 dB) so the effect is obvious.
    if (p == "clarity+") return "hp:90:0.7:0;peak:320:1.1:-5;peak:3500:1.2:6";
    if (p == "warm+")    return "hp:70:0.7:0;ls:230:0.7:6;peak:2800:1:3;hs:9000:0.7:-4";
    if (p == "bright+")  return "hp:85:0.7:0;peak:250:1:-3;peak:4500:1.3:7;hs:11000:0.7:6";
    if (p == "podcast+") return "hp:85:0.7:0;peak:250:1.1:-4;peak:3200:1:5;hs:9500:0.7:4";
    return name;
}

// Parse "type:freq:q:gain;type:freq:q:gain;..." (any number of bands) into a biquad cascade.
// types: peak, hp (high-pass), lp (low-pass), ls (low-shelf), hs (high-shelf).
std::vector<Biquad> buildEq(const std::string& specIn, float Fs) {
    std::vector<Biquad> bands;
    std::string spec = expandEqPreset(specIn);
    if (spec.empty() || lower(spec) == "off") return bands;
    size_t pos = 0;
    while (pos < spec.size()) {
        size_t semi = spec.find(';', pos);
        std::string tok = spec.substr(pos, semi == std::string::npos ? spec.size() - pos : semi - pos);
        pos = (semi == std::string::npos) ? spec.size() : semi + 1;
        std::string part[4]; int np = 0; size_t p2 = 0;
        while (np < 4) {
            size_t colon = tok.find(':', p2);
            part[np++] = tok.substr(p2, colon == std::string::npos ? tok.size() - p2 : colon - p2);
            if (colon == std::string::npos) break;
            p2 = colon + 1;
        }
        if (np < 1 || part[0].empty()) continue;
        std::string type = lower(part[0]);
        float freq = np > 1 ? (float)atof(part[1].c_str()) : 1000.f;
        float q    = np > 2 ? (float)atof(part[2].c_str()) : 0.707f;
        float gain = np > 3 ? (float)atof(part[3].c_str()) : 0.f;
        if (q <= 0.f) q = 0.707f;
        if (freq <= 0.f || freq >= Fs / 2) continue;
        Biquad b;
        if (type == "peak") b.peak(freq, q, gain, Fs);
        else if (type == "hp") b.highpass(freq, q, Fs);
        else if (type == "lp") b.lowpass(freq, q, Fs);
        else if (type == "ls") b.lowshelf(freq, q, gain, Fs);
        else if (type == "hs") b.highshelf(freq, q, gain, Fs);
        else continue;
        bands.push_back(b);
    }
    return bands;
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
    std::vector<Biquad> eq;    // voice EQ cascade (any number of bands), applied before the gain
};

WasapiSink::WasapiSink() : p_(new Impl) {}
WasapiSink::~WasapiSink() { Stop(); delete p_; p_ = nullptr; }

bool WasapiSink::Init(const AVCodecContext* dec, const std::string& deviceMatch, float gainDb, const std::string& eqPreset) {
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
    s.eq = buildEq(eqPreset, (float)s.outRate);   // voice EQ cascade (empty = off)

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
    fprintf(stderr, "[audio] rendering: %d Hz, %d ch, %s (boost %.1f dB, eq %d bands)\n",
            s.outRate, s.outChannels, av_get_sample_fmt_name(s.outFmt),
            20.0f * std::log10(s.gainLinear), (int)s.eq.size());
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

    // Voice EQ (per-channel biquad cascade) -> mic boost -> soft limit, in the endpoint's sample format.
    if ((s.gainLinear != 1.0f || !s.eq.empty()) && got > 0 && s.outChannels <= 8) {
        int n = got * s.outChannels, nch = s.outChannels;
        if (s.outFmt == AV_SAMPLE_FMT_FLT) {
            float* f = reinterpret_cast<float*>(s.buf);
            for (int i = 0; i < n; ++i) {
                float x = f[i]; int ch = i % nch;
                for (auto& b : s.eq) x = b.process(x, ch);
                f[i] = softLimit(x * s.gainLinear);
            }
        } else if (s.outFmt == AV_SAMPLE_FMT_S16) {
            int16_t* q = reinterpret_cast<int16_t*>(s.buf);
            for (int i = 0; i < n; ++i) {
                float x = q[i] / 32768.0f; int ch = i % nch;
                for (auto& b : s.eq) x = b.process(x, ch);
                q[i] = (int16_t)std::lrint(softLimit(x * s.gainLinear) * 32767.0f);
            }
        } else if (s.outFmt == AV_SAMPLE_FMT_S32) {
            int32_t* q = reinterpret_cast<int32_t*>(s.buf);
            for (int i = 0; i < n; ++i) {
                float x = (float)(q[i] / 2147483648.0); int ch = i % nch;
                for (auto& b : s.eq) x = b.process(x, ch);
                q[i] = (int32_t)std::llrint((double)softLimit(x * s.gainLinear) * 2147483647.0);
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
