// WebRTC receive session for receiver.exe (--webrtc). See webrtc_receiver.h.
//
// Combines the three Phase-1/2a de-risked pieces into the real receiver:
//   PCAM3 TCP signaling server (offerer, pairSecret-gated)   [proven: webrtc_signaling.cpp]
//   libdatachannel recvonly Opus track over DTLS-SRTP        [proven: webrtc_loopback.cpp]
//   RTP -> FFmpeg Opus decode -> AVFrame                     [proven: webrtc_audio.cpp]
//   -> bounded queue -> dedicated audio thread -> WasapiSink (CABLE / virtual mic)
//
// Threading: libdatachannel delivers RTP on its own receive thread; we decode there and hand
// cloned AVFrames to a single audio thread that owns COM + the WasapiSink (Init/WriteFrame/Stop
// all on that one thread — WasapiSink::Init does CoInitializeEx(MTA) on the caller). This keeps
// WASAPI off the network thread (no render backpressure into SRTP) and COM correctly balanced.

#include "webrtc_receiver.h"
#include "wasapi_sink.h"
#include "video_sink.h"   // Phase 5: H.264 -> softcam virtual camera

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

#include "rtc/rtc.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/frame.h>
}

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

using namespace std::chrono_literals;

// ---- PCAM3 TCP framing: [1 byte type]['S'|'O'|'A'][4-byte BE len][payload] ----
static bool sendAll(SOCKET s, const char *buf, int len) {
    while (len > 0) {
        int n = send(s, buf, len, 0);
        if (n <= 0) return false;
        buf += n; len -= n;
    }
    return true;
}
static bool recvAll(SOCKET s, char *buf, int len) {
    while (len > 0) {
        int n = recv(s, buf, len, 0);
        if (n <= 0) return false;
        buf += n; len -= n;
    }
    return true;
}
static bool sendMsg(SOCKET s, char type, const std::string &payload) {
    char hdr[5];
    uint32_t n = (uint32_t)payload.size();
    hdr[0] = type;
    hdr[1] = (char)((n >> 24) & 0xff); hdr[2] = (char)((n >> 16) & 0xff);
    hdr[3] = (char)((n >> 8) & 0xff);  hdr[4] = (char)(n & 0xff);
    return sendAll(s, hdr, 5) && (n == 0 || sendAll(s, payload.data(), (int)n));
}
static bool recvMsg(SOCKET s, char &type, std::string &payload) {
    char hdr[5];
    if (!recvAll(s, hdr, 5)) return false;
    type = hdr[0];
    uint32_t n = ((uint32_t)(uint8_t)hdr[1] << 24) | ((uint32_t)(uint8_t)hdr[2] << 16) |
                 ((uint32_t)(uint8_t)hdr[3] << 8)  | (uint32_t)(uint8_t)hdr[4];
    payload.resize(n);
    return n == 0 || recvAll(s, &payload[0], (int)n);
}

// Offset of the RTP payload (past the 12-byte header + CSRCs + optional extension).
static int rtpPayloadOffset(const uint8_t *p, int len) {
    if (len < 12) return -1;
    int cc = p[0] & 0x0F;
    int off = 12 + 4 * cc;
    if (p[0] & 0x10) {                 // X: header extension present
        if (off + 4 > len) return -1;
        int words = (p[off + 2] << 8) | p[off + 3];
        off += 4 + 4 * words;
    }
    return (off <= len) ? off : -1;
}

// ---- bounded frame queue: network(decode) thread -> audio(render) thread ----
struct FrameQueue {
    std::mutex m;
    std::condition_variable cv;
    std::deque<AVFrame *> q;
    bool stop = false;

    void push(AVFrame *f) {
        std::unique_lock<std::mutex> lk(m);
        if (q.size() > 100) {                 // ~2s @ 20ms; drop oldest rather than build latency
            AVFrame *old = q.front(); q.pop_front(); av_frame_free(&old);
        }
        q.push_back(f);
        lk.unlock();
        cv.notify_one();
    }
    AVFrame *pop() {                          // blocks; returns nullptr only once stopped AND drained
        std::unique_lock<std::mutex> lk(m);
        cv.wait(lk, [&] { return stop || !q.empty(); });
        if (!q.empty()) { AVFrame *f = q.front(); q.pop_front(); return f; }
        return nullptr;
    }
    void signalStop() {
        { std::lock_guard<std::mutex> lk(m); stop = true; }
        cv.notify_all();
    }
};

// Owns COM + the WasapiSink. Inits the sink from the first frame's format, then renders.
static void audioThread(FrameQueue *fq, AVCodecContext *sinkFmt, WebrtcRecvConfig cfg) {
    WasapiSink sink;
    bool ready = false;
    for (;;) {
        AVFrame *f = fq->pop();
        if (!f) break;                        // stopped + drained
        if (!ready) {
            ready = sink.Init(sinkFmt, cfg.audioDevice, cfg.micGainDb, cfg.eqPreset);
            if (!ready) {
                fprintf(stderr, "[webrtc] audio sink init failed; dropping audio\n");
                av_frame_free(&f);
                while ((f = fq->pop())) av_frame_free(&f);
                return;
            }
        }
        sink.WriteFrame(f);
        av_frame_free(&f);
    }
    sink.Stop();
}

// One phone session: secret -> offer -> answer -> media until the peer drops or *running clears.
static void handleConnection(SOCKET cli, const AVCodec *dec,
                             const WebrtcRecvConfig &cfg, std::atomic<bool> *running) {
    char type; std::string payload;
    if (!recvMsg(cli, type, payload) || type != 'S') { fprintf(stderr, "[webrtc] expected pairing secret\n"); return; }
    if (payload != cfg.sigSecret)                    { fprintf(stderr, "[webrtc] bad pairing secret — rejected\n"); return; }
    fprintf(stderr, "[webrtc] phone paired — negotiating\n");

    AVCodecContext *decCtx = avcodec_alloc_context3(dec);
    decCtx->sample_rate = 48000;
    av_channel_layout_default(&decCtx->ch_layout, 1);
    if (avcodec_open2(decCtx, dec, nullptr) < 0) { fprintf(stderr, "[webrtc] opus decoder open failed\n"); return; }

    FrameQueue fq;
    AVCodecContext *sinkFmt = avcodec_alloc_context3(nullptr);   // carries the format to the audio thread
    std::atomic<bool> sinkFmtReady{false};
    std::thread at(audioThread, &fq, sinkFmt, cfg);

    rtc::Configuration config;
    auto pc = std::make_shared<rtc::PeerConnection>(config);
    std::atomic<bool> disconnected{false};
    std::atomic<int>  rtpCount{0};

    // Video state (set up below only when cfg.wantVideo). Declared here so onStateChange can ask
    // for a keyframe the moment we connect (instead of waiting a full GOP for the phone's next IDR).
    AVCodecContext *decCtxV = nullptr;
    VideoSink       videoSink(30.0, cfg.wantPreview);
    std::shared_ptr<rtc::Track> vtrack;

    pc->onStateChange([&disconnected, &vtrack](rtc::PeerConnection::State s) {
        using S = rtc::PeerConnection::State;
        if (s == S::Connected) {
            fprintf(stderr, "[webrtc] connected — media flowing\n");
            if (vtrack) vtrack->requestKeyframe();   // PLI: start video without waiting for the GOP
        }
        if (s == S::Disconnected || s == S::Failed || s == S::Closed) {
            fprintf(stderr, "[webrtc] peer state=%d\n", (int)s);
            disconnected = true;
        }
    });
    // Non-trickle: send the offer once ICE gathering is complete (candidates inline).
    pc->onGatheringStateChange([&pc, cli](rtc::PeerConnection::GatheringState g) {
        if (g == rtc::PeerConnection::GatheringState::Complete) {
            if (auto d = pc->localDescription()) {
                fprintf(stderr, "[webrtc] sending offer\n");
                sendMsg(cli, 'O', std::string(*d));
            }
        }
    });

    rtc::Description::Audio media("audio", rtc::Description::Direction::RecvOnly);
    media.addOpusCodec(111);
    media.addSSRC(42, "audio");
    auto track = pc->addTrack(media);
    track->setMediaHandler(std::make_shared<rtc::RtcpReceivingSession>());
    track->onMessage([&, decCtx, sinkFmt](rtc::message_variant m) {
        if (!std::holds_alternative<rtc::binary>(m)) return;
        auto &b = std::get<rtc::binary>(m);
        const uint8_t *p = reinterpret_cast<const uint8_t *>(b.data());
        int off = rtpPayloadOffset(p, (int)b.size());
        if (off < 0 || off >= (int)b.size()) return;
        if (++rtpCount == 1) fprintf(stderr, "[webrtc] first RTP received\n");
        AVPacket *pk = av_packet_alloc();
        pk->data = const_cast<uint8_t *>(p + off);
        pk->size = (int)b.size() - off;
        if (avcodec_send_packet(decCtx, pk) == 0) {
            AVFrame *fr = av_frame_alloc();
            while (avcodec_receive_frame(decCtx, fr) == 0) {
                if (!sinkFmtReady.load()) {
                    sinkFmt->sample_rate = fr->sample_rate;
                    av_channel_layout_copy(&sinkFmt->ch_layout, &fr->ch_layout);
                    sinkFmt->sample_fmt = (AVSampleFormat)fr->format;
                    sinkFmtReady = true;               // publishes sinkFmt before the frame is enqueued
                }
                fq.push(av_frame_clone(fr));
                av_frame_unref(fr);
            }
            av_frame_free(&fr);
        }
        av_packet_free(&pk);
    });

    // Optional recvonly H.264 video: depacketize (Annex-B) -> FFmpeg decode -> softcam (Phase 5).
    if (cfg.wantVideo) {
        const AVCodec *decv = avcodec_find_decoder(AV_CODEC_ID_H264);
        decCtxV = avcodec_alloc_context3(decv);
        avcodec_open2(decCtxV, decv, nullptr);
        rtc::Description::Video vmedia("video", rtc::Description::Direction::RecvOnly);
        vmedia.addH264Codec(96);
        vmedia.addSSRC(43, "video");
        vtrack = pc->addTrack(vmedia);
        auto depack = std::make_shared<rtc::H264RtpDepacketizer>(rtc::NalUnit::Separator::StartSequence);
        depack->addToChain(std::make_shared<rtc::RtcpReceivingSession>());
        vtrack->setMediaHandler(depack);
        vtrack->onFrame([&videoSink, decCtxV](rtc::binary data, rtc::FrameInfo) {
            AVPacket *pk = av_packet_alloc();
            if (av_new_packet(pk, (int)data.size()) == 0) {
                std::memcpy(pk->data, data.data(), data.size());
                if (avcodec_send_packet(decCtxV, pk) == 0) {
                    AVFrame *fr = av_frame_alloc();
                    while (avcodec_receive_frame(decCtxV, fr) == 0) { videoSink.WriteFrame(fr); av_frame_unref(fr); }
                    av_frame_free(&fr);
                }
            }
            av_packet_free(&pk);
        });
        fprintf(stderr, "[webrtc] offering video (H264) + audio (Opus)\n");
    }

    pc->setLocalDescription();                          // -> gather -> onGatheringStateChange sends 'O'

    // Wait for the phone's answer (blocking; it replies promptly after the offer).
    if (recvMsg(cli, type, payload) && type == 'A') {
        fprintf(stderr, "[webrtc] got answer — connecting\n");
        pc->setRemoteDescription(rtc::Description(payload, "answer"));
    } else {
        fprintf(stderr, "[webrtc] no answer — aborting session\n");
    }

    // Media flows on the rtc thread -> queue -> audio thread. Hold here until the peer drops.
    while (*running && !disconnected) std::this_thread::sleep_for(100ms);

    pc->close();
    pc.reset();                                         // ensure no more onMessage before we free decCtx
    fq.signalStop();
    at.join();
    videoSink.Stop();
    if (decCtxV) avcodec_free_context(&decCtxV);
    avcodec_free_context(&decCtx);
    avcodec_free_context(&sinkFmt);
    fprintf(stderr, "[webrtc] session ended (rtp=%d, vframes=%ld) — listening again\n",
            rtpCount.load(), videoSink.frames());
}

int run_webrtc_session(const WebrtcRecvConfig &cfg, std::atomic<bool> *running) {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { fprintf(stderr, "[webrtc] WSAStartup failed\n"); return -1; }
    rtc::InitLogger(rtc::LogLevel::Warning);

    const AVCodec *dec = avcodec_find_decoder(AV_CODEC_ID_OPUS);
    if (!dec) { fprintf(stderr, "[webrtc] no Opus decoder in FFmpeg\n"); WSACleanup(); return -1; }

    SOCKET srv = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (srv == INVALID_SOCKET) { fprintf(stderr, "[webrtc] socket failed\n"); WSACleanup(); return -1; }
    BOOL yes = TRUE;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, (char *)&yes, sizeof yes);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons((u_short)cfg.sigPort);
    if (bind(srv, (sockaddr *)&addr, sizeof addr) != 0 || listen(srv, 1) != 0) {
        fprintf(stderr, "[webrtc] bind/listen on :%d failed (%d)\n", cfg.sigPort, WSAGetLastError());
        closesocket(srv); WSACleanup(); return -1;
    }
    fprintf(stderr, "[phonecam] webrtc: signaling on 0.0.0.0:%d — waiting for the phone (secret-gated)\n",
            cfg.sigPort);

    while (*running) {
        fd_set rfds; FD_ZERO(&rfds); FD_SET(srv, &rfds);
        timeval tv{0, 300000};                          // 300ms so we notice *running clearing
        int sel = select(0, &rfds, nullptr, nullptr, &tv);
        if (sel <= 0) continue;
        sockaddr_in ca; int cl = sizeof ca;
        SOCKET cli = accept(srv, (sockaddr *)&ca, &cl);
        if (cli == INVALID_SOCKET) continue;
        handleConnection(cli, dec, cfg, running);
        closesocket(cli);
    }

    closesocket(srv);
    WSACleanup();
    return 0;
}
