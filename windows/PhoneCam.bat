@echo off
REM ============================================================================
REM  PhoneCam - double-click to use your phone as a webcam.
REM
REM  Autodetects USB (preferred) or Wi-Fi, starts the phone app for you, and
REM  connects. Then open Zoom/Teams/OBS and pick the "PhoneCam Camera" device.
REM
REM  Close this window (or press Ctrl+C) to stop.
REM  You can pass the same flags as the script, e.g.:  PhoneCam.bat -Preview
REM ============================================================================
title PhoneCam  (close this window to stop)
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\phonecam.ps1" %*
if errorlevel 1 (
  echo.
  echo PhoneCam exited with an error - scroll up for details.
  echo Press any key to close . . .
  pause >nul
)
