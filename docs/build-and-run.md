# Build & Run

Two pieces: the **Android app** and the **Windows side** (the C++ `receiver.exe` + the C# `PhoneCam.exe` desktop app). For everyday use you just install both and press Start — this doc is for building from source.

## 1. Android app

**Easiest:** open `android/` in **Android Studio** and Run onto a connected phone.

**CLI:** JDK 17 + the Android SDK (`ANDROID_HOME`, or an `android/local.properties` with `sdk.dir=/path/to/Android/Sdk` — forward slashes on Windows). The repo ships the wrapper config but not the wrapper JAR — generate it once with Gradle 8.7+:

```bash
cd android
gradle wrapper --gradle-version 8.7      # one-time
./gradlew assembleDebug                    # app/build/outputs/apk/debug/app-debug.apk
./gradlew installDebug                     # with a phone on adb + USB debugging
```

On the phone: open **PhoneCam**, pick a mode (Cam + Mic / Camera / Mic) and quality. Then either plug in over USB (the PC drives it) or tap **Scan** and point at the QR the desktop app shows. It streams headless — view the feed on the PC.

## 2. Windows side (Windows 10/11 — real box or a VM)

One-time setup:

1. **Visual Studio 2022** (Desktop C++), CMake, and **[vcpkg](https://github.com/microsoft/vcpkg)**.
2. WebRTC media stack via vcpkg (libdatachannel + OpenSSL-backed SRTP):
   ```powershell
   vcpkg install "libdatachannel[core,srtp,ws]:x64-windows" "libsrtp[openssl]:x64-windows" `
     --overlay-ports=windows/vcpkg-overlays
   ```
3. **FFmpeg** shared dev build → extract to `windows/third_party/ffmpeg/` (`include/ lib/ bin/`). Used for codecs only (H.264 + Opus decode, `swscale`, `swresample`).
4. (For the camera sink) clone + build **softcam** and register both bitnesses once (elevated):
   ```powershell
   git clone https://github.com/tshino/softcam windows/third_party/softcam
   # build softcam.sln Release for x64 AND Win32 -> softcam.dll (+ softcamcore.lib)
   regsvr32 <path>\x64\softcam.dll
   regsvr32 <path>\Win32\softcam.dll
   ```

Build the receiver:

```powershell
cd windows
cmake -B build -A x64 -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release

# with the softcam virtual camera enabled:
cmake -B build -A x64 -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake `
  -DWITH_SOFTCAM=ON -DSOFTCAM_ROOT="%CD%\third_party\softcam" -DSOFTCAM_LIB="<path>\softcamcore.lib"
cmake --build build --config Release
```

`windows/packaging/build-windows.ps1` does the whole reproducible build (deps + FFmpeg + softcam + receiver); `assemble.ps1` compiles the C# `PhoneCam.exe` (Roslyn `csc`) and lays out the `dist/` payload (incl. a bundled `adb`); `PhoneCam.iss` builds the Inno Setup installer. CI does all three on a `v*` tag.

## 3. Running

Normally you just open **PhoneCam.exe**. On **Start** it chooses the transport:

- **USB (preferred):** a phone on adb with USB debugging → it forwards the port, grants permissions, auto-starts the app, and pulls the stream over the cable. No QR.
- **USB tethering:** a `192.168.42.x` adapter present → WebRTC over the cable.
- **Wi-Fi:** otherwise → shows a QR; scan it with the phone.

Then pick **PhoneCam Camera** in Zoom/Teams/Discord/OBS/Chrome. (It won't appear in the built-in Windows *Camera* app — that's Media-Foundation-only; conferencing apps are DirectShow.) Mirror / flip / rotate / switch-camera are buttons in the app and apply live.

To run `receiver.exe` by hand:

```powershell
# Wi-Fi (WebRTC) — encode a matching PCAM3:<pc-ip>:8891:<hex> QR for the phone:
.\build\Release\receiver.exe --sig-port 8891 --sig-secret <hex> --webrtc-video --preview --audio-device "CABLE Input"

# USB — forward the phone's socket first, then connect:
adb forward tcp:27183 tcp:27183
adb shell am start -n com.phonecam/.MainActivity -a com.phonecam.action.USB --ei usbPort 27183 --es mode BOTH
.\build\Release\receiver.exe --usb --usb-port 27183 --preview --audio-device "CABLE Input"
```

Flags: `--webrtc-video`, `--preview`, `--audio-device <substr>`, `--mic-gain <db>`, `--eq <preset|bands>`, `--flip-h`/`--flip-v`/`--rotate <deg>` (manual image transform), `--ice-bind <ip>` (bind ICE to the USB-tethering adapter), `--usb`/`--usb-port`. While running, the receiver also reads live `fliph 0|1` / `flipv 0|1` / `rotate <deg>` commands on stdin (how the app's buttons work).

### Validate without the phone

`-DWITH_WEBRTC_TESTS=ON` builds harnesses that stand in for the phone — `webrtc_testsender` connects to the PCAM3 port and sends real Opus + H.264; `webrtc_audio`/`webrtc_video` bridge a single RTP track through the FFmpeg codecs.

## 4. The microphone

The receiver **renders** decoded phone audio to a WASAPI endpoint (point `--audio-device` at your speakers to just hear it). To make it a selectable **mic**, install **[VB-CABLE](https://vb-audio.com/Cable/)** (free, MS-signed), run with `--audio-device "CABLE Input"`, and pick **CABLE Output** as the microphone in your app. A branded self-contained **PhoneCam Microphone** kernel driver is the optional alternative — see [../windows/driver/README.md](../windows/driver/README.md) — but it needs the WDK and driver signing. Mic **boost** and voice **EQ** presets are in the desktop app (`--mic-gain` / `--eq`).

## Troubleshooting

- **USB not picked up:** enable **USB debugging** (Developer options) and tap *Allow* on the phone; the app needs an *authorized* adb device. A CI/debug-signed APK won't install over a release build — `adb uninstall com.phonecam` first.
- **Wi-Fi can't connect:** same subnet, no AP-isolation/guest network; a full-tunnel VPN on the phone blocks the LAN-direct path (turn it off or allow LAN traffic). The installer adds the Windows Firewall rules; a manual build needs an inbound allow for the signaling port.
- **Flipping the phone / rotating:** as of 0.5.4 this no longer drops the stream, and the app follows your system auto-rotate setting.
- **Stutter over Wi-Fi:** use 5 GHz; WebRTC adapts bitrate/resolution on its own. USB avoids it entirely.
- **`scCreateCamera failed`:** only one softcam camera can exist system-wide — close any other app using it and make sure `softcam.dll` is registered.
