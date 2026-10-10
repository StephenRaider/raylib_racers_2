#pragma once
// Rubber on the track. Tyres lay rubber where they wear, so the racing line gets grippier as cars
// use it (about 3% more grip on a line that has seen a lot of cars) and the rest of the track stays green.
// The map belongs to the weekend, not to a session: Race adds to the one in RaceConfig::rubber, so
// practice, qualifying and the race share it. Cells are 4 m along the track by 0.5 m across.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace rr {

class TrackRubber {
public:
    static constexpr float kCellS = 4.0f, kCellL = 0.5f;
    static constexpr int kLanes = 32;          // +-8 m from the centreline
    static constexpr float kGain = 0.03f;      // grip gained on a fully rubbered cell
    static constexpr float kPerWear = 230.0f;  // rubber laid per unit of tyre wear (a car lap on the line is about 1/100 of a cell)

    void reset(float trackLength) {
        length_ = trackLength;
        cols_ = std::max(1, (int)std::ceil(trackLength / kCellS));
        r_.assign((size_t)cols_ * kLanes, 0.0f);
        ++version_;
    }
    bool empty() const { return r_.empty(); }
    int cols() const { return cols_; }
    float cell(int col, int lane) const { return r_[(size_t)col * kLanes + lane]; }  // 0 green .. 1 saturated
    float length() const { return length_; }
    uint32_t version() const { return version_; }  // changes whenever rubber is laid, for a viewer that caches meshes

    // The mean over the track of the most rubbered lane in each column: how built up the racing line is.
    float meanLine() const {
        if (cols_ == 0) return 0.0f;
        double sum = 0;
        for (int c = 0; c < cols_; ++c) {
            float best = 0;
            for (int l = 0; l < kLanes; ++l) best = std::max(best, cell(c, l));
            sum += best;
        }
        return (float)(sum / cols_);
    }
    // Rubber (0..1) under a point at track distance s and lateral offset (+ = left).
    float at(float s, float lateral) const {
        const int i = index(s, lateral);
        return i < 0 ? 0.0f : r_[(size_t)i];
    }
    // Grip multiplier there.
    float grip(float s, float lateral) const { return 1.0f + kGain * at(s, lateral); }
    // `wear` is the tyre wear a wheel has just taken on this spot.
    void add(float s, float lateral, float wear) {
        const int i = index(s, lateral);
        if (i >= 0 && r_[(size_t)i] < 1.0f) {
            r_[(size_t)i] = std::min(1.0f, r_[(size_t)i] + wear * kPerWear);
            ++version_;
        }
    }

private:
    int index(float s, float lateral) const {
        if (r_.empty()) return -1;
        const int lane = (int)std::floor(lateral / kCellL) + kLanes / 2;
        if (lane < 0 || lane >= kLanes) return -1;
        int col = (int)std::floor(s / kCellS) % cols_;
        if (col < 0) col += cols_;
        return col * kLanes + lane;
    }
    float length_ = 0;
    int cols_ = 0;
    uint32_t version_ = 0;
    std::vector<float> r_;
};

}  // namespace rr
