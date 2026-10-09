#include "car.hpp"

#include <algorithm>

namespace rr {

float CarParams::engineTorque(float rpm) const {
    // Piecewise-linear full-throttle torque curve.
    // A 3-litre V10 of the mid-2000s: ~350 N m, ~660 kW at 18,500 rpm.
    static const float v10[][2] = {{0, 150},     {4000, 220},  {8000, 290},  {12000, 330},
                                   {16000, 352}, {18500, 340}, {19000, 300}, {19400, 0}};
    // A 2013 2.4 l V8 (~750 hp): a wide, flat curve of 270-305 N m, 560 kW at 18,000 rpm, then
    // the rev limiter at 18,200.
    static const float v8[][2] = {{0, 140},      {4000, 175},  {6000, 215},  {8000, 255},   {10000, 275},
                                  {12000, 290},  {14000, 300}, {16000, 305}, {17500, 300},  {18000, 297},
                                  {18200, 250},  {18400, 0}};
    const bool eight = engineV8 > 0.5f;
    const float(*pts)[2] = eight ? v8 : v10;
    const int n = eight ? (int)(sizeof(v8) / sizeof(v8[0])) : (int)(sizeof(v10) / sizeof(v10[0]));
    if (rpm <= pts[0][0]) return pts[0][1];
    for (int i = 1; i < n; ++i) {
        if (rpm <= pts[i][0]) {
            float f = (rpm - pts[i - 1][0]) / (pts[i][0] - pts[i - 1][0]);
            return torqueScale * (pts[i - 1][1] + (pts[i][1] - pts[i - 1][1]) * f);
        }
    }
    return 0;
}

float CarParams::maxPower() const {
    float best = 0;
    for (float rpm = idleRpm; rpm <= maxRpm; rpm += 100) best = std::max(best, engineTorque(rpm) * rpm * (2 * kPi / 60.0f));
    return best;
}

const CarParams::Field* CarParams::fields(int* count) {
#define F(n) {#n, &CarParams::n}
    static const Field f[] = {
        F(mass), F(length), F(width), F(cgToFront), F(cgToRear), F(cgHeight), F(trackFront), F(trackRear),
        F(rollStiffFront), F(suspensionLag), F(diffLock), F(yawInertia), F(maxSteer), F(steerRate), F(tireMu),
        F(tireB), F(tireC), F(frontGrip), F(rearGrip), F(frontStiffness), F(loadSens), F(muLoadDrop),
        F(dragCoeff), F(downforceCoeff), F(downforceFront), F(aeroPitchShift), F(aeroYawLoss),
        F(rollingResist), F(wheelRadius), F(finalDrive), F(reverseRatio), F(idleRpm), F(maxRpm),
        F(maxBrakeForce), F(brakeFront), F(engineBrake), F(drivetrainEff), F(fuelCapacity), F(fuelDensity),
        F(fuelPerJoule), F(wearPerJoule), F(tireHeatCap), F(tireSlideHeat), F(tireLonHeat), F(tireRollHeat),
        F(tireCoolBase), F(tireCoolSpeed), F(blanketTemp), F(brakeHeatCap), F(brakeCoolBase), F(brakeCoolSpeed),
        F(brakeToRim), F(rimHeatCap), F(rimCoolBase), F(rimCoolSpeed), F(rimToTyre), F(brakeTempLo), F(brakeTempHi), F(maxAeroLoss), F(damageForMaxLoss), F(maxDragGain), F(maxPowerLoss), F(maxGripLoss), F(torqueScale),
        F(pitServiceScale), F(engineV8), F(noRefuel), F(kersPower), F(kersEnergy), F(kersHarvest), F(kersStore),
        F(drsDragScale), F(drsDownforceScale), F(kersMaxTorque), F(kersEfficiency), F(kersDeployEfficiency), F(drsFlapOpenTime), F(drsFlapCloseTime),
    };
#undef F
    *count = (int)(sizeof f / sizeof f[0]);
    return f;
}

float* CarParams::field(const std::string& name) {
    int n = 0;
    const Field* f = fields(&n);
    for (int i = 0; i < n; ++i)
        if (name == f[i].name) return &(this->*(f[i].ptr));
    return nullptr;
}

const Compound& compoundInfo(int compound) {
    static const Compound soft{1.05f, 2.0f, 85, 105}, medium{1.0f, 1.0f, 95, 115}, hard{0.965f, 0.72f, 105, 125};
    return compound == RR_TIRE_SOFT ? soft : (compound == RR_TIRE_HARD ? hard : medium);
}

float compoundGrip(int compound) { return compoundInfo(compound).grip; }
float compoundWear(int compound) { return compoundInfo(compound).wear; }

// Cold tyres lose 0.25% grip per degree below the window, hot ones 0.2% per
// degree above it.
float tempGrip(int compound, float t) {
    const Compound& k = compoundInfo(compound);
    float g = 1.0f;
    if (t < k.tempLo) g -= 0.0025f * (k.tempLo - t);
    else if (t > k.tempHi) g -= 0.002f * (t - k.tempHi);
    return std::max(0.8f, g);
}

// Overheated tyres wear fast (blistering: x2 at 17 C over the window), cold
// ones a little faster too (graining).
float tempWear(int compound, float t) {
    const Compound& k = compoundInfo(compound);
    if (t > k.tempHi) return 1.0f + 0.06f * (t - k.tempHi);
    if (t < k.tempLo) return 1.0f + 0.015f * (k.tempLo - t);
    return 1.0f;
}

// Cold carbon brakes bite poorly; past the window they fade.
float brakeGrip(const CarParams& p, float t) {
    if (t < p.brakeTempLo) return 0.75f + 0.25f * clampf((t - 50.0f) / (p.brakeTempLo - 50.0f), 0, 1);
    if (t > p.brakeTempHi) return std::max(0.6f, 1.0f - 0.002f * (t - p.brakeTempHi));
    return 1.0f;
}

float wornGrip(float w) {
    w = clampf(w, 0, 1);
    return 1.0f - 0.07f * w - 0.8f * std::max(0.0f, w - 0.7f);
}

float axleGrip(const CarState& c, int axle) {
    return compoundGrip(c.compound) * wornGrip(c.tireWear[axle]) * tempGrip(c.compound, c.tireTemp[axle]);
}

float damageLevel(const CarParams& p, const CarState& c) { return std::min(1.0f, c.damage / p.damageForMaxLoss); }

float carMass(const CarParams& p, const CarState& c) { return p.mass + std::max(0.0f, c.fuel) * p.fuelDensity; }

RRCarSpec CarParams::spec() const {
    RRCarSpec s{};
    s.mass = mass;
    s.length = length;
    s.width = width;
    s.wheelbase = wheelbase();
    s.cg_to_front = cgToFront;
    s.cg_to_rear = cgToRear;
    s.max_steer = maxSteer;
    s.tire_mu = tireMu;
    s.drag_coeff = dragCoeff;
    s.downforce_coeff = downforceCoeff;
    s.max_rpm = maxRpm;
    s.wheel_radius = wheelRadius;
    s.final_drive = finalDrive;
    s.num_gears = numGears;
    for (int i = 0; i < RR_MAX_GEARS; ++i) s.gear_ratios[i] = gearRatios[i];
    s.max_brake_force = maxBrakeForce;
    s.fuel_capacity = fuelCapacity;
    s.fuel_density = fuelDensity;
    s.cg_height = cgHeight;
    s.track_front = trackFront;
    s.track_rear = trackRear;
    s.downforce_front = downforceFront;
    s.brake_front = brakeFront;
    s.max_power = maxPower();
    s.tire_wear_scale = wearPerJoule / CarParams{}.wearPerJoule;
    s.fuel_use_scale = fuelPerJoule / CarParams{}.fuelPerJoule;
    s.pit_service_scale = pitServiceScale;
    s.kers_power = kersPower;
    s.kers_energy = kersEnergy;
    s.kers_harvest = kersHarvest;
    s.kers_store = kersStore;
    s.drs_drag_scale = drsDragScale;
    s.drs_downforce_scale = drsDownforceScale;
    return s;
}

namespace {

float gearRatio(const CarParams& p, int gear) {
    if (gear > 0) return p.gearRatios[gear - 1] * p.finalDrive;
    if (gear < 0) return -p.reverseRatio * p.finalDrive;
    return 0;
}

float rpmFor(const CarParams& p, float vx, int gear) {
    const float toRpm = 60.0f / (2 * kPi);
    return std::fabs(vx / p.wheelRadius * gearRatio(p, gear)) * toRpm;
}

// Lateral tyre force for slip angle alpha, load fz and friction mu.
// fz0 is the static axle load: away from it the cornering stiffness and the
// friction coefficient change less than linearly, as real tyres do.
float tyreLateral(const CarParams& p, float stiffness, float alpha, float fz, float fz0, float mu) {
    float b = p.tireB * stiffness * std::pow(fz0 / fz, p.loadSens);
    return -mu * fz * std::sin(p.tireC * std::atan(b * alpha));
}

// Saturate a force pair to the friction circle.
void frictionCircle(float& fx, float& fy, float maxF) {
    float f = std::sqrt(fx * fx + fy * fy);
    if (f > maxF && f > 0) {
        float k = maxF / f;
        fx *= k;
        fy *= k;
    }
}

}  // namespace

void stepCar(CarState& c, const CarParams& p, const RRControl& in, bool autoGear,
             const Surface& surf, const WearRates& rates, float dt) {
    const float g = 9.81f;
    const float m = carMass(p, c);
    const float yawInertia = p.yawInertia;  // the fuel sits at the CG: it adds mass, not yaw inertia
    const float a = p.cgToFront, b = p.cgToRear, L = p.wheelbase();

    // --- steering actuator (rate limited) ---
    float target = clampf(in.steer, -1, 1) * p.maxSteer;
    float maxD = p.steerRate * dt;
    c.steerAngle += clampf(target - c.steerAngle, -maxD, maxD);
    const float accel = clampf(in.accel, 0, 1);
    const float brake = clampf(in.brake, 0, 1);

    // --- gearbox ---
    if (autoGear) {
        if (in.gear == -1) {
            if (c.vx < 1.0f) c.gear = -1;
        } else {
            // Forward asked while still rolling backwards: neutral until the car has
            // (nearly) stopped, so the throttle can't keep driving it in reverse.
            if (c.gear < 0 && c.vx <= -1.0f) c.gear = 0;
            if (c.gear <= 0 && c.vx > -1.0f) c.gear = 1;
            if (c.gear > 0) {
                float rpm = rpmFor(p, c.vx, c.gear);
                if (c.gear < p.numGears && rpm > 0.965f * p.maxRpm) {
                    c.gear++;
                } else if (c.gear > 1 && rpm < 0.62f * p.maxRpm &&
                           rpmFor(p, c.vx, c.gear - 1) < 0.9f * p.maxRpm) {  // keep the revs up, F1 style
                    c.gear--;
                }
            }
        }
    } else {
        c.gear = std::max(-1, std::min(in.gear, p.numGears));
    }

    // --- engine ---
    const float ratio = gearRatio(p, c.gear);
    const float engRpm = rpmFor(p, c.vx, c.gear);
    c.rpm = std::max(p.idleRpm, engRpm);  // clutch slips below idle
    float torque = 0, kersTorque = 0;
    const bool hasFuel = c.fuel > 0;
    // KERS deploy: the motor-generator's power at the crank, as torque, from the store, limited by the
    // lap's energy allowance. It follows the throttle and cuts out with the engine at the rev limiter.
    const bool hasKers = p.kersPower > 0;
    if (hasKers && c.gear > 0 && c.rpm < p.maxRpm && c.kersCharge > 0 && c.kersDeployed < p.kersEnergy) {
        const float want = clampf(in.kers, 0, 1) * accel;
        const float omega = std::max(c.rpm, p.idleRpm) * (2 * kPi / 60.0f);
        kersTorque = std::min(want * p.kersPower / omega, want * p.kersMaxTorque);
        const float room = std::min(c.kersCharge * p.kersDeployEfficiency, p.kersEnergy - c.kersDeployed);  // mechanical J left
        const float power = kersTorque * omega;
        if (power * dt > room) kersTorque = room / (dt * omega);
        c.kersPowerNow = kersTorque * omega;
        const float used = c.kersPowerNow * dt;
        c.kersCharge = std::max(0.0f, c.kersCharge - used / p.kersDeployEfficiency);  // the store gives up more than the crank gets
        c.kersDeployed += used;
    } else {
        c.kersPowerNow = 0;
    }
    if (c.gear != 0) {
        if (c.rpm < p.maxRpm && hasFuel) torque = p.engineTorque(c.rpm) * accel * (1 - p.maxPowerLoss * damageLevel(p, c));
        torque += kersTorque;
        if (engRpm > p.idleRpm) torque -= (1 - accel) * p.engineBrake * (engRpm / p.maxRpm);  // engine braking
    }
    // ratio carries the direction (negative in reverse)
    const float fDrive = torque * ratio * p.drivetrainEff / p.wheelRadius;
    if (torque - kersTorque > 0) {
        const float engOmega = c.rpm * (2 * kPi / 60.0f);
        c.fuel = std::max(0.0f, c.fuel - (torque - kersTorque) * engOmega * p.fuelPerJoule * rates.fuel * dt);
    }

    // --- aero ---
    // Downforce with its balance: the nose dives under braking and the
    // balance moves forward; a car sliding sideways loses some of its floor.
    const float dmg = damageLevel(p, c);
    const float aeroLoss = p.maxAeroLoss * dmg;
    const float sideslip = std::fabs(c.vx) > 5.0f ? std::atan(c.vy / std::fabs(c.vx)) : 0.0f;
    const float yawLoss = std::max(0.75f, 1.0f - p.aeroYawLoss * sideslip * sideslip);
    // Dirty air takes away downforce, more of it at the front (the car pushes).
    // DRS: the flap follows the request at its own speed (it closes on a brake touch or when told to).
    const bool hasDrs = p.drsDragScale < 1.0f || p.drsDownforceScale < 1.0f;
    const bool wantOpen = hasDrs && c.drsOpen && brake < 0.02f;
    c.drsFlap = clampf(c.drsFlap + (wantOpen ? dt / std::max(p.drsFlapOpenTime, dt) : -dt / std::max(p.drsFlapCloseTime, dt)), 0.0f, 1.0f);
    const float down = p.downforceCoeff * (1 - aeroLoss) * yawLoss * surf.downforceScale * c.vx * c.vx;
    const float downLossRear = (1 - p.drsDownforceScale) * c.drsFlap;  // share of the total, all taken from the rear axle
    const float balance0 = clampf(p.downforceFront - p.aeroPitchShift * c.ax / g, 0.3f, 0.6f);
    const float frontShare = balance0 * surf.frontDownforceScale;
    const float balance = frontShare / (frontShare + (1 - balance0));
    const float drag = p.dragCoeff * (1 + p.maxDragGain * dmg) * (1 + (p.drsDragScale - 1) * c.drsFlap) * surf.dragScale * c.vx * std::fabs(c.vx);

    // --- wheel loads ---
    // Static weight and downforce per axle, longitudinal transfer between the
    // axles, lateral transfer between left and right split by roll stiffness.
    // ax/ay lag behind the real accelerations like a sprung car does.
    // The road's shape changes the load: a compression presses the car into the
    // road (v^2 k), a crest lifts it; on a banked turn the cornering force has a
    // component into the road too.
    const float roadLoad = clampf(std::cos(std::atan(std::sqrt(surf.slopeX * surf.slopeX + surf.slopeY * surf.slopeY))) +
                                      (c.vx * c.vx * surf.vcurv - c.ay * std::sin(surf.bankY)) / g,
                                  0.0f, 3.0f);
    const float fzAxle0[2] = {m * g * b / L * roadLoad, m * g * a / L * roadLoad};
    // Tyre load sensitivity is measured against the dry car's static load, so
    // fuel weight costs grip as well as acceleration.
    const float fzRef[2] = {p.mass * g * b / L, p.mass * g * a / L};
    float fzAxle[2] = {fzAxle0[0] + down * balance - m * c.ax * p.cgHeight / L,
                       fzAxle0[1] + down * std::max(0.0f, 1 - balance - downLossRear) + m * c.ax * p.cgHeight / L};
    const float track[2] = {p.trackFront, p.trackRear};
    const float rollShare[2] = {p.rollStiffFront, 1 - p.rollStiffFront};
    float fz[4];
    for (int ax = 0; ax < 2; ++ax) {
        fzAxle[ax] = std::max(fzAxle[ax], 0.05f * m * g);
        // ay > 0 is a left turn: load moves to the right-hand (outside) wheels
        float shift = m * c.ay * p.cgHeight / track[ax] * rollShare[ax];
        shift = clampf(shift, -0.48f * fzAxle[ax], 0.48f * fzAxle[ax]);
        fz[2 * ax] = 0.5f * fzAxle[ax] - shift;
        fz[2 * ax + 1] = 0.5f * fzAxle[ax] + shift;
    }
    for (int w = 0; w < 4; ++w) c.wheelLoad[w] = fz[w];
    const float mu = p.tireMu * surf.muScale * (1 - p.maxGripLoss * dmg);

    // --- tyre kinematics ---
    const float cd = std::cos(c.steerAngle), sd = std::sin(c.steerAngle);
    const float vfx = c.vx, vfy = c.vy + a * c.yawRate;  // front axle velocity, body frame
    const float wfLong = vfx * cd + vfy * sd;           // ...in the wheel frame
    const float wfLat = -vfx * sd + vfy * cd;
    const float vrLat = c.vy - b * c.yawRate;
    const float minV = 2.0f;  // keeps slip angles sane near standstill
    const float alpha[2] = {std::atan(wfLat / std::max(std::fabs(wfLong), minV)),
                            std::atan(vrLat / std::max(std::fabs(c.vx), minV))};

    // Per wheel: friction (load sensitive), lateral force from the slip angle,
    // then the longitudinal demand, all inside that wheel's friction circle.
    // (each tyre's own temperature: the axle's compound and wear, then per wheel)
    const float axleGripK[2] = {p.frontGrip * compoundGrip(c.compound) * wornGrip(c.tireWear[0]),
                                p.rearGrip * compoundGrip(c.compound) * wornGrip(c.tireWear[1])};
    const float stiff[2] = {p.frontStiffness, 1.0f};
    float muW[4], fyW[4], fxW[4];
    for (int w = 0; w < 4; ++w) {
        const int ax = w / 2;
        const float fz0 = 0.5f * fzRef[ax];
        muW[w] = mu * axleGripK[ax] * tempGrip(c.compound, c.wheelTemp[w]) * std::max(0.6f, 1.0f - p.muLoadDrop * (fz[w] / fz0 - 1.0f));
        fyW[w] = tyreLateral(p, stiff[ax], alpha[ax], fz[w], fz0, muW[w]);
    }

    // brakes and rolling resistance ramp to zero at standstill so they never push backwards
    const float rampF = clampf(wfLong / 0.5f, -1, 1);
    const float rampR = clampf(c.vx / 0.5f, -1, 1);
    const float fBrake = brake * p.maxBrakeForce;
    // KERS harvest: the motor-generator does part of the rear axle's braking (the total stays what the
    // pedal asks for), limited by its power, the lap's recovery allowance and the room in the store.
    float harvestF = 0;
    if (hasKers && fBrake > 0 && c.vx > 3.0f && c.kersHarvested < p.kersHarvest && c.kersCharge < p.kersStore) {
        const float rearBrake = fBrake * (1 - p.brakeFront);
        harvestF = std::min(rearBrake, p.kersPower / c.vx);
        float power = harvestF * c.vx;
        const float room = std::min(p.kersHarvest - c.kersHarvested, (p.kersStore - c.kersCharge) / p.kersEfficiency);
        if (power * dt > room) { power = room / dt; harvestF = power / c.vx; }
        c.kersHarvested += power * dt;
        c.kersCharge = std::min(p.kersStore, c.kersCharge + power * dt * p.kersEfficiency);
        c.kersPowerNow = -power;
    }
    float brakeW[4];  // each disc's brake force, N (signed with the wheel's travel)
    for (int w = 0; w < 4; ++w) {
        const float share = w < 2 ? p.brakeFront : 1 - p.brakeFront;
        brakeW[w] = 0.5f * (fBrake * share - (w < 2 ? 0.0f : harvestF)) * brakeGrip(p, c.brakeTemp[w]) * (w < 2 ? rampF : rampR);
        fxW[w] = -brakeW[w] - (w < 2 ? 0.0f : 0.5f * harvestF * rampR) - p.rollingResist * fz[w] * (w < 2 ? rampF : rampR);
    }
    // Drive: half to each rear wheel; what a spinning wheel cannot use goes
    // partly to the other one through the limited-slip differential.
    {
        float d[2] = {0.5f * fDrive, 0.5f * fDrive};
        for (int k = 0; k < 2; ++k) {
            const int w = 2 + k, o = 2 + (1 - k);
            const float room = std::sqrt(std::max(0.0f, muW[w] * muW[w] * fz[w] * fz[w] - fyW[w] * fyW[w]));
            const float excess = std::fabs(d[k]) - room;
            if (excess > 0) {
                const float otherRoom = std::sqrt(std::max(0.0f, muW[o] * muW[o] * fz[o] * fz[o] - fyW[o] * fyW[o]));
                const float give = std::min(excess * p.diffLock, std::max(0.0f, otherRoom - std::fabs(d[1 - k])));
                const float sgn = d[k] >= 0 ? 1.0f : -1.0f;
                d[k] -= sgn * give;
                d[1 - k] += sgn * give;
            }
        }
        fxW[2] += d[0];
        fxW[3] += d[1];
    }

    // How far each axle is past its grip (gripUse > 1 = sliding or spinning):
    // the slip angle relative to the peak combined with the longitudinal demand,
    // worst wheel of the axle. wheelSpin is the rear's excess, what a traction
    // control would watch.
    const float peakBa = std::tan(kPi / (2 * p.tireC));  // B*alpha at peak lateral force
    for (int ax = 0; ax < 2; ++ax) {
        float worst = 0;
        for (int w = 2 * ax; w < 2 * ax + 2; ++w) {
            const float fz0 = 0.5f * fzRef[ax];
            const float bEff = p.tireB * stiff[ax] * std::pow(fz0 / fz[w], p.loadSens);
            const float lat = bEff * std::fabs(alpha[ax]) / peakBa;
            const float lon = fxW[w] / (muW[w] * fz[w]);
            worst = std::max(worst, std::sqrt(lat * lat + lon * lon));
        }
        c.gripUse[ax] = worst;
        c.slipAngle[ax] = alpha[ax];
    }
    c.wheelSpin = std::max(0.0f, c.gripUse[1] - 1.0f);
    for (int w = 0; w < 4; ++w) frictionCircle(fxW[w], fyW[w], muW[w] * fz[w]);

    const float fxf = fxW[0] + fxW[1], fyf = fyW[0] + fyW[1];
    const float fxr = fxW[2] + fxW[3], fyr = fyW[2] + fyW[3];

    // Tyre wear from sliding work: lateral slip speed times lateral force,
    // plus longitudinal work (more when the rear is spinning or locking).
    {
        const float v = std::fabs(c.vx);
        const float latF = std::fabs(fyf) * std::fabs(wfLat), lonF = std::fabs(fxf) * 0.03f * v;
        const float latR = std::fabs(fyr) * std::fabs(vrLat);
        const float lonR = std::fabs(fxr) * (0.03f + 0.3f * std::min(1.0f, c.wheelSpin)) * v;
        const float workF = latF + lonF, workR = latR + lonR;
        const float k = p.wearPerJoule * compoundWear(c.compound) * rates.tire * dt;
        c.tireWear[0] = std::min(1.0f, c.tireWear[0] + workF * k * tempWear(c.compound, c.tireTemp[0]));
        c.tireWear[1] = std::min(1.0f, c.tireWear[1] + workR * k * tempWear(c.compound, c.tireTemp[1]));

        // Temperature: sliding and rolling heat in, airflow out.
        const float work[2] = {latF + p.tireLonHeat * lonF, latR + p.tireLonHeat * lonR};
        // Per tyre: the axle's slide work shared by each tyre's force, rolling
        // heat by its load, the rim's heat in, airflow out. Each brake disc
        // takes its braking power and passes some on to the rim.
        const float latW[2] = {latF, latR}, lonW[2] = {p.tireLonHeat * lonF, p.tireLonHeat * lonR};
        const float wheelV[2] = {std::fabs(wfLong), v};
        const float amb = rates.ambient;
        const float cool = 0.5f * (p.tireCoolBase + p.tireCoolSpeed * v);
        const float discCool = p.brakeCoolBase + p.brakeCoolSpeed * v, rimCool = p.rimCoolBase + p.rimCoolSpeed * v;
        for (int w = 0; w < 4; ++w) {
            const int ax = w / 2, o = w ^ 1;
            const float fyS = std::fabs(fyW[w]) / std::max(1.0f, std::fabs(fyW[w]) + std::fabs(fyW[o]));
            const float fxS = std::fabs(fxW[w]) / std::max(1.0f, std::fabs(fxW[w]) + std::fabs(fxW[o]));
            const float slide = (fyS * latW[ax] + fxS * lonW[ax]) * (work[ax] > 0 ? 1.0f : 0.0f);
            float& t = c.wheelTemp[w];
            float& rim = c.rimTemp[w];
            float& disc = c.brakeTemp[w];
            const float toTyre = p.rimToTyre * (rim - t), toRim = p.brakeToRim * (disc - rim);
            const float braking = std::min(std::fabs(brakeW[w]), std::fabs(fxW[w])) * wheelV[ax];
            t += (p.tireSlideHeat * slide + p.tireRollHeat * fz[w] * v + toTyre - cool * (t - amb)) / (0.5f * p.tireHeatCap) * dt;
            rim += (toRim - toTyre - rimCool * (rim - amb)) / p.rimHeatCap * dt;
            disc += (braking - toRim - discCool * (disc - amb)) / p.brakeHeatCap * dt;
        }
        for (int ax = 0; ax < 2; ++ax) c.tireTemp[ax] = 0.5f * (c.wheelTemp[2 * ax] + c.wheelTemp[2 * ax + 1]);
    }

    // --- body forces ---
    // Front wheel forces turn with the steering; left/right differences in
    // longitudinal force (diff, brakes) add a yaw moment.
    float fx = fxf * cd - fyf * sd + fxr - drag;
    float fy = fxf * sd + fyf * cd + fyr;
    float mz = a * (fxf * sd + fyf * cd) - b * fyr + 0.5f * p.trackFront * (fxW[1] - fxW[0]) * cd +
               0.5f * p.trackRear * (fxW[3] - fxW[2]);
    fx -= surf.extraDrag * c.vx;
    fy -= surf.extraDrag * c.vy;

    // gravity along the road: uphill slows the car, a banked road pulls it to the low side
    const float axb = fx / m - g * surf.slopeX, ayb = fy / m - g * surf.slopeY;
    c.vx += (axb + c.vy * c.yawRate) * dt;
    c.vy += (ayb - c.vx * c.yawRate) * dt;
    c.yawRate += mz / yawInertia * dt;
    // The accelerations the suspension feels, lagging like springs and dampers.
    const float k = std::min(1.0f, dt / std::max(p.suspensionLag, dt));
    c.ax += (axb - c.ax) * k;
    c.ay += (ayb - c.ay) * k;

    c.pos += rotate({c.vx, c.vy}, c.yaw) * dt;
    c.yaw = wrapAngle(c.yaw + c.yawRate * dt);
    c.wheelRot += c.vx / p.wheelRadius * dt;
}

}  // namespace rr
