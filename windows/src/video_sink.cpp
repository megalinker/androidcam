#include "video_sink.h"
#include "preview_window.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <utility>

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

// Manual transform state (global; see video_sink.h). Read per frame by whatever VideoSink is live.
namespace {
std::atomic<int>  g_rotate{0};
std::atomic<bool> g_flipH{false};
std::atomic<bool> g_flipV{false};
}
void VideoSetRotate(int deg) { g_rotate = ((deg % 360) + 360) % 360; }
void VideoSetFlipH(bool on)  { g_flipH = on; }
void VideoSetFlipV(bool on)  { g_flipV = on; }

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

// Mirror a packed BGR24 image (w×h) in place: fh = left/right, fv = top/bottom.
static void flipBgr(unsigned char *buf, int w, int h, bool fh, bool fv) {
    const int p = 3;
    if (fv)
        for (int y = 0; y < h / 2; ++y) {
            unsigned char *a = buf + (size_t)y * w * p, *b = buf + (size_t)(h - 1 - y) * w * p;
            for (int i = 0; i < w * p; ++i) std::swap(a[i], b[i]);
        }
    if (fh)
        for (int y = 0; y < h; ++y) {
            unsigned char *row = buf + (size_t)y * w * p;
            for (int x = 0; x < w / 2; ++x) {
                unsigned char *a = row + x * p, *b = row + (w - 1 - x) * p;
                std::swap(a[0], b[0]); std::swap(a[1], b[1]); std::swap(a[2], b[2]);
            }
        }
}

bool VideoSink::ensure(const AVFrame *f) {
    // Fix the output geometry ONCE, from the first frame's aspect (short side -> 720). The virtual
    // camera then keeps a stable resolution for the whole call (Zoom/Teams glitch on mid-call changes)
    // and softcam is never recreated on the WebRTC resolution ramp — that churn was crashing us.
    if (targetW_ == 0) {
        if (f->width >= f->height) { targetH_ = 720; targetW_ = ((720 * f->width  / f->height) + 2) & ~3; }
        else                       { targetW_ = 720; targetH_ = ((720 * f->height / f->width)  + 2) & ~3; }
    }

    // Scaler: (re)build only when the SOURCE resolution changes (the ramp). dst_ is the fixed target.
    if (!sws_ || srcW_ != f->width || srcH_ != f->height) {
        sws_freeContext(sws_);
        srcW_ = f->width; srcH_ = f->height;
        sws_ = sws_getContext(f->width, f->height, (AVPixelFormat)f->format,
                              targetW_, targetH_, AV_PIX_FMT_BGR24, SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!sws_) return false;
        if (!dst_[0] && av_image_alloc(dst_, dstLinesize_, targetW_, targetH_, AV_PIX_FMT_BGR24, 1) < 0) return false;
        fprintf(stderr, "[video] source %dx%d -> %dx%d\n", f->width, f->height, targetW_, targetH_);
    }

    // Softcam + output buffer: (re)build only when the OUTPUT geometry changes — the first frame or a
    // manual rotation that swaps W/H. NEVER on the ramp.
    int rot = g_rotate.load();
    int now = (rot == 90 || rot == 270) ? targetH_ : targetW_;
    int noh = (rot == 90 || rot == 270) ? targetW_ : targetH_;
    if (ow_ != now || oh_ != noh || rot_ != rot) {
        bool geomChanged = (ow_ != now || oh_ != noh);
        rot_ = rot; ow_ = now; oh_ = noh;
        if (geomChanged) {
            obuf_.resize((size_t)ow_ * oh_ * 3);
#ifdef HAVE_SOFTCAM
            if (cam_) { softcam::sender::DeleteCamera(cam_); cam_ = nullptr; }
            cam_ = softcam::sender::CreateCamera(ow_, oh_, (float)fps_);
            if (!cam_) fprintf(stderr, "scCreateCamera(%d,%d,%.1f) failed — another softcam instance?\n", ow_, oh_, fps_);
#endif
            fprintf(stderr, "[video] output %dx%d @ %.1f fps%s\n", ow_, oh_, fps_, rot ? " (rotated)" : "");
        }
    }
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
    // Apply the manual rotate + mirror (never automatic). Passthrough when nothing is set.
    bool fh = g_flipH.load(), fv = g_flipV.load();
    const unsigned char *outbuf; int outw, outh;
    if (rot_ == 0 && !fh && !fv) { outbuf = dst_[0]; outw = targetW_; outh = targetH_; }
    else {
        if (rot_ == 0) std::memcpy(obuf_.data(), dst_[0], (size_t)targetW_ * targetH_ * 3);
        else rotateBgr(dst_[0], targetW_, targetH_, rot_, obuf_.data());
        if (fh || fv) flipBgr(obuf_.data(), ow_, oh_, fh, fv);
        outbuf = obuf_.data(); outw = ow_; outh = oh_;
    }
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
    srcW_ = srcH_ = targetW_ = targetH_ = ow_ = oh_ = rot_ = 0;
}
