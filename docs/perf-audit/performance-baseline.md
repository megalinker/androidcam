# PhoneCam — Baseline & Measurement Methodology

## 0. Measurement-quality disclaimer (read first)

This audit was performed **without access to a running phone + Windows receiver + profiling
hardware**. No live latency, power, CPU, or throughput numbers could be captured. Therefore:

- Every quantitative claim in `performance-findings.md` is labelled **Confirmed** (code fact,
  no runtime needed), **Strong** (mechanism authoritative — e.g. documented FFmpeg behavior — but
  the magnitude needs measurement), **Moderate**, or **Speculative**.
- All "expected benefit" figures are **bounded estimates or hypotheses**, explicitly marked
  *unverified on real hardware*, with an exact measurement procedure to confirm/refute each.
- The repo already ships the right instrument for the single hardest number
  (mouth→virtual-mic latency): **`latbench.exe`** (`windows/src/latbench.cpp`). It is the anchor
  of the audio-latency and A/V-offset methodology below.

Nothing here should be read as "measured X → Y". It is "here is the exact experiment that will
measure X → Y, and here is the code reason to expect a change."

## 1. Environment template (fill per run)

| Field | Value |
|---|---|
| Phone model / SoC / Android / build | |
| Camera (front/back), sensor | |
| PC CPU / GPU / RAM / Windows build / power plan | |
| FFmpeg build (shipped: BtbN `master-latest gpl-shared`, libavcodec 63) | |
| Receiver build (Release /O2; softcam ON) commit | |
| Sink: softcam consumer app; audio endpoint (VB-CABLE vs speakers) | |
| Screen on/off, brightness, charging, ambient °C, battery start % | |
| Transport (USB / Wi-Fi WebRTC / USB-tether WebRTC), band (2.4/5 GHz) | |

## 2. Test matrix (minimum)

Transport {USB, Wi-Fi WebRTC, USB-tether WebRTC} × Track {cam, mic, both} × Camera {front, rear}
× Resolution {720p30, 1080p30, 1080p60, 2160p30} × Duration {60 s warm, ≥10 min sustained}
× Sink {softcam-preview only, real consumer (Zoom/OBS)} × Windows {10, 11}.
Report **p50/p95/p99/min/max/stdev** over ≥5 repetitions; separate **cold vs warm** start.
Never compare runs at materially different thermal states without noting it.

## 3. Clock domains & how to measure cross-device latency

Android-monotonic and Windows-QPC are **not** comparable. Do not subtract them. Use:

1. **Audio (mouth→virtual-mic): `latbench.exe`.** Plays a per-click unique-frequency train out a
   render endpoint, loopback-captures it (REF) and captures `CABLE Output` (RET) on the shared QPC
   stamp; `RET−REF` per click = capture+codec+transport+jitter+render. A fixed acoustic offset is
   constant across runs, so the **A-vs-B delta is exact**. Report mean±stdev, n≥15 inliers.
   - Real run: `latbench.exe` (phone streaming into CABLE, phone mic near the PC speakers, volume up).
   - Self-test: `latbench.exe --selftest` (CABLE Input→Output; validates the tool, expect a few ms).
2. **Video (glass→glass):** film a high-FPS (240 fps) capture of a millisecond clock/LED **and** the
   PC preview in the same frame; count frames between a transition and its appearance. Or use an
   on-screen QR/frame-counter and OCR. This absorbs the whole camera→softcam→consumer path.
3. **A/V offset:** a clap or a synchronized flash+beep; compare the video-onset frame to the audio
   onset in the CABLE recording.
4. **Per-stage (single device):** monotonic timestamps *within* one process only (never across).

## 4. Instrumentation to add (cheap, removable, off in release)

All of these are behind a compile flag / env var and aggregate (never per-frame synchronous disk I/O).

### Windows receiver — per-stage video (single-clock, PC only)
Insert monotonic `steady_clock` stamps and keep rolling p50/p95 counters (emit once/sec to stderr):
- `usb_receiver.cpp` — enqueue time on `VideoQueue.push`; dequeue time on `pop`; **queue residence** = dequeue−enqueue.
- `video_sink.cpp:WriteFrame` — decode-output→`sws_scale`→softcam submit deltas.
- `webrtc_receiver.cpp:onFrame` — packet-arrival→decode-output; and the `decodeMutex` wait time
  (instrument the lock to expose audio-starvation from video, F-03).

### Windows receiver — queue telemetry (all three queues)
Add to each `push`/`pop`: current depth, max depth, mean depth, dropped count + reason, producer
blocks, consumer underruns. Emit aggregated once/sec. (Today only frame *counts* are logged.)

### Phone — per-stage (Android monotonic only)
- `UsbStreamer` encoder callback: `SystemClock.elapsedRealtimeNanos()` at `onOutputBufferAvailable`
  vs `info.presentationTimeUs` → capture→encode delay; count frames/sec actually sent vs requested fps.
- Log dropped pre-encode frames if any (camera producing faster than the encoder drains).

### Audio
- `wasapi_sink.cpp`: log `GetCurrentPadding` (standing WASAPI latency in ms) once/sec, swr delay
  (`swr_get_delay`), and underrun events (avail==0 spins). This directly measures the F-05 200 ms
  buffer's real occupancy.

## 5. Baseline runs to capture (the "before" column)

For each matrix cell, record: end-to-end latency (§3), per-stage (§4), Android CPU%/power
(batterystats / Perfetto / Power Profiler), PC CPU%/GPU%/RAM/threads (WPR+WPA / Task Manager /
VS profiler), encoded bitrate (phone log), frame rate at capture/encode/decode/sink, dropped frames
per stage, queue depths/residence (§4), audio underruns, time-to-first-softcam-frame, and
time-to-first-mic-samples. Repeat ≥5×; keep raw + aggregate.

## 6. Tools
Android Studio profiler, Perfetto, `batterystats`/Battery Historian, Android Power Profiler;
Windows Performance Recorder/Analyzer (ETW), Visual Studio native/managed profilers; `latbench.exe`;
Wireshark only for packet-level evidence. If a tool is unavailable on the hardware, **document the
gap — do not fabricate the number.**

## 7. Acceptance gates (per the audit brief)
- **Latency win** accepted only if the target metric improves beyond run-to-run noise, frame drops
  and audio underruns don't materially rise, A/V offset stays in range, quality holds, sustained
  runs stay stable, reconnect/shutdown still work.
- **Battery win** accepted only if energy improves in repeated comparable runs, not merely from a
  silent quality drop, thermals don't worsen, latency stays in the profile's target, resources
  release on stop.
- **CPU win** accepted only if CPU time/cycles improve in comparable traces with no latency
  regression, no new queueing, no unbounded memory.
