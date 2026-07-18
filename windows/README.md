# PhoneCam — Windows receiver

`receiver.exe` takes the phone's media over one of two transports, decodes it with FFmpeg, and pushes:
- **video** (H.264) → the [`softcam`](https://github.com/tshino/softcam) DirectShow virtual camera (`PhoneCam Camera`), and/or an optional `--preview` window
- **audio** → a **WASAPI render endpoint** ([`wasapi_sink.cpp`](src/wasapi_sink.cpp)). Point it at a virtual-audio cable (VB-CABLE) so the mic reappears as a capture device, or at your speakers to hear it while testing.

Two transports, same decode → sink pipeline:

- **Wi-Fi (WebRTC)** — the phone connects out to the PC's PCAM3 TCP signaling port (gated by a `pairSecret` from the QR); SDP is exchanged, then Opus + H.264 flow over **DTLS-SRTP / UDP**. libdatachannel (via vcpkg) does the SRTP/RTP; FFmpeg does codecs only.
- **USB** — a scrcpy-style pipe: the phone (MediaCodec H.264 + raw PCM) listens on a loopback port, the PC reaches it through `adb forward`, and reads a small framed protocol (`[type][ptsUs][len][payload]`, `H`/`V`/`A`).

The desktop app (`PhoneCam.exe`) picks the transport, drives `adb`, shows the pairing QR when needed, and launches the receiver — you normally never invoke `receiver.exe` by hand.

## Options

```
receiver.exe --sig-port <port> --sig-secret <secret> [--webrtc-video] [--preview]     (Wi-Fi / WebRTC)
             [--audio-device <name-substr>] [--mic-gain <db>] [--eq <preset|type:f:q:db;...>]
             [--ice-bind <local-ipv4>] [--flip-h|--mirror] [--flip-v] [--rotate <deg>]
   or:
receiver.exe --usb --usb-port <local-port> [--preview] [--audio-device ...] [--mic-gain ...] ...   (USB)
```

- `--sig-port` / `--sig-secret` — the PCAM3 TCP signaling endpoint the phone dials (QR: `PCAM3:<pc-ip>:<port>:<secret>`, default port 8891). PC offers **recvonly**, phone answers **sendonly**.
- `--webrtc-video` — also receive an H.264 camera track → softcam (audio-only otherwise).
- `--usb` / `--usb-port` — pull H.264/PCM from the adb-forwarded socket instead of WebRTC (default forward port 27183).
- `--preview` — show the decoded video in a GDI window (the desktop app embeds this).
- `--audio-device <substr>` — render to the endpoint whose name contains `<substr>` (e.g. `CABLE Input`); default = system default output.
- `--mic-gain <db>` — boost the (quiet) phone mic; soft-limited in the sink.
- `--eq <preset|bands>` — a voice EQ preset name, or a `type:freq:q:gain;…` band list.
- `--ice-bind <ipv4>` — bind ICE to this local interface (the USB-tethering adapter) so WebRTC media rides the cable.
- `--flip-h`/`--mirror`, `--flip-v`, `--rotate <0|90|180|270>` — **manual** image transform (initial state). Never automatic.

**Live control:** while running, the receiver reads one command per line on **stdin** — `fliph 0|1`, `flipv 0|1`, `rotate <deg>` — so the desktop app's Mirror/Flip/Rotate buttons take effect without a restart. The virtual camera output is a **fixed resolution** (chosen once from the first frame's aspect, short side → 720), so it never changes size mid-call.

> **Build this on Windows.** MSVC + Windows SDK are required to compile, register (`regsvr32`), and debug a DirectShow filter. A Windows 10/11 VM (KVM/QEMU/VirtualBox) is fine.

## Prerequisites

- Visual Studio 2022 (Desktop C++) or Build Tools + CMake ≥ 3.20
- **[vcpkg](https://github.com/microsoft/vcpkg)** for the WebRTC media stack:
  ```powershell
  vcpkg install "libdatachannel[core,srtp,ws]:x64-windows" "libsrtp[openssl]:x64-windows" `
    --overlay-ports=windows/vcpkg-overlays
  ```
- **FFmpeg dev libraries** (shared, LGPL — e.g. BtbN or gyan.dev). Extract into `windows/third_party/ffmpeg/` (`include/ lib/ bin/`). Codecs only — Opus + H.264 decode, resample, scale; no `avformat`.
- **softcam** as a DirectShow virtual camera — clone https://github.com/tshino/softcam, build **both** `Win32` and `x64` `softcam.dll`, and register once per machine:
  ```
  regsvr32 softcam.dll     (elevated x64 prompt, 64-bit DLL)
  regsvr32 softcam32.dll   (32-bit DLL — for 32-bit apps)
  ```
  To name the device **PhoneCam Camera** (softcam's default is `DirectShow Softcam`), change `FILTER_NAME` in
  `third_party/softcam/src/softcam/softcam.cpp` before building. If you rename *after* registering, unregister
  first (`regsvr32 /u`) or delete the stale
  `HKLM\SOFTWARE\{Classes,WOW6432Node\Classes}\CLSID\{860BB310-5D01-11D0-BD3B-00A0C911CE86}\Instance\<old name>`
  key — monikers are keyed by name.

## Build

```powershell
cd windows
cmake -B build -A x64 -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release
```

Add `-DWITH_SOFTCAM=ON -DSOFTCAM_ROOT=... -DSOFTCAM_LIB=...\softcamcore.lib` to enable the virtual camera, or `-DWITH_WEBRTC_TESTS=ON` for the de-risk harnesses (see `CMakeLists.txt`). `packaging/build-windows.ps1` does the whole thing reproducibly.

## Run

Open **PhoneCam.exe** — it generates the secret, picks USB or Wi-Fi, shows the QR if needed, and launches the receiver. By hand, see the two invocations under **Options** above. Then pick **PhoneCam Camera** as the webcam. The receiver keeps listening and **auto-reconnects** if the phone sleeps or the link drops.

## The phone mic as a selectable microphone

The receiver *renders* decoded phone audio to any WASAPI output. To make that a **microphone**, feed it into a virtual audio cable — install **[VB-CABLE](https://vb-audio.com/Cable/)** (free, MS-signed) and run `receiver.exe … --audio-device "CABLE Input"`, then pick **CABLE Output** as the mic in your app. A branded self-contained **PhoneCam Microphone** kernel driver is the optional alternative — see [driver/README.md](driver/README.md) — but it needs the WDK and driver signing.
