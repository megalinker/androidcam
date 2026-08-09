# Battery investigation (2026-08-09)

Follow-up to the 2026-07-18 audit (`performance-findings.md`), scoped to the question **"is the
Android app drawing more power than the workload justifies?"** plus two adjacent asks: show the phone
battery on the PC, and investigate the occasional visual artifact without paying for it in battery.

**Hardware status: no Android device was attached during this work** (`adb devices` empty). Every
magnitude below is therefore either (a) a structural fact read off the code, or (b) an *expected*
effect with the procedure to measure it. Nothing here is presented as a measured result unless it
says so. The whole point of the diagnostics added in this pass is that the next person does not have
to guess.

---

## The pipeline, as it actually is

### Wi-Fi (WebRTC) — the default

```
Camera2 (via org.webrtc Camera2Enumerator/Camera2Session)
  → SurfaceTextureHelper (GPU texture, no CPU copy)
    → VideoSource / NativeAndroidVideoTrackSource   [adapts: crop+scale on GPU]
      → DefaultVideoEncoderFactory → HardwareVideoEncoder → MediaCodec H.264   [hardware]
        → libwebrtc RTP packetizer → DTLS-SRTP → UDP, host ICE candidates only
──────────────────────────────────────────────────────────────────────────────────────
          → libdatachannel (PC): SRTP decrypt → RtcpReceivingSession → H264RtpDepacketizer
            → FFmpeg h264 decode (single-threaded) → sws_scale → BGR24
              → softcam DirectShow virtual camera (+ optional GDI preview)
```

Audio: `JavaAudioDeviceModule` → Opus (libwebrtc) → same DTLS-SRTP session → FFmpeg Opus decode →
bounded queue → dedicated WASAPI thread → VB-CABLE.

Files: `WebRtcSender.kt`, `StreamService.kt` / `webrtc_receiver.cpp`, `video_sink.cpp`,
`wasapi_sink.cpp`.

### USB (scrcpy-style)

```
Camera2 → MediaCodec input Surface → H.264 (hardware) → framed TCP over `adb forward`
AudioRecord → raw PCM S16LE 48 kHz → same socket
──────────────────────────────────────────────────────────────────────────────────────
  usb_receiver.cpp: framing → VideoQueue → FFmpeg decode → softcam; PcmQueue → WASAPI
```

Files: `UsbStreamer.kt` / `usb_receiver.cpp`.

### Established facts (not assumptions)

* **The encoder is hardware on both paths.** USB uses `MediaCodec.createEncoderByType(AVC)`; the
  Wi-Fi path uses libwebrtc's `HardwareVideoEncoderFactory`, and the Java software factory does not
  implement H.264 at all — so an H.264 stream is hardware or it does not exist. The diagnostics now
  record `MediaCodecInfo.isHardwareAccelerated()` (USB) and `encoderImplementation` (Wi-Fi) so this
  is confirmed per device rather than assumed.
* **No CPU colour conversion, no Bitmaps, no per-frame pixel work on the phone.** Both paths hand the
  camera a Surface the encoder owns; frames never enter the JVM heap.
* **No preview is rendered on the phone.** Deliberate — a lit screen was previously the dominant heat
  source.
* **There is exactly one camera session and one encoder per stream.**
* **The wake lock is `PARTIAL` only, capped at 4 h, released on every stop path**, including the
  failure paths.
* **The service auto-stops** after 5 min with no PC (2 min after a drop).

---

## Findings

Numbered `B-xx` to keep them distinct from the earlier `F-xx` audit.

### Confirmed waste — fixed

**B-01 · The Wi-Fi path opened the camera before it had anywhere to send frames.** *(fixed)*

`WebRtcSender.run()` called `startCamera()` — including `capturer.startCapture()` — *before*
`connectSignaling()`. `connectSignaling()` retries every 2 s for as long as the stream is up, and
`StreamService` allows **5 minutes** with no PC before auto-stopping. So in the "reconnect to last
PC" flow, with the desktop app not yet listening, the sensor, ISP and GPU texture path ran at the
full selected rate (1080p30 by default) and every frame was discarded because the encoder had no
sink.

Fix: split `startCamera()` (creates the track — must happen before `setRemoteDescription`, since
Unified Plan matches it to the offer's video m-line) from `startCapture()` (opens the camera), and
call the latter only after the signaling socket connects.

Expected effect: removes up to 5 minutes of full-rate camera capture per failed/slow connection
attempt. Zero effect on a session that connects immediately. Latency cost: none measurable — camera
open overlaps the SDP exchange, ICE and the DTLS handshake, all of which must complete first.
Compatibility: none; purely local ordering.

**B-02 · A heap allocation per encoded frame on the USB path.** *(fixed)*

`UsbStreamer`'s encoder callback did `ByteArray(info.size)` for every output buffer — 30–60
allocations/s of tens to hundreds of KB at 1080p, all immediately garbage. Replaced with one growable
staging buffer; the callback is single-threaded and `writeFrame` copies to the socket before
returning, so reuse is safe.

Expected effect: removes a steady GC pressure source. Honestly: on a modern ART generational
collector this is unlikely to be a *large* battery item — but it is a confirmed, removable per-frame
allocation, and the brief rates one of those above ten speculative tweaks. `gcCount` in the sample
line makes the before/after visible.

**B-03 · Blind fixed-FPS request could let the sensor run at double rate.** *(fixed)*

`UsbStreamer` asked for `CONTROL_AE_TARGET_FPS_RANGE = [fps, fps]` and, if the device rejected the
request, retried **with no range at all**. With AE unconstrained, the HAL is free to run the sensor at
its maximum (commonly 60) while we encode and transmit 30 — twice the ISP work for frames nobody
sees. Now the advertised `CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES` are queried and the narrowest range
whose upper bound is the requested fps is chosen; a range faster than we encode is never selected.

**B-04 · Capture size was never checked against what the camera supports.** *(fixed)*

The encoder was configured at the preset size and the camera was handed that Surface. Camera2 does
not scale arbitrarily: an unsupported size either fails session configuration or makes the HAL pick
something else and scale — wasted ISP work, and a stream whose real geometry no longer matches the
header we sent the PC. `pickCaptureSize()` now snaps to the nearest supported size **no larger than**
requested, and reports requested-vs-actual in the diagnostics.

**B-05 · Two microphone captures could briefly overlap on reconnect.** *(fixed)*

`stopAudio()` only called `interrupt()`. `AudioRecord.read()` is a blocking native call that ignores
interrupts, so the capture thread kept running until its next ~20 ms read returned — and
`audioThread` was already nulled, so a fast reconnect could start a second `AudioRecord` while the
first was still capturing. Now the socket reference is cleared first (the loop's exit condition) and
the thread is joined with a 500 ms bound.

### Real cost, not waste (measure before touching)

**B-06 · Capture resolution vs transmitted resolution on Wi-Fi.**

The default quality is **1080p30**, and `degradationPreference = MAINTAIN_FRAMERATE` means WebRTC
sheds *resolution* under congestion. libwebrtc implements that by cropping/scaling in the video
source — it does **not** reconfigure the camera. So on a weak link the sensor can be producing 1080p
while the encoder emits 540p: 4× the pixels through the ISP for nothing.

This is how libwebrtc is designed, and changing the camera format mid-session causes a visible glitch
and a slow reconfigure — so this pass deliberately does **not** change it. What it does instead is
make the gap observable: `webrtc_stats` now logs `capGeom` (what the camera produced) next to
`frameWidth`/`frameHeight` (what was encoded). If a real session shows a persistent large gap, the
fix is a *measured* one (lower the preset, or drive `changeCaptureFormat`), not a guess.

**B-07 · The unavoidable floor.** Continuous camera capture, real-time hardware H.264, Opus, a
partial wake lock and a Wi-Fi radio kept awake is inherently a watt-class workload on a phone. Before
concluding anything is wrong, run the `idle-connected` control scenario: it isolates our overhead
from the workload. A finding of "this is the expected cost" is a valid result and the diagnostics are
built to be able to reach it.

### Investigated and ruled out

* **Software encoding / silent fallback to software.** Not possible for H.264 on either path (see
  above). Now confirmed per device rather than argued.
* **Duplicate encoders / duplicate camera sessions / stale frame consumers.** One of each; verified
  by reading every start/stop path.
* **CPU colour conversion, Bitmap churn, per-frame copies on the phone.** None — both paths are
  Surface-to-encoder.
* **Busy-wait loops on the phone.** None. Every loop is blocked on a socket read, an `AudioRecord`
  read, or a `Handler.postDelayed`. The receiver's 100 ms `sleep` spin loop was replaced in this pass
  by a blocking read with a 300 ms timeout, which is strictly better.
* **Excessive timers.** The phone had two periodic tasks (a 1 s UI refresh, active only while the
  activity is visible, and a 30 s idle check). This pass adds one 60 s status heartbeat and, only
  when diagnostics are enabled, one 30 s sampler.
* **Wake locks held too long / leaked.** Released on every path including start failure and
  `onDestroy`.
* **Debug logging left on in production.** The existing logging is milestone-level. New diagnostics
  are off by default.
* **Foreground-service work continuing after stop.** `stopStreaming()` tears down the sender, the
  streamer, the wake lock, the idle check, the status heartbeat and the foreground notification.
* **Reconnect / retry storms.** The signaling retry is a 2 s backoff bounded by the 5 min idle
  auto-stop; the desktop app caps reconnects at 30 attempts.
* **The diagnostics themselves.** Off by default; the hot path is one volatile read.

---

## Artifact investigation (Part 3)

**Not reproduced** — no device was available. What follows is a structural defect found by reading the
code and the library sources, plus the instrumentation needed to confirm it on hardware.

### The transport, and what it does with a lost packet

Wi-Fi is **WebRTC / H.264 over RTP in DTLS-SRTP**, PC side `libdatachannel 0.24.5`. Three facts, from
that library's source:

1. `Description::Video::addVideoCodec` advertises `a=rtcp-fb:96 nack` and `nack pli`
   (`src/description.cpp`). The phone therefore believes retransmission is available.
2. `RtcpReceivingSession` only ever emits **RR, REMB and PLI** (`src/rtcpreceivingsession.cpp`).
   **There is no NACK generator.** We advertise retransmission and never request one, so a lost packet
   is simply gone.
3. `H264RtpDepacketizer::reassemble` builds a frame from whatever packets arrived between two RTP
   timestamps and returns it **unconditionally** (`src/h264rtpdepacketizer.cpp`). A sequence gap only
   clears `continuousFragments` — it never suppresses the frame or flags it. A frame with a hole in it
   is delivered to `onFrame` looking exactly like a complete one.

So: **a lost packet produces a silently-truncated frame that is fed to the decoder as if it were
valid.** The decoder conceals the missing macroblocks, and because subsequent P-frames only code what
*changed*, the wrong pixels persist. WebRTC senders emit keyframes on request rather than on a short
timer, so "until the next keyframe" can be a very long time — but any movement through the region
forces the encoder to re-code it, which clears it.

That is precisely the reported symptom: *a region goes wrong, stays wrong, and clears when a hand
passes through it.*

### Why the existing recovery did not fire

The previous code requested a keyframe when FFmpeg reported
`FF_DECODE_ERROR_INVALID_BITSTREAM | FF_DECODE_ERROR_MISSING_REFERENCE`. A **truncated but
parseable** frame — the exact output of a mid-frame packet loss — does not necessarily set either;
the concealment path sets `FF_DECODE_ERROR_CONCEALMENT_ACTIVE` / `FF_DECODE_ERROR_DECODE_SLICES`
instead. So the most common corruption case had no recovery request at all.

### The fix (near-free, by design)

1. **`RtpSeqTracker` / `RtpLossMonitor`** (`rtp_seq.h`, `rtp_loss.h`) sit at the head of the incoming
   media chain and watch raw RTP sequence numbers. Per packet: a 16-bit subtract, a compare and one
   relaxed atomic store — nothing allocated, negligible beside the SRTP work already done on that
   packet. Reordering and duplicates are distinguished from loss so ordinary jitter never triggers
   recovery; a jump of >1000 is treated as a stream restart, not as 30 000 lost packets.
2. **`onFrame` asks whether packets went missing since the previous frame**, before decoding. If they
   did, the frame it is about to submit is known-incomplete.
3. **The decode-error mask was widened** to include `CONCEALMENT_ACTIVE` and `DECODE_SLICES`.
4. **Either signal requests one keyframe**, rate-limited to **one per second** (raised from the
   previous 400 ms).

Deliberately *not* done, per the brief: no NACK/retransmission, no FEC, no shortened GOP, no bitrate
increase, no redundant encoding, no extra per-frame processing, no quality reduction.

### Battery cost of the artifact fix

**No measurable increase expected; unmeasured on hardware.** Reasoning:

* In a clean session the detector never fires and **nothing changes at all** — zero extra keyframes.
* Detection is a few integer operations per packet, on a packet that has just been through DTLS-SRTP.
* When it does fire, the cost is one intra frame, at most once per second. Intra encoding is
  *cheaper* than inter (no motion search), and under WebRTC's congestion control the extra bytes come
  out of the same bitrate budget rather than adding to it.
* The rate limit was **loosened** from 400 ms to 1 s at the same time as detection was broadened, so
  the worst-case keyframe rate during a loss burst is 2.5× *lower* than before.

To verify rather than trust that: `pli` and `pliSuppressed` are in the session-end line, and
`keyFramesEncoded` / `targetBitrate` are in the phone's `webrtc_stats`. Run the same scenario before
and after and compare `avgEncKbps` and `battPctPerHour`.

### USB path

TCP cannot lose or reorder, so the same mechanism cannot apply. The one way the cable path can corrupt
itself is the receiver's own `VideoQueue` overflow (which drops encoded units and breaks the reference
chain). That is now counted, and it now sends a `'K'` control frame asking the phone for a keyframe
instead of waiting out the GOP — rate-limited to 1/s. Decoder-reported damage is counted separately,
so "the cable path is clean" and "we corrupt it ourselves" are distinguishable.

### If it happens again

Press **Mark video problem**. The `[mark]` line records loss, gaps, reordering, duplicates,
incomplete frames, decoder-damaged frames and keyframe requests at that instant, and the phone stamps
its own log at the same moment. If `gaps` and `incompleteFrames` are non-zero around the mark, the
hypothesis above is confirmed. If they are zero, the corruption is above the transport and the search
moves to the decoder, `sws_scale`/`VideoSink`, or softcam.

---

## What still needs hardware

1. **Every battery magnitude in this document.** Run the scenarios in `docs/diagnostics.md` §4.
2. **Confirmation that the encoder is hardware on the test device** — one line in the log now.
3. **B-06**: how large the capture-vs-encode resolution gap actually is in practice.
4. **The artifact root cause.** Needs one occurrence with a mark on it.
5. **Effect of B-01** — needs a "PC not listening yet" scenario, which is exactly the reconnect flow.
6. **Power rails** need a Pixel 6 or later. The reported test device (Pixel 10 Pro) qualifies.
7. **A second Android model.** All camera/encoder findings are HAL-dependent; the diagnostics report
   requested-vs-actual precisely so another device can be checked quickly.
