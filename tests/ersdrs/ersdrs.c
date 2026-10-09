/*
 * ersdrs: a test robot for KERS and DRS (ABI 10). It drives gently along the
 * centreline, asking for full KERS and an open DRS flap all the time, and
 * checks what the host tells it. At the end of every session it prints
 * "ERSDRS OK" or "ERSDRS FAIL: why" (the ctests look for those).
 *
 * params: wet=1  the race is run with --wet: DRS must never be offered
 *
 * Checks:
 *   - the 2013 car reports KERS (60 kW, 400 kJ, 2 MJ, 4 MJ) and DRS (drag < 1, downforce < 1)
 *   - KERS releases power (+) on the throttle and recovers (-) under braking, never more than
 *     kers_power, and kers_deploy_left never goes up inside a lap or below zero
 *   - races (the car is alone, so it never earns the flap): drs_state is NONE on laps 1-2, wet
 *     or not, then OFF; practice offers the flap in the zone (AVAILABLE, then OPEN)
 *   - the flap is never open outside a zone, nor right after a touch of the brakes
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rr/robot_api.h"

typedef struct {
    float max_steer, kers_power, kers_energy;
    int wet, session, calls;
    int saw_dep, saw_harv, saw_available, saw_open, saw_zones;
    float last_brake, last_left;
    int last_lap;
    char fail[160];
} Bot;

static void fail(Bot* b, const char* why) {
    if (!b->fail[0]) snprintf(b->fail, sizeof b->fail, "%s (call %d)", why, b->calls);
}

static void* create(const RRTrackInfo* track, const RRCarSpec* car, int car_index, const char* params,
                    RRRobotConfig* config) {
    (void)car_index;
    Bot* b = (Bot*)calloc(1, sizeof(Bot));
    b->max_steer = car->max_steer;
    b->kers_power = car->kers_power;
    b->kers_energy = car->kers_energy;
    b->wet = params && strstr(params, "wet=1") != NULL;
    b->session = config->session;
    b->saw_zones = track->num_drs_zones;
    b->last_left = car->kers_energy;
    if (car->kers_power != 60000.0f || car->kers_energy != 400000.0f || car->kers_harvest != 2000000.0f ||
        car->kers_store != 4000000.0f || !(car->drs_drag_scale < 1.0f) || !(car->drs_downforce_scale < 1.0f))
        fail(b, "the car spec does not describe KERS and DRS");
    if (track->num_drs_zones <= 0) fail(b, "the track has no DRS zones");
    config->tire_compound = RR_TIRE_HARD;
    return b;
}

static void drive(void* self, const RRSensors* in, RRControl* out) {
    Bot* b = (Bot*)self;
    ++b->calls;
    out->steer = (-in->angle - in->track_pos * 0.3f) / b->max_steer;
    if (out->steer > 1) out->steer = 1;
    if (out->steer < -1) out->steer = -1;
    /* speed between 24 and 30 m/s: full throttle, then a real brake touch */
    if (in->speed_x < 24.0f) out->accel = 1.0f;
    else if (in->speed_x > 30.0f) out->brake = 0.5f;
    else out->accel = 0.6f;
    out->kers = 1.0f;
    out->drs = 1;

    /* KERS */
    if (in->kers_power > b->kers_power * 1.001f || in->kers_power < -b->kers_power * 1.001f) fail(b, "kers_power over the limit");
    if (in->kers_power > 1000.0f) b->saw_dep = 1;
    if (in->kers_power < -1000.0f) b->saw_harv = 1;
    if (in->kers_deploy_left < 0 || in->kers_deploy_left > b->kers_energy + 1.0f) fail(b, "kers_deploy_left out of range");
    if (in->lap == b->last_lap && in->kers_deploy_left > b->last_left + 1.0f) fail(b, "kers_deploy_left went up inside a lap");
    if (in->kers_store < 0 || in->kers_store > 4000001.0f) fail(b, "kers_store out of range");
    b->last_left = in->kers_deploy_left;
    b->last_lap = in->lap;

    /* DRS */
    if (in->drs_open && in->drs_zone < 0) fail(b, "the flap is open outside a zone");
    if (in->drs_open && b->last_brake >= 0.02f) fail(b, "the flap stayed open after a brake touch");
    if (in->drs_open != (in->drs_state == RR_DRS_OPEN)) fail(b, "drs_open and drs_state disagree");
    if (in->drs_state == RR_DRS_AVAILABLE) b->saw_available = 1;
    if (in->drs_state == RR_DRS_OPEN) b->saw_open = 1;
    if (in->session == RR_SESSION_RACE) {
        if ((b->wet || in->lap < RR_DRS_FIRST_LAP) && in->drs_state != RR_DRS_NONE) fail(b, "DRS offered when it is not allowed");
        if (!b->wet && in->lap >= RR_DRS_FIRST_LAP && b->calls > 2 && in->drs_state == RR_DRS_NONE && in->pit_state == RR_PIT_NONE)
            fail(b, "DRS not allowed after the first laps");
        if (in->drs_state >= RR_DRS_ARMED) fail(b, "a car alone on track earned the flap");
    } else if (b->calls > 2 && in->drs_state == RR_DRS_NONE && in->pit_state == RR_PIT_NONE && !b->wet) {
        fail(b, "DRS not allowed in practice");
    }
    b->last_brake = out->brake;
}

static void session_end(void* self, const RRSessionSummary* s) {
    Bot* b = (Bot*)self;
    (void)s;
    if (b->session != RR_SESSION_RACE) {
        if (!b->saw_available) fail(b, "never offered the flap");
        if (!b->saw_open) fail(b, "the flap never opened");
    }
    if (!b->saw_dep) fail(b, "KERS never deployed");
    if (!b->saw_harv) fail(b, "KERS never recovered energy");
    if (b->fail[0]) printf("ERSDRS FAIL: %s\n", b->fail);
    else printf("ERSDRS OK (%d zones)\n", b->saw_zones);
    fflush(stdout);
}

static void destroy(void* self) { free(self); }

static const RRRobotApi api = {RR_ABI_VERSION, "ersdrs", "Raylib Racers tests", create, drive, destroy, NULL, session_end};

RR_EXPORT const RRRobotApi* rr_robot_entry(void) { return &api; }
