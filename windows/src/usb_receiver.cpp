// USB receive session — see usb_receiver.h. Connects to the phone through an adb-forwarded TCP
// port, decodes H.264 -> softcam VideoSink, and plays PCM -> WasapiSink. Single receive thread does
// the framing + video decode; a decoupled audio thread owns COM + the WasapiSink so its real-time
// pacing never stalls video decode (mirrors the WebRTC path's split).

#include <winsock2.h>
#include <ws2tcpip.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/frame.h>
}

#include "usb_receiver.h"
#include "video_sink.h"
#include "wasapi_sink.h"
#include "stats.h"
#include "pro_audio.h"
#include "phone_status.h"
#include "marker.h"

namespace {

// ---- audio: a bounded PCM queue drained by one COM-owning thread ----
struct PcmQueue {
    std::deque<std::vector<uint8_t>> q;
    std::mutex m;
    std::condition_variable cv;
    bool done = false;
    static constexpr size_t kMax = 64;   // ~1.3s of 20ms chunks — cap so a stall can't grow unbounded

    void push(const uint8_t *p, int n) {
        std::lock_guard<std::mutex> lk(m);
        if (q.size() >= kMax) { q.pop_front(); stats::g_audioQ.drop(); }   // drop oldest: stay low-latency, never back up video
        q.emplace_back(p, p + n);
        stats::g_audioQ.observe((long)q.size());
        cv.notify_one();
    }
    bool pop(std::vector<uint8_t> &out) {
        std::unique_lock<std::mutex> lk(m);
        cv.wait(lk, [&] { return done || !q.empty(); });
        if (q.empty()) return false;
        out = std::move(q.front());
        q.pop_front();
        return true;
    }
    void finish() {
        std::lock_guard<std::mutex> lk(m);
        done = true;
        cv.notify_all();
    }
};

// ---- video: a queue of Annex-B payloads drained by one decode thread ----
struct VideoQueue {
    std::deque<std::vector<uint8_t>> q;
    std::mutex m;
    std::condition_variable cv;
    bool done = false;
    static constexpr size_t kMax = 120;   // ~4s @30fps — bound memory; a real backlog means decode can't keep up

    std::atomic<uint64_t> dropped{0};   // always counted (stats::g_videoQ is flag-gated)

    void push(const uint8_t *p, int n) {
        std::lock_guard<std::mutex> lk(m);
        // Overflow: shed the NEWEST unit (drop this one) rather than the oldest. Dropping the oldest
        // breaks the decoder's reference chain -> macroblock corruption until the next IDR (~1s on the
        // phone's 1s GOP); dropping the newest keeps the buffered GOP (incl. its IDR) contiguous so
        // decode stays clean and only the most-recent frames are lost. Still bounded. See docs/perf-audit F-02.
        if (q.size() >= kMax) { stats::g_videoQ.drop(); dropped.fetch_add(1); return; }
        q.emplace_back(p, p + n);
        stats::g_videoQ.observe((long)q.size());
        cv.notify_one();
    }
    bool pop(std::vector<uint8_t> &out) {
        std::unique_lock<std::mutex> lk(m);
        cv.wait(lk, [&] { return done || !q.empty(); });
        if (q.empty()) return false;
        out = std::move(q.front());
        q.pop_front();
        return true;
    }
    void finish() {
        std::lock_guard<std::mutex> lk(m);
        done = true;
        cv.notify_all();
    }
};

void videoThread(VideoQueue *vq, VideoSink *sink, std::atomic<uint64_t> *damagedFrames) {
    const AVCodec *decv = avcodec_find_decoder(AV_CODEC_ID_H264);
    AVCodecContext *ctx = avcodec_alloc_context3(decv);
    ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
    // NOTE: decode is single-threaded on purpose. FFmpeg defaults thread_count to 1 (it does NOT
    // auto-pick cpu_count), so H.264 frame threading is never active and adds no output delay — the
    // F-05 "frame-threading latency" hypothesis was DISPROVEN by an on-hardware A/B (frame vs slice
    // both ~1.8ms p50 at 720p on a 20-core PC; active_thread_type=0). Single-threaded decode is fast
    // enough here; only revisit (thread_count=0 + FF_THREAD_SLICE) if 4K on a weak PC ever shows
    // video.queue drops. See docs/perf-audit F-05.
    if (avcodec_open2(ctx, decv, nullptr) < 0) { fprintf(stderr, "[usb] h264 decoder open failed\n"); return; }
    if (stats::enabled())
        fprintf(stderr, "[stats] h264 decode: thread_count=%d active_thread_type=%d (0=none,1=FRAME,2=SLICE)\n",
                ctx->thread_count, ctx->active_thread_type);
    std::vector<uint8_t> buf;
    long vframes = 0;
    while (vq->pop(buf)) {
        AVPacket *pk = av_packet_alloc();
        if (av_new_packet(pk, (int)buf.size()) == 0) {
            memcpy(pk->data, buf.data(), buf.size());
            if (stats::enabled()) pk->pts = (int64_t)stats::nowUs();   // decode submit->output pairing (no-B-frame => in order)
            if (avcodec_send_packet(ctx, pk) == 0) {
                AVFrame *fr = av_frame_alloc();
                while (avcodec_receive_frame(ctx, fr) == 0) {
                    if (stats::enabled() && fr->pts != AV_NOPTS_VALUE)
                        stats::g_decodeLat.add((double)((int64_t)stats::nowUs() - fr->pts));
                    // TCP cannot lose or reorder bytes, so a damaged frame here means the loss happened
                    // above the transport — our own queue overflow, or a framing/lifetime bug. Counting
                    // it is what separates "the cable path is clean" from "we corrupt it ourselves".
                    if (fr->decode_error_flags &
                        (FF_DECODE_ERROR_INVALID_BITSTREAM | FF_DECODE_ERROR_MISSING_REFERENCE |
                         FF_DECODE_ERROR_CONCEALMENT_ACTIVE | FF_DECODE_ERROR_DECODE_SLICES))
                        damagedFrames->fetch_add(1);
                    sink->WriteFrame(fr);
                    av_frame_unref(fr);
                    if ((++vframes % 150) == 0) fprintf(stderr, "[usb] %ld video frames\n", vframes);
                }
                av_frame_free(&fr);
            }
        }
        av_packet_free(&pk);
    }
    avcodec_free_context(&ctx);
}

void audioThread(PcmQueue *pq, int rate, int channels, const UsbRecvConfig cfg) {
    ProAudioThread proAudio;   // MMCSS "Pro Audio" scheduling for the real-time render thread (F-09)
    WasapiSink sink;
    AVCodecContext *fmt = avcodec_alloc_context3(nullptr);   // just carries the source PCM format
    fmt->sample_rate = rate;
    av_channel_layout_default(&fmt->ch_layout, channels);
    fmt->sample_fmt = AV_SAMPLE_FMT_S16;
    bool ready = sink.Init(fmt, cfg.audioDevice, cfg.micGainDb, cfg.eqPreset);
    if (!ready) fprintf(stderr, "[usb] WASAPI sink init failed — audio muted\n");

    std::vector<uint8_t> chunk;
    while (pq->pop(chunk)) {
        if (!ready || chunk.empty()) continue;
        int nsamp = (int)chunk.size() / (2 * channels);   // S16
        AVFrame *fr = av_frame_alloc();
        fr->format = AV_SAMPLE_FMT_S16;
        av_channel_layout_default(&fr->ch_layout, channels);
        fr->sample_rate = rate;
        fr->nb_samples = nsamp;
        if (av_frame_get_buffer(fr, 0) == 0) {
            memcpy(fr->data[0], chunk.data(), chunk.size());
            sink.WriteFrame(fr);
        }
        av_frame_free(&fr);
    }
    sink.Stop();
    avcodec_free_context(&fmt);
}

// ---- framing helpers ----
bool readFull(SOCKET s, uint8_t *buf, int n, std::atomic<bool> *running) {
    int got = 0;
    while (got < n) {
        if (!running->load()) return false;
        int r = recv(s, (char *)buf + got, n - got, 0);
        if (r > 0) { got += r; continue; }
        if (r == 0) return false;                 // peer closed
        int e = WSAGetLastError();
        if (e == WSAETIMEDOUT || e == WSAEWOULDBLOCK) continue;   // recv timeout -> re-check running
        return false;
    }
    return true;
}

int64_t be64(const uint8_t *p) {
    int64_t v = 0;
    for (int i = 0; i < 8; ++i) v = (v << 8) | p[i];
    return v;
}
int32_t be32(const uint8_t *p) {
    return (p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
}

// Minimal integer field pull from the JSON header ("...,"arate":48000,...") — both ends are ours.
int jsonInt(const std::string &s, const char *key, int dflt) {
    std::string pat = std::string("\"") + key + "\":";
    auto i = s.find(pat);
    if (i == std::string::npos) return dflt;
    i += pat.size();
    return atoi(s.c_str() + i);
}

SOCKET connectLoop(int port, std::atomic<bool> *running) {
    while (running->load()) {
        SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s == INVALID_SOCKET) return INVALID_SOCKET;
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons((u_short)port);
        inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
        if (connect(s, (sockaddr *)&addr, sizeof(addr)) == 0) {
            BOOL one = TRUE;
            setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof(one));
            DWORD to = 500;   // ms; lets recv wake to notice shutdown
            setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&to, sizeof(to));
            return s;
        }
        closesocket(s);
        std::this_thread::sleep_for(std::chrono::milliseconds(800));   // phone/adb-forward not up yet
    }
    return INVALID_SOCKET;
}

// PC -> phone control frame: [1B type][4B BE len][payload]. Only this thread writes to the socket.
bool sendControl(SOCKET s, char type, const std::string &payload) {
    char hdr[5];
    uint32_t n = (uint32_t)payload.size();
    hdr[0] = type;
    hdr[1] = (char)((n >> 24) & 0xff); hdr[2] = (char)((n >> 16) & 0xff);
    hdr[3] = (char)((n >> 8) & 0xff);  hdr[4] = (char)(n & 0xff);
    const char *buf = hdr; int len = 5;
    for (int pass = 0; pass < 2; ++pass) {
        while (len > 0) {
            int w = send(s, buf, len, 0);
            if (w <= 0) return false;
            buf += w; len -= w;
        }
        if (pass == 0) { buf = payload.data(); len = (int)n; if (len == 0) break; }
    }
    return true;
}

// One connected session: read frames until EOF/stop. The receive thread does I/O only and hands
// video off to a decode thread and audio to a render thread, so neither can starve the socket reads.
void session(SOCKET s, const UsbRecvConfig &cfg, std::atomic<bool> *running) {
    VideoSink videoSink(30.0, cfg.wantPreview);
    VideoQueue vq;
    PcmQueue pq;
    std::atomic<uint64_t> damagedFrames{0};
    std::thread vt(videoThread, &vq, &videoSink, &damagedFrames);
    std::thread at;
    bool audioStarted = false;
    marker::Consumer marks;
    uint64_t statusMsgs = 0, lastSeenDrops = 0, keyframeAsks = 0;
    auto lastAskMs = std::chrono::steady_clock::now() - std::chrono::seconds(10);

    uint8_t hdr[13];
    std::vector<uint8_t> payload;
    while (running->load()) {
        if (!readFull(s, hdr, 13, running)) break;
        char type = (char)hdr[0];
        (void)be64(hdr + 1);                 // ptsUs — reserved for A/V sync; real-time playback for now
        int len = be32(hdr + 9);
        if (len < 0 || len > 16 * 1024 * 1024) { fprintf(stderr, "[usb] bad frame len %d\n", len); break; }
        payload.resize(len);
        if (len > 0 && !readFull(s, payload.data(), len, running)) break;

        if (type == 'H') {
            std::string meta((char *)payload.data(), payload.size());
            int arate = jsonInt(meta, "arate", 48000);
            int ach = jsonInt(meta, "achannels", 1);
            fprintf(stderr, "[usb] header: %s\n", meta.c_str());
            if (!audioStarted && meta.find("\"audio\":\"pcm") != std::string::npos) {
                at = std::thread(audioThread, &pq, arate, ach ? ach : 1, cfg);
                audioStarted = true;
            }
        } else if (type == 'V') {
            vq.push(payload.data(), len);
        } else if (type == 'A') {
            pq.push(payload.data(), len);
        } else if (type == phonestatus::kUsbStatusType) {
            PhoneStatusMsg st = phonestatus::parse(std::string((char *)payload.data(), payload.size()));
            if (st.valid) {
                ++statusMsgs;
                phonestatus::emit(st);
                // The phone stopped rotating its own frames (it costs it ~6-42 CPU points);
                // it now tells us the angle and its capture geometry, and we apply both here.
                VideoSetPhoneGeometry(st.videoW, st.videoH, st.rotation);
            }
        }

        // --- outbound control, driven off the same thread so the socket has a single writer ---
        std::string note;
        if (marks.poll(note)) {
            fprintf(stderr, "[mark] note=\"%s\" transport=usb vframes=%ld queueDrops=%llu "
                            "damagedFrames=%llu keyframeAsks=%llu\n",
                    note.c_str(), videoSink.frames(), (unsigned long long)vq.dropped.load(),
                    (unsigned long long)damagedFrames.load(), (unsigned long long)keyframeAsks);
            fflush(stderr);
            sendControl(s, 'M', note);
        }
        // A queue overflow drops encoded units, which breaks the decoder's reference chain until the
        // phone's next IDR. TCP gives us no loss, so this is the one place the cable path can corrupt
        // itself — ask for a keyframe instead of waiting out the GOP. Rate-limited to 1/s so a sustained
        // overload cannot turn into a keyframe storm.
        uint64_t drops = vq.dropped.load();
        if (drops != lastSeenDrops) {
            lastSeenDrops = drops;
            auto now = std::chrono::steady_clock::now();
            if (now - lastAskMs >= std::chrono::seconds(1)) {
                lastAskMs = now;
                ++keyframeAsks;
                sendControl(s, 'K', "");
                fprintf(stderr, "[usb] video queue overflow — requested a keyframe\n");
            }
        }
    }

    vq.finish();
    if (vt.joinable()) vt.join();
    pq.finish();
    if (at.joinable()) at.join();
    videoSink.Stop();
    fprintf(stderr, "[usb] session ended (vframes=%ld, status=%llu) queueDrops=%llu damagedFrames=%llu "
                    "keyframeAsks=%llu\n",
            videoSink.frames(), (unsigned long long)statusMsgs, (unsigned long long)vq.dropped.load(),
            (unsigned long long)damagedFrames.load(), (unsigned long long)keyframeAsks);
}

}  // namespace

int run_usb_session(const UsbRecvConfig &cfg, std::atomic<bool> *running) {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { fprintf(stderr, "[usb] WSAStartup failed\n"); return 1; }
    fprintf(stderr, "[usb] connecting to 127.0.0.1:%d (adb-forwarded phone stream)…\n", cfg.usbPort);

    while (running->load()) {
        SOCKET s = connectLoop(cfg.usbPort, running);
        if (s == INVALID_SOCKET) break;
        fprintf(stderr, "[usb] connected — receiving\n");
        session(s, cfg, running);
        closesocket(s);
        if (running->load()) fprintf(stderr, "[usb] phone stream ended — will retry\n");
    }
    WSACleanup();
    return 0;
}
