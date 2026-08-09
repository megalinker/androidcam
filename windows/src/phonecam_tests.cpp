// Unit tests for the receiver's dependency-free logic: the phone-status wire format, RTP sequence
// tracking (the artifact-detection core) and the problem marker.
//
// Deliberately links nothing: no FFmpeg, no libdatachannel, no Winsock. That keeps the pieces most
// likely to regress runnable in a second, on any machine, without the media stack being built.
//
//   cmake --build build-webrtc --config Release --target phonecam_tests
//   build-webrtc\Release\phonecam_tests.exe
#include "phone_status.h"
#include "rtp_seq.h"
#include "marker.h"

#include <cstdio>
#include <string>

static int g_failed = 0, g_ran = 0;

#define CHECK(cond)                                                                    \
    do {                                                                               \
        ++g_ran;                                                                       \
        if (!(cond)) {                                                                 \
            ++g_failed;                                                                \
            std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);              \
        }                                                                              \
    } while (0)

#define CHECK_EQ(a, b)                                                                 \
    do {                                                                               \
        ++g_ran;                                                                       \
        auto _a = (a); auto _b = (b);                                                  \
        if (!(_a == _b)) {                                                             \
            ++g_failed;                                                                \
            std::printf("  FAIL %s:%d  %s == %s  (got %lld, want %lld)\n", __FILE__,   \
                        __LINE__, #a, #b, (long long)_a, (long long)_b);               \
        }                                                                              \
    } while (0)

// ---------------------------------------------------------------- phone status

static void test_status_parse_full() {
    std::printf("phone_status: full message\n");
    auto m = phonestatus::parse(
        R"({"t":"status","v":1,"ts":1723200000000,"sid":"a1b2c3d4","pct":73,"chg":1,"plug":2,"tempC":31.2})");
    CHECK(m.valid);
    CHECK_EQ(m.proto, 1);
    CHECK_EQ(m.batteryPct, 73);
    CHECK_EQ(m.charging, 1);
    CHECK_EQ(m.plug, 2);
    CHECK(m.tempC > 31.1f && m.tempC < 31.3f);
    CHECK(m.sid == "a1b2c3d4");
}

static void test_status_omitted_fields_are_unknown() {
    // The phone OMITS values the platform did not expose. "Absent" must stay distinct from "zero" —
    // otherwise an unsupported charge counter would be displayed as a real 0 % battery.
    std::printf("phone_status: omitted fields stay unknown\n");
    auto m = phonestatus::parse(R"({"t":"status","v":1,"ts":1,"chg":0})");
    CHECK(m.valid);
    CHECK_EQ(m.batteryPct, -1);          // no "pct" key -> unknown, NOT 0
    CHECK_EQ(m.charging, 0);
    CHECK(m.tempC <= -1000.0f);          // no temperature reported
    CHECK(m.sid.empty());
}

static void test_status_rejects_non_status() {
    std::printf("phone_status: non-status payloads rejected\n");
    CHECK(!phonestatus::parse(R"({"t":"hello","v":1})").valid);
    CHECK(!phonestatus::parse("").valid);
    CHECK(!phonestatus::parse("not json at all").valid);
    CHECK(!phonestatus::parse("{").valid);
}

static void test_status_out_of_range_is_dropped() {
    std::printf("phone_status: out-of-range values dropped\n");
    CHECK_EQ(phonestatus::parse(R"({"t":"status","pct":150})").batteryPct, -1);
    CHECK_EQ(phonestatus::parse(R"({"t":"status","pct":-5})").batteryPct, -1);
    CHECK(phonestatus::parse(R"({"t":"status","tempC":9999})").tempC <= -1000.0f);
    CHECK_EQ(phonestatus::parse(R"({"t":"status","pct":0})").batteryPct, 0);   // a real 0% is valid
    CHECK_EQ(phonestatus::parse(R"({"t":"status","pct":100})").batteryPct, 100);
}

static void test_status_forward_compatible() {
    // A newer phone may add keys or bump the version; an older receiver must still read what it knows.
    std::printf("phone_status: forward compatible with unknown keys\n");
    auto m = phonestatus::parse(
        R"({"t":"status","v":7,"pct":42,"chg":0,"newField":"whatever","nested":{"a":1}})");
    CHECK(m.valid);
    CHECK_EQ(m.proto, 7);
    CHECK_EQ(m.batteryPct, 42);
    CHECK_EQ(m.charging, 0);
}

static void test_hello_advertises_features() {
    std::printf("phone_status: hello advertises the features the phone gates on\n");
    std::string h = phonestatus::helloJson();
    CHECK(h.find("\"status\"") != std::string::npos);
    CHECK(h.find("\"mark\"") != std::string::npos);
    CHECK(h.find("\"keyframe\"") != std::string::npos);
}

// ---------------------------------------------------------------- RTP sequence tracking

static void test_seq_in_order_is_clean() {
    std::printf("rtp_seq: a clean stream reports no loss\n");
    RtpSeqTracker t;
    for (int i = 0; i < 1000; ++i) t.observe((uint16_t)(5000 + i));
    CHECK_EQ(t.received(), 1000u);
    CHECK_EQ(t.lost(), 0u);
    CHECK_EQ(t.gaps(), 0u);
    CHECK(!t.consumeGap());
    CHECK(t.lossPercent() == 0.0);
}

static void test_seq_detects_gap() {
    std::printf("rtp_seq: a dropped packet is a gap of exactly one\n");
    RtpSeqTracker t;
    t.observe(100); t.observe(101); /* 102 lost */ t.observe(103);
    CHECK_EQ(t.lost(), 1u);
    CHECK_EQ(t.gaps(), 1u);
    CHECK(t.consumeGap());
    CHECK(!t.consumeGap());   // the flag is one-shot: one keyframe request per burst, not per packet
}

static void test_seq_burst_counts_every_packet() {
    std::printf("rtp_seq: a burst loss counts every missing packet but is one gap event\n");
    RtpSeqTracker t;
    t.observe(1);
    t.observe(12);            // 2..11 = 10 packets lost
    CHECK_EQ(t.lost(), 10u);
    CHECK_EQ(t.gaps(), 1u);
}

static void test_seq_wraparound() {
    // The single most likely place for a sequence tracker to invent phantom loss.
    std::printf("rtp_seq: wrapping 65535 -> 0 is not loss\n");
    RtpSeqTracker t;
    t.observe(65533); t.observe(65534); t.observe(65535); t.observe(0); t.observe(1);
    CHECK_EQ(t.lost(), 0u);
    CHECK_EQ(t.gaps(), 0u);
    CHECK(!t.consumeGap());
}

static void test_seq_wraparound_with_loss() {
    std::printf("rtp_seq: loss across the wrap point is still counted once\n");
    RtpSeqTracker t;
    t.observe(65534); /* 65535 and 0 lost */ t.observe(1);
    CHECK_EQ(t.lost(), 2u);
    CHECK_EQ(t.gaps(), 1u);
}

static void test_seq_reorder_is_not_loss() {
    // Reordering must NOT trigger a keyframe request: the packet did arrive, just late, and asking
    // the phone for an IDR every time the network reorders would be a standing battery cost.
    std::printf("rtp_seq: reordering is repaired, not counted as loss\n");
    RtpSeqTracker t;
    t.observe(10); t.observe(11); t.observe(13); t.observe(12);
    CHECK_EQ(t.reordered() + t.duplicates(), 1u);
    CHECK_EQ(t.lost(), 0u);   // the late packet gave its "lost" credit back
}

static void test_seq_duplicate() {
    std::printf("rtp_seq: a duplicated packet is counted as such\n");
    RtpSeqTracker t;
    t.observe(20); t.observe(21); t.observe(21);
    CHECK_EQ(t.duplicates(), 1u);
    CHECK_EQ(t.lost(), 0u);
}

static void test_seq_huge_jump_is_a_restart() {
    std::printf("rtp_seq: a huge jump is treated as a restart, not 30000 lost packets\n");
    RtpSeqTracker t;
    t.observe(1);
    t.observe(30000);
    CHECK_EQ(t.lost(), 0u);
    CHECK_EQ(t.gaps(), 0u);
    CHECK(!t.consumeGap());
    t.observe(30001);          // and it resynchronises from the new base
    t.observe(30002);
    CHECK_EQ(t.lost(), 0u);
}

static void test_seq_loss_percent() {
    std::printf("rtp_seq: loss percentage is over expected, not received\n");
    RtpSeqTracker t;
    t.observe(0);
    t.observe(2);              // 1 lost, 2 received -> 1 of 3 expected
    double p = t.lossPercent();
    CHECK(p > 33.2 && p < 33.4);
}

// ---------------------------------------------------------------- marker

static void test_marker_one_shot() {
    std::printf("marker: each raise is consumed exactly once\n");
    marker::Consumer c;
    std::string note;
    CHECK(!c.poll(note));                       // nothing raised yet
    marker::raise("artifact seen");
    CHECK(c.poll(note));
    CHECK(note == "artifact seen");
    CHECK(!c.poll(note));                       // consumed
    marker::raise("second");
    CHECK(c.poll(note));
    CHECK(note == "second");
}

static void test_marker_independent_consumers() {
    std::printf("marker: a fresh consumer does not replay history\n");
    marker::raise("older");
    marker::Consumer fresh;                     // starts from the current sequence
    std::string note;
    CHECK(!fresh.poll(note));
    marker::raise("newer");
    CHECK(fresh.poll(note));
    CHECK(note == "newer");
}

// ----------------------------------------------------------------

int main() {
    std::printf("=== PhoneCam receiver unit tests ===\n");
    test_status_parse_full();
    test_status_omitted_fields_are_unknown();
    test_status_rejects_non_status();
    test_status_out_of_range_is_dropped();
    test_status_forward_compatible();
    test_hello_advertises_features();

    test_seq_in_order_is_clean();
    test_seq_detects_gap();
    test_seq_burst_counts_every_packet();
    test_seq_wraparound();
    test_seq_wraparound_with_loss();
    test_seq_reorder_is_not_loss();
    test_seq_duplicate();
    test_seq_huge_jump_is_a_restart();
    test_seq_loss_percent();

    test_marker_one_shot();
    test_marker_independent_consumers();

    std::printf("\n%d checks, %d failed\n", g_ran, g_failed);
    return g_failed == 0 ? 0 : 1;
}
