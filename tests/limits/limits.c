/*
 * limits: a test robot for the track-limits rule (ABI 12). It holds a steady
 * lateral position, in units of half the track width (params: pos=<track_pos>),
 * at 18 m/s. pos=-0.9 keeps the wheels inside the white line; pos=-1.3 drives (the right: the oval pit area is on the left)
 * with all four wheels beyond it and must earn penalties.
 */
#include <stdlib.h>
#include <string.h>

#include "rr/robot_api.h"

typedef struct {
    float pos, max_steer;
} Bot;

static void* create(const RRTrackInfo* track, const RRCarSpec* car, int car_index, const char* params,
                    RRRobotConfig* config) {
    (void)track; (void)car_index; (void)config;
    Bot* b = (Bot*)calloc(1, sizeof(Bot));
    const char* e = params ? strstr(params, "pos=") : NULL;
    b->pos = e ? (float)atof(e + 4) : 0.0f;
    b->max_steer = car->max_steer;
    return b;
}

static void drive(void* self, const RRSensors* in, RRControl* out) {
    Bot* b = (Bot*)self;
    out->steer = (-in->angle + (b->pos - in->track_pos) * 0.3f) / b->max_steer;
    if (out->steer > 1) out->steer = 1;
    if (out->steer < -1) out->steer = -1;
    if (in->speed_x < 18.0f) out->accel = 0.5f;
    else out->brake = 0.2f;
}

static void destroy(void* self) { free(self); }

static const RRRobotApi api = {RR_ABI_VERSION, "limits", "Raylib Racers tests", create, drive, destroy, NULL, NULL};

RR_EXPORT const RRRobotApi* rr_robot_entry(void) { return &api; }
