#pragma once
#include "rr/robot_api.h"
#include <string>

#include "vec2.hpp"

namespace rr {

// Defaults approximate a 2004-2010 Formula 1 car: ~605 kg with driver, a 19,000 rpm
// ~650 kW engine, a 7-speed seamless gearbox, carbon brakes and roughly 2.5x the
// car's weight in downforce at 300 km/h.
struct CarParams {
    float mass = 605;
    float length = 4.6f, width = 1.8f;
    float cgToFront = 1.62f, cgToRear = 1.40f;  // 46% of the static weight on the front axle
    float cgHeight = 0.27f;
    // Suspension: wheel-centre track widths, the front axle's share of the roll
    // stiffness (more at the front = more stable at the limit) and how quickly
    // load moves between the wheels (springs and dampers, as a first-order lag).
    float trackFront = 1.46f, trackRear = 1.40f;
    float rollStiffFront = 0.56f;
    float suspensionLag = 0.06f;  // s
    // Differential locking: share of the drive torque a spinning inside wheel
    // hands to the outside one (0 open diff, 1 spool).
    float diffLock = 0.55f;
    float yawInertia = 850;
    float maxSteer = 0.30f;       // rad
    float steerRate = 2.5f;       // rad/s at the road wheels
    float tireMu = 1.65f;         // slicks / grooved tyres, mechanical grip
    float tireB = 26.0f, tireC = 1.3f;  // simplified magic formula: peak slip ~6 deg static
    // Rear tyres a little stiffer and grippier than the fronts: a stable,
    // mildly understeering car, like most racing setups.
    float frontGrip = 0.96f, rearGrip = 1.04f;
    float frontStiffness = 0.85f;  // scales tireB at the front
    // Load sensitivity: cornering stiffness grows like Fz^(1 - loadSens) and
    // friction drops by muLoadDrop per extra static load.
    float loadSens = 0.3f;
    float muLoadDrop = 0.08f;
    float dragCoeff = 0.75f;      // 0.5 rho Cd A (Cd A ~ 1.2 m^2)
    float downforceCoeff = 2.3f;  // 0.5 rho Cl A (lift-to-drag ~ 3)
    float downforceFront = 0.42f; // share on the front axle (aero balance)
    float aeroPitchShift = 0.02f; // balance moves forward this much per g of braking (nose dives)
    float aeroYawLoss = 0.8f;     // downforce lost per rad^2 of sideslip (a sliding car loses its floor)
    float rollingResist = 0.015f;
    float wheelRadius = 0.33f;
    float finalDrive = 3.0f;
    int numGears = 7;
    // top speeds at the limiter: ~100, 135, 170, 205, 243, 283, 325 km/h
    float gearRatios[RR_MAX_GEARS] = {7.88f, 5.84f, 4.64f, 3.84f, 3.24f, 2.78f, 2.42f, 0};
    float reverseRatio = 8.0f;
    float idleRpm = 4000, maxRpm = 19000;
    float maxBrakeForce = 30000;  // N, carbon discs: enough to lock the wheels at speed
    float brakeFront = 0.60f;     // brake bias
    float engineBrake = 35.0f;    // N m of engine braking at max rpm, off throttle
    float drivetrainEff = 0.9f;

    // Fuel: mass is the dry car with driver; fuel adds to it.
    float fuelCapacity = 65.0f;   // litres, on every track: about 28 laps of Circuit Raylib at ~2.3 l/lap
    float fuelDensity = 0.75f;    // kg/l
    float fuelPerJoule = 1.2e-7f; // litres per joule of engine work (~25% efficient at 34 MJ/l)
    // Tyre wear: wear per joule of sliding work, before compound, temperature
    // and race multipliers.
    float wearPerJoule = 2.0e-8f;
    // Tyre temperature, one lumped temperature per axle. Heat comes from the
    // sliding work (the same work that wears the tyre) and from rolling under
    // load; the airflow takes it away, more at speed.
    float tireHeatCap = 9000.0f;     // J/K per axle
    float tireSlideHeat = 0.85f;     // share of the cornering slide work that heats the tyre
    float tireLonHeat = 0.3f;        // ...and of the braking / traction slip work
    float tireRollHeat = 0.008f;     // W per (N of load x m/s)
    float tireCoolBase = 60.0f;      // W/K per axle, standing still
    float tireCoolSpeed = 2.2f;      // extra W/K per m/s
    float blanketTemp = 80.0f;       // C: tyres come off the warmers at this temperature
    // Brakes: the discs take the braking energy and lose it to the cooling
    // ducts, and some of it soaks through the wheel rim into the tyre, slowly
    // (the rim is a big heat store, so it lags the brakes by a minute or more).
    float brakeHeatCap = 900.0f;     // J/K per disc (carbon, about 1 kg)
    float brakeCoolBase = 5.0f;      // W/K per disc, standing still
    float brakeCoolSpeed = 1.0f;     // extra W/K per m/s (the ducts)
    float brakeToRim = 25.0f;        // W/K, disc to rim
    float rimHeatCap = 8000.0f;      // J/K per wheel rim
    float rimCoolBase = 3.0f, rimCoolSpeed = 0.3f;  // W/K, and extra per m/s
    float rimToTyre = 40.0f;         // W/K, rim to tyre
    float brakeTempLo = 350.0f, brakeTempHi = 1000.0f;  // C: carbon brakes bite fully inside this window
    // Damage, growing linearly up to damageForMaxLoss: broken wings and floor
    // cost downforce and add drag, a hurt engine loses power, bent suspension
    // loses mechanical grip.
    float maxAeroLoss = 0.35f, damageForMaxLoss = 8000.0f;
    float maxDragGain = 0.10f, maxPowerLoss = 0.12f, maxGripLoss = 0.08f;
    // Development multipliers (see specs/development.json): engine output, and
    // how long this team's pit crew takes.
    float torqueScale = 1.0f;
    float pitServiceScale = 1.0f;
    // Engine: 0 the 2000s 3.0 l V10 (19,000 rpm), 1 the 2013 2.4 l V8 (18,000 rpm, 560 kW).
    float engineV8 = 0.0f;
    // 1: no refuelling in the pits (2010 onwards); a stop changes tyres and repairs only.
    float noRefuel = 0.0f;
    // 2013's energy recovery and drag reduction (all zero/1 = the car has neither).
    // KERS: a motor-generator of kersPower W on the crankshaft. Under braking it takes
    // part of the rear braking force and charges the store (kersHarvest J a lap at most,
    // 85% ends up in the store; 90% of what leaves it reaches the crank); on request it adds up to kersPower W to the engine, at
    // most kersEnergy J a lap, from a store of kersStore J. Its weight is in the car's
    // minimum mass already. DRS: the open flap scales the drag coefficient by drsDragScale
    // and the downforce by drsDownforceScale, the loss all at the rear axle.
    float kersPower = 0.0f, kersEnergy = 0.0f, kersHarvest = 0.0f, kersStore = 0.0f;
    float drsDragScale = 1.0f, drsDownforceScale = 1.0f;
    float kersMaxTorque = 200.0f;  // N m the motor-generator adds at the crank, whatever the revs
    float kersEfficiency = 0.85f;  // share of the recovered energy that reaches the store
    float kersDeployEfficiency = 0.90f;  // share of the energy drawn from the store that reaches the crank
    float drsFlapOpenTime = 0.25f, drsFlapCloseTime = 0.15f;  // s for the flap to travel
    float shiftTime = 0.0f;  // s of torque cut at each gear change (0 = instant)

    float wheelbase() const { return cgToFront + cgToRear; }
    float engineTorque(float rpm) const;  // N m at full throttle
    float maxPower() const;               // W
    RRCarSpec spec() const;

    // Every tunable number by name, for spec files and development rules.
    struct Field { const char* name; float CarParams::*ptr; };
    static const Field* fields(int* count);
    float* field(const std::string& name);
};

struct CarState {
    Vec2 pos;
    float yaw = 0;
    float vx = 0, vy = 0;   // body frame
    float yawRate = 0;
    float steerAngle = 0;   // actual road-wheel angle
    int gear = 1;
    float rpm = 0;
    float ax = 0, ay = 0;   // body-frame accelerations as the suspension feels them (load transfer)
    float wheelLoad[4] = {0, 0, 0, 0};  // N: front left, front right, rear left, rear right
    float gripUse[2] = {0, 0};
    float slipAngle[2] = {0, 0};          // front, rear: tyre force asked / available (> 1 = sliding)
    float wheelSpin = 0;
    float wheelRot = 0;     // for rendering
    float damage = 0;
    float fuel = 58;
    float tireWear[2] = {0, 0};  // front, rear
    float tireTemp[2] = {80, 80};  // C, front, rear: the mean of the axle's two tyres
    float wheelTemp[4] = {80, 80, 80, 80};      // C, each tyre: front left, front right, rear left, rear right
    float brakeTemp[4] = {300, 300, 300, 300};  // C, each disc
    float rimTemp[4] = {70, 70, 70, 70};        // C, each wheel rim
    int compound = RR_TIRE_MEDIUM;
    // KERS: J in the store, this lap's deployed and recovered energy, and the power now (+ deploying, - recovering).
    float kersCharge = 0, kersDeployed = 0, kersHarvested = 0, kersPowerNow = 0;
    // DRS: whether the flap is asked open (the race sets it) and how far it has travelled, 0 closed .. 1 open.
    bool drsOpen = false;
    float drsFlap = 0;
    float shiftLeft = 0;  // s left of the current gear change, torque cut meanwhile

    Vec2 velWorld() const { return rotate({vx, vy}, yaw); }
    void setVelWorld(Vec2 v) { Vec2 b = rotate(v, -yaw); vx = b.x; vy = b.y; }
};

struct Surface {
    int type = 0;           // RR_SURF_*
    float muScale = 1.0f;   // grip multiplier
    float extraDrag = 0.0f; // N per m/s (grass, gravel)
    // Per wheel (front left, front right, rear left, rear right), when perWheel: each tyre has the grip
    // of what it runs on (muScale is then 1) and a quarter of the surface's drag acts at that wheel,
    // so two wheels on grass pull the car round and slow it unevenly.
    bool perWheel = false;
    float wheelMu[4] = {1, 1, 1, 1};
    float wheelDrag[4] = {0, 0, 0, 0};  // N per m/s at each wheel
    float dragScale = 1.0f; // < 1 in another car's slipstream
    float downforceScale = 1.0f, frontDownforceScale = 1.0f;  // < 1 in another car's dirty air
    // The road's 3D shape under the car (0 on a flat track):
    float slopeX = 0, slopeY = 0;  // height gained per metre along the car's x (forward) and y (left)
    float bankY = 0;               // the road's roll about the car's x axis, rad (+ = left side higher)
    float vcurv = 0;               // vertical curvature along the car's path, 1/m (+ = compression)
};

// Race-wide multipliers (command line) for consumables.
struct WearRates {
    float fuel = 1.0f;
    float tire = 1.0f;
    float ambient = 25.0f;  // C, air and track
};

// Tyre compounds: grip of a new tyre, wear rate and the temperature window
// where the tyre works best (C).
struct Compound {
    float grip, wear, tempLo, tempHi;
};
const Compound& compoundInfo(int compound);

float compoundGrip(int compound);  // grip multiplier of a new tyre
float compoundWear(int compound);  // wear-rate multiplier
float wornGrip(float wear);        // grip multiplier from wear (1 when new, cliff past 0.7)
float tempGrip(int compound, float temp);  // grip multiplier from temperature (1 inside the window)
float tempWear(int compound, float temp);
float brakeGrip(const CarParams& p, float temp);  // brake force multiplier from disc temperature (1 inside the window)  // wear multiplier from temperature (1 inside the window)
float axleGrip(const CarState& c, int axle);  // compound x wear x temperature, 0 front / 1 rear
float carMass(const CarParams& p, const CarState& c);  // including fuel
float damageLevel(const CarParams& p, const CarState& c);  // 0 intact .. 1 at damageForMaxLoss

// One fixed physics step of a planar dynamic bicycle model with load
// transfer, aero, a simple engine/gearbox and friction-circle tyres.
void stepCar(CarState& c, const CarParams& p, const RRControl& in, bool autoGear,
             const Surface& surf, const WearRates& rates, float dt);

}  // namespace rr
