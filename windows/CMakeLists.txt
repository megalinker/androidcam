cmake_minimum_required(VERSION 3.20)
project(phonecam_receiver LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---------------------------------------------------------------------------
# FFmpeg (shared dev build). Download per windows/README.md and extract into
# windows/third_party/ffmpeg so you have include/ lib/ bin/ under it.
# ---------------------------------------------------------------------------
set(FFMPEG_ROOT "${CMAKE_SOURCE_DIR}/third_party/ffmpeg" CACHE PATH "FFmpeg dev build root")
if(NOT EXISTS "${FFMPEG_ROOT}/include")
    message(FATAL_ERROR "FFmpeg not found at ${FFMPEG_ROOT}. See windows/README.md.")
endif()

add_executable(receiver
    src/receiver.cpp
    src/wasapi_sink.cpp      # audio -> WASAPI render endpoint (virtual mic loopback / speakers)
    src/preview_window.cpp)  # optional GDI video preview (--preview)

target_include_directories(receiver PRIVATE "${FFMPEG_ROOT}/include")
target_link_directories(receiver PRIVATE "${FFMPEG_ROOT}/lib")
target_link_libraries(receiver PRIVATE avformat avcodec avutil swscale swresample)

# Windows system libs: WASAPI/COM (ole32), GDI preview (gdi32/user32).
if(WIN32)
    target_link_libraries(receiver PRIVATE ole32 gdi32 user32)
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
    # The static softcamcore.lib exposes the C++ sender API (softcam::sender::*) in SenderAPI.h.
    # (The plain-C sc* wrappers live only in softcam.dll; linking the core lib keeps receiver.exe
    # self-contained.)
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
# WebRTC media path (libdatachannel) — Phase 0 of docs/webrtc-migration.md.
# OFF by default so the RTSP/SRT receiver keeps building unchanged. Enable with:
#   vcpkg install libdatachannel:x64-windows
#   cmake -B build-webrtc -A x64 -DWITH_WEBRTC=ON \
#         -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake
# libdatachannel gives us DTLS-SRTP + RTP; codecs stay on FFmpeg, sinks stay as-is.
# ---------------------------------------------------------------------------
option(WITH_WEBRTC "Build the WebRTC (libdatachannel) receiver path" OFF)
if(WITH_WEBRTC)
    find_package(LibDataChannel CONFIG REQUIRED)
    target_sources(receiver PRIVATE src/webrtc_receiver.cpp)   # --webrtc receive path
    target_link_libraries(receiver PRIVATE LibDataChannel::LibDataChannel ws2_32)
    target_compile_definitions(receiver PRIVATE HAVE_WEBRTC)
    # Phase 1 de-risk: in-process DTLS-SRTP media loopback.
    add_executable(webrtc_loopback src/webrtc_loopback.cpp)
    target_link_libraries(webrtc_loopback PRIVATE LibDataChannel::LibDataChannel)
    # Phase 1b: PCAM3 TCP signaling (SDP offer/answer over the wire) + full WebRTC bring-up.
    add_executable(webrtc_signaling src/webrtc_signaling.cpp)
    target_link_libraries(webrtc_signaling PRIVATE LibDataChannel::LibDataChannel ws2_32)
    # Phase 2 codec bridge: libdatachannel RTP <-> FFmpeg Opus (encode tone -> RTP -> decode -> verify).
    add_executable(webrtc_audio src/webrtc_audio.cpp)
    target_include_directories(webrtc_audio PRIVATE "${FFMPEG_ROOT}/include")
    target_link_directories(webrtc_audio PRIVATE "${FFMPEG_ROOT}/lib")
    target_link_libraries(webrtc_audio PRIVATE LibDataChannel::LibDataChannel avcodec avutil)
    # Phase 2b: phone stand-in — PCAM3 client that sends a real Opus tone (validates receiver.exe --webrtc).
    add_executable(webrtc_testsender src/webrtc_testsender.cpp)
    target_include_directories(webrtc_testsender PRIVATE "${FFMPEG_ROOT}/include")
    target_link_directories(webrtc_testsender PRIVATE "${FFMPEG_ROOT}/lib")
    target_link_libraries(webrtc_testsender PRIVATE LibDataChannel::LibDataChannel avcodec avutil ws2_32)
    message(STATUS "WebRTC path enabled (libdatachannel)")
endif()

# ---------------------------------------------------------------------------
# latbench — end-to-end acoustic latency benchmark (WASAPI only, no webrtc dep).
# Drives the Phase-4 go/no-go gate (SRT baseline vs WebRTC). Built with the webrtc
# tools for convenience; enable standalone with -DWITH_LATBENCH=ON.
# ---------------------------------------------------------------------------
option(WITH_LATBENCH "Build the latbench latency benchmark tool" OFF)
if(WITH_LATBENCH OR WITH_WEBRTC)
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
