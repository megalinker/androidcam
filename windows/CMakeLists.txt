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
    if(NOT EXISTS "${SOFTCAM_ROOT}/src/softcam/softcam.h")
        message(FATAL_ERROR "softcam.h not found under ${SOFTCAM_ROOT}/src/softcam")
    endif()
    if(NOT SOFTCAM_LIB)
        message(FATAL_ERROR "Set -DSOFTCAM_LIB=<path>/softcamcore.lib")
    endif()
    target_compile_definitions(receiver PRIVATE HAVE_SOFTCAM)
    target_include_directories(receiver PRIVATE "${SOFTCAM_ROOT}/src/softcam")
    target_link_libraries(receiver PRIVATE "${SOFTCAM_LIB}")
endif()

# Copy the FFmpeg runtime DLLs next to receiver.exe after each build.
file(GLOB FFMPEG_DLLS "${FFMPEG_ROOT}/bin/*.dll")
if(FFMPEG_DLLS)
    add_custom_command(TARGET receiver POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${FFMPEG_DLLS} $<TARGET_FILE_DIR:receiver>)
endif()
