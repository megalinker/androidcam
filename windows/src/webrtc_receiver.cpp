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
#include "stats.h"        // flag-gated (PHONECAM_STATS) latency/queue instrumentation
#include "pro_audio.h"    // MMCSS "Pro Audio" for the render thread (F-09)
#include "phone_status.h" // phone battery/charging over the signaling socket ('B')
#include "rtp_loss.h"     // RTP sequence-continuity monitor (incomplete-frame detection)
#include "marker.h"       // "Mark video problem" correlation marker

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

// Minimum spacing between keyframe (PLI) requests. Deliberately conservative: a keyframe costs the
// phone several times a P-frame in encode work and bytes, so recovery must stay event-driven and
// bounded rather than becoming a standing bitrate/battery tax. 1 s also matches the USB path's GOP,
// i.e. the worst case here is no worse than what the cable path already does continuously.
static constexpr uint64_t kPliMinIntervalUs = 1000000;

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

// Blocking read that tolerates the socket's receive timeout, so the session loop stays responsive to
// Ctrl-C / peer loss while still being able to receive the phone's status messages.
//   1 = a message was read, 0 = nothing arrived before the timeout, -1 = the peer closed / errored.
// Once the first byte of a message has been consumed we must not return mid-message (that would
// desync the framing), so only the very first read is allowed to time out.
static int recvMsgTimed(SOCKET s, char &type, std::string &payload) {
    char hdr[5];
    int n = recv(s, hdr, 1, 0);
    if (n == 0) return -1;
    if (n < 0) {
        int e = WSAGetLastError();
        return (e == WSAETIMEDOUT || e == WSAEWOULDBLOCK) ? 0 : -1;
    }
    // The rest of the message must be read to completion; retry across receive timeouts so a payload
    // that happens to straddle one does not desync the framing.
    auto recvRest = [](SOCKET sk, char *buf, int len) {
        while (len > 0) {
            int r = recv(sk, buf, len, 0);
            if (r > 0) { buf += r; len -= r; continue; }
            if (r == 0) return false;
            int e = WSAGetLastError();
            if (e == WSAETIMEDOUT || e == WSAEWOULDBLOCK) continue;
            return false;
        }
        return true;
    };
    if (!recvRest(s, hdr + 1, 4)) return -1;
    type = hdr[0];
    uint32_t len = ((uint32_t)(uint8_t)hdr[1] << 24) | ((uint32_t)(uint8_t)hdr[2] << 16) |
                   ((uint32_t)(uint8_t)hdr[3] << 8)  | (uint32_t)(uint8_t)hdr[4];
    if (len > 10u * 1024 * 1024) return -1;   // desync guard
    payload.resize(len);
    if (len && !recvRest(s, &payload[0], (int)len)) return -1;
    return 1;
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
            stats::g_audioQ.drop();
        }
        q.push_back(f);
        stats::g_audioQ.observe((long)q.size());
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
    ProAudioThread proAudio;   // MMCSS "Pro Audio" scheduling for the real-time render thread (F-09)
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
    // Force ICE (and therefore media) onto one local interface when asked — e.g. the PC's
    // USB-tethering adapter (192.168.42.x). juice then gathers only that host candidate, so the
    // only workable pair is phone-rndis0 <-> PC-rndis: media rides the USB cable, not Wi-Fi.
    if (!cfg.iceBind.empty()) {
        config.bindAddress = cfg.iceBind;
        fprintf(stderr, "[webrtc] binding ICE to %s (USB-tethering path)\n", cfg.iceBind.c_str());
    }
    auto pc = std::make_shared<rtc::PeerConnection>(config);
    std::atomic<bool> disconnected{false};
    std::atomic<int>  rtpCount{0};

    // Video state (set up below only when cfg.wantVideo). Declared here so onStateChange can ask
    // for a keyframe the moment we connect (instead of waiting a full GOP for the phone's next IDR).
    AVCodecContext *decCtxV = nullptr;
    VideoSink       videoSink(30.0, cfg.wantPreview);
    std::shared_ptr<rtc::Track> vtrack;
    auto lossMon = std::make_shared<RtpLossMonitor>();   // raw-RTP sequence continuity (see rtp_loss.h)
    uint64_t        lastPliUs = 0;   // rate-limit keyframe requests (decode error OR packet loss)
    // Counters for the session-end line and for the "mark video problem" snapshot.
    std::atomic<uint64_t> incompleteFrames{0};   // frames delivered after an RTP gap
    std::atomic<uint64_t> decodeErrFrames{0};    // frames FFmpeg flagged as damaged/concealed
    std::atomic<uint64_t> pliSent{0}, pliSuppressed{0};
    // libdatachannel dispatches track callbacks from a thread pool, so audio onMessage and video
    // onFrame (and successive frames of each) can run concurrently. The Opus and H.264 decoders use
    // independent AVCodecContexts and independent sinks, so they only need to be serialized against
    // THEMSELVES, not against each other — one lock per decoder lets audio decode while a video frame
    // is being decoded+converted (each callback takes only its own lock, so no deadlock). See F-07.
    std::mutex audioDecodeMutex;
    std::mutex videoDecodeMutex;

    // One place decides whether to ask the phone for a fresh IDR. Declared at SESSION scope (not
    // inside the wantVideo block) because the onFrame callback captures it by reference and outlives
    // any narrower scope — the track is only torn down at pc.reset() below.
    //
    // Rate-limited, because a keyframe is the single most expensive thing we can ask the phone's
    // encoder for: unthrottled requests during a loss burst would raise bitrate, encoder load and
    // therefore battery. At one per second the worst case is a keyframe cadence no tighter than the
    // phone's own USB-path GOP, and in a clean session it never fires at all.
    auto askKeyframe = [&](const char *why) {
        if (!vtrack) return;
        uint64_t now = stats::nowUs();
        if (now - lastPliUs < kPliMinIntervalUs) { pliSuppressed.fetch_add(1); return; }
        lastPliUs = now;
        pliSent.fetch_add(1);
        try { vtrack->requestKeyframe(); } catch (...) {}
        fprintf(stderr, "[video] keyframe requested (%s)\n", why);
    };

    pc->onStateChange([&disconnected, &vtrack](rtc::PeerConnection::State s) {
        using S = rtc::PeerConnection::State;
        if (s == S::Connected) {
            fprintf(stderr, "[webrtc] connected — media flowing\n");
            // PLI to start video without waiting for the GOP; harmless if the track isn't open yet.
            if (vtrack) { try { vtrack->requestKeyframe(); } catch (...) {} }
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
        std::lock_guard<std::mutex> lk(audioDecodeMutex);
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
                AVFrame *clone = av_frame_clone(fr);
                if (stats::enabled()) clone->pts = (int64_t)stats::nowUs();   // arrival stamp for audio.e2e
                fq.push(clone);
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
        // Single-threaded by default (FFmpeg leaves thread_count=1). The F-05 frame-threading-latency
        // hypothesis was disproven by measurement, so no thread config is set here. See docs/perf-audit F-05.
        avcodec_open2(decCtxV, decv, nullptr);
        rtc::Description::Video vmedia("video", rtc::Description::Direction::RecvOnly);
        vmedia.addH264Codec(96);
        vmedia.addSSRC(43, "video");
        vtrack = pc->addTrack(vmedia);
        auto depack = std::make_shared<rtc::H264RtpDepacketizer>(rtc::NalUnit::Separator::StartSequence);
        depack->addToChain(std::make_shared<rtc::RtcpReceivingSession>());
        // Added last => sees raw RTP FIRST on the incoming path (incomingChain unwinds from the tail).
        depack->addToChain(lossMon);
        vtrack->setMediaHandler(depack);

        vtrack->onFrame([&, decCtxV](rtc::binary data, rtc::FrameInfo) {
            // Did packets go missing since the previous frame? If so this frame was reassembled with a
            // hole in it: libdatachannel's depacketizer emits partial frames without saying so, and no
            // NACK is ever sent, so this is our only chance to notice. Read BEFORE decoding so the
            // flag belongs to the frame we are about to submit.
            const bool gapped = lossMon->consumeGap();
            std::lock_guard<std::mutex> lk(videoDecodeMutex);
            AVPacket *pk = av_packet_alloc();
            if (av_new_packet(pk, (int)data.size()) == 0) {
                std::memcpy(pk->data, data.data(), data.size());
                if (stats::enabled()) pk->pts = (int64_t)stats::nowUs();   // decode submit->output pairing
                if (avcodec_send_packet(decCtxV, pk) == 0) {
                    AVFrame *fr = av_frame_alloc();
                    while (avcodec_receive_frame(decCtxV, fr) == 0) {
                        if (stats::enabled() && fr->pts != AV_NOPTS_VALUE)
                            stats::g_decodeLat.add((double)((int64_t)stats::nowUs() - fr->pts));
                        // Two independent corruption signals, because neither alone is sufficient:
                        //  - the transport signal (an RTP gap) is exact but only covers losses we saw;
                        //  - FFmpeg's decode_error_flags catch damage the transport can't see. The
                        //    original code checked only INVALID_BITSTREAM|MISSING_REFERENCE, which a
                        //    truncated-but-parseable frame does NOT set — the concealment path sets
                        //    CONCEALMENT_ACTIVE/DECODE_SLICES instead, so those cases silently produced
                        //    the persistent corrupt region with no recovery request at all.
                        const int kDamaged = FF_DECODE_ERROR_INVALID_BITSTREAM |
                                             FF_DECODE_ERROR_MISSING_REFERENCE |
                                             FF_DECODE_ERROR_CONCEALMENT_ACTIVE |
                                             FF_DECODE_ERROR_DECODE_SLICES;
                        const bool damaged = (fr->decode_error_flags & kDamaged) != 0;
                        if (damaged) decodeErrFrames.fetch_add(1);
                        if (gapped) incompleteFrames.fetch_add(1);
                        if (damaged || gapped) askKeyframe(gapped ? "rtp-gap" : "decode-error");
                        videoSink.WriteFrame(fr); av_frame_unref(fr);
                    }
                    av_frame_free(&fr);
                }
            }
            av_packet_free(&pk);
        });
        fprintf(stderr, "[webrtc] offering video (H264) + audio (Opus)\n");
    }

    pc->setLocalDescription();                          // -> gather -> onGatheringStateChange sends 'O'

    // Wait for the phone's answer (recv is bounded by the F-01 SO_RCVTIMEO, so a silent peer can't
    // block here forever).
    bool negotiated = false;
    if (recvMsg(cli, type, payload) && type == 'A') {
        fprintf(stderr, "[webrtc] got answer — connecting\n");
        pc->setRemoteDescription(rtc::Description(payload, "answer"));
        negotiated = true;
    } else {
        fprintf(stderr, "[webrtc] no answer — aborting session\n");
    }

    // Tell the phone what this receiver understands. Sent AFTER the answer on purpose: a phone that
    // predates the feature is by then in its "read until EOF" loop and harmlessly discards these
    // bytes, whereas a hello sent before the offer would break its `expected offer 'O'` check.
    if (negotiated) sendMsg(cli, 'V', phonestatus::helloJson());

    // Media flows on the rtc thread -> queue -> audio thread. Hold here until the peer drops — but ONLY
    // if we actually negotiated. A valid-secret-but-no-answer peer must not hold this single-threaded
    // server in the spin loop (it would block every later phone); fall straight through to cleanup. (F-01)
    //
    // While holding, this thread also serves the signaling socket as a control channel: it receives
    // the phone's periodic device status ('B') and forwards "mark video problem" ('M'). A short
    // receive timeout keeps it as responsive to Ctrl-C as the old 100 ms sleep loop was.
    {
        DWORD ctrlTimeoutMs = 300;
        setsockopt(cli, SOL_SOCKET, SO_RCVTIMEO, (const char *)&ctrlTimeoutMs, sizeof(ctrlTimeoutMs));
    }
    marker::Consumer marks;
    uint64_t statusMsgs = 0, unknownMsgs = 0;
    while (negotiated && *running && !disconnected) {
        std::string note;
        if (marks.poll(note)) {
            // Snapshot the numbers that explain a visual artifact, at the instant it was seen.
            fprintf(stderr,
                    "[mark] note=\"%s\" rtp_recv=%llu rtp_lost=%llu (%.3f%%) gaps=%llu reorder=%llu dup=%llu "
                    "incompleteFrames=%llu decodeErrFrames=%llu pli=%llu pliSuppressed=%llu vframes=%ld\n",
                    note.c_str(),
                    (unsigned long long)lossMon->received(), (unsigned long long)lossMon->lost(),
                    lossMon->lossPercent(), (unsigned long long)lossMon->gaps(),
                    (unsigned long long)lossMon->reordered(), (unsigned long long)lossMon->duplicates(),
                    (unsigned long long)incompleteFrames.load(), (unsigned long long)decodeErrFrames.load(),
                    (unsigned long long)pliSent.load(), (unsigned long long)pliSuppressed.load(),
                    videoSink.frames());
            fflush(stderr);
            sendMsg(cli, 'M', note);   // let the phone stamp its own log at the same moment
        }

        char t; std::string payload;
        int r = recvMsgTimed(cli, t, payload);
        if (r < 0) { fprintf(stderr, "[webrtc] signaling closed by peer\n"); break; }
        if (r == 0) continue;
        if (t == 'B') {
            PhoneStatusMsg st = phonestatus::parse(payload);
            if (st.valid) { ++statusMsgs; phonestatus::emit(st); }
        } else if (++unknownMsgs <= 5) {
            // A newer phone may send message types we don't know; ignoring them is the compatibility
            // contract. Log only the first few so a chatty peer can never flood the desktop app's log.
            fprintf(stderr, "[webrtc] ignoring control message '%c' (%zu bytes)\n", t, payload.size());
        }
    }

    pc->close();
    pc.reset();                                         // ensure no more onMessage before we free decCtx
    fq.signalStop();
    at.join();
    videoSink.Stop();
    if (decCtxV) avcodec_free_context(&decCtxV);
    avcodec_free_context(&decCtx);
    avcodec_free_context(&sinkFmt);
    // One line per session with everything needed to judge link quality after the fact. Always on:
    // it is a single printf at teardown, not instrumentation.
    fprintf(stderr,
            "[webrtc] session ended (rtp=%d, vframes=%ld, status=%llu) video: recv=%llu lost=%llu (%.3f%%) "
            "gaps=%llu reorder=%llu dup=%llu incompleteFrames=%llu decodeErrFrames=%llu pli=%llu "
            "pliSuppressed=%llu — listening again\n",
            rtpCount.load(), videoSink.frames(), (unsigned long long)statusMsgs,
            (unsigned long long)lossMon->received(), (unsigned long long)lossMon->lost(),
            lossMon->lossPercent(), (unsigned long long)lossMon->gaps(),
            (unsigned long long)lossMon->reordered(), (unsigned long long)lossMon->duplicates(),
            (unsigned long long)incompleteFrames.load(), (unsigned long long)decodeErrFrames.load(),
            (unsigned long long)pliSent.load(), (unsigned long long)pliSuppressed.load());
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
        // Bound how long a half-open / stalled peer can hold this single-threaded (backlog-1) server.
        // Without a recv timeout, a peer that connects but never sends the 5-byte header wedges the
        // accept loop forever and even makes Ctrl-C un-interruptible (recvAll blocks in recv()). 20s is
        // far longer than a real pairing (the phone caps ICE gathering at 6s), so legitimate sessions
        // are never aborted; on timeout recvAll returns false and the session cleanly aborts to accept.
        // See docs/perf-audit F-01.
        DWORD sigRecvTimeoutMs = 20000;
        setsockopt(cli, SOL_SOCKET, SO_RCVTIMEO, (const char *)&sigRecvTimeoutMs, sizeof(sigRecvTimeoutMs));
        handleConnection(cli, dec, cfg, running);
        closesocket(cli);
    }

    closesocket(srv);
    WSACleanup();
    return 0;
}
