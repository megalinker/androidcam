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

void VideoSink::SetRotation(int deg) { rotation_ = ((deg % 360) + 360) % 360; }

// Rotate a packed BGR24 image (w×h) clockwise by deg into out (whose dims are per-deg). No-op for 0.
static void rotateBgr(const unsigned char *src, int w, int h, int deg, unsigned char *out) {
    const int p = 3;
    if (deg == 90) {                    // out is h×w
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                std::memcpy(out + (x * h + (h - 1 - y)) * p, src + (y * w + x) * p, p);
    } else if (deg == 270) {            // out is h×w
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                std::memcpy(out + ((w - 1 - x) * h + y) * p, src + (y * w + x) * p, p);
    } else if (deg == 180) {            // out is w×h
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                std::memcpy(out + ((h - 1 - y) * w + (w - 1 - x)) * p, src + (y * w + x) * p, p);
    }
}

bool VideoSink::ensure(const AVFrame *f) {
    int cw = f->width & ~3;    // softcam wants width/height a multiple of 4
    int ch = f->height & ~3;
    int rot = rotation_.load();
    if (sws_ && w_ == cw && h_ == ch && rot_ == rot) return true;

    sws_freeContext(sws_);
    av_freep(&dst_[0]);
#ifdef HAVE_SOFTCAM
    if (cam_) { softcam::sender::DeleteCamera(cam_); cam_ = nullptr; }
#endif

    w_ = cw; h_ = ch; rot_ = rot;
    // Output (post-rotation) dims: 90/270 swap width and height.
    ow_ = (rot == 90 || rot == 270) ? ch : cw;
    oh_ = (rot == 90 || rot == 270) ? cw : ch;
    sws_ = sws_getContext(f->width, f->height, (AVPixelFormat)f->format,
                          cw, ch, AV_PIX_FMT_BGR24, SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!sws_) return false;
    if (av_image_alloc(dst_, dstLinesize_, cw, ch, AV_PIX_FMT_BGR24, 1) < 0) return false;
    if (rot != 0) rbuf_.resize((size_t)ow_ * oh_ * 3);

#ifdef HAVE_SOFTCAM
    cam_ = softcam::sender::CreateCamera(ow_, oh_, (float)fps_);
    if (!cam_) fprintf(stderr, "scCreateCamera(%d,%d,%.1f) failed — another softcam instance?\n", ow_, oh_, fps_);
#endif
    fprintf(stderr, "[video] %dx%d @ %.1f fps%s\n", ow_, oh_, fps_, rot ? " (rotated)" : "");
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
    // Apply rotation (if any) into rbuf_; otherwise use the swscale output directly.
    const unsigned char *outbuf; int outw, outh;
    if (rot_ == 0) { outbuf = dst_[0]; outw = w_; outh = h_; }
    else { rotateBgr(dst_[0], w_, h_, rot_, rbuf_.data()); outbuf = rbuf_.data(); outw = ow_; outh = oh_; }
#ifdef HAVE_SOFTCAM
    if (cam_) softcam::sender::SendFrame(cam_, outbuf);
#endif
    if (wantPreview_) {                       // hand the latest frame to the preview thread
        std::lock_guard<std::mutex> lk(pmutex_);
        size_t n = (size_t)outw * outh * 3;
        if (pbuf_.size() != n) pbuf_.resize(n);
        std::memcpy(pbuf_.data(), outbuf, n);
        pw_ = outw; ph_ = outh; pdirty_ = true;
    }
    if ((++frames_ % 60) == 0) fprintf(stderr, "[video] %ld frames decoded (%dx%d)\n", frames_, outw, outh);
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
