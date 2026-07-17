// USB receive path for receiver.exe (--usb). Connects to the phone's stream through an
// `adb forward`ed TCP port, reads the PhoneCam USB frame protocol, decodes H.264 with FFmpeg into
// the softcam VideoSink and plays raw PCM into the WasapiSink. No WebRTC, no tethering — a
// scrcpy-style low-latency pipe over the cable.
//
// Frame:  [1B type][8B ptsUs BE][4B len BE][payload]
//   'H' one JSON header (geometry, audio rate/channels)
//   'V' H.264 Annex-B (includes the codec-config SPS/PPS as the first 'V')
//   'A' interleaved PCM S16LE
#pragma once

#include <atomic>
#include <string>

struct UsbRecvConfig {
    int         usbPort = 0;        // local (adb-forwarded) TCP port to connect to on 127.0.0.1
    std::string audioDevice;       // WASAPI endpoint substring (e.g. "CABLE Input")
    float       micGainDb = 0.0f;  // mic boost (soft-limited in the sink)
    std::string eqPreset;          // voice EQ preset / band list
    bool        wantPreview = false;  // show/embed a GDI preview window of the decoded video
};

// Runs until *running becomes false. Retries the connection between phone sessions.
int run_usb_session(const UsbRecvConfig &cfg, std::atomic<bool> *running);
