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

## Phase 2 — the phone mic as a selectable microphone

The receiver already *renders* decoded phone audio to any WASAPI output. To make that
audio show up as a **microphone** in Zoom/Teams/Discord, feed it into a **virtual audio
cable** — a render endpoint whose samples reappear on a capture endpoint.

### Recommended: a pre-signed virtual cable (works for everyone, no driver signing)

Install **[VB-CABLE](https://vb-audio.com/Cable/)** (free, Microsoft-signed → installs on any
Win10/11 with **Secure Boot on, no test-signing**) or use **Voicemeeter**. Then:

```powershell
.\scripts\phonecam.ps1 -Mic          # routes phone audio into "CABLE Input"
# or directly:
.\build\Release\receiver.exe rtsp://<phone-ip>:8554/ --audio-device "CABLE Input"
```

In your app, pick **CABLE Output (VB-Audio Virtual Cable)** as the microphone. (Verified
end to end: the phone audio comes out that capture device.) To show it as **"PhoneCam
Microphone"** instead, right-click it in *Sound settings → Recording → Properties → Rename*
(one-time, cosmetic).

### Alternative: the custom kernel driver (branded, self-contained — but needs signing)

A from-scratch **PhoneCam Microphone** driver lives in [driver/](driver) (forked from
Virtual-Audio-Driver, with a real render→capture loopback added). It builds and is
signable, but a kernel driver only loads either **test-signed** (Secure Boot **off**, your
PC only) or **Microsoft-attestation-signed** (EV cert, for distribution). See
[driver/README.md](driver/README.md). For "anyone can install it," the pre-signed cable
above is the practical choice.
