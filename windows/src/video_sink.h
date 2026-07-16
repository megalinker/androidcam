// VideoSink — decoded YUV AVFrames -> packed BGR24 -> tshino/softcam virtual camera.
// Shared by the RTSP path (receiver.cpp) conceptually and the WebRTC path (webrtc_receiver.cpp).
// Softcam output is compiled in only under HAVE_SOFTCAM; otherwise WriteFrame just tracks stats so
// the decode path can still be validated. Geometry (multiple of 4) is derived from the first frame
// and re-derived if it changes.
#pragma once

struct AVFrame;
struct SwsContext;

class VideoSink {
public:
    explicit VideoSink(double fps = 30.0) : fps_(fps) {}
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

    double      fps_ = 30.0;
    SwsContext *sws_ = nullptr;
    unsigned char *dst_[4] = {nullptr, nullptr, nullptr, nullptr};
    int         dstLinesize_[4] = {0, 0, 0, 0};
    int         w_ = 0, h_ = 0;
    void       *cam_ = nullptr;   // softcam handle (void* to keep the header softcam-free)
    long        frames_ = 0;
};
