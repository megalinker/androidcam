# PhoneCam — How to measure (with just your phone)

The receiver now has flag-gated, single-clock instrumentation. It's **off** unless `PHONECAM_STATS`
is set, and it never logs per-frame (aggregates p50/p95/max/mean to stderr ~once a second). All
numbers are **PC-internal deltas** (steady_clock/QPC) — no phone↔PC clock calibration needed. For the
true cross-device totals (glass-to-glass video, mouth-to-mic audio) you still need an external method
(high-speed camera / `latbench`); everything else is covered here.

## 0. Prerequisites
- Receiver built (done): `windows/build-webrtc/Release/receiver.exe`.
- **USB path:** phone with USB-debugging authorized (`adb devices` shows `device`).
- **Wi-Fi path / battery tests:** phone on the same Wi-Fi. (You **can't** measure phone battery over
  USB — the cable charges it; use Wi-Fi for battery/thermal.)
- **Mic latency:** VB-CABLE installed (only for `latbench`).
- Bundled `adb` is under `dist/PhoneCam/bin/adb/adb.exe` (or your platform-tools).

## 1. The `[stats]` lines you'll see
```
[stats] video.decode   n=.. p50=..ms p95=..ms max=..ms mean=..ms   <- packet submit -> its decoded frame (THE F-05 metric)
[stats] video.sink     n=.. p50=..ms ...                            <- sws_scale + rotate/flip + softcam push
[stats] video.queue    depth_max=.. drops=..                        <- USB VideoQueue backlog + overflow drops
[stats] audio.e2e      n=.. p50=..ms ...                            <- (WebRTC) Opus arrival -> WASAPI submit
[stats] audio.sink     n=.. p50=..ms ...                            <- resample + EQ/gain + render, per chunk
[stats] audio.swr      n=.. p50=..ms ...                            <- samples buffered in the resampler
[stats] audio.wasapi   n=.. p50=..ms ...                            <- standing WASAPI render latency (F-11; watch it rise = F-03 drift)
[stats] audio.queue    depth_max=.. drops=..                        <- audio queue backlog + drops
```
The desktop app forwards receiver stderr to its log, so these also land in **Copy diagnostics**.

## 2. Video decode / pipeline (F-05 was measured here — and rejected)
> **Already done on a Pixel 10 Pro + 20-core PC (see `measured-results.md`):** decode is
> `thread_count=1, active_thread_type=0` (single-threaded), `video.decode` ≈ **1.8 ms**, `video.sink`
> ≈ 1.7 ms, `video.queue depth_max=1 drops=0`. FFmpeg does **not** auto-multithread, so F-05's
> frame-threading latency never existed and the fix was reverted. The `PHONECAM_DECODE_THREADING` knob
> was removed. Decode is **not** the bottleneck.

To re-check on your own hardware / resolution, just read `[stats] video.decode` and `video.queue`:
```powershell
$adb="C:\Users\J2p99\Downloads\TEMPP\phonecam\dist\PhoneCam\bin\adb\adb.exe"
$rx ="C:\Users\J2p99\Downloads\TEMPP\phonecam\windows\build-webrtc\Release\receiver.exe"
& $adb forward tcp:27183 tcp:27183
& $adb shell am start -n com.phonecam/.MainActivity -a com.phonecam.action.USB --ei usbPort 27183 --es mode BOTH --es quality FHD_1080P30
$env:PHONECAM_STATS="1"
& $rx --usb --usb-port 27183       # watch [stats] video.decode / video.queue, Ctrl-C after ~60s
```
The `[stats] h264 decode: thread_count=… active_thread_type=…` line prints the resolved threading at
start. **If (and only if) 4K on a weak PC shows `video.queue drops>0`**, decode can't keep up — that's a
*throughput* fix (`thread_count=0` + `FF_THREAD_SLICE`), not a latency one; measure before applying.

For the **true glass-to-glass** number (camera→app), the receiver stats can't help — it's dominated by
phone capture/encode + softcam→consumer. Use a high-speed camera (a second phone's 240 fps) filming a
ms-clock the phone sees next to the PC preview, or read the consuming app's own stats.

## 3. Mic path (PC-internal breakdown)
`latbench` gives the total *acoustic* mouth→CABLE number; these stats decompose the **PC side** of it
so you can see where the mic latency sits and whether F-11/F-03 changes help — no acoustic rig needed:
- `audio.wasapi` — the standing render latency (this is what the 200 ms buffer / F-11 governs).
- `audio.swr` — resampler contribution.
- `audio.sink` — per-chunk processing cost.
- `audio.e2e` (WebRTC) — arrival→submit inside the receiver.
- `audio.queue depth/drops` — backlog and the drop-oldest events behind F-03 clicks.

**Drift (F-03):** run a long call (≥10 min) and watch `audio.wasapi` and `audio.queue`. A steadily
**rising** `audio.wasapi` (or periodic `audio.queue drops`) = phone-vs-PC clock drift — the thing F-03
compensation would flatten.

**Total acoustic mic latency (latbench):** phone streaming mic into CABLE, phone mic near the PC
speakers, volume up, quiet room:
```powershell
& "C:\Users\J2p99\Downloads\TEMPP\phonecam\windows\build-webrtc\Release\latbench.exe"   # if built (WITH_LATBENCH)
# tool self-check (no phone): latbench.exe --selftest
```

## 4. CPU / GPU / memory / battery
- **PC CPU/GPU/mem:** Task Manager while streaming; for a rigorous trace use WPR/WPA (ETW). Compare
  preview-visible vs minimized vs no softcam consumer to isolate preview cost (F-22/F-25).
- **Phone (Wi-Fi path only):** charge to full, unplug, then:
  ```
  adb shell dumpsys batterystats --reset      # before
  # stream N minutes
  adb shell dumpsys batterystats > after.txt  # or use Battery Historian
  adb shell dumpsys thermalservice            # thermal state
  ```
  Read battery % delta over a fixed duration; keep screen state/brightness/ambient constant between runs.

## 5. Rules
One variable per run; ≥5 repetitions; report p50/p95 (not just mean); same scene/thermal state for
before/after; never subtract a phone timestamp from a PC timestamp. Fill results into the table in
`performance-experiments.md`.
