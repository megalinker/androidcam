cmake_minimum_required(VERSION 3.20)
project(phonecam_receiver LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---------------------------------------------------------------------------
# FFmpeg (shared dev build). Download per windows/README.md and extract into
# windows/third_party/ffmpeg so you have include/ lib/ bin/ under it.
# We use FFmpeg for the codecs only (Opus + H.264 decode, resample, scale);
# the transport is WebRTC (libdatachannel), so avformat is no longer needed.
# ---------------------------------------------------------------------------
set(FFMPEG_ROOT "${CMAKE_SOURCE_DIR}/third_party/ffmpeg" CACHE PATH "FFmpeg dev build root")
if(NOT EXISTS "${FFMPEG_ROOT}/include")
    message(FATAL_ERROR "FFmpeg not found at ${FFMPEG_ROOT}. See windows/README.md.")
endif()

# ---------------------------------------------------------------------------
# WebRTC media stack (libdatachannel) — the receiver's only transport. Install with:
#   vcpkg install "libdatachannel[core,srtp,ws]:x64-windows" "libsrtp[openssl]:x64-windows" \
#     --overlay-ports=windows/vcpkg-overlays
#   cmake -B build -A x64 -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake
# libdatachannel delivers DTLS-SRTP + RTP; codecs stay on FFmpeg, sinks stay as-is.
# ---------------------------------------------------------------------------
find_package(LibDataChannel CONFIG REQUIRED)

add_executable(receiver
    src/receiver.cpp
    src/webrtc_receiver.cpp   # PCAM3 signaling + DTLS-SRTP Opus/H.264 receive
    src/usb_receiver.cpp      # scrcpy-style H.264/PCM over an adb-forwarded socket (--usb)
    src/video_sink.cpp        # decoded H.264 -> BGR24 -> softcam + embedded preview
    src/wasapi_sink.cpp       # decoded Opus -> WASAPI render endpoint (virtual mic loopback)
    src/preview_window.cpp)   # embedded GDI video preview (--preview)

target_include_directories(receiver PRIVATE "${FFMPEG_ROOT}/include")
target_link_directories(receiver PRIVATE "${FFMPEG_ROOT}/lib")
target_link_libraries(receiver PRIVATE avcodec avutil swscale swresample LibDataChannel::LibDataChannel)
target_compile_definitions(receiver PRIVATE HAVE_WEBRTC)

# Windows system libs: WASAPI/COM (ole32), GDI preview (gdi32/user32), Winsock (ws2_32).
if(WIN32)
    target_link_libraries(receiver PRIVATE ole32 gdi32 user32 ws2_32)
endif()

# ---------------------------------------------------------------------------
# softcam (the DirectShow virtual camera sink). OFF by default so the decode
# path builds and can be smoke-tested (it just logs frames) before you wire the
# camera. To enable:
#   1. git clone https://github.com/tshino/softcam third_party/softcam
#   2. build softcam.sln (x64 Release) -> produces softcamcore.lib
#   3. cmake -B build -A x64 -DWITH_SOFTCAM=ON \
#        -DSOFTCAM_ROOT="${CMAKE_SOURCE_DIR}/third_party/softcam" \
#        -DSOFTCAM_LIB="<path>/softcamcore.lib"
# Remember to also build+register softcam.dll (x64 AND Win32) once on the machine.
# ---------------------------------------------------------------------------
option(WITH_SOFTCAM "Push decoded video into the tshino/softcam virtual camera" OFF)
if(WITH_SOFTCAM)
    set(SOFTCAM_ROOT "${CMAKE_SOURCE_DIR}/third_party/softcam" CACHE PATH "tshino/softcam checkout")
    set(SOFTCAM_LIB "" CACHE FILEPATH "Path to built softcamcore.lib")
    if(NOT EXISTS "${SOFTCAM_ROOT}/src/softcamcore/SenderAPI.h")
        message(FATAL_ERROR "SenderAPI.h not found under ${SOFTCAM_ROOT}/src/softcamcore")
    endif()
    if(NOT SOFTCAM_LIB)
        message(FATAL_ERROR "Set -DSOFTCAM_LIB=<path>/softcamcore.lib")
    endif()
    target_compile_definitions(receiver PRIVATE HAVE_SOFTCAM)
    target_include_directories(receiver PRIVATE "${SOFTCAM_ROOT}/src/softcamcore")
    # softcamcore's Timer uses the multimedia timer (timeSetEvent) -> winmm.
    target_link_libraries(receiver PRIVATE "${SOFTCAM_LIB}" winmm)
endif()

# ---------------------------------------------------------------------------
# WebRTC de-risk / QA test tools (dev only; not part of the shipped receiver).
# These stand in for the phone or exercise a single layer:
#   webrtc_loopback   in-process DTLS-SRTP media loopback
#   webrtc_signaling  PCAM3 TCP SDP exchange + bring-up
#   webrtc_audio      Opus RTP <-> FFmpeg codec bridge
#   webrtc_video      H.264 RTP <-> FFmpeg codec bridge
#   webrtc_testsender phone stand-in: PCAM3 client sending real Opus + H.264
# ---------------------------------------------------------------------------
option(WITH_WEBRTC_TESTS "Build the WebRTC de-risk / QA test tools" OFF)
if(WITH_WEBRTC_TESTS)
    add_executable(webrtc_loopback src/webrtc_loopback.cpp)
    target_link_libraries(webrtc_loopback PRIVATE LibDataChannel::LibDataChannel)

    add_executable(webrtc_signaling src/webrtc_signaling.cpp)
    target_link_libraries(webrtc_signaling PRIVATE LibDataChannel::LibDataChannel ws2_32)

    foreach(t webrtc_audio webrtc_video webrtc_testsender)
        add_executable(${t} src/${t}.cpp)
        target_include_directories(${t} PRIVATE "${FFMPEG_ROOT}/include")
        target_link_directories(${t} PRIVATE "${FFMPEG_ROOT}/lib")
        target_link_libraries(${t} PRIVATE LibDataChannel::LibDataChannel avcodec avutil ws2_32)
    endforeach()
endif()

# ---------------------------------------------------------------------------
# latbench — end-to-end acoustic latency benchmark (WASAPI only, no webrtc dep).
# ---------------------------------------------------------------------------
option(WITH_LATBENCH "Build the latbench latency benchmark tool" OFF)
if(WITH_LATBENCH OR WITH_WEBRTC_TESTS)
    add_executable(latbench src/latbench.cpp)
    if(WIN32)
        target_link_libraries(latbench PRIVATE ole32)
    endif()
endif()

# Copy the FFmpeg runtime DLLs next to receiver.exe after each build.
file(GLOB FFMPEG_DLLS "${FFMPEG_ROOT}/bin/*.dll")
if(FFMPEG_DLLS)
    add_custom_command(TARGET receiver POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${FFMPEG_DLLS} $<TARGET_FILE_DIR:receiver>)
endif()
