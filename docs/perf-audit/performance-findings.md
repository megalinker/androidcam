# PhoneCam — Performance Findings

Every finding below was **independently verified against the code** (a separate reviewer re-read the
cited file, confirmed the mechanism, and checked the proposed fix against the non-negotiable
constraints). Verdicts: **Confirmed** (code fact, mechanism holds), **Plausible** (code consistent,
runtime magnitude unmeasured), **Rejected** (code contradicts the claim). No live hardware was
available, so all *magnitudes* are estimates pending the `latbench`/instrumentation procedure in
`performance-baseline.md`. Severities are the reviewer-corrected, honest values — several seed claims
were **downgraded** after verification, which is noted inline.

Legend: **Sev** = corrected severity · **Conf** = confidence · **T** = transport · rows ordered by tier then severity.

## Implementation status (working tree, uncommitted, all compile-verified)

| Finding | State | Verified |
|---|---|---|
| F-01 signaling recv timeout | ✅ applied | MSBuild |
| F-02 USB tail-drop | ✅ applied | MSBuild |
| F-03 drift compensation (opt-in `PHONECAM_DRIFT`) | ✅ applied | MSBuild **+ 10-min on-hardware run** (holds ~35 ms flat) |
| F-07 split decode locks (audio/video) | ✅ applied | MSBuild |
| F-08 rate-limited PLI on decode error | ✅ applied | MSBuild |
| F-09 MMCSS "Pro Audio" on render thread | ✅ applied | MSBuild |
| F-10 sync-frame on USB camera switch | ✅ applied | Gradle |
| F-15 encoder `KEY_PRIORITY=0` | ✅ applied | Gradle |
| F-16 Android audio/encoder thread priority | ✅ applied | Gradle |
| F-17 disable EIS on the USB capture request | ✅ applied | Gradle |
| F-18 adb chain off the WinForms UI thread | ✅ applied | csc |
| F-19 drain receiver stdout | ✅ applied | csc |
| F-13 WebRTC start-bitrate (cut resolution ramp) | ✅ applied | Gradle |
| F-20 Android ABI filter (arm-only, ~half APK) | ✅ applied | Gradle |
| F-22 preview double-buffer (drop redundant copy) | ✅ applied | MSBuild |
| F-28 GUI MoveWindow size-guard | ✅ applied | csc |
| ~~F-11 event-driven WASAPI~~ | ❌ **applied then REMOVED** — measured no-win (25→28.8 ms; audio already at the floor), not worth the added complexity | — |
| F-12 raw-mic / no-AEC (opt-in GUI checkbox → `--ez rawMic`) | ✅ applied | Gradle + csc |
| F-34 idle skip when no camera consumer (softcam `IsConnected` + preview-visible signal) | ✅ applied | MSBuild (no-softcam path); softcam `#ifdef` line inspection-verified |
| Instrumentation (`stats.h`, `PHONECAM_STATS`) | ✅ applied | MSBuild + on-hardware |
| F-05 / F-06 decode threading | ❌ **rejected by measurement**, reverted | — |

**Opt-in env/UI flags now available:** `PHONECAM_STATS=1` (instrumentation), `PHONECAM_DRIFT=1` (audio drift
comp, F-03); GUI "Raw mic (no AEC)" checkbox (F-12). (F-11's `PHONECAM_LOWLATENCY_AUDIO` was removed — measured no-win.)

_Declined per the brief's "no speculative micro-changes" rule (unmeasurable gain vs regression risk):_
_F-23 (meter subsample), F-24 (buffer pooling), F-29 (LTO — FFmpeg is in DLLs), F-31 (CV disconnect wake)._

**Runtime-confirmed on a Pixel 10 Pro (USB):** decode ~1.8 ms single-threaded (F-05 dead), F-03 drift real
(+40 ms/10 min) and its fix holds ~35 ms flat. See `measured-results.md`.

Not yet implemented (opt-in modes / install-size / micro / experiments): F-11 (event-driven WASAPI —
low value: measured standing latency only ~20 ms), F-12 (no-AEC mic mode), F-13 (WebRTC start-bitrate),
F-14 (Wi-Fi lock), F-20 (ABI filter), Tier-2 micros, Tier-3 experiments (F-34 idle skip is the pick).

---

## Tier 0 — correctness / robustness (fix first)

### [F-01] Signaling accept blocks forever on a half-open peer (server wedge + un-interruptible shutdown)
- **Category:** Stability · **Sev:** Medium · **Conf:** Confirmed · **Transport:** WebRTC · **Track:** Both
- **Code:** `windows/src/webrtc_receiver.cpp:146-148, 269-274, 299-324` (verify id `N-win-transport-threads-0`)
- **Current behavior:** `run_webrtc_session` listens with backlog 1 and runs `handleConnection` **synchronously** from the accept loop. The accepted `cli` socket has **no `SO_RCVTIMEO`** (only `srv` gets a 300 ms `select` tick). `recvAll` does a blocking `recv()`.
- **Evidence:** A peer that connects and sends <5 bytes (port scanner, stale/duplicate connection) parks the single server thread in `recvAll` at line 147 forever; a phone that pairs then dies without FIN/RST (power loss / partition) blocks at line 269. With backlog 1 and no concurrency, **no later phone is ever served**, and because `*running` is only polled in the `select`/100 ms loops, **Ctrl-C/Stop cannot wake a thread parked in `recv()`** — shutdown hangs too.
- **Mechanism:** blocking `recv` with no timeout on a single-threaded server = head-of-line wedge; recovery only via process kill.
- **Proposed change:** `setsockopt(cli, SO_RCVTIMEO, ~15-20 s)` right after `accept()`. On `WSAETIMEDOUT`, `recv` returns ≤0 → `recvAll` returns false → `handleConnection` aborts to the accept loop where `*running` is re-checked. Generous timeout so a real (ICE-gathering ≤6 s) pairing is never aborted.
- **Expected benefit:** eliminates the wedge; bounds shutdown latency to the timeout. *Unverified on hardware* (behavioral, code-verifiable).
- **Regressions:** if the timeout were set too tight it could abort a slow-but-legit pairing → keep it ≥15 s (ICE gather caps at 6 s on the phone).
- **Validation:** `nc <pc> 8891` and send nothing → server must recover and still accept a real phone; kill the phone mid-negotiation → server returns to listening within the timeout; Ctrl-C while a peer is half-open → exits within the timeout.
- **Rollback:** remove the `setsockopt`. · **Constraint check:** signaling-only; DTLS-SRTP/pairing untouched.

### [F-02] USB `VideoQueue` overflow drops the **oldest** NAL → ~1 s of corruption to the next IDR
- **Category:** Stability/Video · **Sev:** Medium · **Conf:** Confirmed · **Transport:** USB · **Track:** Camera
- **Code:** `windows/src/usb_receiver.cpp:62-74, 97-112` (verify id `N-win-transport-threads-2`)
- **Current behavior:** on overflow (≥120 units, ~4 s) `push` does `pop_front()` — sheds the **oldest** queued access unit, i.e. the one the decoder is about to consume. USB is lossless TCP, so this queue is the *only* place video is ever dropped, and there is **no keyframe request** back to the phone.
- **Evidence:** dropping a mid-GOP NAL breaks inter-frame prediction → macroblock/green corruption until the next IDR (~1 s at the phone's 1 s GOP); if the shed unit is the IDR (or its prepended SPS/PPS) recovery waits a full extra GOP. Overflow-gated (needs a sustained ~4 s backlog), so not steady-state.
- **Mechanism:** oldest-drop is the worst unit to drop for a reference-coded stream.
- **Proposed change (cheap, no phone change):** on overflow drop from the **tail** (newest) instead, so the decoder keeps a contiguous run and the buffered IDR chain stays intact. Better (more work): drop whole GOPs to the next IDR; best: add a lightweight in-band keyframe-request byte on the adb socket (two-sided).
- **Expected benefit:** shorter/no corruption window on the rare overflow. *Unverified on hardware.*
- **Regressions:** tail-drop discards newest frames (adds a little latency during the stall) instead of corrupting — a strictly better failure mode. Queue stays bounded.
- **Validation:** force overflow (throttle decode / 4K on a weak PC) and observe corruption duration before/after.
- **Rollback:** revert to `pop_front`. · **Constraint check:** stays bounded (no unbounded queue); no resolution change.

### [F-03] No resampler drift compensation → periodic audible click / dropped chunk on long calls
- **Category:** AudioLatency/Stability · **Sev:** Medium · **Conf:** **Confirmed on hardware** · **Transport:** Both · **Track:** Microphone · **Status:** fix implemented (opt-in `PHONECAM_DRIFT`)
- **📈 Measured:** `audio.wasapi` climbed 37→45→62→72→77 ms over a stable 10-min stream (Pixel 10 Pro, ≈67 ppm; +40 ms/10 min); 0 drops (ring hadn't hit its ~200 ms ceiling). **Confirmed real.** Fix (proportional `swr_set_compensation` to a 30 ms target, ±1000 ppm, opt-in `PHONECAM_DRIFT`) **confirmed over 10 min**: after a ~4-min settling transient it locks to ~35 ms flat (vs baseline's runaway to 77), 0 drops. **Needs by-ear validation + a real flag before default-on.** See `measured-results.md`.
- **Code:** `windows/src/wasapi_sink.cpp` (drift loop added); resampler `:265-269`; queues `usb_receiver.cpp:38-45`, `webrtc_receiver.cpp:100-108` (verify id `N-win-audio-1`)
- **Current behavior:** the resampler is built once (`swr_alloc_set_opts2`/`swr_init`) at **nominal** rates and only converts format/rate/layout — **no `swr_set_compensation`**, no async path. `WriteFrame` paces to the WASAPI clock, so the phone-crystal-vs-PC-crystal offset (tens–hundreds of PPM) walks the feeding queue depth to the drop threshold.
- **Evidence:** phone faster than PC → a whole ~20 ms chunk is eventually dropped (audible click); phone slower → the queue empties toward underrun. Nothing closes the clock loop. Grep confirms `swr_set_compensation` is never called.
- **Mechanism:** fixed-ratio resampling can't absorb independent-clock drift; drop-oldest queues turn it into periodic clicks.
- **Proposed change:** track render-buffer / queue fill vs a target depth and apply a tiny continuous `swr_set_compensation` (a few samples/s) to null steady-state drift, so the queue sits near target instead of walking to the drop edge.
- **Expected benefit:** removes periodic clicks on multi-minute calls; keeps A/V lip-sync from slowly sliding. *Unverified — needs a long (≥10 min) run to observe.*
- **Regressions:** over-aggressive compensation adds pitch wobble — keep the correction small and slew-limited.
- **Validation:** ≥10 min tone into CABLE; count discontinuities and measure queue-depth trend before/after.
- **Rollback:** drop the compensation call. · **Constraint check:** doesn't drop audio quality (it *reduces* drops); no unbounded buffering.

### [F-04] WASAPI render guard-cap drops audio **silently** on an endpoint stall
- **Category:** Stability · **Sev:** Low · **Conf:** Confirmed · **Transport:** Both · **Track:** Microphone
- **Code:** `windows/src/wasapi_sink.cpp:349-361` (verify id `N-win-audio-2`)
- **Current behavior:** the render loop `Sleep(2)`-polls; on sustained fullness it exits after `guard++ < 1000` (~2 s) with `written < got`, **returns true, and discards the unwritten tail with no log**. Triggers on device switch / format renegotiation / an exclusive-mode grab.
- **Evidence:** `if (avail==0){ Sleep(2); continue; }` bounded by `guard`; on cap it silently drops. (The dedicated audio thread means this does **not** stall SRTP — corrected down from the seed's "starves network".)
- **Proposed change (cheap half):** log/surface when the guard trips instead of dropping silently, so drift/underrun handling can react. (Full fix = event-driven WASAPI, see F-11.)
- **Expected benefit:** observability of a currently-invisible audio-loss path. · **Rollback:** remove the log.
- **Constraint check:** satisfies "don't silently drop audio quality." · *Unverified magnitude; correctness of the log is code-evident.*

---

## Tier 1 — low-risk quick wins

### [F-05] ~~H.264 decoders run default frame-threading → standing decode latency~~ — **REJECTED (measured)**
- **Category:** VideoLatency · **Sev:** ~~High~~ **none** · **Conf:** **Disproven on hardware** · **Transport:** Both · **Track:** Camera
- **⛔ Measurement (Pixel 10 Pro USB 720p30, 20-core PC) refuted this — see `measured-results.md`.** The decoders resolve to `thread_count=1, active_thread_type=0` (single-threaded); frame vs slice A/B were identical (~1.83 ms p50). **FFmpeg defaults `thread_count` to 1 — it does NOT auto-pick `cpu_count`** — so `FF_THREAD_FRAME` was never active and there was no latency to remove. The verify-by-reading claim (S1) was wrong. The F-05/F-06 code was reverted (no-op). *Kept below for the record.*
- **Code:** `windows/src/usb_receiver.cpp:90-94`, `windows/src/webrtc_receiver.cpp:240-242` (verify id `S1`)
- **~~Current behavior:~~ (false premise):** the claim was that neither H.264 context sets `thread_count`/`thread_type`, so `avcodec_open2` auto-picks `thread_count=av_cpu_count()` with `FF_THREAD_FRAME` active — **this does not happen**; the default is single-threaded.
- **Evidence (authoritative):** the bundled `libavcodec/avcodec.h:1583` states *"Use of FF_THREAD_FRAME will increase decoding delay by one frame per thread."* Frame threading keeps `thread_count-1` frames in flight, so `avcodec_receive_frame` returns output a standing `thread_count-1` frames behind the newest submitted packet — a continuous pipeline delay, **independent of B-frames** (encoder sets `KEY_MAX_B_FRAMES=0`). On an 8-core PC that is ~7 frames ≈ **~230 ms @30fps**. `AV_CODEC_FLAG_LOW_DELAY` (set only on the USB decoder) collapses the *reorder* buffer but does **not** remove frame-thread delay.
- **Mechanism:** frame-thread pipeline latency scales with the PC's core count.
- **Proposed change:** set `ctx->thread_type = FF_THREAD_SLICE;` on both H.264 contexts before `avcodec_open2` (slice threading emits each frame immediately and still parallelizes multi-slice frames; `thread_count=1` also works but serializes decode). Add `AV_CODEC_FLAG_LOW_DELAY` on the WebRTC decoder too (see F-06).
- **Expected benefit:** removes a standing multi-frame decode latency on **both** paths — the single biggest glass-to-glass latency lever found. Estimate ~1–7 frames saved depending on PC cores; **must be measured** (decode-submit→`receive_frame`, and end-to-end via §3 of the baseline).
- **Regressions:** for a single-slice stream, slice threading effectively single-threads decode — verify 4K USB still decodes real-time on the weakest target PC (720p/1080p are trivial). If not, allow a small `thread_count` with `FF_THREAD_SLICE`.
- **Validation:** instrument decode-submit→output latency; A/B `FF_THREAD_SLICE` vs default on 8-core and 4-core PCs at 720p/1080p/4K; confirm no frame-drop increase.
- **Rollback:** delete the `thread_type` line. · **Constraint check:** no resolution/transport/DTLS impact; default behavior only gets lower-latency.

### [F-06] WebRTC decoders miss `AV_CODEC_FLAG_LOW_DELAY` (USB sets it; parity) — **reverted with F-05**
- **Category:** VideoLatency · **Sev:** Micro · **Conf:** Confirmed (but ~0 effect) · **Transport:** WebRTC · **Track:** Camera
- **Note:** bundled with the F-05 edit and reverted after measurement showed decode is already single-threaded and fast (~1.8 ms). LOW_DELAY only matters if a stream signals reorder frames; the phone's no-B-frame stream doesn't, so the effect is ~0. Re-add only if a future stream shows reorder buffering. Kept below for the record.
- **Code:** `windows/src/webrtc_receiver.cpp:241-242` (verify id `S2`)
- **Current behavior:** USB H.264 decoder sets `LOW_DELAY` (`usb_receiver.cpp:93`); WebRTC sets it on neither decoder.
- **Evidence:** on a constrained-baseline no-B-frame phone stream (`max_num_reorder_frames=0`) output is usually already immediate, so the real latency saved is ~0 — but it's a free correctness/parity fix that guarantees no reorder buffering if the VUI ever lacks the hint.
- **Proposed change:** `decCtxV->flags |= AV_CODEC_FLAG_LOW_DELAY;` before `avcodec_open2` (bundle with F-05). LOW_DELAY on Opus is a harmless no-op — skip it.
- **Expected benefit:** parity guarantee; likely ~0 ms on typical streams. · **Rollback:** remove the flag. · **Constraint check:** none affected.

### [F-07] One `decodeMutex` serializes Opus audio behind H.264 **decode + pixel work**
- **Category:** AudioLatency/AVSync · **Sev:** Low · **Conf:** Confirmed · **Transport:** WebRTC · **Track:** Both
- **Code:** `windows/src/webrtc_receiver.cpp:181, 217, 250-262` (verify ids `S3`, `N-win-video-1`, `N-win-transport-threads-1`)
- **Current behavior:** the single `decodeMutex` is held by the audio `onMessage` **and** the video `onFrame` — and on the video side it's held across `videoSink.WriteFrame` (sws_scale + rotate/flip + softcam push + preview copy, several ms at 1080p), even though the two paths share **no non-atomic state** (`decCtx`/`fq` vs `decCtxV`/`videoSink`; only `rtpCount`/`disconnected`, which are atomic).
- **Evidence:** an Opus packet (~every 20 ms) can't be depacketized/decoded/enqueued while a video frame is being color-converted/blitted → audio bunches into bursts (jitter), not steady starvation (audio is decoupled by `FrameQueue` + a separate render thread, so severity is Low, not High).
- **Proposed change:** give video its own `videoDecodeMutex` and audio its own (or none for the shared lock); at minimum move `videoSink.WriteFrame` **out** of the lock — but **only** together with a dedicated video mutex, because libdatachannel can dispatch successive `onFrame`s concurrently and `VideoSink` is not thread-safe (the file comment at 178-180 documents the heap corruption). The "just move it out, VideoSink is the only writer" one-liner is **unsafe**.
- **Expected benefit:** removes cross-path audio jitter under video load; helps A/V under 1080p. *Unverified magnitude.*
- **Regressions:** none if the video mutex is retained (no deadlock — each callback takes only its own lock).
- **Validation:** instrument the audio-lock wait time (baseline §4) at 1080p; A/B before/after.
- **Rollback:** revert to one mutex. · **Constraint check:** advances "video must not starve audio."

### [F-08] Mid-stream H.264 loss waits a full GOP — no PLI on decode error
- **Category:** VideoLatency · **Sev:** Medium · **Conf:** Confirmed · **Transport:** WebRTC · **Track:** Camera
- **Code:** `windows/src/webrtc_receiver.cpp:183-194, 250-262` (verify id `S16`)
- **Current behavior:** the only `requestKeyframe()` fires once on `Connected`. `onFrame` hands every `AVFrame` to the sink **without inspecting** `avcodec` return codes or `fr->decode_error_flags`, and no NACK responder is attached — so after packet loss the receiver renders corrupted frames until the phone's next IDR.
- **Evidence:** grep confirms `requestKeyframe` only at connect. The Wi-Fi path's keyframe cadence is PLI-driven (Android `DefaultVideoEncoderFactory` sets no explicit I-interval), so mid-stream PLI matters most there.
- **Proposed change:** when `fr->decode_error_flags & (MISSING_REFERENCE|INVALID_BITSTREAM)`, call `vtrack->requestKeyframe()` — **rate-limited** (≤1 PLI per ~300–500 ms) to avoid a keyframe storm/bitrate spike. Optionally add libdatachannel's NACK responder for small losses so PLI is reserved for reference loss.
- **Expected benefit:** cuts the visible-corruption window on loss from up-to-a-GOP to one RTT+keyframe. *Unverified — needs induced packet loss.*
- **Regressions:** un-debounced PLI storms spike bitrate → the rate-limit is mandatory.
- **Validation:** drop packets (clumsy AP / netem on tether) and measure recovery time before/after.
- **Rollback:** remove the error-triggered PLI. · **Constraint check:** PLI is standard, stays encrypted/local.

### [F-09] WASAPI render thread not registered with MMCSS (audio underrun risk under load)
- **Category:** AudioLatency · **Sev:** Low (pure-win default) · **Conf:** Confirmed · **Transport:** Both · **Track:** Microphone
- **Code:** `windows/src/wasapi_sink.cpp:280-363`; thread owners `usb_receiver.cpp:116`, `webrtc_receiver.cpp:122` (verify id `N-win-audio-0`)
- **Current behavior:** the real-time render thread runs at **default priority**; no `AvSetMmThreadCharacteristics`, no MMCSS anywhere in `windows/src`.
- **Evidence:** shared-mode WASAPI render is soft-real-time; MMCSS "Pro Audio"/"Audio" is Microsoft's documented remedy for render-thread scheduling under CPU contention.
- **Proposed change:** at each `audioThread` entry, `AvSetMmThreadCharacteristicsW(L"Pro Audio", &idx)` + `AvRevertMmThreadCharacteristics` on exit (link `avrt.lib`). Default-on, no latency/quality tradeoff.
- **Expected benefit:** fewer render-buffer underruns when the PC is busy. *Unverified magnitude; the 200 ms buffer already cushions, so effect is modest.*
- **Regressions:** none material. · **Validation:** stress the PC (video encode elsewhere) and count underruns before/after.
- **Rollback:** remove the two calls + `avrt`. · **Constraint check:** none affected; Win10+Win11.

### [F-10] Camera switch on USB doesn't force a sync frame → brief corruption on the new lens
- **Category:** VideoLatency · **Sev:** Low · **Conf:** Confirmed · **Transport:** USB · **Track:** Camera
- **Code:** `android/.../UsbStreamer.kt:246-256` + 159 (verify id `N-android-camera-encoder-0`)
- **Current behavior:** `switchCamera` closes the session and reopens the other lens on the **same** encoder Surface; the encoder is never told to emit an IDR, so the very different new-lens image is coded as P-frames against stale references until the next 1 s IDR.
- **Proposed change:** after the new session's `setRepeatingRequest` succeeds, `encoder.setParameters(Bundle().apply{ putInt(MediaCodec.PARAMETER_KEY_REQUEST_SYNC_FRAME,0) })`. (Manual switch, so this is user-driven.)
- **Expected benefit:** clean cut on lens switch instead of ~1 s of artifacts. · **Rollback:** remove the setParameters call.
- **Constraint check:** manual-only; no auto orientation. · *Unverified magnitude; code-evident.*

### [F-11] WASAPI is poll-driven (`Sleep(2)`) with a 200 ms buffer — blocks a smaller low-latency buffer
- **Category:** AudioLatency · **Sev:** Low → **Micro (measured)** · **Conf:** Confirmed · **Transport:** Both · **Track:** Microphone
- **📉 Measured:** `audio.wasapi` standing latency is only **~20 ms** (Pixel 10 Pro USB), not the 200 ms buffer size — the 200 ms is a *ceiling*, confirmed. So the real standing mic latency here is ~20 ms; an event-driven/smaller-buffer rework is **much lower value than the audit implied**. Keep it as an opt-in, deprioritize. See `measured-results.md`.
- **Code:** `windows/src/wasapi_sink.cpp:253-256, 348-361` (verify ids `S6`, `N-win-audio-2`)
- **Current behavior:** `Initialize(..., flags=0, kBufDuration=200 ms, ...)` — no `AUDCLNT_STREAMFLAGS_EVENTCALLBACK`; the drain loop `Sleep(2)`-polls. 200 ms is a latency **ceiling** that persists after a jitter burst (no drain/drift correction). The upstream ~2 s drop-oldest audio FrameQueue is the *larger* standing-latency contributor.
- **Proposed change (optional low-latency mode, not default):** event-driven WASAPI (`EVENTCALLBACK` + `WaitForSingleObject(period)`) with a smaller buffer (~30–50 ms) and a safety floor; **and** shrink/drain the upstream FrameQueue target (bigger lever). Keep the predictable default; expose an opt-in.
- **Expected benefit:** lower mic latency — measure with `latbench`. *Unverified.*
- **Regressions:** a too-small buffer raises underrun risk if the audio thread is delayed → keep behind an opt-in with a floor.
- **Validation:** `latbench` A/B (default vs low-latency mode); watch underruns. · **Rollback:** revert to `flags=0`/200 ms.
- **Constraint check:** default stays predictable (opt-in only); no silent quality drop.

### [F-12] Optional mic mode without AEC/NS (phone-as-remote-mic has nothing to echo-cancel)
- **Category:** AudioLatency/Quality · **Sev:** Low · **Conf:** Confirmed · **Transport:** Both · **Track:** Microphone
- **Code:** `android/.../WebRtcSender.kt:96-99`, `UsbStreamer.kt:291-293` (verify id `S7`)
- **Current behavior:** both paths capture through the platform VOICE chain (WebRTC: HW AEC+NS; USB: `AudioSource.VOICE_COMMUNICATION`). The phone plays no PC audio, so **HW AEC references silence** — dead weight; NS can pump/gate; the VOICE path is often band-limited (~wideband/muffled) on many devices.
- **Proposed change:** offer an opt-in "clean mic" mode using `AudioSource.MIC` / disabling HW AEC+NS. Keep AEC/NS as the predictable default.
- **Expected benefit:** clearer, full-band mic; small latency drop. *Primarily a quality win; unverified per-device.*
- **Regressions:** MIC source picks up more room noise → keep it opt-in. · **Validation:** record CABLE with/without; inspect spectrum + listen.
- **Rollback:** remove the option. · **Constraint check:** optional; default predictable; no DTLS/resolution impact.

### [F-13] WebRTC video sets no start bitrate → slow resolution ramp (soft first seconds)
- **Category:** Startup · **Sev:** Low · **Conf:** Confirmed · **Transport:** WebRTC · **Track:** Camera
- **Code:** `android/.../WebRtcSender.kt:224-235` (verify id `S8`)
- **Current behavior:** the only `sender.parameters` mutation is `degradationPreference=MAINTAIN_FRAMERATE`; no start/min/max bitrate, so WebRTC BWE ramps from ~300 kbps / low resolution over a few seconds (the ramp the receiver engineers around in `video_sink.cpp:71-87`).
- **Proposed change:** raise the start estimate via `PeerConnection.setBitrate(start)` + set `encoding.maxBitrateBps` and `scaleResolutionDownBy=1`; keep `minBitrate` **conservative** (a high floor overrides congestion control and could starve audio on a weak link — that would violate "video must not starve audio").
- **Expected benefit:** shorter time-to-full-resolution on LAN. *Startup-only; unverified.*
- **Regressions:** aggressive min bitrate on a weak link → keep min low. · **Validation:** measure start→full-res frame time before/after.
- **Rollback:** remove the encoding hints. · **Constraint check:** don't force min above link capacity.

### [F-14] No `WifiLock(WIFI_MODE_FULL_LOW_LATENCY)` on the Wi-Fi path
- **Category:** VideoLatency/Power · **Sev:** Low · **Conf:** Confirmed · **Transport:** WebRTC(Wi-Fi) · **Track:** Both
- **Code:** `android/.../StreamService.kt:228-235` (verify id `S9`)
- **Current behavior:** only a `PARTIAL_WAKE_LOCK`; no `WifiLock`. Wi-Fi power-save can add tens of ms of jitter (mostly on downlink RTCP/NACK/ICE — the phone is TX-heavy).
- **Proposed change:** acquire `WIFI_MODE_FULL_LOW_LATENCY` (API 29+, fallback `FULL_HIGH_PERF`) during Wi-Fi streaming; release on stop. Note it only fully engages **foreground + screen-on**, so it partly downgrades in this app's screen-off use — make it opt-in/efficiency-aware.
- **Expected benefit:** less Wi-Fi jitter (power cost). *Unverified; device/AP-dependent.* · **Rollback:** remove the lock.
- **Constraint check:** optional; releases on stop; Wi-Fi-only; no metered/cellular.

### [F-15] USB encoder missing `KEY_PRIORITY=0` realtime hint
- **Category:** VideoLatency · **Sev:** Micro · **Conf:** Confirmed · **Transport:** USB · **Track:** Camera
- **Code:** `android/.../UsbStreamer.kt:155-166` (verify id `N-android-camera-encoder-1`)
- **Current behavior:** sets `KEY_LOW_LATENCY`/`KEY_LATENCY`/no-B-frames/CBR but never `KEY_PRIORITY` (default best-effort). `KEY_PRIORITY=0` is the documented realtime hint (scheduling bias), orthogonal to `KEY_LOW_LATENCY` (pipeline depth).
- **Proposed change:** `runCatching { setInteger(MediaFormat.KEY_PRIORITY, 0) }` alongside the existing best-effort levers. Do **not** set `KEY_OPERATING_RATE` (pins max clocks → battery).
- **Expected benefit:** small, device-specific jitter reduction (mainly pre-R SoCs). · **Rollback:** remove the line. · **Constraint check:** no-op where unsupported.

### [F-16] USB audio/encoder threads at default priority
- **Category:** AudioLatency · **Sev:** Low · **Conf:** Confirmed · **Transport:** USB · **Track:** Both
- **Code:** `android/.../UsbStreamer.kt:285-311` (audio), `170-193` (enc) (verify ids `S15`, `N-android-camera-encoder-5`)
- **Current behavior:** the `usb-audio` capture thread and `usb-enc` encode/socket thread run at default niceness (no `Process.setThreadPriority`). The ~40 ms AudioRecord buffer absorbs most jitter, so impact is bounded.
- **Proposed change:** `Process.setThreadPriority(THREAD_PRIORITY_URGENT_AUDIO)` first line of the audio runnable; `THREAD_PRIORITY_DISPLAY`/`URGENT_DISPLAY` for `usb-enc`. **Raise both** (they share `writeLock`) to avoid a priority inversion on the shared socket writer.
- **Expected benefit:** less capture/send jitter under CPU load. *Unverified; needs on-device load test.* · **Rollback:** remove the calls.
- **Constraint check:** none affected.

### [F-17] Camera2 USB request never disables video stabilization (EIS latency on some phones)
- **Category:** VideoLatency · **Sev:** Low · **Conf:** Plausible (device-dependent) · **Transport:** USB · **Track:** Camera
- **Code:** `android/.../UsbStreamer.kt:221-228` (verify id `N-android-camera-encoder-4`)
- **Current behavior:** `TEMPLATE_RECORD` request sets only the AE FPS range; never `CONTROL_VIDEO_STABILIZATION_MODE`. `TEMPLATE_RECORD` *may* enable EIS, which buffers/look-aheads (≥1 frame) + warps — latency at odds with the scrcpy-style intent. On/off is OEM-default-dependent.
- **Proposed change:** explicitly set `CONTROL_VIDEO_STABILIZATION_MODE=OFF` (optionally `NOISE_REDUCTION/EDGE_MODE=FAST`) on **both** the primary and the AE-fallback request builds; guard via `CONTROL_AVAILABLE_VIDEO_STABILIZATION_MODES` + `runCatching`.
- **Expected benefit:** removes EIS latency on phones that default it on. *Device-dependent; measure glass-to-glass on an affected phone.* · **Rollback:** remove the key.
- **Constraint check:** manual per-request control; no auto orientation; no resolution change.

### [F-18] Move the adb command chain off the WinForms UI thread (Start/switch-cam freeze)
- **Category:** Startup/UX · **Sev:** High · **Conf:** Confirmed · **Transport:** USB · **Track:** Both
- **Code:** `windows/packaging/files/PhoneCam-GUI.cs:456-555, 469-485` (verify id `N-csharp-build-0`)
- **Current behavior:** `StartReceiver`→`AdbDevice`→`StartUsbPath` runs the whole adb sequence **synchronously on the UI thread** via `AdbRun` (each `Process.Start`+`WaitForExit`): `devices`(5s) + `forward --remove`(4s) + `forward`(5s) + 3×`pm grant`(12s) + `wakeup`(3s) + `am start`(6s). Worst case ~35 s; nominal 1–3 s incl. a cold adb-server fork. The message pump is blocked → the window greys out / "Not Responding".
- **Proposed change:** run the adb round-trips on `Task.Run`; marshal only the final UI updates (and the `RunReceiverProcess` shared-state writes read by `OnTick`) back via `BeginInvoke`. Only the `WaitForExit` calls belong on the worker.
- **Expected benefit:** responsive UI during USB Start / camera switch. *UX; code-verifiable.* · **Rollback:** revert to synchronous.
- **Constraint check:** UI-only; no media/transport change. **Caveat:** guard the cross-thread WinForms access (`recv`/`usbMode`/`lastUrl`/`videoSeen`/`micMeter`).

### [F-19] Drain the receiver's redirected **stdout** (latent pipe-fill hazard)
- **Category:** Stability · **Sev:** Low · **Conf:** Plausible · **Transport:** Both · **Track:** Both
- **Code:** `windows/packaging/files/PhoneCam-GUI.cs:579, 602` (verify id `N-csharp-build-1`)
- **Current behavior:** `RedirectStandardOutput=true` but only `BeginErrorReadLine()` is called — **stdout is never read**. libdatachannel's default plog appender may write to stdout; within one long-lived receiver process (it re-listens across sessions) enough warning text could fill the ~4 KB pipe and block the logging thread.
- **Proposed change:** simplest — `RedirectStandardOutput=false` (inherit/discard); or `BeginOutputReadLine()` with a discard/`Log` handler. (Note: not `webrtc_receiver.cpp` output accumulates across the 30 reconnects — each is a fresh process/pipe.)
- **Expected benefit:** removes a latent deadlock. *Unverified whether ever hit (libdatachannel is a prebuilt dep).* · **Rollback:** revert. · **Constraint check:** none.

### [F-20] Restrict Android ABIs (ship arm only) — APK/download size
- **Category:** (install size) · **Sev:** Low · **Conf:** Confirmed · **Transport:** — · **Track:** —
- **Code:** `android/app/build.gradle.kts:12-18, 71` (verify ids `N-csharp-build-3`, `S25`)
- **Current behavior:** no `abiFilters`/splits, so `assembleRelease` bundles the large `libjingle_peerconnection_so.so` for arm64-v8a, armeabi-v7a, **x86, x86_64** (emulator-only). Also `isMinifyEnabled=false` (no R8).
- **Proposed change:** `ndk { abiFilters += ["arm64-v8a","armeabi-v7a"] }` (drop x86/x86_64 — keep an emulator build variant if needed); consider R8 with org.webrtc keep rules.
- **Expected benefit:** ~20–40 MB smaller sideload APK; faster install. **Runtime unaffected** (Android only loads the device ABI). *Install-size only.* · **Rollback:** remove the filter.
- **Constraint check:** none require emulator support; nothing runtime-facing.

---

## Tier 2 — moderate / lower-value confirmed items (batch opportunistically)

| ID | Sev | What | Fix | Verify id |
|---|---|---|---|---|
| F-21 | Micro | USB encoder callback allocates a `ByteArray` per frame | reuse a grown scratch buffer; `writeFrame` already takes (offset,len) | S5 |
| F-22 | Micro | Preview does an extra full-frame BGR copy every frame regardless of visibility | double-buffer swap instead of copy-in+copy-out; optionally gate on visibility | S11, N-win-video-2 |
| F-23 | Micro | Mic-level meter scans every sample even at 0 dB/EQ-off | subsample, or add a meter-enable flag | S12 |
| F-24 | Micro | USB receiver: vector copy + `av_new_packet`/`av_frame` alloc+memcpy per unit | pool buffers; a raw-PCM `WasapiSink` overload kills the audio alloc+copy | S13, N-win-audio-3 |
| F-25 | Micro | Preview uses HALFTONE stretch (slowest) every frame | cheaper stretch mode (COLORONCOLOR) for the live monitor; set mode once | S10 |
| F-26 | Micro | Mic-only WebRTC still builds EGL + video encoder/decoder factories | gate video factories on `withVideo`; drop decoder factory (send-only) | N-android-camera-encoder-2 |
| F-27 | Low | USB path discards `ptsUs` — no A/V-sync stats or jitter buffer | expose per-stream residence/queue-depth stats; optional PTS alignment (bounded) | S20 |
| F-28 | Micro | GUI: identical-rect `MoveWindow` every 700 ms tick; `AppendAllText` per log line | size-guard the move / hook `Resize`; batch log writes | S24, N-csharp-build-5 |
| F-29 | Micro | Receiver Release build: no LTO | enable `CMAKE_INTERPROCEDURAL_OPTIMIZATION` Release-only (small; FFmpeg is in DLLs — drop the PGO idea) | S19 |
| F-30 | Micro | Ships full BtbN `gpl-shared` FFmpeg (avformat/avfilter/avdevice unused) | ship a minimized decode-only FFmpeg — **installer-size only**, runtime ~unaffected | N-csharp-build-4 |
| F-31 | Micro | WebRTC session disconnect polled at 100 ms | optional CV wake (bounded `wait_for` still needed for `*running`) — lowest priority | S22 |

---

## Tier 3 — architectural experiments (need measurement before commitment)

- **F-32 Conditional hardware H.264 decode (D3D11VA/DXVA2).** Could cut PC CPU/power — **but** the softcam sink needs BGR24 in system memory, forcing a GPU→CPU readback that can erase the benefit. Measure full latency + copy cost + compatibility before adopting; keep software decode as the default.
- **F-33 Dedicated video decode+sink thread for the WebRTC path** (mirror the USB `VideoQueue` design) — the clean, complete form of F-07, removes all audio/video decode contention. Larger refactor.
- **F-34 Skip convert/transform/softcam-push when no camera consumer is attached** — `softcam` exposes `IsConnected()` (`SenderAPI.h:13`), so gate the (decode-must-stay) color-convert + rotate/flip + `SendFrame` + preview copy on `IsConnected(cam_) || preview-visible`. Real idle-CPU/power win in the always-on auto-listen/tray scenario. (verify id `S23`, feasibility confirmed by direct header read.)
- **F-35 In-band keyframe-request byte on the USB adb socket** — lets the PC ask the phone for an IDR after an overflow shed (F-02) or decode error; needs a phone-side reverse read path (currently write-only).

---

## Rejected / inconclusive (do NOT action as written)

| ID | Claim | Why rejected | Revisit? |
|---|---|---|---|
| S4 | Longer USB GOP saves battery/bandwidth | **CBR pins bitrate** (a longer GOP only re-spends the fixed budget on P-frames); phone is USB-cabled/charging so encode energy is moot; motion-est on extra P-frames may *raise* compute; I-frame decode isn't costlier. Only effect is a tiny quality-per-bit tradeoff. | Only if the USB path ever switches to VBR. |
| S21 | H.264 High profile burns phone battery | The PC offers constrained-baseline (`webrtc_receiver.cpp:244`, profile-level-id 42e01f), so High is **never negotiated** — `enableH264HighProfile=true` is a no-op. Even if used, HW MediaCodec delta is small and High *lowers* bitrate → less TX energy. | Only if the PC offer advertises High. |
| N-android-audio-power-1 | HW AEC/NS stacks with software AEC3/NS (double processing) | libwebrtc does **not** stack them — `WebRtcVoiceEngine` disables the SW APM stage when HW effects are active. Only SW AGC + high-pass remain (tiny). | Disable only `googAutoGainControl`/`googHighpassFilter` if measured. |
| N-win-video-0 | Zero-copy H.264 packet eliminates a per-frame copy | With `av_new_packet` the code already does exactly **one** copy; the "borrow" style forces FFmpeg to copy internally instead — no net win. Only a real `av_buffer_create` wrapper eliminates the copy (more involved). | Revisit as F-24 (hoist persistent packet/frame — saves struct allocs only). |
| N-win-transport-threads-3 | USB `VideoQueue` grows to 4 s of latency | The decode drain is unthrottled and the softcam sink is **latest-wins**, so transient stalls already skip forward; the 4 s bound only guards memory. Lowering `kMax` to ~1 s is a fine memory ceiling but doesn't fix a real latency growth. | Optional `kMax` reduction only. |

**Plausibles pending measurement** (real code facts, magnitude unverified): F-17, F-19 (above), plus
`N-android-audio-power-0` (mic-only still allocates ADM — but WebRTC builds a default ADM anyway, so
the "fix" saves nothing), `N-android-camera-encoder-3` (camera-only allocates ADM object — no HAL
touched, Micro), `N-csharp-build-2` (adb `ReadToEnd` deadlock — unreachable with the current tiny-output
command set).
