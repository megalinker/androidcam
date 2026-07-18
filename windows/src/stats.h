// Lightweight, flag-gated latency / queue instrumentation for the receiver. DEV/MEASUREMENT ONLY.
//
// OFF unless the env var PHONECAM_STATS is set to a non-empty, non-"0" value. When on, each aggregator
// collects samples and flushes p50/p95/max/mean (latencies) or depth/drops (queues) to stderr about
// once a second — it NEVER logs per-frame, so it can't become the bottleneck. The desktop app already
// forwards receiver stderr to its log, so [stats] lines show up in "Copy diagnostics" too.
//
// Single clock only: steady_clock (QPC-backed, monotonic on Windows). Every delta measured here is
// WITHIN the PC receiver process — it never crosses the phone/PC clock boundary, so no calibration is
// needed. For true cross-device latency (glass-to-glass, mouth-to-mic) use an external method
// (high-speed camera / latbench), not these numbers.
//
// To remove: delete this header and its call sites (grep "stats::" / "tuning::").
#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

namespace stats {

inline bool enabled() {
    static int v = -1;
    if (v < 0) { const char* e = std::getenv("PHONECAM_STATS"); v = (e && e[0] && std::strcmp(e, "0")) ? 1 : 0; }
    return v == 1;
}

inline uint64_t nowUs() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Latency aggregator: feed microsecond deltas; prints p50/p95/max/mean/n ~1/s.
class LatAgg {
public:
    explicit LatAgg(const char* name) : name_(name) {}
    void add(double us) {
        if (!enabled()) return;
        std::lock_guard<std::mutex> lk(m_);
        s_.push_back(us);
        uint64_t now = nowUs();
        if (last_ == 0) last_ = now;
        if (now - last_ < 1000000 || s_.empty()) return;
        last_ = now;
        std::sort(s_.begin(), s_.end());
        size_t n = s_.size();
        size_t i95 = (size_t)(n * 0.95); if (i95 >= n) i95 = n - 1;
        double sum = 0; for (double v : s_) sum += v;
        std::fprintf(stderr, "[stats] %-14s n=%zu p50=%.2fms p95=%.2fms max=%.2fms mean=%.2fms\n",
                     name_, n, s_[n / 2] / 1000.0, s_[i95] / 1000.0, s_.back() / 1000.0, (sum / n) / 1000.0);
        s_.clear();
    }
private:
    const char* name_;
    std::mutex m_;
    std::vector<double> s_;
    uint64_t last_ = 0;
};

// Queue gauge: report occupancy (window max) + cumulative drops ~1/s.
class QueueStat {
public:
    explicit QueueStat(const char* name) : name_(name) {}
    void observe(long depth) {
        if (!enabled()) return;
        std::lock_guard<std::mutex> lk(m_);
        if (depth > maxDepth_) maxDepth_ = depth;
        uint64_t now = nowUs();
        if (last_ == 0) last_ = now;
        if (now - last_ < 1000000) return;
        last_ = now;
        std::fprintf(stderr, "[stats] %-14s depth_max=%ld drops=%ld\n", name_, maxDepth_, drops_);
        maxDepth_ = 0;   // reset window max; drops stay cumulative
    }
    void drop(long n = 1) {
        if (!enabled()) return;
        std::lock_guard<std::mutex> lk(m_);
        drops_ += n;
    }
private:
    const char* name_;
    std::mutex m_;
    long maxDepth_ = 0, drops_ = 0;
    uint64_t last_ = 0;
};

// Shared aggregators (one transport runs per process, so global sharing is fine).
// Video path:
inline LatAgg    g_decodeLat("video.decode");   // packet submit -> its decoded frame (the F-05 metric)
inline LatAgg    g_sinkLat("video.sink");       // sws_scale + transform + softcam push
inline QueueStat g_videoQ("video.queue");
// Mic path:
inline LatAgg    g_audioE2E("audio.e2e");        // RTP/decoded arrival -> WASAPI submit (WebRTC path)
inline LatAgg    g_audioSink("audio.sink");      // WasapiSink::WriteFrame: resample + EQ/gain + render
inline LatAgg    g_swrDelay("audio.swr");        // samples buffered inside the resampler (ms)
inline LatAgg    g_wasapiPad("audio.wasapi");    // standing WASAPI render latency (F-11 / F-03 drift signal)
inline QueueStat g_audioQ("audio.queue");

inline void banner() {
    if (enabled())
        std::fprintf(stderr, "[stats] instrumentation ON (PHONECAM_STATS) — video: decode/sink/queue; "
                             "mic: e2e/sink/swr/wasapi/queue, ~1Hz\n");
}

} // namespace stats
