#pragma once
// The pipe protocol between the simulator and rr_bothost. Each message is a
// header (type, payload size) and the payload. Structs travel as raw bytes:
// the simulator and rr_bothost are built together, so their layouts match.
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "rr/robot_api.h"

namespace rr::botproto {

enum : uint32_t {
    // host -> simulator
    HELLO = 1,    // i32 abi, i32 has_debug_path, i32 has_session_end, str name, str author
    CREATED,      // i32 ok, RRRobotConfig
    CONTROL,      // RRControl, f64 cpu seconds
    PATH,         // i32 count, count * 2 floats
    MEMORY,       // the weekend memory bytes
    FAILED,       // str why (library would not load)
    // simulator -> host
    CREATE = 100, // i32 index, str params, RRCarSpec, RRRobotConfig, track, memory bytes
    DRIVE,        // RRSensors, RRControl
    DEBUG_PATH,   // i32 max points
    SESSION_END,  // RRSessionSummary
    DESTROY,      // (none): the host answers MEMORY and exits
};

struct Header {
    uint32_t type, size;
};

// Building and reading payloads.
struct Writer {
    std::vector<unsigned char> b;
    void raw(const void* p, size_t n) { b.insert(b.end(), (const unsigned char*)p, (const unsigned char*)p + n); }
    template <class T> void pod(const T& v) { raw(&v, sizeof v); }
    void str(const std::string& s) { pod((uint32_t)s.size()); raw(s.data(), s.size()); }
    void bytes(const unsigned char* p, size_t n) { pod((uint32_t)n); raw(p, n); }
};

struct Reader {
    const unsigned char* p;
    size_t n, at = 0;
    bool ok = true;
    Reader(const std::vector<unsigned char>& v) : p(v.data()), n(v.size()) {}
    bool raw(void* out, size_t k) {
        if (!ok || at + k > n) return ok = false;
        std::memcpy(out, p + at, k);
        at += k;
        return true;
    }
    template <class T> T pod() { T v{}; raw(&v, sizeof v); return v; }
    std::string str() {
        const uint32_t k = pod<uint32_t>();
        if (!ok || at + k > n) { ok = false; return {}; }
        std::string s((const char*)p + at, k);
        at += k;
        return s;
    }
    std::vector<unsigned char> bytes() {
        const uint32_t k = pod<uint32_t>();
        if (!ok || at + k > n) { ok = false; return {}; }
        std::vector<unsigned char> v(p + at, p + at + k);
        at += k;
        return v;
    }
};

// The track: everything RRTrackInfo points at.
inline void writeTrack(Writer& w, const RRTrackInfo& t) {
    w.str(t.name ? t.name : "");
    w.pod(t.length);
    w.pod(t.runoff);
    w.pod(t.pit);
    w.bytes((const unsigned char*)t.points, sizeof(RRTrackPoint) * (size_t)t.num_points);
    w.bytes((const unsigned char*)t.turns, sizeof(RRTurn) * (size_t)t.num_turns);
    w.bytes((const unsigned char*)t.points3, t.points3 ? sizeof(RRTrackPoint3) * (size_t)t.num_points : 0);
}

struct TrackCopy {
    std::string name;
    std::vector<RRTrackPoint> points;
    std::vector<RRTurn> turns;
    std::vector<RRTrackPoint3> points3;
    RRTrackInfo info{};
};

inline bool readTrack(Reader& r, TrackCopy& t) {
    t.name = r.str();
    t.info.length = r.pod<float>();
    t.info.runoff = r.pod<float>();
    t.info.pit = r.pod<RRPitInfo>();
    std::vector<unsigned char> pts = r.bytes(), turns = r.bytes(), pts3 = r.bytes();
    if (!r.ok || pts.size() % sizeof(RRTrackPoint) || turns.size() % sizeof(RRTurn)) return false;
    if (pts3.size() != pts.size() / sizeof(RRTrackPoint) * sizeof(RRTrackPoint3)) return false;
    t.points.resize(pts.size() / sizeof(RRTrackPoint));
    if (!pts.empty()) std::memcpy(t.points.data(), pts.data(), pts.size());
    t.turns.resize(turns.size() / sizeof(RRTurn));
    if (!turns.empty()) std::memcpy(t.turns.data(), turns.data(), turns.size());
    t.info.name = t.name.c_str();
    t.info.num_points = (int)t.points.size();
    t.info.points = t.points.data();
    t.info.num_turns = (int)t.turns.size();
    t.info.turns = t.turns.data();
    t.points3.resize(t.points.size());
    if (!pts3.empty()) std::memcpy(t.points3.data(), pts3.data(), pts3.size());
    t.info.points3 = t.points3.data();
    return true;
}

}  // namespace rr::botproto
