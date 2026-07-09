@echo off
REM PhoneCam - double-click to use your phone as a webcam. Close this window to stop.
REM Extra flags pass through, e.g.:  PhoneCam.bat -Wifi -Ip 192.168.0.101   or   PhoneCam.bat -Preview
title PhoneCam  (close this window to stop)
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0bin\phonecam.ps1" %*
if errorlevel 1 (
  echo.
  echo PhoneCam stopped or hit a problem - scroll up for details.
  echo Press any key to close . . .
  pause >nul
)
