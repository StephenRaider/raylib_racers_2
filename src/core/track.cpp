#include "track.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace rr {

namespace {

// Centripetal Catmull-Rom point between p1 and p2 (u in [0,1]).
Vec2 catmullRom(Vec2 p0, Vec2 p1, Vec2 p2, Vec2 p3, float u) {
    auto knot = [](float t, Vec2 a, Vec2 b) {
        float d = length(b - a);
        return t + std::max(std::sqrt(d), 1e-4f);
    };
    float t0 = 0, t1 = knot(t0, p0, p1), t2 = knot(t1, p1, p2), t3 = knot(t2, p2, p3);
    float t = t1 + (t2 - t1) * u;
    Vec2 a1 = p0 * ((t1 - t) / (t1 - t0)) + p1 * ((t - t0) / (t1 - t0));
    Vec2 a2 = p1 * ((t2 - t) / (t2 - t1)) + p2 * ((t - t1) / (t2 - t1));
    Vec2 a3 = p2 * ((t3 - t) / (t3 - t2)) + p3 * ((t - t2) / (t3 - t2));
    Vec2 b1 = a1 * ((t2 - t) / (t2 - t0)) + a2 * ((t - t0) / (t2 - t0));
    Vec2 b2 = a2 * ((t3 - t) / (t3 - t1)) + a3 * ((t - t1) / (t3 - t1));
    return b1 * ((t2 - t) / (t2 - t1)) + b2 * ((t - t1) / (t2 - t1));
}

// Uniform Catmull-Rom for a scalar between v1 and v2 (u in [0,1]).
float catmullRom1(float v0, float v1, float v2, float v3, float u) {
    float u2 = u * u, u3 = u2 * u;
    return 0.5f * (2 * v1 + (v2 - v0) * u + (2 * v0 - 5 * v1 + 4 * v2 - v3) * u2 + (3 * v1 - v0 - 3 * v2 + v3) * u3);
}

}  // namespace

bool Track::load(const std::string& path, std::string* err) {
    std::ifstream in(path);
    if (!in) {
        if (err) *err = "cannot open track file " + path;
        return false;
    }
    std::vector<Vec2> ctrl;
    std::vector<float> widths, heights, banks;
    std::string line;
    int lineNo = 0;
    while (std::getline(in, line)) {
        ++lineNo;
        auto hash = line.find('#');
        if (hash != std::string::npos) line.resize(hash);
        std::istringstream ss(line);
        std::string key;
        if (!(ss >> key)) continue;
        if (key == "name") {
            std::getline(ss >> std::ws, name_);
        } else if (key == "scenery") {
            ss >> scenery_;
        } else if (key == "width") {
            ss >> defaultWidth_;
        } else if (key == "runoff") {
            ss >> runoff_;
        } else if (key == "pit") {
            std::string side;
            if (!(ss >> side >> pitCfg_.entry_s >> pitCfg_.lane_start_s >> pitCfg_.lane_end_s >> pitCfg_.exit_s) ||
                (side != "left" && side != "right")) {
                if (err) *err = path + ":" + std::to_string(lineNo) + ": expected 'pit left|right entry lane_start lane_end exit'";
                return false;
            }
            pitCfg_.has_pit = 1;
            pitCfg_.side = side == "left" ? 1 : -1;
        } else if (key == "gravel") {
            std::string mode;
            ss >> mode;
            if (mode != "auto" && mode != "none") {
                if (err) *err = path + ":" + std::to_string(lineNo) + ": expected 'gravel auto|none'";
                return false;
            }
            autoGravel_ = mode == "auto";
        } else if (key == "offtrack") {
            std::string nm;
            ss >> nm;
            offtrack_ = surfaceFromName(nm);
            if (offtrack_ < 0) {
                if (err) *err = path + ":" + std::to_string(lineNo) + ": unknown surface '" + nm + "'";
                return false;
            }
        } else if (key == "surface") {
            std::string nm, side;
            SurfaceZone z{};
            z.from = kKerbWidth;
            z.to = 1e9f;
            ss >> nm >> side;
            z.type = surfaceFromName(nm);
            z.side = side == "left" ? 1 : side == "right" ? -1 : 0;
            if (z.type < 0 || (side != "left" && side != "right" && side != "both") || !(ss >> z.s0 >> z.s1)) {
                if (err) *err = path + ":" + std::to_string(lineNo) + ": expected 'surface <type> left|right|both s0 s1 [from to]'";
                return false;
            }
            float a, b;
            if (ss >> a >> b) { z.from = a; z.to = b; }
            zones_.push_back(z);
        } else if (key == "pitspeed") {
            ss >> pitCfg_.speed_limit;
        } else if (key == "drs") {
            drsFromFile_ = true;
            RRDrsZone z{};
            std::string first;
            ss >> first;
            if (first != "none") {
                std::istringstream zs(first);
                if (!(zs >> z.detect_s) || !(ss >> z.start_s >> z.end_s)) {
                    if (err) *err = path + ":" + std::to_string(lineNo) + ": expected 'drs detect_s start_s end_s' or 'drs none'";
                    return false;
                }
                drsFile_.push_back(z);
            }
        } else if (key == "p") {
            float x, y, w = -1, h = 0, bank = 0;
            const std::string usage = path + ":" + std::to_string(lineNo) + ": expected 'p x y [width] [h=m] [bank=deg]'";
            if (!(ss >> x >> y)) {
                if (err) *err = usage;
                return false;
            }
            std::string tok;
            bool first = true;
            while (ss >> tok) {
                auto num = [&](const std::string& v, float* out) {
                    char* end = nullptr;
                    *out = std::strtof(v.c_str(), &end);
                    return !v.empty() && end && *end == 0;
                };
                bool ok;
                if (tok.rfind("h=", 0) == 0) ok = num(tok.substr(2), &h);
                else if (tok.rfind("bank=", 0) == 0) ok = num(tok.substr(5), &bank);
                else if (first && tok == "-") ok = true;
                else ok = first && num(tok, &w);
                if (!ok) {
                    if (err) *err = usage;
                    return false;
                }
                first = false;
            }
            ctrl.push_back({x, y});
            widths.push_back(w);
            heights.push_back(h);
            banks.push_back(bank);
        } else {
            if (err) *err = path + ":" + std::to_string(lineNo) + ": unknown key '" + key + "'";
            return false;
        }
    }
    for (auto& w : widths)
        if (w <= 0) w = defaultWidth_;
    return build(ctrl, widths, err, heights, banks);
}

bool Track::build(const std::vector<Vec2>& ctrl, const std::vector<float>& widths, std::string* err,
                  const std::vector<float>& heights, const std::vector<float>& banks) {
    const int n = (int)ctrl.size();
    if (n < 4) {
        if (err) *err = "a track needs at least 4 control points";
        return false;
    }
    if ((!heights.empty() && (int)heights.size() != n) || (!banks.empty() && (int)banks.size() != n)) {
        if (err) *err = "heights and banks need one value per control point";
        return false;
    }
    auto hAt = [&](int i) { return heights.empty() ? 0.0f : heights[(i + n) % n]; };
    auto bAt = [&](int i) { return banks.empty() ? 0.0f : banks[(i + n) % n] * kPi / 180.0f; };
    is3D_ = false;
    for (int i = 0; i < n; ++i)
        if (hAt(i) != 0 || bAt(i) != 0) is3D_ = true;
    // 1. Dense spline polyline.
    std::vector<Vec2> dense;
    std::vector<float> denseW, denseZ, denseB;
    for (int i = 0; i < n; ++i) {
        Vec2 p0 = ctrl[(i - 1 + n) % n], p1 = ctrl[i], p2 = ctrl[(i + 1) % n], p3 = ctrl[(i + 2) % n];
        float w1 = widths[i], w2 = widths[(i + 1) % n];
        int steps = std::max(16, (int)(rr::length(p2 - p1) / 0.25f));
        for (int k = 0; k < steps; ++k) {
            float u = (float)k / steps;
            dense.push_back(catmullRom(p0, p1, p2, p3, u));
            // smoothstep the width so width changes do not kink the edges
            float su = u * u * (3 - 2 * u);
            denseW.push_back(w1 + (w2 - w1) * su);
            // height and bank follow a Catmull-Rom curve so the grade has no
            // steps at the control points
            denseZ.push_back(catmullRom1(hAt(i - 1), hAt(i), hAt(i + 1), hAt(i + 2), u));
            denseB.push_back(catmullRom1(bAt(i - 1), bAt(i), bAt(i + 1), bAt(i + 2), u));
        }
    }
    // 2. Resample by arc length.
    std::vector<float> cum(dense.size() + 1, 0.0f);
    for (size_t i = 0; i < dense.size(); ++i)
        cum[i + 1] = cum[i] + rr::length(dense[(i + 1) % dense.size()] - dense[i]);
    const float total = cum.back();
    const int count = std::max(16, (int)std::lround(total / 1.0f));
    ds_ = total / count;
    length_ = total;
    samples_.assign(count, {});
    size_t j = 0;
    for (int i = 0; i < count; ++i) {
        float s = i * ds_;
        while (j + 1 < cum.size() - 1 && cum[j + 1] < s) ++j;
        float segLen = cum[j + 1] - cum[j];
        float f = segLen > 1e-6f ? (s - cum[j]) / segLen : 0.0f;
        Vec2 a = dense[j], b = dense[(j + 1) % dense.size()];
        float wa = denseW[j], wb = denseW[(j + 1) % dense.size()];
        samples_[i].p = a + (b - a) * f;
        samples_[i].halfWidth = 0.5f * (wa + (wb - wa) * f);
        samples_[i].s = s;
        size_t jb = (j + 1) % dense.size();
        samples_[i].z = denseZ[j] + (denseZ[jb] - denseZ[j]) * f;
        samples_[i].bank = denseB[j] + (denseB[jb] - denseB[j]) * f;
    }
    finalize();
    return true;
}

void Track::finalize() {
    const int n = size();
    for (int i = 0; i < n; ++i) {
        Vec2 d = at(i + 1).p - at(i - 1).p;
        samples_[i].t = normalize(d);
        samples_[i].n = perpLeft(samples_[i].t);
        samples_[i].grade = (at(i + 1).z - at(i - 1).z) / (2 * ds_);
    }
    {
        // vertical curvature over +-10 m: the 1 m samples are too noisy to difference directly
        const int h = std::max(1, (int)std::lround(10.0f / ds_));
        for (int i = 0; i < n; ++i) samples_[i].vcurv = (at(i + h).grade - at(i - h).grade) / (2 * h * ds_);
    }
    std::vector<float> raw(n);
    for (int i = 0; i < n; ++i) {
        float a0 = std::atan2(at(i - 1).t.y, at(i - 1).t.x);
        float a1 = std::atan2(at(i + 1).t.y, at(i + 1).t.x);
        raw[i] = wrapAngle(a1 - a0) / (2 * ds_);
    }
    const int k = 3;  // box filter radius
    for (int i = 0; i < n; ++i) {
        float sum = 0;
        for (int o = -k; o <= k; ++o) sum += raw[wrap(i + o)];
        samples_[i].curvature = sum / (2 * k + 1);
    }

    // Sanity checks: edges folding over in tight corners, and parts of the
    // track overlapping each other.
    warnings_.clear();
    for (int i = 0; i < n; ++i) {
        const auto& s = samples_[i];
        if (std::fabs(s.curvature) * s.halfWidth > 0.95f) {
            char buf[160];
            std::snprintf(buf, sizeof buf, "corner at s=%.0f m is tighter (r=%.1f m) than the half width (%.1f m)",
                          s.s, 1.0f / std::fabs(s.curvature), s.halfWidth);
            warnings_.push_back(buf);
            break;
        }
    }
    for (int i = 0; i < n && warnings_.size() < 4; i += 2) {
        for (int j = i + 1; j < n; j += 2) {
            const auto& a = samples_[i];
            const auto& b = samples_[j];
            float arc = std::min(b.s - a.s, length_ - (b.s - a.s));
            float minDist = a.halfWidth + b.halfWidth + 2.0f;
            if (arc < 2.5f * minDist + 10.0f) continue;
            if (rr::length(a.p - b.p) < minDist) {
                char buf[160];
                std::snprintf(buf, sizeof buf, "track overlaps itself near s=%.0f m and s=%.0f m", a.s, b.s);
                warnings_.push_back(buf);
                i += 50;
                break;
            }
        }
    }

    apiPoints_.resize(n);
    apiPoints3_.resize(n);
    for (int i = 0; i < n; ++i) {
        const auto& s = samples_[i];
        apiPoints_[i] = {s.p.x, s.p.y, s.t.x, s.t.y, s.s, s.halfWidth, s.curvature};
        apiPoints3_[i] = {s.z, s.bank, s.grade, s.vcurv};
    }
    info_.name = name_.c_str();
    info_.length = length_;
    info_.runoff = runoff_;
    info_.num_points = n;
    info_.points = apiPoints_.data();
    info_.points3 = apiPoints3_.data();
    info_.pit = pitCfg_;
    if (info_.pit.has_pit) {
        auto wrapS = [&](float v) { v = std::fmod(v, length_); return v < 0 ? v + length_ : v; };
        RRPitInfo& p = info_.pit;
        p.entry_s = wrapS(p.entry_s);
        p.lane_start_s = wrapS(p.lane_start_s);
        p.lane_end_s = wrapS(p.lane_end_s);
        p.exit_s = wrapS(p.exit_s);
        float hw = at(indexAt(p.lane_start_s)).halfWidth;
        p.lane_offset = p.side * (hw + kLaneCentre);
        p.box_offset = p.side * (hw + kBoxCentre);
        if (p.speed_limit <= 0) p.speed_limit = 22.0f;
        if (runoff_ > kPitBarrier) warnings_.push_back("runoff is wider than the pit area; the pit barrier will stick out");
    }
    findTurns();
    findDrsZones();
    buildEdgeGrid();
    kerb_.assign(n, 0);
    for (int i = 0; i < n; ++i)
        if (std::fabs(at(i).curvature) > 1.0f / 220.0f)
            for (int k = -12; k <= 12; ++k) kerb_[wrap(i + k)] = 1;
    // Gravel traps on the outside of the faster corners, a little beyond them either way.
    {
        std::vector<float> k(n);
        for (int i = 0; i < n; ++i) {
            float sum = 0;
            for (int o = -20; o <= 20; ++o) sum += at(i + o).curvature;
            k[i] = sum / 41;
        }
        gravelSide_.assign(n, 0.0f);
        for (int i = 0; autoGravel_ && i < n; ++i) {
            float x = clampf((std::fabs(k[i]) - 1.0f / 260.0f) / (1.0f / 110.0f - 1.0f / 260.0f), 0, 1);
            const float strength = x * x * (3 - 2 * x);
            if (strength <= 0) continue;
            const float side = k[i] > 0 ? -1.0f : 1.0f;  // outside of the bend
            for (int o = -10; o <= 45; ++o) {            // gravel runs on past the exit
                int j = wrap(i + o);
                float v = strength * (1.0f - std::max(0, o - 25) / 20.0f);
                if (std::fabs(gravelSide_[j]) < v) gravelSide_[j] = side * v;
            }
        }
        if (hasPit())
            for (int i = 0; i < n; ++i)
                if (inPitArea(at(i).s) && gravelSide_[i] * info_.pit.side > 0) gravelSide_[i] = 0;
    }
    for (SurfaceZone& z : zones_) {
        z.s0 = std::fmod(z.s0, length_); if (z.s0 < 0) z.s0 += length_;
        z.s1 = std::fmod(z.s1, length_); if (z.s1 < 0) z.s1 += length_;
    }
}

// DRS zones: the track file's, or else the longest straights (at most three) that are 450 m or more
// between two corners. A zone starts 40 m after the corner and ends 130 m before the next one; its
// detection point is at the start of the corner before it.
void Track::findDrsZones() {
    drsZones_.clear();
    auto wrapS = [&](float v) { v = std::fmod(v, length_); return v < 0 ? v + length_ : v; };
    if (drsFromFile_) {
        for (RRDrsZone z : drsFile_) drsZones_.push_back({wrapS(z.detect_s), wrapS(z.start_s), wrapS(z.end_s)});
    } else {
        struct Straight { float len; RRDrsZone z; };
        std::vector<Straight> found;
        const int nt = (int)turns_.size();
        for (int i = 0; i < nt && nt > 1; ++i) {
            const RRTurn& a = turns_[i];
            const RRTurn& b = turns_[(i + 1) % nt];
            float len = b.start_s - a.end_s;
            if (len <= 0) len += length_;
            if (len < 450.0f) continue;
            found.push_back({len, {wrapS(a.start_s), wrapS(a.end_s + 40.0f), wrapS(b.start_s - 130.0f)}});
        }
        std::sort(found.begin(), found.end(), [](const Straight& x, const Straight& y) { return x.len > y.len; });
        if (found.size() > 3) found.resize(3);
        std::sort(found.begin(), found.end(), [](const Straight& x, const Straight& y) { return x.z.start_s < y.z.start_s; });
        for (const Straight& f : found) drsZones_.push_back(f.z);
    }
    info_.num_drs_zones = (int)drsZones_.size();
    info_.drs_zones = drsZones_.data();
}

// Corners: stretches tighter than a 300 m radius (curvature smoothed over
// 20 m), same-direction stretches less than 30 m apart joined, anything
// turning less than 15 degrees dropped. A chicane is two turns.
void Track::findTurns() {
    const int n = size();
    turns_.clear();
    turnOf_.assign(n, -1);
    if (n < 10) return;
    const int half = std::max(1, (int)std::lround(10.0f / ds_));
    std::vector<float> k(n);
    for (int i = 0; i < n; ++i) {
        float sum = 0;
        for (int j = -half; j <= half; ++j) sum += at(i + j).curvature;
        k[i] = sum / (2 * half + 1);
    }
    const float kMin = 1.0f / 300.0f;
    auto dirOf = [&](int i) { return k[i] > kMin ? 1 : (k[i] < -kMin ? -1 : 0); };
    // start on a straight so no corner is split by the scan's start
    int i0 = 0;
    for (int i = 0; i < n; ++i)
        if (std::fabs(k[i]) < std::fabs(k[i0])) i0 = i;
    struct Run { int a, b, dir; };  // sample offsets from i0, b exclusive
    std::vector<Run> runs;
    for (int o = 0; o < n;) {
        const int d = dirOf((i0 + o) % n);
        if (!d) { ++o; continue; }
        int e = o;
        while (e < n && dirOf((i0 + e) % n) == d) ++e;
        const int gap = (int)std::lround(30.0f / ds_);
        if (!runs.empty() && runs.back().dir == d && o - runs.back().b < gap) runs.back().b = e;
        else runs.push_back({o, e, d});
        o = e;
    }
    for (const Run& r : runs) {
        float angle = 0, kMax = 0;
        int apex = r.a;
        for (int o = r.a; o < r.b; ++o) {
            const float kk = std::fabs(k[(i0 + o) % n]);
            angle += kk * ds_;
            if (kk > kMax) { kMax = kk; apex = o; }
        }
        if (angle < 15.0f * kPi / 180.0f) continue;
        RRTurn t{};
        t.direction = r.dir;
        t.start_s = at(i0 + r.a).s;
        t.end_s = at(i0 + r.b - 1).s;
        t.apex_s = at(i0 + apex).s;
        t.min_radius = 1.0f / std::max(kMax, 1e-4f);
        t.angle = angle;
        turns_.push_back(t);
        for (int o = r.a; o < r.b; ++o) turnOf_[(i0 + o) % n] = -2 - (int)turns_.size() + 1;  // fixed below
    }
    // number them in race order from the start line
    std::vector<int> order(turns_.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = (int)i;
    std::sort(order.begin(), order.end(), [&](int a, int b) { return turns_[a].start_s < turns_[b].start_s; });
    std::vector<int> rank(order.size());
    std::vector<RRTurn> sorted;
    for (size_t r = 0; r < order.size(); ++r) {
        rank[order[r]] = (int)r;
        sorted.push_back(turns_[order[r]]);
        sorted.back().id = (int)r + 1;
    }
    for (int& v : turnOf_)
        if (v <= -2) v = rank[-2 - v];
    turns_ = sorted;
    info_.num_turns = (int)turns_.size();
    info_.turns = turns_.data();
}

int Track::nextTurn(float s, float* ds) const {
    if (turns_.empty()) return -1;
    const int cur = turnAt(s);
    auto fwd = [&](float a, float b) { float d = std::fmod(b - a, length_); return d < 0 ? d + length_ : d; };
    int best = -1;
    float bestD = 1e30f;
    for (int i = 0; i < (int)turns_.size(); ++i) {
        if (i == cur) continue;
        const float d = fwd(s, turns_[i].start_s);
        if (d < bestD) { bestD = d; best = i; }
    }
    if (best < 0) { best = cur; bestD = fwd(s, turns_[cur].start_s); }
    if (ds) *ds = bestD;
    return best;
}

bool Track::inSpan(float s, float a, float b) const {
    auto w = [&](float v) { v = std::fmod(v, length_); return v < 0 ? v + length_ : v; };
    s = w(s); a = w(a); b = w(b);
    return a <= b ? (s >= a && s <= b) : (s >= a || s <= b);
}

bool Track::inPitArea(float s) const { return hasPit() && inSpan(s, info_.pit.entry_s, info_.pit.exit_s); }
bool Track::inPitLane(float s) const { return hasPit() && inSpan(s, info_.pit.lane_start_s, info_.pit.lane_end_s); }

float Track::barrierOffset(float s, int side, float halfWidth) const {
    if (side == info_.pit.side && inPitArea(s)) return halfWidth + kPitBarrier;
    return halfWidth + runoff_;
}

bool Track::paved(float s, float lateral, float halfWidth) const {
    switch (surfaceAt(s, lateral, halfWidth)) {
        case RR_SURF_TARMAC: case RR_SURF_KERB: case RR_SURF_PIT: case RR_SURF_RUNOFF: return true;
        default: return false;
    }
}

int Track::surfaceAt(float s, float lateral, float halfWidth) const {
    const float a = std::fabs(lateral);
    if (a <= halfWidth) return RR_SURF_TARMAC;
    const int side = lateral > 0 ? 1 : -1;
    const float out = a - halfWidth;
    const int t = zoneSurface(s, lateral, halfWidth);
    if (t >= 0) return t;
    if (side == info_.pit.side && inPitArea(s) && a <= halfWidth + kPitBarrier) return RR_SURF_PIT;
    if (out <= kKerbWidth) return kerbAt(indexAt(s)) ? RR_SURF_KERB : RR_SURF_TARMAC;
    const int i = indexAt(s);
    if (gravelSide_[i] * lateral > 0 && std::fabs(gravelSide_[i]) >= 0.5f && out >= 2.0f &&
        a < barrierOffset(s, side, halfWidth) - 1.1f)
        return RR_SURF_GRAVEL;
    return offtrack_;
}

int Track::zoneSurface(float s, float lateral, float halfWidth) const {
    const float out = std::fabs(lateral) - halfWidth;
    const int side = lateral > 0 ? 1 : -1;
    int t = -1;
    for (const SurfaceZone& z : zones_)
        if ((z.side == 0 || z.side == side) && out >= z.from && out <= z.to && inSpan(s, z.s0, z.s1)) t = z.type;
    return t;
}

const SurfaceProps& surfaceProps(int type) {
    // Grass is what the sim always used (0.7, 250); the rest are set against it. Kerbs keep
    // nearly all the grip and add no drag.
    static const SurfaceProps t[RR_NUM_SURFACES] = {
        {"tarmac", 1.00f, 0.0f}, {"kerb", 0.97f, 0.0f}, {"grass", 0.70f, 250.0f}, {"gravel", 0.55f, 700.0f},
        {"dirt", 0.65f, 380.0f}, {"pit", 1.00f, 0.0f},  {"runoff", 0.95f, 0.0f}};
    return t[type < 0 || type >= RR_NUM_SURFACES ? 0 : type];
}

int surfaceFromName(const std::string& name) {
    for (int i = 0; i < RR_NUM_SURFACES; ++i)
        if (name == surfaceProps(i).name) return i;
    return name == "asphalt" ? RR_SURF_TARMAC : name == "pitlane" ? RR_SURF_PIT : -1;
}

int Track::indexAt(float s) const {
    s = std::fmod(s, length_);
    if (s < 0) s += length_;
    return wrap((int)(s / ds_));
}

TrackLoc Track::locate(Vec2 p, int hint, int window) const {
    float best = 1e30f;
    TrackLoc loc;
    for (int k = -window; k <= window; ++k) {
        int i = wrap(hint + k);
        const auto& a = samples_[i];
        const auto& b = at(i + 1);
        Vec2 ab = b.p - a.p;
        float len2 = dot(ab, ab);
        float t = len2 > 0 ? clampf(dot(p - a.p, ab) / len2, 0, 1) : 0;
        Vec2 proj = a.p + ab * t;
        Vec2 d = p - proj;
        float d2 = dot(d, d);
        if (d2 < best) {
            best = d2;
            Vec2 nrm = normalize(a.n * (1 - t) + b.n * t);
            loc.idx = i;
            loc.s = a.s + t * ds_;
            if (loc.s >= length_) loc.s -= length_;
            loc.lateral = dot(d, nrm);
            loc.halfWidth = a.halfWidth + (b.halfWidth - a.halfWidth) * t;
        }
    }
    return loc;
}

TrackLoc Track::locateGlobal(Vec2 p) const {
    // Coarse pass then refine.
    int bestI = 0;
    float best = 1e30f;
    for (int i = 0; i < size(); i += 4) {
        Vec2 d = p - samples_[i].p;
        float d2 = dot(d, d);
        if (d2 < best) { best = d2; bestI = i; }
    }
    return locate(p, bestI, 6);
}

Vec2 Track::pointAt(float s, float lateral) const {
    s = std::fmod(s, length_);
    if (s < 0) s += length_;
    int i = wrap((int)(s / ds_));
    float f = (s - samples_[i].s) / ds_;
    const auto& a = samples_[i];
    const auto& b = at(i + 1);
    Vec2 c = a.p + (b.p - a.p) * f;
    Vec2 nrm = normalize(a.n * (1 - f) + b.n * f);
    return c + nrm * lateral;
}

Vec2 Track::dirAt(float s) const {
    s = std::fmod(s, length_);
    if (s < 0) s += length_;
    int i = wrap((int)(s / ds_));
    float f = (s - samples_[i].s) / ds_;
    return normalize(samples_[i].t * (1 - f) + at(i + 1).t * f);
}

float Track::heightAt(float s, float lateral) const {
    s = std::fmod(s, length_);
    if (s < 0) s += length_;
    int i = wrap((int)(s / ds_));
    float f = (s - samples_[i].s) / ds_;
    const auto& a = samples_[i];
    const auto& b = at(i + 1);
    float z = a.z + (b.z - a.z) * f;
    float bank = a.bank + (b.bank - a.bank) * f;
    return z + lateral * std::tan(bank);
}

void Track::shapeAt(float s, float* grade, float* bank, float* vcurv) const {
    s = std::fmod(s, length_);
    if (s < 0) s += length_;
    int i = wrap((int)(s / ds_));
    float f = (s - samples_[i].s) / ds_;
    const auto& a = samples_[i];
    const auto& b = at(i + 1);
    if (grade) *grade = a.grade + (b.grade - a.grade) * f;
    if (bank) *bank = a.bank + (b.bank - a.bank) * f;
    if (vcurv) *vcurv = a.vcurv + (b.vcurv - a.vcurv) * f;
}

void Track::buildEdgeGrid() {
    float minX = 1e30f, minY = 1e30f, maxX = -1e30f, maxY = -1e30f;
    for (int i = 0; i < size(); ++i)
        for (Vec2 p : {leftEdge(i), rightEdge(i)}) {
            minX = std::min(minX, p.x); maxX = std::max(maxX, p.x);
            minY = std::min(minY, p.y); maxY = std::max(maxY, p.y);
        }
    gridX0_ = minX - gridCell_;
    gridY0_ = minY - gridCell_;
    gridW_ = (int)((maxX - gridX0_) / gridCell_) + 2;
    gridH_ = (int)((maxY - gridY0_) / gridCell_) + 2;
    std::vector<std::vector<int>> cells((size_t)gridW_ * gridH_);
    auto insert = [&](Vec2 a, Vec2 b, int item) {
        int x0 = (int)((std::min(a.x, b.x) - gridX0_) / gridCell_), x1 = (int)((std::max(a.x, b.x) - gridX0_) / gridCell_);
        int y0 = (int)((std::min(a.y, b.y) - gridY0_) / gridCell_), y1 = (int)((std::max(a.y, b.y) - gridY0_) / gridCell_);
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x) cells[(size_t)y * gridW_ + x].push_back(item);
    };
    for (int i = 0; i < size(); ++i) {
        insert(leftEdge(i), leftEdge(i + 1), i);
        insert(rightEdge(i), rightEdge(i + 1), ~i);
    }
    gridStart_.assign(cells.size() + 1, 0);
    gridItems_.clear();
    for (size_t c = 0; c < cells.size(); ++c) {
        gridStart_[c] = (int)gridItems_.size();
        gridItems_.insert(gridItems_.end(), cells[c].begin(), cells[c].end());
    }
    gridStart_[cells.size()] = (int)gridItems_.size();
}

float Track::raycastEdge(Vec2 o, Vec2 dir, float maxRange) const {
    float best = maxRange;
    auto test = [&](int item) {
        int i = item >= 0 ? item : ~item;
        Vec2 a = item >= 0 ? leftEdge(i) : rightEdge(i);
        Vec2 b = item >= 0 ? leftEdge(i + 1) : rightEdge(i + 1);
        Vec2 e = b - a;
        float den = cross(dir, e);
        if (std::fabs(den) < 1e-9f) return;
        Vec2 ao = a - o;
        float u = cross(ao, e) / den;    // along the ray
        float v = cross(ao, dir) / den;  // along the segment
        if (u > 1e-4f && u < best && v >= 0.0f && v <= 1.0f) best = u;
    };
    // Walk the grid cells along the ray (Amanatides & Woo).
    float fx = (o.x - gridX0_) / gridCell_, fy = (o.y - gridY0_) / gridCell_;
    int cx = (int)std::floor(fx), cy = (int)std::floor(fy);
    int stepX = dir.x > 0 ? 1 : -1, stepY = dir.y > 0 ? 1 : -1;
    float tDeltaX = std::fabs(dir.x) > 1e-9f ? gridCell_ / std::fabs(dir.x) : 1e30f;
    float tDeltaY = std::fabs(dir.y) > 1e-9f ? gridCell_ / std::fabs(dir.y) : 1e30f;
    float tMaxX = std::fabs(dir.x) > 1e-9f ? ((dir.x > 0 ? (cx + 1 - fx) : (fx - cx)) * gridCell_) / std::fabs(dir.x) : 1e30f;
    float tMaxY = std::fabs(dir.y) > 1e-9f ? ((dir.y > 0 ? (cy + 1 - fy) : (fy - cy)) * gridCell_) / std::fabs(dir.y) : 1e30f;
    float t = 0;
    while (t < best) {
        if (cx < 0 || cy < 0 || cx >= gridW_ || cy >= gridH_) break;
        size_t c = (size_t)cy * gridW_ + cx;
        for (int k = gridStart_[c]; k < gridStart_[c + 1]; ++k) test(gridItems_[k]);
        // a hit inside this cell cannot be beaten by later cells
        if (tMaxX < tMaxY) { t = tMaxX; tMaxX += tDeltaX; cx += stepX; }
        else { t = tMaxY; tMaxY += tDeltaY; cy += stepY; }
    }
    return best;
}

}  // namespace rr
