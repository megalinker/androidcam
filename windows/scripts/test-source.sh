#!/usr/bin/env bash
# PhoneCam — first-run test harness (Linux/macOS).
#
# Same as test-source.ps1 but for a POSIX box. Handy to serve the test stream from your Linux
# dev machine and connect the Windows receiver to it across the LAN (validates the network path
# too), or just to sanity-check the ffmpeg command. Requires ffmpeg.
#
#   ./test-source.sh                       # serve on 0.0.0.0:8554/live
#   # then on the Windows box:
#   receiver.exe rtsp://<this-machine-ip>:8554/live --preview
#
# FFmpeg's RTSP muxer with -rtsp_flags listen acts as the server (one client). Ctrl+C to stop.
set -euo pipefail

PORT="${1:-8554}"
STREAM_PATH="${2:-live}"
SIZE="${3:-1280x720}"
FPS="${4:-30}"

command -v ffmpeg >/dev/null 2>&1 || { echo "ffmpeg not found on PATH"; exit 1; }

echo "Serving test RTSP at rtsp://0.0.0.0:${PORT}/${STREAM_PATH}  (Ctrl+C to stop)"
echo "Connect with: receiver.exe rtsp://<this-machine-ip>:${PORT}/${STREAM_PATH} --preview"

exec ffmpeg -hide_banner -re \
    -f lavfi -i "testsrc2=size=${SIZE}:rate=${FPS}" \
    -f lavfi -i "sine=frequency=440:sample_rate=48000" \
    -c:v libx264 -preset ultrafast -tune zerolatency -pix_fmt yuv420p -g "${FPS}" \
    -c:a aac -ar 48000 -ac 1 \
    -f rtsp -rtsp_flags listen "rtsp://0.0.0.0:${PORT}/${STREAM_PATH}"
