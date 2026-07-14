// Phase 2b test sender — stands in for the phone so receiver.exe --webrtc can be validated
// without the tester's device. Speaks the real PCAM3 wire protocol (TCP client / answerer) and
// sends a real Opus-encoded tone over a DTLS-SRTP audio track, exactly as the WebRTC Android
// phone will. Point receiver.exe at "CABLE Input" and this proves audio traverses the whole
// WebRTC pipeline into the virtual mic.  Usage: webrtc_testsender <host> <port> <secret> [seconds]

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
    int nframes = seconds * 50;   // 20ms frames

    WSADATA wsa; WSAStartup(MAKEWORD(2, 2), &wsa);
    rtc::InitLogger(rtc::LogLevel::Warning);

    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons((u_short)port);
    inet_pton(AF_INET, host, &a.sin_addr);
    if (connect(s, (sockaddr *)&a, sizeof a) != 0) { printf("connect(%s:%d) failed %d\n", host, port, WSAGetLastError()); return 1; }
    printf("[sender] connected to %s:%d\n", host, port);
    sendMsg(s, 'S', secret);

    // Opus encoder (48k mono, matches the phone's mic path).
    const AVCodec *enc = avcodec_find_encoder_by_name("libopus");
    if (!enc) enc = avcodec_find_encoder(AV_CODEC_ID_OPUS);
    AVCodecContext *encCtx = avcodec_alloc_context3(enc);
    encCtx->sample_rate = 48000; av_channel_layout_default(&encCtx->ch_layout, 1);
    encCtx->sample_fmt = AV_SAMPLE_FMT_FLT; encCtx->bit_rate = 64000;
    avcodec_open2(encCtx, enc, nullptr);
    const int frameSize = encCtx->frame_size > 0 ? encCtx->frame_size : 960;

    rtc::Configuration config;
    auto pc = std::make_shared<rtc::PeerConnection>(config);
    std::atomic<bool> done{false};

    pc->onGatheringStateChange([&pc, s](rtc::PeerConnection::GatheringState g) {
        if (g == rtc::PeerConnection::GatheringState::Complete)
            if (auto d = pc->localDescription()) { printf("[sender] sending answer\n"); sendMsg(s, 'A', std::string(*d)); }
    });

    static std::shared_ptr<rtc::Track> keep;
    pc->onTrack([&, encCtx, frameSize, nframes](std::shared_ptr<rtc::Track> t) {
        keep = t;
        t->onOpen([&, t, encCtx, frameSize, nframes]() {
            printf("[sender] track open — sending %ds of 440Hz Opus tone\n", nframes / 50);
            AVFrame *fr = av_frame_alloc();
            fr->format = encCtx->sample_fmt; av_channel_layout_copy(&fr->ch_layout, &encCtx->ch_layout);
            fr->sample_rate = 48000; fr->nb_samples = frameSize; av_frame_get_buffer(fr, 0);
            uint16_t seq = 0; uint32_t ts = 0; double phase = 0.0;
            for (int n = 0; n < nframes; n++) {
                av_frame_make_writable(fr);
                float *ff = reinterpret_cast<float *>(fr->extended_data[0]);
                for (int i = 0; i < frameSize; i++) { ff[i] = (float)(0.3 * std::sin(phase)); phase += 2 * 3.14159265 * 440.0 / 48000.0; }
                if (avcodec_send_frame(encCtx, fr) == 0) {
                    AVPacket *ap = av_packet_alloc();
                    while (avcodec_receive_packet(encCtx, ap) == 0) {
                        std::vector<uint8_t> rtp(12 + ap->size);
                        rtp[0] = 0x80; rtp[1] = 111;
                        rtp[2] = seq >> 8; rtp[3] = seq & 0xff;
                        rtp[4] = ts >> 24; rtp[5] = ts >> 16; rtp[6] = ts >> 8; rtp[7] = ts;
                        rtp[11] = 42;
                        std::memcpy(rtp.data() + 12, ap->data, ap->size);
                        t->send(reinterpret_cast<const std::byte *>(rtp.data()), rtp.size());
                        seq++; ts += frameSize; av_packet_unref(ap);
                    }
                    av_packet_free(&ap);
                }
                std::this_thread::sleep_for(20ms);
            }
            av_frame_free(&fr);
            printf("[sender] done — sent %d frames\n", nframes);
            done = true;
        });
    });

    // Answerer: recv offer -> setRemoteDescription -> setLocalDescription (answer) -> onTrack.
    char t; std::string pl;
    if (recvMsg(s, t, pl) && t == 'O') {
        printf("[sender] got offer — answering\n");
        pc->setRemoteDescription(rtc::Description(pl, "offer"));
        pc->setLocalDescription();
    } else { printf("[sender] no offer\n"); return 1; }

    for (int i = 0; i < (seconds + 8) * 10 && !done; i++) std::this_thread::sleep_for(100ms);
    std::this_thread::sleep_for(500ms);
    printf("[sender] exit\n");
    avcodec_free_context(&encCtx);
    return 0;
}
