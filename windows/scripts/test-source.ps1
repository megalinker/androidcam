# PhoneCam — first-run test harness (Windows).
#
# Serves a synthetic RTSP stream (moving test pattern + 440 Hz tone) so you can validate
# receiver.exe end-to-end WITHOUT the phone app installed. Requires ffmpeg.exe on PATH
# (the FFmpeg you downloaded for the receiver already includes it in its bin/ folder).
#
# Terminal A:   .\test-source.ps1
# Terminal B:   ..\build\Release\receiver.exe rtsp://127.0.0.1:8554/live --preview
#   -> the preview window should show the test pattern and your speakers play a tone.
#
# FFmpeg's RTSP muxer with -rtsp_flags listen acts as the server and waits for the receiver
# to connect (one client). Ctrl+C to stop.

param(
    [int]$Port    = 8554,
    [string]$Path = "live",
    [string]$Size = "1280x720",
    [int]$Fps     = 30
)
$ErrorActionPreference = 'Stop'

if (-not (Get-Command ffmpeg -ErrorAction SilentlyContinue)) {
    throw "ffmpeg not on PATH. Add your FFmpeg bin\ folder to PATH (the one under windows\third_party\ffmpeg\bin)."
}

$url = "rtsp://0.0.0.0:$Port/$Path"
Write-Host "Serving test RTSP at rtsp://127.0.0.1:$Port/$Path  (Ctrl+C to stop)" -ForegroundColor Green
Write-Host "Then run:  ..\build\Release\receiver.exe rtsp://127.0.0.1:$Port/$Path --preview" -ForegroundColor Yellow

ffmpeg -hide_banner -re `
    -f lavfi -i "testsrc2=size=$Size`:rate=$Fps" `
    -f lavfi -i "sine=frequency=440:sample_rate=48000" `
    -c:v libx264 -preset ultrafast -tune zerolatency -pix_fmt yuv420p -g $Fps `
    -c:a aac -ar 48000 -ac 1 `
    -f rtsp -rtsp_flags listen $url
