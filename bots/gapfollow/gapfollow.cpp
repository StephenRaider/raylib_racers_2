// gapfollow: sensor-only robot that steers towards the longest free range
// finder (the "follow the gap" heuristic used by many SCR bots) and dodges
// slower cars with the opponent sensors. Like the other examples it keeps out
// of the way of cars alongside or closing from behind, gives way under blue
// flags and uses the shared grip guard (rr_awareness.h) for traction and
// oversteer.
//
// params: speed=<scale, 0.9>  gain=<steering gain, 1.3>  dodge=<0|1, 1>
#include <algorithm>
#include <cmath>
#include <cstdio>

#include "../common/rr_awareness.h"
#include "../common/rr_safety.h"
#include "../common/rr_params.h"
#include "../common/rr_recovery.h"
#include "rr/robot_api.h"

namespace {

constexpr float kDeg = 3.14159265f / 180.0f;

struct GapFollow {
    float speedScale, gain, maxSteer;
    bool dodge;
    float angles[RR_NUM_TRACK_SENSORS];
    float dodgeOffset = 0;  // smoothed lateral shift, rad of target heading
    float tc = 1;           // traction-control throttle limit
    float steer = 0;        // smoothed steering output
    RRRecovery recovery{};
};

void* create(const RRTrackInfo*, const RRCarSpec* car, int, const char* params, RRRobotConfig* cfg) {
    auto* g = new GapFollow();
    g->speedScale = rr_param(params, "speed", 0.9f);
    g->gain = rr_param(params, "gain", 1.3f);
    g->dodge = rr_param(params, "dodge", 1.0f) != 0.0f;
    g->maxSteer = car->max_steer;
    // Denser beams around the nose than the SCR default.
    const float a[RR_NUM_TRACK_SENSORS] = {-90, -60, -40, -25, -15, -10, -6, -3, -1, 0,
                                           1,   3,   6,   10,  15,  25,  40, 60, 90};
    for (int i = 0; i < RR_NUM_TRACK_SENSORS; ++i) cfg->track_sensor_angles[i] = g->angles[i] = a[i];
    return g;
}

void drive(void* self, const RRSensors* in, RRControl* out) {
    auto* g = static_cast<GapFollow*>(self);
    if (rr_held(in, out)) {
        std::snprintf(out->status, sizeof out->status, "held by the marshals");
        return;
    }
    if (rr_recover(&g->recovery, in, out, g->maxSteer)) {
        std::snprintf(out->status, sizeof out->status, "recovering");
        return;
    }

    if (!in->on_track) {
        // Recovery: head back to the tarmac at a modest angle.
        float want = -std::clamp(in->track_pos, -1.5f, 1.5f) * 0.4f;
        out->steer = std::clamp((want - in->angle) / g->maxSteer, -1.0f, 1.0f);
        out->accel = in->speed_x < 12 ? 0.5f : 0.0f;
        out->brake = in->speed_x > 18 ? 0.3f : 0.0f;
        std::snprintf(out->status, sizeof out->status, "recovering");
        return;
    }

    // Longest beam within +-45 degrees (side beams can see far through a
    // corner but are no direction to drive in), refined with its neighbours.
    int best = -1;
    for (int i = 0; i < RR_NUM_TRACK_SENSORS; ++i) {
        if (std::fabs(g->angles[i]) > 45.0f) continue;
        if (best < 0 || in->track[i] > in->track[best]) best = i;
    }
    float heading = g->angles[best] * kDeg;
    if (best > 0 && best < RR_NUM_TRACK_SENSORS - 1) {
        float l = in->track[best - 1], c = in->track[best], r = in->track[best + 1];
        float den = l - 2 * c + r;
        if (std::fabs(den) > 1e-3f) {
            float off = std::clamp(0.5f * (l - r) / den, -0.5f, 0.5f);
            float step = off > 0 ? g->angles[best + 1] - g->angles[best] : g->angles[best] - g->angles[best - 1];
            heading += off * step * kDeg;
        }
    }
    float freeDist = in->track[best];

    // Keep off the edges: bias away from whichever side is close.
    float edgeBias = 0;
    if (in->track_pos > 0.7f) edgeBias = -(in->track_pos - 0.7f) * 0.5f;
    if (in->track_pos < -0.7f) edgeBias = -(in->track_pos + 0.7f) * 0.5f;

    // Cars alongside or closing from behind: keep out of their way. The
    // track width comes from the side range finders.
    const float hw = std::max(2.0f, 0.5f * (in->track[0] + in->track[RR_NUM_TRACK_SENSORS - 1]) * std::cos(in->angle));
    const float myLat = in->track_pos * hw;
    float lo = -hw + 1.2f, hi = hw - 1.2f;
    rr_side_limits(in, myLat, in->speed_x, &lo, &hi);
    if (myLat > hi - 0.6f) edgeBias -= 0.12f * (myLat - (hi - 0.6f));
    if (myLat < lo + 0.6f) edgeBias -= 0.12f * (myLat - (lo + 0.6f));
    // Blue flag: move to one side and lift a little.
    float aside = myLat;
    const float blue = rr_blue_flag(in, myLat, hw, &aside);
    if (in->blue_flag) edgeBias += std::clamp(0.06f * (aside - myLat), -0.15f, 0.15f);

    // Dodge a car close ahead: pick the side with more room.
    float dodgeTarget = 0;
    float ahead = RR_SENSOR_RANGE;
    for (int s = 16; s <= 19; ++s) ahead = std::min(ahead, in->opponents[s]);  // -20..+20 deg
    if (g->dodge && ahead < 25.0f) {
        float room = in->track_pos;  // + = we are left, more room on the right
        dodgeTarget = (room > 0 ? -1.0f : 1.0f) * 0.08f * (25.0f - ahead) / 25.0f;
    }
    g->dodgeOffset += (dodgeTarget - g->dodgeOffset) * 0.2f;

    // Aim part of the way to the gap (it is far ahead), damp with yaw rate.
    float target = 0.6f * heading + edgeBias + g->dodgeOffset;
    float steer = g->gain * (target - 0.05f * in->yaw_rate) / g->maxSteer;
    g->steer += (std::clamp(steer, -1.0f, 1.0f) - g->steer) * 0.5f;
    out->steer = g->steer;

    // Speed from the free distance (braking-limited), slower when steering hard.
    float d = std::max(0.0f, freeDist - 12.0f);
    float vTarget = (16.0f + std::sqrt(2.0f * 9.0f * rr_stopping_factor(in) * d)) * g->speedScale;
    vTarget *= 1.0f - 0.35f * std::fabs(out->steer);
    vTarget = std::min(vTarget * blue, rr_follow_speed(in, myLat, myLat, in->speed_x, 7.0f));
    vTarget = std::min(vTarget, rr_hazard_speed(in, myLat, myLat, in->speed_x, 14.0f * rr_stopping_factor(in)));
    vTarget = std::min(vTarget, rr_flag_speed(in));

    float err = vTarget - in->speed_x;
    if (err > 0) {
        out->accel = std::clamp(0.4f + 0.2f * err, 0.0f, 1.0f);
    } else {
        out->brake = std::clamp(-0.12f * err, 0.0f, 1.0f);
    }
    rr_grip_guard(in, out, &g->tc, g->maxSteer);

    std::snprintf(out->status, sizeof out->status, "gap %.0f m @ %+.0f deg", freeDist, heading / kDeg);
    out->debug[0] = vTarget;
    out->debug[1] = heading;
}

void destroy(void* self) { delete static_cast<GapFollow*>(self); }

const RRRobotApi kApi = {RR_ABI_VERSION, "gapfollow", "Raylib Racers examples", create, drive, destroy, nullptr, nullptr};

}  // namespace

extern "C" RR_EXPORT const RRRobotApi* rr_robot_entry(void) { return &kApi; }
