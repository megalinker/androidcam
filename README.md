# PhoneCam

Use an Android phone as a **max-quality webcam and/or microphone** for a Windows 10 PC, over Wi-Fi — **fully self-contained**: your Android app + your Windows app/driver. No OBS, no VB-CABLE, no VoiceMeeter, no external services.

The user picks **camera only**, **mic only**, or **both** — they show up in Windows as two independent, selectable devices (a virtual camera + a virtual microphone).

## How it works

```
ANDROID (build on Linux)                     WINDOWS 10 (build/test on Windows)
┌────────────────────────────┐               ┌─────────────────────────────────────┐
│ Camera2  → MediaCodec H.264 │   Wi-Fi/LAN   │ receiver.exe                        │
│ Mic      → AAC/Opus         │  ==========>  │  FFmpeg decode                      │
│ (one muxed RTSP session,    │   RTSP pull   │   ├─ video → softcam (DirectShow) ─►│  "PhoneCam Camera"
│  phone runs the RTSP server)│               │   └─ audio → virtual-mic driver ───►│  "PhoneCam Microphone"
└────────────────────────────┘               └─────────────────────────────────────┘
        the phone is the RTSP server; the PC connects to rtsp://<phone-ip>:8554/
```

Keeping audio and video in **one RTSP session** gives lip-sync for free (shared RTCP timing) — no manual A/V re-sync.

## The two Windows sinks (why the mic is the hard part)

| Sink | Windows mechanism | Code signing | Difficulty |
|------|-------------------|--------------|------------|
| **Virtual camera** | user-mode **DirectShow filter** (COM DLL, `regsvr32`) — [`tshino/softcam`](https://github.com/tshino/softcam) (MIT) | **none** | easy |
| **Virtual microphone** | **kernel-mode audio driver** (WDM/PortCls WaveRT) — fork [`VirtualDrivers/Virtual-Audio-Driver`](https://github.com/VirtualDrivers/Virtual-Audio-Driver) (MIT) or MS `SysVAD` | **required** (test-signing for personal use; EV cert to distribute) | hard |

Windows 10 has **no user-mode way** to add a microphone — the audio engine only enumerates kernel capture endpoints. That's why this repo is built in two phases.

## Roadmap

- **Phase 1 — Camera (this scaffold):** Android RTSP-server app streaming H.264 → Windows receiver decodes with FFmpeg → pushes frames into `softcam`. No driver, no signing. Works in Zoom/Teams/Discord/Chrome/OBS.
- **Phase 2 — Microphone:** add a kernel virtual-audio driver; the receiver decodes the audio track and feeds PCM into it. Wire the mic/cam/both toggle in the app.

## Layout

```
phonecam/
├── android/     # the phone app — build on Linux with Android Studio / Gradle
├── windows/     # the receiver + (Phase 2) the virtual-audio driver — build on Windows (MSVC + Windows SDK + WDK)
└── docs/        # architecture, decisions, references
```

## Prior art we lean on (all MIT/Apache — safe to fork)

- [`darusc/VCamdroid`](https://github.com/darusc/VCamdroid) (MIT) — closest end-to-end reference: Android → Windows softcam virtual camera.
- [`pedroSG94/RootEncoder`](https://github.com/pedroSG94/RootEncoder) (Apache-2.0) — Camera2 + MediaCodec + audio over RTSP/RTMP/SRT, incl. an on-device RTSP server.
- [`tshino/softcam`](https://github.com/tshino/softcam) (MIT) — DirectShow virtual camera with a tiny frame-push API.

See [docs/architecture.md](docs/architecture.md) for the full design and the research behind these choices.

## Status

🚧 Pre-first-build (written against verified current APIs, not yet compiled — treat first build on each side as bring-up).

**Android app** — RTSP-server service; **camera / mic / both** toggle; **quality presets** (720p→4K); **front/back** switch; **on-screen preview**; remembers last mode/quality; keeps screen on.
**Windows receiver** — RTSP → FFmpeg decode → softcam (video) + WASAPI (audio); **auto-reconnect**; `--preview` GDI window (live fps in the title bar); `--audio-device` / `--no-audio` / `--udp` / `--smooth` flags.
**Test harness** — `windows/scripts/test-source.*` serves a synthetic RTSP stream so you can validate the receiver with **no phone** and no drivers.
**Phase 2 (not coded, but tooled)** — the kernel virtual-mic loopback driver; `windows/driver/` has the full runbook + test-sign/install scripts.

See [docs/build-and-run.md](docs/build-and-run.md) for how to run each side, and the subfolder READMEs for the `TODO` markers.
