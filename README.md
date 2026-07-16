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

See [docs/build-and-run.md](docs/build-and-run.md) for how to run each side, and the subfolder READMEs for the `TODO` markers.
