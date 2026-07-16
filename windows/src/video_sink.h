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
    int  width() const { return w_; }
    int  height() const { return h_; }

private:
    bool ensure(const AVFrame *f);
    void previewLoop();

    double      fps_ = 30.0;
    bool        wantPreview_ = false;
    SwsContext *sws_ = nullptr;
    unsigned char *dst_[4] = {nullptr, nullptr, nullptr, nullptr};
    int         dstLinesize_[4] = {0, 0, 0, 0};
    int         w_ = 0, h_ = 0;
    void       *cam_ = nullptr;   // softcam handle (void* to keep the header softcam-free)
    long        frames_ = 0;

    // Preview thread + shared latest-BGR frame.
    std::thread              previewThread_;
    std::atomic<bool>        previewRun_{false};
    std::mutex               pmutex_;
    std::vector<unsigned char> pbuf_;
    int                      pw_ = 0, ph_ = 0;
    std::atomic<bool>        pdirty_{false};
};
