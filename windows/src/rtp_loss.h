// RTP sequence-continuity monitor for the WebRTC video track.
//
// WHY THIS EXISTS
// ---------------
// libdatachannel's H264RtpDepacketizer reassembles a frame from whatever packets arrived between two
// RTP timestamps and then emits it **unconditionally** — a frame that lost packets is delivered
// looking exactly like a complete one (see h264rtpdepacketizer.cpp: a sequence gap only clears
// `continuousFragments`, it never suppresses or flags the frame). Nothing above the depacketizer can
// tell "whole frame" from "frame with a hole in it".
//
// Meanwhile the receiver never asks for a retransmission: the offer advertises `a=rtcp-fb:96 nack`
// (Description::Video::addVideoCodec adds it), but RtcpReceivingSession only ever emits RR, REMB and
// PLI — it has no NACK generator. So a lost packet is simply gone.
//
// The visible consequence is the classic one: the concealed macroblocks stay wrong, and because
// subsequent P-frames only code what *changed*, the bad region persists until something moves through
// it or a keyframe arrives. WebRTC senders emit keyframes on demand rather than on a short timer, so
// "until a keyframe arrives" can mean a very long time.
//
// WHAT THIS DOES
// --------------
// Sits at the head of the incoming media chain (so it sees raw RTP before depacketization) and tracks
// sequence numbers via RtpSeqTracker. Cost per packet: a 16-bit subtract, a compare and one relaxed
// atomic store — far below the DTLS/SRTP work already done on that same packet, and it allocates
// nothing.
//
// Chain/order note: MediaHandler::incomingChain recurses to the END of the chain first and unwinds,
// so the LAST handler added sees packets FIRST. Adding this after RtcpReceivingSession therefore puts
// it before both the RTCP session and the depacketizer.
#pragma once

#include "rtp_seq.h"

#include "rtc/rtc.hpp"

/// Counts sequence gaps, duplicates and reordering on an incoming RTP stream.
class RtpLossMonitor final : public rtc::MediaHandler {
public:
    void incoming(rtc::message_vector &messages, const rtc::message_callback &) override {
        for (const auto &m : messages) {
            if (!m || m->type != rtc::Message::Binary) continue;
            if (m->size() < sizeof(rtc::RtpHeader)) continue;
            auto *h = reinterpret_cast<const rtc::RtpHeader *>(m->data());
            if (h->version() != 2) continue;
            seq_.observe(h->seqNumber());
        }
    }

    bool     consumeGap()  { return seq_.consumeGap(); }
    uint64_t received()    const { return seq_.received(); }
    uint64_t lost()        const { return seq_.lost(); }
    uint64_t gaps()        const { return seq_.gaps(); }
    uint64_t reordered()   const { return seq_.reordered(); }
    uint64_t duplicates()  const { return seq_.duplicates(); }
    double   lossPercent() const { return seq_.lossPercent(); }

private:
    RtpSeqTracker seq_;
};
