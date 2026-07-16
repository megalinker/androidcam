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
        if (q.size() >= kMax) q.pop_front();     // drop oldest: stay low-latency, never back up video
        q.emplace_back(p, p + n);
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

void audioThread(PcmQueue *pq, int rate, int channels, const UsbRecvConfig cfg) {
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

// One connected session: read frames until EOF/stop. Returns when the socket ends.
void session(SOCKET s, const UsbRecvConfig &cfg, std::atomic<bool> *running) {
    const AVCodec *decv = avcodec_find_decoder(AV_CODEC_ID_H264);
    AVCodecContext *decCtxV = avcodec_alloc_context3(decv);
    decCtxV->flags |= AV_CODEC_FLAG_LOW_DELAY;
    if (avcodec_open2(decCtxV, decv, nullptr) < 0) { fprintf(stderr, "[usb] h264 decoder open failed\n"); return; }
    VideoSink videoSink(30.0, cfg.wantPreview);

    PcmQueue pq;
    std::thread at;
    bool audioStarted = false;

    uint8_t hdr[13];
    std::vector<uint8_t> payload;
    long vframes = 0;
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
            AVPacket *pk = av_packet_alloc();
            if (av_new_packet(pk, len) == 0) {
                memcpy(pk->data, payload.data(), len);
                if (avcodec_send_packet(decCtxV, pk) == 0) {
                    AVFrame *fr = av_frame_alloc();
                    while (avcodec_receive_frame(decCtxV, fr) == 0) {
                        videoSink.WriteFrame(fr);
                        av_frame_unref(fr);
                        if ((++vframes % 150) == 0) fprintf(stderr, "[usb] %ld video frames\n", vframes);
                    }
                    av_frame_free(&fr);
                }
            }
            av_packet_free(&pk);
        } else if (type == 'A') {
            pq.push(payload.data(), len);
        }
    }

    videoSink.Stop();
    pq.finish();
    if (at.joinable()) at.join();
    avcodec_free_context(&decCtxV);
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
