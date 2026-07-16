// Phase 2b/5b test sender — stands in for the phone so receiver.exe --webrtc[-video] can be
// validated without the tester's device. Speaks the real PCAM3 wire protocol (TCP client /
// answerer) and, per track the PC offers, sends a real Opus tone and/or a real H.264 moving-box
// video over DTLS-SRTP — exactly as the WebRTC Android phone will.
//   webrtc_testsender <host> <port> <secret> [seconds]

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
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
}

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

static constexpr int VW = 320, VH = 240, VFPS = 30;   // test video geometry (file scope: usable in lambdas)

static bool sendAll(SOCKET s, const char *b, int n) { while (n > 0) { int k = send(s, b, n, 0); if (k <= 0) return false; b += k; n -= k; } return true; }
static bool recvAll(SOCKET s, char *b, int n)       { while (n > 0) { int k = recv(s, b, n, 0); if (k <= 0) return false; b += k; n -= k; } return true; }
static bool sendMsg(SOCKET s, char t, const std::string &p) {
    char h[5]; uint32_t n = (uint32_t)p.size();
    h[0] = t; h[1] = (char)(n >> 24); h[2] = (char)(n >> 16); h[3] = (char)(n >> 8); h[4] = (char)n;
    return sendAll(s, h, 5) && (n == 0 || sendAll(s, p.data(), (int)n));
}
static bool recvMsg(SOCKET s, char &t, std::string &p) {
    char h[5]; if (!recvAll(s, h, 5)) return false;
    t = h[0];
    uint32_t n = ((uint32_t)(uint8_t)h[1] << 24) | ((uint32_t)(uint8_t)h[2] << 16) | ((uint32_t)(uint8_t)h[3] << 8) | (uint32_t)(uint8_t)h[4];
    p.resize(n);
    return n == 0 || recvAll(s, &p[0], (int)n);
}

int main(int argc, char **argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 4) { printf("usage: %s <host> <port> <secret> [seconds]\n", argv[0]); return 2; }
    const char *host = argv[1];
    int port = atoi(argv[2]);
    std::string secret = argv[3];
    int seconds = argc > 4 ? atoi(argv[4]) : 5;

    WSADATA wsa; WSAStartup(MAKEWORD(2, 2), &wsa);
    rtc::InitLogger(rtc::LogLevel::Warning);

    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons((u_short)port);
    inet_pton(AF_INET, host, &a.sin_addr);
    if (connect(s, (sockaddr *)&a, sizeof a) != 0) { printf("connect(%s:%d) failed %d\n", host, port, WSAGetLastError()); return 1; }
    printf("[sender] connected to %s:%d\n", host, port);
    sendMsg(s, 'S', secret);

    // Opus encoder (audio) — matches the phone mic path.
    const AVCodec *aenc = avcodec_find_encoder_by_name("libopus");
    if (!aenc) aenc = avcodec_find_encoder(AV_CODEC_ID_OPUS);
    AVCodecContext *aCtx = avcodec_alloc_context3(aenc);
    aCtx->sample_rate = 48000; av_channel_layout_default(&aCtx->ch_layout, 1);
    aCtx->sample_fmt = AV_SAMPLE_FMT_FLT; aCtx->bit_rate = 64000;
    avcodec_open2(aCtx, aenc, nullptr);
    const int frameSize = aCtx->frame_size > 0 ? aCtx->frame_size : 960;

    // libx264 encoder (video) — 320x240 moving box, zerolatency, no B-frames.
    const AVCodec *venc = avcodec_find_encoder_by_name("libx264");
    AVCodecContext *vCtx = avcodec_alloc_context3(venc);
    vCtx->width = VW; vCtx->height = VH; vCtx->pix_fmt = AV_PIX_FMT_YUV420P;
    vCtx->time_base = {1, VFPS}; vCtx->framerate = {VFPS, 1};
    vCtx->gop_size = VFPS; vCtx->max_b_frames = 0; vCtx->bit_rate = 800000;
    av_opt_set(vCtx->priv_data, "preset", "ultrafast", 0);
    av_opt_set(vCtx->priv_data, "tune", "zerolatency", 0);
    avcodec_open2(vCtx, venc, nullptr);

    rtc::Configuration config;
    auto pc = std::make_shared<rtc::PeerConnection>(config);
    pc->onGatheringStateChange([&pc, s](rtc::PeerConnection::GatheringState g) {
        if (g == rtc::PeerConnection::GatheringState::Complete)
            if (auto d = pc->localDescription()) { printf("[sender] sending answer\n"); sendMsg(s, 'A', std::string(*d)); }
    });

    static std::vector<std::shared_ptr<rtc::Track>> keep;
    pc->onTrack([&, aCtx, frameSize, vCtx, seconds](std::shared_ptr<rtc::Track> t) {
        keep.push_back(t);
        bool isVideo = t->description().type() == "video";
        if (isVideo) {
            auto cfg = std::make_shared<rtc::RtpPacketizationConfig>(43, "video", 96, rtc::H264RtpPacketizer::ClockRate);
            t->setMediaHandler(std::make_shared<rtc::H264RtpPacketizer>(rtc::NalUnit::Separator::StartSequence, cfg));
            t->onOpen([t, vCtx, seconds]() {
                int nfr = seconds * VFPS;
                printf("[sender] video track open — sending %ds of H264 (moving box)\n", seconds);
                AVFrame *fr = av_frame_alloc();
                fr->format = AV_PIX_FMT_YUV420P; fr->width = VW; fr->height = VH; av_frame_get_buffer(fr, 0);
                AVPacket *pk = av_packet_alloc();
                for (int n = 0; n < nfr; n++) {
                    av_frame_make_writable(fr);
                    memset(fr->data[0], 90, (size_t)fr->linesize[0] * VH);
                    memset(fr->data[1], 128, (size_t)fr->linesize[1] * (VH / 2));
                    memset(fr->data[2], 128, (size_t)fr->linesize[2] * (VH / 2));
                    int bx = (n * 3) % (VW - 30);
                    for (int y = VH / 2 - 15; y < VH / 2 + 15; y++) memset(fr->data[0] + y * fr->linesize[0] + bx, 235, 30);
                    fr->pts = n;
                    if (avcodec_send_frame(vCtx, fr) == 0)
                        while (avcodec_receive_packet(vCtx, pk) == 0) {
                            rtc::binary au(reinterpret_cast<std::byte *>(pk->data), reinterpret_cast<std::byte *>(pk->data) + pk->size);
                            t->sendFrame(std::move(au), rtc::FrameInfo((uint32_t)(n * 90000 / VFPS)));
                            av_packet_unref(pk);
                        }
                    std::this_thread::sleep_for(std::chrono::milliseconds(1000 / VFPS));
                }
                av_packet_free(&pk); av_frame_free(&fr);
                printf("[sender] video done\n");
            });
        } else {
            t->onOpen([t, aCtx, frameSize, seconds]() {
                int nframes = seconds * 50;
                printf("[sender] audio track open — sending %ds of 440Hz Opus tone\n", seconds);
                AVFrame *fr = av_frame_alloc();
                fr->format = aCtx->sample_fmt; av_channel_layout_copy(&fr->ch_layout, &aCtx->ch_layout);
                fr->sample_rate = 48000; fr->nb_samples = frameSize; av_frame_get_buffer(fr, 0);
                uint16_t seq = 0; uint32_t ts = 0; double phase = 0.0;
                AVPacket *ap = av_packet_alloc();
                for (int n = 0; n < nframes; n++) {
                    av_frame_make_writable(fr);
                    float *ff = reinterpret_cast<float *>(fr->extended_data[0]);
                    for (int i = 0; i < frameSize; i++) { ff[i] = (float)(0.3 * std::sin(phase)); phase += 2 * 3.14159265 * 440.0 / 48000.0; }
                    if (avcodec_send_frame(aCtx, fr) == 0)
                        while (avcodec_receive_packet(aCtx, ap) == 0) {
                            std::vector<uint8_t> rtp(12 + ap->size);
                            rtp[0] = 0x80; rtp[1] = 111; rtp[2] = seq >> 8; rtp[3] = seq & 0xff;
                            rtp[4] = ts >> 24; rtp[5] = ts >> 16; rtp[6] = ts >> 8; rtp[7] = ts; rtp[11] = 42;
                            std::memcpy(rtp.data() + 12, ap->data, ap->size);
                            t->send(reinterpret_cast<const std::byte *>(rtp.data()), rtp.size());
                            seq++; ts += frameSize; av_packet_unref(ap);
                        }
                    std::this_thread::sleep_for(20ms);
                }
                av_packet_free(&ap); av_frame_free(&fr);
                printf("[sender] audio done\n");
            });
        }
    });

    char t; std::string pl;
    if (recvMsg(s, t, pl) && t == 'O') {
        printf("[sender] got offer — answering\n");
        pc->setRemoteDescription(rtc::Description(pl, "offer"));
        pc->setLocalDescription();
    } else { printf("[sender] no offer\n"); return 1; }

    for (int i = 0; i < (seconds + 8) * 10; i++) std::this_thread::sleep_for(100ms);
    printf("[sender] exit\n");
    avcodec_free_context(&aCtx); avcodec_free_context(&vCtx);
    return 0;
}
