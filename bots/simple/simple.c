/*
 * simple: the classic SCR "simple driver", written in plain C.
 *
 * Uses only the SCR sensors: steers to line up with the track axis and stay
 * near the centre, and picks a target speed from the free distance straight
 * ahead. Shares the other examples' manners (rr_awareness.h): no moving across
 * cars alongside or closing from behind, gives way under blue flags, and a
 * grip guard for traction and oversteer.
 *
 * params: speed=<scale, default 1.0>
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "../common/rr_awareness.h"
#include "../common/rr_safety.h"
#include "../common/rr_params.h"
#include "../common/rr_recovery.h"
#include "rr/robot_api.h"

typedef struct {
    float speed_scale;
    float max_steer;
    float tc; /* traction-control throttle limit */
    RRRecovery recovery;
} Simple;

static void* create(const RRTrackInfo* track, const RRCarSpec* car, int car_index, const char* params,
                    RRRobotConfig* config) {
    (void)track; (void)car_index; (void)config;
    Simple* s = (Simple*)calloc(1, sizeof(Simple));
    s->speed_scale = rr_param(params, "speed", 1.0f);
    s->max_steer = car->max_steer;
    s->tc = 1.0f;
    return s;
}

static void drive(void* self, const RRSensors* in, RRControl* out) {
    Simple* s = (Simple*)self;
    if (rr_held(in, out)) {
        snprintf(out->status, sizeof out->status, "held by the marshals");
        return;
    }
    if (rr_recover(&s->recovery, in, out, s->max_steer)) {
        snprintf(out->status, sizeof out->status, "recovering");
        return;
    }

    /* Where to drive: the centreline, unless a car alongside is there or a
     * lapping car needs the room. Half width from the side range finders. */
    float hw = 0.5f * (in->track[0] + in->track[RR_NUM_TRACK_SENSORS - 1]) * cosf(in->angle);
    if (hw < 2.0f) hw = 2.0f;
    const float myLat = in->track_pos * hw;
    float want = 0.0f, lo = -hw + 1.2f, hi = hw - 1.2f;
    const float blue = rr_blue_flag(in, myLat, hw, &want);
    rr_side_limits(in, myLat, in->speed_x, &lo, &hi);
    want = rr_clamp(want, lo, hi);

    /* Steer: cancel the heading error and pull towards that lateral. */
    out->steer = rr_clamp((-in->angle - (myLat - want) / hw * 0.5f) / s->max_steer, -1.0f, 1.0f);

    /* Speed: what we could brake down from within the free distance ahead. */
    float front = in->track[9];
    float target;
    if (front < 0) {
        target = 12.0f; /* off the tarmac: crawl back */
    } else {
        float d = front - 15.0f;
        target = 14.0f + sqrtf(2.0f * 6.0f * rr_stopping_factor(in) * (d > 0 ? d : 0));
    }
    target *= s->speed_scale * blue;
    {
        float follow = rr_follow_speed(in, myLat, want, in->speed_x, 6.0f);
        if (follow < target) target = follow;
        follow = rr_hazard_speed(in, myLat, want, in->speed_x, 10.0f * rr_stopping_factor(in));
        if (follow < target) target = follow;
        follow = rr_flag_speed(in);
        if (follow < target) target = follow;
    }

    float err = target - in->speed_x;
    if (err > 0) {
        out->accel = rr_clamp(0.3f + err * 0.25f, 0.0f, 1.0f);
    } else {
        out->brake = rr_clamp(-err * 0.15f, 0.0f, 1.0f);
    }
    /* traction, oversteer and understeer */
    rr_grip_guard(in, out, &s->tc, s->max_steer);

    snprintf(out->status, sizeof out->status, "target %.0f km/h", target * 3.6f);
    out->debug[0] = target;
}

static void destroy(void* self) { free(self); }

static const RRRobotApi api = {RR_ABI_VERSION, "simple", "Raylib Racers examples", create, drive, destroy, NULL, NULL};

RR_EXPORT const RRRobotApi* rr_robot_entry(void) { return &api; }
