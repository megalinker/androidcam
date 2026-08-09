# PhoneCam

Use an Android phone as a **webcam and/or low-latency microphone** on Windows 10/11. Camera and mic show up as a normal **`PhoneCam Camera`** and (via a virtual audio cable) a normal microphone, so any app — Zoom, Teams, Meet, Discord, OBS, Chrome — can pick them.

The phone streams over one of two transports, chosen automatically:

| Transport | When | Path |
|---|---|---|
| **USB** (preferred) | phone plugged in with USB debugging | scrcpy-style H.264 + PCM over an `adb forward` socket — no Wi-Fi, no QR, no tethering |
| **Wi-Fi / WebRTC** | no cable | Opus + H.264 over authenticated **DTLS-SRTP**, LAN-direct, QR-paired |

Both feed the **same** Windows decode → sink pipeline (FFmpeg → softcam / WASAPI). Everything is local: no cloud, no account, no STUN/TURN. The earlier RTSP and SRT transports were retired once WebRTC video passed the real-device latency gate.

## How it works

```
Android phone                                          Windows PC (receiver.exe)
                                                        ┌────────────────────────────────────────┐
Camera ─ Camera2 → MediaCodec H.264 ─┐                  │ FFmpeg H.264 decode → BGR → rotate/flip │→ softcam  → "PhoneCam Camera"
Mic    ─ AudioRecord → PCM ──────────┴─ USB: adb-fwd ──→│                                        │
                                        TCP socket      │ raw PCM ─────────────────────────────► │→ WASAPI   → CABLE Output (mic)
                                                        └────────────────────────────────────────┘

Camera ─ org.webrtc H.264 ─┐                            ┌────────────────────────────────────────┐
Mic    ─ org.webrtc Opus  ─┴─ Wi-Fi: DTLS-SRTP / UDP ──→│ libdatachannel → FFmpeg decode → sinks  │→ softcam / WASAPI
                              (PCAM3 TCP signaling)      └────────────────────────────────────────┘
```

The **phone's mode** (Cam+Mic / Camera / Mic) decides which tracks it sends; the receiver feeds whichever sink applies. The desktop app (`PhoneCam.exe`) is the launcher: it detects the phone, picks the transport, drives `adb`, shows the pairing QR when needed, and hosts a live preview.

### Phone battery on the PC

The desktop app's Status panel shows the phone's remaining battery and whether it is charging. It
rides the control channel each transport already has — the PCAM3 signaling socket over Wi-Fi, the
media socket's frame format over USB — so there is no extra connection and no polling: one ~90-byte
message a minute, plus an immediate update when the charger is plugged or unplugged. Older phone or
PC builds simply never negotiate it and keep working.

### Manual image controls (never automatic)

Mirror (L/R), flip (U/D), rotate (0/90/180/270), and front/back camera switch are all **user-driven buttons** in the desktop app, applied **live** while streaming (the app streams commands to `receiver.exe` over its stdin; the camera flip goes to the phone over `adb`). Nothing rotates or mirrors on its own — the raw sensor image is shown as-is until you change it.

## The two Windows sinks (why the mic is the hard part)

| Sink | Windows mechanism | Code signing |
|------|-------------------|--------------|
| **Virtual camera** | user-mode **DirectShow filter** (COM DLL, `regsvr32`) — [`tshino/softcam`](https://github.com/tshino/softcam) (MIT) | **none** |
| **Virtual microphone** | **kernel** audio endpoint — a pre-signed virtual cable ([VB-CABLE](https://vb-audio.com/Cable/)), or the optional driver in [`windows/driver/`](windows/driver) | required for a custom driver; VB-CABLE is already MS-signed |

Windows has **no user-mode way** to add a microphone — the audio engine only enumerates kernel capture endpoints. So the receiver *renders* decoded phone audio to a WASAPI output; pointing that at VB-CABLE's input makes it reappear as **CABLE Output**, a selectable mic. The camera uses the simpler user-mode softcam filter, which outputs a **fixed resolution** (chosen once from the first frame) so the virtual camera never changes size mid-call.

## Technology stack

| Piece | Tech |
|---|---|
| Phone app | Kotlin, Camera2, MediaCodec (H.264), AudioRecord (PCM), a foreground `Service`; [`stream-webrtc-android`](https://github.com/GetStream/webrtc-android) (`org.webrtc`) for the Wi-Fi path; [ZXing](https://github.com/journeyapps/zxing-android-embedded) for QR scan |
| PC receiver | C++ (MSVC); [libdatachannel](https://github.com/paullouisageneau/libdatachannel) (DTLS-SRTP + RTP, libjuice ICE, libsrtp/OpenSSL); FFmpeg 7.x (H.264/Opus **decode**, `swscale`, `swresample` — no `avformat`); WASAPI; DirectShow softcam |
| Desktop app | C# / WinForms (`PhoneCam.exe`); bundled `adb`; [QRCoder](https://github.com/codebude/QRCoder) for the pairing QR |
| Transports | WebRTC (DTLS-SRTP/Opus/H.264) over Wi-Fi; a custom framed TCP protocol over `adb forward` for USB |
| Packaging | vcpkg (deps), CMake (receiver), Roslyn `csc` (GUI), Inno Setup (installer), GitHub Actions (`v*` tag → release) |

## Layout

```
phonecam/
├── android/     # the phone app — build on Linux/Mac/Windows with Android Studio / Gradle
├── windows/     # the C++ receiver, the C# desktop app, packaging, and the optional audio driver
├── tools/       # battery/power measurement scripts (adb + Perfetto, no root)
└── docs/        # architecture + build/run + diagnostics
```

## Prior art we lean on

- [`stream-webrtc-android`](https://github.com/GetStream/webrtc-android) (BSD) — the prebuilt `org.webrtc` stack the phone uses for Opus/H.264 capture and DTLS-SRTP over Wi-Fi.
- [`libdatachannel`](https://github.com/paullouisageneau/libdatachannel) (MPL-2.0) — the PC WebRTC stack; keeps the receiver self-contained without a full `libwebrtc` build.
- [`tshino/softcam`](https://github.com/tshino/softcam) (MIT) — DirectShow virtual camera with a tiny frame-push API.
- [`scrcpy`](https://github.com/Genymobile/scrcpy) (Apache-2.0) — the reference for the USB path (MediaCodec → framed socket over `adb`).
- [`darusc/VCamdroid`](https://github.com/darusc/VCamdroid) (MIT) — early end-to-end reference: Android → Windows softcam.

See **[docs/architecture.md](docs/architecture.md)** for the design and the reasoning, and **[docs/build-and-run.md](docs/build-and-run.md)** for building and running each side.

Measuring battery, thermals and stream health — and capturing evidence when the video glitches — is
covered in **[docs/diagnostics.md](docs/diagnostics.md)** (opt-in; off by default, because recording
costs battery too). The scripts live in [`tools/`](tools).
