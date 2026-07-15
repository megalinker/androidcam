// Phase 2 core de-risk (docs/webrtc-migration.md): the codec bridge between libdatachannel's
// RTP and FFmpeg's Opus decoder — the new thing WebRTC adds to our receiver. A sender encodes
// a 440 Hz tone to real Opus and sends it as RTP over a DTLS-SRTP track; the receiver strips the
// RTP header (handling CSRC + extensions, as real WebRTC senders add them), feeds the Opus to
// FFmpeg's decoder, and checks the decoded PCM actually has energy. In-process (signaling proven
// in 1b). Reuses the proven roles: receiver = offerer/recvonly, sender = answerer/sendonly.

#include "rtc/rtc.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/frame.h>
#include <libavutil/samplefmt.h>
}

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

static std::atomic<bool> g_recvConnected{false}, g_sendConnected{false};
static std::atomic<bool> g_sendDone{false};
static std::atomic<int>  g_rtpIn{0}, g_framesDecoded{0};
static double g_energy = 0.0;   // sum of squares of decoded samples
static long   g_samples = 0;
static std::mutex g_decMutex;

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

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    rtc::InitLogger(rtc::LogLevel::Warning);

    // ---- FFmpeg: libopus encoder + Opus decoder (48 kHz mono) ----
    const AVCodec *enc = avcodec_find_encoder_by_name("libopus");
    if (!enc) enc = avcodec_find_encoder(AV_CODEC_ID_OPUS);
    const AVCodec *dec = avcodec_find_decoder(AV_CODEC_ID_OPUS);
    if (!enc || !dec) { printf("no opus codec (enc=%p dec=%p)\n", (void*)enc, (void*)dec); return 1; }

    AVCodecContext *encCtx = avcodec_alloc_context3(enc);
    encCtx->sample_rate = 48000;
    av_channel_layout_default(&encCtx->ch_layout, 1);
    encCtx->sample_fmt = AV_SAMPLE_FMT_FLT;   // libopus accepts FLT (FFmpeg 7.x dropped AVCodec::sample_fmts)
    encCtx->bit_rate = 64000;
    if (avcodec_open2(encCtx, enc, nullptr) < 0) { printf("enc open failed\n"); return 1; }

    AVCodecContext *decCtx = avcodec_alloc_context3(dec);
    decCtx->sample_rate = 48000;
    av_channel_layout_default(&decCtx->ch_layout, 1);
    if (avcodec_open2(decCtx, dec, nullptr) < 0) { printf("dec open failed\n"); return 1; }

    const int frameSize = encCtx->frame_size > 0 ? encCtx->frame_size : 960;  // 20 ms @ 48k
    printf("[codec] opus enc=%s fmt=%s frame=%d ; decoding with FFmpeg native\n",
           enc->name, av_get_sample_fmt_name(encCtx->sample_fmt), frameSize);

    // ---- WebRTC (in-process): receiver offers recvonly, sender answers sendonly ----
    rtc::Configuration config;
    auto receiver = std::make_shared<rtc::PeerConnection>(config);
    auto sender   = std::make_shared<rtc::PeerConnection>(config);
    receiver->onLocalDescription([sender](rtc::Description d) { sender->setRemoteDescription(std::move(d)); });
    receiver->onLocalCandidate([sender](rtc::Candidate c)     { sender->addRemoteCandidate(std::move(c)); });
    sender->onLocalDescription([receiver](rtc::Description d)  { receiver->setRemoteDescription(std::move(d)); });
    sender->onLocalCandidate([receiver](rtc::Candidate c)     { receiver->addRemoteCandidate(std::move(c)); });
    receiver->onStateChange([](rtc::PeerConnection::State s) { if (s == rtc::PeerConnection::State::Connected) g_recvConnected = true; });
    sender->onStateChange([](rtc::PeerConnection::State s)   { if (s == rtc::PeerConnection::State::Connected) g_sendConnected = true; });

    // Receiver: RTP -> strip header -> FFmpeg Opus decode -> accumulate energy.
    rtc::Description::Audio media("audio", rtc::Description::Direction::RecvOnly);
    media.addOpusCodec(111);
    media.addSSRC(42, "audio");
    auto rtrack = receiver->addTrack(media);
    rtrack->setMediaHandler(std::make_shared<rtc::RtcpReceivingSession>());
    rtrack->onMessage([decCtx](rtc::message_variant m) {
        if (!std::holds_alternative<rtc::binary>(m)) return;
        auto &pkt = std::get<rtc::binary>(m);
        const uint8_t *p = reinterpret_cast<const uint8_t *>(pkt.data());
        int off = rtpPayloadOffset(p, (int)pkt.size());
        if (off < 0 || off >= (int)pkt.size()) return;
        g_rtpIn++;
        std::lock_guard<std::mutex> lock(g_decMutex);
        AVPacket *ap = av_packet_alloc();
        ap->data = const_cast<uint8_t *>(p + off);
        ap->size = (int)pkt.size() - off;
        if (avcodec_send_packet(decCtx, ap) == 0) {
            AVFrame *fr = av_frame_alloc();
            while (avcodec_receive_frame(decCtx, fr) == 0) {
                g_framesDecoded++;
                const float *f = reinterpret_cast<const float *>(fr->extended_data[0]);
                const int16_t *s = reinterpret_cast<const int16_t *>(fr->extended_data[0]);
                for (int i = 0; i < fr->nb_samples; i++) {
                    double v = (fr->format == AV_SAMPLE_FMT_FLT || fr->format == AV_SAMPLE_FMT_FLTP)
                                   ? f[i] : (s[i] / 32768.0);
                    g_energy += v * v; g_samples++;
                }
                av_frame_unref(fr);
            }
            av_frame_free(&fr);
        }
        av_packet_free(&ap);
    });

    // Sender: encode a 440 Hz tone to Opus, send each frame as RTP.
    static std::shared_ptr<rtc::Track> keep;
    sender->onTrack([encCtx, frameSize](std::shared_ptr<rtc::Track> t) {
        keep = t;
        t->onOpen([t, encCtx, frameSize]() {
            printf("[sender] track open — encoding + sending 100 Opus frames (2s tone)\n");
            AVFrame *fr = av_frame_alloc();
            fr->format = encCtx->sample_fmt;
            av_channel_layout_copy(&fr->ch_layout, &encCtx->ch_layout);
            fr->sample_rate = 48000; fr->nb_samples = frameSize;
            av_frame_get_buffer(fr, 0);
            uint16_t seq = 0; uint32_t ts = 0; double phase = 0.0;
            for (int n = 0; n < 100; n++) {
                av_frame_make_writable(fr);
                float *ff = reinterpret_cast<float *>(fr->extended_data[0]);
                int16_t *ss = reinterpret_cast<int16_t *>(fr->extended_data[0]);
                for (int i = 0; i < frameSize; i++) {
                    double v = 0.3 * std::sin(phase); phase += 2 * 3.14159265 * 440.0 / 48000.0;
                    if (fr->format == AV_SAMPLE_FMT_FLT) ff[i] = (float)v; else ss[i] = (int16_t)(v * 32767);
                }
                if (avcodec_send_frame(encCtx, fr) == 0) {
                    AVPacket *ap = av_packet_alloc();
                    while (avcodec_receive_packet(encCtx, ap) == 0) {
                        std::vector<uint8_t> rtp(12 + ap->size);
                        rtp[0] = 0x80; rtp[1] = 111;
                        rtp[2] = seq >> 8; rtp[3] = seq & 0xff;
                        rtp[4] = ts >> 24; rtp[5] = ts >> 16; rtp[6] = ts >> 8; rtp[7] = ts;
                        rtp[8] = 0; rtp[9] = 0; rtp[10] = 0; rtp[11] = 42;
                        std::memcpy(rtp.data() + 12, ap->data, ap->size);
                        t->send(reinterpret_cast<const std::byte *>(rtp.data()), rtp.size());
                        seq++; ts += frameSize;
                        av_packet_unref(ap);
                    }
                    av_packet_free(&ap);
                }
                std::this_thread::sleep_for(20ms);
            }
            av_frame_free(&fr);
            printf("[sender] done sending\n");
            g_sendDone = true;
        });
    });

    receiver->setLocalDescription();

    for (int i = 0; i < 150 && (g_framesDecoded < 90 || !g_sendDone); i++) std::this_thread::sleep_for(100ms);

    double rms = g_samples ? std::sqrt(g_energy / g_samples) : 0.0;
    printf("\n=== RESULT ===\n");
    printf("connected:        %s / %s\n", g_recvConnected ? "recv" : "-", g_sendConnected ? "send" : "-");
    printf("RTP in:           %d\n", g_rtpIn.load());
    printf("Opus frames dec:  %d\n", g_framesDecoded.load());
    printf("decoded samples:  %ld  RMS=%.4f (tone should be ~0.2)\n", g_samples, rms);
    bool ok = g_recvConnected && g_framesDecoded >= 90 && rms > 0.05;
    printf("AUDIO %s\n", ok ? "PASS" : "FAIL");
    sender->close(); receiver->close();
    keep.reset(); rtrack.reset(); sender.reset(); receiver.reset();
    std::this_thread::sleep_for(100ms);
    {
        std::lock_guard<std::mutex> lock(g_decMutex);
        avcodec_free_context(&encCtx); avcodec_free_context(&decCtx);
    }
    return ok ? 0 : 1;
}
