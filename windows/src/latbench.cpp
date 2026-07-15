// latbench — end-to-end mouth-to-virtual-mic latency benchmark (docs/webrtc-migration.md).
//
// Plays a click train out a render endpoint and captures TWO WASAPI streams on the shared
// QPC clock the audio engine stamps each packet with:
//   REF = loopback of the render endpoint   (when the click was actually emitted)
//   RET = capture of "CABLE Output"          (when the click came back via phone → stream → CABLE)
// Per click, RET_onset − REF_onset = the full capture+codec+transport+jitter+render latency.
// A fixed speaker/DAC/mic/acoustic offset is constant across runs, so the SRT-vs-WebRTC *delta*
// is exact even if the absolute carries that offset. Report mean ± stddev over N clicks.
//
//   Real run:   latbench.exe                 (clicks out the default speakers; phone must be
//                                              streaming into CABLE, mic near the speakers)
//   Self-test:  latbench.exe --selftest      (clicks straight into CABLE Input → CABLE Output;
//                                              no phone — validates the tool, expect a few ms)
// Flags: --render <substr> (default = default speakers), --ret <substr> (default "CABLE Output"),
//        --clicks N (30), --interval <ms> (1000), --thresh <0..1> (0.03).

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX          // keep windows.h min/max macros from breaking std::max/std::min
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

static std::atomic<bool> g_run{true};

static std::string lower(std::string s) { for (auto &c : s) c = (char)tolower((unsigned char)c); return s; }
static double qpcMs() {
    LARGE_INTEGER f, c; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c);
    return 1000.0 * (double)c.QuadPart / (double)f.QuadPart;
}

// Find an endpoint whose friendly name contains `match` (case-insensitive); default endpoint if empty.
static IMMDevice *findDevice(IMMDeviceEnumerator *e, EDataFlow flow, const std::string &match) {
    if (match.empty()) {
        IMMDevice *d = nullptr;
        e->GetDefaultAudioEndpoint(flow, eConsole, &d);
        return d;
    }
    IMMDeviceCollection *col = nullptr;
    if (FAILED(e->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &col))) return nullptr;
    UINT n = 0; col->GetCount(&n);
    std::string want = lower(match);
    IMMDevice *found = nullptr;
    for (UINT i = 0; i < n && !found; i++) {
        IMMDevice *d = nullptr; col->Item(i, &d);
        IPropertyStore *ps = nullptr;
        if (SUCCEEDED(d->OpenPropertyStore(STGM_READ, &ps))) {
            PROPVARIANT v; PropVariantInit(&v);
            if (SUCCEEDED(ps->GetValue(PKEY_Device_FriendlyName, &v)) && v.vt == VT_LPWSTR) {
                char name[256]; WideCharToMultiByte(CP_UTF8, 0, v.pwszVal, -1, name, sizeof name, nullptr, nullptr);
                if (lower(name).find(want) != std::string::npos) { found = d; d = nullptr; }
            }
            PropVariantClear(&v); ps->Release();
        }
        if (d) d->Release();
    }
    col->Release();
    return found;
}

// Interleaved endpoint samples -> mono float (average channels). Supports float32 and int16.
static void toMono(const BYTE *data, UINT frames, WAVEFORMATEX *fmt, std::vector<float> &out) {
    out.resize(frames);
    int ch = fmt->nChannels;
    bool isFloat = (fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) ||
                   (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE && fmt->wBitsPerSample == 32);
    if (isFloat) {
        const float *f = reinterpret_cast<const float *>(data);
        for (UINT i = 0; i < frames; i++) { float s = 0; for (int c = 0; c < ch; c++) s += f[i * ch + c]; out[i] = s / ch; }
    } else {
        const int16_t *s16 = reinterpret_cast<const int16_t *>(data);
        for (UINT i = 0; i < frames; i++) { float s = 0; for (int c = 0; c < ch; c++) s += s16[i * ch + c] / 32768.0f; out[i] = s / ch; }
    }
}

// Onset detector: marks the QPC time (ms) at which energy first crosses a threshold, then re-arms
// once the click has passed and the signal is quiet again.
struct Detector {
    double absThresh;
    double noiseFloor = 1e-4;
    bool   armed = true;
    double lastOnsetMs = -1e9;
    std::vector<double> onsets;

    void feed(const std::vector<float> &mono, double startMs, double sr) {
        double perSample = 1000.0 / sr;
        for (size_t i = 0; i < mono.size(); i++) {
            double a = std::fabs(mono[i]);
            if (a < noiseFloor * 2) noiseFloor = 0.9995 * noiseFloor + 0.0005 * a;
            double thr = std::max(absThresh, noiseFloor * 8);
            double t = startMs + i * perSample;
            if (armed && a > thr) { onsets.push_back(t); armed = false; lastOnsetMs = t; }
            else if (!armed && (t - lastOnsetMs) > 150.0 && a < thr * 0.5) armed = true;
        }
    }
};

struct Capture {
    IAudioClient *client = nullptr;
    IAudioCaptureClient *cap = nullptr;
    WAVEFORMATEX *fmt = nullptr;
    Detector det;
    double recordStartMs = 0;
    std::vector<float> recorded;
    std::thread th;
};

static bool initCapture(IMMDevice *dev, bool loopback, Capture &c) {
    if (FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void **)&c.client))) return false;
    if (FAILED(c.client->GetMixFormat(&c.fmt))) return false;
    DWORD flags = loopback ? AUDCLNT_STREAMFLAGS_LOOPBACK : 0;
    if (FAILED(c.client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 10000000, 0, c.fmt, nullptr))) return false;
    if (FAILED(c.client->GetService(__uuidof(IAudioCaptureClient), (void **)&c.cap))) return false;
    c.recorded.reserve((size_t)c.fmt->nSamplesPerSec * 40);
    return SUCCEEDED(c.client->Start());
}

static void captureLoop(Capture *c) {
    double sr = c->fmt->nSamplesPerSec;
    std::vector<float> mono;
    while (g_run.load()) {
        UINT32 packet = 0;
        if (FAILED(c->cap->GetNextPacketSize(&packet))) break;
        if (packet == 0) { Sleep(3); continue; }
        BYTE *data; UINT32 frames; DWORD flags; UINT64 devPos, qpcPos;
        if (FAILED(c->cap->GetBuffer(&data, &frames, &flags, &devPos, &qpcPos))) break;
        if (frames > 0) {
            double startMs = qpcPos / 10000.0;                       // qpcPos is in 100ns units
            if (flags & AUDCLNT_BUFFERFLAGS_SILENT) mono.assign(frames, 0.0f);
            else toMono(data, frames, c->fmt, mono);
            if (c->recorded.empty()) c->recordStartMs = startMs;
            c->recorded.insert(c->recorded.end(), mono.begin(), mono.end());
            if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT)) c->det.feed(mono, startMs, sr);
        }
        c->cap->ReleaseBuffer(frames);
    }
}

static std::vector<float> makeProbe(double sr) {
    int len = (int)(0.012 * sr);
    std::vector<float> probe(len);
    for (int i = 0; i < len; i++) {
        double env = 1.0;
        if (i > len - (int)(0.002 * sr)) env = (double)(len - i) / (0.002 * sr);
        probe[i] = (float)(env * std::sin(2 * 3.14159265 * 1500.0 * i / sr));
    }
    return probe;
}

struct CorrelationMatch { double timeMs = 0; double score = 0; };

static CorrelationMatch findProbe(const Capture &c, const std::vector<float> &probe,
                                  double fromMs, double toMs) {
    CorrelationMatch best;
    if (c.recorded.empty() || probe.empty()) return best;
    double sr = c.fmt->nSamplesPerSec;
    ptrdiff_t first = (ptrdiff_t)((fromMs - c.recordStartMs) * sr / 1000.0);
    ptrdiff_t last = (ptrdiff_t)((toMs - c.recordStartMs) * sr / 1000.0);
    first = std::max<ptrdiff_t>(0, first);
    last = std::min<ptrdiff_t>((ptrdiff_t)c.recorded.size() - (ptrdiff_t)probe.size(), last);
    if (last <= first) return best;

    double probeEnergy = 0;
    for (float v : probe) probeEnergy += v * v;
    ptrdiff_t bestPos = first;
    auto scoreAt = [&](ptrdiff_t pos) {
        double dot = 0, signalEnergy = 0;
        for (size_t i = 0; i < probe.size(); i++) {
            double s = c.recorded[(size_t)pos + i];
            dot += s * probe[i];
            signalEnergy += s * s;
        }
        return signalEnergy > 1e-12 ? std::fabs(dot) / std::sqrt(signalEnergy * probeEnergy) : 0.0;
    };

    for (ptrdiff_t pos = first; pos <= last; pos += 4) {
        double score = scoreAt(pos);
        if (score > best.score) { best.score = score; bestPos = pos; }
    }
    ptrdiff_t refineFirst = std::max(first, bestPos - 4);
    ptrdiff_t refineLast = std::min(last, bestPos + 4);
    for (ptrdiff_t pos = refineFirst; pos <= refineLast; pos++) {
        double score = scoreAt(pos);
        if (score > best.score) { best.score = score; bestPos = pos; }
    }
    best.timeMs = c.recordStartMs + 1000.0 * bestPos / sr;
    return best;
}

int main(int argc, char **argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    std::string renderMatch, retMatch = "CABLE Output";
    int nClicks = 30, intervalMs = 1000;
    double thresh = 0.03;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--selftest") renderMatch = "CABLE Input";
        else if (a == "--render" && i + 1 < argc) renderMatch = argv[++i];
        else if (a == "--ret" && i + 1 < argc) retMatch = argv[++i];
        else if (a == "--clicks" && i + 1 < argc) nClicks = atoi(argv[++i]);
        else if (a == "--interval" && i + 1 < argc) intervalMs = atoi(argv[++i]);
        else if (a == "--thresh" && i + 1 < argc) thresh = atof(argv[++i]);
    }

    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IMMDeviceEnumerator *en = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                __uuidof(IMMDeviceEnumerator), (void **)&en))) { printf("enumerator failed\n"); return 1; }

    IMMDevice *rdev = findDevice(en, eRender, renderMatch);
    IMMDevice *cdev = findDevice(en, eCapture, retMatch);
    if (!rdev) { printf("render endpoint '%s' not found\n", renderMatch.empty() ? "(default)" : renderMatch.c_str()); return 1; }
    if (!cdev) { printf("capture endpoint '%s' not found\n", retMatch.c_str()); return 1; }
    printf("[latbench] render=%s  ret=%s  clicks=%d  interval=%dms  thresh=%.3f\n",
           renderMatch.empty() ? "(default speakers)" : renderMatch.c_str(), retMatch.c_str(), nClicks, intervalMs, thresh);

    // Render client on the render endpoint (to play clicks) + a loopback capture of the same endpoint (REF).
    IAudioClient *rc = nullptr; IAudioRenderClient *rr = nullptr; WAVEFORMATEX *rfmt = nullptr; UINT32 rbuf = 0;
    if (FAILED(rdev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void **)&rc)) ||
        FAILED(rc->GetMixFormat(&rfmt)) ||
        FAILED(rc->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 10000000, 0, rfmt, nullptr)) ||
        FAILED(rc->GetService(__uuidof(IAudioRenderClient), (void **)&rr)) ||
        FAILED(rc->GetBufferSize(&rbuf))) { printf("render init failed\n"); return 1; }

    Capture ref, ret;
    ref.det.absThresh = thresh; ret.det.absThresh = thresh;
    if (!initCapture(rdev, true, ref)) { printf("REF loopback init failed\n"); return 1; }
    if (!initCapture(cdev, false, ret)) { printf("RET capture init failed\n"); return 1; }

    // Pre-render a click: 12 ms of 1.5 kHz at 0.6, sharp attack (a crisp transient that survives Opus).
    double rsr = rfmt->nSamplesPerSec; int rch = rfmt->nChannels;
    int clickLen = (int)(0.012 * rsr);
    std::vector<float> click(clickLen);
    for (int i = 0; i < clickLen; i++) {
        double env = i < clickLen - 1 ? 1.0 : 0.0;
        if (i > clickLen - (int)(0.002 * rsr)) env = (double)(clickLen - i) / (0.002 * rsr);  // 2ms release
        click[i] = (float)(0.6 * env * std::sin(2 * 3.14159265 * 1500.0 * i / rsr));
    }

    ref.th = std::thread(captureLoop, &ref);
    ret.th = std::thread(captureLoop, &ret);

    // Render thread: silence, punctuated by a click every intervalMs.
    std::vector<double> clickTimes;
    std::thread render([&]() {
        rc->Start();
        int clickPos = clickLen;                 // start idle
        double nextClick = qpcMs() + 500;        // 0.5s warm-up
        int played = 0;
        bool isFloat = (rfmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) ||
                       (rfmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE && rfmt->wBitsPerSample == 32);
        while (g_run.load()) {
            UINT32 pad = 0; if (FAILED(rc->GetCurrentPadding(&pad))) break;
            UINT32 avail = rbuf - pad;
            if (avail == 0) { Sleep(3); continue; }
            if (clickPos >= clickLen && qpcMs() >= nextClick && played < nClicks) {
                clickPos = 0;
                clickTimes.push_back(qpcMs());
                nextClick += intervalMs;
                played++;
            }
            BYTE *buf; if (FAILED(rr->GetBuffer(avail, &buf))) break;
            float *ff = reinterpret_cast<float *>(buf);
            int16_t *si = reinterpret_cast<int16_t *>(buf);
            for (UINT32 i = 0; i < avail; i++) {
                float s = (clickPos < clickLen) ? click[clickPos++] : 0.0f;
                for (int c = 0; c < rch; c++) { if (isFloat) ff[i * rch + c] = s; else si[i * rch + c] = (int16_t)(s * 32767); }
            }
            rr->ReleaseBuffer(avail, 0);
        }
    });

    // Run until all clicks played + one interval of settle.
    double deadline = qpcMs() + 500 + (double)nClicks * intervalMs + std::max(2000, intervalMs);
    while (qpcMs() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    g_run = false;
    render.join(); ref.th.join(); ret.th.join();

    // Correlate the exact probe waveform in a bounded window for each scheduled click. Unlike
    // threshold crossings, this remains selective when speech, music, or notifications are
    // present on either capture stream.
    std::vector<float> refProbe = makeProbe(ref.fmt->nSamplesPerSec);
    std::vector<float> retProbe = makeProbe(ret.fmt->nSamplesPerSec);
    std::vector<double> lat, refScores, retScores;
    for (double scheduled : clickTimes) {
        CorrelationMatch f = findProbe(ref, refProbe, scheduled - 25.0, scheduled + 250.0);
        if (f.score < 0.45) continue;
        CorrelationMatch r = findProbe(ret, retProbe, f.timeMs, f.timeMs + 900.0);
        if (r.score < 0.12) continue;
        lat.push_back(r.timeMs - f.timeMs);
        refScores.push_back(f.score);
        retScores.push_back(r.score);
    }

    size_t correlated = lat.size();
    if (lat.size() >= 5) {
        std::vector<double> sorted = lat;
        std::sort(sorted.begin(), sorted.end());
        double center = sorted[sorted.size() / 2];
        std::vector<double> deviations;
        deviations.reserve(sorted.size());
        for (double v : sorted) deviations.push_back(std::fabs(v - center));
        std::sort(deviations.begin(), deviations.end());
        double mad = deviations[deviations.size() / 2];
        double limit = std::max(30.0, 6.0 * mad);
        lat.erase(std::remove_if(lat.begin(), lat.end(), [&](double v) {
            return std::fabs(v - center) > limit;
        }), lat.end());
    }

    printf("\n=== latbench ===\n");
    printf("raw onsets: REF=%zu RET=%zu   scheduled clicks=%zu\n",
           ref.det.onsets.size(), ret.det.onsets.size(), clickTimes.size());
    double minRefScore = refScores.empty() ? 0 : *std::min_element(refScores.begin(), refScores.end());
    double minRetScore = retScores.empty() ? 0 : *std::min_element(retScores.begin(), retScores.end());
    printf("correlated: %zu   inliers: %zu   minimum scores: REF=%.3f RET=%.3f\n",
           correlated, lat.size(), minRefScore, minRetScore);
    size_t minValid = std::max<size_t>(3, clickTimes.size() / 2);
    if (lat.size() < minValid) {
        printf("No matched clicks. In a real run: is the phone streaming into CABLE, mic near the speakers,\n"
               "volume up? Need at least %zu correlated returns; got %zu.\n"
               "(--selftest needs VB-CABLE looping.)\n", minValid, lat.size());
    } else {
        std::sort(lat.begin(), lat.end());
        double sum = 0; for (double v : lat) sum += v; double mean = sum / lat.size();
        double var = 0; for (double v : lat) var += (v - mean) * (v - mean); double sd = std::sqrt(var / lat.size());
        double med = lat[lat.size() / 2];
        printf("latency ms: mean %.1f  stddev %.1f  median %.1f  min %.1f  max %.1f  (n=%zu)\n",
               mean, sd, med, lat.front(), lat.back(), lat.size());
        printf("NOTE: absolute carries a fixed speaker+mic+acoustic offset; the SRT-vs-WebRTC delta is the real number.\n");
    }
    return 0;
}
