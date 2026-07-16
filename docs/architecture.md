# Architecture & Design Decisions

## Goal

Turn an Android phone into a **max-quality webcam and/or microphone** for a **Windows 10/11** PC over **Wi-Fi**. The camera needs **no third-party software** on the PC; the microphone rides a virtual-audio endpoint (VB-CABLE, or the optional kernel driver under `windows/driver/`). The user chooses camera-only, mic-only, or both.

## Constraints that shaped the design

1. **Windows 10, not just 11.** The clean modern virtual-camera API (Media Foundation `MFCreateVirtualCamera`) is **Windows 11 only (build 22000+)**. Windows 11's built-in "phone as connected camera" (Phone Link) also does **not** exist on Windows 10. So on Win10 the virtual camera must be a **DirectShow filter**.
2. **Minimal PC-side setup.** The camera sink is built in (no OBS Virtual Camera dependency). The mic needs a kernel capture endpoint, which Windows has no user-mode way to add — so it uses a pre-signed virtual-audio cable (VB-CABLE) or the optional bundled driver.
3. **Wi-Fi / LAN transport.** Media is LAN-direct (host ICE candidates only, no STUN/TURN). USB tethering is a possible future path; there is no USB UVC-gadget mode. Network jitter/bandwidth is the quality ceiling.
4. **Mic + cam as a toggle.** Windows treats camera and microphone as unrelated device classes, so these are **two independent virtual devices**; the phone's mode decides which media tracks it sends and the receiver feeds whichever sink applies.

## The camera/mic asymmetry (the key fact)

| | Virtual camera | Virtual microphone |
|---|---|---|
| Windows mechanism | user-mode DirectShow filter (COM DLL) | **kernel-mode** audio driver (WDM/PortCls WaveRT) |
| Install | `regsvr32 softcam.dll` (x86 **and** x64) | driver install (INF), or a pre-signed cable (VB-CABLE) |
| Code signing | **none** | **required** for a custom driver — VB-CABLE sidesteps it (already MS-signed) |
| Effort | low; reuse `softcam` (MIT) | high for a custom driver; VB-CABLE is the practical default |
| Visible in | DirectShow apps: Zoom, Teams, Discord, Webex, OBS, Chrome/Edge | all apps (kernel endpoint) |
| **Not** visible in | built-in Windows Camera app, some MF-only UWP/Store apps | — |

→ The camera shipped first (no signing); the mic followed, resolved by routing through VB-CABLE.

## Transport: WebRTC (DTLS-SRTP), FFmpeg codecs on the PC

- **WebRTC, LAN-direct.** The phone (org.webrtc) and the PC (libdatachannel) exchange SDP over a small TCP signaling channel (PCAM3) gated by a `pairSecret` from the QR, then media flows over UDP with **host ICE candidates only** — no STUN/TURN, no cloud. The DTLS fingerprints in the SDP are the pinned identity.
- **PC offers, phone answers.** The PC is the offerer/listener (recvonly Opus + H.264); the phone answers sendonly. So the PC needs no knowledge of the phone's address up front — the phone dials out after scanning the QR, which also traverses most home-router topologies cleanly.
- **A/V sync for free.** Opus and H.264 are separate SRTP tracks but share the WebRTC clock and RTCP sender reports, so lip-sync is handled by the stack rather than by manual re-timing.
- The receiver decodes with **FFmpeg** (`avcodec_send_packet`/`avcodec_receive_frame`), `sws_scale` → BGR for softcam, `swresample` → 16-bit PCM for the audio endpoint. libdatachannel handles DTLS-SRTP + RTP; FFmpeg is codecs only (no avformat).
- **History:** the MVP used an on-device **RTSP** server (later an encrypted **SRT** variant) with FFmpeg's `avformat` demuxer on the PC — simplest to stand up, but higher latency and a permanent backlog on the camera path. Both were retired once WebRTC video passed the real-device latency gate.

### Encoder targets (max quality vs. a real Wi-Fi link)

- **Sweet spot:** 1080p @ 30–60 fps, H.264, ~6–8 Mbps; the phone captures up to the selected Quality preset and WebRTC adapts bitrate / sheds resolution under congestion.
- **4K30** only if the link + PC decoder genuinely keep up.
- Prefer **5 GHz Wi-Fi**. H.264 (not HEVC) is the safe default for decoder compatibility, and matches the PC's offered codec.

## Build split: Linux vs Windows

- **On Linux (the whole phone app):** Android Studio, Gradle, SDK/NDK, ADB, emulator, APK signing — all native. Capture/encode/WebRTC is cross-platform and testable here.
- **On Windows (a Win10/11 VM or spare box):** the receiver (MSVC + Windows SDK + FFmpeg + libdatachannel via vcpkg) and the optional audio driver (**WDK**). You cannot `regsvr32`, enumerate in Zoom, sign, or debug these from Linux. The DirectShow camera route needs **no signing**; only the kernel mic driver does.

## Decisions log

- **WebRTC transport** — after RTSP/SRT MVPs, WebRTC (DTLS-SRTP) won on latency and native A/V sync. libdatachannel keeps the PC receiver self-contained (no full `libwebrtc` build).
- **softcam (MIT)** as the camera sink — no signing, proven, tiny push API.
- **Phone dials the PC** — the QR carries the PC's signaling endpoint + secret; the phone connects out, so the PC needs no inbound discovery and the phone can retry a saved endpoint.
- **Audio via WASAPI render + virtual cable** — the receiver renders decoded audio to an ordinary output endpoint (`wasapi_sink.cpp`); pointing it at VB-CABLE's input makes it reappear as a capture device with no custom app↔driver IPC. Bonus: pointing it at real speakers gives a fully testable audio path.

## Open questions / risks

- DirectShow camera invisible to the Windows Camera app and some UWP apps (acceptable for conferencing).
- A full-tunnel VPN or AP-isolation can block the LAN-direct path (no relay fallback by design).
- Custom kernel driver signing for any non-personal distribution — VB-CABLE avoids this for most users.
- USB tethering as a future low-jitter transport (RNDIS gives ICE an interface to use).
