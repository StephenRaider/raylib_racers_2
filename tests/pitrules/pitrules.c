/*
 * pitrules: a test robot for the pit road rules and served penalties (ABI 13). It drives round at
 * 18 m/s and takes the pit lane every lap (the track needs one), without stopping unless asked.
 * Params, "key=value,key=value":
 *   vpit=<m/s>  speed on the pit road (default 18, under the 22 m/s limit)
 *   exit=early  cross the white line as soon as the lane ends instead of at the exit line
 *   stop=1      stop in the box once a penalty is owed (a stop with nothing to fit: it only serves the penalty),
 *               then keep to 18 m/s
 *   limiter=1   engage the pit limiter on the pit road (vpit is then capped by the host)
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "rr/robot_api.h"

typedef struct {
    RRPitInfo pit;
    const RRTrackPoint* tp;
    int n;
    float length, vpit, max_steer;
    int early, stop, limiter, served, arm;
} Bot;

static float param(const char* p, const char* key, float def) {
    const char* e = p ? strstr(p, key) : NULL;
    return e ? (float)atof(e + strlen(key)) : def;
}

static void* create(const RRTrackInfo* track, const RRCarSpec* car, int car_index, const char* params,
                    RRRobotConfig* config) {
    (void)car_index; (void)config;
    if (!track->pit.has_pit) return NULL;
    Bot* b = (Bot*)calloc(1, sizeof(Bot));
    b->pit = track->pit;
    b->tp = track->points;
    b->n = track->num_points;
    b->length = track->length;
    b->vpit = param(params, "vpit=", 18.0f);
    b->early = params && strstr(params, "exit=early");
    b->stop = param(params, "stop=", 0) > 0.5f;
    b->limiter = param(params, "limiter=", 0) > 0.5f;
    b->max_steer = car->max_steer;
    return b;
}

static float fwd(const Bot* b, float from, float to) {
    float d = fmodf(to - from, b->length);
    return d < 0 ? d + b->length : d;
}

static int in_span(const Bot* b, float s, float a, float c) { return fwd(b, a, s) <= fwd(b, a, c); }

static float smooth(float u) {
    u = u < 0 ? 0 : u > 1 ? 1 : u;
    return u * u * (3 - 2 * u);
}

static void drive(void* self, const RRSensors* in, RRControl* out) {
    Bot* b = (Bot*)self;
    const RRPitInfo* p = &b->pit;
    const float s = in->dist_from_start;
    const float hw = b->tp[in->track_index].half_width;
    const float side = (float)p->side;
    const float edge = 1.0f + 1.5f / hw, lane = 1.0f + 5.0f / hw;  // track_pos at the pit side of the white line
    float target = 0.0f;  // track_pos, + = left
    float v = 18.0f;

    if (in_span(b, s, p->entry_s, p->exit_s)) {
        v = in_span(b, s, p->entry_s, p->lane_start_s) ? 18.0f : b->vpit;  // arrive at a legal speed
        if (in_span(b, s, p->entry_s, p->lane_start_s)) {
            b->arm = in->penalty_owed > 0;  // stop in the box on this visit
            float len = 0.4f * fwd(b, p->entry_s, p->lane_start_s);
            target = side * lane * smooth(fwd(b, p->entry_s, s) / len);
        } else if (in_span(b, s, p->lane_start_s, p->lane_end_s)) {
            target = side * lane;
        } else if (b->early) {
            target = 0.0f;
        } else {
            float to_exit = fwd(b, s, p->exit_s);
            target = to_exit > 12.0f ? side * edge : side * edge * smooth(to_exit / 12.0f);
        }
        if (in->pit_state == RR_PIT_DONE) b->served = 1;
        if (b->served) v = 18.0f;
        if (b->stop && !b->served && b->arm) {
            float to_box = fwd(b, s, in->pit_box_s);
            if (to_box > b->length / 2) to_box -= b->length;
            out->pit_request = 1;
            out->pit_tires = 0;
            out->pit_fuel = 0;
            if (to_box < 70.0f && to_box > -3.0f) target = side * (1.0f + 9.0f / hw);  // pull into the box
            if (to_box < 80.0f) {
                float vs = sqrtf(2 * 3.0f * (to_box > 0.3f ? to_box - 0.3f : 0.0f));
                if (vs < v) v = vs;
            }
            if (in->pit_state == RR_PIT_SERVICE) v = 0;
        }
        out->pit_limiter = b->limiter;
    }
    out->steer = (-in->angle + (target - in->track_pos) * 0.3f) / b->max_steer;
    if (out->steer > 1) out->steer = 1;
    if (out->steer < -1) out->steer = -1;
    if (in->speed_x < v) out->accel = in->speed_x < v - 3 ? 0.6f : 0.3f;
    else out->brake = in->speed_x > v + 2 ? 0.6f : 0.15f;
}

static void destroy(void* self) { free(self); }

static const RRRobotApi api = {RR_ABI_VERSION, "pitrules", "Raylib Racers tests", create, drive, destroy, NULL, NULL};

RR_EXPORT const RRRobotApi* rr_robot_entry(void) { return &api; }
