// Tyre model checks: controlled runs of the car model with no bot and no track. Locked braking wears
// both axles, a steady turn heats the outside tyres, hot tyres cool towards the air, and the numbers
// do not depend on the physics step. Exits non-zero on the first failure.
#include <cmath>
#include <cstdio>

#include "car.hpp"

static int fails = 0;
#define CHECK(c)                                                  \
    do {                                                          \
        if (!(c)) {                                               \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
            ++fails;                                              \
        }                                                         \
    } while (0)

static const rr::CarParams P;
static const rr::Surface S;
static const rr::WearRates R;

// Steady turn of radius r at speed v for `seconds`: steer follows yaw rate, the throttle holds the speed.
static rr::CarState corner(float r, float v, float seconds, float dt) {
    rr::CarState s;
    s.fuel = 40;
    s.vx = v;
    s.gear = 3;
    float integ = 0;
    for (int i = 0; i < (int)(seconds / dt); ++i) {
        RRControl c{};
        const float err = v / r - s.yawRate;
        integ += err * dt;
        c.steer = std::fmax(-1.0f, std::fmin(1.0f, (P.wheelbase() / r + 0.15f * err + integ) / P.maxSteer));
        const float dv = v - s.vx;
        if (dv > 0) c.accel = std::fmin(1.0f, 0.3f * dv);
        else c.brake = std::fmin(1.0f, -0.3f * dv);
        rr::stepCar(s, P, c, true, S, R, dt);
    }
    return s;
}

int main() {
    // Locked braking: both axles are past their grip, so both wear well beyond their rolling wear,
    // and the front (60% of the braking, more of the load) does at least as much work as the rear.
    {
        rr::CarState s;
        s.fuel = 40;
        s.vx = 83;
        s.gear = 6;
        RRControl c{};
        c.brake = 1;
        for (int i = 0; i < 4000 && s.vx > 28; ++i) rr::stepCar(s, P, c, true, S, R, 0.002f);
        CHECK(s.gripUse[0] > 1.0f && s.gripUse[1] > 1.0f);
        CHECK(s.tireWear[0] > 0.002f && s.tireWear[1] > 0.002f);
        CHECK(s.tireWear[0] >= s.tireWear[1]);
        CHECK(s.brakeTemp[0] > s.brakeTemp[2]);  // front discs take more braking energy
    }
    // Steady turn: the outside tyres carry more load and force, so they run hotter; nothing goes wrong.
    {
        const rr::CarState s = corner(100, 40, 30, 0.002f);
        CHECK(s.wheelTemp[1] > s.wheelTemp[0] + 5 && s.wheelTemp[3] > s.wheelTemp[2] + 5);
        CHECK(std::isfinite(s.wheelTemp[0]) && s.tireWear[0] > 0 && s.tireWear[1] > 0);
        CHECK(s.tireWear[0] < 0.02f && s.tireWear[1] < 0.02f);
    }
    // The physics step does not change the answer.
    {
        const rr::CarState a = corner(100, 40, 30, 0.002f), b = corner(100, 40, 30, 0.005f);
        for (int w = 0; w < 4; ++w) CHECK(std::fabs(a.wheelTemp[w] - b.wheelTemp[w]) < 0.5f);
        for (int ax = 0; ax < 2; ++ax) CHECK(std::fabs(a.tireWear[ax] - b.tireWear[ax]) < 0.03f * a.tireWear[ax]);
    }
    // Cooling: hot tyres standing still lose heat monotonically towards the air and never undershoot it.
    {
        rr::CarState s;
        for (float& t : s.wheelTemp) t = 110;
        for (float& t : s.rimTemp) t = 60;
        for (float& t : s.brakeTemp) t = 60;
        RRControl c{};
        float prev = 110;
        bool monotonic = true;
        for (int i = 0; i < 500 * 1200; ++i) {
            rr::stepCar(s, P, c, true, S, R, 0.002f);
            if (i % 500 == 0) {
                monotonic = monotonic && s.wheelTemp[0] <= prev + 1e-3f;
                prev = s.wheelTemp[0];
            }
        }
        CHECK(monotonic);
        CHECK(s.wheelTemp[0] >= R.ambient - 0.01f && s.wheelTemp[0] < 45.0f);
    }
    // A hot rim warms a cooler tyre (the brake heat path) and the heat is not created from nothing:
    // with a hot disc and nothing else, the tyre never gets hotter than the disc.
    {
        rr::CarState s;
        for (float& t : s.wheelTemp) t = 60;
        for (float& t : s.rimTemp) t = 150;
        for (float& t : s.brakeTemp) t = 400;
        RRControl c{};
        float maxTyre = 0;
        for (int i = 0; i < 500 * 120; ++i) {
            rr::stepCar(s, P, c, true, S, R, 0.002f);
            maxTyre = std::fmax(maxTyre, s.wheelTemp[0]);
        }
        CHECK(maxTyre > 70.0f && maxTyre < 150.0f);
    }
    if (fails) return 1;
    std::printf("tyres: ok\n");
    return 0;
}
