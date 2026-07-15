# PhoneCam

Use an Android phone as a webcam or low-latency microphone on Windows 10/11 over the local network.

The desktop app defaults to **WebRTC mic-only** (Opus over DTLS-SRTP). **RTSP camera** remains as the compatibility mode. SRT was retired after the real-device latency gate for v0.5.0.

## How it works

```
Android phone                              Windows PC
Mic → Opus → WebRTC/DTLS-SRTP ──────────→ WASAPI → CABLE Output
Camera → H.264/AAC → RTSP ───────────────→ FFmpeg → PhoneCam Camera
```

Pairing is local and QR-bootstrapped. No cloud signaling, STUN, or TURN service is used.

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
- [`pedroSG94/RootEncoder`](https://github.com/pedroSG94/RootEncoder) (Apache-2.0) — Camera2 + MediaCodec and the on-device RTSP server used by camera mode.
- [`tshino/softcam`](https://github.com/tshino/softcam) (MIT) — DirectShow virtual camera with a tiny frame-push API.

See [docs/architecture.md](docs/architecture.md) for the full design and the research behind these choices.

## Status

**v0.5.0 validated on a Pixel 10 Pro** (2026-07-15): WebRTC pairing, Opus microphone capture, DTLS-SRTP media, CABLE rendering, live status, and the mic meter all passed on real hardware. Acoustic latency measured `166.3 ± 26.1 ms`, 53.5% below the retired SRT baseline.

**Android app** — WebRTC mic sender plus RTSP camera server; QR pairing; one-tap WebRTC reconnect that waits for the PC app; camera/mic/both controls for RTSP; quality presets and front/back switching.
**Windows receiver** — WebRTC → Opus → WASAPI for the default mic mode; RTSP → FFmpeg → softcam for camera compatibility; auto-reconnect and mic metering.
**Test harness** — `windows/scripts/test-source.*` serves a synthetic RTSP stream so you can validate the receiver with **no phone** and no drivers.
**Microphone endpoint** — the packaged GUI targets VB-CABLE; the repository also retains the experimental kernel virtual-mic driver under `windows/driver/`.

See [docs/build-and-run.md](docs/build-and-run.md) for how to run each side, and the subfolder READMEs for the `TODO` markers.
