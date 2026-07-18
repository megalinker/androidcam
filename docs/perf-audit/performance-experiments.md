# PhoneCam — Experiments & Before/After

Each experiment: hypothesis · controlled variables · procedure · acceptance threshold · rollback.
**No hardware was available to run these** — results columns are left for the operator. Change **one
variable per experiment**; never combine unrelated changes in one benchmark.

---

## E-01 — H.264 decode frame-threading (F-05) — **DONE: REJECTED**
- **Hypothesis:** `thread_type = FF_THREAD_SLICE` removes a standing `(thread_count-1)`-frame decode delay.
- **Result (Pixel 10 Pro, USB 720p30, 20-core PC, 2026-07-18):** **refuted.** The decoder resolved to
  `thread_count=1, active_thread_type=0` (single-threaded); frame vs slice A/B were **identical**
  (p50 1.83 vs 1.82 ms, p95 2.17 vs 2.16 ms, `video.queue depth_max=1 drops=0` both). FFmpeg defaults
  `thread_count` to 1 — it does **not** auto-multithread — so `FF_THREAD_FRAME` was never active. No
  latency existed to remove. **Code reverted.** See `measured-results.md`.
- **Bonus data:** `video.sink` ≈ 1.7 ms, `audio.wasapi` standing ≈ 20 ms (not the 200 ms buffer),
  `audio.swr`/`audio.sink` ≈ 0 ms — the PC receiver is not the latency bottleneck.

## E-02 — Signaling recv timeout (F-01)
- **Hypothesis:** `SO_RCVTIMEO` on the accepted socket makes a half-open peer recoverable and Ctrl-C
  responsive, without aborting real pairings.
- **Procedure:** (a) `nc <pc> 8891`, send nothing → confirm the server recovers and a real phone still
  pairs; (b) kill the phone mid-negotiation → server re-listens within the timeout; (c) Ctrl-C during a
  half-open connection → exits within the timeout; (d) 20× normal pairings → 0 spurious aborts.
- **Acceptance:** (a)-(c) pass; (d) shows no regression in pair success rate.
- **Rollback:** remove `setsockopt`. **Result:** _(unfilled)_

## E-03 — adb chain off the UI thread (F-18)
- **Hypothesis:** running the adb sequence on a worker keeps the GUI responsive during USB Start /
  camera switch.
- **Procedure:** with a slow/cold adb, click Start and camera-switch; confirm the window never enters
  "Not Responding" (message pump stays live — e.g. drag the window) and status still updates.
- **Acceptance:** no UI freeze; no cross-thread WinForms exception; media still starts.
- **Rollback:** revert to synchronous. **Result:** _(unfilled)_

## E-04 — MMCSS on the WASAPI render thread (F-09)
- **Hypothesis:** `AvSetMmThreadCharacteristics("Pro Audio")` reduces render underruns under CPU load.
- **Procedure:** run a CPU stressor (or a parallel encode) on the PC; count WASAPI underruns
  (instrument `avail==0` spins / a padding-hit-zero counter) over ≥5×5 min, with and without MMCSS.
- **Acceptance:** underrun count drops or is unchanged (never worse); no added latency.
- **Rollback:** remove the two `Avrt` calls. **Result:** _(unfilled)_

## E-05 — USB VideoQueue tail-drop (F-02)
- **Hypothesis:** dropping the newest unit on overflow yields a shorter/no corruption window vs dropping
  the oldest.
- **Procedure:** force overflow (4K on a weak PC, or artificially slow the decode thread); measure the
  duration of visible corruption per overflow event, oldest-drop vs tail-drop.
- **Acceptance:** corruption duration drops; queue stays bounded; no new stall.
- **Rollback:** revert to `pop_front`. **Result:** _(unfilled)_

## E-06 — Rate-limited PLI on decode error (F-08)
- **Hypothesis:** requesting a keyframe on `decode_error_flags` cuts post-loss corruption from up-to-a-GOP
  to ~one RTT + keyframe.
- **Controlled:** induce fixed packet loss (netem on the tether, or a congested AP); same clip.
- **Procedure:** measure time-from-loss-to-clean-frame with/without the error-triggered PLI; verify the
  PLI rate-limit prevents a keyframe storm (watch encoded bitrate).
- **Acceptance:** recovery time drops; no sustained bitrate spike; no regression on a clean link.
- **Rollback:** remove the error-triggered PLI. **Result:** _(unfilled)_

## E-07 — Event-driven WASAPI low-latency mode (F-11, opt-in)
- **Hypothesis:** `EVENTCALLBACK` + ~30–50 ms buffer (and a smaller upstream FrameQueue target) lowers
  mouth-to-virtual-mic latency measurably, without raising underruns beyond tolerance.
- **Procedure:** `latbench.exe` A/B (default vs low-latency mode), ≥15 inlier clicks each, plus a
  ≥10 min underrun count.
- **Acceptance:** `latbench` mean drops beyond noise **and** underruns don't materially rise; if
  underruns rise, keep the mode opt-in with a larger floor.
- **Rollback:** revert to `flags=0` / 200 ms. **Result:** _(unfilled — this is the ideal `latbench` case)_

## E-08 — WebRTC start-bitrate (F-13)
- **Hypothesis:** `setBitrate(start)` + `maxBitrate` + `scaleResolutionDownBy=1` shortens time-to-full-
  resolution on LAN.
- **Procedure:** log the source-resolution ramp (receiver `[video] source WxH` lines already print on
  each change); measure start→full-res wall time, with/without hints, over ≥5 connects.
- **Acceptance:** time-to-full-res drops; no audio starvation on a deliberately weak link (keep min low).
- **Rollback:** remove the encoding hints. **Result:** _(unfilled)_

## E-09 — No-AEC/NS mic mode (F-12, opt-in)
- **Hypothesis:** `AudioSource.MIC` / HW AEC+NS off yields fuller-band, less-pumped mic audio.
- **Procedure:** record `CABLE Output` with/without; compare spectra (band edge) and A/B listen; confirm
  no echo problem (there is no phone-side playback to echo).
- **Acceptance:** wider bandwidth / less pumping; acceptable room noise. Ship opt-in either way.
- **Rollback:** remove the option. **Result:** _(unfilled)_

## E-10 — Resampler drift compensation (F-03)
- **Hypothesis:** a small `swr_set_compensation` keyed to queue-fill nulls the phone-vs-PC clock drift,
  removing periodic clicks over long calls.
- **Procedure:** ≥15 min continuous tone into CABLE; count discontinuities and plot queue-depth trend,
  with/without compensation.
- **Acceptance:** click count → ~0; queue depth stays near target (no walk to the drop edge); no pitch
  wobble.
- **Rollback:** remove the compensation call. **Result:** _(unfilled)_

---

## Before/After results table (fill after each verified experiment)

| Metric | Baseline | New | Δ abs | Δ % | Runs | p95 / var | Hardware | Quality impact | Confidence |
|---|---|---|---|---|---|---|---|---|---|
| Decode submit→output (E-01, 720p, 8-core) | | | | | | | | | |
| Glass-to-glass, USB 1080p (E-01) | | | | | | | | | |
| Mouth→virtual-mic, `latbench` (E-07) | | | | | | | | | |
| WASAPI underruns / 5 min under load (E-04) | | | | | | | | | |
| Post-loss recovery time (E-06) | | | | | | | | | |
| Start→full-res time (E-08) | | | | | | | | | |
| Clicks / 15 min (E-10) | | | | | | | | | |

> Rule: no row is "improved" unless before/after used **comparable conditions** (same thermal state,
> scene, hardware) and the change is outside run-to-run noise. Report raw baselines, never bare %.

---

## Rejected experiments (evidence contradicted the hypothesis — see `performance-findings.md`)

- Longer USB GOP for battery/bandwidth (**S4** — CBR pins bitrate; phone on cable power).
- Force H.264 baseline to save battery (**S21** — High is never negotiated; would be a no-op).
- Remove "double" AEC/NS (**N-android-audio-power-1** — libwebrtc doesn't stack HW+SW).
- Zero-copy H.264 packet borrow (**N-win-video-0** — moves the copy into FFmpeg, no net win).
- Lower USB `VideoQueue` for latency growth (**N-win-transport-threads-3** — latest-wins sink already
  skips forward; only a memory ceiling).

Recording these prevents a future pass from re-running them. Revisit only under the specific conditions
noted in the findings table.
