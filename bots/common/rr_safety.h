/* Yellow flags, the virtual safety car and incident holds (ABI 14) for the example robots.
 *
 *  - rr_flag_speed(): the speed to aim at while the host holds the car to a cap (yellow zone, VSC,
 *    rejoin). A little under the cap so the host's limiter never has to step in.
 *  - rr_held(): the host holds the car after an incident; its controls are ignored, so just stay put
 *    (and do not let a recovery routine count the time as being stuck). */
#ifndef RR_SAFETY_H
#define RR_SAFETY_H
#include "rr/robot_api.h"

/* Speed target while flagged, or a huge number if there is no cap. Robots of older ABIs never see a
 * cap (the field is zero) and are held by the host instead. */
static inline float rr_flag_speed(const RRSensors* in) {
    return in->speed_cap > 0.0f ? (in->speed_cap > 3.0f ? in->speed_cap - 2.0f : in->speed_cap) : 1e9f;
}

/* Fills the controls for a held car and returns 1. */
static inline int rr_held(const RRSensors* in, RRControl* out) {
    if (!in->held) return 0;
    out->accel = 0.0f;
    out->brake = 1.0f;
    out->steer = 0.0f;
    out->gear = in->gear;
    return 1;
}

#endif
