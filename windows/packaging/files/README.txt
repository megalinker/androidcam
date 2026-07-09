========================================================================
 PhoneCam - use your Android phone as a webcam on Windows (camera only)
========================================================================

WHAT YOU NEED
  - Windows 10 or 11 (64-bit)
  - An Android phone
  - Both on the same Wi-Fi   (OR a USB cable - see "USB" below)

------------------------------------------------------------------------
 IF YOU DOWNLOADED THIS AS A ZIP: unblock it first
------------------------------------------------------------------------
  Right-click the .zip -> Properties -> tick "Unblock" -> OK, THEN extract.
  Extract it to a place it can STAY (e.g. C:\PhoneCam) - not a temp folder.
  (Windows may still warn "unknown publisher" - this app isn't signed.
   Click "More info" -> "Run anyway".)

------------------------------------------------------------------------
 ON THE PHONE (once)
------------------------------------------------------------------------
  1. Copy PhoneCam.apk to the phone and tap it to install.
     (You may need to allow "Install unknown apps" for your file manager.)
  2. Open PhoneCam, choose "Camera + Mic" or "Camera only", press Start,
     and allow the camera permission. It shows an address like:
         rtsp://192.168.0.101:8554/
     Leave it running.

------------------------------------------------------------------------
 ON THE PC
------------------------------------------------------------------------
  1. Double-click  1-Install-Camera.bat   (say YES to the admin prompt).
     Do this ONCE per PC. It adds the "PhoneCam Camera" webcam.
  2. Double-click  PhoneCam.bat
     - It asks for the address shown on the phone. Type it (or just the
       numbers, e.g. 192.168.0.101) and press Enter.
  3. Open Zoom / Teams / Meet / Discord / OBS and pick the camera named
       "PhoneCam Camera".
     That's it - your phone is now the webcam.

  Keep the PhoneCam.bat window open while you use it. Close it to stop.

------------------------------------------------------------------------
 USB instead of Wi-Fi (optional, more reliable, no typing an address)
------------------------------------------------------------------------
  On the phone enable Developer Options -> USB debugging, plug it in, tap
  "Allow" when asked. Then just double-click PhoneCam.bat - it starts the
  phone app and connects automatically. (Great if Wi-Fi is blocked by a
  VPN or a "guest"/isolated network.)

------------------------------------------------------------------------
 NOTES / TROUBLESHOOTING
------------------------------------------------------------------------
  - "PhoneCam Camera" shows up in Zoom/Teams/OBS/Discord/Chrome, but NOT
    in the built-in Windows "Camera" app (that app uses a different system).
  - The picture is black until PhoneCam.bat is running and connected.
  - Can't connect over Wi-Fi? Make sure the PC and phone are on the SAME
    network, the phone app is streaming, and turn OFF any VPN on either
    device. Some routers block devices from seeing each other ("AP
    isolation"/guest Wi-Fi) - use a normal network, or use USB.
  - Audio (mic) is NOT included - this is the camera only.
  - To remove it later: double-click 2-Uninstall-Camera.bat.

  Files here:
    PhoneCam.bat            - run this to start
    1-Install-Camera.bat    - run once to add the camera (admin)
    2-Uninstall-Camera.bat  - remove the camera (admin)
    PhoneCam.apk            - the Android app
    bin\, redist\           - program files (leave them alone)
