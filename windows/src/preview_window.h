// PreviewWindow — a dependency-free Win32/GDI window that displays decoded BGR24 frames.
// Lets you verify the full Wi-Fi -> decode path before softcam is built/registered.
// Enable with the receiver's --preview flag.
#pragma once

#include <cstdint>

class PreviewWindow {
public:
    PreviewWindow();
    ~PreviewWindow();

    PreviewWindow(const PreviewWindow&) = delete;
    PreviewWindow& operator=(const PreviewWindow&) = delete;

    // Display one top-down, packed 24-bit BGR frame. The window is created lazily on the
    // first call and sized to w x h. `w` must be a multiple of 4 so each row is DWORD-aligned
    // for GDI — the receiver's softcam geometry already guarantees that.
    void ShowFrame(const uint8_t* bgr24, int w, int h);

    // Process pending window messages (resize/close). ShowFrame() calls this; also call it
    // yourself during idle periods (e.g. while waiting to reconnect) to keep the window live.
    void Pump();

    // Update the window title bar (used for live fps/resolution stats). UTF-8.
    void SetTitle(const char* utf8);

    // True once the user closed the preview window.
    bool closed() const;

    // Opaque implementation, defined in preview_window.cpp. Declared public (as an
    // incomplete type) so the window procedure — a free function — can recover it
    // from GWLP_USERDATA; the pointer itself stays private.
    struct Impl;

private:
    Impl* p_ = nullptr;
};
