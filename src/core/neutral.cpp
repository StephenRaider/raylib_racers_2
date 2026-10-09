// Incidents, yellow flags and the virtual safety car (ABI 14).
//
// A car that crashes hard, stops on the track or sits across it is held still by the host so it cannot
// drive back into traffic, and released once the track behind it is clear. While any car is held the
// track around it is yellow and the whole field runs under the virtual safety car (a speed cap, no
// overtaking). All of it is a function of the simulation state: no randomness.
#include <algorithm>
#include <cmath>
#include <cstdio>

#include "race.hpp"

namespace rr {

namespace {
const char* kPassWhy = "overtaking under yellow flag or virtual safety car:";
}

void Race::logIncident(const Car& c, const char* kind) {
    incidents_.push_back({time_, (int)(&c - &cars_[0]), c.currentLap(cfg_.laps), c.trackS, kind});
}

// Nobody is about to run into a car standing at c.trackS: no car within 40 m either side, and every
// car behind it more than 7 s away at its current closing speed (10 s if the car faces the wrong way and
// needs room to turn round).
bool Race::trackClearBehind(const Car& c) const {
    const float ang = wrapAngle(c.state.yaw - std::atan2(track_.dirAt(c.trackS).y, track_.dirAt(c.trackS).x));
    const float need = std::fabs(ang) > 1.2f ? 10.0f : 7.0f;
    for (const Car& o : cars_) {
        if (&o == &c || o.dnf || o.finished || o.parked || o.held || o.pitState != RR_PIT_NONE || o.distRaced < 0) continue;
        const float behind = wrapDs(c.trackS - o.trackS);  // + = o is behind c
        const bool slow = std::fabs(o.state.vx) < 5.0f;     // a crawling car is no danger from further than a car length
        if (std::fabs(behind) < (slow ? 8.0f : 40.0f)) return false;
        if (slow) continue;
        if (behind > 0 && behind < 400.0f) {
            const float closing = std::max(1.0f, o.state.vx - std::max(0.0f, c.state.vx));
            if (behind / closing < need) return false;
        }
    }
    return true;
}

// The speed the host holds a car to: the lowest of the yellow zone, the VSC and the rejoin limits.
float Race::neutralCap(const Car& c) const {
    float cap = 0;
    auto lower = [&](float v) { cap = cap > 0 ? std::min(cap, v) : v; };
    if (c.flagState == RR_FLAG_YELLOW) lower(RR_YELLOW_SPEED);
    if (c.flagState == RR_FLAG_DOUBLE_YELLOW) lower(RR_DYELLOW_SPEED);
    if (vscState_ == RR_VSC_ACTIVE) lower(RR_VSC_SPEED);
    if (time_ - c.releasedAt < RR_REJOIN_WATCH) lower(RR_REJOIN_SPEED);
    if (cap > 0) {
        // No passing under a flag: a car just behind another may not run faster than it (and closes up
        // to about 6 m). A stopped car beside the track does not count: the lateral gap must be small.
        for (const Car& o : cars_) {
            if (&o == &c || o.dnf || o.finished || o.pitState != RR_PIT_NONE || o.distRaced < 0) continue;
            const float gap = wrapDs(o.trackS - c.trackS) - 5.5f;
            if (gap < -3.0f || gap > 30.0f || std::fabs(o.lateral - c.lateral) > 3.0f) continue;
            lower(std::max(0.0f, o.state.vx) * 0.97f + 0.5f * (gap - 6.0f));
        }
    }
    return cap;
}

void Race::updateNeutral() {
    flagged_.clear();
    if (!cfg_.neutral || cfg_.session != RR_SESSION_RACE || over_) {
        for (Car& c : cars_) {
            c.flagState = RR_FLAG_GREEN;
            c.speedCap = 0;
            c.incidentDs = -1;
            c.prevPosition = c.position;
        }
        return;
    }
    const float tick = cfg_.dt * (float)robotPeriod_;
    const float L = track_.length();
    const bool racing = leaderFinish_ < 0;

    // 1. Overtaking under a flag: a car that moved ahead of one that was ahead of it at the last tick.
    //    The flags and the VSC of the last tick count, so the same moment cannot be both.
    for (Car& x : cars_) {
        if (x.dnf || x.finished || x.held || x.pitState != RR_PIT_NONE || x.distRaced < 0 || x.abi < 14) continue;
        if (x.position >= x.prevPosition || x.prevPosition == 0) continue;
        if (x.capSince < 0 || time_ - x.capSince < RR_NEUTRAL_SETTLE) continue;  // the field is still slowing down
        const bool xFlag = vscState_ == RR_VSC_ACTIVE || x.flagState != RR_FLAG_GREEN;
        if (!xFlag) continue;
        for (const Car& y : cars_) {
            if (&y == &x || y.dnf || y.finished || y.held || y.pitState != RR_PIT_NONE || y.distRaced < 0) continue;
            if (time_ - y.releasedAt < RR_REJOIN_WATCH) continue;  // a car crawling back on is not "passed"
            if (!(y.prevPosition < x.prevPosition && y.position > x.position)) continue;
            if (wrapDs(x.trackS - y.trackS) < 5.0f) continue;  // side by side is not a pass: a car length clear
            if (vscState_ != RR_VSC_ACTIVE && y.flagState == RR_FLAG_GREEN) continue;
            if (time_ - x.lastNeutralPen < 5.0) break;
            x.lastNeutralPen = time_;
            char why[160];
            std::snprintf(why, sizeof why, "%s %s (gap %.0f m, laps %.2f vs %.2f)", kPassWhy, y.name.c_str(),
                          (double)wrapDs(x.trackS - y.trackS), x.distRaced / L, y.distRaced / L);
            givePenalty(x, RR_PEN_NEUTRAL_PASS, why, true);
            break;
        }
    }

    for (Car& c : cars_) {
        c.flagState = RR_FLAG_GREEN;
        c.speedCap = 0;
        c.incidentDs = -1;
    }

    // 2. Detect incidents; hold the cars involved.
    for (Car& c : cars_) {
        const bool live = !c.dnf && !c.finished && !c.parked && c.pitState == RR_PIT_NONE && !c.pitZone &&
                          c.distRaced >= 0 && !c.held;
        const float speed = std::sqrt(c.state.vx * c.state.vx + c.state.vy * c.state.vy);
        const Vec2 d = track_.dirAt(c.trackS);
        const float ang = std::fabs(wrapAngle(c.state.yaw - std::atan2(d.y, d.x)));
        // A released car gets a few seconds to get going before it counts as stuck again.
        const bool settling = time_ - c.releasedAt < RR_REJOIN_GRACE;
        c.stoppedT = live && !settling && speed < 2.0f && time_ > 6.0 ? c.stoppedT + tick : 0.0f;
        c.acrossT = live && !settling && ang > RR_INCIDENT_ANGLE && speed < RR_INCIDENT_MAX_SPEED ? c.acrossT + tick : 0.0f;
        if (!live) continue;
        const char* kind = nullptr;
        if (time_ - c.hardHit < 3.0 && speed < 8.0f) kind = "crash";
        else if (c.stoppedT >= RR_INCIDENT_STOP_TIME) kind = "stopped";
        else if (c.acrossT >= RR_INCIDENT_WRONG_TIME) kind = "spin";
        if (!kind) continue;
        ++c.holdCount;
        c.stoppedT = c.acrossT = 0;
        c.hardHit = -1e9;
        if (time_ - c.releasedAt < RR_REJOIN_WATCH || c.holdCount >= 3) {
            logIncident(c, "removed");
            retire(c, "removed after incident");
            continue;
        }
        c.held = true;
        c.heldSince = time_;
        logIncident(c, kind);
    }

    // 3. Held cars: remove or release.
    for (Car& c : cars_) {
        if (!c.held) continue;
        if (c.dnf || c.finished) { c.held = false; continue; }
        const float speed = std::sqrt(c.state.vx * c.state.vx + c.state.vy * c.state.vy);
        if (time_ - c.heldSince > RR_HOLD_MAX_TIME) {
            c.held = false;
            logIncident(c, "removed");
            retire(c, "removed after incident");
        } else if (time_ - c.heldSince > 1.5 && speed < 1.5f && trackClearBehind(c)) {
            c.held = false;
            c.hardHit = -1e9;
            c.releasedAt = time_;
            c.stoppedT = c.acrossT = 0;
            logIncident(c, "released");
        }
    }

    // 4. Yellow flags around the held cars.
    int heldCars = 0;
    for (const Car& c : cars_) {
        if (!c.held) continue;
        ++heldCars;
        const bool onRoad = std::fabs(c.lateral) < c.halfWidth + 1.0f;
        flagged_.push_back({c.trackS, onRoad ? 2 : 1});
    }
    for (Car& c : cars_) {
        if (c.dnf || c.finished) continue;
        for (const auto& f : flagged_) {
            const float ahead = std::fmod(f.first - c.trackS + L, L);  // to the incident
            if (c.held && ahead < 1.0f) continue;
            const bool inZone = ahead <= RR_YELLOW_BEHIND || ahead >= L - RR_YELLOW_AFTER;
            if (ahead < 600.0f && (c.incidentDs < 0 || ahead < c.incidentDs)) c.incidentDs = ahead;
            if (!inZone) continue;
            c.flagState = std::max(c.flagState, f.second == 2 ? RR_FLAG_DOUBLE_YELLOW : RR_FLAG_YELLOW);
        }
    }

    // 5. The virtual safety car.
    const double leaderDist = cars_[order_[0]].distRaced;
    if (racing && heldCars > 0 && vscState_ != RR_VSC_ACTIVE) {
        if (vscState_ == RR_VSC_NONE) {
            VscPeriod p;
            p.start = time_;
            p.startLap = cars_[order_[0]].currentLap(cfg_.laps);
            p.reason = "incident";
            vscPeriods_.push_back(p);
            vscMinDist_ = leaderDist + std::min(L, 3000.0f);
        } else {
            vscMinDist_ = std::max(vscMinDist_, leaderDist + 0.5 * L);  // trouble again while ending: another half lap
        }
        vscState_ = RR_VSC_ACTIVE;
    } else if (vscState_ == RR_VSC_ACTIVE && heldCars == 0 && (leaderDist >= vscMinDist_ || !racing)) {
        vscState_ = RR_VSC_ENDING;
        vscEndingAt_ = time_ + RR_VSC_ENDING_TIME;
    } else if (vscState_ == RR_VSC_ENDING && time_ >= vscEndingAt_) {
        vscState_ = RR_VSC_NONE;
        if (!vscPeriods_.empty()) vscPeriods_.back().end = time_;
    }

    // 6. What each car is held to.
    for (Car& c : cars_) {
        if (c.dnf || c.finished || c.held || c.pitState != RR_PIT_NONE || c.pitZone) continue;
        c.speedCap = neutralCap(c);
    }
    for (Car& c : cars_) {
        if (c.speedCap <= 0) c.capSince = -1;
        else if (c.capSince < 0) c.capSince = time_;
    }
    for (Car& c : cars_) c.prevPosition = c.position;
}

}  // namespace rr
