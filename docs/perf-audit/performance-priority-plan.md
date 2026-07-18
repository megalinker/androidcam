# PhoneCam — Prioritized Work Plan

Scores 0–5 (higher = more). **Value** blends latency/battery/CPU impact × confidence × breadth.
All items verified against code; magnitudes need the `performance-baseline.md` procedure.

## Tier 0 — correctness / robustness (do first, order as listed)

| ID | Finding | Lat | Batt | Win | Conf | QualityRisk | StabRisk | Effort | Test |
|---|---|---|---|---|---|---|---|---|---|
| F-01 | Signaling `recv` timeout (server wedge + shutdown hang) | 0 | 0 | 0 | 5 | 0 | 1 | 1 | 2 |
| F-03 | Resampler drift compensation (periodic clicks) | 1 | 0 | 0 | 4 | 2 | 2 | 3 | 4 |
| F-02 | USB `VideoQueue` tail-drop not oldest-drop | 2 | 0 | 0 | 5 | 1 | 1 | 1 | 2 |
| F-04 | Log the silent WASAPI guard-cap drop | 0 | 0 | 0 | 5 | 0 | 0 | 1 | 1 |

Deps: none. F-03 is the highest-effort/risk of the four (needs a long run to tune the correction
slew) — do it last in the tier and keep the correction small.

## Tier 1 — low-risk quick wins (one at a time, benchmark each)

| ID | Finding | Lat | Batt | Win | Conf | QualityRisk | StabRisk | Effort | Test | Deps |
|---|---|---|---|---|---|---|---|---|---|---|
| ~~F-05~~ | ~~H.264 `FF_THREAD_SLICE`~~ **REJECTED (measured: decode is single-threaded, ~1.8ms; no delay to remove)** | 0 | 0 | 0 | — | — | — | — | — | — |
| F-18 | adb chain off the WinForms UI thread | 0 | 0 | 0 | 5 | 0 | 2 | 3 | 2 | — |
| F-06 | `LOW_DELAY` on WebRTC decoders (parity) | 1 | 0 | 0 | 5 | 0 | 0 | 1 | 1 | bundle w/ F-05 |
| F-09 | WASAPI render thread MMCSS "Pro Audio" | 1 | 0 | 1 | 5 | 0 | 1 | 1 | 3 | — |
| F-07 | Split decode locks / video-only mutex | 2 | 0 | 1 | 5 | 0 | 2 | 2 | 3 | — |
| F-08 | Rate-limited PLI on decode error | 3 | 0 | 0 | 4 | 1 | 2 | 2 | 4 | — |
| F-10 | Sync frame on USB camera switch | 2 | 0 | 0 | 5 | 0 | 1 | 1 | 2 | — |
| F-13 | WebRTC start-bitrate (cut resolution ramp) | 2 | 1 | 0 | 4 | 2 | 2 | 2 | 3 | — |
| F-15 | USB encoder `KEY_PRIORITY=0` | 1 | 0 | 0 | 4 | 0 | 0 | 1 | 3 | — |
| F-16 | Android USB audio+enc thread priority | 1 | 0 | 0 | 4 | 0 | 1 | 1 | 3 | — |
| F-17 | Disable EIS on the USB Camera2 request | 2 | 0 | 0 | 3 | 1 | 1 | 1 | 3 | — |
| F-11 | Event-driven WASAPI + smaller buffer (**opt-in**) | 3 | 0 | 1 | 4 | 2 | 3 | 3 | 4 | ships as mode |
| F-12 | No-AEC/NS mic mode (**opt-in**) | 1 | 0 | 1 | 5 | 3 | 1 | 2 | 3 | ships as mode |
| F-14 | Wi-Fi `WIFI_MODE_FULL_LOW_LATENCY` (**opt-in**) | 2 | -2 | 0 | 4 | 0 | 1 | 1 | 3 | ships as mode |
| F-19 | Drain receiver stdout | 0 | 0 | 0 | 3 | 0 | 2 | 1 | 2 | — |
| F-20 | Android ABI filter (install size) | 0 | 0 | 0 | 5 | 0 | 1 | 1 | 2 | — |

**Batt = -2 for F-14** = it trades power for latency (that's the point; efficiency-aware/opt-in).

## Tier 2 — moderate / micro cleanups (batch when touching the file)

F-21 (encoder ByteArray reuse), F-22 (double-buffer preview copy), F-23 (meter subsample),
F-24 (pool receiver buffers / raw-PCM sink overload), F-25 (cheaper preview stretch),
F-26 (mic-only skips video factories), F-27 (A/V-sync stats + optional PTS align),
F-28 (GUI MoveWindow guard + log batching), F-29 (receiver LTO), F-30 (minimized FFmpeg — install size),
F-31 (CV disconnect wake). All Micro/Low — do opportunistically, not as standalone tasks.

## Tier 3 — experiments (measure before committing)

- **F-34** idle skip via softcam `IsConnected()` — best idle-power win; feasibility confirmed.
- **F-33** dedicated WebRTC video decode+sink thread (complete form of F-07).
- **F-32** conditional D3D11VA/DXVA2 hardware decode — gated on the softcam GPU→CPU readback cost.
- **F-35** in-band USB keyframe-request byte (needs phone-side reverse read).

## Suggested sequencing

1. **F-01, F-04, F-02** (fast, safe, correctness) → **F-05** (flagship, measure) → **F-18, F-09**.
2. Then **F-06/F-07/F-10/F-08** (WebRTC video quality/latency) → **F-15/F-16/F-17** (Android).
3. Ship the three **opt-in modes** (F-11, F-12, F-14) together with a single "ultra-low-latency /
   clean-mic" toggle so the default stays predictable.
4. **F-13** (start-bitrate) → **F-03** (drift, needs long-run tuning) → **F-20** (size).
5. Tier 2 as you touch files; Tier 3 only after their measurements justify the effort.

Rule (from the brief): benchmark after every meaningful change, never combine unrelated changes in one
benchmark, keep each independently revertible, ship hardware-dependent knobs as opt-in.
