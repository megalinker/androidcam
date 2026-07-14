# PhoneCam → WebRTC media migration (design + plan)

## Goal

Replace the current media transports (RTSP/AAC and SRT/AAC) with a **WebRTC media
stack** — DTLS‑SRTP over RTP/UDP, Opus audio, hardware‑H.264 video, adaptive
jitter buffers — paired over the LAN with **QR‑pinned identities** (no signaling
server, no STUN/TURN). This is the standardized, battle‑tested version of the
"Oboe → short‑frame codec → RTP/SRTP → tiny buffer" design.

### Why (what it improves, honestly)

| Front | Today (AAC/SRT) | After (WebRTC) | Extent |
|---|---|---|---|
| Latency | ~150–250 ms added (AAC 21 ms framing + 120 ms SRT buffer + fat WASAPI buffer) | ~30–60 ms | **Big** |
| Security | SRT‑AES: encrypts, but not certificate‑authenticated | DTLS‑SRTP: authenticated encryption + QR‑pinned identity + ephemeral keys + replay protection | **Big** |
| Battery (mic‑only) | Runs a *dummy 16×16 video encoder* just to satisfy RootEncoder's server | Pure audio track, no video encoder; Opus ≪ AAC/PCM data | **Better** |
| Battery (video) | H.264 + AAC + SRT | H.264 + Opus + SRTP; sensor/encoder dominate either way; adaptive bitrate can help on bad Wi‑Fi | **~Neutral** |

The only thing WebRTC can't beat: *absolute‑floor* latency AND lower battery at
once (physics — tiny buffers ⇒ frequent wakeups). We are not chasing that floor;
the balanced WebRTC profile (10 ms Opus, ~10–20 ms buffer, normal Wi‑Fi
power‑save) is the target.

## Architecture

```
Phone (WebRTC Android SDK, org.webrtc)                 PC (C++ + libdatachannel)
  mic  → Opus  ─┐                                        ┌─ Opus → FFmpeg dec → WASAPI (CABLE)
  cam  → H.264 ─┼─ RTP/SRTP (DTLS) over UDP, LAN direct ─┼─ H.264 → FFmpeg dec → softcam
               ─┘                                        └─
        SDP offer/answer + DTLS fingerprints over the QR‑bootstrapped TCP channel
```

### Library choices (deliberate, not shortcuts)

- **Phone: WebRTC Android SDK** (`org.webrtc`, a maintained artifact such as
  `io.github.webrtc-sdk:android` or `io.getstream:stream-webrtc-android`). The
  standard way to do WebRTC on Android — Oboe/AAudio low‑latency capture, Opus,
  H.264, DTLS‑SRTP, congestion control, all handled. We do **not** hand‑roll
  libsrtp/BoringSSL/RTP — that's the exact "easy to get insecure" trap.
- **PC: libdatachannel** (C++, MIT, standards‑compliant WebRTC: DTLS‑SRTP + RTP).
  Chosen over Google's libwebrtc (brutal to build on Windows) and Pion/Go
  (would force cgo bridges to our existing FFmpeg decode + softcam + WASAPI).
  libdatachannel drops cleanly into the existing C++ receiver: it delivers
  SRTP‑decrypted RTP; we depacketize → FFmpeg decode → the sinks we already have.

### Signaling (QR‑bootstrapped, no server)

The SDP is too big and dynamic for the QR alone, so the QR only **bootstraps**:

1. PC generates its DTLS identity (cert + fingerprint), opens a TCP signaling
   listener, and shows a QR: `PCAM3:<pcIP>:<sigPort>:<pairSecret>`.
2. Phone scans, connects to the TCP listener, and the two exchange **SDP
   offer/answer** (containing DTLS fingerprints and LAN host ICE candidates) over
   that TCP channel, authenticated by `pairSecret`. **The PC is the WebRTC
   *offerer* with recvonly tracks (+ `RtcpReceivingSession`); the phone *answers*
   sendonly.** (Proven necessary in Phase 1: a sendonly offer to an unprepared
   answerer is rejected with `m=… 0`/port 0 and the track never opens — every
   working libdatachannel media example has the receiver offer.)
3. WebRTC media (DTLS‑SRTP/UDP) establishes LAN‑direct. The exchanged DTLS
   fingerprints ARE the pinned identity; `pairSecret` gates the exchange (MITM
   resistance without a CA).

This reuses the pairing pattern we already have (PCAM1/PCAM2), just carrying SDP.

**Wire protocol** (proven in Phase 1b, `webrtc_signaling.cpp`; the phone mirrors it in
Kotlin): length‑prefixed messages `[1 byte type][4‑byte big‑endian length][payload]`,
types `S`=pairSecret, `O`=offer SDP, `A`=answer SDP. Sequence over the TCP channel:
`phone→PC 'S'` (gated) → `PC→PC 'O'` offer (recvonly, sent after ICE gathering complete
so candidates are inline — no trickle) → `phone→PC 'A'` answer (sendonly, after its
gathering). Then DTLS‑SRTP media over UDP.

## Migration strategy (no bandaids, no breakage)

- **Parallel, not rip‑and‑replace.** RTSP and SRT modes keep working through the
  whole migration. WebRTC ships as a third mode, becomes default only once it's
  proven on the tester's hardware, and the old paths are removed last.
- **Mic‑only first.** It's the cleanest win (all three fronts improve, no video
  complexity, and it deletes the dummy‑video‑encoder hack). Video track second.

## Phases

| # | Deliverable | Who can test |
|---|---|---|
| 0 | ✅ DONE. Deps + build: libdatachannel (`WITH_WEBRTC`) links into the receiver; WebRTC SDK gradle dep on Android — branch APK build green. | Me / CI |
| 1a | ✅ DONE. DTLS‑SRTP media loopback (`webrtc_loopback.exe`): two peers, LAN host candidates, Opus RTP flows (45/50). Biggest transport de‑risk. | Me |
| 1b | ✅ DONE. `PCAM3` TCP SDP offer/answer exchange, `pairSecret`‑gated (`webrtc_signaling.exe`): full handshake + WebRTC media over a real socket (40/50 RTP). | Me |
| 2a | ✅ DONE. Codec bridge (`webrtc_audio.exe`): libopus‑encoded tone → RTP → FFmpeg Opus decode, decoded PCM RMS=0.21 matches the tone. RTP header parse handles CSRC+extensions. | Me |
| 2b | ✅ DONE. `receiver.exe --webrtc` (webrtc_receiver.cpp): PCAM3 signaling server + offerer + RTP→Opus decode → audio thread → `WasapiSink`. Verified with `webrtc_testsender.exe` (phone stand-in, real Opus): recorded CABLE Output shows a steady −18 dB tone for the full 6 s, no dropouts. | Me |
| 3 | ✅ CODE DONE (awaiting tester runtime check). Phone `WebRtcSender.kt` (org.webrtc): PCAM3 client, answers the PC's recvonly offer with a sendonly mic Opus track. GUI: "Wi-Fi transport" dropdown (Standard/Encrypted/Low-latency⚡) launches `receiver.exe --webrtc` + PCAM3 QR. Both sides compile; CI APK green (0.5.0-webrtc.1). First real phone→PC WebRTC audio needs the tester's device. | Tester device |
| 4 | Mic‑only end‑to‑end + measure latency & battery on the tester's phone vs SRT. Go/no‑go on the numbers. | Tester device |
| 5 | Add **H.264 video** track both ends → full webcam. | Tester device |
| 6 | Make WebRTC the default, keep RTSP as the compatibility fallback, retire SRT. | Tester device |

Gate: after Phase 4 we look at real numbers. If WebRTC doesn't beat SRT on the
tester's hardware, we stop and keep SRT — the parallel strategy means that costs
us nothing already shipped.

## Benchmarking (before/after — drives the Phase‑4 gate)

Both transports get measured under identical conditions (same phone, mode,
quality, room, Wi‑Fi), SRT first (baseline), WebRTC after.

- **Latency — `latbench.exe`** (PC tool, built on the existing WASAPI code):
  opens a WASAPI **loopback** capture of the default speakers AND a capture of
  **CABLE Output**, plays a click train through the speakers (phone mic hears it
  → streams back → lands in CABLE), and cross‑correlates the two channels per
  click. The gap = full mouth‑to‑virtual‑mic latency (capture + codec + transport
  + jitter + render). Both channels share one capture clock, so no cross‑process
  sync. A fixed speaker/acoustic offset is constant across runs, so the
  before/after **delta is exact** even if the absolute carries that offset.
  Report mean ± stddev over ~30 clicks.
- **Battery**: phone streaming a fixed mode/quality/duration, screen off; sample
  `adb shell cat /sys/class/power_supply/battery/current_now` (µA) at intervals
  and/or `dumpsys batterystats` for the app UID → average mA. SRT vs WebRTC.

Capture the **SRT baseline before touching the media path**, so "before" is
locked in.

## Build notes (PC / vcpkg)

- Required feature set: **`libdatachannel[core,srtp,ws]`** plus **`libsrtp[openssl]`**.
  Defaults omit both SRTP and OpenSSL — without SRTP media tracks are disabled;
  without OpenSSL, SRTP falls back to a crypto path that **fast‑fails** (0xC0000409)
  during keying. With OpenSSL the log shows "Deriving SRTP keying material (OpenSSL)".
- libSRTP 2.8.0 won't build under MSVC with warnings‑as‑errors (`C4214`). Fixed via
  the vcpkg **overlay port** at `windows/vcpkg-overlays/libsrtp` (adds
  `-DENABLE_WARNINGS_AS_ERRORS=OFF`). CI must pass `--overlay-ports` to that dir.
- Configure the WebRTC receiver with `-DWITH_WEBRTC=ON -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/…/vcpkg.cmake`.

## Risks

- **Android is untestable by me** (CI builds only). Phases 3–5 need the tester's
  device in the loop; expect device‑specific iteration.
- **libdatachannel dependency chain on Windows** (OpenSSL, libsrtp, libjuice,
  usrsctp) — Phase 0 sets this up via vcpkg/CMake in CI.
- **A/V sync** once video is added (two tracks, one PeerConnection — WebRTC
  handles RTCP sync, but the softcam/WASAPI feed timing needs care).
- **QR size** — mitigated by exchanging SDP over TCP, not in the QR.
