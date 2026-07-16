========================================================================
 PhoneCam 0.5.2 - Android camera and low-latency microphone for Windows
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

RECONNECTING
  After the first scan, the phone shows Reconnect. Either side may be started
  first: the phone retries the saved PC endpoint while it waits for the Windows
  app to begin listening.

USB (lowest latency, optional)
  For the steadiest, lowest-latency link, enable USB tethering on the phone
  (Settings > Network & internet > Hotspot & tethering > USB tethering) with the
  cable plugged in. PhoneCam on the PC detects it automatically and routes media
  over the cable instead of Wi-Fi - just scan the QR as usual. Unplug or turn
  tethering off to go back to Wi-Fi.

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
