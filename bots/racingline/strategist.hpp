// racingline's race strategist: given what the car has measured about itself
// (fuel and tyre wear per lap, lap time, pit loss) it finds the fastest way to
// the flag: how many stops, on which laps, on which compounds and with how
// much fuel. racingline.cpp re-plans once a lap and adds the racecraft on top
// (undercut, covering a rival's stop, rejoining in clear air).
//
// The model is deliberately simple, one lap at a time:
//   lap time = reference lap
//            + compound pace (see Compounds: it depends on how hot the driver runs the tyres)
//            + tyre wear (the grip lost to wear, at ~0.3 s of lap time per 1% grip)
//            + fuel weight (~0.023 s per kg per lap)
// and a stop costs the pit lane time plus the service (refuelling dominates).
#pragma once
#include <algorithm>
#include <cmath>

#include "rr/robot_api.h"

namespace strat {

// How each compound behaves for a driver, relative to the medium: wear rate and
// lap time. Both depend on how hot the driver runs the tyres (racingline's
// `heat`): the soft's window is the lowest, so a driver who runs hot overheats
// it, wearing it ~2.75x as fast as a medium for almost no pace; one who keeps
// the tyres cool gets ~2.5% a lap from it at 2x the wear. Measured on the
// circuit with the three racingline styles.
struct Compounds {
    float wear[4] = {1, 2.0f, 1, 0.72f};    // by RR_TIRE_*
    float pace[4] = {0, -0.025f, 0, 0.035f};
    static Compounds forHeat(float heat) {
        const float h = std::clamp(heat / 5.0f, 0.0f, 1.0f);
        Compounds c;
        c.wear[RR_TIRE_SOFT] = 2.0f + 0.75f * h;
        c.pace[RR_TIRE_SOFT] = -0.025f + 0.023f * h;
        c.pace[RR_TIRE_HARD] = 0.035f - 0.02f * h;
        return c;
    }
};
inline int okCompound(int c) { return c >= RR_TIRE_SOFT && c <= RR_TIRE_HARD ? c : RR_TIRE_MEDIUM; }

struct Model {
    float lapRef = 55;          // s, a clean lap on mediums at average fuel
    float fuelPerLap = 2.3f;    // l
    float wearPerLapMed = 0.025f;  // worse axle, per lap, as if on mediums
    float wearLimit = 0.7f;     // past this the grip falls off a cliff
    float wearGrowth = 1.2f;    // worn tyres slide more and wear faster: rate x (1 + growth x wear)
    float pitLoss = 20;         // s lost driving through the pit lane (without the service)
    float serviceScale = 1;     // pit_service_scale
    float fuelCap = 65, fuelDensity = 0.75f;
    float fuelSecPerKg = 0.023f;
    int maxStops = 3;
    // What a stop costs beyond the pit lane and the service: the out lap on
    // cold tyres and rejoining in traffic. Stops that only just pay off on
    // paper do not pay off on track.
    float stopRisk = 4.0f;
    Compounds tyres;

    float compoundWear(int c) const { return tyres.wear[okCompound(c)]; }
    float compoundPace(int c) const { return tyres.pace[okCompound(c)]; }

    float serviceTime(float fuel, bool tyres) const {
        return serviceScale * (RR_PIT_SERVICE_BASE + std::max(fuel / RR_PIT_FUEL_RATE, tyres ? RR_PIT_TIRE_CHANGE : 0.0f));
    }
    float wearCost(float w) const {  // s per lap from worn tyres
        return lapRef * 0.3f * (0.07f * w + 0.8f * std::max(0.0f, w - 0.7f));
    }
    float wearRate(int c, float w) const { return wearPerLapMed * compoundWear(c) * (1 + wearGrowth * w); }
    // Time for n laps on compound c starting at wear w and fuel f; updates w and f.
    float stint(int n, int c, float& w, float& f) const {
        float t = 0;
        for (int i = 0; i < n; ++i) {
            const float wpl = wearRate(c, w);
            t += lapRef * (1 + compoundPace(c)) + wearCost(w + 0.5f * wpl) + fuelSecPerKg * fuelDensity * std::max(0.0f, f);
            w += wpl;
            f -= fuelPerLap;
        }
        return t;
    }
};

struct Plan {
    bool valid = false;
    float cost = 1e30f;
    int stops = 0;
    int firstStint = 0;          // more laps on the current tyres after this one
    int compounds[4] = {0, 0, 0, 0};  // compound of each later stint
    int stintLaps[4] = {0, 0, 0, 0};
    float nextFuel = 0;          // litres to add at the next stop
    int windowLo = 0, windowHi = 0;  // range of firstStint within a second of the best
};

// How far past wearLimit the current stint may stretch (planned stints stop at
// wearLimit, leaving the margin for a wear rate that grows more than expected).
constexpr float kWearMargin = 0.06f;

inline int popcount(int m) { int n = 0; for (; m; m &= m - 1) ++n; return n; }

// lapsLeft: complete laps after the current one. lapFrac: share of the current
// lap still to drive before the pit entry decision applies (0 at the line).
// used: compounds used so far (bit 1 << RR_TIRE_*). rule: two-compound rule.
// stopNow: only plans that stop at the end of this lap (what to fit and fuel
// when racecraft calls the stop early).
inline Plan plan(const Model& m, int lapsLeft, int cur, float wear, float fuel, float lapFrac, int used, bool rule,
                 bool stopNow = false) {
    Plan best;
    const float reserve = 0.4f * m.fuelPerLap;
    struct Cand { float cost; int x; int k; int seq[4]; int len[4]; float nextFuel; };
    Cand bestC{1e30f, 0, 0, {0, 0, 0, 0}, {0, 0, 0, 0}, 0};
    // all candidates' costs per (x, k, seq), to find the window afterwards
    static float costs[128][4][81];
    for (auto& a : costs) for (auto& b : a) for (float& c : b) c = 1e30f;

    for (int k = 0; k <= m.maxStops; ++k) {
        const int nseq = k == 0 ? 1 : (k == 1 ? 3 : (k == 2 ? 9 : 27));
        for (int x = 0; x <= lapsLeft && x < 128; ++x) {
            if (k == 0 && x != lapsLeft) continue;
            if (k > 0 && x >= lapsLeft) continue;
            if (stopNow && (k == 0 || x != 0)) continue;
            const int rest = lapsLeft - x;
            if (k > rest) continue;
            // first stint: the current tyres and fuel
            float w = wear, f = fuel - lapFrac * m.fuelPerLap;
            float t = m.stint(x, cur, w, f);
            // to the flag a splash of reserve is enough; at the pit entry nothing is needed
            const float minFuel = k == 0 ? 0.1f * m.fuelPerLap : (x == 0 ? 0.0f : reserve * 0.5f);
            if (f < minFuel || w > m.wearLimit + kWearMargin) continue;
            for (int s = 0; s < nseq; ++s) {
                int seq[4] = {0, 0, 0, 0}, len[4] = {0, 0, 0, 0};
                for (int i = 0, q = s; i < k; ++i, q /= 3) seq[i] = RR_TIRE_SOFT + q % 3;
                for (int i = 0; i < k; ++i) len[i] = rest / k + (i < rest % k ? 1 : 0);
                float tt = t, ff = f, firstFuel = 0;
                bool ok = true;
                int mask = used;
                for (int i = 0; i < k && ok; ++i) {
                    const float need = len[i] * m.fuelPerLap * 1.06f + reserve;  // consumption varies a few % (more on a light car)
                    const float add = std::max(0.0f, need - std::max(0.0f, ff));
                    if (std::max(0.0f, ff) + add > m.fuelCap) { ok = false; break; }
                    if (i == 0) firstFuel = add;
                    ff = std::max(0.0f, ff) + add;
                    tt += m.pitLoss + m.serviceTime(add, true) + m.stopRisk;
                    float ww = 0;
                    tt += m.stint(len[i], seq[i], ww, ff);
                    if (ww > m.wearLimit) ok = false;  // fresh stints keep the margin for surprises
                    mask |= 1 << seq[i];
                }
                if (!ok) continue;
                if (rule && popcount(mask) < 2) tt += RR_TWO_COMPOUND_PENALTY;
                costs[x][k][s] = tt;
                if (tt < bestC.cost) {
                    bestC = {tt, x, k, {seq[0], seq[1], seq[2], seq[3]}, {len[0], len[1], len[2], len[3]}, firstFuel};
                }
            }
        }
    }
    if (bestC.cost >= 1e29f) return best;
    best.valid = true;
    best.cost = bestC.cost;
    best.stops = bestC.k;
    best.firstStint = bestC.x;
    for (int i = 0; i < 4; ++i) { best.compounds[i] = bestC.seq[i]; best.stintLaps[i] = bestC.len[i]; }
    best.nextFuel = bestC.nextFuel;
    // the window: other first-stint lengths for the same number of stops and
    // compounds that cost at most a second more
    int sIdx = 0;
    for (int i = bestC.k - 1; i >= 0; --i) sIdx = sIdx * 3 + (bestC.seq[i] - RR_TIRE_SOFT);
    best.windowLo = best.windowHi = bestC.x;
    if (bestC.k > 0)
        for (int x = 0; x < lapsLeft && x < 128; ++x)
            if (costs[x][bestC.k][sIdx] <= bestC.cost + 1.0f) {
                best.windowLo = std::min(best.windowLo, x);
                best.windowHi = std::max(best.windowHi, x);
            }
    return best;
}

}  // namespace strat
