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
#include <thread>
#include <iostream>
#include <sstream>

#include "webrtc_receiver.h"   // PCAM3 signaling + DTLS-SRTP Opus/H264 -> WASAPI + softcam
#include "usb_receiver.h"      // scrcpy-style H.264/PCM over an adb-forwarded socket -> WASAPI + softcam
#include "video_sink.h"        // VideoSetRotate/FlipH/FlipV — manual, live output transform
#include "stats.h"             // flag-gated (PHONECAM_STATS) latency/queue instrumentation
#include "marker.h"            // "Mark video problem" — operator-raised correlation marker

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
    bool        usb = false;          // --usb: receive over an adb-forwarded socket instead of WebRTC
    int         usbPort = 0;          // --usb-port: local (adb-forwarded) TCP port to connect to
    bool        flipH = false;        // --flip-h/--mirror: mirror the image left/right (MANUAL, never auto)
    bool        flipV = false;        // --flip-v: flip the image top/bottom
    int         rotate = 0;           // --rotate: 0/90/180/270 CW
};

// Live flip/rotate control: the GUI writes one command per line to our stdin as the user clicks the
// Flip/Rotate buttons. Never automatic. Commands: "fliph 0|1", "flipv 0|1", "rotate 0|90|180|270",
// "preview 0|1", and "mark <note>" (the operator saw a visual artifact right now).
static void stdin_control_thread() {
    std::string line;
    while (std::getline(std::cin, line)) {
        std::istringstream ss(line);
        std::string cmd; ss >> cmd;
        if (cmd == "fliph") { int v = 0; ss >> v; VideoSetFlipH(v != 0); }
        else if (cmd == "flipv") { int v = 0; ss >> v; VideoSetFlipV(v != 0); }
        else if (cmd == "rotate") { int v = 0; ss >> v; VideoSetRotate(v); }
        else if (cmd == "preview") { int v = 1; ss >> v; VideoSetPreviewVisible(v != 0); }   // F-34: hidden => skip idle video work
        else if (cmd == "mark") {
            std::string note;
            std::getline(ss, note);
            if (!note.empty() && note[0] == ' ') note.erase(0, 1);
            marker::raise(note.empty() ? "video artifact" : note);
        }
    }
}

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
        else if (a == "--usb") o.usb = true;
        else if (a == "--usb-port" && i + 1 < argc) o.usbPort = atoi(argv[++i]);
        else if (a == "--flip-h" || a == "--mirror") o.flipH = true;
        else if (a == "--flip-v") o.flipV = true;
        else if (a == "--rotate" && i + 1 < argc) o.rotate = atoi(argv[++i]);
        else if (a.rfind("--", 0) == 0) fprintf(stderr, "ignoring unknown option: %s\n", a.c_str());
    }
    return o;
}

int main(int argc, char **argv) {
    Options opt = parse_args(argc, argv);
    signal(SIGINT, on_sigint);
    stats::banner();   // one line if PHONECAM_STATS is set; instrumentation is otherwise a no-op

    // Manual output transform (applies to both transports' video). Initial state from flags; the GUI
    // updates it live over stdin as the user toggles Flip/Rotate. NEVER changed automatically.
    VideoSetRotate(opt.rotate);
    VideoSetFlipH(opt.flipH);
    VideoSetFlipV(opt.flipV);
    std::thread(stdin_control_thread).detach();

    // --usb: the scrcpy-style path over an adb-forwarded socket (no WebRTC).
    if (opt.usb) {
        if (opt.usbPort <= 0) {
            fprintf(stderr, "usage: %s --usb --usb-port <local-port> [--preview] "
                    "[--audio-device <name-substr>] [--mic-gain <db>] [--eq <...>]\n", argv[0]);
            return 1;
        }
        fprintf(stderr, "[phonecam] transport=usb (H264 + PCM over adb :%d)  audio=%s\n",
                opt.usbPort, opt.audioDevice.empty() ? "default-output" : opt.audioDevice.c_str());
        UsbRecvConfig ucfg;
        ucfg.usbPort     = opt.usbPort;
        ucfg.audioDevice = opt.audioDevice;
        ucfg.micGainDb   = opt.micGainDb;
        ucfg.eqPreset    = opt.eqPreset;
        ucfg.wantPreview = opt.preview;
        return run_usb_session(ucfg, &g_running);
    }

    if (opt.sigPort <= 0 || opt.sigSecret.empty()) {
        fprintf(stderr,
            "usage: %s --sig-port <port> --sig-secret <secret> [--webrtc-video] [--preview] "
            "[--audio-device <name-substr>] [--mic-gain <db>] [--eq <preset|type:f:q:db;...>] "
            "[--ice-bind <local-ipv4>]\n"
            "   or: %s --usb --usb-port <local-port> [--preview] [--audio-device <name-substr>] ...\n",
            argv[0], argv[0]);
        return 1;
    }
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
