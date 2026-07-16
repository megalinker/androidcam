// WebRTC receive session for receiver.exe (--webrtc). Runs a PCAM3 TCP signaling server
// (offerer role), accepts an Opus audio track over DTLS-SRTP, decodes it with FFmpeg, and
// feeds AVFrames to the existing WasapiSink (CABLE / virtual mic). See docs/webrtc-migration.md.
#pragma once

#include <atomic>
#include <string>

struct WebrtcRecvConfig {
    int         sigPort = 0;        // TCP signaling port (the PCAM3 QR points here)
    std::string sigSecret;         // pairSecret gate
    std::string audioDevice;       // WASAPI endpoint substring (e.g. "CABLE Input")
    float       micGainDb = 0.0f;  // mic boost (soft-limited in the sink)
    std::string eqPreset;          // voice EQ preset / band list
    bool        wantVideo = false; // also offer a recvonly H.264 video track -> softcam (Phase 5)
};

// Runs until *running becomes false (Ctrl+C in receiver.exe). Re-listens between phone sessions.
int run_webrtc_session(const WebrtcRecvConfig &cfg, std::atomic<bool> *running);
