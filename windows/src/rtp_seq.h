// RTP sequence-number continuity tracking — pure logic, no dependencies, unit-testable.
//
// Used by RtpLossMonitor (rtp_loss.h) to answer one question per decoded frame: "did any packets go
// missing since the last frame?". See rtp_loss.h for why that question has to be asked at all.
#pragma once

#include <atomic>
#include <cstdint>

/// Tracks a single RTP stream's sequence numbers. Thread-safety: observe() is expected to be called
/// from one receive thread; the counters are atomics so any thread may read them.
class RtpSeqTracker {
public:
    /// Feed one packet's 16-bit sequence number, in arrival order.
    void observe(uint16_t seq) {
        received_.fetch_add(1, std::memory_order_relaxed);
        if (!primed_) { primed_ = true; expected_ = (uint16_t)(seq + 1); return; }
        int16_t delta = (int16_t)(seq - expected_);   // signed 16-bit: wraps correctly at 2^16
        if (delta == 0) {                             // in order — the common case
            expected_ = (uint16_t)(seq + 1);
            return;
        }
        if (delta > 0) {
            // Forward jump: `delta` packets never arrived. A very large jump is a stream restart or a
            // resync rather than loss, so it is not attributed to the loss figure.
            if (delta < kMaxGap) {
                lost_.fetch_add((uint64_t)delta, std::memory_order_relaxed);
                gapEvents_.fetch_add(1, std::memory_order_relaxed);
                gapPending_.store(true, std::memory_order_relaxed);
            }
            expected_ = (uint16_t)(seq + 1);
        } else {
            // Arrived out of order. Reordering is NOT loss: do not ask for a keyframe, and give back
            // the packet we previously counted as missing so the loss figure stays honest.
            if (delta == -1) duplicates_.fetch_add(1, std::memory_order_relaxed);
            else             reordered_.fetch_add(1, std::memory_order_relaxed);
            uint64_t l = lost_.load(std::memory_order_relaxed);
            while (l > 0 && !lost_.compare_exchange_weak(l, l - 1, std::memory_order_relaxed)) {}
        }
    }

    /// True (and cleared) if a gap occurred since the previous call.
    bool consumeGap() { return gapPending_.exchange(false, std::memory_order_relaxed); }

    uint64_t received()   const { return received_.load(std::memory_order_relaxed); }
    uint64_t lost()       const { return lost_.load(std::memory_order_relaxed); }
    uint64_t gaps()       const { return gapEvents_.load(std::memory_order_relaxed); }
    uint64_t reordered()  const { return reordered_.load(std::memory_order_relaxed); }
    uint64_t duplicates() const { return duplicates_.load(std::memory_order_relaxed); }

    /// Packets estimated lost as a percentage of packets expected, over the whole session.
    double lossPercent() const {
        uint64_t r = received(), l = lost();
        return (r + l) ? (100.0 * (double)l / (double)(r + l)) : 0.0;
    }

    static constexpr int kMaxGap = 1000;   // beyond this it is a restart, not packet loss

private:
    bool     primed_ = false;
    uint16_t expected_ = 0;
    std::atomic<uint64_t> received_{0}, lost_{0}, gapEvents_{0}, reordered_{0}, duplicates_{0};
    std::atomic<bool>     gapPending_{false};
};
