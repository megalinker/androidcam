// PhoneCam receiver — WebRTC (DTLS-SRTP) receive path only.
//   PCAM3 TCP signaling (offerer) -> the phone answers with:
//     Opus mic   -> FFmpeg decode -> WASAPI render endpoint (CABLE / virtual mic)   [wasapi_sink]
//     H.264 cam  -> FFmpeg decode -> tshino/softcam DirectShow virtual camera        [video_sink, WITH_SOFTCAM]
//                                 \-> optional embedded GDI preview window            [--preview]
//
// The RTSP/SRT camera path and its adb-forward "USB mode" were retired once WebRTC video was
// validated on real hardware; the whole media stack is WebRTC now. All the work is in
// webrtc_receiver.cpp; this file is just argument parsing + lifecycle.
//
// Usage:
//   receiver.exe --sig-port <port> --sig-secret <secret> [--webrtc-video] [--preview]
//                [--audio-device <name-substr>] [--mic-gain <db>] [--eq <preset|type:f:q:db;...>]
//                [--ice-bind <local-ipv4>]   (bind ICE to the USB-tethering adapter)
//
// Build/run: see windows/README.md and docs/build-and-run.md.

#include <cstdio>
#include <cstdlib>
#include <string>
#include <atomic>
#include <csignal>

#include "webrtc_receiver.h"   // PCAM3 signaling + DTLS-SRTP Opus/H264 -> WASAPI + softcam

static std::atomic<bool> g_running{true};
static void on_sigint(int) { g_running = false; }

struct Options {
    bool        webrtcVideo = false;  // --webrtc-video: also receive an H.264 video track -> softcam
    bool        preview = false;      // --preview: embed/show a GDI preview of the decoded video
    int         sigPort = 0;          // --sig-port: PCAM3 TCP signaling port
    std::string sigSecret;            // --sig-secret: pairSecret gate
    std::string audioDevice;          // --audio-device: WASAPI render endpoint substring (e.g. "CABLE Input")
    float       micGainDb = 0.0f;     // --mic-gain: boost the (quiet) phone mic; soft-limited in the sink
    std::string eqPreset;             // --eq: voice EQ preset or a "type:freq:q:gain;..." band list
    std::string iceBind;              // --ice-bind: bind ICE to this local IPv4 (USB-tethering adapter)
};

static Options parse_args(int argc, char **argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--webrtc") { /* implied — the only transport now; accepted for compatibility */ }
        else if (a == "--webrtc-video") o.webrtcVideo = true;
        else if (a == "--preview") o.preview = true;
        else if (a == "--sig-port" && i + 1 < argc) o.sigPort = atoi(argv[++i]);
        else if (a == "--sig-secret" && i + 1 < argc) o.sigSecret = argv[++i];
        else if (a == "--audio-device" && i + 1 < argc) o.audioDevice = argv[++i];
        else if (a == "--mic-gain" && i + 1 < argc) o.micGainDb = (float)atof(argv[++i]);
        else if (a == "--eq" && i + 1 < argc) o.eqPreset = argv[++i];
        else if (a == "--ice-bind" && i + 1 < argc) o.iceBind = argv[++i];
        else if (a.rfind("--", 0) == 0) fprintf(stderr, "ignoring unknown option: %s\n", a.c_str());
    }
    return o;
}

int main(int argc, char **argv) {
    Options opt = parse_args(argc, argv);
    if (opt.sigPort <= 0 || opt.sigSecret.empty()) {
        fprintf(stderr,
            "usage: %s --sig-port <port> --sig-secret <secret> [--webrtc-video] [--preview] "
            "[--audio-device <name-substr>] [--mic-gain <db>] [--eq <preset|type:f:q:db;...>] "
            "[--ice-bind <local-ipv4>]\n", argv[0]);
        return 1;
    }
    signal(SIGINT, on_sigint);
    fprintf(stderr, "[phonecam] transport=webrtc (DTLS-SRTP + Opus%s)  audio=%s\n",
            opt.webrtcVideo ? " + H264" : "",
            opt.audioDevice.empty() ? "default-output" : opt.audioDevice.c_str());

    WebrtcRecvConfig cfg;
    cfg.sigPort     = opt.sigPort;
    cfg.sigSecret   = opt.sigSecret;
    cfg.audioDevice = opt.audioDevice;
    cfg.micGainDb   = opt.micGainDb;
    cfg.eqPreset    = opt.eqPreset;
    cfg.wantVideo   = opt.webrtcVideo;
    cfg.wantPreview = opt.preview;
    cfg.iceBind     = opt.iceBind;
    return run_webrtc_session(cfg, &g_running);
}
