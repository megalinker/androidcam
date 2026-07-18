# PhoneCam — Performance Audit Executive Summary

**Scope:** whole-codebase read (Android ×6 Kotlin, Windows receiver ×7 C++, 1049-line C# GUI, CMake,
manifests, CI/packaging) + a 51-candidate finding set, each **independently verified against the
code** (35 Confirmed, 8 Plausible, 5 Rejected). **No live phone/receiver/profiling hardware was
available**, so magnitudes are estimates with a measurement procedure attached (`performance-baseline.md`),
never claimed results.

> **⚠️ Corrected after on-hardware measurement — see `measured-results.md`.** Finding #1 below (F-05)
> was **DISPROVEN**: on a Pixel 10 Pro + 20-core PC the decoder is `thread_count=1,
> active_thread_type=0` (single-threaded), decode ≈ **1.8 ms**, and a frame-vs-slice A/B was identical.
> FFmpeg defaults `thread_count` to 1 — it does *not* auto-multithread — so there was no latency to
> remove; the fix was reverted. Measurement also showed the whole PC pipeline is lean (video
> decode+convert ≈ 3.5 ms, audio standing latency ≈ 20 ms, zero queue drops), so **the receiver is not
> where glass-to-glass latency accumulates** — it's the phone (capture/encode) + softcam→consumer.

## Dominant current bottlenecks (original assessment; #1 since refuted)

1. ~~**H.264 decode frame-threading latency.**~~ **REJECTED by measurement** (see box above). Decode was
   already single-threaded and fast (~1.8 ms). A code-verified, authoritative-sounding finding turned
   out empirically false — the audit process catching itself. *(F-05)*
2. **Robustness gaps that hang the tool, not just slow it.** The WebRTC signaling accept blocks in
   `recv()` with no timeout on a single-threaded, backlog-1 server → a half-open peer wedges it and
   even Ctrl-C can't interrupt it (F-01). The USB Start / camera-switch runs the whole adb chain
   **synchronously on the WinForms UI thread** → the window goes "Not Responding" for 1–35 s (F-18).
3. **Audio integrity on long calls.** No resampler drift compensation → the phone-vs-PC clock offset
   walks the drop-oldest queue to its edge → periodic clicks / dropped 20 ms chunks (F-03); the WASAPI
   render path can also drop audio silently on an endpoint stall (F-04).
4. **Video error recovery.** No mid-stream keyframe request on decode error → up to a full GOP of
   corruption after packet loss on Wi-Fi (F-08); on USB overflow the queue drops the **oldest** NAL
   (worst choice) instead of the newest (F-02).

## Best immediate opportunities (low risk, high value)

- ~~F-05 decode threading~~ — **rejected by measurement** (decode was already single-threaded/fast).
- **F-01** `SO_RCVTIMEO` on the accepted signaling socket — removes a real wedge + un-interruptible shutdown. **(now the top confirmed win; applied + compile-verified)**
- **F-18** adb chain off the UI thread — removes the Start/switch "Not Responding" freeze.
- **F-09** register the WASAPI render thread with MMCSS "Pro Audio" — pure-win default, fewer underruns.
- **F-02** USB `VideoQueue` tail-drop instead of oldest-drop — strictly better failure mode, one edit.

## Best latency opportunities

**Measurement moved the target.** The PC receiver is already lean (decode ≈ 1.8 ms, convert ≈ 1.7 ms,
audio standing ≈ 20 ms), so PC-side latency findings (F-05 rejected; F-07, F-11) are small. The
remaining latency lives **on the phone and in delivery**, so the highest-value latency work is now:
F-13 (WebRTC start-bitrate to cut the resolution ramp), F-17 (disable EIS on affected phones), F-15/F-16
(Android encoder/thread hints), F-08/F-02 (faster loss recovery). Confirm each with the measurement
harness (`measure-howto.md`) + external glass-to-glass before investing.

## Best battery opportunities

The battery story is smaller than the latency story, and the audit **rejected the biggest battery
claims** (longer GOP, High-profile) as non-issues (see rejected table). The real levers:
- **F-34** (Tier 3): skip color-convert/transform/softcam-push (not decode) when `IsConnected()==false`
  and preview hidden — idle-power win for the always-on auto-listen/tray scenario (softcam exposes
  `IsConnected`, confirmed).
- **F-14** optional Wi-Fi low-latency lock (a power *cost* for latency — efficiency-aware).
- **F-12** optional no-AEC/NS mic mode (mostly a quality win, minor CPU).
- Note the USB path phone is cabled/charging, so phone-battery findings there are near-moot; battery
  effort belongs on the **Wi-Fi** path.

## Best Windows-resource opportunities

Mostly Micro (the pipeline is already lean — bounded queues, contexts reused, no per-frame swscale/swr
rebuild). F-34 (idle skip), F-22 (double-buffer the preview copy), F-25 (cheaper preview stretch),
F-24 (pool receiver buffers / raw-PCM sink overload), F-29 (LTO). F-32 (hardware decode) is a Tier-3
experiment gated on the softcam GPU→CPU readback cost.

## Largest unknowns (need hardware)

- ~~The magnitude of F-05~~ — **resolved: no effect** (decode is single-threaded; see `measured-results.md`).
  The true glass-to-glass / mouth-to-mic totals still need an external method (high-FPS video / `latbench`).
- Whether F-03 drift is audible on the operator's specific phone/PC clock pair over ≥10 min.
- Per-device EIS (F-17) and VOICE-path band-limiting (F-12) behavior.
- Whether F-19's stdout pipe ever actually fills (libdatachannel is a prebuilt dependency).

## Key architectural conclusions

The architecture is **sound and constraint-respecting** — all queues are bounded drop-oldest, contexts
are reused, the audio thread is correctly decoupled from the network thread, and the fixed-resolution
softcam decision is right. There is **no case for a rewrite or a transport change**; every worthwhile
win is an incremental, individually-revertible edit. Measurement also showed the PC receiver is already
lean, so the remaining latency is on the **phone (capture/encode)** and **softcam→consumer delivery** —
the audit's PC-side latency findings are smaller than first assessed.

## Recommended implementation order

Tier 0 correctness (**F-01 applied**, F-02 applied, then F-03/F-04) → Tier 1 quick wins, **one at a
time, measured** (F-18, F-09, F-07, F-08, F-10, F-13..F-17) → Tier 2 batch cleanups → Tier 3
experiments only after their measurements justify them. **F-05/F-06 rejected by measurement.** Do
**not** combine unrelated changes in one benchmark. Every change stays behind an easy revert;
hardware-dependent knobs (F-11, F-14, F-12) ship as opt-in modes so the default stays predictable.
