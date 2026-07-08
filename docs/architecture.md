# Architecture & Design Decisions

## Goal

Turn an Android phone into a **max-quality webcam and/or microphone** for a **Windows 10** PC over **Wi-Fi**, with **no third-party software** on the PC (no OBS / VB-CABLE / VoiceMeeter). The user chooses camera-only, mic-only, or both.

## Constraints that shaped the design

1. **Windows 10, not 11.** The clean modern virtual-camera API (Media Foundation `MFCreateVirtualCamera`) is **Windows 11 only (build 22000+)**. Windows 11's built-in "phone as connected camera" (Phone Link) also does **not** exist on Windows 10. So on Win10 the virtual camera must be a **DirectShow filter**.
2. **No external apps.** Rules out the easy paths (OBS Virtual Camera as the video sink, VB-CABLE as the audio sink). We build both sinks ourselves.
3. **Wi-Fi transport.** No USB/ADB tunnel, no USB UVC-gadget mode. Network jitter/bandwidth is the quality ceiling.
4. **Mic + cam as a toggle.** Windows treats camera and microphone as unrelated device classes, so these are **two independent virtual devices**; the toggle just decides which we register/feed and which media tracks the app negotiates.

## The camera/mic asymmetry (the key fact)

| | Virtual camera | Virtual microphone |
|---|---|---|
| Windows mechanism | user-mode DirectShow filter (COM DLL) | **kernel-mode** audio driver (WDM/PortCls WaveRT) |
| Install | `regsvr32 softcam.dll` (x86 **and** x64) | driver install (INF) |
| Code signing | **none** | **required** — test-signing for personal use, EV cert + MS attestation to distribute |
| Effort | low; reuse `softcam` (MIT) | high; fork `Virtual-Audio-Driver` (MIT) / `SysVAD` |
| Visible in | DirectShow apps: Zoom, Teams, Discord, Webex, OBS, Chrome/Edge | all apps (kernel endpoint) |
| **Not** visible in | built-in Windows Camera app, some MF-only UWP/Store apps | — |

→ **Ship the camera first (Phase 1), add the mic second (Phase 2).**

## Transport: RTSP (phone = server), FFmpeg on the PC

- The **phone runs an RTSP server** (`rtsp://<phone-ip>:8554/`); the Windows receiver **pulls**. This avoids any PC-side inbound config and makes the phone the source of truth.
- **One RTSP session carries both audio and video** → free lip-sync via shared RTCP timing. Never split A/V across two transports; that forces manual re-sync and they drift.
- The receiver decodes with **FFmpeg** (`avcodec_send_packet`/`avcodec_receive_frame`), `sws_scale` → BGR for softcam, `swresample` → 16-bit PCM for the mic driver.
- **Why not WebRTC?** Lower latency and native A/V sync, but embedding `libwebrtc` in a from-scratch Windows receiver is a large lift. RTSP + FFmpeg is the pragmatic self-contained baseline. WebRTC is a possible later upgrade for latency-critical use.

### Encoder targets (max quality vs. a real Wi-Fi link)

- **Sweet spot:** 1080p @ 30–60 fps, H.264 CBR ~6–8 Mbps, `KEY_LOW_LATENCY`, no B-frames, 1 s keyframe interval.
- **4K30** @ 15–25 Mbps only if the link + PC decoder genuinely keep up.
- Prefer **5 GHz Wi-Fi**; expect ~100–200 ms latency. H.264 (not HEVC) is the safe default for decoder compatibility.

## Build split: Linux vs Windows

- **On Linux (the whole phone app):** Android Studio, Gradle, SDK/NDK, ADB, emulator, APK signing — all native. Capture/encode/RTSP is cross-platform and testable here.
- **On Windows (a Win10 VM or spare box):** the receiver (MSVC + Windows SDK + FFmpeg) and, in Phase 2, the audio driver (**WDK**). You cannot `regsvr32`, enumerate in Zoom, sign, or debug these from Linux. The DirectShow camera route needs **no signing**; only the kernel mic driver does.

## Decisions log

- **RTSP over WebRTC** for the MVP — self-contained, minimal PC-side deps. Revisit if latency matters.
- **softcam (MIT)** as the camera sink — no signing, proven, tiny push API.
- **Phone as RTSP server** — simplest Wi-Fi topology, PC just needs the phone's IP.
- **Audio via WASAPI render + loopback driver** — the receiver renders decoded audio to an ordinary output endpoint (`wasapi_sink.cpp`); the Phase-2 driver is then just a plain **loopback** (render→capture), so there is **no custom app↔driver IPC** to build. Bonus: pointing it at real speakers gives a fully testable audio path before the driver exists.
- **Two phases** — decouple the no-signing camera win from the driver-signing mic work.

## Open questions / risks

- DirectShow camera invisible to the Windows Camera app and some UWP apps (acceptable for conferencing).
- Wi-Fi jitter → need a small jitter buffer + adaptive bitrate.
- Kernel driver signing for any non-personal distribution.
- Phase 2 A/V sync when the audio is pulled from the same RTSP session but rendered through a separate device clock — match 48 kHz/16-bit and watch for drift.
