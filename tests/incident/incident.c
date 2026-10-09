/*
 * incident: a test robot for incident holds, yellow flags and the virtual safety car (ABI 14).
 * It holds a lateral position (params: pos=<track_pos>) at a target speed (v=<m/s>, default 30).
 *   stop=<s>    brakes to a halt at that race time and stays there (an incident, in the line of
 *               the host's "stopped" rule)
 *   heed=1      aims at speed_cap when the host gives one (the example robots do)
 *   check=1     exits with status 3 if the car runs more than 3 m/s over speed_cap for 1.5 s: the
 *               host must hold every robot to the cap, whatever its ABI
 * Built twice: as ABI 14 (incident) and as ABI 13 (incident13), which the host holds by force.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rr/robot_api.h"

#ifndef TEST_ABI
#define TEST_ABI RR_ABI_VERSION
#endif

typedef struct {
    float pos, v, stop, heed, check, max_steer, over;
} Bot;

static float param(const char* p, const char* key, float def) {
    const char* e = p ? strstr(p, key) : NULL;
    return e ? (float)atof(e + strlen(key)) : def;
}

static void* create(const RRTrackInfo* track, const RRCarSpec* car, int car_index, const char* params,
                    RRRobotConfig* config) {
    (void)track; (void)car_index; (void)config;
    Bot* b = (Bot*)calloc(1, sizeof(Bot));
    b->pos = param(params, "pos=", 0.0f);
    b->v = param(params, "v=", 30.0f);
    b->stop = param(params, "stop=", -1.0f);
    b->heed = param(params, "heed=", 0.0f);
    b->check = param(params, "check=", 0.0f);
    b->max_steer = car->max_steer;
    return b;
}

static void drive(void* self, const RRSensors* in, RRControl* out) {
    Bot* b = (Bot*)self;
    out->gear = in->gear < 1 ? 1 : in->gear;
    out->steer = (-in->angle + (b->pos - in->track_pos) * 0.3f) / b->max_steer;
    if (out->steer > 1) out->steer = 1;
    if (out->steer < -1) out->steer = -1;
    if (b->stop >= 0 && in->time >= b->stop) {
        out->steer = 0;
        out->brake = 1;
        return;
    }
    float target = b->v;
    if (b->heed && in->speed_cap > 0 && in->speed_cap - 2.0f < target) target = in->speed_cap - 2.0f;
    if (b->check) {
        if (in->speed_cap > 0 && !in->pit_zone && in->speed_x > in->speed_cap + 3.0f) b->over += in->dt;
        else b->over = 0;
        if (b->over > 1.5f) {
            fprintf(stderr, "incident: %.1f m/s at %.1f s with a cap of %.1f m/s\n", in->speed_x, in->time,
                    in->speed_cap);
            exit(3);
        }
    }
    if (in->speed_x < target) out->accel = 0.6f;
    else out->brake = 0.3f;
}

static void destroy(void* self) { free(self); }

static const RRRobotApi api = {TEST_ABI, "incident", "Raylib Racers tests", create, drive, destroy, NULL, NULL};

RR_EXPORT const RRRobotApi* rr_robot_entry(void) { return &api; }
