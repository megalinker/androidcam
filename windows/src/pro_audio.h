// RAII: register the calling thread with MMCSS "Pro Audio" for its lifetime, reverting on scope exit.
// WASAPI shared-mode render is soft-real-time; MMCSS reduces render-buffer underruns when the PC is
// under CPU load. No-op if registration fails. Win10 + Win11. See docs/perf-audit F-09.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <avrt.h>
#pragma comment(lib, "Avrt.lib")

struct ProAudioThread {
    HANDLE h_ = nullptr;
    DWORD  idx_ = 0;
    ProAudioThread() { h_ = AvSetMmThreadCharacteristicsW(L"Pro Audio", &idx_); }
    ~ProAudioThread() { if (h_) AvRevertMmThreadCharacteristics(h_); }
    ProAudioThread(const ProAudioThread&) = delete;
    ProAudioThread& operator=(const ProAudioThread&) = delete;
};
