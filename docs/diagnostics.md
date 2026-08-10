# PhoneCam diagnostics

Everything here exists to answer one question with evidence instead of intuition:

> Is the phone's battery drain the expected cost of capturing, encoding and transmitting video — or
> are we wasting power?

Diagnostics are **off by default**. Recording them costs a little battery itself (a 30 s sampler,
logging, and a `getStats()` collection), which would be self-defeating in a battery tool that was
always on. The one piece that *is* always on is the battery percentage sent to the PC: that is a
product feature, and it costs one sticky-intent read and one ~90-byte socket write per minute.

---

## 1. Turning diagnostics on

Three equivalent switches; use whichever fits:

| Where | How | Applies to |
|---|---|---|
| Phone app | **Diagnostics → "Record battery diagnostics"** | every session until you turn it off |
| Desktop app | **"Record phone diagnostics"** checkbox, then Connect | the next USB session |
| adb / scripts | `--ez diag true` (add `--ez deepDiag true` for 5 s sampling) | that one session |

Levels:

* **off** (default) — no sampler thread, no logging, no files, no `getStats()`, no counters. The hot
  paths read a single `volatile boolean` and do nothing else.
* **basic** — samples every 30 s. This is what you want for a battery run.
* **deep** — samples every 5 s and enables the verbose event set. A temporary developer mode; it
  measurably costs more, so don't use it for a drain measurement you intend to trust.

The switch takes effect at the **start of a stream**. Toggling it mid-session does nothing until you
stop and reconnect (the app says so).

---

## 2. Where the data goes

**Primary channel — logcat.** Every event and sample is written under the tag `PhoneCamDiag`:

```
adb logcat -d -s PhoneCamDiag:I
```

This always works: no permissions, no file access, and Android bounds the buffer for us.

**Durable copy — the app's own files.** `…/Android/data/com.phonecam/files/diag/session-<id>.log`,
capped at **256 KB per session** and **8 sessions** kept. Leaving diagnostics on cannot fill storage.

**Share from the phone** — Diagnostics → **Share diagnostics** sends the current session's events as
plain text to any share target (mail, notes, chat).

**PC side** — the receiver's stderr goes into the desktop app's log, so `[status]`, `[mark]`,
`[video]` and `[stats]` lines are all included in **Copy troubleshooting info**.

---

## 3. What gets recorded

### Events (milestones — never per frame)

`session_started`, `camera_prepared`, `camera_started`, `camera_size`, `camera_switched`,
`encoder_started`, `encoder_format`, `encoder_error`, `audio_started`, `transport_connected`,
`pc_connected`, `pc_disconnected`, `ice_state`, `status_channel_ready`, `keyframe_requested`,
`thermal_state_changed`, `battery_state_changed`, `wakelock_acquired`, `wakelock_released`,
`idle_autostop`, `problem_mark`, `webrtc_stats`, `session_summary`.

### Samples (every 30 s)

```
127400ms sample batt=71 chg=0 plug=0 battStatus=3 tempC=34.2 mV=3987 chargeUAh=3120000
         currentUA=-612000 energyNWh=n/a cpu=18.4% threads=42 capFps=30.0 encFps=29.8
         encKbps=4210.5 thermal=1 headroom=0.42 heapKB=24880 gcCount=31 uidTxKbps=4380.2
```

* **battery** — from the sticky `ACTION_BATTERY_CHANGED` broadcast plus `BatteryManager`
  properties. `chargeUAh` (the coulomb counter) is the trustworthy number; the percentage has 1-point
  granularity and is far too coarse for a 15-minute run.
* **cpu** — this process's own `utime+stime` from `/proc/self/stat`, as a percentage of one core.
* **thermal** — `PowerManager.getCurrentThermalStatus()` (API 29+) and `getThermalHeadroom()`
  (API 31+).
* **capFps / encFps / encKbps** — from the hot-path counters, i.e. what the pipeline *actually* did,
  not what it was asked to do.

### WebRTC statistics

On the Wi-Fi path the diagnostics do **not** reinvent network measurement — they read the stack's own
`RTCStatsReport` (`PeerConnection.getStats`) once per sample tick:

```
webrtc_stats out.video[bytes=… pkts=… frames=… key=… fps=… 1280x720 target=… limit=cpu
             enc=OMX.qcom.video.encoder.avc pli=3 nack=0 fir=0 encTime=…] rin[lost=12 frac=0.003
             jitter=0.004 rtt=0.011] pair[rtt=0.009 avail=5100000] capGeom=1920x1080
```

`enc=` is the definitive answer to "is the encoder hardware?", and `capGeom` vs `frameWidth/Height`
is the definitive answer to "are we capturing more pixels than we transmit?".

### Session summary

One line at the end of every session:

```
902341ms session_summary sid=6f2a91bc reason=stream_stopped durationS=902 battStart=88 battEnd=82
  battDelta=6 battPctPerHour=23.9 chargeDeltaUAh=284000 avgCurrentMA=1133.2 charged=0 tempMaxC=38.1
  thermalMax=1 cpuAvg=17.9% avgCapFps=29.9 avgEncFps=29.9 avgEncKbps=4180.2 camFrames=26991 …
```

`charged=1` means the phone spent part of the run on power — **the drain figures in that run are not
a measurement**, and the comparison script excludes it automatically.

### Values that are not available

Marked `n/a`, never estimated. `BATTERY_PROPERTY_CURRENT_NOW` and `ENERGY_COUNTER` are optional on
Android and their sign convention is not standardised across vendors; `getThermalHeadroom` returns
`NaN` on devices without the sensors. Absence of a number is reported as absence, not as zero.

### What is never logged

Frame or audio contents, the pairing secret, Wi-Fi credentials, SSIDs, IP addresses.

---

## 4. Running a battery benchmark

`tools/battery-session.ps1` runs one measured session end-to-end: reset counters → stream for a fixed
time → stop → collect → summarise.

```powershell
# Baseline: 15 min, static scene, Wi-Fi, phone unplugged.
.\tools\battery-session.ps1 -Transport wifi -Minutes 15 -Scenario static

# Same length, lots of motion.
.\tools\battery-session.ps1 -Transport wifi -Minutes 15 -Scenario motion

# The control that separates "the workload" from "our overhead": app connected, nothing streaming.
.\tools\battery-session.ps1 -Transport wifi -Minutes 15 -Scenario idle-connected -NoStream

# Quality comparison.
.\tools\battery-session.ps1 -Transport wifi -Minutes 15 -Scenario static -Quality HD_720P30
```

Each run writes `docs/perf-audit/runs/<timestamp>-<transport>-<scenario>/` containing `summary.json`,
`phonecam-diag.log`, `batterystats.txt`, `thermal.txt` and the before/after `dumpsys battery` dumps.

Then aggregate:

```powershell
.\tools\battery-compare.ps1
.\tools\battery-compare.ps1 -Scenario static
```

**Rules that make the numbers mean something**

1. **Wi-Fi only for drain.** USB charges the phone; a battery delta measured over the cable is
   meaningless. Use USB runs for pipeline metrics (fps, bitrate, drops) only. The script warns you.
2. **Repeat.** Battery percentage moves in 1-point steps. Three runs per scenario minimum, five is
   better. `battery-compare.ps1` refuses to look confident below three.
3. **One variable per comparison.** Same scene, same brightness, same room temperature, same screen
   state, similar starting charge and similar starting phone temperature.
4. **Start from a comparable thermal state.** A phone that is already warm throttles sooner and draws
   differently. Let it settle between runs.

---

## 4b. Decomposing the CPU cost (the mode sweep)

The first on-hardware session (Pixel 9 Pro XL, 2.8 h, 720p24 over Wi-Fi) measured **656 mA / ~2.5 W**
and, more interestingly, **~54 % of one CPU core sustained**. That is far more CPU than a
Surface→hardware-encoder pipeline should need — the encoder was confirmed hardware
(`enc=c2.exynos.h264.encoder`) — so something is doing per-frame or per-packet work that isn't
obviously necessary.

Two candidates, both confirmed present in the code but **not** yet confirmed as the cost:

* **Software audio processing.** `createAudioSource(MediaConstraints())` leaves libwebrtc's APM
  (AEC3, noise suppression, AGC, high-pass) enabled. The "raw mic" option disables the *hardware*
  AEC/NS on the audio device module but not this. A phone used as a standalone remote mic has no
  local playback to echo-cancel.
* **Pixel rotation before encode.** The camera delivered 1280×720 but the encoder emitted 720×1280,
  so the rotation is baked into the frames. libdatachannel never offers the
  `urn:3gpp:video-orientation` (CVO) extension, so the rotation cannot be signalled in RTP and has to
  be applied to pixels — which can force a texture→I420 conversion per frame.

**The experiment that tells them apart — no code changes, ~20 minutes.** Run three sessions and
compare `cpu=` and `currentUA` in the sample lines:

| Run | Phone mode | Isolates |
|---|---|---|
| 1 | Cam + Mic | the baseline (~54 %) |
| 2 | Camera only | video pipeline alone |
| 3 | Mic only | audio pipeline alone |

Five minutes each is plenty — the numbers are steady within a couple of samples. Keep the scene,
lighting and room temperature identical, stay on Wi-Fi, and stay unplugged.

Reading it:

* **Mic-only lands at 20–30 %** → the APM is the cost. The fix is to pass explicit constraints
  turning AEC/NS/AGC off, gated behind the existing raw-mic flag so the default sound is unchanged.
* **Camera-only carries most of it** → the rotation is the cost. The fix is to stop rotating on the
  phone and use the receiver's existing rotate control instead (PC-side rotation is nearly free —
  `video_sink.cpp` already does it).
* **Both are substantial** → they are additive and both fixes apply.

Either fix must then be re-measured the same way before it is kept: the point is a lower `currentUA`
at the same `capFps`/`encKbps`, not a lower CPU number on its own.

---

## 5. Platform profilers (independent of our telemetry)

Our own numbers say what the app thinks it is doing. These say what the device measured.

**Perfetto power trace** — `tools/power-trace.ps1`:

```powershell
.\tools\power-trace.ps1 -Seconds 120
```

Records battery counters, CPU frequency/idle, and — where the hardware has an on-device power monitor
— per-subsystem **power rails**. Open the result at <https://ui.perfetto.dev>.

> Power rails require ODPM hardware, present on **Pixel 6 and later**. On other devices the rail
> tracks are simply absent. The script probes for it and says which case you are in, because
> "no rail data" must never be read as "no power used".

**Android Studio Power Profiler** reads the same ODPM source with a UI — use it for exploration, and
the script for repeatable runs.

**Batterystats / Battery Historian** — `battery-session.ps1` already captures
`dumpsys batterystats` after each run. For the full Historian input:

```
adb shell dumpsys batterystats --reset
# … stream, unplugged …
adb bugreport bugreport.zip
```

Note that Google no longer actively maintains Battery Historian; system tracing and the Power
Profiler are the current recommendations.

**Thermal** — `adb shell dumpsys thermalservice` (also captured per run).

---

## 6. The "Mark video problem" marker

When you *see* a glitch, press **Mark video problem** in the desktop app. No video is recorded — the
point is a synchronised timestamp with the state that explains it.

The receiver immediately writes:

```
[mark] note="video artifact seen at 14:32:09.412" rtp_recv=184213 rtp_lost=37 (0.020%) gaps=6
       reorder=2 dup=0 incompleteFrames=6 decodeErrFrames=4 pli=5 pliSuppressed=1 vframes=8912
```

and forwards the mark to the phone, which stamps its own event stream at the same moment. Afterwards
you can line up the two logs and see what the seconds before the glitch looked like on both sides.

Over USB the marker reports the transport-side equivalents (`queueDrops`, `damagedFrames`).

The mark is also available without the GUI — write `mark <note>` to `receiver.exe`'s stdin.

---

## 7. Receiver-side instrumentation

Separate from the phone, and separately gated. Set `PHONECAM_STATS=1` before launching
`receiver.exe` for ~1 Hz aggregated latency/queue statistics (`video.decode`, `video.sink`,
`audio.wasapi`, queue depths and drops). See `docs/perf-audit/measure-howto.md`.

Always on, because they are a single `printf` at teardown rather than instrumentation:

* `[status] battery=… charging=… tempC=… sid=…` — one per phone heartbeat (~1/min)
* `[webrtc] session ended … recv=… lost=… gaps=… incompleteFrames=… decodeErrFrames=… pli=…`
* `[usb] session ended … queueDrops=… damagedFrames=… keyframeAsks=…`
* `[video] keyframe requested (rtp-gap | decode-error)` — only when recovery actually fired

---

## 8. Overhead of the diagnostics themselves

| Mode | Cost |
|---|---|
| off (default) | one volatile read on hot paths; no thread, no I/O, no allocation |
| basic | one background wakeup / 30 s; ~15 cheap reads and one log line per tick; one `getStats()` per tick on Wi-Fi; ≤256 KB of file writes per session |
| deep | as basic, 6× the cadence, plus verbose events — **do not** use it for a drain measurement |
| battery → PC (always on) | one sticky-intent read and one ~90-byte write per minute |

To confirm the overhead rather than trust the table, run the same scenario with the switch off and on
and compare — that is exactly what `battery-compare.ps1` is for.
