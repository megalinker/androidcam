========================================================================
 PhoneCam 0.5.4 - Android camera and low-latency microphone for Windows
========================================================================

WHAT YOU NEED
  - Windows 10 or 11 (64-bit)
  - An Android phone
  - Both devices on the same local network
  - VB-CABLE for microphone output: https://vb-audio.com/Cable/

FIRST-TIME SETUP
  1. Install PhoneCam.apk on the phone and allow camera/microphone access.
  2. Open PhoneCam on Windows - it shows a pairing QR.
  3. On the phone, pick a mode (Cam + Mic / Camera / Mic) and quality.
  4. Tap Scan on the phone and point it at the QR on the PC.
  5. For video: in Zoom, Teams, Meet, Discord, or OBS choose "PhoneCam Camera".
     For the mic: tick "Use microphone" on the PC, then choose CABLE Output as
     the microphone in your call app.

Media uses WebRTC with Opus (audio), H.264 (video), and authenticated
DTLS-SRTP encryption. Pairing and media stay on your LAN, direct between the
phone and PC; there is no cloud signaling service.

IMAGE CONTROLS
  Mirror (L/R), Flip (U/D), Rotate, and Switch camera (front/back) are buttons in
  the Windows app. They apply live while streaming and are never automatic - the
  picture is shown exactly as the camera sees it until you change it.

RECONNECTING
  After the first scan, the phone shows Reconnect. Either side may be started
  first: the phone retries the saved PC endpoint while it waits for the Windows
  app to begin listening.

USB (lowest latency, optional)
  For the steadiest, lowest-latency link, use the cable. Two ways, both auto-detected
  by PhoneCam on the PC (no QR scan needed):

  1. Recommended - USB debugging. On the phone, enable Developer options, then turn on
     "USB debugging" and plug in. Tap "Allow" if prompted. Press Start on the PC: it
     finds the phone, starts it, and streams H.264 + mic straight over the cable. No
     tethering, no scan.
  2. USB tethering. If you'd rather not enable USB debugging, turn on USB tethering
     (Settings > Network & internet > Hotspot & tethering > USB tethering) with the
     cable in; PhoneCam routes the encrypted WebRTC media over the cable instead of Wi-Fi.

  Unplug to go back to Wi-Fi.

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
  bin\, redist\           - application files
