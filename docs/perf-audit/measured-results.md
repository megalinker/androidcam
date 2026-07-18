# PhoneCam — Measured Results (on hardware)

**Rig:** Pixel 10 Pro (streaming), **USB path**, 720p30, Cam+Mic; Windows PC with **20 logical CPUs**;
receiver `windows/build-webrtc/Release` (Release /O2), instrumentation build (`PHONECAM_STATS=1`).
Date 2026-07-18. Single-clock (steady_clock) PC-internal deltas; ~1 Hz aggregation.

## Headline: F-05 (decode frame-threading latency) is **DISPROVEN**

The flagship hypothesis was that both H.264 decoders ran FFmpeg *frame* threading (auto thread_count),
adding a standing ~(cores-1)-frame decode-output delay. **Measurement refutes it:**

```
[stats] h264 decode: thread_count=1 active_thread_type=0   (0=none, 1=FRAME, 2=SLICE)
```

A/B via a temporary env knob (`PHONECAM_DECODE_THREADING=frame` vs `slice`), 40 s each:

| Config | video.decode p50 | p95 | queue depth_max | drops |
|---|---|---|---|---|
| frame (old default) | **1.83 ms** | 2.17 ms | 1 | 0 |
| slice (proposed fix) | **1.82 ms** | 2.16 ms | 1 | 0 |

**Identical**, because in *both* cases the decoder resolved to `thread_count=1, active_thread_type=0`
— i.e. **single-threaded, no threading at all**. The premise was wrong: **FFmpeg leaves
`thread_count=1` by default; it does *not* auto-pick `cpu_count`.** Setting `thread_type` alone does
nothing unless `thread_count` is also set to 0/auto or >1. The bundled `avcodec.h:1583` comment about
`FF_THREAD_FRAME` is accurate — but `FF_THREAD_FRAME` was **never active**, so there was no latency to
remove.

**Action taken:** the F-05/F-06 code (thread-type switch + the A/B env knob) was **reverted** — it was
a no-op. Decode is left single-threaded on purpose. F-05 is reclassified **Rejected (measured)**.

> Corollary throughput note: single-threaded 720p decodes in ~1.8 ms with `queue depth_max=1, drops=0`,
> so decode is *not* a bottleneck here. The only scenario where multi-threaded decode would help is 4K
> on a weak PC — if that ever shows `video.queue drops`, the throughput fix is `thread_count=0` +
> `FF_THREAD_SLICE`, tested then. Not a latency fix.

## Decode headroom (720p and 1080p both trivial, single-threaded)

| Resolution | `video.decode` p50 | p95 | `video.queue` |
|---|---|---|---|
| 720p30 | 1.82 ms | 2.16 ms | depth 1, 0 drops |
| 1080p30 | **1.6–1.7 ms** | ~2.2 ms | depth 1, 0 drops |

1080p is essentially the same as 720p (both dominated by fixed per-frame overhead, not pixel count, at
these sizes) — single-threaded decode has ample headroom, so 4K would very likely keep up too on this
20-core PC. Multi-threaded decode would only ever matter on a much weaker PC at 4K, and only for
*throughput* (drops), never latency.

## Positive result: the PC-side pipeline is lean (not the bottleneck)

| Metric | p50 | p95 / max | Reading |
|---|---|---|---|
| `video.decode` | 1.8 ms | 2.2 ms / ~8 ms | H.264 decode is cheap at 720p |
| `video.sink` (sws→BGR + softcam) | 1.7 ms | 1.9 ms | convert+push is cheap |
| `video.queue` | depth_max=1 | drops=0 | decode keeps up trivially |
| `audio.wasapi` (standing render latency) | **20 ms** | 20 ms | **the 200 ms buffer is a ceiling, not standing latency — real standing latency ≈ 20 ms** |
| `audio.swr` | ~0 ms | ~0 ms | resampler adds nothing |
| `audio.sink` | 0.01 ms | 0.3 ms | processing negligible |
| `audio.queue` | depth_max=1 | drops=0 | no backlog |

**Implications for the audit:**
- The whole **PC receive→decode→convert→sink** budget is **~3.5 ms video + ~20 ms audio-standing** — the
  receiver is *not* where glass-to-glass latency accumulates. The remaining latency lives on the phone
  (capture + encode) and in softcam→consumer delivery, which need external measurement (a high-speed
  camera / the consuming app's own stats), not these counters.
- **F-11 is right-sized down:** the feared 200 ms WASAPI buffer shows only **~20 ms** standing latency
  in practice — so an event-driven / smaller-buffer rework is far lower value than the audit implied.
  Still worth an opt-in, but not a priority.
- Findings that depended on video decode being slow (**F-07** audio-behind-video head-of-line) are
  bounded by `video.sink` ≈ 1.7 ms — real but sub-frame; keep as a cheap cleanup, not a priority.

## F-03 (audio clock drift) — CONFIRMED by measurement

A 10-min continuous stream (Pixel 10 Pro USB 1080p30, stable — 1 session, 0 reconnects) showed the
WASAPI standing latency (`audio.wasapi`) climbing **monotonically**:

| minute | 0–2 | 2–4 | 4–6 | 6–8 | 8–10 |
|---|---|---|---|---|---|
| `audio.wasapi` mean | 37 ms | 45 ms | 62 ms | 72 ms | **77 ms** |
| `audio.queue drops` | 0 | 0 | 0 | 0 | 0 |

**+40 ms over 10 min ≈ 67 ppm**: the phone's audio capture clock runs slightly faster than the PC's
render clock, so audio piles up in the render ring. No clicks yet (the ring hasn't reached its ~200 ms
ceiling — that would take ~40 min), but mic latency and A/V lip-sync visibly drift, and a long call
(>40 min) would start dropping ~20 ms chunks (audible clicks). **F-03 is real**, unlike F-05.

### Fix (implemented, opt-in `PHONECAM_DRIFT=1`, off by default)
`wasapi_sink.cpp` now runs a proportional `swr_set_compensation` loop that holds the ring near a 30 ms
target (clamped to ±1000 ppm — an inaudible pitch nudge for voice, 5 ms deadband). `swr_set_compensation`
returned success (supported by the resampler). A preliminary 2.5-min run kept `audio.wasapi` bounded
(~23–45 ms, mean ~34) with **0 drops** instead of the baseline march to 77 ms. A **10-min confirmation
run is pending** (results below when it lands). **By-ear validation for pitch/artifacts is still required
before this is defaulted on** — the measurement can prove the ring stops filling, but not the absence of
audible artifacts.

### 10-min confirmation (drift ON) — the fix holds

| minute | 0–2 | 2–4 | 4–6 | 6–8 | 8–10 | verdict |
|---|---|---|---|---|---|---|
| baseline (off) | 37 | 45 | 62 | 72 | **77** | monotonic runaway |
| **drift on** | 46 | 49 | **34** | **35** | **37** | **flat after settling** |

Stable stream (1 session), **0 `audio.queue` drops**. After a ~4-minute settling transient (one early
`max` excursion to ~98 ms as the compensation engages / swr re-inits), `audio.wasapi` **locks to ~35 ms
and stays flat** for the rest of the run — i.e. the ~67 ppm drift is cancelled, versus the baseline's
climb to 77 ms (and, extrapolated, to the ~200 ms click threshold by ~40 min). **The fix solves the
measured problem.**

Open items before default-on: (1) smooth the ~4-min startup transient (e.g. pre-set the resampler flag
so the first `swr_set_compensation` doesn't re-init, or ramp the gain); (2) **by-ear validation** that
the ±1000 ppm nudge is inaudible for voice; (3) wire to a real `--drift` flag / GUI toggle instead of
the env var; (4) confirm the same on the WebRTC/Opus path (needs a QR-paired session).

## Still valid / unaffected by this measurement
- **F-01** (signaling recv timeout) and **F-02** (USB tail-drop) — independent robustness fixes, kept.
- All **instrumentation** — kept; it just proved its worth by killing the flagship finding.
- Findings needing their own measurement remain open: **F-03** drift (needs ≥10 min; watch `audio.wasapi`
  trend), **F-08** loss recovery (needs induced loss), **F-18** UI-thread freeze, the Android-side items.

## What this says about method
An authoritative-sounding, "verified against the code" finding (S1/F-05) was **empirically false** — the
mis-belief was about FFmpeg's default `thread_count`. Reading the code and the header wasn't enough;
one A/B on hardware settled it in minutes. Treat every remaining *unmeasured* magnitude in
`performance-findings.md` with the same skepticism until a run confirms it.
