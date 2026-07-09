#include "preview_window.h"

#include <windows.h>
#include <atomic>
#include <string>

struct PreviewWindow::Impl {
    HWND hwnd = nullptr;
    HINSTANCE inst = nullptr;
    bool classRegistered = false;
    std::atomic<bool> closed{false};
    int w = 0, h = 0;
};

namespace {
const wchar_t* kClass = L"PhoneCamPreview";

LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    if (m == WM_CLOSE) {
        auto* self = reinterpret_cast<PreviewWindow::Impl*>(GetWindowLongPtrW(h, GWLP_USERDATA));
        if (self) self->closed = true;
        DestroyWindow(h);
        return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}
} // namespace

PreviewWindow::PreviewWindow() : p_(new Impl) {}

PreviewWindow::~PreviewWindow() {
    if (p_ && p_->hwnd) DestroyWindow(p_->hwnd);
    delete p_;
    p_ = nullptr;
}

bool PreviewWindow::closed() const { return p_ && p_->closed.load(); }

void PreviewWindow::Pump() {
    if (!p_ || !p_->hwnd) return;
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

void PreviewWindow::SetTitle(const char* utf8) {
    if (!p_ || !p_->hwnd || !utf8) return;
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
    if (n <= 0) return;
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, w.data(), n);
    SetWindowTextW(p_->hwnd, w.c_str());
}

void PreviewWindow::ShowFrame(const uint8_t* bgr24, int w, int h) {
    Impl& s = *p_;
    if (s.closed.load()) return;

    if (!s.hwnd) {
        s.inst = GetModuleHandleW(nullptr);
        if (!s.classRegistered) {
            WNDCLASSW wc = {};
            wc.lpfnWndProc = WndProc;
            wc.hInstance = s.inst;
            wc.lpszClassName = kClass;
            wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
            RegisterClassW(&wc);
            s.classRegistered = true;
        }
        RECT r = {0, 0, w, h};
        AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
        s.hwnd = CreateWindowExW(0, kClass, L"PhoneCam preview", WS_OVERLAPPEDWINDOW,
                                 CW_USEDEFAULT, CW_USEDEFAULT,
                                 r.right - r.left, r.bottom - r.top,
                                 nullptr, nullptr, s.inst, nullptr);
        if (!s.hwnd) return;
        SetWindowLongPtrW(s.hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&s));
        ShowWindow(s.hwnd, SW_SHOW);
        s.w = w; s.h = h;
    }

    Pump(); // drain resize/close messages
    if (s.closed.load()) return;

    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;          // negative => top-down (matches our buffer)
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 24;        // 24-bit BGR
    bi.bmiHeader.biCompression = BI_RGB;

    RECT client;
    GetClientRect(s.hwnd, &client);
    int cw = client.right - client.left;
    int ch = client.bottom - client.top;
    if (cw <= 0 || ch <= 0) return;

    // Preserve the source aspect ratio (letterbox/pillarbox) so the image is never
    // stretched when the window isn't exactly w:h. (std::min is avoided here because
    // <windows.h> defines a conflicting min() macro.)
    double sx = (double)cw / w, sy = (double)ch / h;
    double scale = (sx < sy) ? sx : sy;
    int dw = (int)(w * scale + 0.5);
    int dh = (int)(h * scale + 0.5);
    int dx = (cw - dw) / 2;
    int dy = (ch - dh) / 2;

    HDC dc = GetDC(s.hwnd);
    // Paint only the margins black (avoids full-frame flicker on every frame).
    HBRUSH bg = (HBRUSH)GetStockObject(BLACK_BRUSH);
    if (dy > 0) { RECT t{0,0,cw,dy}, b{0,ch-dy,cw,ch}; FillRect(dc,&t,bg); FillRect(dc,&b,bg); }
    if (dx > 0) { RECT l{0,0,dx,ch}, r{cw-dx,0,cw,ch}; FillRect(dc,&l,bg); FillRect(dc,&r,bg); }
    SetStretchBltMode(dc, HALFTONE);
    StretchDIBits(dc,
                  dx, dy, dw, dh,        // dest (aspect-preserved, centered)
                  0, 0, w, h,            // src
                  bgr24, &bi, DIB_RGB_COLORS, SRCCOPY);
    ReleaseDC(s.hwnd, dc);
}
