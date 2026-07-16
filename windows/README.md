# PhoneCam — Windows receiver

Receives the phone's **WebRTC** stream (DTLS-SRTP, LAN-direct), decodes it with FFmpeg, and pushes:
- **video** (H.264) → the [`softcam`](https://github.com/tshino/softcam) DirectShow virtual camera (`PhoneCam Camera`), and/or an optional `--preview` window
- **audio** (Opus) → a **WASAPI render endpoint** via [`wasapi_sink.cpp`](src/wasapi_sink.cpp). Point it at the loopback "speaker" of a virtual-audio cable (VB-CABLE) so the phone mic reappears as a selectable microphone, or at your real speakers to just hear it while testing.

The phone connects out to the PC; the two exchange SDP over a small TCP signaling channel (PCAM3) gated by a `pairSecret`, then media flows over UDP. The desktop app (`PhoneCam.exe`) shows the pairing QR and launches the receiver for you — you normally never invoke `receiver.exe` by hand.

## Options

```
receiver.exe --sig-port <port> --sig-secret <secret>
             [--webrtc-video] [--preview]
             [--audio-device <name-substr>] [--mic-gain <db>] [--eq <preset|type:f:q:db;...>]
```

- `--sig-port` / `--sig-secret` — the PCAM3 TCP signaling endpoint the phone dials (encoded in the QR as `PCAM3:<pc-ip>:<port>:<secret>`).
- `--webrtc-video` — also receive an H.264 camera track and push it to softcam (audio-only otherwise).
- `--preview` — show the decoded video in a GDI window (verify the pipeline without softcam).
- `--audio-device <substr>` — render audio to the endpoint whose name contains `<substr>` (e.g. `CABLE Input`); default is the system default output.
- `--mic-gain <db>` — boost the (usually quiet) phone mic; soft-limited in the sink.
- `--eq <preset|bands>` — a voice EQ preset name, or a `type:freq:q:gain;…` band list.

> **Build this on Windows.** MSVC + Windows SDK are required to compile, register (`regsvr32`), and debug a DirectShow filter. It cannot be built or tested from Linux. A Windows 10/11 VM (KVM/QEMU/VirtualBox) is fine.

## Prerequisites

- Visual Studio 2022 (Desktop C++ workload) or Build Tools + CMake ≥ 3.20
- **[vcpkg](https://github.com/microsoft/vcpkg)** for the WebRTC media stack (libdatachannel + OpenSSL-backed SRTP):
  ```powershell
  vcpkg install "libdatachannel[core,srtp,ws]:x64-windows" "libsrtp[openssl]:x64-windows" `
    --overlay-ports=windows/vcpkg-overlays
  ```
- **FFmpeg dev libraries** (shared, LGPL) — e.g. the `*-full_build-shared` from https://www.gyan.dev/ffmpeg/builds/ or https://github.com/BtbN/FFmpeg-Builds/releases.
  Extract into `windows/third_party/ffmpeg/` so you have `third_party/ffmpeg/{include,lib,bin}`. Used for the codecs only (Opus + H.264 decode, resample, scale).
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
cmake -B build -A x64 -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release
```

Add `-DWITH_SOFTCAM=ON -DSOFTCAM_ROOT=... -DSOFTCAM_LIB=...\softcamcore.lib` to enable the virtual camera (see `CMakeLists.txt`). `windows/packaging/build-windows.ps1` does the whole thing — deps, FFmpeg, softcam, and the softcam-enabled receiver — reproducibly.

## Run

Normally you just open **PhoneCam.exe** (the desktop app): it generates the `--sig-secret`, shows the pairing QR, and launches `receiver.exe` with the right flags. To run the receiver directly, pass a port + secret and encode a `PCAM3:<pc-ip>:<port>:<secret>` QR for the phone to scan:

```powershell
.\build\Release\receiver.exe --sig-port 8891 --sig-secret <hex> --webrtc-video --preview --audio-device "CABLE Input"
```

Then open Zoom/Teams/Discord/OBS and pick **PhoneCam Camera** as the webcam.

## The phone mic as a selectable microphone

The receiver **renders** decoded phone audio to any WASAPI output. To make that audio show up as a **microphone** in Zoom/Teams/Discord, feed it into a **virtual audio cable** — a render endpoint whose samples reappear on a capture endpoint.

Install **[VB-CABLE](https://vb-audio.com/Cable/)** (free, Microsoft-signed → installs on any Win10/11 with Secure Boot on, no test-signing) or use **Voicemeeter**, then:

```powershell
.\build\Release\receiver.exe --sig-port 8891 --sig-secret <hex> --audio-device "CABLE Input"
```

In your app, pick **CABLE Output (VB-Audio Virtual Cable)** as the microphone. To show it as **"PhoneCam Microphone"** instead, right-click it in *Sound settings → Recording → Properties → Rename* (one-time, cosmetic).
