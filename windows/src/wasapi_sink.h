// WasapiSink — renders decoded audio frames to a WASAPI *render* endpoint (shared mode).
//
// Phase-2 model (fully self-contained, no VB-CABLE): our own virtual-audio driver exposes a
// loopback pair — a render endpoint ("PhoneCam Audio") whose samples reappear on a capture
// endpoint ("PhoneCam Microphone") that apps select as their mic. receiver.exe just *plays*
// the decoded phone audio into that render endpoint via WASAPI; the driver does the loopback.
//
// Before the driver exists you can point this at the system default endpoint (empty match) and
// literally hear the phone's mic on your speakers — a complete, testable audio path today.
//
// All resampling to the endpoint's mix format is handled internally, so callers just hand over
// decoded AVFrames.
#pragma once

#include <string>

struct AVCodecContext;
struct AVFrame;

class WasapiSink {
public:
    WasapiSink();
    ~WasapiSink();

    WasapiSink(const WasapiSink&) = delete;
    WasapiSink& operator=(const WasapiSink&) = delete;

    // deviceMatch: case-insensitive substring of the render endpoint's friendly name
    // (e.g. "PhoneCam"), or empty to use the system default render endpoint.
    // `dec` supplies the source audio format (rate / channel layout / sample format).
    // gainDb boosts the (typically quiet) phone-mic level; a soft limiter after the gain
    // keeps peaks from clipping. 0 dB = passthrough.
    // eqPreset selects a voice EQ curve ("clarity" / "warm" / "bright" / "podcast"); "" or
    // "off" = no EQ. Applied per-channel before the gain/limiter.
    bool Init(const AVCodecContext* dec, const std::string& deviceMatch,
              float gainDb = 0.0f, const std::string& eqPreset = "");

    // Resample `frame` to the endpoint mix format and render it. Returns false on a fatal error.
    bool WriteFrame(const AVFrame* frame);

    void Stop();

private:
    struct Impl;
    Impl* p_ = nullptr;
};
