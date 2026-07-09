# Build & Run

Two pieces: the **Android app** (build on Linux) and the **Windows receiver** (build on Windows). Both machines must be on the **same Wi-Fi/LAN**.

## 1. Android app (on your Linux box)

**Easiest:** open `android/` in **Android Studio** (Giraffe or newer). It will download the SDK, generate the Gradle wrapper JAR, and let you Run onto a connected phone.

**CLI:** you need JDK 17 and the Android SDK (`ANDROID_HOME` set, or an `android/local.properties` with `sdk.dir=/path/to/Android/Sdk`). This repo ships the wrapper *config* but not the wrapper JAR — generate it once with a system Gradle 8.7+:

```bash
cd android
gradle wrapper --gradle-version 8.7      # one-time: creates ./gradlew and the wrapper jar
./gradlew assembleDebug
./gradlew installDebug                   # with a phone connected via adb + USB debugging
```

Then on the phone: open **PhoneCam**, pick a mode (Camera+Mic / Camera / Mic), grant permissions, press **Start**. The screen shows the pull URL, e.g. `rtsp://192.168.1.42:8554/`. It streams headless (no on-screen preview — view the feed on the PC).

> **URL caveat:** the shown URL comes from RootEncoder's `getEndPointConnection()`, which picks the first interface address — if the phone is on a **VPN** it may show a non-LAN (e.g. IPv6) address the PC can't reach, and the server binds there too. Turn the phone's VPN off, or read the phone's Wi-Fi IPv4 from Settings and use `rtsp://<that-ip>:8554/`.
>
> **"Mic only"** still opens the camera (RootEncoder won't serve until the video encoder emits a keyframe) but only audio is sent. If you want the camera truly off, that's a RootEncoder limitation — use it knowing the camera light stays on.

> First run tip: if `prepareVideo(1920x1080…)` fails on a weaker phone, lower it to `1280×720` / `4_000_000` bps in [StreamService.kt](../android/app/src/main/java/com/phonecam/StreamService.kt).

## 2. Windows receiver (on Windows 10/11 — real box or a VM)

One-time setup:

1. Install **Visual Studio 2022** (Desktop C++), CMake.
2. **FFmpeg** shared dev build → extract to `windows/third_party/ffmpeg/` (`include/ lib/ bin/`).
3. (For the camera sink) clone + build **softcam**, and register it once:
   ```powershell
   git clone https://github.com/tshino/softcam windows/third_party/softcam
   # build softcam.sln in Release for BOTH x64 and Win32 → softcam.dll (+ softcamcore.lib)
   # register both bitnesses (elevated):
   regsvr32 <path>\x64\softcam.dll
   regsvr32 <path>\Win32\softcam.dll
   ```

Build & run:

```powershell
cd windows
cmake -B build -A x64
cmake --build build --config Release

# 1. Smoke test — see the video in a window + hear the mic on your speakers.
#    Proves the whole Wi-Fi -> decode path with zero drivers installed.
.\build\Release\receiver.exe rtsp://<phone-ip>:8554/ --preview

# 2. Enable the softcam virtual camera (after building + registering softcam):
cmake -B build -A x64 -DWITH_SOFTCAM=ON `
  -DSOFTCAM_ROOT="%CD%\third_party\softcam" `
  -DSOFTCAM_LIB="<path>\softcamcore.lib"
cmake --build build --config Release
.\build\Release\receiver.exe rtsp://<phone-ip>:8554/
```

Flags: `--preview` (GDI video window), `--no-audio`, `--audio-device <name-substr>` (target a specific output, e.g. the Phase-2 `PhoneCam Audio` endpoint), `--udp` (lower-latency RTSP transport), `--smooth` (jitter buffer for weak Wi-Fi, trades latency for smoothness).

Open Zoom/Teams/Discord/OBS/Chrome and choose **PhoneCam Camera**. (It won't appear in the built-in Windows *Camera* app — that's Media-Foundation-only; conferencing apps are DirectShow and will see it.)

The receiver **auto-reconnects** (every 2s) if the phone sleeps or the stream drops — leave it running.

### Validate without the phone (test harness)

Before the phone app is even installed, serve a synthetic RTSP stream and point the receiver at it:

```powershell
# Terminal A — needs ffmpeg on PATH (it ships in windows\third_party\ffmpeg\bin):
.\scripts\test-source.ps1
# Terminal B:
.\build\Release\receiver.exe rtsp://127.0.0.1:8554/live --preview
```

The preview window shows a moving test pattern (its title bar shows live **fps + resolution**) and your speakers play a 440 Hz tone — proving the decode + WASAPI path with no phone and no drivers. Use [../windows/scripts/test-source.sh](../windows/scripts/test-source.sh) to serve from a Linux/macOS box across the LAN instead.

## 3. Phase 2 — the microphone

The receiver already **renders** the phone audio to a WASAPI endpoint (you can hear it on your speakers now). To make Windows expose a selectable **PhoneCam Microphone**, build the kernel virtual-audio **loopback** driver — follow the runbook in [../windows/driver/README.md](../windows/driver/README.md), which uses the ready-made test-sign/install scripts in [../windows/driver/scripts/](../windows/driver/scripts/) — then run `receiver.exe --audio-device "PhoneCam Audio"`. This driver is the part that needs the WDK and driver signing (test-signing is fine for your own PC).

## Troubleshooting

- **PC can't connect:** confirm both devices are on the same subnet; some routers isolate Wi-Fi clients ("AP isolation"/guest network) — turn that off. Check the phone's firewall isn't blocking port 8554.
- **Phone slept / stream dropped:** the receiver **auto-reconnects every 2s** — just wake the phone or re-press Start; no need to restart `receiver.exe`.
- **Stutter/latency over Wi-Fi:** use 5 GHz, lower the bitrate, or try `rtsp_transport=udp` in [receiver.cpp](../windows/src/receiver.cpp).
- **`scCreateCamera failed`:** only one softcam camera can exist system-wide; close any other app using it, and make sure you registered `softcam.dll`.
