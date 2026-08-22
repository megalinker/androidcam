// VideoSink — decoded YUV AVFrames -> packed BGR24 -> tshino/softcam virtual camera, and
// (optionally) an embedded GDI preview window. Used by the WebRTC path (webrtc_receiver.cpp).
// Softcam output is compiled in only under HAVE_SOFTCAM; otherwise WriteFrame just tracks stats.
//
// Threading: WriteFrame runs on libdatachannel's decode thread. Win32 windows are thread-affine,
// so the preview lives on its own thread that owns the window (create / pump / show / destroy);
// WriteFrame only copies the latest BGR frame into a shared buffer for it. Mirrors the audio thread.
#pragma once

#include <atomic>
#include <mutex>
#include <thread>
#include <vector>

struct AVFrame;
struct SwsContext;

class VideoSink {
public:
    explicit VideoSink(double fps = 30.0, bool preview = false);
    ~VideoSink();

    VideoSink(const VideoSink &) = delete;
    VideoSink &operator=(const VideoSink &) = delete;

    // Convert + push one decoded frame. Returns false on a fatal conversion error.
    bool WriteFrame(const AVFrame *frame);
    void Stop();

    long frames() const { return frames_; }
    int  width() const { return ow_; }
    int  height() const { return oh_; }

private:
    bool ensure(const AVFrame *f);
    // True while we are still waiting for the phone to tell us its geometry/rotation. Bounded, so an
    // older phone that never reports simply falls back after the timeout.
    bool awaitingPhoneGeometry();
    void previewLoop();

    double      fps_ = 30.0;
    bool        wantPreview_ = false;
    SwsContext *sws_ = nullptr;
    unsigned char *dst_[4] = {nullptr, nullptr, nullptr, nullptr};
    int         dstLinesize_[4] = {0, 0, 0, 0};
    int         srcW_ = 0, srcH_ = 0;           // last source (decoded) size — drives the sws rebuild on the ramp
    int         targetW_ = 0, targetH_ = 0;     // FIXED scaler output, chosen once from the first frame
    int         rot_ = 0, ow_ = 0, oh_ = 0;     // rotation + softcam/preview output dims (target, rotated)
    std::vector<unsigned char> obuf_;           // transformed BGR (used when any rotate/flip is active)
    void       *cam_ = nullptr;   // softcam handle (void* to keep the header softcam-free)
    long        frames_ = 0;
    unsigned long long firstFrameUs_ = 0;   // when video started, for the geometry-wait timeout

    // Preview thread + shared latest-BGR frame.
    std::thread              previewThread_;
    std::atomic<bool>        previewRun_{false};
    std::mutex               pmutex_;
    std::vector<unsigned char> pbuf_;
    int                      pw_ = 0, ph_ = 0;
    std::atomic<bool>        pdirty_{false};
};

// Manual output transform — set from the receiver's --flip-h/--flip-v/--rotate flags and updated
// LIVE from the GUI's Flip/Rotate buttons (over the receiver's stdin). NEVER automatic. Global so a
// control thread can change it without holding the (per-session) VideoSink; the sink reads it per frame.
void VideoSetRotate(int deg);    // 0/90/180/270 clockwise
void VideoSetFlipH(bool on);     // mirror left/right
void VideoSetFlipV(bool on);     // flip top/bottom
void VideoSetPreviewVisible(bool on);   // GUI tells us when its embedded preview is hidden (F-34)

/// The phone's video geometry and the rotation IT IS NO LONGER APPLYING, from its status message.
///
/// Measured on a Pixel 9 Pro XL: rotating frames on the phone before encoding cost ~6 points of one
/// CPU core at 720p and ~42 points at 1080p — the single largest avoidable draw we found. So the
/// phone now sends sensor-native frames and tells us the angle, and we rotate here, where a BGR
/// rotate is free next to everything else the desktop is doing.
///
/// This is NOT a change to the "orientation is manual" rule: the phone was already auto-orienting
/// (libwebrtc baked device rotation into the pixels). The automatic part just moved to the cheap
/// side. The user's manual Rotate/Mirror/Flip still compose on top of it.
///
/// @param w,h  the phone's capture geometry BEFORE rotation (0 = not reported)
/// @param rotationDeg  clockwise degrees to apply (-1 = the phone did not report; it rotated itself)
void VideoSetPhoneGeometry(int w, int h, int rotationDeg);
