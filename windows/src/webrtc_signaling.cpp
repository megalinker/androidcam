// Phase 1b (docs/webrtc-migration.md): the PCAM3 signaling wire. The QR carries
// PCAM3:<pcIP>:<sigPort>:<pairSecret>; the phone connects over TCP and the two exchange
// SDP offer/answer (PC offers recvonly, phone answers sendonly), pairSecret-gated. Then
// WebRTC media (DTLS-SRTP/UDP) flows LAN-direct. This test runs BOTH roles in one process
// over 127.0.0.1 to validate the wire protocol + full WebRTC bring-up over it.
//
// Framing (easy to mirror in Kotlin on the phone): [1 byte type]['S'|'O'|'A'][4-byte BE len][payload].
//   S = pairSecret,  O = offer SDP,  A = answer SDP.
// Flow:  phone→PC 'S'   |   PC→phone 'O'   |   phone→PC 'A'.

#include "rtc/rtc.hpp"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>

using namespace std::chrono_literals;

static std::atomic<int>  g_received{0};
static std::atomic<bool> g_pcConnected{false}, g_phoneConnected{false};
static const char *kSecret = "pcam3-test-secret";
static const uint16_t kPort = 9899;

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

// ---- length-prefixed message framing over TCP ----
static bool sendAll(SOCKET s, const char *buf, int len) {
    int sent = 0;
    while (sent < len) { int n = send(s, buf + sent, len - sent, 0); if (n <= 0) return false; sent += n; }
    return true;
}
static bool recvAll(SOCKET s, char *buf, int len) {
    int got = 0;
    while (got < len) { int n = recv(s, buf + got, len - got, 0); if (n <= 0) return false; got += n; }
    return true;
}
static bool sendMsg(SOCKET s, char type, const std::string &p) {
    uint32_t len = (uint32_t)p.size();
    char hdr[5] = {type, (char)(len >> 24), (char)(len >> 16), (char)(len >> 8), (char)len};
    return sendAll(s, hdr, 5) && (len == 0 || sendAll(s, p.data(), (int)len));
}
static bool recvMsg(SOCKET s, char &type, std::string &p) {
    char hdr[5];
    if (!recvAll(s, hdr, 5)) return false;
    type = hdr[0];
    uint32_t len = ((uint32_t)(uint8_t)hdr[1] << 24) | ((uint32_t)(uint8_t)hdr[2] << 16) |
                   ((uint32_t)(uint8_t)hdr[3] << 8) | (uint32_t)(uint8_t)hdr[4];
    p.resize(len);
    return len == 0 || recvAll(s, &p[0], (int)len);
}

// ================= PC side: TCP server, WebRTC offerer (recvonly) =================
static void runPc() {
    SOCKET srv = socket(AF_INET, SOCK_STREAM, 0);
    int yes = 1; setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, (char *)&yes, sizeof(yes));
    sockaddr_in addr = {}; addr.sin_family = AF_INET; addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); addr.sin_port = htons(kPort);
    if (bind(srv, (sockaddr *)&addr, sizeof(addr)) != 0) { printf("[pc] bind failed\n"); return; }
    listen(srv, 1);
    printf("[pc] listening on 127.0.0.1:%u\n", kPort);
    SOCKET cli = accept(srv, nullptr, nullptr);
    if (cli == INVALID_SOCKET) { printf("[pc] accept failed\n"); return; }

    char type; std::string payload;
    if (!recvMsg(cli, type, payload) || type != 'S') { printf("[pc] no secret\n"); closesocket(cli); return; }
    if (payload != kSecret) { printf("[pc] BAD SECRET — rejecting\n"); closesocket(cli); return; }
    printf("[pc] secret OK — building offer\n");

    rtc::Configuration config;
    auto pc = std::make_shared<rtc::PeerConnection>(config);
    pc->onStateChange([](rtc::PeerConnection::State s) { if (s == rtc::PeerConnection::State::Connected) g_pcConnected = true; });
    pc->onGatheringStateChange([pc, cli](rtc::PeerConnection::GatheringState s) {
        if (s == rtc::PeerConnection::GatheringState::Complete) {
            auto d = pc->localDescription();
            printf("[pc] gathering done — sending offer\n");
            sendMsg(cli, 'O', std::string(*d));
        }
    });

    rtc::Description::Audio media("audio", rtc::Description::Direction::RecvOnly);
    media.addOpusCodec(111);
    media.addSSRC(42, "pc-recv");
    auto track = pc->addTrack(media);
    track->setMediaHandler(std::make_shared<rtc::RtcpReceivingSession>());
    track->onMessage([](rtc::message_variant m) { if (std::holds_alternative<rtc::binary>(m)) g_received++; });
    pc->setLocalDescription();

    if (recvMsg(cli, type, payload) && type == 'A') {
        printf("[pc] got answer — connecting\n");
        pc->setRemoteDescription(rtc::Description(payload, "answer"));
    }
    for (int i = 0; i < 100 && g_received < 40; i++) std::this_thread::sleep_for(100ms);
    closesocket(cli); closesocket(srv);
}

// ================= phone side: TCP client, WebRTC answerer (sendonly) =================
static void runPhone() {
    std::this_thread::sleep_for(300ms);   // let the PC bind/listen first
    SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr = {}; addr.sin_family = AF_INET; addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); addr.sin_port = htons(kPort);
    if (connect(s, (sockaddr *)&addr, sizeof(addr)) != 0) { printf("[phone] connect failed\n"); return; }
    sendMsg(s, 'S', kSecret);

    char type; std::string payload;
    if (!recvMsg(s, type, payload) || type != 'O') { printf("[phone] no offer\n"); closesocket(s); return; }
    printf("[phone] got offer — answering\n");

    rtc::Configuration config;
    auto pc = std::make_shared<rtc::PeerConnection>(config);
    pc->onStateChange([](rtc::PeerConnection::State st) { if (st == rtc::PeerConnection::State::Connected) g_phoneConnected = true; });
    static std::shared_ptr<rtc::Track> keep;
    pc->onTrack([](std::shared_ptr<rtc::Track> t) {
        keep = t;
        t->onOpen([t]() {
            printf("[phone] track open — sending 50 RTP\n");
            for (uint16_t i = 0; i < 50; i++) { t->send(makeRtp(i, i * 960, 42)); std::this_thread::sleep_for(10ms); }
        });
    });
    pc->onGatheringStateChange([pc, s](rtc::PeerConnection::GatheringState st) {
        if (st == rtc::PeerConnection::GatheringState::Complete) {
            auto d = pc->localDescription();
            printf("[phone] gathering done — sending answer\n");
            sendMsg(s, 'A', std::string(*d));
        }
    });
    pc->setRemoteDescription(rtc::Description(payload, "offer"));   // auto-creates the answer

    for (int i = 0; i < 120 && g_received < 40; i++) std::this_thread::sleep_for(100ms);
    closesocket(s);
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    WSADATA wsa; WSAStartup(MAKEWORD(2, 2), &wsa);
    rtc::InitLogger(rtc::LogLevel::Warning);

    std::thread pcThread(runPc);
    runPhone();
    pcThread.join();

    printf("\n=== RESULT ===\n");
    printf("PC connected:    %s\n", g_pcConnected ? "yes" : "no");
    printf("phone connected: %s\n", g_phoneConnected ? "yes" : "no");
    printf("RTP received:    %d / 50\n", g_received.load());
    bool ok = g_pcConnected && g_phoneConnected && g_received >= 40;
    printf("SIGNALING %s\n", ok ? "PASS" : "FAIL");
    WSACleanup();
    return ok ? 0 : 1;
}
