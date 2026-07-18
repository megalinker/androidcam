# PhoneCam — Performance Architecture Map

> Ground-truth map built by reading every core source file (Android Kotlin ×6, Windows C++ ×7,
> C# GUI, CMake, manifests, CI). Line references are to the code as of `0.5.4` / commit `1c3f03b`.
> Where this map and `docs/architecture.md` disagree, the **code** is authoritative and the drift is noted.

## 1. Processes & components

| Process | Language | Role | Key files |
|---|---|---|---|
| PhoneCam (phone app) | Kotlin | Capture + encode + transport | `MainActivity`, `StreamService`, `UsbStreamer`, `WebRtcSender`, `ControlReceiver`, `PcLink` |
| `PhoneCam.exe` | C# / WinForms | Launcher, transport pick, adb drive, QR, preview host, live controls | `PhoneCam-GUI.cs` |
| `receiver.exe` | C++ / MSVC | Decode → softcam (camera) + WASAPI (mic) | `receiver.cpp`, `webrtc_receiver.cpp`, `usb_receiver.cpp`, `video_sink.cpp`, `wasapi_sink.cpp`, `preview_window.cpp` |
| softcam.dll (×86+×64) | C++ | DirectShow virtual camera (shared-mem frame push) | `third_party/softcam` (tshino, MIT) |
| VB-CABLE (or driver) | — | Kernel capture endpoint the mic reappears on | external / `windows/driver` |
| `latbench.exe` | C++ | Acoustic mouth→virtual-mic latency benchmark (dev) | `latbench.cpp` |

Two media transports, **one** Windows decode→sink pipeline:
- **USB (preferred):** scrcpy-style H.264 + raw PCM over a framed TCP socket via `adb forward`.
- **Wi-Fi / USB-tether:** WebRTC (DTLS-SRTP, Opus + H.264) via libdatachannel; host ICE only.

## 2. Threads & long-running loops

### Phone — USB (`UsbStreamer.kt`)
| Thread | Created | Loop / role | Notes |
|---|---|---|---|
| `usb-accept` | `start()` | `ServerSocket.accept()`; one client at a time | daemon |
| `usb-enc` (HandlerThread) | `startVideo()` | MediaCodec async callback → socket write | must be off main (does socket I/O) |
| `usb-cam` (HandlerThread) | `openCamera()` | Camera2 callbacks / repeating request | |
| `usb-audio` (Thread) | `startAudio()` | blocking `AudioRecord.read` → socket write | **default priority** |
| (client) accept thread | `handleClient()` | blocks on `inp.read()` for EOF | |

### Phone — Wi-Fi (`WebRtcSender.kt`)
| Thread | Role |
|---|---|
| `webrtc-sender` (worker) | signaling handshake, then blocks on signaling-socket EOF |
| org.webrtc internal pool | capture (SurfaceTextureHelper `CaptureThread`), Opus/H.264 encode (MediaCodec), network, pacing, congestion control |
| teardown thread | `dispose()` off the caller thread |

### Windows receiver — USB (`usb_receiver.cpp`)
| Thread | Loop | Queue in/out |
|---|---|---|
| receive/session | `readFull` framing; dispatch `V`→VideoQueue, `A`→PcmQueue | producer |
| `videoThread` | `vq.pop()` → FFmpeg H.264 decode → `VideoSink::WriteFrame` | VideoQueue consumer |
| `audioThread` | `pq.pop()` → build AVFrame → `WasapiSink::WriteFrame` | PcmQueue consumer |
| preview (in VideoSink) | 8 ms loop: pump window, `StretchDIBits` | reads pbuf_ |

### Windows receiver — WebRTC (`webrtc_receiver.cpp`)
| Thread | Loop | Notes |
|---|---|---|
| listener (main) | `select()` 300 ms → `accept` → `handleConnection`; then `sleep_for(100ms)` disconnect poll | |
| libdatachannel pool | `track->onMessage` (Opus RTP) + `vtrack->onFrame` (H.264) — **both serialized by one `decodeMutex`** | decode happens here |
| `audioThread` | `FrameQueue.pop()` → `WasapiSink::WriteFrame`; owns COM + sink | |
| preview (in VideoSink) | as above | |
| `stdin_control_thread` (in `receiver.cpp`, detached) | reads `fliph/flipv/rotate` lines | sets global atomics |

### Desktop (`PhoneCam-GUI.cs`)
| Timer/thread | Interval | Role |
|---|---|---|
| WinForms `timer` | 700 ms | embed preview HWND, reconnect FSM, status from receiver stderr |
| `recv.ErrorDataReceived` | async | parse `[level]`/`[video]`/`[audio]`/`session ended`; drive meter + status |
| adb child processes | on Start / switch-cam only | `adb devices/forward/pm grant/am start/am broadcast` |

## 3. Queues (producer/consumer, bound, drop policy, locking, shutdown)

| Queue | Producer → Consumer | Item | Cap | Full | Empty | Lock | Unbounded? | Stale video? | Blocks net read? | Shutdown wakes? |
|---|---|---|---|---|---|---|---|---|---|---|
| `VideoQueue` (usb) | receive → decode | `vector<u8>` Annex-B | 120 (~4 s@30) | **drop oldest** | cv wait | mutex+cv | No | drops oldest | No (decoupled) | Yes (`finish()`) |
| `PcmQueue` (usb) | receive → audio | `vector<u8>` PCM | 64 (~1.3 s) | **drop oldest** | cv wait | mutex+cv | No | n/a | No | Yes |
| `FrameQueue` (webrtc audio) | rtc decode → audio | `AVFrame*` | 100 (~2 s@20ms) | **drop oldest**, `av_frame_free` | cv wait | mutex+cv | No | n/a | No | Yes (`signalStop`) |
| softcam FrameBuffer | WriteFrame → DShow consumer | BGR24 shared mem | 1 (latest) | overwrite | — | shared-mem mutex | No | latest-wins | No | n/a |
| WASAPI ring | WriteFrame → audio engine | endpoint PCM | 200 ms | `Sleep(2)` poll wait | — | WASAPI | No | n/a | n/a | on Stop |
| WebRTC video | libdatachannel `onFrame` — **no app queue**; decode inline on the pool thread | — | — | — | — | `decodeMutex` | — | — | — | — |

**All application queues are bounded and drop-oldest — the "no unbounded queue" constraint holds.** The one shared hazard is `decodeMutex` coupling audio and video decode (see findings F-03).

## 4. Media formats & the copy map

### Video — one frame, end to end
**USB:** Camera2 → encoder input **Surface** (GPU, zero-copy) → H.264 Annex-B → *[copy 1: `ByteBuffer.get(ByteArray)` in encoder callback, `UsbStreamer.kt:179`]* → socket → PC → *[copy 2: `payload.resize`+recv]* → *[copy 3: `VideoQueue` `emplace_back(p,p+n)`]* → *[copy 4: `av_new_packet`+memcpy, `usb_receiver.cpp:99`]* → FFmpeg decode (YUV) → *[copy 5: `sws_scale` YUV→BGR24]* → *(copy 6: rotate/flip only if active)* → softcam `SendFrame` *(copy into shared mem)* + *[copy 7: `pbuf_` preview memcpy, `video_sink.cpp:137`]*.

**WebRTC:** Camera2 → texture → org.webrtc H.264 encode (GPU) → SRTP → libdatachannel depacketize (Annex-B) → *[copy: `av_new_packet`+memcpy `webrtc_receiver.cpp:253`]* → decode → sws_scale → softcam + preview copy. (No app-level video queue — decode is inline on the rtc pool thread under `decodeMutex`.)

### Audio — one block
**USB:** AudioRecord → reused `chunk` ByteArray (no per-read alloc) → *[socket]* → *[PcmQueue vector copy]* → *[new AVFrame + `av_frame_get_buffer` + memcpy `usb_receiver.cpp:135`]* → swr_convert → EQ/gain/limiter in place → *[memcpy into WASAPI `GetBuffer`]*.
**WebRTC:** Opus RTP → decode → *[`av_frame_clone` `webrtc_receiver.cpp:230`]* → FrameQueue → swr_convert → EQ/gain → WASAPI copy.

The Windows **swscale context** is rebuilt only on a *source*-size change (the WebRTC ramp); the swscale **output buffer** and softcam are (re)built only on an *output*-geometry change (first frame or a 90/270 rotate). The **swresample** context is built once per session. These are correct (no per-frame context churn).

## 5. Timestamp / clock domains

- **Phone monotonic** (`presentationTimeUs` from the encoder; `samples * 1e6/rate` for PCM). USB frames carry an 8-byte `ptsUs`.
- **PC QPC** (WASAPI stamps each packet; `latbench` uses this).
- **WebRTC clock + RTCP SRs** — the only path with real cross-stream A/V sync (handled inside the stack).
- **USB path discards `ptsUs`** (`usb_receiver.cpp:213` `(void)be64(...)`) → no timestamp-based A/V sync or jitter buffer; both streams play as-fast-as-received (F-13/S20). Android and Windows monotonic clocks are **not** comparable without calibration — cross-device latency must be measured acoustically (`latbench`) or with a common external event.

## 6. Per-transport pipelines

### USB — camera
```
Camera2 ─Surface(GPU)→ MediaCodec H.264(CBR,1s GOP,low-latency) ─callback:ByteArray copy→
  writeFrame[lock+flush per frame] → adb-fwd TCP → receiver recv → VideoQueue(120,drop-oldest)
  → videoThread: av_new_packet+memcpy → H.264 decode [LOW_DELAY set; FRAME threading NOT disabled]
  → sws_scale YUV→BGR24 → (rotate/flip) → softcam SendFrame  (+ preview pbuf_ copy → 8ms GDI HALFTONE)
```
### USB — microphone
```
AudioRecord(VOICE_COMMUNICATION, 48k mono S16, ~20ms chunk, reused buffer, default prio)
  → writeFrame[shared lock w/ video] → TCP → recv → PcmQueue(64,drop-oldest)
  → audioThread: new AVFrame+memcpy → swr_convert → EQ/gain/limiter + per-sample meter
  → WASAPI shared 200ms buffer, Sleep(2) poll render
```
### WebRTC — camera
```
Camera2 texture → org.webrtc H.264(HIGH profile, MAINTAIN_FRAMERATE, no bitrate hints → ramps from ~360p)
  → DTLS-SRTP UDP → libdatachannel depacketize → onFrame[decodeMutex shared w/ audio]
  → H.264 decode [no LOW_DELAY; FRAME threading not disabled] → sws_scale → softcam (+preview)
```
### WebRTC — microphone
```
org.webrtc mic (HW AEC+NS) → Opus → DTLS-SRTP → libdatachannel onMessage[decodeMutex]
  → Opus decode [no LOW_DELAY] → av_frame_clone → FrameQueue(100,drop-oldest)
  → audioThread → swr_convert → EQ/gain → WASAPI 200ms shared, poll render
```

## 7. Documentation drift (code vs docs)
- `receiver.cpp` header comment says "The RTSP/SRT camera path and its adb-forward 'USB mode' were retired … the whole media stack is WebRTC now." — **stale**: `--usb` (scrcpy-style) is a first-class transport (`usb_receiver.cpp`, wired in `main`). README/architecture are correct; the receiver.cpp banner is not.
- `docs/architecture.md` "softcam … output pinned to one resolution" — accurate (`VideoSink::ensure`, short-side→720).
- FFmpeg shipped as full **gpl-shared** (avformat/avfilter/avdevice DLLs present in `dist/`) but the receiver links only `avcodec avutil swscale swresample` — unused DLLs are shipped (size only).
