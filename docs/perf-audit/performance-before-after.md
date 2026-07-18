# PhoneCam — Before / After (measured)

Rig: Pixel 10 Pro (USB, 1080p30 unless noted), 20-core Windows PC, receiver built from this branch with
`PHONECAM_STATS=1`. Harness: `windows/packaging/before-after.ps1` (drives a clean USB stream, runs 60 s,
parses the `[stats]` lines). "Before" = the flag off / the pre-change baseline; "After" = the change.
Honest headline: **the pipeline was already lean, so the wins are the drift fix + APK size + robustness,
not a big latency drop.**

## Measured — clear wins

| Metric | Before | After | Δ | How |
|---|---|---|---|---|
| **Mic clock drift** (audio.wasapi over 10 min) | 37→45→62→72→**77 ms** (rising, ~67 ppm; would click ~40 min in) | **~35 ms, flat** | drift eliminated | `-Drift` A/B, 10-min runs (F-03) |
| **APK size** | 26.6 MB (arm64+armv7+x86+x86_64) | **15.5 MB** (arm64+armv7) | **−11.1 MB (−42%)** | build both, `ls`/zip listing (F-20) |

## Measured — no change (honest negatives)

| Metric | Before | After | Verdict | How |
|---|---|---|---|---|
| **H.264 decode latency** | 1.8 ms (single-threaded) | 1.8 ms | **F-05 rejected** — frame-threading never active (FFmpeg defaults `thread_count=1`) | `video.decode` A/B `frame`/`slice` |
| **Audio standing latency** | 25 ms | 28.8 ms | **F-11 no win** — audio path was already at the floor; the 200 ms buffer was a ceiling, not standing latency | `-LowLatencyAudio` A/B, 60 s |
| **video.sink (convert+push)** | 1.66 ms | 1.69 ms | unchanged — convert is cheap; F-22 removed a *preview* copy, not a sink copy | `video.sink` p50 |
| **Queue drops** | 0 | 0 | no backlog either way | `audio.queue`/`video.queue` |

These "no-wins" are results, not failures: measurement kept two plausible-sounding findings (F-05, F-11)
from shipping as false wins, and confirmed the PC receiver is not the latency bottleneck (decode+convert
≈ 3.5 ms, audio standing ≈ 25 ms, no drops). Remaining glass-to-glass latency is phone-side + delivery.

## Verified by code review + build, not yet by a runtime number (pass/fail or scenario-gated)

| Change | Nature | Evidence |
|---|---|---|
| F-01 signaling recv timeout / no-answer wedge | robustness (pass/fail) | reviewed; trigger: connect + send nothing → server recovers & Ctrl-C works within 20 s |
| F-02 USB tail-drop on overflow | robustness (pass/fail) | reviewed; trigger: force queue overflow → contiguous GOP, no reference-chain corruption |
| F-08 rate-limited PLI on decode error | recovery (needs induced loss) | reviewed; trigger: drop packets → recovery in ~1 RTT vs a GOP |
| F-18 adb chain off the UI thread | UX (observe) | drag the window during USB Start — before: "Not Responding"; after: responsive |
| F-07 split decode locks | audio jitter under video load | reviewed; effect bounded by `video.sink` ≈ 1.7 ms (sub-frame) |
| F-09 MMCSS on the render thread | underruns under CPU load | reviewed; measure `audio.queue drops` while stressing the PC |
| F-34 idle skip (no camera consumer) | idle CPU | reviewed; needs a softcam build + no-consumer scenario to show CPU% delta |
| F-12 raw mic (no AEC) | mic quality | **by ear** — record `CABLE Output` with/without the checkbox |
| F-13/F-15/F-16/F-17 phone-side hints | glass-to-glass / jitter | device-dependent; need a high-speed camera |

## How to reproduce
```powershell
cd windows\packaging
.\before-after.ps1 -Label baseline                 # default
.\before-after.ps1 -Label lowlat  -LowLatencyAudio # F-11
.\before-after.ps1 -Label drift   -Drift -Seconds 600   # F-03 (watch AudioWasapiMeanMs trend)
```
APK: build `assembleDebug` on `main` (all ABIs) vs this branch (arm-only), compare file size.

## Bottom line
Two clean measured wins (the **audio drift fix** and the **42% smaller APK**), two measured non-wins that
measurement correctly stopped from shipping as fake wins (F-05, F-11), a confirmed-lean pipeline, and a
set of reviewed robustness/quality fixes whose value is "the bug no longer happens" / "it sounds better"
rather than a latency number. The most valuable single change is **F-03** — the only continuous
regression the audit found and fixed, verified over a 10-minute run.
