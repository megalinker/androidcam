========================================================================
 PhoneCam 0.5.0 - Android camera and low-latency microphone for Windows
========================================================================

WHAT YOU NEED
  - Windows 10 or 11 (64-bit)
  - An Android phone
  - Both devices on the same local network
  - VB-CABLE for microphone output: https://vb-audio.com/Cable/

FIRST-TIME SETUP
  1. Install PhoneCam.apk on the phone and allow camera/microphone access.
  2. Open PhoneCam on Windows.
  3. Select Wi-Fi - scan QR and leave the default Mic - low latency mode.
  4. Press Start, then scan the displayed QR with the phone app.
  5. In your call app, choose CABLE Output as the microphone.

The phone's mic uses WebRTC with Opus and authenticated DTLS-SRTP encryption.
Pairing and media stay on your LAN; there is no cloud signaling service.

RECONNECTING
  After the first scan, the phone shows Reconnect. Either side may be started
  first: the phone retries the saved PC endpoint while it waits for the Windows
  app to begin listening.

CAMERA MODE
  In the Windows Wi-Fi mode list, choose Camera - standard. Press Start and scan
  the QR. In Zoom, Teams, Meet, Discord, or OBS choose PhoneCam Camera.

TROUBLESHOOTING
  - A full-tunnel VPN can hide LAN devices. Disable it or enable Allow LAN traffic
    on both the phone and PC.
  - Avoid guest Wi-Fi or access-point isolation.
  - The Windows Camera app may not list the DirectShow camera; conferencing apps do.
  - The installer and portable builds are unsigned, so Windows may show an
    Unknown publisher warning.

FILES
  PhoneCam.exe            - desktop app
  PhoneCam.apk            - Android app
  PhoneCam.bat            - command-line RTSP camera fallback
  1-Install-Camera.bat    - register the camera (portable build)
  2-Uninstall-Camera.bat  - remove the camera registration
  bin\, redist\           - application files
