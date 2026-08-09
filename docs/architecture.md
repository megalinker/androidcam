# Architecture & Design Decisions

## Goal

Turn an Android phone into a **webcam and/or microphone** for a **Windows 10/11** PC, at the lowest practical latency, with **minimal PC-side setup**. The camera needs no third-party software; the microphone rides a virtual-audio endpoint (VB-CABLE, or the optional kernel driver in `windows/driver/`). The user picks camera-only, mic-only, or both.

## Constraints that shaped the design

1. **Windows 10, not just 11.** The modern virtual-camera API (Media Foundation `MFCreateVirtualCamera`) is **Windows 11 only (22000+)**, and Win11's "phone as connected camera" (Phone Link) doesn't exist on Win10. So the virtual camera must be a **DirectShow filter**.
2. **No mandatory extra apps for video.** The camera sink is built in (no OBS Virtual Camera dependency). The mic needs a *kernel* capture endpoint — Windows has no user-mode way to add one — so it uses a pre-signed virtual cable (VB-CABLE) or the optional bundled driver.
3. **Two links, same pipeline.** USB (a cable) and Wi-Fi (WebRTC) both terminate in the *same* FFmpeg-decode → softcam/WASAPI code on the PC. Only the transport differs.
4. **Cam + mic as a per-mode toggle.** Windows treats camera and microphone as unrelated device classes, so these are two independent virtual devices; the phone's mode decides which media it sends and the receiver feeds whichever sink applies.

## The camera/mic asymmetry (the key fact)

| | Virtual camera | Virtual microphone |
|---|---|---|
| Windows mechanism | user-mode DirectShow filter (COM DLL) | **kernel-mode** audio endpoint |
| Install | `regsvr32 softcam.dll` (x86 **and** x64) | a pre-signed cable (VB-CABLE), or a driver INF |
| Code signing | **none** | required for a custom driver — VB-CABLE sidesteps it |
| Visible in | DirectShow apps: Zoom, Teams, Discord, OBS, Chrome/Edge | all apps (kernel endpoint) |
| **Not** visible in | built-in Windows *Camera* app, some MF-only UWP apps | — |

→ The camera shipped first (no signing); the mic followed, resolved by routing decoded audio through VB-CABLE.

## Transport 1 — Wi-Fi: WebRTC (DTLS-SRTP)

- **Stacks.** Phone = **`org.webrtc`** (the maintained `stream-webrtc-android` artifact). PC = **libdatachannel** (DTLS-SRTP + RTP via libjuice/libsrtp), *not* a full `libwebrtc` build — that keeps the receiver small and self-contained.
- **Signaling = PCAM3.** The desktop app shows a QR `PCAM3:<pc-ip>:<port>:<secret>` (TCP **8891**). The phone dials that endpoint and the two exchange SDP over a tiny framed TCP channel: `[1 byte type][4-byte big-endian length][payload]`, types `S` = pairSecret, `O` = offer, `A` = answer. **Non-trickle** — ICE candidates are inline in the SDP.
- **PC offers, phone answers.** The PC is the offerer/listener (**recvonly** Opus + H.264); the phone answers **sendonly**. A sendonly *offer* to an unprepared answerer is rejected, which is why the roles are this way round. The phone dialing out also traverses most home routers cleanly, and lets it retry a saved endpoint.
- **LAN-direct, pinned identity.** **Host ICE candidates only** — no STUN, no TURN, no cloud. The `pairSecret` gates the signaling channel; the DTLS fingerprints in the SDP are the pinned identity.
- **A/V sync for free.** Opus and H.264 are separate SRTP tracks sharing the WebRTC clock + RTCP sender reports, so lip-sync is handled by the stack.
- **Codecs stay on FFmpeg.** libdatachannel delivers decrypted RTP; the receiver decodes with `avcodec_send_packet`/`avcodec_receive_frame`, `sws_scale` → BGR for softcam, `swresample` → PCM for the audio endpoint. No `avformat`.
- **The signaling socket doubles as the control channel.** It is open for the whole session anyway (the phone blocks on it to notice the PC pressing Stop), so small status/control messages ride it rather than justifying a second connection. After the answer the PC sends `V` (hello, advertising what it understands); the phone then pushes `B` (device status: battery %, charging, temperature) about once a minute, and the PC may send `M` (the operator pressed "Mark video problem"). Unknown types are skipped by length in both directions, so either side can add messages — and an older peer, which never sends or reads them, keeps working unchanged.

## Packet loss and the visible consequence

libdatachannel's `H264RtpDepacketizer` emits a frame built from whatever packets arrived, **without
signalling that any were missing**, and its `RtcpReceivingSession` never generates NACKs even though
the offer advertises them. A lost packet therefore reaches the decoder as a silently-truncated frame;
the concealed macroblocks then persist through subsequent P-frames until something moves through that
region or a keyframe arrives.

The receiver closes that gap where it is cheapest: `RtpLossMonitor` (`rtp_seq.h`/`rtp_loss.h`) sits at
the head of the incoming media chain and watches raw RTP sequence numbers — a subtract, a compare and
one relaxed atomic store per packet. If packets went missing since the previous frame, or FFmpeg flags
the decoded frame as damaged/concealed, the receiver requests one keyframe, rate-limited to 1 s. In a
clean session it never fires. Reordering and duplicates are distinguished from loss so ordinary jitter
costs nothing. No NACK, no FEC, no shortened GOP — recovery stays event-driven rather than becoming a
standing bitrate and battery tax. See `docs/perf-audit/battery-investigation.md`.

## Transport 2 — USB: a scrcpy-style pipe over adb

The cable is the steadiest link (no Wi-Fi jitter, congestion, or AP-isolation), so it's preferred when available. But it needs a *different* transport, for a hard reason:

> **WebRTC media is UDP; `adb forward` is TCP-only; and libdatachannel's ICE (libjuice) is UDP-only.** So the Wi-Fi WebRTC media simply can't ride an adb tunnel. Modeled on **[scrcpy](https://github.com/Genymobile/scrcpy)**, the USB path is therefore its own thing: MediaCodec-encoded media over a raw framed TCP socket.

- **Phone side** (`UsbStreamer.kt`): Camera2 feeds a **MediaCodec H.264** encoder through its input Surface (GPU path, `KEY_LOW_LATENCY`, no B-frames, 1 s GOP); the mic is captured as **raw PCM** (S16LE 48 kHz — USB has bandwidth to spare, so no audio codec/latency). The phone binds a TCP `ServerSocket` on `127.0.0.1:27183` and the PC connects through the forward. The MediaCodec callback runs on a **background thread** (writing to a socket on the main thread throws `NetworkOnMainThreadException`).
- **Frame protocol:** `[1 byte type][8-byte ptsUs big-endian][4-byte length big-endian][payload]`. `H` = one JSON header (geometry, audio rate/channels), `V` = H.264 Annex-B (the codec-config SPS/PPS is the first `V`), `A` = interleaved PCM, `S` = device status (battery). The reverse direction — previously read only to detect EOF — carries `[1 byte type][4-byte length][payload]` control frames: `K` = emit a keyframe now (sent when the receiver's video queue overflows, which is the only way the cable path can corrupt itself), `M` = the operator marked a video problem. Unknown types are skipped by length, so old and new builds interoperate in both directions.
- **PC side** (`usb_receiver.cpp`, `receiver --usb --usb-port <n>`): connects to the adb-forwarded port; a receive thread does I/O only and dispatches `V` to a **decode thread** and `A` to an **audio thread**, so neither can starve the socket reads. Same FFmpeg → softcam / WASAPI sinks as WebRTC.
- **Orchestration** (desktop app): on Start it detects an authorized adb device, runs `adb forward tcp:27183 tcp:27183`, `pm grant`s camera/mic, `am start`s the app with `ACTION_USB`, and launches `receiver --usb`. **No QR, no tethering** — plug in and press Start.

## Transport 3 — USB tethering (a WebRTC-over-cable fallback)

If the user prefers not to enable USB debugging, they can turn on **USB tethering**. Android then exposes an RNDIS/NCM interface (the PC gets a `192.168.42.x` address), which *is* a UDP-capable network over the cable. The desktop app detects that adapter, encodes its IP in the QR, and binds the receiver's ICE to it (`--ice-bind`), so the normal WebRTC path runs over USB instead of Wi-Fi. Enabling tethering stays a manual OS toggle (Android has no reliable non-root way to flip it), but detection + routing are automatic. On the phone, `PeerConnectionFactory.Options.disableNetworkMonitor` makes libwebrtc enumerate interfaces natively so the tethering interface is offered as an ICE candidate.

## Transport selection (desktop app, on Start)

1. **Authorized adb device present →** USB (scrcpy-style). Best latency, no scan, no tethering.
2. **A `192.168.42.x` (USB-tethering) adapter present →** WebRTC over the cable (ICE bound to it).
3. **Otherwise →** Wi-Fi WebRTC (show the QR).

## The virtual camera is a fixed resolution

WebRTC ramps resolution up from a low start (~360p) toward the selected quality. The receiver used to recreate the softcam camera on every resolution change — but recreating a DirectShow filter while a conferencing app reads it **crashes**, and conferencing apps also dislike the resolution changing mid-call. So `VideoSink` now picks an output size **once** from the first frame's aspect (short side → 720) and scales every frame to it: softcam is created once, and only the swscale context rebuilds on a source-size change.

## Orientation is manual, never automatic

The phone streams the sensor-native image. All correction — mirror, flip, rotate — is a user button in the desktop app, applied live (over `receiver.exe`'s stdin) and persisted; the front/back camera switch goes to the phone over `adb` (an exported `ControlReceiver`). The phone's `MainActivity` no longer forces `screenOrientation` and declares `configChanges`, so physically rotating the phone neither rotates the app against the system setting nor recreates the Activity mid-stream.

## Build split: Linux vs Windows

- **Phone app:** builds anywhere with Android Studio / Gradle (JDK 17). Capture/encode is standard Android.
- **PC side:** the receiver (MSVC + Windows SDK + FFmpeg + libdatachannel via vcpkg) and the C# desktop app must build/run on **Windows** — you cannot `regsvr32`, enumerate in Zoom, or drive a DirectShow filter from Linux. The optional kernel mic driver needs the WDK.

## Decisions log

- **WebRTC for Wi-Fi** — after RTSP/SRT MVPs, WebRTC won on latency and native A/V sync; libdatachannel keeps the PC receiver self-contained.
- **A separate scrcpy-style path for USB** — WebRTC-over-adb is impossible (UDP media, TCP tunnel, UDP-only PC ICE), so USB is MediaCodec/PCM over a framed TCP socket into the same sinks.
- **softcam (MIT)** as the camera sink — no signing, tiny push API; output pinned to one resolution for stability.
- **Audio via WASAPI render + VB-CABLE** — the receiver renders decoded audio to an output endpoint; VB-CABLE's input reappears as a capture device, so there's no custom app↔driver IPC. Pointing it at real speakers gives a fully testable audio path.
- **Manual-only orientation** — the app never rotates/mirrors on its own.

## Open questions / risks

- DirectShow camera invisible to the built-in Windows *Camera* app and some UWP apps (fine for conferencing).
- A full-tunnel VPN or AP-isolation blocks the Wi-Fi LAN-direct path (no relay fallback, by design) — USB sidesteps it.
- Fixed 720p softcam caps output for users who select 1080p+ (webcam-standard, but plumbing the exact target through is a possible follow-up).
- Custom kernel driver signing for non-personal distribution — VB-CABLE avoids this for most users.
