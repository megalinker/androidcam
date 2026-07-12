// PhoneCam receiver — pulls the phone's RTSP stream, decodes with FFmpeg, and pushes:
//   video -> tshino/softcam DirectShow virtual camera  (24-bit BGR, tightly packed)   [WITH_SOFTCAM]
//        \-> optional GDI preview window                                              [--preview]
//   audio -> a WASAPI render endpoint (the loopback "speaker" of our virtual mic, or
//            your real speakers for testing)                                          [wasapi_sink]
//
// Decode uses the modern FFmpeg 6.x/7.x API (send_packet / receive_frame, AVChannelLayout).
//
// Usage:
//   receiver.exe rtsp://<phone-ip>:8554/ [--preview] [--no-audio]
//                [--audio-device <name-substr>] [--udp]
//
// Build/run: see windows/README.md and docs/build-and-run.md.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <string>
#include <atomic>
#include <thread>
#include <chrono>
#include <vector>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

#include "wasapi_sink.h"
#include "preview_window.h"

#ifdef HAVE_SOFTCAM
// tshino/softcam exposes the plain-C sc* API only from its DLL; the static softcamcore.lib
// we link provides the equivalent C++ sender API (softcam::sender::*). Use that so receiver.exe
// stays self-contained (no runtime dependency on softcam.dll — apps still load the registered
// filter DLL and share frames via shared memory).
#include "SenderAPI.h"   // softcam::sender::CreateCamera / SendFrame / DeleteCamera
#endif

static std::atomic<bool> g_running{true};
static void on_sigint(int) { g_running = false; }

// av_err2str() is a C-only macro; use this in C++.
static std::string errstr(int e) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_strerror(e, buf, sizeof buf);
    return buf;
}

// ---- video conversion: decoded YUV -> packed BGR24 -> softcam and/or preview ----
struct VideoConv {
    SwsContext* sws = nullptr;
    uint8_t*    dst_data[4] = {0};
    int         dst_linesize[4] = {0};
    int         w = 0, h = 0;   // softcam geometry (multiples of 4)
    double      fps = 30.0;
    void*       cam = nullptr;  // scCamera handle (void*)
    PreviewWindow* preview = nullptr;
    std::chrono::steady_clock::time_point lastStat{};  // preview fps counter
    long        statFrames = 0;
    bool        flipH = false, flipV = false;   // mirror the output image
    std::vector<uint8_t> flipRow;               // scratch row for the vertical flip
};

// In-place mirror of a tightly-packed BGR24 image (w a multiple of 4, stride = dst_linesize).
static void flip_bgr24(uint8_t* data, int w, int h, int stride, bool fh, bool fv,
                       std::vector<uint8_t>& tmp) {
    if (fv) {                                   // swap row y with row (h-1-y)
        if ((int)tmp.size() < stride) tmp.resize(stride);
        for (int y = 0; y < h / 2; ++y) {
            uint8_t* a = data + (size_t)y * stride;
            uint8_t* b = data + (size_t)(h - 1 - y) * stride;
            memcpy(tmp.data(), a, stride);
            memcpy(a, b, stride);
            memcpy(b, tmp.data(), stride);
        }
    }
    if (fh) {                                   // reverse the pixels within each row
        for (int y = 0; y < h; ++y) {
            uint8_t* row = data + (size_t)y * stride;
            for (int x = 0; x < w / 2; ++x) {
                uint8_t* p = row + (size_t)x * 3;
                uint8_t* q = row + (size_t)(w - 1 - x) * 3;
                for (int c = 0; c < 3; ++c) { uint8_t t = p[c]; p[c] = q[c]; q[c] = t; }
            }
        }
    }
}

static int open_decoder(AVFormatContext* fmt, AVMediaType type,
                        int* stream_idx, AVCodecContext** dec_ctx, bool lowLatency) {
    int idx = av_find_best_stream(fmt, type, -1, -1, nullptr, 0);
    if (idx < 0) return idx;

    AVStream* st = fmt->streams[idx];
    const AVCodec* dec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!dec) return AVERROR_DECODER_NOT_FOUND;

    AVCodecContext* c = avcodec_alloc_context3(dec);
    if (!c) return AVERROR(ENOMEM);

    int ret = avcodec_parameters_to_context(c, st->codecpar);
    if (ret < 0) { avcodec_free_context(&c); return ret; }

    if (lowLatency) {
        c->flags  |= AV_CODEC_FLAG_LOW_DELAY;
        c->flags2 |= AV_CODEC_FLAG2_FAST;
        c->thread_count = 1;   // frame-threading buffers frames -> adds latency
    } else {
        c->thread_count = 0;   // auto threads: smoother throughput, higher latency
    }

    ret = avcodec_open2(c, dec, nullptr);
    if (ret < 0) { avcodec_free_context(&c); return ret; }

    *stream_idx = idx;
    *dec_ctx = c;
    return 0;
}

static int video_ensure(VideoConv* v, const AVFrame* f) {
    int cw = f->width  & ~3;   // softcam + GDI both want width a multiple of 4
    int ch = f->height & ~3;
    if (v->sws && v->w == cw && v->h == ch) return 0;

    sws_freeContext(v->sws);
    av_freep(&v->dst_data[0]);
#ifdef HAVE_SOFTCAM
    if (v->cam) { softcam::sender::DeleteCamera(v->cam); v->cam = nullptr; }
#endif

    v->w = cw; v->h = ch;
    v->sws = sws_getContext(f->width, f->height, (AVPixelFormat)f->format,
                            cw, ch, AV_PIX_FMT_BGR24,
                            SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!v->sws) return AVERROR(EINVAL);

    int ret = av_image_alloc(v->dst_data, v->dst_linesize, cw, ch, AV_PIX_FMT_BGR24, 1);
    if (ret < 0) return ret;

#ifdef HAVE_SOFTCAM
    v->cam = softcam::sender::CreateCamera(cw, ch, (float)v->fps);
    if (!v->cam)
        fprintf(stderr, "scCreateCamera(%d,%d,%.1f) failed — another softcam instance running?\n",
                cw, ch, v->fps);
#endif
    fprintf(stderr, "[video] %dx%d @ %.1f fps\n", cw, ch, v->fps);
    return 0;
}

static int video_process(VideoConv* v, const AVFrame* f) {
    int ret = video_ensure(v, f);
    if (ret < 0) return ret;

    sws_scale(v->sws, (const uint8_t* const*)f->data, f->linesize, 0, f->height,
              v->dst_data, v->dst_linesize);

    if (v->flipH || v->flipV)
        flip_bgr24(v->dst_data[0], v->w, v->h, v->dst_linesize[0], v->flipH, v->flipV, v->flipRow);

#ifdef HAVE_SOFTCAM
    if (v->cam) softcam::sender::SendFrame(v->cam, v->dst_data[0]);
#endif
    if (v->preview) {
        v->preview->ShowFrame(v->dst_data[0], v->w, v->h);
        auto now = std::chrono::steady_clock::now();
        if (v->lastStat.time_since_epoch().count() == 0) v->lastStat = now;
        v->statFrames++;
        double dt = std::chrono::duration<double>(now - v->lastStat).count();
        if (dt >= 1.0) {
            char title[160];
            snprintf(title, sizeof title, "PhoneCam preview  |  %dx%d  @ %.1f fps",
                     v->w, v->h, v->statFrames / dt);
            v->preview->SetTitle(title);
            v->statFrames = 0;
            v->lastStat = now;
        }
    }

#ifndef HAVE_SOFTCAM
    if (!v->preview) {
        static long n = 0;
        if ((n++ % 60) == 0)
            fprintf(stderr, "[video] decoded frame %ld (--preview to view, -DWITH_SOFTCAM=ON to output)\n", n);
    }
#endif
    return 0;
}

// Send one packet (NULL to flush), then drain every ready frame.
static int decode(AVCodecContext* c, AVPacket* pkt, AVFrame* frame,
                  int (*on_frame)(void*, const AVFrame*), void* user) {
    int ret = avcodec_send_packet(c, pkt);
    if (ret < 0 && ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) return ret;

    for (;;) {
        ret = avcodec_receive_frame(c, frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) return 0;
        if (ret < 0) return ret;
        ret = on_frame(user, frame);
        av_frame_unref(frame);
        if (ret < 0) return ret;
    }
}

static int on_video(void* u, const AVFrame* f) { return video_process((VideoConv*)u, f); }
static int on_audio(void* u, const AVFrame* f) {
    return ((WasapiSink*)u)->WriteFrame(f) ? 0 : AVERROR_EXTERNAL;
}

struct Options {
    const char* url = nullptr;
    bool preview = false;
    bool noAudio = false;
    bool udp = false;
    bool smooth = false;   // trade latency for jitter resistance
    bool flipH = false, flipV = false;   // mirror the webcam image
    std::string audioDevice;
    float micGainDb = 0.0f;   // boost the (quiet) phone mic; soft-limited in the sink
};

static Options parse_args(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--preview") o.preview = true;
        else if (a == "--no-audio") o.noAudio = true;
        else if (a == "--udp") o.udp = true;
        else if (a == "--smooth") o.smooth = true;
        else if (a == "--flip-h" || a == "--mirror") o.flipH = true;
        else if (a == "--flip-v") o.flipV = true;
        else if (a == "--audio-device" && i + 1 < argc) o.audioDevice = argv[++i];
        else if (a == "--mic-gain" && i + 1 < argc) o.micGainDb = (float)atof(argv[++i]);
        else if (a.rfind("--", 0) == 0) fprintf(stderr, "ignoring unknown option: %s\n", a.c_str());
        else o.url = argv[i];
    }
    return o;
}

// Run one RTSP session end-to-end. Returns 0 for a clean stop (Ctrl+C or preview closed),
// negative if the stream failed/ended and the caller should reconnect. `preview` (may be null)
// persists across reconnects so the window isn't torn down between attempts.
static int run_session(const Options& opt, PreviewWindow* preview) {
    AVDictionary* opts = nullptr;
    av_dict_set(&opts, "rtsp_transport", opt.udp ? "udp" : "tcp", 0);
    av_dict_set(&opts, "rtsp_flags",     "prefer_tcp", 0);
    av_dict_set(&opts, "timeout",        "5000000", 0);
    if (opt.smooth) {
        // Absorb Wi-Fi jitter with a reorder buffer + demux delay (smoother, higher latency).
        av_dict_set(&opts, "reorder_queue_size", "2048", 0);
        av_dict_set(&opts, "max_delay",          "500000", 0);
    } else {
        // Lowest latency: no reordering, no demux buffering.
        av_dict_set(&opts, "reorder_queue_size", "0", 0);
        av_dict_set(&opts, "max_delay",          "0", 0);
        // Don't spend the default 5s / 5MB analysing a stream we already know (H264 + AAC): cap it so
        // the first frame shows quickly instead of a long "Connecting…" pause.
        av_dict_set(&opts, "probesize",          "1000000", 0);   // 1 MB
        av_dict_set(&opts, "analyzeduration",    "1000000", 0);   // 1 s
    }

    AVFormatContext* fmt = avformat_alloc_context();
    if (!fmt) { av_dict_free(&opts); return -1; }
    if (!opt.smooth) fmt->flags |= AVFMT_FLAG_NOBUFFER | AVFMT_FLAG_FLUSH_PACKETS;

    int ret = avformat_open_input(&fmt, opt.url, nullptr, &opts);
    av_dict_free(&opts);
    if (ret < 0) { fprintf(stderr, "open_input(%s): %s\n", opt.url, errstr(ret).c_str()); return -1; }

    if ((ret = avformat_find_stream_info(fmt, nullptr)) < 0) {
        fprintf(stderr, "find_stream_info: %s\n", errstr(ret).c_str());
        avformat_close_input(&fmt);
        return -1;
    }
    av_dump_format(fmt, 0, opt.url, 0);

    int vstream = -1, astream = -1;
    AVCodecContext* vdec = nullptr;
    AVCodecContext* adec = nullptr;
    if (open_decoder(fmt, AVMEDIA_TYPE_VIDEO, &vstream, &vdec, !opt.smooth) < 0)
        fprintf(stderr, "no usable video stream (mic-only mode?)\n");
    if (open_decoder(fmt, AVMEDIA_TYPE_AUDIO, &astream, &adec, !opt.smooth) < 0)
        fprintf(stderr, "no usable audio stream (camera-only mode?)\n");

    WasapiSink sink;
    bool audioReady = false;
    if (adec && !opt.noAudio) {
        audioReady = sink.Init(adec, opt.audioDevice, opt.micGainDb);
        if (!audioReady) fprintf(stderr, "[audio] sink init failed; continuing without audio\n");
    }

    VideoConv vconv;
    if (vdec) {
        double fps = av_q2d(fmt->streams[vstream]->avg_frame_rate);
        vconv.fps = (fps > 0.0) ? fps : 30.0;
        vconv.preview = preview;
        vconv.flipH = opt.flipH;
        vconv.flipV = opt.flipV;
    }

    int rc = -1; // assume disconnect unless we detect a clean stop
    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    if (pkt && frame) {
        while (g_running) {
            ret = av_read_frame(fmt, pkt);
            if (ret < 0) { rc = -1; break; }   // EOF / timeout / error -> reconnect
            if (vdec && pkt->stream_index == vstream)
                decode(vdec, pkt, frame, on_video, &vconv);
            else if (adec && audioReady && pkt->stream_index == astream)
                decode(adec, pkt, frame, on_audio, &sink);
            av_packet_unref(pkt);
            if (preview && preview->closed()) { rc = 0; break; }
        }
        if (!g_running) rc = 0;
        if (vdec) decode(vdec, nullptr, frame, on_video, &vconv);
    }

    av_frame_free(&frame);
    av_packet_free(&pkt);

    sink.Stop();
    sws_freeContext(vconv.sws);
    av_freep(&vconv.dst_data[0]);
#ifdef HAVE_SOFTCAM
    if (vconv.cam) softcam::sender::DeleteCamera(vconv.cam);
#endif
    avcodec_free_context(&vdec);
    avcodec_free_context(&adec);
    avformat_close_input(&fmt);
    return rc;
}

int main(int argc, char** argv) {
    Options opt = parse_args(argc, argv);
    if (!opt.url) {
        fprintf(stderr,
            "usage: %s rtsp://<phone-ip>:8554/ [--preview] [--no-audio] "
            "[--audio-device <name-substr>] [--mic-gain <db>] [--udp] [--smooth] [--flip-h] [--flip-v]\n", argv[0]);
        return 1;
    }
    signal(SIGINT, on_sigint);
    avformat_network_init();

    fprintf(stderr, "[phonecam] url=%s  transport=%s  pacing=%s  audio=%s\n",
            opt.url, opt.udp ? "udp" : "tcp", opt.smooth ? "smooth" : "low-latency",
            opt.noAudio ? "off" : (opt.audioDevice.empty() ? "default-output" : opt.audioDevice.c_str()));

    PreviewWindow preview;
    PreviewWindow* pv = opt.preview ? &preview : nullptr;

    // Reconnect loop: the phone may sleep, roam networks, or the app may be restarted.
    while (g_running) {
        int rc = run_session(opt, pv);
        if (rc == 0) break;                              // clean stop: Ctrl+C or window closed
        if (!g_running || (pv && pv->closed())) break;
        fprintf(stderr, "[net] stream ended/unreachable — reconnecting in 2s (Ctrl+C to quit)\n");
        for (int i = 0; i < 20 && g_running; ++i) {
            if (pv) { pv->Pump(); if (pv->closed()) { g_running = false; break; } }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    avformat_network_deinit();
    return 0;
}
