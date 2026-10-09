#pragma once
#include <cstdio>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "car.hpp"
#include "config.hpp"
#include "robot_driver.hpp"
#include "robot_loader.hpp"
#include "track.hpp"

namespace rr {

struct Car {
    // identity
    std::string name;
    std::string robotName;
    std::string params;
    std::shared_ptr<RobotDriver> driver;  // the robot, in this process or sandboxed
    RRRobotConfig robotCfg{};
    // CPU the robot used in drive(), s (wall time on Windows), and calls over the cap
    double cpuTotal = 0, cpuMax = 0;
    long long driveCalls = 0;
    int cpuOverruns = 0;
    std::shared_ptr<std::vector<unsigned char>> memory;  // the weekend memory the robot writes to
    bool sessionEnded = false;  // session_end() called

    // physics
    CarParams phys{};
    CarState state{};
    RRControl control{};
    RRSensors sensors{};

    // track position
    int trackIdx = 0;
    float trackS = 0;
    float lateral = 0;
    float halfWidth = 7;
    bool onTrack = true;
    int surface = 0;  // RR_SURF_* under the centre

    // race progress
    double distRaced = 0;
    int lapsDone = 0;
    double lapStart = 0;
    std::vector<float> lapTimes;
    float bestLap = 0;
    bool finished = false;
    double finishTime = 0;
    bool dnf = false;
    std::string dnfReason;
    int position = 0;
    double gap = 0;        // seconds behind the leader at the same point of the track, -1 if not timed yet
    int lapsBehind = 0;
    int collisions = 0;
    float stuckTime = 0;
    std::vector<double> checkpoints;  // race time at every 10 m of distance

    // consumables and pit
    int lapsOnTires = 0;
    int pitState = RR_PIT_NONE;
    int pitStops = 0;
    float pitBoxS = 0;
    float serviceLeft = 0;
    RRControl pitOrder{};       // the request as it was when service started
    double pitLaneTime = 0;     // total time spent in the pit lane
    std::vector<int> pitLaps;   // lap on which each stop happened
    // What happened at each stop, for the race log.
    struct StopLog {
        int lap = 0;
        double time = 0;
        float fuelBefore = 0, fuelAdded = 0, wear[2] = {0, 0}, damage = 0, service = 0;
        int tiresBefore = 0, tiresFitted = 0;  // RR_TIRE_*, 0 = kept
        bool repair = false;
        std::string reason;                    // the robot's status text as it stopped
    };
    std::vector<StopLog> stopLog;
    std::vector<int> lapPositions;  // race position at the end of each lap
    struct PenaltyLog {
        double time = 0;
        int lap = 0;
        float seconds = 0;
        std::string reason;
    };
    std::vector<PenaltyLog> penaltyLog;
    // Tyre temperatures over each completed lap: average and peak, front and rear (C).
    struct LapTemps {
        float avg[2] = {0, 0}, max[2] = {0, 0};
    };
    std::vector<LapTemps> lapTemps;
    double tempSum[2] = {0, 0};
    float tempMax[2] = {0, 0};
    long tempN = 0;
    float noFuelTime = 0;

    // blue flags and penalties
    int blueCar = -1;           // car lapping us, close behind (-1 = no blue flag)
    float blueDs = 0;
    float blueHeld = 0;         // s spent holding that car up
    int blueFlags = 0;          // times a blue flag was shown
    int penalties = 0;
    float penaltyTime = 0;      // s added to the race time
    double raceTime() const { return finishTime + penaltyTime; }

    // after the flag: a cool-down lap into the pit lane
    bool parked = false;
    int compoundsUsed = 0;      // bit (1 << RR_TIRE_*) per compound fitted
    bool startTiresSet = false; // the team chose the starting tyres
    bool twoCompoundPenalty = false;
    float draft = 0, dirtyAir = 0;  // drag and downforce lost to other cars' wakes, fractions
    int parkSlot = -1;          // spot in the pit lane, 0 = furthest down

    // DRS: per zone, whether this car earned it at the detection point, and when it last crossed
    // that point (-1 never); the gap to the car ahead then; the zone we are in; what the robots see.
    std::vector<char> drsEligible;
    std::vector<double> drsDetectTime;
    float drsPrevS = -1;
    float drsGap = -1;
    int drsZone = -1, drsNextZone = -1, drsLastZone = -1;
    float drsNextDs = 0;
    int drsState = RR_DRS_NONE;

    FILE* telemetry = nullptr;

    int currentLap(int raceLaps) const { return std::min(raceLaps, std::max(1, lapsDone + 1)); }
};

// A car-to-car impact, for analysis (and later, stewarding).
struct Contact {
    double time;
    int a, b;          // a is the car behind (by track distance) at the moment of contact
    float speed;       // closing speed along the contact normal, m/s
    float ds;          // track distance from a to b, m (+ = b ahead)
    float lateral;     // b's lateral minus a's, m
    float relYaw;      // b's heading minus a's, rad
};

class Race {
public:
    Race() = default;
    ~Race();
    Race(const Race&) = delete;
    Race& operator=(const Race&) = delete;

    // botDirs/trackDirs are searched for robot libraries and track files.
    bool setup(const RaceConfig& cfg, const std::vector<std::string>& botDirs,
               const std::vector<std::string>& trackDirs, std::string* err);

    void step();                // one physics step of cfg.dt
    void advance(double seconds);  // as many steps as fit
    bool isOver() const { return over_; }           // results are final
    // After the flag the cars drive a cool-down lap into the pit lane and park;
    // step() keeps simulating that until this is true.
    bool cooledDown() const;

    double time() const { return time_; }
    float dt() const { return cfg_.dt; }
    int laps() const { return cfg_.laps; }
    const Track& track() const { return track_; }
    const std::vector<Car>& cars() const { return cars_; }
    // For replays in the viewer only: overwrite a car's pose to show a recorded
    // moment, and restore it before the next step().
    std::vector<Car>& carsForReplay() { return cars_; }
    const std::vector<int>& order() const { return order_; }  // car indices by position
    const RaceConfig& config() const { return cfg_; }
    const std::vector<Contact>& contacts() const { return contacts_; }
    // Two different compounds must be used (cfg.twoCompounds; automatic: races over 20 laps).
    bool twoCompoundRule() const { return twoCompoundRuleFor(cfg_); }
    static bool twoCompoundRuleFor(const RaceConfig& c) { return c.twoCompounds > 0 || (c.twoCompounds < 0 && c.laps > 20); }

    void printResults(FILE* out) const;
    bool writeJson(const std::string& path, double wallSeconds) const;

private:
    void placeOnGrid();
    void callRobots();
    void computeSensors(Car& c);
    void resolveWalls(Car& c);
    void resolveCarPair(Car& a, Car& b);
    void updateProgress(Car& c);
    void updateOrder();
    void writeTelemetry(const Car& c);
    void updatePit(Car& c);
    void updateBlueFlags();
    void updateDrs(Car& c);
    RRControl coolDownControl(Car& c);
    void endSession(Car& c);
    void retire(Car& c, const std::string& why);
    static constexpr int kMaxCpuOverruns = 50;  // drive() calls over the CPU cap before the car is out
    void wake(Car& c) const;  // slipstream and dirty air behind other cars
    void finishService(Car& c);
    float wrapDs(float ds) const;

    RaceConfig cfg_;
    Track track_;
    std::vector<Car> cars_;
    std::vector<int> order_;
    double time_ = 0;
    long long steps_ = 0;
    int robotPeriod_ = 10;
    bool over_ = false;
    double leaderFinish_ = -1;
    double overTime_ = 0;
    std::vector<Contact> contacts_;
    int parkedSlots_ = 0;
    double maxTime_ = 0;
    std::mt19937_64 rng_;
};

// Path of a track by name (tracks/<name>.trk) or path, "" if not found.
std::string trackFile(const std::string& name, const std::vector<std::string>& dirs);

}  // namespace rr
