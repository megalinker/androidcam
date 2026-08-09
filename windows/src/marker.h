// "Mark video problem" — a one-shot, near-free correlation marker.
//
// The operator sees the artifact and presses a button in the desktop app; the app writes
// `mark <note>` to receiver.exe's stdin (the same channel that already carries fliph/rotate/preview).
// The receiver stamps its own log with the marker plus a snapshot of the numbers that matter right
// then (packet loss, gaps, keyframe requests, decoder errors, queue depth), and forwards the mark to
// the phone so both sides' logs can be lined up afterwards.
//
// Deliberately NOT a video recorder: the brief is a timestamp, not frames. Cost when unused is one
// relaxed atomic load per receive-loop iteration (which already runs at 100–500 ms).
#pragma once

#include <atomic>
#include <mutex>
#include <string>

namespace marker {

inline std::atomic<unsigned> g_seq{0};   // bumped by the stdin thread on every mark
inline std::mutex            g_mutex;
inline std::string           g_note;

/// Called from receiver.cpp's stdin thread.
inline void raise(const std::string &note) {
    { std::lock_guard<std::mutex> lk(g_mutex); g_note = note; }
    g_seq.fetch_add(1, std::memory_order_release);
}

/// Poll from a receive loop. Returns true once per raise(); fills `note` with the operator's text.
///
/// A consumer starts from the CURRENT sequence, so a mark raised before this session began is not
/// replayed into it. Sessions are created per phone connection, and re-emitting a mark from the
/// previous connection would point the investigation at the wrong moment.
class Consumer {
public:
    Consumer() : seen_(g_seq.load(std::memory_order_acquire)) {}

    bool poll(std::string &note) {
        unsigned s = g_seq.load(std::memory_order_acquire);
        if (s == seen_) return false;
        seen_ = s;
        std::lock_guard<std::mutex> lk(g_mutex);
        note = g_note;
        return true;
    }
private:
    unsigned seen_;
};

}  // namespace marker
