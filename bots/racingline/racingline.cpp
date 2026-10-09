// racingline: a planning robot. At create() it computes a minimum-curvature
// racing line inside the track limits and a grip-limited speed profile, then
// drives it with pure pursuit and a speed controller.
//
// On top of that it races:
//   - the speed profile is re-planned every lap for the current fuel load and
//     tyre grip;
//   - strategy (strategist.hpp): it measures its own fuel use, tyre wear, lap
//     time and pit loss, and re-plans every lap how many stops to make, when,
//     on which compounds and with how much fuel. Within the planned window it
//     reads the timing screen: it pits early to undercut the car ahead or to
//     cover a rival behind that has just stopped, stays out to overcut a car
//     ahead that has just pitted, and waits a lap when it would rejoin right
//     behind another car;
//   - overtaking: picks the side with more room around the car ahead and does
//     not run into the back of it;
//   - defending: when a car close behind is closing, covers the inside of the
//     next corner, one move, then holds it;
//   - awareness: never moves across a car alongside or one closing from
//     behind, and brakes in time for the car ahead;
//   - blue flags: moves aside and lifts for a car that is lapping it;
//   - the limit: learns, per 20 m of track, how much grip there really is.
//   - the weekend: practice learns the limit faster and probes a little past
//     it, and runs a stint on each compound (medium, soft, hard, medium again)
//     to measure each one's wear, fuel use and pace (a fit that separates the
//     compounds from the track getting quicker as it learns); the grip per
//     20 m, pace, fuel use, wear per compound and pit loss are kept in the
//     weekend memory: qualifying runs the fastest compound, the race plans
//     with the measured numbers.
//   - brakes and tyres: cold or faded discs move the braking points, hot
//     discs bring lift and coast, and the hottest single tyre sets how much
//     to back off.
//     Sliding (front or rear past its grip) lowers that stretch's speed and the
//     braking zone before it; clean laps well inside the limit raise it. On top
//     of that a driver-aid layer (rr_awareness.h) catches oversteer and manages
//     wheelspin.
//
// params:
//   grip=<0.8>     share of the tyre friction the speed profile may use
//   brake=<0.7>    share of the braking capacity the profile assumes
//   margin=<1.3>   distance kept from the tarmac edge, m
//   look=<1.0>     pure-pursuit lookahead scale
//   pass=<1>       0 disables overtaking moves
//   defend=<1>     0 disables defending
//   wear=<0.7>     tyre wear at which to stop for new tyres
//   fuel=<auto>    litres at the start (default: what the planned first stint needs)
//   tires=<auto>   starting compound: 1 soft, 2 medium, 3 hard (default: the strategist picks;
//                  a team choice in the menu or --tires overrides both)
//   pit=<1>        0 never stops
//   push=<1.2>     how far above `grip` the learnt limit may go (1 = never)
//   learn=<1>      0 disables learning the limit
//   attack=<1>     racecraft aggression: > 1 follows closer and looks for gaps sooner
//   heat=<5>       how far (C) past the top of the tyres' window it keeps pushing before
//                  backing off to cool them
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "../common/rr_awareness.h"
#include "../common/rr_params.h"
#include "../common/rr_recovery.h"
#include "rr/robot_api.h"
#include "strategist.hpp"

// Other robots are this one with their own defaults: bots/dave/dave.cpp sets
// RL_NAME and RL_DEFAULTS and includes this file. Params given at race time
// still win over RL_DEFAULTS.
#ifndef RL_NAME
#define RL_NAME "racingline"
#endif
#ifndef RL_DEFAULTS
#define RL_DEFAULTS ""
#endif

namespace {

float param(const char* params, const char* key, float def) {
    return rr_param(params, key, rr_param(RL_DEFAULTS, key, def));
}

constexpr float kCoastDist = 250.0f;  // m of lift and coast before a braking zone at full fuel saving

struct P2 { float x, y; };

enum Mode { RACE, PIT_IN, PIT_OUT };

float smooth01(float u) { u = std::clamp(u, 0.0f, 1.0f); return u * u * (3 - 2 * u); }

struct RacingLine {
    // settings
    float grip, brakeScale, margin, look, wearLimit;
    bool pass, defend, usePit;
    RRCarSpec car;
    RRPitInfo pit;
    float ds, L;
    // track copy and plan (indexed like the track samples)
    std::vector<RRTrackPoint> tp;
    std::vector<RRTrackPoint3> tp3;  // heights, banking, crests (all 0 on a flat track)
    std::vector<float> offset;   // lateral offset of the line, + = left
    std::vector<P2> line;
    std::vector<float> kappa;    // curvature of the line, 1/m
    std::vector<float> kappaSigned;  // the same, + = turning left
    float yawGain = 0.06f;
    std::vector<float> speed;    // target speed
    float plannedMass = 0, plannedGrip = 0;
    float damage = 0;            // 0..1, as the speed profile last assumed
    float attack = 1.0f;         // racecraft: > 1 follows closer and goes for gaps sooner
    float heat = 5.0f;           // C past the tyre window tolerated before backing off
    float fuelSave = 0;          // 0..1: lift and coast before braking zones (save= forces it)
    float fuelSaveParam = 0;
    float tyreNow = 1.0f;        // speed factor for the tyres' temperature right now (smoothed)
    float startLat = 0, startDist = 0;  // grid slot: hold that lane off the line, then ease onto the racing line
    float sideLo = -1e9f, sideHi = 1e9f;  // lateral room left by cars alongside (absolute, m)
    int plannedLap = -1;
    // brakes: the discs bite less cold and fade hot, so brake earlier for them
    float brakeNow = 1.0f;       // disc grip now, smoothed (1 inside the window)
    float brakePlanned = 1.0f;   // the disc grip the speed profile assumes
    double brakeReplanAt = 0;
    bool brakesHot = false;      // lift and coast to cool them
    // the weekend
    int session = RR_SESSION_RACE;
    unsigned char* memory = nullptr;
    int memorySize = 0;
    bool haveNotes = false;
    float noteLapRef = 0, noteFuel = 0, noteWear = 0, notePitLoss = 0, noteSpan = 0;
    // Each compound as measured on clean laps: lap time taken to an empty tank, tyre
    // wear (worse axle, growth taken out) and fuel per lap. Practice runs every
    // compound for this; the notes carry it to qualifying and the race.
    struct CompoundRun { int laps = 0; float time = 0, wear = 0, fuel = 0; };  // sums over laps
    CompoundRun runs[4];
    CompoundRun noteRuns[4];         // from the notes (earlier sessions)
    float lapStartFuel = 0, lapStartWear = 0;
    struct LapRecord { int lap, compound; float time; };
    std::vector<LapRecord> lapLog;   // clean laps this session, for the compound pace fit
    float notePace[4] = {};          // lap time against the medium (fraction), fitted in practice
    bool notePaceSet = false;
    // The practice programme: stints on each compound, the stop at the end of each.
    int progTires[4] = {}, progEnd[4] = {}, progStints = 0, progAt = 0;

    // learning the limit: grip multiplier per 20 m bin
    static constexpr float kBin = 20.0f;
    std::vector<float> adj;
    float push = 1.2f;
    bool learn = true, adjDirty = false;
    int bin = -1;
    float binUse = 0;          // worst grip use seen in the current bin
    bool binClean = true;      // no traffic or racecraft moves in this bin
    bool binLost = false;      // spun, ran off or needed recovery
    float lastAccel = 0;       // throttle we asked for last time
    int binOf(int i) const { return std::min((int)adj.size() - 1, (int)(tp[wrap(i)].s / kBin)); }

    // racecraft
    float passOffset = 0;        // current shift from the line, m
    int passCar = -1;            // car we are passing
    float passSide = 0;          // side we pass on
    float defendSide = 0;        // side we are covering, 0 = not defending
    float blueSide = 0;          // side we keep to while being lapped
    double defendUntil = 0, defendCooldown = 0;
    float tc = 1;                // traction-control throttle limit
    RRRecovery recovery{};

    // strategy
    Mode mode = RACE;
    bool decidedThisLap = false;
    strat::Model model;
    strat::Plan planNow;
    int planLap = 0;                      // lap the plan was made on
    float lapRefN = 0;                    // clean laps averaged into model.lapRef
    double entryTime = -1;                // when we last passed the pit entry
    bool entryPitting = false;
    float spanNormal = 0;                 // s from pit entry to pit exit when not stopping
    float lastService = 0;                // s, the service of the stop under way
    int prevStops[RR_MAX_CARS] = {};      // rivals' stop counts at our last decision
    bool cleanLap = true;                 // no pit, recovery or start on this lap
    float fuelRef = 0, fuelDistRef = 0;   // since the last refuel
    float wearRef = 0, wearDistRef = 0;   // since the last tyre change
    int stopsSeen = 0;
    int index = 0;                        // our car index
    RRControl order{};                    // what we ask the crew for
    char plan[48] = "";

    int n() const { return (int)tp.size(); }
    int wrap(int i) const { int m = n(); i %= m; return i < 0 ? i + m : i; }
    P2 pointAt(int i, float off) const {
        const auto& p = tp[wrap(i)];
        return {p.x - p.dir_y * off, p.y + p.dir_x * off};
    }
    float fwd(float from, float to) const { float d = std::fmod(to - from, L); return d < 0 ? d + L : d; }
    float signedDs(float from, float to) const { float d = fwd(from, to); return d > L * 0.5f ? d - L : d; }
    bool inSpan(float s, float a, float b) const { return fwd(a, s) <= fwd(a, b); }
};

// Minimise the summed squared curvature of the line (squared second
// differences of the points) by projected gradient descent on the lateral
// offsets. Coarse-to-fine so long corners converge quickly.
void planLine(RacingLine& r) {
    const int n = r.n();
    std::vector<float> full(n, 0.0f);
    for (float spacing : {24.0f, 12.0f, 6.0f, 3.0f}) {
        const int stride = std::max(1, (int)std::lround(spacing / r.ds));
        const int m = n / stride;
        if (m < 8) continue;
        std::vector<float> e(m), lim(m);
        std::vector<P2> c(m), nr(m), pos(m), d2(m);
        for (int k = 0; k < m; ++k) {
            const auto& p = r.tp[k * stride];
            c[k] = {p.x, p.y};
            nr[k] = {-p.dir_y, p.dir_x};
            lim[k] = std::max(0.0f, p.half_width - r.margin);
            e[k] = full[k * stride];
        }
        for (int it = 0; it < 3000; ++it) {
            for (int k = 0; k < m; ++k) pos[k] = {c[k].x + nr[k].x * e[k], c[k].y + nr[k].y * e[k]};
            for (int k = 0; k < m; ++k) {
                const P2& a = pos[(k - 1 + m) % m];
                const P2& b = pos[k];
                const P2& d = pos[(k + 1) % m];
                d2[k] = {a.x - 2 * b.x + d.x, a.y - 2 * b.y + d.y};
            }
            for (int k = 0; k < m; ++k) {
                const P2& a = d2[(k - 1 + m) % m];
                const P2& b = d2[k];
                const P2& d = d2[(k + 1) % m];
                float gx = a.x - 2 * b.x + d.x, gy = a.y - 2 * b.y + d.y;
                e[k] = std::clamp(e[k] - 0.05f * (gx * nr[k].x + gy * nr[k].y), -lim[k], lim[k]);
            }
        }
        // Back to full resolution (the last coarse interval may be longer than stride).
        for (int i = 0; i < n; ++i) {
            int k0 = std::min(i / stride, m - 1), k1 = (k0 + 1) % m;
            int i0 = k0 * stride, i1 = k1 == 0 ? n : k1 * stride;
            float f = (float)(i - i0) / (float)(i1 - i0);
            full[i] = e[k0] * (1 - f) + e[k1] * f;
        }
    }
    r.offset.resize(n);
    for (int i = 0; i < n; ++i) {
        float lim = std::max(0.0f, r.tp[i].half_width - r.margin);
        r.offset[i] = std::clamp(full[i], -lim, lim);
    }
    r.line.resize(n);
    for (int i = 0; i < n; ++i) r.line[i] = r.pointAt(i, r.offset[i]);

    r.kappa.resize(n);
    r.kappaSigned.resize(n);
    const int h = 4;  // circle through points i-h, i, i+h
    for (int i = 0; i < n; ++i) {
        P2 a = r.line[r.wrap(i - h)], b = r.line[i], c = r.line[r.wrap(i + h)];
        float abx = b.x - a.x, aby = b.y - a.y, bcx = c.x - b.x, bcy = c.y - b.y, acx = c.x - a.x, acy = c.y - a.y;
        float cr = abx * bcy - aby * bcx;
        float la = std::sqrt(abx * abx + aby * aby), lb = std::sqrt(bcx * bcx + bcy * bcy),
              lc = std::sqrt(acx * acx + acy * acy);
        r.kappaSigned[i] = (la * lb * lc > 1e-6f) ? 2 * cr / (la * lb * lc) : 0.0f;
        r.kappa[i] = std::fabs(r.kappaSigned[i]);
    }
}

// The fastest speed through a point of curvature k (signed, + = left) with
// friction mu and downforce D (per unit v^2) on a car of mass m, on a road
// that may be banked, cresting or compressing (p3): the tyres must supply
// v^2 k plus the pull of the bank, from a load of g plus the compression's
// v^2 kv plus the cornering force pressed into the bank.
float cornerSpeed(float k, float mu, float D, float m, const RRTrackPoint3& p3, float vCap) {
    const float g = 9.81f;
    if (p3.bank == 0 && p3.vert_curvature == 0) {
        // m v^2 k = mu (m g + D v^2)  ->  v^2 = mu g / (k - mu D / m)
        float den = std::fabs(k) - mu * D / m;
        return den > 1e-6f ? std::min(vCap, std::sqrt(mu * g / den)) : vCap;
    }
    const float tb = std::tan(p3.bank), sb = std::sin(p3.bank);
    auto ok = [&](float v) {
        float v2 = v * v;
        float need = std::fabs(v2 * k + g * tb);
        float load = g * std::cos(std::atan(std::hypot(p3.grade, tb))) + v2 * p3.vert_curvature - v2 * k * sb + D * v2 / m;
        return load > 0 && need <= mu * load;
    };
    if (ok(vCap)) return vCap;
    float lo = 0, hi = vCap;
    for (int it = 0; it < 24; ++it) {
        float mid = 0.5f * (lo + hi);
        (ok(mid) ? lo : hi) = mid;
    }
    return lo;
}

// Grip-limited speed for a given mass and tyre grip: lateral limit from
// curvature, then a backward pass for braking.
void planSpeed(RacingLine& r, float mass, float tyreGrip) {
    const int n = r.n();
    const float g = 9.81f, m = mass;
    // Damage costs downforce and mechanical grip (see RRSensors.damage).
    const float mu0 = r.car.tire_mu * r.grip * tyreGrip * (1 - 0.08f * r.damage);
    const float D = r.car.downforce_coeff * (1 - 0.35f * r.damage), drag = r.car.drag_coeff * (1 + 0.1f * r.damage);
    const float vCap = 95.0f;
    const auto& kappa = r.kappa;
    r.speed.assign(n, vCap);
    for (int i = 0; i < n; ++i) {
        const float mu = mu0 * r.adj[r.binOf(i)];
        r.speed[i] = cornerSpeed(r.kappaSigned[i], mu, D, m, r.tp3[i], vCap);
    }
    // Backward pass: v_i^2 <= v_{i+1}^2 + 2 a ds, a from the friction left over after cornering.
    for (int pass = 0; pass < 2; ++pass) {
        for (int i = n - 1; i >= 0; --i) {
            int j = r.wrap(i + 1);
            const float mu = mu0 * r.adj[r.binOf(j)];
            float v = r.speed[j];
            const RRTrackPoint3& p3 = r.tp3[j];
            float normal = m * g + D * v * v, lat = m * v * v * kappa[j];
            if (p3.bank != 0 || p3.vert_curvature != 0) {
                // the road's shape moves the load and the pull of the bank (see cornerSpeed)
                normal = std::max(0.0f, m * (g + v * v * p3.vert_curvature - v * v * r.kappaSigned[j] * std::sin(p3.bank)) + D * v * v);
                lat = m * std::fabs(v * v * r.kappaSigned[j] + g * std::tan(p3.bank));
            }
            float fLong = std::sqrt(std::max(0.0f, mu * mu * normal * normal - lat * lat));
            fLong = std::min(fLong, r.car.max_brake_force * r.brakePlanned);  // cold or faded discs give less
            // braking uphill gravity helps, downhill it fights the brakes
            float a = (fLong * r.brakeScale + drag * v * v) / m + g * p3.grade;
            float vMax = std::sqrt(v * v + 2 * a * r.ds);
            r.speed[i] = std::min(r.speed[i], vMax);
        }
    }
    r.plannedMass = mass;
    r.plannedGrip = tyreGrip;
}

void initStrategy(RacingLine& r, const char* params, RRRobotConfig* cfg);

// ---------------------------------------------------------------- weekend notes
// What practice and qualifying teach us, kept in the weekend memory the host
// carries from session to session: the grip learnt per 20 m of track and the
// strategy model's measured pace, fuel use, tyre wear and pit loss.
struct Notes {
    unsigned magic;        // kNotesMagic
    int bins;
    float trackLen;
    int sessions;          // sessions that wrote these notes
    float lapRef, fuelPerLap, wearPerLapMed, pitLoss, spanNormal;
    int runLaps[4];        // per compound (RR_TIRE_*): clean laps measured
    float runTime[4], runWear[4], runFuel[4];  // per compound: sums over those laps
    int paceSet;           // pace below fitted in practice
    float pace[4];         // per compound: lap time against the medium (fraction)
    float adj[1];          // bins entries
};
constexpr unsigned kNotesMagic = 0x524c4e32;  // "RLN2"

size_t notesSize(int bins) { return sizeof(Notes) + sizeof(float) * (size_t)std::max(0, bins - 1); }

void loadNotes(RacingLine& r) {
    const int bins = (int)r.adj.size();
    if (!r.memory || (size_t)r.memorySize < notesSize(bins)) return;
    Notes n;
    std::memcpy(&n, r.memory, sizeof n);
    if (n.magic != kNotesMagic || n.bins != bins || std::fabs(n.trackLen - r.L) > 1.0f) return;
    const float* adj = reinterpret_cast<const float*>(r.memory + offsetof(Notes, adj));
    for (int b = 0; b < bins; ++b) {
        float a;
        std::memcpy(&a, adj + b, sizeof a);
        if (std::isfinite(a)) r.adj[b] = std::clamp(a, 0.8f, r.push);
    }
    r.haveNotes = true;
    r.noteLapRef = n.lapRef;
    r.noteFuel = n.fuelPerLap;
    r.noteWear = n.wearPerLapMed;
    r.notePitLoss = n.pitLoss;
    r.noteSpan = n.spanNormal;
    for (int c = 0; c < 4; ++c)
        if (n.runLaps[c] > 0 && std::isfinite(n.runTime[c] + n.runWear[c] + n.runFuel[c]))
            r.noteRuns[c] = {n.runLaps[c], n.runTime[c], n.runWear[c], n.runFuel[c]};
    r.notePaceSet = n.paceSet != 0;
    for (int c = 0; c < 4; ++c) r.notePace[c] = std::isfinite(n.pace[c]) ? std::clamp(n.pace[c], -0.08f, 0.08f) : 0.0f;
}

// The compounds' pace from the practice laps: lap time = base + trend x lap +
// compound offset (the medium's is 0), least squares. The trend soaks up the
// track getting quicker as the limits are learnt. Needs every compound and at
// least six laps; false otherwise.
bool fitPace(const RacingLine& r, float pace[4]) {
    int count[4] = {};
    for (const auto& l : r.lapLog) ++count[l.compound];
    if (r.lapLog.size() < 6 || count[RR_TIRE_SOFT] < 1 || count[RR_TIRE_MEDIUM] < 2 || count[RR_TIRE_HARD] < 1)
        return false;
    // unknowns: base, trend, soft, hard
    double A[4][5] = {};
    for (const auto& l : r.lapLog) {
        const double x[4] = {1.0, (double)l.lap, l.compound == RR_TIRE_SOFT ? 1.0 : 0.0, l.compound == RR_TIRE_HARD ? 1.0 : 0.0};
        for (int i = 0; i < 4; ++i) {
            for (int j = 0; j < 4; ++j) A[i][j] += x[i] * x[j];
            A[i][4] += x[i] * l.time;
        }
    }
    for (int i = 0; i < 4; ++i) {  // Gaussian elimination with partial pivoting
        int p = i;
        for (int k = i + 1; k < 4; ++k)
            if (std::fabs(A[k][i]) > std::fabs(A[p][i])) p = k;
        if (std::fabs(A[p][i]) < 1e-9) return false;
        for (int j = 0; j < 5; ++j) std::swap(A[i][j], A[p][j]);
        for (int k = 0; k < 4; ++k) {
            if (k == i) continue;
            const double f = A[k][i] / A[i][i];
            for (int j = i; j < 5; ++j) A[k][j] -= f * A[i][j];
        }
    }
    const double base = A[0][4] / A[0][0], trend = A[1][4] / A[1][1];
    const double ref = base + trend * r.lapLog.back().lap;  // a medium lap as the session ended
    if (ref <= 1) return false;
    pace[RR_TIRE_MEDIUM] = 0;
    pace[RR_TIRE_SOFT] = (float)(A[2][4] / A[2][2] / ref);
    pace[RR_TIRE_HARD] = (float)(A[3][4] / A[3][3] / ref);
    return std::isfinite(pace[RR_TIRE_SOFT]) && std::isfinite(pace[RR_TIRE_HARD]);
}

// What we know of a compound: this session's laps, else the notes'.
const RacingLine::CompoundRun& knownRun(const RacingLine& r, int c) {
    return r.runs[c].laps >= 2 || r.runs[c].laps > r.noteRuns[c].laps ? r.runs[c] : r.noteRuns[c];
}

void saveNotes(const RacingLine& r) {
    const int bins = (int)r.adj.size();
    if (!r.memory || (size_t)r.memorySize < notesSize(bins)) return;
    Notes n{};
    std::memcpy(&n, r.memory, sizeof n);
    const int sessions = n.magic == kNotesMagic ? n.sessions + 1 : 1;
    n = Notes{};
    n.magic = kNotesMagic;
    n.bins = bins;
    n.trackLen = r.L;
    n.sessions = sessions;
    n.lapRef = r.model.lapRef;
    n.fuelPerLap = r.model.fuelPerLap;
    n.wearPerLapMed = r.model.wearPerLapMed;
    n.pitLoss = r.model.pitLoss;
    n.spanNormal = r.spanNormal;
    for (int c = 0; c < 4; ++c) {
        const RacingLine::CompoundRun& k = knownRun(r, c);
        n.runLaps[c] = k.laps;
        n.runTime[c] = k.time;
        n.runWear[c] = k.wear;
        n.runFuel[c] = k.fuel;
    }
    float pace[4] = {};
    if (fitPace(r, pace)) {
        n.paceSet = 1;
        for (int c = 0; c < 4; ++c) n.pace[c] = pace[c];
    } else {
        n.paceSet = r.notePaceSet;
        for (int c = 0; c < 4; ++c) n.pace[c] = r.notePace[c];
    }
    std::memcpy(r.memory, &n, offsetof(Notes, adj));
    std::memcpy(r.memory + offsetof(Notes, adj), r.adj.data(), sizeof(float) * (size_t)bins);
}

void sessionEnd(void* self, const RRSessionSummary* summary) {
    auto* r = static_cast<RacingLine*>(self);
    if (std::getenv("RL_DEBUG"))
        std::fprintf(stderr, "car %d session %d over: %d laps, best %.3f; notes saved\n", r->index, summary->session,
                     summary->laps_done, summary->best_lap);
    saveNotes(*r);
    float pace[4] = {};
    if (std::getenv("RL_DEBUG") && fitPace(*r, pace))
        std::fprintf(stderr, "  pace fit: soft %+.2f%%, hard %+.2f%% against the medium\n", pace[1] * 100, pace[3] * 100);
    if (std::getenv("RL_DEBUG"))
        for (int c = RR_TIRE_SOFT; c <= RR_TIRE_HARD; ++c) {
            const RacingLine::CompoundRun& k = knownRun(*r, c);
            if (k.laps)
                std::fprintf(stderr, "  compound %d: %d laps, %.3f s (empty tank), wear %.4f/lap, fuel %.2f l/lap\n", c,
                             k.laps, k.time / k.laps, k.wear / k.laps, k.fuel / k.laps);
        }
}

void* create(const RRTrackInfo* track, const RRCarSpec* car, int index, const char* params, RRRobotConfig* cfg) {
    auto* r = new RacingLine();
    r->index = index;
    r->grip = param(params, "grip", 0.8f);
    r->brakeScale = param(params, "brake", 0.7f);
    r->margin = param(params, "margin", 1.3f);
    r->look = param(params, "look", 1.0f);
    r->yawGain = param(params, "yawgain", 0.06f);
    r->pass = param(params, "pass", 1.0f) != 0.0f;
    r->defend = param(params, "defend", 1.0f) != 0.0f;
    r->wearLimit = param(params, "wear", 0.7f);
    r->usePit = param(params, "pit", 1.0f) != 0.0f && track->pit.has_pit && !cfg->pits_closed;
    r->push = std::max(1.0f, param(params, "push", 1.2f));
    r->learn = param(params, "learn", 1.0f) != 0.0f;
    r->attack = std::clamp(param(params, "attack", 1.0f), 0.5f, 2.0f);
    r->heat = param(params, "heat", 5.0f);
    r->fuelSaveParam = r->fuelSave = std::clamp(param(params, "save", 0.0f), 0.0f, 1.0f);
    r->car = *car;
    r->pit = track->pit;
    r->tp.assign(track->points, track->points + track->num_points);
    if (track->points3) r->tp3.assign(track->points3, track->points3 + track->num_points);
    else r->tp3.assign(track->num_points, RRTrackPoint3{});
    r->L = track->length;
    r->ds = track->length / track->num_points;
    r->adj.assign((size_t)std::ceil(r->L / RacingLine::kBin), 1.0f);
    r->session = cfg->session;
    r->memory = cfg->memory;
    r->memorySize = cfg->memory_size;
    loadNotes(*r);
    planLine(*r);
    planSpeed(*r, car->mass + 0.5f * car->fuel_capacity * car->fuel_density, 1.0f);
    initStrategy(*r, params, cfg);
    planSpeed(*r, car->mass + cfg->initial_fuel * car->fuel_density, 1.0f);
    return r;
}

// ---------------------------------------------------------------- strategy

// Time along the planned line from track distance a to b, s.
float lineTime(const RacingLine& r, float a, float b) {
    float t = 0;
    const int n0 = (int)(a / r.ds), steps = (int)(r.fwd(a, b) / r.ds);
    for (int k = 0; k < steps; ++k) t += r.ds / std::max(5.0f, r.speed[r.wrap(n0 + k)]);
    return t;
}

// Priors before the car has measured itself, the starting tyres and the
// starting fuel (enough for the planned first stint).
// Practice: a stint on each compound to measure its wear and pace, then back
// to the first to compare it on the track as learnt by then. Fuel for each
// stint goes in at the stop before it.
void planPractice(RacingLine& r, RRRobotConfig* cfg, int laps) {
    strat::Model& m = r.model;
    const int first = cfg->starting_compound_set ? strat::okCompound(cfg->tire_compound) : RR_TIRE_MEDIUM;
    int order[4] = {first, 0, 0, first};
    int k = 1;
    for (int c : {RR_TIRE_SOFT, RR_TIRE_MEDIUM, RR_TIRE_HARD})
        if (c != first) order[k++] = c;
    // Later stints are 4 laps (out lap, two timed laps, in lap); the first gets the rest.
    // With 15 laps or more the first compound comes back at the end, so the fit can
    // tell the track getting quicker under us (learning it) from the compounds.
    r.progStints = laps >= 15 ? 4 : 3;
    for (int i = 0; i < r.progStints; ++i) {
        r.progTires[i] = order[i];
        r.progEnd[i] = laps - 4 * (r.progStints - 1 - i);  // the lap each stint stops at the end of
    }
    r.progAt = 0;
    cfg->tire_compound = first;
    if (!cfg->starting_fuel_set)
        cfg->initial_fuel = std::min(r.car.fuel_capacity, (r.progEnd[0] + 1.5f) * m.fuelPerLap * 1.05f);
}

// Practice: box at the end of each stint for the next compound and its fuel.
void practiceStops(RacingLine& r, const RRSensors* in) {
    if (r.mode != RACE || r.progAt >= r.progStints - 1 || in->lap < r.progEnd[r.progAt]) return;
    const float toEntry = r.fwd(in->dist_from_start, r.pit.entry_s);
    if (toEntry > 400.0f || toEntry < 30.0f) return;
    strat::Model& m = r.model;
    ++r.progAt;
    const int stintLaps = r.progEnd[r.progAt] - in->lap;
    RRControl o{};
    o.pit_request = 1;
    o.pit_tires = r.progTires[r.progAt];
    o.pit_fuel = std::max(0.0f, (stintLaps + 1.5f) * m.fuelPerLap * 1.05f - in->fuel);
    o.pit_repair = in->damage > 500;
    r.order = o;
    r.mode = PIT_IN;
    r.lastService = m.serviceTime(o.pit_fuel, true);
    static const char* names[] = {"", "soft", "medium", "hard"};
    std::snprintf(r.plan, sizeof r.plan, "practice: %s run", names[strat::okCompound(o.pit_tires)]);
}

void initStrategy(RacingLine& r, const char* params, RRRobotConfig* cfg) {
    strat::Model& m = r.model;
    const float wearRate = cfg->wear_rate > 0 ? cfg->wear_rate : 1.0f;
    const float fuelRate = cfg->fuel_rate > 0 ? cfg->fuel_rate : 1.0f;
    m.lapRef = lineTime(r, 0, r.L - r.ds) * 1.03f;
    m.fuelPerLap = 0.72e-3f * r.L * r.car.fuel_use_scale * fuelRate;  // ~0.72 l per km
    m.tyres = strat::Compounds::forHeat(r.heat);
    m.wearPerLapMed = 0.019f * r.car.tire_wear_scale * wearRate * (1 + 0.025f * std::max(0.0f, r.heat - 5));
    m.wearLimit = r.wearLimit;
    m.serviceScale = r.car.pit_service_scale > 0 ? r.car.pit_service_scale : 1.0f;
    m.fuelCap = r.car.fuel_capacity;
    m.fuelDensity = r.car.fuel_density;
    // Pit loss: the lane at the limit and the slower entry and exit, against
    // the line, plus stopping in and leaving the box.
    if (r.usePit) {
        const RRPitInfo& p = r.pit;
        float pitT = r.fwd(p.lane_start_s, p.lane_end_s) / p.speed_limit;
        pitT += (r.fwd(p.entry_s, p.lane_start_s) + r.fwd(p.lane_end_s, p.exit_s)) / (1.6f * p.speed_limit);
        r.spanNormal = lineTime(r, p.entry_s, p.exit_s);
        m.pitLoss = std::max(5.0f, pitT - r.spanNormal) + 4.0f;
    }
    // What practice measured beats the priors.
    if (r.haveNotes) {
        if (r.noteLapRef > 10) { m.lapRef = r.noteLapRef; r.lapRefN = 2; }
        if (r.noteFuel > 0.05f) m.fuelPerLap = r.noteFuel;
        if (r.noteWear > 0.001f) m.wearPerLapMed = r.noteWear;
        if (r.usePit && r.notePitLoss > 3) m.pitLoss = r.notePitLoss;
        if (r.usePit && r.noteSpan > 1) r.spanNormal = r.noteSpan;
        // Each compound's wear and pace against the medium, as practice measured them.
        const RacingLine::CompoundRun& med = r.noteRuns[RR_TIRE_MEDIUM];
        if (med.laps >= 2 && med.wear > 0) {
            m.wearPerLapMed = med.wear / med.laps;
            for (int c : {RR_TIRE_SOFT, RR_TIRE_HARD}) {
                const RacingLine::CompoundRun& k = r.noteRuns[c];
                if (k.laps < 2 || k.wear <= 0) continue;
                m.tyres.wear[c] = std::clamp(k.wear / k.laps / m.wearPerLapMed, 0.25f, 5.0f);
                if (!r.notePaceSet)
                    m.tyres.pace[c] = 0.5f * (m.tyres.pace[c] + std::clamp((k.time / k.laps) / (med.time / med.laps) - 1.0f, -0.08f, 0.08f));
            }
        }
        // Measured on fresh tyres over two laps; a race stint runs them hotter and longer,
        // so the measurement only moves the prior half way.
        if (r.notePaceSet)
            for (int c : {RR_TIRE_SOFT, RR_TIRE_HARD}) m.tyres.pace[c] = 0.5f * (m.tyres.pace[c] + r.notePace[c]);
    }
    const int laps = cfg->race_laps > 0 ? cfg->race_laps : 10;
    if (r.session == RR_SESSION_PRACTICE && r.usePit && laps >= 10) {
        planPractice(r, cfg, laps);
        return;
    }
    const int tires = (int)param(params, "tires", 0.0f);
    int start = tires >= RR_TIRE_SOFT && tires <= RR_TIRE_HARD ? tires : 0;
    if (cfg->starting_compound_set) start = cfg->tire_compound;  // the team's choice: plan around it
    strat::Plan best;
    for (int c = RR_TIRE_SOFT; c <= RR_TIRE_HARD; ++c) {
        if (start && c != start) continue;
        strat::Plan pl = r.usePit ? strat::plan(m, laps - 1, c, 0, m.fuelCap, 1.0f, 1 << c, cfg->two_compound_rule)
                                  : strat::Plan{};
        if (!r.usePit) { pl.valid = true; pl.cost = 0; pl.firstStint = laps - 1; }
        if (pl.valid && (!best.valid || pl.cost < best.cost)) { best = pl; cfg->tire_compound = c; }
    }
    if (start) cfg->tire_compound = start;
    // Qualifying: the compound practice found fastest over a lap.
    if (r.session == RR_SESSION_QUALIFYING && !cfg->starting_compound_set && !start) {
        int fastest = cfg->tire_compound;
        for (int c = RR_TIRE_SOFT; c <= RR_TIRE_HARD; ++c)
            if (m.compoundPace(c) < m.compoundPace(fastest) - 1e-4f) fastest = c;
        cfg->tire_compound = fastest;
    }
    // Fuel: the first stint (the start lap plus firstStint laps) and a lap spare.
    const float need = (best.valid ? best.firstStint + 2.0f : (float)laps + 1.0f) * m.fuelPerLap * 1.04f;
    if (cfg->starting_fuel_set) return;  // the team chose it
    cfg->initial_fuel = std::min(r.car.fuel_capacity, param(params, "fuel", need));
    if (!r.usePit) cfg->initial_fuel = param(params, "fuel", r.car.fuel_capacity);
}

// What the timing screen says about the cars around us, in seconds.
struct Traffic {
    float ahead = 1e9f, behind = -1e9f;  // gap to the nearest car ahead / behind (behind is negative)
    int aheadIdx = -1, behindIdx = -1;
    bool aheadPitted = false;            // ...and whether it stopped since our last look
    bool behindPitted = false;
};

Traffic readTiming(const RacingLine& r, const RRSensors* in) {
    Traffic t;
    for (int k = 0; k < in->num_timing; ++k) {
        const RRTimingEntry& e = in->timing[k];
        if (e.dnf || e.finished || e.gap == 0.0f) continue;
        const bool stopped = e.car_index < RR_MAX_CARS && e.pit_stops > r.prevStops[e.car_index];
        if (e.gap > 0 && e.gap < t.ahead) { t.ahead = e.gap; t.aheadIdx = e.car_index; t.aheadPitted = stopped || e.pit_state != RR_PIT_NONE; }
        if (e.gap < 0 && e.gap > t.behind) { t.behind = e.gap; t.behindIdx = e.car_index; t.behindPitted = stopped || e.pit_state != RR_PIT_NONE; }
    }
    return t;
}

// Would we come out of a stop costing `loss` seconds right behind someone?
bool rejoinBlocked(const RRSensors* in, float loss) {
    for (int k = 0; k < in->num_timing; ++k) {
        const RRTimingEntry& e = in->timing[k];
        if (e.dnf || e.finished || e.gap == 0.0f || e.pit_state != RR_PIT_NONE) continue;
        const float after = e.gap + loss;  // their gap to us once we have stopped
        if (after > 0.0f && after < 1.5f) return true;
    }
    return false;
}

// Once a lap, shortly before the pit entry: update what the car knows about
// itself, re-plan the race and decide whether to stop now.
void strategy(RacingLine& r, const RRSensors* in) {
    if (!r.usePit || r.mode != RACE) return;
    const float s = in->dist_from_start;
    const float toEntry = r.fwd(s, r.pit.entry_s);
    if (toEntry > 400.0f || toEntry < 30.0f) {
        if (toEntry > 400.0f) r.decidedThisLap = false;
        return;
    }
    if (r.decidedThisLap) return;
    r.decidedThisLap = true;
    strat::Model& m = r.model;

    // Measured rates since the last refuel / tyre change replace the priors
    // once there is most of a lap of data.
    const float fuelDist = in->dist_raced - r.fuelDistRef, wearDist = in->dist_raced - r.wearDistRef;
    const float wearNow = std::max(in->tire_wear[0], in->tire_wear[1]);
    if (fuelDist > r.L * 0.8f) m.fuelPerLap = std::max(0.1f, r.fuelRef - in->fuel) / fuelDist * r.L;
    if (wearDist > r.L * 0.8f)
        m.wearPerLapMed = std::max(0.002f, wearNow - r.wearRef) / wearDist * r.L / m.compoundWear(in->tire_compound) /
                          (1 + m.wearGrowth * 0.5f * (wearNow + r.wearRef));
    const float fuelPerM = m.fuelPerLap / r.L, wearPerM = m.wearRate(in->tire_compound, wearNow) / r.L;

    // Distance from the pit entry to the flag.
    const float atEntry = in->dist_raced + toEntry;
    const float toGo = in->race_laps * r.L - atEntry;
    const int lapsLeft = in->race_laps - in->lap;  // complete laps after this one
    const float fuelAtEntry = in->fuel - fuelPerM * toEntry;
    const float wearAtEntry = wearNow + wearPerM * toEntry;
    const float lapFrac = std::clamp(toEntry / r.L, 0.0f, 1.0f);

    // Things that force a stop whatever the plan says.
    const float reserve = 0.1f * m.fuelPerLap;  // litres at the flag, as the plan assumes
    const bool fuelShort = fuelAtEntry - fuelPerM * toGo < reserve &&                    // won't make the flag
                           fuelAtEntry - fuelPerM * r.L < reserve + fuelPerM * 400.0f;  // nor the next pit entry
    const bool tyresGone = wearAtEntry + wearPerM * r.L > m.wearLimit + strat::kWearMargin && wearPerM * toGo > 0.05f;
    // Damage: worth a stop of its own only when the time it costs to the flag
    // beats the stop (the next planned stop repairs it anyway). At full damage
    // (lost downforce, power and grip) a car is roughly 8% slower.
    const float dmgLevel = std::min(1.0f, in->damage / 8000.0f);  // the stock car loses the most at 8000
    const float dmgLossPerLap = 0.08f * m.lapRef * dmgLevel;
    const float repairStop = m.pitLoss + m.serviceScale * (RR_PIT_SERVICE_BASE + RR_PIT_REPAIR_PER_1000 * in->damage / 1000.0f);
    const bool broken = dmgLossPerLap * toGo / r.L > repairStop * 1.3f;
    const bool must = toGo > 0.3f * r.L && (fuelShort || tyresGone || broken);

    // The plan.
    strat::Plan pl = lapsLeft > 0 ? strat::plan(m, lapsLeft, in->tire_compound, wearAtEntry, fuelAtEntry, lapFrac,
                                                in->compounds_used, in->two_compound_rule != 0)
                                  : strat::Plan{};
    r.planNow = pl;
    r.planLap = in->lap;
    if (std::getenv("RL_DEBUG"))
        std::fprintf(stderr, "car %d t=%.0f lap %d left %d: ref %.2f fuel/lap %.2f wear/lap(med) %.4f loss %.1f | stops %d first +%d [%d..%d] %d/%d/%d laps %d/%d/%d fuel %.1f cost %.1f\n",
                     r.index, in->time, in->lap, lapsLeft, m.lapRef, m.fuelPerLap, m.wearPerLapMed, m.pitLoss, pl.stops, pl.firstStint,
                     pl.windowLo, pl.windowHi, pl.compounds[0], pl.compounds[1], pl.compounds[2], pl.stintLaps[0],
                     pl.stintLaps[1], pl.stintLaps[2], pl.nextFuel, pl.cost);

    // Racecraft around the plan, inside its window.
    const Traffic tr = readTiming(r, in);
    const float stopLoss = m.pitLoss + m.serviceTime(pl.valid ? pl.nextFuel : 10.0f, true);
    const bool planned = pl.valid && pl.stops > 0 && pl.firstStint == 0;
    bool now = must || planned;
    const char* why = planned ? "plan" : fuelShort ? "fuel" : tyresGone ? "tyres" : "damage";
    if (!must && pl.valid && pl.stops > 0 && pl.windowLo == 0 && pl.firstStint <= 3) {  // racecraft moves a stop by a few laps at most
        const bool canWait = pl.windowHi > 0;
        if (tr.behindPitted && tr.behind > -3.0f) { now = true; why = "cover"; }               // they undercut us: answer now
        else if (tr.aheadPitted && tr.ahead < 2.5f && canWait && wearNow < 0.5f) { now = false; why = "overcut"; }
        else if (tr.ahead < 1.5f && !tr.aheadPitted) { now = true; why = "undercut"; }         // fresh tyres beat the car ahead
        if (now && canWait && rejoinBlocked(in, stopLoss) && std::strcmp(why, "cover") != 0) { now = false; why = "traffic"; }
    }
    for (int k = 0; k < in->num_timing; ++k)
        if (in->timing[k].car_index < RR_MAX_CARS) r.prevStops[in->timing[k].car_index] = in->timing[k].pit_stops;

    // A stop for a splash of fuel costs ~20 s; lifting and coasting saves ~10%
    // of the fuel for ~0.2 s a lap. When fuel is all that is missing to the
    // flag (the tyres last, the tyre rule is met, no damage to repair), save it.
    r.fuelSave = r.fuelSaveParam;
    {
        const float need = fuelPerM * toGo + reserve;
        const float shortBy = need - fuelAtEntry;
        float w = wearAtEntry, f = fuelAtEntry;
        m.stint(lapsLeft, in->tire_compound, w, f);
        const bool tyresLast = w <= m.wearLimit + strat::kWearMargin;
        const bool ruleMet = !in->two_compound_rule || strat::popcount(in->compounds_used) >= 2;
        if (shortBy > 0 && shortBy < 0.13f * need && tyresLast && ruleMet && !broken) {
            r.fuelSave = std::max(r.fuelSaveParam, std::clamp(shortBy / need / 0.33f, 0.1f, 0.4f));
            if (now) {
                now = false;
                why = "save";
                if (std::getenv("RL_DEBUG"))
                    std::fprintf(stderr, "car %d lap %d: saving fuel instead of stopping (%.1f l short, save %.2f)\n",
                                 r.index, in->lap, shortBy, r.fuelSave);
            }
        }
    }
    if (!now) return;

    // Stopping earlier than planned: what to fit and how much fuel for a stop now.
    if (pl.valid && pl.firstStint != 0)
        pl = strat::plan(m, lapsLeft, in->tire_compound, wearAtEntry, fuelAtEntry, lapFrac, in->compounds_used,
                         in->two_compound_rule != 0, true);
    RRControl o{};
    o.pit_request = 1;
    if (pl.valid && pl.stops > 0) {
        o.pit_fuel = pl.nextFuel;
        o.pit_tires = pl.compounds[0];
    } else {
        // Out of plans (a forced stop on the last laps): fuel to the flag, the softest tyres that last.
        o.pit_fuel = std::max(0.0f, fuelPerM * toGo * 1.08f + reserve + 1.0f - fuelAtEntry);
        // The softest tyres that last, a different compound if the rule still wants one.
        const bool needOther = in->two_compound_rule && strat::popcount(in->compounds_used) < 2;
        o.pit_tires = 0;
        for (int c : {RR_TIRE_SOFT, RR_TIRE_MEDIUM, RR_TIRE_HARD}) {
            if (needOther && (in->compounds_used & (1 << c))) continue;
            if (m.wearRate(c, 0) * toGo / r.L < r.wearLimit || c == RR_TIRE_HARD) { o.pit_tires = c; break; }
        }
        if (!o.pit_tires) o.pit_tires = RR_TIRE_MEDIUM;
    }
    // A repair adds only RR_PIT_REPAIR_PER_1000 s per 1000 damage to a stop that is
    // happening anyway: fix whatever would cost more than that by the flag, so the
    // car does not have to come back in a few laps later just for repairs.
    const float repairExtra = m.serviceScale * RR_PIT_REPAIR_PER_1000 * in->damage / 1000.0f;
    o.pit_repair = in->damage > 0 && dmgLossPerLap * toGo / r.L > repairExtra;
    r.order = o;
    r.mode = PIT_IN;
    r.lastService = m.serviceTime(o.pit_fuel, o.pit_tires != 0);
    if (std::getenv("RL_DEBUG")) std::fprintf(stderr, "car %d BOX lap %d: %s\n", r.index, in->lap, why);
    std::snprintf(r.plan, sizeof r.plan, "box (%s): %.0f l%s%s", why, o.pit_fuel,
                  o.pit_tires == RR_TIRE_SOFT     ? " +soft"
                  : o.pit_tires == RR_TIRE_MEDIUM ? " +medium"
                  : o.pit_tires == RR_TIRE_HARD   ? " +hard"
                                                  : "",
                  o.pit_repair ? " +repair" : "");
}

// Learns the reference lap and the real pit loss from the timing of each lap.
void learnTiming(RacingLine& r, const RRSensors* in, bool newLap) {
    if (!r.usePit) return;
    strat::Model& m = r.model;
    const RRPitInfo& p = r.pit;
    const float s = in->dist_from_start;
    // Crossing the pit entry and exit: time the span with and without a stop.
    if (r.entryTime < 0 && r.fwd(p.entry_s, s) < 20.0f && in->dist_raced > 0) {
        r.entryTime = in->time;
        r.entryPitting = r.mode == PIT_IN;
    } else if (r.entryTime >= 0 && r.fwd(p.exit_s, s) < 20.0f) {
        const float span = (float)(in->time - r.entryTime);
        if (!r.entryPitting && span < r.spanNormal * 1.15f) r.spanNormal += (span - r.spanNormal) * 0.3f;
        else if (r.entryPitting && in->pit_stops > 0) {
            const float loss = span - r.spanNormal - r.lastService;
            if (loss > 3.0f && loss < 60.0f) m.pitLoss += (loss - m.pitLoss) * 0.5f;
        }
        r.entryTime = -1;
    } else if (r.entryTime >= 0 && in->time - r.entryTime > 200.0) {
        r.entryTime = -1;
    }
    if (newLap) {
        // A clean lap: what it says about the compound it was on.
        const float wearNow = std::max(in->tire_wear[0], in->tire_wear[1]);
        if (r.cleanLap && in->lap > 2 && in->last_lap_time > 0 && r.lapStartFuel > in->fuel) {
            auto& k = r.runs[strat::okCompound(in->tire_compound)];
            const float fuelMid = 0.5f * (r.lapStartFuel + in->fuel);
            k.laps += 1;
            k.time += in->last_lap_time - m.fuelSecPerKg * r.car.fuel_density * fuelMid;
            k.wear += std::max(0.0f, wearNow - r.lapStartWear) / (1 + m.wearGrowth * 0.5f * (wearNow + r.lapStartWear));
            k.fuel += r.lapStartFuel - in->fuel;
            r.lapLog.push_back({in->lap - 1, strat::okCompound(in->tire_compound),
                                in->last_lap_time - m.fuelSecPerKg * r.car.fuel_density * fuelMid});
        }
        r.lapStartFuel = in->fuel;
        r.lapStartWear = wearNow;
        // A clean racing lap updates the reference (taken back to mediums).
        if (r.cleanLap && in->lap > 2 && in->last_lap_time > 0 && in->last_lap_time < in->best_lap_time * 1.04f) {
            const float ref = in->last_lap_time / (1 + m.compoundPace(in->tire_compound));
            r.lapRefN += 1;
            m.lapRef += (ref - m.lapRef) / std::min(r.lapRefN, 5.0f);
        }
        r.cleanLap = true;
    }
    if (r.mode != RACE || in->pit_state != RR_PIT_NONE) r.cleanLap = false;
}

// Lateral offset (from the centreline) of the pit path at track sample i.
float pitOffset(const RacingLine& r, int i, float boxS, bool serviced) {
    const RRPitInfo& p = r.pit;
    const float s = r.tp[r.wrap(i)].s;
    const float line = r.offset[r.wrap(i)];
    const float lane = p.lane_offset, box = p.box_offset;
    const float blend = 12.0f;  // metres to move between the fast lane and the box
    if (r.inSpan(s, p.entry_s, p.lane_start_s)) {
        float len = std::max(10.0f, r.fwd(p.entry_s, p.lane_start_s) - 10.0f);
        float u = smooth01(r.fwd(p.entry_s, s) / len);
        return line + (lane - line) * u;
    }
    if (r.inSpan(s, p.lane_start_s, p.lane_end_s)) {
        float d = r.signedDs(boxS, s);  // + = past the box
        if (!serviced) {
            if (d >= 0) return box;
            if (d > -blend) return lane + (box - lane) * smooth01((d + blend) / blend);
            return lane;
        }
        if (d >= 0 && d < blend) return box + (lane - box) * smooth01(d / blend);
        if (d < 0 && d > -blend) return box;
        return lane;
    }
    if (r.inSpan(s, p.lane_end_s, p.exit_s)) {
        float start = 5.0f;  // stay clear of the end of the pit wall
        float len = std::max(10.0f, r.fwd(p.lane_end_s, p.exit_s) - start);
        float u = smooth01((r.fwd(p.lane_end_s, s) - start) / len);
        return lane + (line - lane) * u;
    }
    return line;
}

// ---------------------------------------------------------------- racecraft

// Speed limit for holding a lateral position other than the racing line
// through the next corners: path curvature from the centreline curvature at
// that offset, then what can still be braked for.
float offLineSpeed(const RacingLine& r, int idx, float lat, float mass, float tyreGrip) {
    const float g = 9.81f, mu = r.car.tire_mu * r.grip * tyreGrip, D = r.car.downforce_coeff;
    const float decel = 0.6f * mu * g;
    float best = 1e9f;
    const int steps = (int)(160.0f / r.ds);
    for (int k = 0; k < steps; k += 2) {
        const RRTrackPoint& p = r.tp[r.wrap(idx + k)];
        float c = p.curvature;
        float k2 = c / std::max(0.2f, 1.0f - c * lat);
        const RRTrackPoint3& p3 = r.tp3[r.wrap(idx + k)];
        float vc = cornerSpeed(k2, mu, D, mass, p3, 1e4f);
        if (vc >= 1e4f) continue;
        (void)g;
        best = std::min(best, std::sqrt(vc * vc + 2 * decel * k * r.ds));
    }
    return best;
}

const RROpponent* carAhead(const RRSensors* in, float maxDs) {
    for (int k = 0; k < in->num_nearby; ++k) {
        const RROpponent& o = in->nearby[k];
        if (o.ds > 0 && o.ds < maxDs && o.pit_state == RR_PIT_NONE) return &o;
    }
    return nullptr;
}

// Side (+1 left, -1 right) of the inside of the next real corner within range, 0 if none.
float nextCornerInside(const RacingLine& r, int idx, float range) {
    int steps = (int)(range / r.ds);
    for (int k = 10; k < steps; ++k) {
        float c = r.tp[r.wrap(idx + k)].curvature;
        if (std::fabs(c) > 0.012f) return c > 0 ? 1.0f : -1.0f;
    }
    return 0;
}

// Target lateral shift from the racing line (m) for passing or defending;
// may also lower the target speed.
float racecraft(RacingLine& r, const RRSensors* in, int idx, float v, float* speedCap) {
    const float hw = r.tp[idx].half_width - r.margin;
    const float line = r.offset[idx];
    const float myLat = in->track_pos * r.tp[idx].half_width;
    auto pathLat = [&](float ds) { return r.offset[r.wrap(idx + (int)(ds / r.ds))] + r.passOffset; };

    // Follow: never drive into the back of a car in our path, keep a gap that
    // grows with speed.
    for (int k = 0; k < in->num_nearby; ++k) {
        const RROpponent& o = in->nearby[k];
        if (o.ds <= 0 || o.ds > 15.0f + v || o.pit_state != RR_PIT_NONE) continue;
        // A stopped or crawling car is an obstacle to drive round, not a car to queue
        // behind (rr_hazard_speed slows us past it): following it to 0 jams the field.
        if (o.speed < 5.0f) continue;
        bool inPath = std::fabs(o.lateral - pathLat(o.ds)) < 2.3f || (o.ds < 12.0f && std::fabs(o.lateral - myLat) < 2.3f);
        if (!inPath) continue;
        // Gap that grows with speed; close it gently, and if they are
        // braking hard (or stopped) brake for their speed in time.
        float gap = o.ds - 4.8f, want = (1.5f + 0.07f * v) / r.attack;
        float cap = o.speed + 0.6f * (gap - want);
        if (gap > want) cap = std::min(cap, std::sqrt(o.speed * std::max(0.0f, o.speed) + 2.0f * 9.0f * (gap - want)) + 3.0f);
        *speedCap = std::min(*speedCap, std::max(0.0f, cap));
    }

    float target = line;  // absolute lateral we want
    bool busy = false;

    // Overtaking: go round the nearest car ahead on the side with more room.
    // The side is held once alongside, and re-chosen while still behind (the
    // car ahead may move to defend).
    if (r.pass && !in->blue_flag) {  // no attacking while being lapped
        const RROpponent* a = carAhead(in, (8.0f + 0.15f * v) * r.attack);
        if (a && (v > a->speed + 0.5f || a->ds < 12.0f)) {
            float roomLeft = hw - a->lateral, roomRight = hw + a->lateral;
            bool alongside = a->ds < 7.0f && r.passCar == a->car_index;
            if (!alongside || r.passSide == 0) {
                // Pick the side that costs least through the next corners
                // (usually the inside), among those with room for a car.
                const float mass = r.car.mass + in->fuel * r.car.fuel_density;
                float best = 0, bestV = -1;
                for (float side : {1.0f, -1.0f}) {
                    float room = side > 0 ? roomLeft : roomRight;
                    if (room < 2.8f) continue;
                    float lat = std::clamp(a->lateral + side * 3.4f, -hw, hw);
                    float vs = offLineSpeed(r, idx, lat, mass, in->tire_grip);
                    if (side == r.passSide && r.passCar == a->car_index) vs *= 1.03f;  // hysteresis
                    if (vs > bestV) { bestV = vs; best = side; }
                }
                r.passSide = best;
            }
            r.passCar = a->car_index;
            float t = std::clamp(a->lateral + r.passSide * 3.4f, -hw, hw);
            if (r.passSide != 0 && std::fabs(t - a->lateral) >= 2.8f) {
                target = t;
                busy = true;
            }
        } else {
            r.passCar = -1;
            r.passSide = 0;
        }
    }

    // Defending: a car within 25 m behind, on the same lap, and closing.
    if (!busy && r.defend && in->time >= r.defendCooldown) {
        const RROpponent* threat = nullptr;
        for (int k = 0; k < in->num_nearby; ++k) {
            const RROpponent& o = in->nearby[k];
            if (o.ds < 0 && o.ds > -25.0f && o.laps_ahead == 0 && o.pit_state == RR_PIT_NONE &&
                (o.speed > v + 0.5f || o.ds > -10.0f)) {
                threat = &o;
                break;
            }
        }
        if (r.defendSide == 0 && threat && threat->ds < -7.0f) {
            float inside = nextCornerInside(r, idx, 150.0f);
            // One move, and not across a car that is already on that side.
            if (inside != 0 && (threat->lateral - myLat) * inside < 1.0f) {
                r.defendSide = inside;
                r.defendUntil = in->time + 6.0;
            }
        }
        if (r.defendSide != 0) {
            if (!threat || in->time > r.defendUntil) {
                r.defendSide = 0;
                r.defendCooldown = in->time + 3.0;
            } else {
                target = r.defendSide * (hw - 1.0f);
            }
        }
    }

    // Blue flag: a car is lapping us. Keep to one side and lift a little.
    if (in->blue_flag) {
        float aside = target;
        rr_blue_flag_side(in, myLat, hw, &r.blueSide, &aside);
        // Lift on the next straight once it is close: corners stay racing
        // speed, so a train of backmarkers does not hold the leaders up.
        float vMin = 1e9f;
        for (int k = 0; k < (int)(150.0f / r.ds); k += 5) vMin = std::min(vMin, r.speed[r.wrap(idx + k)]);
        if (-in->blue_flag_ds < 30.0f && vMin > 45.0f) *speedCap = std::min(*speedCap, std::max(30.0f, v - 6.0f));
        r.passCar = -1;
        r.passSide = 0;
        target = aside;
        r.defendSide = 0;
    } else {
        r.blueSide = 0;
    }

    // Never squeeze a car that is alongside, or move across one closing from behind.
    float lo = -(hw + r.margin - 1.0f), hi = hw + r.margin - 1.0f;
    rr_side_limits(in, myLat, v, &lo, &hi);
    r.sideLo = lo;
    r.sideHi = hi;
    const float wanted = target;
    target = std::clamp(target, lo, hi);
    // Squeezed out of a pass (another car is where we wanted to go): back out
    // and tuck in behind rather than force it.
    if (busy && std::fabs(target - wanted) > 0.8f && r.passCar >= 0) {
        for (int k = 0; k < in->num_nearby; ++k) {
            const RROpponent& o = in->nearby[k];
            if (o.car_index == r.passCar && o.ds > -2.0f && o.ds < 10.0f)
                *speedCap = std::min(*speedCap, std::max(4.0f, o.speed - 1.5f));  // never to a halt
        }
        r.passSide = 0;
    }
    // Off the line the corners are tighter (or wider): slow for them, with a
    // margin, since there is a car next to us.
    const float myPath = std::clamp(line + r.passOffset, lo, hi);
    if (std::fabs(target - line) > 0.5f || std::fabs(myPath - line) > 0.5f) {
        const float mass = r.car.mass + in->fuel * r.car.fuel_density;
        float lat = std::fabs(target - line) > std::fabs(myPath - line) ? target : myPath;
        *speedCap = std::min(*speedCap, 0.96f * offLineSpeed(r, idx, lat, mass, in->tire_grip));
    }
    return target - line;
}

// ---------------------------------------------------------------- the limit

// Learns how much grip each 20 m of track really has. Only laps in clean air
// count: traffic and racecraft moves put the car off its line.
void learnLimit(RacingLine& r, const RRSensors* in, int idx, bool busy) {
    if (!r.learn) return;
    const int b = r.binOf(idx), nb = (int)r.adj.size();
    if (b != r.bin) {
        if (r.bin >= 0 && r.binClean) {
            if (r.binLost || r.binUse > 1.12f) {
                // Over the limit: slower here, and brake earlier for it.
                const float cut = r.binLost ? 0.05f : 0.025f;
                for (int k = 0; k < 3; ++k) {
                    float& a = r.adj[(r.bin - k + nb) % nb];
                    a = std::max(0.8f, a - cut * (1.0f - 0.3f * k));
                }
                r.adjDirty = true;
            } else if (r.binUse > 0.4f && r.binUse < 0.9f) {
                // Working the tyres but well inside their grip: a little faster next time.
                // Practice is for finding the limit: bigger steps, a little further.
                const bool practice = r.session == RR_SESSION_PRACTICE;
                float& a = r.adj[r.bin];
                const float na = std::min(r.push + (practice ? 0.05f : 0.0f), a + (practice ? 0.02f : 0.01f));
                if (na != a) { a = na; r.adjDirty = true; }
            }
        }
        r.bin = b;
        r.binUse = 0;
        r.binClean = true;
        r.binLost = false;
    }
    // The rear under power is traction control's business, not the corner speed's.
    float use = in->grip_use[0];
    if (r.lastAccel < 0.15f) use = std::max(use, in->grip_use[1]);
    r.binUse = std::max(r.binUse, use);
    if (!in->on_track || std::fabs(in->angle) > 0.5f) r.binLost = true;
    bool traffic = false;
    for (int k = 0; k < in->num_nearby; ++k)
        if (in->nearby[k].ds > -10.0f && in->nearby[k].ds < 40.0f) traffic = true;
    if (busy || traffic || in->pit_state != RR_PIT_NONE || in->speed_x < 15.0f || in->dist_raced < 0) r.binClean = false;
}

// ---------------------------------------------------------------- driving

void drive(void* self, const RRSensors* in, RRControl* out) {
    auto* r = static_cast<RacingLine*>(self);
    const float v = std::max(0.0f, in->speed_x);
    const int idx = in->track_index;
    const float s = in->dist_from_start;

    if (in->time < 0.05) {
        r->startLat = in->track_pos * r->tp[idx].half_width;
        r->startDist = in->dist_raced;
        r->fuelRef = in->fuel;
        r->fuelDistRef = r->wearDistRef = std::max(0.0f, in->dist_raced);
    }
    // A finished service resets the strategy's references.
    if (in->pit_stops != r->stopsSeen) {
        r->stopsSeen = in->pit_stops;
        r->fuelRef = in->fuel;
        r->fuelDistRef = in->dist_raced;
        if (r->order.pit_tires) {
            r->wearRef = 0;
            r->wearDistRef = in->dist_raced;
        }
        r->mode = PIT_OUT;
    }

    if (in->pit_state == RR_PIT_SERVICE) {
        out->brake = 1;
        std::snprintf(out->status, sizeof out->status, "in the box  %.1f s", in->service_time_left);
        return;
    }
    learnLimit(*r, in, idx, r->mode != RACE || r->passCar >= 0 || r->defendSide != 0 || std::fabs(r->passOffset) > 0.5f);
    // Stuck or turned round (outside the pit lane, also on the way in or out):
    // get going again; a stop we were heading for is given up and re-planned.
    if (in->pit_state != RR_PIT_SERVICE && rr_recover(&r->recovery, in, out, r->car.max_steer)) {
        r->binLost = true;
        // Spun in the pit lane: straighten up and carry on with the stop.
        // Outside it, give the stop up and plan again.
        if (r->mode != RACE && in->pit_state == RR_PIT_NONE) {
            if (std::getenv("RL_DEBUG")) std::fprintf(stderr, "car %d stop abandoned (recovering) lap %d\n", r->index, in->lap);
            r->mode = RACE;
            r->plan[0] = 0;
            r->decidedThisLap = false;
        }
        r->cleanLap = false;
        std::snprintf(out->status, sizeof out->status, "recovering");
        return;
    }

    // Re-plan the speed profile for the fuel load and tyres once a lap, and at
    // once after a hit that cost downforce.
    const float mass = r->car.mass + in->fuel * r->car.fuel_density;
    const float dmg = std::min(1.0f, in->damage / 8000.0f);
    if (std::fabs(dmg - r->damage) > 0.03f) {
        r->damage = dmg;
        planSpeed(*r, mass, in->tire_grip);
    }
    learnTiming(*r, in, in->lap != r->plannedLap);
    if (in->lap != r->plannedLap) {
        r->plannedLap = in->lap;
        if (r->adjDirty || std::fabs(mass - r->plannedMass) > 3.0f || std::fabs(in->tire_grip - r->plannedGrip) > 0.003f) {
            planSpeed(*r, mass, in->tire_grip);
            r->adjDirty = false;
        }
    }

    if (r->progStints > 0) practiceStops(*r, in);
    else strategy(*r, in);
    if (r->mode == PIT_OUT && in->pit_state == RR_PIT_NONE && !r->inSpan(s, r->pit.entry_s, r->pit.exit_s)) {
        r->mode = RACE;
        r->plan[0] = 0;
    }
    // Overshot the box: give up on this stop.
    if (r->mode == PIT_IN && in->pit_state == RR_PIT_LANE && r->signedDs(in->pit_box_s, s) > 2.5f) {
        r->mode = PIT_OUT;
        std::snprintf(r->plan, sizeof r->plan, "missed the box");
    }
    const bool pitting = r->mode != RACE;
    const bool serviced = r->mode == PIT_OUT;

    float speedCap = 1e9f;
    float want = 0;
    if (!pitting) {
        want = racecraft(*r, in, idx, v, &speedCap);
    } else {
        r->passCar = -1;
        r->defendSide = 0;
    }
    {
        // Smooth, rate-limited lateral moves (at most 2 m/s sideways).
        const float dt = in->dt > 0 ? in->dt : 0.02f;
        float step = (want - r->passOffset) * 0.08f;
        r->passOffset += std::clamp(step, -2.0f * dt, 2.0f * dt);
    }

    // The path itself (not just the racecraft target) keeps clear of cars
    // alongside: the racing line moves across the track into corners.
    if (pitting) {
        r->sideLo = -1e9f;
        r->sideHi = 1e9f;
    }
    // Off the grid: keep to our side of the track and ease onto the line over
    // the first few hundred metres instead of diving across the field.
    const float launch = smooth01((in->dist_raced - r->startDist) / 400.0f);
    auto targetOffset = [&](int i) {
        if (pitting) return pitOffset(*r, i, in->pit_box_s, serviced);
        float line = r->offset[r->wrap(i)];
        if (launch < 1.0f) line = r->startLat + (line - r->startLat) * launch;
        return std::clamp(line + r->passOffset, r->sideLo, r->sideHi);
    };

    // Pure pursuit on the path, plus a cross-track term (pure pursuit alone
    // drifts wide at speed because the car understeers).
    float ld = r->look * (6.0f + 0.22f * v);
    int ahead = (int)(ld / r->ds);
    int ti = r->wrap(idx + ahead);
    P2 tgt = r->pointAt(ti, targetOffset(ti));
    float dx = tgt.x - in->x, dy = tgt.y - in->y;
    float cy = std::cos(in->yaw), sy = std::sin(in->yaw);
    float lx = cy * dx + sy * dy, ly = -sy * dx + cy * dy;
    float alpha = std::atan2(ly, lx);
    float dist = std::sqrt(lx * lx + ly * ly);
    float delta = std::atan(2.0f * r->car.wheelbase * std::sin(alpha) / std::max(dist, 1.0f));
    float lateral = in->track_pos * r->tp[idx].half_width;
    float crossErr = targetOffset(idx) - lateral;  // + = path is to our left
    delta += std::atan(1.5f * crossErr / (v + 5.0f));
    // Yaw-rate feedback against the rate the line asks for: damps the weave
    // that pure pursuit plus the cross-track term can build up at speed.
    delta -= r->yawGain * (in->yaw_rate - v * r->kappaSigned[r->wrap(idx + 2)]);
    out->steer = std::clamp(delta / r->car.max_steer, -1.0f, 1.0f);

    // Tyre temperature: the profile assumes tyres in their window. Cold or
    // overheated ones have less grip (axle_grip against tire_grip), and past
    // our heat tolerance we back off on purpose to cool them.
    {
        float f = 1.0f;
        if (in->tire_temp_window[1] > 0) {  // ABI 4 host
            const float grip = std::max(0.5f, in->tire_grip);
            f = std::clamp(std::min(in->axle_grip[0], in->axle_grip[1]) / grip, 0.8f, 1.05f);
            // the hottest single tyre: one cooking tyre goes off before its axle's mean shows it
            float hottest = std::max(in->tire_temp[0], in->tire_temp[1]);
            if (in->tire_temp_wheel[0] > 0)
                for (int w = 0; w < 4; ++w) hottest = std::max(hottest, in->tire_temp_wheel[w] - 3.0f);
            const float over = hottest - (in->tire_temp_window[1] + r->heat);
            if (over > 0) f *= std::max(0.9f, 1.0f - 0.006f * over);
        }
        const float dt = in->dt > 0 ? in->dt : 0.02f;
        r->tyreNow += (f - r->tyreNow) * std::min(1.0f, dt / 1.5f);
    }
    // Brakes: cold discs bite less and hot ones fade (see brake_temp_window).
    // Re-plan the braking points for the grip they have, and lift and coast to
    // cool them when they get close to fading.
    if (in->brake_temp_window[1] > 0) {
        const float lo = in->brake_temp_window[0], hi = in->brake_temp_window[1];
        float coldest = 1e9f, hottest = -1e9f;
        for (int w = 0; w < 4; ++w) {
            coldest = std::min(coldest, in->brake_temp[w]);
            hottest = std::max(hottest, in->brake_temp[w]);
        }
        float f = 1.0f;
        if (coldest < lo) f = 0.75f + 0.25f * std::clamp((coldest - 50.0f) / (lo - 50.0f), 0.0f, 1.0f);
        if (hottest > hi) f = std::min(f, std::max(0.6f, 1.0f - 0.002f * (hottest - hi)));
        const float dt = in->dt > 0 ? in->dt : 0.02f;
        r->brakeNow += (f - r->brakeNow) * std::min(1.0f, dt / 1.0f);
        if (std::fabs(r->brakeNow - r->brakePlanned) > 0.03f && in->time >= r->brakeReplanAt) {
            r->brakePlanned = r->brakeNow;
            planSpeed(*r, mass, in->tire_grip);
            r->brakeReplanAt = in->time + 2.0;
        }
        r->brakesHot = hottest > hi - (r->brakesHot ? 60.0f : 30.0f);
    }
    // Speed control against the profile, looking a little ahead for actuator lag.
    int si = r->wrap(idx + (int)(v * 0.15f / r->ds) + 1);
    float vTarget = std::min(r->speed[si] * std::sqrt(r->tyreNow), speedCap);
    // A crawling, stopped or rejoining car ahead: brake to pass it safely (rr_awareness.h).
    {
        const float hzLat = in->track_pos * r->tp[idx].half_width;
        vTarget = std::min(vTarget, rr_hazard_speed(in, hzLat, hzLat, v, 22.0f * rr_stopping_factor(in)));
    }

    if (pitting) {
        const RRPitInfo& p = r->pit;
        const float vLim = p.speed_limit * 0.93f;
        if (r->mode == PIT_IN) {
            out->pit_request = 1;
            out->pit_fuel = r->order.pit_fuel;
            out->pit_tires = r->order.pit_tires;
            out->pit_repair = r->order.pit_repair;
            // Brake for the limit by the lane start, then for the box.
            float toLane = r->inSpan(s, p.lane_start_s, p.lane_end_s) ? 0.0f : r->fwd(s, p.lane_start_s);
            vTarget = std::min(vTarget, std::sqrt(vLim * vLim + 2 * 7.0f * toLane));
            float toBox = r->signedDs(s, in->pit_box_s);
            if (toBox > -3.0f) vTarget = std::min(vTarget, std::sqrt(2 * 3.0f * std::max(0.0f, toBox - 0.3f)));
            if (toBox < 0.6f && v < 3.0f) vTarget = 0;
        } else if (r->inSpan(s, p.lane_start_s, p.lane_end_s)) {
            vTarget = std::min(vTarget, vLim);
            // Pulling out of the box: let cars already in the lane go by.
            if (v < 3.0f)
                for (int k = 0; k < in->num_nearby; ++k) {
                    const RROpponent& o = in->nearby[k];
                    if (o.ds < 2.0f && o.ds > -30.0f && o.speed > 3.0f && o.pit_state != RR_PIT_NONE) vTarget = 0;
                }
        }
    }

    // Lift and coast to save fuel: off the throttle this far before a braking zone.
    bool coast = false;
    const float coastShare = std::max(r->fuelSave, r->brakesHot ? 0.5f : 0.0f);
    if (coastShare > 0 && !pitting && v > 40.0f) {
        const int ahead = (int)(coastShare * kCoastDist / r->ds);
        for (int k = 4; k <= ahead && !coast; k += 4) coast = r->speed[r->wrap(idx + k)] < v - 8.0f;
    }
    float err = vTarget - v;
    if (vTarget <= 0.01f) out->brake = 1;
    else if (coast && err > -2.0f) out->accel = 0;
    else if (err > 0) out->accel = std::clamp(0.5f + 0.5f * err, 0.0f, 1.0f);
    else out->brake = std::clamp(-0.25f * err, 0.0f, 1.0f);
    // Traction control: cut quickly when the rear is past its grip, restore slowly.
    // Traction, oversteer catches and understeer (see rr_awareness.h).
    rr_grip_guard(in, out, &r->tc, r->car.max_steer);
    r->lastAccel = out->accel;
    if (!in->on_track && !pitting) out->accel = std::min(out->accel, 0.5f);
    // 2013's KERS on a full throttle with the wheels gripping, and the DRS flap whenever it is ours
    // and we are not braking or about to (it closes on the brakes by itself).
    out->kers = (in->kers_deploy_left > 0 && out->accel > 0.9f && in->wheel_spin < 0.05f && !pitting) ? 1.0f : 0.0f;
    out->drs = ((in->drs_state == RR_DRS_AVAILABLE || in->drs_state == RR_DRS_OPEN) && out->brake < 0.02f &&
                out->accel > 0.9f && !coast && in->grip_use[1] < 1.0f)
                   ? 1
                   : 0;

    const char* what = pitting              ? r->plan
                       : r->defendSide != 0 ? "defending"
                       : r->passCar >= 0    ? "passing"
                                            : "line";
    if (pitting) std::snprintf(out->status, sizeof out->status, "%s", what);
    else std::snprintf(out->status, sizeof out->status, "%s  %.0f km/h", what, vTarget * 3.6f);
    // The plan, for the viewer: the window of the next stop and its tyres.
    if (r->planNow.valid && r->planNow.stops > 0 && !pitting) {
        out->pit_window[0] = r->planLap + r->planNow.windowLo;
        out->pit_window[1] = r->planLap + r->planNow.windowHi;
        out->pit_plan_tires = r->planNow.compounds[0];
    }
    out->debug[0] = vTarget;
    out->debug[1] = r->offset[idx];
    out->debug[2] = r->passOffset;
    out->debug[3] = (float)r->mode;
    out->debug[4] = r->defendSide;
    out->debug[5] = std::max(-99.0f, r->sideLo);
    out->debug[6] = std::min(99.0f, r->sideHi);
    out->debug[7] = std::min(999.0f, speedCap);
}

void destroy(void* self) { delete static_cast<RacingLine*>(self); }

int debugPath(void* self, float* xy, int maxPoints) {
    auto* r = static_cast<RacingLine*>(self);
    int step = std::max(1, (int)std::lround(2.0f / r->ds));
    int count = 0;
    for (int i = 0; i <= r->n() && count < maxPoints; i += step, ++count) {
        const P2& p = r->line[r->wrap(i)];
        xy[2 * count] = p.x;
        xy[2 * count + 1] = p.y;
    }
    return count;
}

const RRRobotApi kApi = {RR_ABI_VERSION, RL_NAME, "Raylib Racers examples", create, drive, destroy, debugPath, sessionEnd};

}  // namespace

extern "C" RR_EXPORT const RRRobotApi* rr_robot_entry(void) { return &kApi; }
