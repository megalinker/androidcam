# Build & Run

Two pieces: the **Android app** (build on Linux) and the **Windows receiver** (build on Windows). Both machines must be on the **same Wi-Fi/LAN**. Media rides **WebRTC** (DTLS-SRTP, LAN-direct); the phone connects out to the PC after scanning its pairing QR.

## 1. Android app (on your Linux box)

**Easiest:** open `android/` in **Android Studio** (Giraffe or newer). It will download the SDK, generate the Gradle wrapper JAR, and let you Run onto a connected phone.

**CLI:** you need JDK 17 and the Android SDK (`ANDROID_HOME` set, or an `android/local.properties` with `sdk.dir=/path/to/Android/Sdk`). This repo ships the wrapper *config* but not the wrapper JAR — generate it once with a system Gradle 8.7+:

```bash
cd android
gradle wrapper --gradle-version 8.7      # one-time: creates ./gradlew and the wrapper jar
./gradlew assembleDebug
./gradlew installDebug                    # with a phone connected via adb + USB debugging
```

Then on the phone: open **PhoneCam**, pick a mode (Cam + Mic / Camera / Mic) and quality, grant permissions, tap **Scan**, and point the camera at the QR shown in the PhoneCam app on the PC. It streams headless (no on-screen preview — view the feed on the PC). The status card shows the PC it's connected to; after the first scan, **Reconnect** re-dials the saved PC.

> **VPN note:** a **full-tunnel VPN** on the phone can block LAN connections entirely — if the phone can't reach the PC, turn the phone's VPN off, switch it to split-tunnel, or enable "Allow LAN traffic". Media is host-candidate-only (no STUN/TURN), so both devices must be able to reach each other directly on the LAN.

## 2. Windows receiver (on Windows 10/11 — real box or a VM)

One-time setup:

1. Install **Visual Studio 2022** (Desktop C++), CMake, and **[vcpkg](https://github.com/microsoft/vcpkg)**.
2. WebRTC media stack via vcpkg:
   ```powershell
   vcpkg install "libdatachannel[core,srtp,ws]:x64-windows" "libsrtp[openssl]:x64-windows" `
     --overlay-ports=windows/vcpkg-overlays
   ```
3. **FFmpeg** shared dev build → extract to `windows/third_party/ffmpeg/` (`include/ lib/ bin/`).
4. (For the camera sink) clone + build **softcam**, and register it once:
   ```powershell
   git clone https://github.com/tshino/softcam windows/third_party/softcam
   # build softcam.sln in Release for BOTH x64 and Win32 → softcam.dll (+ softcamcore.lib)
   # register both bitnesses (elevated):
   regsvr32 <path>\x64\softcam.dll
   regsvr32 <path>\Win32\softcam.dll
   ```

Build:

```powershell
cd windows
cmake -B build -A x64 -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release

# with the softcam virtual camera enabled:
cmake -B build -A x64 -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake `
  -DWITH_SOFTCAM=ON -DSOFTCAM_ROOT="%CD%\third_party\softcam" -DSOFTCAM_LIB="<path>\softcamcore.lib"
cmake --build build --config Release
```

Run: open **PhoneCam.exe** (the desktop app) — it generates the pairing secret, shows the QR, and launches the receiver. Or run the receiver directly with a signaling port + secret and encode a matching `PCAM3:<pc-ip>:<port>:<secret>` QR for the phone:

```powershell
.\build\Release\receiver.exe --sig-port 8891 --sig-secret <hex> --webrtc-video --preview --audio-device "CABLE Input"
```

Open Zoom/Teams/Discord/OBS/Chrome and choose **PhoneCam Camera**. (It won't appear in the built-in Windows *Camera* app — that's Media-Foundation-only; conferencing apps are DirectShow and will see it.)

The receiver keeps the signaling port open and **auto-reconnects** if the phone sleeps or the link drops — leave it (or the desktop app) running.

### Validate without the phone (de-risk tools)

The `WITH_WEBRTC_TESTS` CMake option builds standalone harnesses that stand in for the phone or exercise a single layer — `webrtc_testsender` connects to the receiver's PCAM3 port and sends real Opus + H.264, and `webrtc_audio`/`webrtc_video` bridge an RTP track through the FFmpeg codecs:

```powershell
cmake -B build -A x64 -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake -DWITH_WEBRTC_TESTS=ON
cmake --build build --config Release
```

## 3. The microphone

The receiver already **renders** the phone audio to a WASAPI endpoint (you can hear it on your speakers with `--audio-device` pointed at them). To make Windows expose a selectable microphone, feed it into a **virtual audio cable** — install **[VB-CABLE](https://vb-audio.com/Cable/)** (free, Microsoft-signed) and run `receiver.exe … --audio-device "CABLE Input"`, then pick **CABLE Output** as the mic in your app. A branded, self-contained **PhoneCam Microphone** kernel driver is the optional alternative — see [../windows/driver/README.md](../windows/driver/README.md) — but it needs the WDK and driver signing.

## Troubleshooting

- **PC can't connect:** confirm both devices are on the same subnet; some routers isolate Wi-Fi clients ("AP isolation"/guest network) — turn that off. Make sure Windows Firewall allows PhoneCam (the installer adds the rules; a manual build needs an inbound allow for the signaling port).
- **Phone slept / link dropped:** the receiver keeps listening and the phone retries the saved PC — just wake the phone or tap Reconnect; no need to restart the receiver.
- **Stutter/latency over Wi-Fi:** use 5 GHz and keep both devices off a congested channel. WebRTC adapts bitrate and sheds resolution under congestion on its own.
- **`scCreateCamera failed`:** only one softcam camera can exist system-wide; close any other app using it, and make sure you registered `softcam.dll`.
