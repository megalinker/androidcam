#include "video_sink.h"
#include "preview_window.h"

#include <chrono>
#include <cstdio>
#include <cstring>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

#ifdef HAVE_SOFTCAM
#include "SenderAPI.h"   // softcam::sender::CreateCamera / SendFrame / DeleteCamera
#endif

VideoSink::VideoSink(double fps, bool preview) : fps_(fps), wantPreview_(preview) {}

VideoSink::~VideoSink() { Stop(); }

bool VideoSink::ensure(const AVFrame *f) {
    int cw = f->width & ~3;    // softcam wants width/height a multiple of 4
    int ch = f->height & ~3;
    if (sws_ && w_ == cw && h_ == ch) return true;

    sws_freeContext(sws_);
    av_freep(&dst_[0]);
#ifdef HAVE_SOFTCAM
    if (cam_) { softcam::sender::DeleteCamera(cam_); cam_ = nullptr; }
#endif

    w_ = cw; h_ = ch;
    sws_ = sws_getContext(f->width, f->height, (AVPixelFormat)f->format,
                          cw, ch, AV_PIX_FMT_BGR24, SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!sws_) return false;
    if (av_image_alloc(dst_, dstLinesize_, cw, ch, AV_PIX_FMT_BGR24, 1) < 0) return false;

#ifdef HAVE_SOFTCAM
    cam_ = softcam::sender::CreateCamera(cw, ch, (float)fps_);
    if (!cam_) fprintf(stderr, "scCreateCamera(%d,%d,%.1f) failed — another softcam instance?\n", cw, ch, fps_);
#endif
    fprintf(stderr, "[video] %dx%d @ %.1f fps\n", cw, ch, fps_);
    return true;
}

bool VideoSink::WriteFrame(const AVFrame *frame) {
    if (!ensure(frame)) return false;
    // Spin up the preview window lazily on the first video frame (never for a mic-only session).
    if (wantPreview_ && !previewThread_.joinable()) {
        previewRun_ = true;
        previewThread_ = std::thread(&VideoSink::previewLoop, this);
    }
    sws_scale(sws_, (const uint8_t *const *)frame->data, frame->linesize, 0, frame->height,
              dst_, dstLinesize_);
#ifdef HAVE_SOFTCAM
    if (cam_) softcam::sender::SendFrame(cam_, dst_[0]);
#endif
    if (wantPreview_) {                       // hand the latest frame to the preview thread
        std::lock_guard<std::mutex> lk(pmutex_);
        size_t n = (size_t)w_ * h_ * 3;
        if (pbuf_.size() != n) pbuf_.resize(n);
        std::memcpy(pbuf_.data(), dst_[0], n);
        pw_ = w_; ph_ = h_; pdirty_ = true;
    }
    if ((++frames_ % 60) == 0) fprintf(stderr, "[video] %ld frames decoded (%dx%d)\n", frames_, w_, h_);
    return true;
}

// Owns the preview window (create / pump / show / destroy all on this one thread).
void VideoSink::previewLoop() {
    PreviewWindow win;
    std::vector<unsigned char> local;
    int lw = 0, lh = 0;
    while (previewRun_.load()) {
        win.Pump();
        if (win.closed()) break;
        if (pdirty_.exchange(false)) {
            std::lock_guard<std::mutex> lk(pmutex_);
            local = pbuf_; lw = pw_; lh = ph_;
            if (!local.empty()) win.ShowFrame(local.data(), lw, lh);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(8));
    }
}

void VideoSink::Stop() {
    if (previewRun_.exchange(false) && previewThread_.joinable()) previewThread_.join();
    sws_freeContext(sws_); sws_ = nullptr;
    av_freep(&dst_[0]);
#ifdef HAVE_SOFTCAM
    if (cam_) { softcam::sender::DeleteCamera(cam_); cam_ = nullptr; }
#endif
    w_ = h_ = 0;
}
