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
        } else if (key == "pitspeed") {
            ss >> pitCfg_.speed_limit;
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
    for (int i = 0; i < n; ++i) {
        const auto& s = samples_[i];
        apiPoints_[i] = {s.p.x, s.p.y, s.t.x, s.t.y, s.s, s.halfWidth, s.curvature};
    }
    info_.name = name_.c_str();
    info_.length = length_;
    info_.runoff = runoff_;
    info_.num_points = n;
    info_.points = apiPoints_.data();
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
    buildEdgeGrid();
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
    float a = std::fabs(lateral);
    if (a <= halfWidth + 1.2f) return true;
    int side = lateral > 0 ? 1 : -1;
    return side == info_.pit.side && inPitArea(s) && a <= halfWidth + kPitBarrier;
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
