// Phone device status (battery) received over whichever control channel the transport already has.
//
//   WebRTC : the PCAM3 signaling socket, message type 'B'   (see webrtc_receiver.cpp)
//   USB    : the media socket's frame format, type 'S'      (see usb_receiver.cpp)
//
// The payload is a small JSON object produced by the phone's PhoneStatus.encode():
//   {"t":"status","v":1,"ts":1723200000000,"sid":"a1b2c3d4","pct":73,"chg":1,"plug":2,"tempC":31.2}
// Fields the phone could not read are OMITTED, never faked — so "absent" and "zero" stay distinct.
//
// The parser is deliberately a tiny scalar scanner rather than a JSON library: both ends are ours,
// the schema is six flat keys, and pulling in a dependency for it would be silly. Unknown keys are
// ignored, so either side can add fields without breaking the other.
//
// Output goes to stderr as one stable line the desktop app parses (like the existing [level] meter):
//   [status] battery=73 charging=1 tempC=31.2 sid=a1b2c3d4
#pragma once

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

struct PhoneStatusMsg {
    int   proto = 0;
    int   batteryPct = -1;     // -1 = the phone did not report a level
    int   charging = -1;       // -1 = unknown, 0 = on battery, 1 = charging/plugged
    int   plug = 0;            // BatteryManager.BATTERY_PLUGGED_* (0 = not plugged)
    float tempC = -1000.0f;    // -1000 = unavailable
    std::string sid;           // phone-side session id, for lining the two logs up
    bool  valid = false;
};

namespace phonestatus {

/// USB media-socket frame type carrying the same payload (phone -> PC).
inline constexpr char kUsbStatusType = 'S';

// Locate "key": in a flat JSON object and return a pointer just past the colon, or null.
inline const char *field(const std::string &j, const char *key) {
    std::string pat = std::string("\"") + key + "\"";
    size_t i = j.find(pat);
    if (i == std::string::npos) return nullptr;
    i += pat.size();
    while (i < j.size() && (j[i] == ' ' || j[i] == ':')) ++i;
    return (i < j.size()) ? j.c_str() + i : nullptr;
}

inline bool intField(const std::string &j, const char *key, int &out) {
    const char *p = field(j, key);
    if (!p) return false;
    char *end = nullptr;
    long v = std::strtol(p, &end, 10);
    if (end == p) return false;
    out = (int)v;
    return true;
}

inline bool floatField(const std::string &j, const char *key, float &out) {
    const char *p = field(j, key);
    if (!p) return false;
    char *end = nullptr;
    double v = std::strtod(p, &end);
    if (end == p) return false;
    out = (float)v;
    return true;
}

inline bool strField(const std::string &j, const char *key, std::string &out) {
    const char *p = field(j, key);
    if (!p || *p != '"') return false;
    const char *s = p + 1;
    const char *e = std::strchr(s, '"');
    if (!e) return false;
    out.assign(s, (size_t)(e - s));
    return true;
}

/// Parse a status payload. Returns a message with .valid=false if it is not one.
inline PhoneStatusMsg parse(const std::string &json) {
    PhoneStatusMsg m;
    if (json.find("\"status\"") == std::string::npos) return m;
    intField(json, "v", m.proto);
    int pct = -1;
    if (intField(json, "pct", pct) && pct >= 0 && pct <= 100) m.batteryPct = pct;
    int chg = 0;
    if (intField(json, "chg", chg)) m.charging = chg ? 1 : 0;
    intField(json, "plug", m.plug);
    float t = 0.0f;
    if (floatField(json, "tempC", t) && t > -100.0f && t < 200.0f) m.tempC = t;
    strField(json, "sid", m.sid);
    m.valid = true;
    return m;
}

/// The one line the desktop app reads. Absent values stay absent: an unknown level is printed as -1
/// and an unavailable temperature as "n/a", so the UI can show "—" instead of an invented 0.
inline void emit(const PhoneStatusMsg &m) {
    char temp[16];
    if (m.tempC <= -1000.0f) std::snprintf(temp, sizeof temp, "n/a");
    else                     std::snprintf(temp, sizeof temp, "%.1f", m.tempC);
    std::fprintf(stderr, "[status] battery=%d charging=%d plug=%d tempC=%s sid=%s\n",
                 m.batteryPct, m.charging, m.plug, temp, m.sid.empty() ? "-" : m.sid.c_str());
    std::fflush(stderr);
}

/// The PC's hello, sent once per session so an older phone never has to know about any of this.
inline std::string helloJson() {
    return "{\"t\":\"hello\",\"app\":\"phonecam\",\"v\":1,\"feat\":[\"status\",\"mark\",\"keyframe\"]}";
}

}  // namespace phonestatus
