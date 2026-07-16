# PhoneCam

Use an Android phone as a webcam or low-latency microphone on Windows 10/11 over the local network.

The desktop app defaults to **WebRTC** (Opus over DTLS-SRTP), low-latency and authenticated. v0.5.1 adds **H.264 camera over WebRTC**, so cam+mic runs on the same low-latency path; **RTSP camera** stays as a compatibility fallback. SRT was retired after the real-device latency gate for v0.5.0.

## How it works

```
Android phone                                    Windows PC
Mic    → Opus  ─┐                               ┌→ FFmpeg → WASAPI  → CABLE Output    (mic)
Camera → H.264 ─┴─ WebRTC / DTLS-SRTP ─────────→┼→ FFmpeg → softcam → PhoneCam Camera  (low-latency)
Camera → H.264 / AAC → RTSP ───────────────────→ FFmpeg → softcam → PhoneCam Camera   (compatibility)
```

The phone's mode (mic / camera / both) decides which tracks it sends; the low-latency WebRTC path carries whatever it offers. Pairing is local and QR-bootstrapped — no cloud signaling, STUN, or TURN service is used.

## The two Windows sinks (why the mic is the hard part)

| Sink | Windows mechanism | Code signing | Difficulty |
|------|-------------------|--------------|------------|
| **Virtual camera** | user-mode **DirectShow filter** (COM DLL, `regsvr32`) — [`tshino/softcam`](https://github.com/tshino/softcam) (MIT) | **none** | easy |
| **Virtual microphone** | **kernel-mode audio driver** (WDM/PortCls WaveRT) — fork [`VirtualDrivers/Virtual-Audio-Driver`](https://github.com/VirtualDrivers/Virtual-Audio-Driver) (MIT) or MS `SysVAD` | **required** (test-signing for personal use; EV cert to distribute) | hard |

Windows 10 has **no user-mode way** to add a microphone — the audio engine only enumerates kernel capture endpoints. That's why the mic rides a virtual-audio endpoint (VB-CABLE, or the experimental kernel driver under `windows/driver/`) while the camera uses the simpler user-mode softcam filter.

## Layout

```
phonecam/
├── android/     # the phone app — build on Linux with Android Studio / Gradle
├── windows/     # the receiver + the experimental virtual-audio driver — build on Windows (MSVC + Windows SDK + WDK)
└── docs/        # architecture, decisions, references
```

## Prior art we lean on (all MIT/Apache — safe to fork)

- [`darusc/VCamdroid`](https://github.com/darusc/VCamdroid) (MIT) — closest end-to-end reference: Android → Windows softcam virtual camera.
- [`pedroSG94/RootEncoder`](https://github.com/pedroSG94/RootEncoder) (Apache-2.0) — Camera2 + MediaCodec and the on-device RTSP server used by camera mode.
- [`tshino/softcam`](https://github.com/tshino/softcam) (MIT) — DirectShow virtual camera with a tiny frame-push API.

See [docs/architecture.md](docs/architecture.md) for the full design and the research behind these choices.

## Status

**v0.5.0 validated on a Pixel 10 Pro** (2026-07-15): WebRTC pairing, Opus microphone capture, DTLS-SRTP media, CABLE rendering, live status, and the mic meter all passed on real hardware. Acoustic latency measured `166.3 ± 26.1 ms`, 53.5% below the retired SRT baseline.

**v0.5.1 (in progress)** — H.264 **camera over WebRTC** → softcam, so cam+mic shares the low-latency path, with the decoded feed embedded in the desktop preview. The PC receive path is verified against a test sender (H.264 + Opus decoded concurrently); the first real phone→PC video still needs on-device validation of the libdatachannel↔libwebrtc H.264 profile handshake. Also fixes saved-device reconnect — the PC now *listens* for the phone's WebRTC reconnect instead of dialing the retired RTSP URL, and auto-listen defaults on.

**Android app** — WebRTC sender for mic (Opus) and camera (hardware H.264); RTSP camera server as the compatibility fallback; QR pairing; one-tap WebRTC reconnect that waits for the PC app; camera/mic/both controls; quality presets and front/back switching.
**Windows receiver** — WebRTC → Opus → WASAPI (mic) and WebRTC → H.264 → softcam (camera) on the low-latency path; RTSP → FFmpeg → softcam for compatibility; embedded preview, auto-reconnect, and mic metering.
**Test harness** — `windows/scripts/test-source.*` serves a synthetic RTSP stream so you can validate the receiver with **no phone** and no drivers.
**Microphone endpoint** — the packaged GUI targets VB-CABLE; the repository also retains the experimental kernel virtual-mic driver under `windows/driver/`.

See [docs/build-and-run.md](docs/build-and-run.md) for how to run each side, and the subfolder READMEs for the `TODO` markers.
