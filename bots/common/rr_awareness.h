/* Shared racecraft helpers for the example robots: knowing where the other cars
 * are (ahead, alongside and behind), blue flags, and driving at the tyres'
 * limit without throwing the car away. Plain C99, header only.
 *
 * Lateral positions are in metres from the centreline, + = left, like
 * RROpponent.lateral; "my lateral" is track_pos * half_width. */
#ifndef RR_AWARENESS_H
#define RR_AWARENESS_H
#include <math.h>

#include "rr/robot_api.h"

#define RR_CAR_GAP 2.7f /* centre-to-centre lateral spacing for two cars side by side */

/* --------------------------------------------------------------- traffic */

/* The lateral range [*lo, *hi] we can move into without hitting a car that is
 * alongside or about to be: cars overlapping us lengthways now, and cars behind
 * that will be alongside within ~1.2 s at the current closing speed. Start with
 * lo and hi set to the track limits; they are only narrowed. Returns the number
 * of cars that constrained the range. */
static inline int rr_side_limits(const RRSensors* in, float myLat, float v, float* lo, float* hi) {
    int n = 0;
    for (int k = 0; k < in->num_nearby; ++k) {
        const RROpponent* o = &in->nearby[k];
        if (o->pit_state != RR_PIT_NONE && in->pit_state == RR_PIT_NONE) continue;
        float closing = o->speed - v; /* + = they are faster */
        float back = 5.5f + (closing > 0 ? closing * 1.2f : 0.0f);
        if (o->ds > 5.5f || o->ds < -back) continue;
        float d = o->lateral - myLat;
        if (fabsf(d) < 1.0f) continue; /* nose to tail: that is a following problem, not a side one */
        if (d > 0) {
            float lim = o->lateral - RR_CAR_GAP;
            if (lim < *hi) { *hi = lim; ++n; }
        } else {
            float lim = o->lateral + RR_CAR_GAP;
            if (lim > *lo) { *lo = lim; ++n; }
        }
    }
    if (*lo > *hi) { float m = 0.5f * (*lo + *hi); *lo = *hi = m; }
    return n;
}

/* Speed limit for not running into the car ahead in our lane (laneLat: where we
 * are heading, myLat: where we are). Keeps a gap that grows with speed and
 * brakes for the closing speed in time, assuming we can brake at `decel`. */
static inline float rr_follow_speed(const RRSensors* in, float myLat, float laneLat, float v, float decel) {
    float cap = 1e9f;
    for (int k = 0; k < in->num_nearby; ++k) {
        const RROpponent* o = &in->nearby[k];
        if (o->ds <= 0 || o->ds > 20.0f + 1.2f * v) continue;
        if (o->pit_state != RR_PIT_NONE && in->pit_state == RR_PIT_NONE) continue;
        int inPath = fabsf(o->lateral - laneLat) < 2.2f || (o->ds < 12.0f && fabsf(o->lateral - myLat) < 2.2f);
        if (!inPath) continue;
        float gap = o->ds - 5.0f;               /* nose to tail */
        float want = 1.5f + 0.06f * v;           /* gap we like to keep */
        float room = gap - want;
        /* Speed we can still shed to theirs within the room left. */
        float vMax = sqrtf(fmaxf(0.0f, o->speed * o->speed + 2.0f * decel * fmaxf(0.0f, room)));
        if (room < 0) vMax = fmaxf(0.0f, o->speed + 0.6f * room);
        /* A stopped car is an obstacle to steer round, not a queue to join. */
        if (o->speed < 3.0f && vMax < 5.0f) vMax = 5.0f;
        if (vMax < cap) cap = vMax;
    }
    return cap;
}

/* Speed cap for a crawling, stopped or rejoining car ahead (a spin, an off,
 * a car limping back on): unlike a car we follow, it may be far slower than
 * us and moving across the track, so it is seen from braking distance away,
 * on a wider path, and we brake to pass it at a safe speed. myLat is our
 * lateral now, laneLat where we are heading, decel what we can brake at
 * (m/s^2). Returns a large number when nothing is in the way. */
static inline float rr_hazard_speed(const RRSensors* in, float myLat, float laneLat, float v, float decel) {
    float cap = 1e9f;
    if (decel < 1.0f) decel = 1.0f;
    for (int k = 0; k < in->num_nearby; ++k) {
        const RROpponent* o = &in->nearby[k];
        if (o->ds <= 0) continue;
        if (o->pit_state != RR_PIT_NONE && in->pit_state == RR_PIT_NONE) continue;
        const float along = o->speed * cosf(o->rel_yaw);  /* their speed down the track */
        const int slow = along < 0.6f * v && v - along > 12.0f;
        const int across = fabsf(sinf(o->rel_yaw)) > 0.35f;  /* sideways: spun or rejoining */
        if (!slow && !across) continue;
        const float reach = 30.0f + (v * v - along * along) / (2.0f * decel) * 1.3f;
        if (o->ds > reach) continue;
        /* a sideways car may cross our path; a slow one only blocks its own lane */
        const float lo = fminf(myLat, laneLat), hi = fmaxf(myLat, laneLat);
        const float pad = across ? 4.5f : 3.0f;
        if (o->lateral < lo - pad || o->lateral > hi + pad) continue;
        /* pass it no faster than this: close to its speed when it is in our lane */
        const float passV = fmaxf(along, 0.0f) + (fabsf(o->lateral - laneLat) > 2.6f && !across ? 15.0f : 4.0f);
        const float room = fmaxf(0.0f, o->ds - 8.0f);
        const float vMax = sqrtf(passV * passV + 2.0f * decel * room);
        if (vMax < cap) cap = vMax;
    }
    return cap;
}

/* --------------------------------------------------------------- blue flag */

/* Under a blue flag: give the lapping car room. Picks the side away from it
 * (keeping to the side we are on if it is right behind us), writes the lateral
 * to drive to into *targetLat and returns a speed factor (< 1 once it is close).
 * *side (start at 0) remembers the side chosen until the flag goes, so the car
 * does not weave in front of the lapping car; pass NULL to choose afresh each
 * call. Returns 1 and leaves *targetLat alone without a blue flag. */
static inline float rr_blue_flag_side(const RRSensors* in, float myLat, float halfWidth, float* side, float* targetLat) {
    if (!in->blue_flag) {
        if (side) *side = 0;
        return 1.0f;
    }
    float s = side ? *side : 0;
    if (s == 0) {
        const RROpponent* o = 0;
        for (int k = 0; k < in->num_nearby; ++k)
            if (in->nearby[k].car_index == in->blue_flag_car) o = &in->nearby[k];
        if (o && fabsf(o->lateral - myLat) > 0.8f) s = o->lateral > myLat ? -1.0f : 1.0f;
        else s = myLat >= 0 ? 1.0f : -1.0f;
        if (side) *side = s;
    }
    *targetLat = s * halfWidth * 0.7f;
    float ds = -in->blue_flag_ds; /* how far back it is */
    return ds < 30.0f ? 0.9f : 0.97f;
}

static inline float rr_blue_flag(const RRSensors* in, float myLat, float halfWidth, float* targetLat) {
    return rr_blue_flag_side(in, myLat, halfWidth, 0, targetLat);
}

/* --------------------------------------------------------------- grip */

/* Keeps a car at the edge of grip rather than over it. Call after working out
 * steer/accel/brake; `tc` is the robot's traction-control state (start at 1).
 *  - traction: throttle is cut when the rear asks for more than it has and
 *    given back gently;
 *  - oversteer: when the rear slides more than the front, countersteer into
 *    the slide and come off the throttle;
 *  - understeer: past the front's peak more lock only scrubs speed, so the
 *    car lifts a little to let the front bite again.
 * maxSteer is the road-wheel angle at steer = 1. Works with ABI 2 hosts too
 * (falls back to wheel_spin when grip_use is all zero). */
static inline void rr_grip_guard(const RRSensors* in, RRControl* out, float* tc, float maxSteer) {
    float rear = in->grip_use[1], front = in->grip_use[0];
    if (rear == 0 && front == 0) rear = 1.0f + in->wheel_spin; /* old host */
    /* traction */
    if (rear > 1.02f && in->speed_x > 3.0f) {
        float cut = 2.5f * (rear - 1.02f);
        *tc = *tc - cut < 0.08f ? 0.08f : *tc - cut;
    } else {
        *tc = *tc + 0.04f > 1.0f ? 1.0f : *tc + 0.04f;
    }
    out->accel *= *tc;
    if (in->speed_x < 8.0f) return;

    float ar = in->slip_angle[1], af = in->slip_angle[0];
    /* oversteer: rear slip angle well past the front's, rear past its grip */
    if (rear > 1.1f && fabsf(ar) > fabsf(af) + 0.03f && fabsf(ar) > 0.06f) {
        float catchAng = (fabsf(ar) - 0.04f) * 1.4f; /* road-wheel angle towards the slide */
        float s = out->steer + (ar > 0 ? 1.0f : -1.0f) * catchAng / maxSteer;
        out->steer = s > 1.0f ? 1.0f : (s < -1.0f ? -1.0f : s);
        if (out->accel > 0.25f) out->accel = 0.25f;
        if (out->brake > 0.15f) out->brake = 0.15f; /* braking a sliding rear makes it worse */
    } else if (front > 1.15f && fabsf(af) > fabsf(ar)) {
        /* understeer: past the front's peak more lock only scrubs speed. Unwind
         * the lock towards the peak slip angle and lift, which moves weight
         * onto the front and lets it bite again. */
        float k = 1.1f / front;
        out->steer *= k < 0.6f ? 0.6f : k;
        if (front > 1.3f) out->accel = 0.0f;
        else if (out->accel > 0.3f) out->accel *= 0.6f;
    }
}

/* --------------------------------------------------------------- brakes and tyres */

/* How much of its usual stopping power the car has now (about 0.6 to 1):
 * cold discs bite less and overheated ones fade (brake_temp, ABI 7), and
 * tyres out of their window or cooking grip less (axle_grip against
 * tire_grip), and the grade underfoot (ABI 9). Multiply a planned deceleration
 * by it. 1 on older hosts. */
static inline float rr_stopping_factor(const RRSensors* in) {
    float f = 1.0f;
    if (in->brake_temp_window[1] > 0) {
        const float lo = in->brake_temp_window[0], hi = in->brake_temp_window[1];
        float cold = 1e9f, hot = -1e9f, b = 1.0f;
        int w;
        for (w = 0; w < 4; ++w) {
            if (in->brake_temp[w] < cold) cold = in->brake_temp[w];
            if (in->brake_temp[w] > hot) hot = in->brake_temp[w];
        }
        if (cold < lo) {
            float u = (cold - 50.0f) / (lo - 50.0f);
            b = 0.75f + 0.25f * (u < 0 ? 0 : u > 1 ? 1 : u);
        }
        if (hot > hi) {
            float fade = 1.0f - 0.002f * (hot - hi);
            if (fade < 0.6f) fade = 0.6f;
            if (fade < b) b = fade;
        }
        /* the tyres usually limit braking before the discs do: count half */
        f *= 0.5f + 0.5f * b;
    }
    if (in->tire_temp_window[1] > 0 && in->tire_grip > 0.3f) {
        float g = (in->axle_grip[0] < in->axle_grip[1] ? in->axle_grip[0] : in->axle_grip[1]) / in->tire_grip;
        f *= g < 0.8f ? 0.8f : g > 1.0f ? 1.0f : g;
    }
    /* Hills (ABI 9): gravity adds to the brakes uphill and fights them downhill,
     * about g * grade against the 8 m/s^2 or so these robots plan for. */
    {
        float h = 1.0f + 9.81f * in->grade / 8.0f;
        f *= h < 0.6f ? 0.6f : h > 1.3f ? 1.3f : h;
    }
    return f;
}

#endif
