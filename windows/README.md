# PhoneCam — Windows receiver

Pulls the phone's RTSP stream, decodes it with FFmpeg, and pushes:
- **video** → the [`softcam`](https://github.com/tshino/softcam) DirectShow virtual camera (`PhoneCam Camera`), and/or an optional `--preview` window
- **audio** → a **WASAPI render endpoint** via [`wasapi_sink.cpp`](src/wasapi_sink.cpp). Point it at the loopback "speaker" of the Phase-2 virtual-mic driver, or (default) your real speakers to hear the phone mic while testing.

## Options

```
receiver.exe rtsp://<phone-ip>:8554/ [--preview] [--no-audio] [--audio-device <name-substr>] [--udp] [--flip-h] [--flip-v]
```

- `--preview` — show the decoded video in a GDI window (verify the pipeline without softcam).
- `--no-audio` — video only.
- `--audio-device <substr>` — render audio to the endpoint whose name contains `<substr>` (e.g. `PhoneCam`); default is the system default output.
- `--udp` — use RTSP-over-UDP (lower latency, tolerates loss) instead of TCP.
- `--flip-h` / `--flip-v` — mirror the image left/right or top/bottom (combine for a 180° rotation). Applies to both the softcam camera and the `--preview` window.
- `--smooth` — add a jitter buffer (reorder queue + demux delay + threaded decode). Smoother on a congested/weak Wi-Fi link at the cost of latency. Default is lowest-latency.

> **Build this on Windows.** MSVC + Windows SDK are required to compile, register (`regsvr32`), and debug a DirectShow filter. It cannot be built or tested from Linux. A Windows 10/11 VM (KVM/QEMU/VirtualBox) is fine.

## Prerequisites

- Visual Studio 2022 (Desktop C++ workload) or Build Tools + CMake ≥ 3.20
- **FFmpeg dev libraries** (shared, LGPL) — e.g. the `*-full_build-shared` from https://www.gyan.dev/ffmpeg/builds/ or https://github.com/BtbN/FFmpeg-Builds/releases.
  Extract into `windows/third_party/ffmpeg/` so you have `third_party/ffmpeg/{include,lib,bin}`.
- **softcam** built as a DirectShow virtual camera — clone https://github.com/tshino/softcam and build **both** `Win32` and `x64` `softcam.dll`, then register:
  ```
  regsvr32 softcam.dll            (from an elevated x64 prompt for the 64-bit DLL)
  regsvr32 softcam32.dll          (for the 32-bit DLL — needed by 32-bit apps)
  ```
  You only need to register once per machine. Link this receiver against `softcamcore` (see `CMakeLists.txt`).
  To make the device show up as **PhoneCam Camera** (softcam's default is `DirectShow Softcam`), change
  `FILTER_NAME` in `third_party/softcam/src/softcam/softcam.cpp` before building the DLL. If you rename it
  *after* registering, unregister first (`regsvr32 /u`) or delete the stale
  `HKLM\SOFTWARE\{Classes,WOW6432Node\Classes}\CLSID\{860BB310-5D01-11D0-BD3B-00A0C911CE86}\Instance\<old name>`
  key — those monikers are keyed by name, so re-registering leaves the old one behind.

## Build

```powershell
cd windows
cmake -B build -A x64
cmake --build build --config Release
```

## Run

```powershell
# rtsp URL is printed by the Android app once you press Start
.\build\Release\receiver.exe rtsp://<phone-ip>:8554/
```

Then open Zoom/Teams/Discord/OBS and pick **PhoneCam Camera** as the webcam.

## Status

- `src/receiver.cpp` — RTSP → FFmpeg decode → softcam (video) + WASAPI (audio). **Video path is the Phase-1 deliverable.**
- `src/wasapi_sink.cpp` — audio is fully wired: it resamples to the endpoint mix format and renders via WASAPI. Against real speakers this already works end to end; the only missing piece for a *microphone* is the Phase-2 loopback driver that makes a render endpoint reappear as a capture device.
- `src/preview_window.cpp` — GDI preview (`--preview`), so you can validate video before softcam is registered.

## Phase 2 — the virtual microphone

See [driver/README.md](driver/README.md). The receiver already *renders* audio via WASAPI, so the driver just needs to be a **loopback** device: a render endpoint ("PhoneCam Audio") whose samples reappear on a capture endpoint ("PhoneCam Microphone"). Fork [`VirtualDrivers/Virtual-Audio-Driver`](https://github.com/VirtualDrivers/Virtual-Audio-Driver) (MIT) or Microsoft's `SysVAD`; then run `receiver.exe --audio-device "PhoneCam Audio"`. Requires the **WDK** and driver **signing** (test-signing for your own PC; EV cert + Microsoft attestation to distribute).
