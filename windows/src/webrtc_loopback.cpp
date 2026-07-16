// Phase 1 de-risk: two libdatachannel PeerConnections in one
// process exchange SDP + ICE candidates (this in-process wiring becomes the PCAM3 TCP
// signaling channel), establish a DTLS-SRTP connection, and send/receive Opus RTP.
//
// Roles match production: the *receiver* (our PC) is the OFFERER with a recvonly track
// (+ RtcpReceivingSession) — the pattern every working libdatachannel media example uses;
// the *sender* (our phone) is the ANSWERER that reciprocates with a sendonly track. Offering
// recvonly is what keeps the negotiated m-line valid (a sendonly offer to an unprepared
// answerer gets rejected with port 0 and never opens).
//
// Build: cmake -DWITH_WEBRTC=ON …  →  webrtc_loopback.exe

#include "rtc/rtc.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <thread>

using namespace std::chrono_literals;

static std::atomic<int>  g_received{0};
static std::atomic<bool> g_recvConnected{false}, g_sendConnected{false};

static const char *stateName(rtc::PeerConnection::State s) {
    using S = rtc::PeerConnection::State;
    switch (s) {
        case S::New: return "New"; case S::Connecting: return "Connecting";
        case S::Connected: return "Connected"; case S::Disconnected: return "Disconnected";
        case S::Failed: return "Failed"; case S::Closed: return "Closed"; default: return "?";
    }
}

static rtc::binary makeRtp(uint16_t seq, uint32_t ts, uint32_t ssrc) {
    uint8_t h[32] = {0};
    h[0] = 0x80; h[1] = 111;
    h[2] = seq >> 8;  h[3] = seq & 0xff;
    h[4] = ts >> 24;  h[5] = ts >> 16;  h[6] = ts >> 8;  h[7] = ts;
    h[8] = ssrc >> 24; h[9] = ssrc >> 16; h[10] = ssrc >> 8; h[11] = ssrc;
    for (int i = 12; i < 32; i++) h[i] = (uint8_t)i;
    rtc::binary p(32);
    std::memcpy(p.data(), h, 32);
    return p;
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    rtc::InitLogger(rtc::LogLevel::Warning);
    rtc::Configuration config;   // no ICE servers → LAN host candidates only

    auto receiver = std::make_shared<rtc::PeerConnection>(config);   // OFFERER (recvonly) — the PC
    auto sender   = std::make_shared<rtc::PeerConnection>(config);   // ANSWERER (sendonly) — the phone

    // ---- signaling (becomes the PCAM3 TCP channel) ----
    receiver->onLocalDescription([sender](rtc::Description d) { sender->setRemoteDescription(std::move(d)); });
    receiver->onLocalCandidate([sender](rtc::Candidate c)     { sender->addRemoteCandidate(std::move(c)); });
    sender->onLocalDescription([receiver](rtc::Description d)  { receiver->setRemoteDescription(std::move(d)); });
    sender->onLocalCandidate([receiver](rtc::Candidate c)     { receiver->addRemoteCandidate(std::move(c)); });

    receiver->onStateChange([](rtc::PeerConnection::State s) {
        printf("[receiver] %s\n", stateName(s));
        if (s == rtc::PeerConnection::State::Connected) g_recvConnected = true;
    });
    sender->onStateChange([](rtc::PeerConnection::State s) {
        printf("[sender]   %s\n", stateName(s));
        if (s == rtc::PeerConnection::State::Connected) g_sendConnected = true;
    });

    const uint32_t ssrc = 42;

    // Sender (answerer): gets the reciprocal send-only track via onTrack, sends RTP when it opens.
    static std::shared_ptr<rtc::Track> keepAlive;
    sender->onTrack([ssrc](std::shared_ptr<rtc::Track> t) {
        printf("[sender]   onTrack mid=%s\n", t->mid().c_str());
        keepAlive = t;
        t->onOpen([t, ssrc]() {
            printf("[sender]   track open — sending 50 RTP\n");
            for (uint16_t i = 0; i < 50; i++) { t->send(makeRtp(i, i * 960, ssrc)); std::this_thread::sleep_for(10ms); }
        });
    });

    // Receiver (offerer): recvonly Opus track + RtcpReceivingSession, then offer.
    rtc::Description::Audio media("audio", rtc::Description::Direction::RecvOnly);
    media.addOpusCodec(111);
    media.addSSRC(ssrc, "loopback-audio");
    auto rtrack = receiver->addTrack(media);
    rtrack->setMediaHandler(std::make_shared<rtc::RtcpReceivingSession>());
    rtrack->onMessage([](rtc::message_variant msg) {
        if (std::holds_alternative<rtc::binary>(msg)) g_received++;
    });
    receiver->setLocalDescription();

    for (int i = 0; i < 100 && g_received < 40; i++) std::this_thread::sleep_for(100ms);

    printf("\n=== RESULT ===\n");
    printf("receiver connected: %s\n", g_recvConnected ? "yes" : "no");
    printf("sender connected:   %s\n", g_sendConnected ? "yes" : "no");
    printf("RTP received:       %d / 50\n", g_received.load());
    bool ok = g_recvConnected && g_sendConnected && g_received >= 40;
    printf("LOOPBACK %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
