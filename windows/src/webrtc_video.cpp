// Phase 5a de-risk: the VIDEO codec bridge between libdatachannel's H.264
// RTP and FFmpeg's H.264 decoder — the new thing Phase 5 adds. A sender encodes a moving test pattern
// with libx264 (zerolatency, no B-frames) -> H264RtpPacketizer -> DTLS-SRTP -> H264RtpDepacketizer ->
// onFrame -> FFmpeg h264 decode, and we verify the frames decode at the right size and actually move.
// In-process (signaling proven in 1b). Roles: receiver = offerer/recvonly, sender = answerer/sendonly.

#include "rtc/rtc.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
}

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

static std::atomic<bool> g_recvConnected{false}, g_sendConnected{false};
static std::atomic<int>  g_frames{0};
static std::atomic<int>  g_w{0}, g_h{0};
static std::mutex g_decMutex;
static std::vector<int> g_boxCols;   // detected bright-box column per decoded frame (to prove motion)

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    rtc::InitLogger(rtc::LogLevel::Warning);

    const int W = 320, H = 240, FPS = 30, NFR = 90;   // 3 s of 320x240

    // ---- FFmpeg: libx264 encoder (Annex-B, low-latency) + H.264 decoder ----
    const AVCodec *enc = avcodec_find_encoder_by_name("libx264");
    const AVCodec *dec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!enc || !dec) { printf("no h264 codec (enc=%p dec=%p)\n", (void *)enc, (void *)dec); return 1; }

    AVCodecContext *encCtx = avcodec_alloc_context3(enc);
    encCtx->width = W; encCtx->height = H;
    encCtx->pix_fmt = AV_PIX_FMT_YUV420P;
    encCtx->time_base = {1, FPS};
    encCtx->framerate = {FPS, 1};
    encCtx->gop_size = FPS;             // 1 s keyframe interval
    encCtx->max_b_frames = 0;           // no reordering -> no latency
    encCtx->bit_rate = 600000;
    av_opt_set(encCtx->priv_data, "preset", "ultrafast", 0);
    av_opt_set(encCtx->priv_data, "tune", "zerolatency", 0);
    if (avcodec_open2(encCtx, enc, nullptr) < 0) { printf("x264 open failed\n"); return 1; }

    AVCodecContext *decCtx = avcodec_alloc_context3(dec);
    if (avcodec_open2(decCtx, dec, nullptr) < 0) { printf("h264 dec open failed\n"); return 1; }
    printf("[codec] enc=%s %dx%d@%d annexb ; decoding with FFmpeg h264\n", enc->name, W, H, FPS);

    // ---- WebRTC (in-process): receiver offers recvonly H264, sender answers sendonly ----
    rtc::Configuration config;
    auto receiver = std::make_shared<rtc::PeerConnection>(config);
    auto sender   = std::make_shared<rtc::PeerConnection>(config);
    receiver->onLocalDescription([sender](rtc::Description d) { sender->setRemoteDescription(std::move(d)); });
    receiver->onLocalCandidate([sender](rtc::Candidate c)     { sender->addRemoteCandidate(std::move(c)); });
    sender->onLocalDescription([receiver](rtc::Description d)  { receiver->setRemoteDescription(std::move(d)); });
    sender->onLocalCandidate([receiver](rtc::Candidate c)     { receiver->addRemoteCandidate(std::move(c)); });
    receiver->onStateChange([](rtc::PeerConnection::State s) { if (s == rtc::PeerConnection::State::Connected) g_recvConnected = true; });
    sender->onStateChange([](rtc::PeerConnection::State s)   { if (s == rtc::PeerConnection::State::Connected) g_sendConnected = true; });

    // Receiver: H264 RTP -> depacketize (Annex-B) -> FFmpeg decode -> record size + box position.
    rtc::Description::Video media("video", rtc::Description::Direction::RecvOnly);
    media.addH264Codec(96);
    media.addSSRC(42, "video");
    auto rtrack = receiver->addTrack(media);
    auto depack = std::make_shared<rtc::H264RtpDepacketizer>(rtc::NalUnit::Separator::StartSequence);
    depack->addToChain(std::make_shared<rtc::RtcpReceivingSession>());
    rtrack->setMediaHandler(depack);
    rtrack->onFrame([decCtx](rtc::binary data, rtc::FrameInfo) {
        std::lock_guard<std::mutex> lock(g_decMutex);
        AVPacket *pk = av_packet_alloc();
        if (av_new_packet(pk, (int)data.size()) == 0) {
            std::memcpy(pk->data, data.data(), data.size());
            if (avcodec_send_packet(decCtx, pk) == 0) {
                AVFrame *fr = av_frame_alloc();
                while (avcodec_receive_frame(decCtx, fr) == 0) {
                    g_frames++; g_w = fr->width; g_h = fr->height;
                    // find the brightest column on the middle row (the white box) to prove motion
                    const uint8_t *row = fr->data[0] + (fr->height / 2) * fr->linesize[0];
                    int bestX = 0, bestV = -1;
                    for (int x = 0; x < fr->width; x++) if (row[x] > bestV) { bestV = row[x]; bestX = x; }
                    g_boxCols.push_back(bestX);
                    av_frame_unref(fr);
                }
                av_frame_free(&fr);
            }
        }
        av_packet_free(&pk);
    });

    // Sender: on the reciprocal track, packetize + send an encoded moving box.
    static std::shared_ptr<rtc::Track> keep;
    sender->onTrack([encCtx, W, H, FPS, NFR](std::shared_ptr<rtc::Track> t) {
        keep = t;
        auto rtpConfig = std::make_shared<rtc::RtpPacketizationConfig>(42, "video", 96, rtc::H264RtpPacketizer::ClockRate);
        t->setMediaHandler(std::make_shared<rtc::H264RtpPacketizer>(rtc::NalUnit::Separator::StartSequence, rtpConfig));
        t->onOpen([t, encCtx, W, H, FPS, NFR]() {
            printf("[sender] track open — encoding + sending %d H264 frames (moving box)\n", NFR);
            AVFrame *fr = av_frame_alloc();
            fr->format = AV_PIX_FMT_YUV420P; fr->width = W; fr->height = H;
            av_frame_get_buffer(fr, 0);
            AVPacket *pk = av_packet_alloc();
            for (int n = 0; n < NFR; n++) {
                av_frame_make_writable(fr);
                // gray background, a 30px white box sliding left->right (position encodes the frame)
                memset(fr->data[0], 90, (size_t)fr->linesize[0] * H);
                memset(fr->data[1], 128, (size_t)fr->linesize[1] * (H / 2));
                memset(fr->data[2], 128, (size_t)fr->linesize[2] * (H / 2));
                int bx = (n * 3) % (W - 30);
                for (int y = H / 2 - 15; y < H / 2 + 15; y++)
                    memset(fr->data[0] + y * fr->linesize[0] + bx, 235, 30);
                fr->pts = n;
                if (avcodec_send_frame(encCtx, fr) == 0) {
                    while (avcodec_receive_packet(encCtx, pk) == 0) {
                        rtc::binary annexb(reinterpret_cast<std::byte *>(pk->data),
                                           reinterpret_cast<std::byte *>(pk->data) + pk->size);
                        t->sendFrame(std::move(annexb), rtc::FrameInfo((uint32_t)(n * 90000 / FPS)));
                        av_packet_unref(pk);
                    }
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1000 / FPS));
            }
            av_packet_free(&pk);
            av_frame_free(&fr);
            printf("[sender] done sending\n");
        });
    });

    receiver->setLocalDescription();

    for (int i = 0; i < 120 && g_frames < NFR - 5; i++) std::this_thread::sleep_for(100ms);

    // Did the box actually move? Count how many distinct columns we saw.
    int distinct = 0;
    { std::lock_guard<std::mutex> lock(g_decMutex);
      std::vector<int> cols = g_boxCols;
      std::sort(cols.begin(), cols.end());
      cols.erase(std::unique(cols.begin(), cols.end()), cols.end());
      distinct = (int)cols.size(); }

    printf("\n=== RESULT ===\n");
    printf("connected:      %s / %s\n", g_recvConnected ? "recv" : "-", g_sendConnected ? "send" : "-");
    printf("frames decoded: %d / %d  at %dx%d\n", g_frames.load(), NFR, g_w.load(), g_h.load());
    printf("box positions:  %d distinct columns (motion)\n", distinct);
    bool ok = g_recvConnected && g_frames >= NFR - 20 && g_w == W && g_h == H && distinct >= 10;
    printf("VIDEO %s\n", ok ? "PASS" : "FAIL");
    avcodec_free_context(&encCtx); avcodec_free_context(&decCtx);
    return ok ? 0 : 1;
}
