#pragma once
#include <string>
#include <vector>

#include "rr/robot_api.h"
#include "vec2.hpp"

namespace rr {

struct TrackSample {
    Vec2 p;            // centreline point
    Vec2 t;            // unit tangent (race direction)
    Vec2 n;            // unit left normal
    float s = 0;       // arc length from the start line
    float halfWidth = 7;
    float curvature = 0;  // 1/m, + = left
    // 3D shape. The sim is still 2D: s and curvature are measured in plan view,
    // and these are for the renderer (and later the 3D bot API).
    float z = 0;          // centreline height, m
    float bank = 0;       // road bank angle, rad, + = left edge raised
    float grade = 0;      // dz/ds, + = uphill in race direction
};

// Where a point is relative to the track.
struct TrackLoc {
    int idx = 0;        // segment start sample
    float s = 0;        // arc length of the projection, [0, length)
    float lateral = 0;  // signed distance from the centreline, + = left
    float halfWidth = 7;
};

// A closed track built from control points with a Catmull-Rom spline and
// resampled to ~1 m spacing.
//
// File format (.trk, text, '#' comments):
//   name   <words>
//   width  <metres>            default tarmac width
//   runoff <metres>            tarmac edge to barrier
//   p <x> <y> [width] [h=<m>] [bank=<deg>]
//                              control point, in race order; start line at the first.
//                              width <= 0 or "-" uses the default; h is the road
//                              height, bank is + when the left edge is higher
//   pit left|right <entry_s> <lane_start_s> <lane_end_s> <exit_s>
//                              optional pit lane alongside the track (s in metres)
//   pitspeed <m/s>             pit lane speed limit (default 22)
class Track {
public:
    bool load(const std::string& path, std::string* err);
    // heights and banks (degrees) are optional: empty means flat.
    bool build(const std::vector<Vec2>& ctrl, const std::vector<float>& widths, std::string* err,
               const std::vector<float>& heights = {}, const std::vector<float>& banks = {});

    const std::string& name() const { return name_; }
    // The viewer's scenery theme ("scenery forest" in the file; "" = the default mix).
    const std::string& scenery() const { return scenery_; }
    float length() const { return length_; }
    float runoff() const { return runoff_; }
    float spacing() const { return ds_; }
    int size() const { return (int)samples_.size(); }
    const TrackSample& at(int i) const { return samples_[wrap(i)]; }
    int wrap(int i) const {
        int n = size();
        i %= n;
        return i < 0 ? i + n : i;
    }
    int indexAt(float s) const;

    // Local search around a hint index (fast), or over the whole track.
    TrackLoc locate(Vec2 p, int hint, int window) const;
    TrackLoc locateGlobal(Vec2 p) const;

    Vec2 pointAt(float s, float lateral) const;
    Vec2 dirAt(float s) const;
    // Road surface height at track distance s and lateral offset (banking
    // included; beyond the tarmac edge the bank plane is simply extended).
    float heightAt(float s, float lateral = 0) const;
    // True when any control point has a height or bank.
    bool is3D() const { return is3D_; }

    // Distance along a ray to the first tarmac edge it crosses, or maxRange.
    float raycastEdge(Vec2 origin, Vec2 dir, float maxRange) const;

    // Pit lane. Lateral values are unsigned distances from the centreline on
    // the pit side.
    bool hasPit() const { return info_.pit.has_pit != 0; }
    const RRPitInfo& pit() const { return info_.pit; }
    bool inSpan(float s, float a, float b) const;  // s in [a, b], all wrapped
    bool inPitArea(float s) const;    // between pit entry and exit
    bool inPitLane(float s) const;    // between lane start and end (divider present)
    static constexpr float kDividerIn = 2.2f, kDividerOut = 2.8f;  // beyond the tarmac edge
    static constexpr float kLaneCentre = 5.0f, kBoxCentre = 9.0f, kPitBarrier = 12.5f;
    // Distance from the centreline to the barrier on side (+1 left, -1 right).
    float barrierOffset(float s, int side, float halfWidth) const;
    // Paved for grip purposes: tarmac plus kerbs, plus the pit area.
    bool paved(float s, float lateral, float halfWidth) const;

    // Self-intersection / overlap warnings found while building.
    const std::vector<std::string>& warnings() const { return warnings_; }

    const RRTrackInfo& info() const { return info_; }

    // The corners, numbered from the start line (see RRTurn).
    const std::vector<RRTurn>& turns() const { return turns_; }
    // The turn (index into turns()) at track distance s, -1 on a straight.
    int turnAt(float s) const { return turns_.empty() ? -1 : turnOf_[indexAt(s)]; }
    // The next turn starting ahead of s (after the current one), and the distance to its start.
    int nextTurn(float s, float* ds) const;

    // Border polylines (same indexing as samples).
    Vec2 leftEdge(int i) const { const auto& s = at(i); return s.p + s.n * s.halfWidth; }
    Vec2 rightEdge(int i) const { const auto& s = at(i); return s.p - s.n * s.halfWidth; }

private:
    void finalize();
    void buildEdgeGrid();

    // Uniform grid over the edge segments for fast ray casts. Each entry is a
    // segment start index; >= 0 for the left edge, ~i for the right edge.
    float gridCell_ = 8.0f;
    float gridX0_ = 0, gridY0_ = 0;
    int gridW_ = 0, gridH_ = 0;
    std::vector<int> gridStart_;  // gridW*gridH + 1 offsets into gridItems_
    std::vector<int> gridItems_;

    std::string name_ = "unnamed";
    std::string scenery_;
    RRPitInfo pitCfg_{};
    float defaultWidth_ = 14;
    float runoff_ = 6;
    float length_ = 0;
    float ds_ = 1;
    bool is3D_ = false;
    std::vector<TrackSample> samples_;
    std::vector<RRTrackPoint> apiPoints_;
    RRTrackInfo info_{};
    std::vector<RRTurn> turns_;
    std::vector<int> turnOf_;
    void findTurns();
    std::vector<std::string> warnings_;
};

}  // namespace rr
