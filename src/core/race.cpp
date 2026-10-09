#include "race.hpp"

#include <algorithm>
#include <cstring>
#include <filesystem>

#include "spec.hpp"

namespace fs = std::filesystem;

namespace rr {

namespace {

const float kDefaultSensorAngles[RR_NUM_TRACK_SENSORS] = {-90, -75, -60, -45, -30, -20, -15, -10, -5, 0,
                                                          5,   10,  15,  20,  30,  45,  60,  75,  90};
const float kCheckpointSpacing = 10.0f;
const float kRestitutionWall = 0.25f;
const float kRestitutionCar = 0.2f;
const float kDraftMax = 0.45f, kDraftLength = 60.0f, kDraftWidth = 3.5f;  // slipstream
const float kDirtyMax = 0.10f, kDirtyLength = 40.0f, kDirtyWidth = 3.0f;  // dirty air: downforce lost
const float kBoxSpacing = 14.0f, kFirstBox = 25.0f;
const float kServiceBase = RR_PIT_SERVICE_BASE;
const float kFuelFlow = RR_PIT_FUEL_RATE;
const float kTireChange = RR_PIT_TIRE_CHANGE;
const float kRepairPer1000 = RR_PIT_REPAIR_PER_1000;

float yawInertia(const Car& c) { return c.phys.yawInertia; }  // fuel sits at the CG

float cross2(Vec2 r, Vec2 n) { return r.x * n.y - r.y * n.x; }
Vec2 angVel(float w, Vec2 r) { return {-w * r.y, w * r.x}; }

std::string findTrack(const std::string& t, const std::vector<std::string>& dirs) {
    std::error_code ec;
    if (fs::exists(t, ec) && !fs::is_directory(t, ec)) return t;
    for (const auto& d : dirs) {
        for (const auto& cand : {t, t + ".trk"}) {
            fs::path p = fs::path(d) / cand;
            if (fs::exists(p, ec) && !fs::is_directory(p, ec)) return p.string();
        }
    }
    return "";
}

std::string fmtTime(double t) {
    if (t <= 0) return "-";
    int m = (int)(t / 60);
    char buf[32];
    std::snprintf(buf, sizeof buf, "%d:%06.3f", m, t - m * 60);
    return buf;
}

std::string jsonEscape(const std::string& s) {
    std::string o;
    for (char ch : s) {
        if (ch == '"' || ch == '\\') { o += '\\'; o += ch; }
        else if ((unsigned char)ch < 0x20) o += ' ';
        else o += ch;
    }
    return o;
}

}  // namespace

std::string trackFile(const std::string& name, const std::vector<std::string>& dirs) { return findTrack(name, dirs); }


// Tells the robot its session is over (once), with how it went.
void Race::endSession(Car& c) {
    if (c.sessionEnded || !c.driver) return;
    c.sessionEnded = true;
    RRSessionSummary s{};
    s.session = cfg_.session;
    s.laps_done = c.lapsDone;
    s.best_lap = c.bestLap;
    s.total_time = (float)time_;
    s.finished = c.finished;
    for (size_t i = 0; i < c.lapTimes.size() && i < 64; ++i) s.lap_times[i] = c.lapTimes[i];
    s.tire_compound = c.state.compound;
    s.tire_wear[0] = c.state.tireWear[0];
    s.tire_wear[1] = c.state.tireWear[1];
    s.fuel = c.state.fuel;
    c.driver->sessionEnd(s, c.memory.get());
}

Race::~Race() {
    for (auto& c : cars_) {
        endSession(c);
        c.driver.reset();  // destroy()
        if (c.telemetry) std::fclose(c.telemetry);
    }
}

bool Race::setup(const RaceConfig& cfg, const std::vector<std::string>& botDirs,
                 const std::vector<std::string>& trackDirs, std::string* err) {
    cfg_ = cfg;
    rng_.seed(cfg.seed);
    robotPeriod_ = std::max(1, (int)std::lround(1.0 / (cfg.robotHz * cfg.dt)));

    std::string trackPath = findTrack(cfg.track, trackDirs);
    if (trackPath.empty()) {
        if (err) *err = "track '" + cfg.track + "' not found";
        return false;
    }
    if (!track_.load(trackPath, err)) return false;
    if (cfg.entries.empty()) {
        if (err) *err = "no cars: add at least one --car";
        return false;
    }

    // Car specs and development rules live in specs/ next to tracks/.
    std::vector<std::string> specDirs;
    for (const auto& d : trackDirs) specDirs.push_back((fs::path(d).parent_path() / "specs").string());
    specDirs.push_back("specs");
    DevRules rules;
    bool needRules = false;
    for (const auto& e : cfg.entries) needRules = needRules || !e.dev.empty();
    if (needRules) {
        std::string path = findDataFile(cfg.devRules, specDirs);
        if (path.empty()) {
            if (err) *err = "development rules '" + cfg.devRules + "' not found";
            return false;
        }
        if (!loadDevRules(path, rules, err)) return false;
    }

    cars_.resize(cfg.entries.size());
    for (size_t i = 0; i < cfg.entries.size(); ++i) {
        const auto& e = cfg.entries[i];
        Car& c = cars_[i];
        const std::string specName = e.spec.empty() ? cfg.carSpec : e.spec;
        if (!specName.empty()) {
            std::string path = findDataFile(specName, specDirs);
            if (path.empty()) {
                if (err) *err = "car spec '" + specName + "' not found";
                return false;
            }
            if (!loadCarSpec(path, c.phys, err)) return false;
        }
        if (!e.dev.empty()) {
            std::vector<int> tokens;
            std::string why;
            if (!parseDevelopment(rules, e.dev, tokens, &why)) {
                if (err) *err = "car " + std::to_string(i) + ": " + why;
                return false;
            }
            applyDevelopment(rules, tokens, c.phys);
        }
        if (cfg.sandbox) {
            const std::string lib = RobotModule::find(e.robot, botDirs);
            if (lib.empty()) {
                if (err) *err = "robot '" + e.robot + "' not found (looked in the bots directory and as a path)";
                return false;
            }
            c.driver = RobotDriver::sandbox(cfg.botHost, lib, err);
            if (!c.driver) return false;
            c.driver->setHangTimeout(std::max(2.0, 200.0 * cfg.cpuCapMs / 1000.0));
        } else {
            auto mod = RobotModule::load(e.robot, botDirs, err);
            if (!mod) return false;
            c.driver = RobotDriver::inProcess(mod);
        }
        c.robotName = c.driver->name().empty() ? e.robot : c.driver->name();
        c.name = e.name.empty() ? c.robotName : e.name;
        c.params = e.params;
        std::memcpy(c.robotCfg.track_sensor_angles, kDefaultSensorAngles, sizeof kDefaultSensorAngles);
        c.robotCfg.auto_gear = 1;
        c.robotCfg.initial_fuel = c.phys.fuelCapacity;
        c.robotCfg.tire_compound = e.tires ? e.tires : RR_TIRE_MEDIUM;
        c.robotCfg.race_laps = cfg.laps;
        c.robotCfg.two_compound_rule = twoCompoundRuleFor(cfg);
        c.robotCfg.fuel_rate = cfg.fuelRate;
        c.robotCfg.wear_rate = cfg.wearRate;
        c.robotCfg.ambient_temp = cfg.ambient;
        c.robotCfg.starting_compound_set = e.tires != 0;
        c.robotCfg.pits_closed = cfg.pitsClosed;
        // with no refuelling the starting load is fixed by the rules: a full tank
        c.robotCfg.starting_fuel_set = e.fuel > 0 || c.phys.noRefuel > 0.5f;
        if (e.fuel > 0) c.robotCfg.initial_fuel = std::min(e.fuel, c.phys.fuelCapacity);
        c.robotCfg.session = cfg.session;
        c.robotCfg.session_laps = cfg.laps;
        c.memory = e.memory ? e.memory : newWeekendMemory();
        if ((int)c.memory->size() < RR_SESSION_MEMORY) c.memory->resize(RR_SESSION_MEMORY, 0);
        c.robotCfg.memory = c.memory->data();
        c.robotCfg.memory_size = RR_SESSION_MEMORY;
        RRCarSpec spec = c.phys.spec();
        if (!c.driver->create(track_.info(), spec, (int)i, c.params, c.robotCfg, err)) {
            if (err && err->empty())
                *err = "robot '" + c.robotName + "' refused car " + std::to_string(i) + " (params: \"" + c.params + "\")";
            return false;
        }
        c.robotCfg.initial_fuel = clampf(c.robotCfg.initial_fuel, 0.0f, c.phys.fuelCapacity);
        if (c.phys.noRefuel > 0.5f) c.robotCfg.initial_fuel = c.phys.fuelCapacity;
        if (cfg.fuelLimit > 0) c.robotCfg.initial_fuel = std::min(c.robotCfg.initial_fuel, cfg.fuelLimit);
        if (c.robotCfg.tire_compound < RR_TIRE_SOFT || c.robotCfg.tire_compound > RR_TIRE_HARD)
            c.robotCfg.tire_compound = RR_TIRE_MEDIUM;
        if (e.tires) c.robotCfg.tire_compound = e.tires;  // the team's call wins
        if (e.fuel > 0) c.robotCfg.initial_fuel = std::min(e.fuel, c.phys.fuelCapacity);
        c.startTiresSet = e.tires != 0;
    }
    // Make duplicate names unique: "simple", "simple #2", ...
    for (size_t i = 0; i < cars_.size(); ++i) {
        int dup = 1;
        for (size_t j = 0; j < i; ++j)
            if (cfg.entries[j].name.empty() && cars_[j].robotName == cars_[i].robotName) ++dup;
        if (dup > 1 && cfg.entries[i].name.empty()) cars_[i].name += " #" + std::to_string(dup);
    }

    if (!cfg.telemetryDir.empty()) {
        std::error_code ec;
        fs::create_directories(cfg.telemetryDir, ec);
        for (size_t i = 0; i < cars_.size(); ++i) {
            std::string fname = cfg.telemetryDir + "/car" + std::to_string(i) + ".csv";
            cars_[i].telemetry = std::fopen(fname.c_str(), "w");
            if (!cars_[i].telemetry) {
                if (err) *err = "cannot write " + fname;
                return false;
            }
            std::fprintf(cars_[i].telemetry,
                         "time,x,y,yaw,speed,vx,vy,yaw_rate,steer,accel,brake,gear,rpm,track_pos,angle,"
                         "dist_raced,lap,on_track,wheel_spin,fuel,wear_front,wear_rear,tire_grip,damage,pit_state,"
                         "grip_front,grip_rear,slip_front,slip_rear,accel_x,accel_y,blue_flag,temp_front,temp_rear,"
                         "axle_grip_front,axle_grip_rear,slipstream,dirty_air,d0,d1,d2,d3,d4,d5,d6,d7,"
                         "tyre_fl,tyre_fr,tyre_rl,tyre_rr,brake_fl,brake_fr,brake_rl,brake_rr,"
                         "kers_store,kers_power,kers_deploy_left,kers_request,drs_state,drs_flap,drs_request\n");
        }
    }

    maxTime_ = cfg.maxTime > 0 ? cfg.maxTime : cfg.laps * (track_.length() / 12.0) + 120.0;
    placeOnGrid();
    updateOrder();
    return true;
}

void Race::placeOnGrid() {
    for (size_t i = 0; i < cars_.size(); ++i) {
        Car& c = cars_[i];
        float s = -(10.0f + 7.0f * (float)i);
        float side = (i % 2 == 0) ? 1.0f : -1.0f;
        float lat = side * track_.at(track_.indexAt(s)).halfWidth * 0.4f;
        Vec2 d = track_.dirAt(s);
        c.state = CarState{};
        c.state.pos = track_.pointAt(s, lat);
        c.state.yaw = std::atan2(d.y, d.x);
        c.state.fuel = c.robotCfg.initial_fuel;
        c.state.compound = c.robotCfg.tire_compound;
        c.state.kersCharge = std::min(c.phys.kersEnergy, c.phys.kersStore);  // a lap's worth from the formation lap
        c.compoundsUsed = 1 << c.state.compound;
        for (float& t : c.state.wheelTemp) t = c.phys.blanketTemp;
        c.state.tireTemp[0] = c.state.tireTemp[1] = c.phys.blanketTemp;
        TrackLoc loc = track_.locateGlobal(c.state.pos);
        c.trackIdx = loc.idx;
        c.trackS = loc.s;
        c.lateral = loc.lateral;
        c.halfWidth = loc.halfWidth;
        c.distRaced = s;
        c.control = RRControl{};
        c.control.gear = 1;
        if (track_.hasPit()) {
            // Boxes from the start of the lane, 14 m apart; cars share them if the lane is short.
            const RRPitInfo& p = track_.pit();
            float laneLen = std::fmod(p.lane_end_s - p.lane_start_s + track_.length(), track_.length());
            int boxes = std::max(1, (int)((laneLen - 2 * kFirstBox) / kBoxSpacing) + 1);
            float bs = p.lane_start_s + kFirstBox + kBoxSpacing * (float)(i % boxes);
            c.pitBoxS = std::fmod(bs, track_.length());
        }
    }
}

float Race::wrapDs(float ds) const {
    const float L = track_.length();
    ds = std::fmod(ds, L);
    if (ds > L * 0.5f) ds -= L;
    if (ds <= -L * 0.5f) ds += L;
    return ds;
}

void Race::computeSensors(Car& c) {
    RRSensors& s = c.sensors;
    const CarState& st = c.state;
    const double prevTime = s.time;
    std::memset(&s, 0, sizeof s);
    s.time = time_;
    s.dt = (float)(time_ - prevTime);

    Vec2 td = track_.dirAt(c.trackS);
    s.angle = wrapAngle(st.yaw - std::atan2(td.y, td.x));
    s.track_pos = c.lateral / c.halfWidth;
    s.on_track = c.onTrack ? 1 : 0;

    std::normal_distribution<float> noise(0.0f, cfg_.sensorNoise);
    for (int k = 0; k < RR_NUM_TRACK_SENSORS; ++k) {
        if (!c.onTrack) { s.track[k] = -1; continue; }
        float ang = st.yaw + c.robotCfg.track_sensor_angles[k] * (kPi / 180.0f);
        float d = track_.raycastEdge(st.pos, fromAngle(ang), RR_SENSOR_RANGE);
        if (cfg_.sensorNoise > 0) d = clampf(d * (1.0f + noise(rng_)), 0.0f, RR_SENSOR_RANGE);
        s.track[k] = d;
    }

    for (float& o : s.opponents) o = RR_SENSOR_RANGE;
    for (const Car& other : cars_) {
        if (&other == &c) continue;
        Vec2 d = other.state.pos - st.pos;
        float dist = length(d);
        if (dist >= RR_SENSOR_RANGE) continue;
        float rel = wrapAngle(std::atan2(d.y, d.x) - st.yaw);
        int sector = (int)std::floor((rel + kPi) / (2 * kPi / RR_NUM_OPPONENT_SENSORS));
        sector = std::max(0, std::min(RR_NUM_OPPONENT_SENSORS - 1, sector));
        s.opponents[sector] = std::min(s.opponents[sector], dist);
    }

    s.grip_use[0] = st.gripUse[0];
    s.grip_use[1] = st.gripUse[1];
    s.slip_angle[0] = st.slipAngle[0];
    s.slip_angle[1] = st.slipAngle[1];
    s.accel_x = st.ax;
    s.accel_y = st.ay;
    for (int w = 0; w < 4; ++w) s.wheel_load[w] = st.wheelLoad[w];
    s.blue_flag = c.blueCar >= 0;
    s.blue_flag_car = c.blueCar;
    s.blue_flag_ds = c.blueCar >= 0 ? c.blueDs : 0.0f;
    s.penalties = c.penalties;
    s.penalty_time = c.penaltyTime;

    // The timing screen, in race order.
    s.num_timing = std::min((int)order_.size(), RR_MAX_CARS);
    for (int p = 0; p < s.num_timing; ++p) {
        const Car& o = cars_[order_[p]];
        RRTimingEntry& t = s.timing[p];
        t.car_index = order_[p];
        t.race_pos = o.position;
        t.laps_done = o.lapsDone;
        t.gap_to_leader = (float)o.gap;
        t.gap = (o.gap >= 0 && c.gap >= 0) ? (float)(c.gap - o.gap) : 0.0f;
        // (gaps to the leader are taken at the same point of the track, so laps are included)
        t.dist_raced = (float)o.distRaced;
        t.last_lap = o.lapTimes.empty() ? 0.0f : o.lapTimes.back();
        t.best_lap = o.bestLap;
        t.pit_state = o.pitState;
        t.pit_stops = o.pitStops;
        t.tire_compound = o.state.compound;
        t.laps_on_tires = o.lapsOnTires;
        t.compounds_used = o.compoundsUsed;
        t.finished = o.finished;
        t.dnf = o.dnf;
    }
    s.two_compound_rule = twoCompoundRule();
    s.compounds_used = c.compoundsUsed;
    s.starting_compound_set = c.startTiresSet;
    s.pits_closed = cfg_.pitsClosed;
    s.session = cfg_.session;
    {
        const int t = track_.turnAt(c.trackS);
        float ds = 0;
        const int nx = track_.nextTurn(c.trackS, &ds);
        s.turn = t >= 0 ? track_.turns()[t].id : 0;
        s.next_turn = nx >= 0 ? track_.turns()[nx].id : 0;
        s.next_turn_ds = nx >= 0 ? ds : 0;
    }

    s.speed_x = st.vx;
    s.speed_y = st.vy;
    s.yaw_rate = st.yawRate;
    s.rpm = st.rpm;
    s.gear = st.gear;
    s.wheel_spin = st.wheelSpin;
    s.damage = st.damage;
    s.dist_from_start = c.trackS;
    {
        float grade, bank;
        track_.shapeAt(c.trackS, &grade, &bank, nullptr);
        s.z = track_.heightAt(c.trackS, c.lateral);
        s.grade = grade;
        s.bank = bank;
    }
    s.dist_raced = (float)c.distRaced;
    s.lap = c.currentLap(cfg_.laps);
    s.race_laps = cfg_.laps;
    s.race_pos = c.position;
    s.num_cars = (int)cars_.size();
    s.cur_lap_time = (float)(time_ - c.lapStart);
    s.last_lap_time = c.lapTimes.empty() ? 0.0f : c.lapTimes.back();
    s.best_lap_time = c.bestLap;
    s.x = st.pos.x;
    s.y = st.pos.y;
    s.yaw = st.yaw;
    s.track_index = c.trackIdx;

    s.fuel = st.fuel;
    s.tire_wear[0] = st.tireWear[0];
    s.tire_wear[1] = st.tireWear[1];
    s.tire_grip = 0.5f * (compoundGrip(st.compound) * (wornGrip(st.tireWear[0]) + wornGrip(st.tireWear[1])));
    const Compound& comp = compoundInfo(st.compound);
    for (int ax = 0; ax < 2; ++ax) {
        s.tire_temp[ax] = st.tireTemp[ax];
        s.axle_grip[ax] = axleGrip(st, ax);
    }
    for (int w = 0; w < 4; ++w) {
        s.tire_temp_wheel[w] = st.wheelTemp[w];
        s.brake_temp[w] = st.brakeTemp[w];
    }
    s.brake_temp_window[0] = c.phys.brakeTempLo;
    s.brake_temp_window[1] = c.phys.brakeTempHi;
    s.tire_temp_window[0] = comp.tempLo;
    s.tire_temp_window[1] = comp.tempHi;
    s.ambient_temp = cfg_.ambient;
    s.slipstream = c.draft;
    s.dirty_air = c.dirtyAir;
    s.tire_compound = st.compound;
    s.laps_on_tires = c.lapsOnTires;
    s.pit_state = c.pitState;
    s.pit_stops = c.pitStops;
    s.pit_box_s = c.pitBoxS;
    s.service_time_left = c.pitState == RR_PIT_SERVICE ? c.serviceLeft : 0.0f;

    if (c.phys.kersPower > 0) {
        s.kers_store = st.kersCharge;
        s.kers_deploy_left = std::max(0.0f, c.phys.kersEnergy - st.kersDeployed);
        s.kers_harvest_left = std::max(0.0f, c.phys.kersHarvest - st.kersHarvested);
        s.kers_power = st.kersPowerNow;
    }
    s.drs_state = c.drsState;
    s.drs_open = st.drsOpen ? 1 : 0;
    s.drs_zone = c.drsZone;
    s.drs_next_zone = c.drsNextZone;
    s.drs_next_ds = c.drsNextDs;
    s.drs_gap = c.drsGap;

    // Nearby cars, nearest first by track distance.
    struct Cand { float key; int idx; float ds; };
    Cand cand[64];
    int nc = 0;
    for (size_t j = 0; j < cars_.size() && nc < 64; ++j) {
        const Car& o = cars_[j];
        if (&o == &c) continue;
        float ds = wrapDs(o.trackS - c.trackS);
        cand[nc++] = {std::fabs(ds), (int)j, ds};
    }
    std::sort(cand, cand + nc, [](const Cand& a, const Cand& b) { return a.key < b.key || (a.key == b.key && a.idx < b.idx); });
    s.num_nearby = std::min(nc, RR_MAX_NEARBY);
    const float cy = std::cos(-st.yaw), sy = std::sin(-st.yaw);
    for (int k = 0; k < s.num_nearby; ++k) {
        const Car& o = cars_[cand[k].idx];
        RROpponent& r = s.nearby[k];
        Vec2 d = o.state.pos - st.pos;
        r.car_index = cand[k].idx;
        r.race_pos = o.position;
        r.ds = cand[k].ds;
        r.lateral = o.lateral;
        r.speed = o.state.vx;
        r.rel_x = d.x * cy - d.y * sy;
        r.rel_y = d.x * sy + d.y * cy;
        r.rel_yaw = wrapAngle(o.state.yaw - st.yaw);
        r.pit_state = o.pitState;
        r.laps_ahead = (int)std::floor(o.distRaced / track_.length()) - (int)std::floor(c.distRaced / track_.length());
    }
}

// Blue flags: a car about to lap us is close behind. Holding it up for too
// long costs a time penalty.
void Race::updateBlueFlags() {
    if (over_) {
        for (Car& c : cars_) c.blueCar = -1;
        return;
    }
    const float L = track_.length();
    const float tick = cfg_.dt * robotPeriod_;
    for (size_t i = 0; i < cars_.size(); ++i) {
        Car& c = cars_[i];
        int was = c.blueCar;
        c.blueCar = -1;
        if (c.finished || c.dnf || c.pitState != RR_PIT_NONE || c.distRaced < 0) {
            c.blueHeld = 0;
            continue;
        }
        float bestDs = -1e9f;
        for (size_t j = 0; j < cars_.size(); ++j) {
            const Car& o = cars_[j];
            if (j == i || o.dnf || o.finished || o.pitState != RR_PIT_NONE) continue;
            if (o.distRaced < c.distRaced + 0.5 * L) continue;  // not lapping us
            float ds = wrapDs(o.trackS - c.trackS);
            float range = std::max(RR_BLUE_FLAG_RANGE, 1.2f * o.state.vx);
            if (ds > 0 || ds < -range || ds < bestDs) continue;
            bestDs = ds;
            c.blueCar = (int)j;
        }
        if (c.blueCar < 0) {
            c.blueHeld = 0;
            continue;
        }
        c.blueDs = bestDs;
        if (c.blueCar != was) {
            c.blueFlags++;
            c.blueHeld = 0;
        }
        // The clock runs while the lapping car is stuck right behind us.
        if (bestDs > -30.0f) c.blueHeld += tick;
        if (c.blueHeld > RR_BLUE_FLAG_LIMIT) {
            c.penalties++;
            c.penaltyTime += RR_BLUE_FLAG_PENALTY;
            char why[160];
            std::snprintf(why, sizeof why, "blue flag: held up %s (lapping it) for over %.0f s",
                          cars_[c.blueCar].name.c_str(), (double)RR_BLUE_FLAG_LIMIT);
            c.penaltyLog.push_back({time_, c.currentLap(cfg_.laps), RR_BLUE_FLAG_PENALTY, why});
            c.blueHeld = -1e9f;  // one penalty per car held up
        }
    }
}

// The robot is out (crashed, hung, over the CPU limit): the car stops where it is.
void Race::retire(Car& c, const std::string& why) {
    c.dnf = true;
    c.dnfReason = why;
    c.control = RRControl{};
    c.control.brake = 1;
    c.control.gear = c.state.gear;
}

void Race::callRobots() {
    updateBlueFlags();
    // Ask every robot first, then collect the answers: sandboxed robots think at the same time.
    for (Car& c : cars_) {
        if (c.dnf) continue;
        computeSensors(c);
        RRControl in{};
        in.gear = c.state.gear;
        c.driver->beginDrive(c.sensors, in);
    }
    for (Car& c : cars_) {
        if (c.dnf) {
            c.control = RRControl{};
            c.control.brake = 1;
            c.control.gear = c.state.gear;
            continue;
        }
        RRControl ctl{};
        const DriveResult res = c.driver->endDrive(ctl);
        if (res.failed) {
            retire(c, res.why);
            continue;
        }
        c.cpuTotal += res.cpu;
        c.cpuMax = std::max(c.cpuMax, res.cpu);
        ++c.driveCalls;
        // Over the CPU cap: this answer is too late, the car keeps its last controls.
        if (cfg_.cpuCapMs > 0 && res.cpu * 1000.0 > cfg_.cpuCapMs) {
            ++c.cpuOverruns;
            if (c.cpuOverruns > kMaxCpuOverruns) {
                retire(c, "over the CPU limit");
                continue;
            }
            if (c.telemetry) writeTelemetry(c);
            continue;
        }
        ctl.status[sizeof ctl.status - 1] = 0;
        if (!std::isfinite(ctl.steer)) ctl.steer = 0;
        if (!std::isfinite(ctl.accel)) ctl.accel = 0;
        if (!std::isfinite(ctl.brake)) ctl.brake = 0;
        if (!std::isfinite(ctl.kers)) ctl.kers = 0;
        c.control = ctl;
        if (c.telemetry) writeTelemetry(c);
    }
}

void Race::writeTelemetry(const Car& c) {
    const auto& s = c.sensors;
    const auto& k = c.control;
    std::fprintf(c.telemetry, "%.3f,%.3f,%.3f,%.4f,%.3f,%.3f,%.3f,%.4f,%.4f,%.3f,%.3f,%d,%.0f,%.4f,%.4f,%.2f,%d,%d,%.3f,%.3f,%.4f,%.4f,%.4f,%.0f,%d",
                 s.time, s.x, s.y, s.yaw, std::sqrt(s.speed_x * s.speed_x + s.speed_y * s.speed_y), s.speed_x,
                 s.speed_y, s.yaw_rate, k.steer, k.accel, k.brake, s.gear, s.rpm, s.track_pos, s.angle, s.dist_raced,
                 s.lap, s.on_track, s.wheel_spin, s.fuel, s.tire_wear[0], s.tire_wear[1], s.tire_grip,
                 s.damage, s.pit_state);
    std::fprintf(c.telemetry, ",%.3f,%.3f,%.4f,%.4f,%.2f,%.2f,%d", s.grip_use[0], s.grip_use[1], s.slip_angle[0],
                 s.slip_angle[1], s.accel_x, s.accel_y, s.blue_flag);
    std::fprintf(c.telemetry, ",%.1f,%.1f,%.4f,%.4f,%.3f,%.3f", s.tire_temp[0], s.tire_temp[1], s.axle_grip[0],
                 s.axle_grip[1], s.slipstream, s.dirty_air);
    for (float d : k.debug) std::fprintf(c.telemetry, ",%.4g", d);
    for (float t : s.tire_temp_wheel) std::fprintf(c.telemetry, ",%.1f", t);
    for (float t : s.brake_temp) std::fprintf(c.telemetry, ",%.0f", t);
    std::fprintf(c.telemetry, ",%.0f,%.0f,%.0f,%.2f,%d,%.2f,%d", s.kers_store, s.kers_power, s.kers_deploy_left, k.kers,
                 s.drs_state, c.state.drsFlap, k.drs);
    std::fputc('\n', c.telemetry);
}

void Race::resolveWalls(Car& c) {
    CarState& st = c.state;
    const float hl = c.phys.length * 0.5f, hw = c.phys.width * 0.5f;
    const Vec2 fwd = fromAngle(st.yaw), left = perpLeft(fwd);
    const Vec2 corners[4] = {fwd * hl + left * hw, fwd * hl - left * hw, -fwd * hl + left * hw, -fwd * hl - left * hw};
    const float m = carMass(c.phys, st), I = yawInertia(c);
    const RRPitInfo& pit = track_.pit();
    // Corners are at most half the diagonal from the centre: skip when far from
    // every barrier (the outer barrier is never closer than the runoff) and from
    // the pit wall.
    const float reach = std::sqrt(hl * hl + hw * hw);
    const float divMid = c.halfWidth + 0.5f * (Track::kDividerIn + Track::kDividerOut);
    const bool nearDivider = track_.hasPit() &&
                             track_.inSpan(c.trackS, pit.lane_start_s - 2 * reach, pit.lane_end_s + 2 * reach) &&
                             std::fabs(c.lateral * pit.side - divMid) < reach + 1.0f;
    if (!nearDivider && std::fabs(c.lateral) + reach < c.halfWidth + track_.runoff() - 0.5f) return;
    // Which side of the pit wall the car is on decides which face it hits.
    const bool carInLane = track_.hasPit() && c.lateral * pit.side > divMid;

    for (const Vec2& r0 : corners) {
        Vec2 p = st.pos + r0;
        TrackLoc loc = track_.locate(p, c.trackIdx, 12);
        const int side = loc.lateral > 0 ? 1 : -1;
        const float a = std::fabs(loc.lateral);
        const Vec2 out = track_.at(loc.idx).n * (float)side;  // away from the centreline
        float pen = 0;
        Vec2 nrm;
        float wall = track_.barrierOffset(loc.s, side, loc.halfWidth);
        if (a > wall) {
            pen = a - wall;
            nrm = -out;
        } else if (track_.hasPit() && side == pit.side && track_.inPitLane(loc.s)) {
            float in = loc.halfWidth + Track::kDividerIn, outer = loc.halfWidth + Track::kDividerOut;
            if (a > in && a < outer) {
                if (carInLane) { pen = outer - a; nrm = out; }
                else { pen = a - in; nrm = -out; }
            }
        }
        if (pen <= 0) continue;
        st.pos += nrm * pen;
        Vec2 r = r0;
        Vec2 v = st.velWorld() + angVel(st.yawRate, r);
        float vn = dot(v, nrm);
        if (vn >= 0) continue;
        float rn = cross2(r, nrm);
        float j = -(1 + kRestitutionWall) * vn / (1 / m + rn * rn / I);
        Vec2 tan = perpLeft(nrm);
        float vt = dot(v, tan);
        float rt = cross2(r, tan);
        float jt = -vt / (1 / m + rt * rt / I);
        jt = clampf(jt, -0.4f * j, 0.4f * j);
        Vec2 impulse = nrm * j + tan * jt;
        st.setVelWorld(st.velWorld() + impulse * (1 / m));
        st.yawRate += cross2(r, impulse) / I;
        if (-vn > 1.0f) {
            st.damage += (-vn) * (-vn);
            c.collisions++;
        }
    }
}

void Race::resolveCarPair(Car& a, Car& b) {
    Vec2 d0 = a.state.pos - b.state.pos;
    if (dot(d0, d0) > 36.0f) return;
    // Each car is approximated by three circles along its length.
    const float offs[3] = {-1.45f, 0.0f, 1.45f};
    const float ra = a.phys.width * 0.5f, rb = b.phys.width * 0.5f;
    Vec2 fa = fromAngle(a.state.yaw), fb = fromAngle(b.state.yaw);
    float bestPen = 0;
    Vec2 bestN, bestC;
    for (float oa : offs) {
        Vec2 pa = a.state.pos + fa * oa;
        for (float ob : offs) {
            Vec2 pb = b.state.pos + fb * ob;
            Vec2 d = pa - pb;
            float dist = length(d);
            float pen = ra + rb - dist;
            if (pen > bestPen && dist > 1e-5f) {
                bestPen = pen;
                bestN = d * (1 / dist);
                bestC = pb + bestN * rb;
            }
        }
    }
    if (bestPen <= 0) return;
    const float ma = carMass(a.phys, a.state), mb = carMass(b.phys, b.state), Ia = yawInertia(a), Ib = yawInertia(b);
    a.state.pos += bestN * (bestPen * 0.5f);
    b.state.pos -= bestN * (bestPen * 0.5f);
    Vec2 rA = bestC - a.state.pos, rB = bestC - b.state.pos;
    Vec2 va = a.state.velWorld() + angVel(a.state.yawRate, rA);
    Vec2 vb = b.state.velWorld() + angVel(b.state.yawRate, rB);
    float vrel = dot(va - vb, bestN);
    if (vrel >= 0) return;
    float rnA = cross2(rA, bestN), rnB = cross2(rB, bestN);
    float j = -(1 + kRestitutionCar) * vrel / (1 / ma + 1 / mb + rnA * rnA / Ia + rnB * rnB / Ib);
    Vec2 imp = bestN * j;
    a.state.setVelWorld(a.state.velWorld() + imp * (1 / ma));
    b.state.setVelWorld(b.state.velWorld() - imp * (1 / mb));
    a.state.yawRate += cross2(rA, imp) / Ia;
    b.state.yawRate -= cross2(rB, imp) / Ib;
    if (-vrel > 1.0f) {
        if (!over_) {
            int ia = (int)(&a - &cars_[0]), ib = (int)(&b - &cars_[0]);
            float ds = wrapDs(b.trackS - a.trackS);
            if (ds < 0) { std::swap(ia, ib); ds = -ds; }
            const Car& ca = cars_[ia];
            const Car& cb = cars_[ib];
            contacts_.push_back({time_, ia, ib, -vrel, ds, cb.lateral - ca.lateral, wrapAngle(cb.state.yaw - ca.state.yaw)});
        }
        a.state.damage += vrel * vrel;
        b.state.damage += vrel * vrel;
        a.collisions++;
        b.collisions++;
    }
}

void Race::updateProgress(Car& c) {
    TrackLoc loc = track_.locate(c.state.pos, c.trackIdx, 8);
    float L = track_.length();
    float ds = loc.s - c.trackS;
    if (ds > L * 0.5f) ds -= L;
    if (ds < -L * 0.5f) ds += L;
    c.distRaced += ds;
    c.trackIdx = loc.idx;
    c.trackS = loc.s;
    c.lateral = loc.lateral;
    c.halfWidth = loc.halfWidth;
    c.onTrack = std::fabs(loc.lateral) <= loc.halfWidth;

    if (c.finished || c.dnf || over_) return;

    if (c.distRaced >= 0) {
        size_t k = (size_t)(c.distRaced / kCheckpointSpacing);
        while (c.checkpoints.size() <= k) c.checkpoints.push_back(time_);
    }
    int lapsNow = (int)std::floor(c.distRaced / L);
    while (lapsNow > c.lapsDone) {
        float lt = (float)(time_ - c.lapStart);
        c.lapTimes.push_back(lt);
        c.lapPositions.push_back(c.position);
        Car::LapTemps lt2;
        for (int k = 0; k < 2; ++k) {
            lt2.avg[k] = c.tempN ? (float)(c.tempSum[k] / c.tempN) : c.state.tireTemp[k];
            lt2.max[k] = c.tempN ? c.tempMax[k] : c.state.tireTemp[k];
            c.tempSum[k] = 0;
            c.tempMax[k] = 0;
        }
        c.tempN = 0;
        c.lapTemps.push_back(lt2);
        if (c.bestLap <= 0 || lt < c.bestLap) c.bestLap = lt;
        c.lapStart = time_;
        c.state.kersDeployed = c.state.kersHarvested = 0;  // the lap's allowances start again
        c.lapsDone++;
        c.lapsOnTires++;
        if (c.lapsDone >= cfg_.laps) {
            c.finished = true;
            c.finishTime = time_;
            if (twoCompoundRule() && (c.compoundsUsed & (c.compoundsUsed - 1)) == 0) {
                c.penalties++;
                c.penaltyTime += RR_TWO_COMPOUND_PENALTY;
                c.twoCompoundPenalty = true;
                c.penaltyLog.push_back({time_, c.lapsDone, RR_TWO_COMPOUND_PENALTY,
                                        "two-compound rule: finished having raced only one tyre compound"});
            }
            if (leaderFinish_ < 0) leaderFinish_ = time_;
            break;
        }
    }

    float speed = std::sqrt(c.state.vx * c.state.vx + c.state.vy * c.state.vy);
    c.stuckTime = (speed < 0.5f && c.pitState != RR_PIT_SERVICE) ? c.stuckTime + cfg_.dt : 0.0f;
    c.noFuelTime = (speed < 0.5f && c.state.fuel <= 0) ? c.noFuelTime + cfg_.dt : 0.0f;
    if (c.noFuelTime > 5.0f) {
        c.dnf = true;
        c.dnfReason = "out of fuel";
    } else if (c.stuckTime > 60.0f) {
        c.dnf = true;
        c.dnfReason = "stuck";
    }
}

// Running in another car's wake. Slipstream: less drag, strongest right
// behind it, gone 60 m back or 3.5 m to the side. Dirty air: less downforce
// (up to 10%), mostly at the front, gone 40 m back or 3 m to the side.
void Race::wake(Car& c) const {
    c.draft = c.dirtyAir = 0;
    if (c.state.vx < 15.0f) return;
    for (const Car& o : cars_) {
        if (&o == &c || o.state.vx < 15.0f) continue;
        Vec2 rel = rotate(c.state.pos - o.state.pos, -o.state.yaw);
        float behind = -rel.x, side = std::fabs(rel.y);
        if (behind < 3.0f || behind > kDraftLength || side > kDraftWidth) continue;
        c.draft = std::max(c.draft, kDraftMax * (1 - behind / kDraftLength) * (1 - side / kDraftWidth));
        if (behind < kDirtyLength && side < kDirtyWidth)
            c.dirtyAir = std::max(c.dirtyAir, kDirtyMax * (1 - behind / kDirtyLength) * (1 - side / kDirtyWidth));
    }
}

// DRS (2013 rules). At each zone's detection point the gap to the car ahead is timed; within
// RR_DRS_GAP s it earns the flap for the zone, from its start to its end. Races only count it
// from lap RR_DRS_FIRST_LAP and never on a wet track; practice and qualifying allow it in every
// zone. The flap closes on a touch of the brakes, in the pit lane, and when the robot lets go.
void Race::updateDrs(Car& c) {
    const std::vector<RRDrsZone>& zones = track_.drsZones();
    const int nz = (int)zones.size();
    const bool hasDrs = c.phys.drsDragScale < 1.0f || c.phys.drsDownforceScale < 1.0f;
    c.state.drsOpen = false;
    c.drsState = RR_DRS_NONE;
    c.drsZone = c.drsNextZone = -1;
    c.drsNextDs = 0;
    if (!hasDrs || nz == 0) return;
    if ((int)c.drsEligible.size() != nz) {
        c.drsEligible.assign(nz, 0);
        c.drsDetectTime.assign(nz, -1.0);
    }
    const float L = track_.length(), s = c.trackS;
    auto fwdDist = [&](float to, float from) { float d = std::fmod(to - from, L); return d < 0 ? d + L : d; };
    const bool race = cfg_.session == RR_SESSION_RACE;
    const bool rules = !cfg_.wet && (!race || c.lapsDone + 1 >= RR_DRS_FIRST_LAP);

    // detection points crossed since the last step
    const float moved = c.drsPrevS < 0 ? 0.0f : fwdDist(s, c.drsPrevS);
    for (int z = 0; z < nz && moved > 0 && moved < 0.5f * L; ++z) {
        const float d = fwdDist(zones[z].detect_s, c.drsPrevS);
        if (d <= 0 || d > moved) continue;
        c.drsEligible[z] = 0;
        c.drsDetectTime[z] = time_;
        c.drsGap = -1;
        if (!race || c.position < 2 || c.finished || c.dnf) continue;
        const Car& ahead = cars_[order_[c.position - 2]];
        if ((int)ahead.drsDetectTime.size() != nz || ahead.drsDetectTime[z] < 0) continue;
        c.drsGap = (float)(time_ - ahead.drsDetectTime[z]);
        if (rules && c.drsGap <= RR_DRS_GAP) c.drsEligible[z] = 1;
    }
    c.drsPrevS = s;

    int zone = -1;
    for (int z = 0; z < nz; ++z)
        if (track_.inSpan(s, zones[z].start_s, zones[z].end_s)) zone = z;
    // leaving a zone ends what was earned for it
    if (c.drsLastZone >= 0 && c.drsLastZone != zone) c.drsEligible[c.drsLastZone] = 0;
    c.drsLastZone = zone;
    if (!race && rules && zone >= 0) c.drsEligible[zone] = 1;
    c.drsZone = zone;

    float best = L;
    for (int z = 0; z < nz; ++z) {
        const float d = z == zone ? 0.0f : fwdDist(zones[z].start_s, s);
        if (d < best) { best = d; c.drsNextZone = z; }
    }
    c.drsNextDs = c.drsNextZone >= 0 ? best : 0.0f;

    const bool allowed = rules && c.pitState == RR_PIT_NONE && !c.finished && !c.dnf && !over_ && c.onTrack;
    if (!allowed) return;
    c.drsState = RR_DRS_OFF;
    for (int z = 0; z < nz; ++z)
        if (z != zone && c.drsEligible[z]) c.drsState = RR_DRS_ARMED;
    if (zone >= 0 && c.drsEligible[zone]) {
        c.drsState = RR_DRS_AVAILABLE;
        if (c.control.drs && c.control.brake < 0.02f) {
            c.state.drsOpen = true;
            c.drsState = RR_DRS_OPEN;
        }
    }
}

void Race::updatePit(Car& c) {
    if (!track_.hasPit()) return;
    const RRPitInfo& p = track_.pit();
    const float side = (float)p.side;
    const float divMid = c.halfWidth + 0.5f * (Track::kDividerIn + Track::kDividerOut);
    const bool inLane = track_.inPitLane(c.trackS) && c.lateral * side > divMid;
    const float speed = std::sqrt(c.state.vx * c.state.vx + c.state.vy * c.state.vy);

    if (inLane && !c.finished && !over_) c.pitLaneTime += cfg_.dt;
    switch (c.pitState) {
    case RR_PIT_NONE:
        if (inLane && !c.dnf) c.pitState = RR_PIT_LANE;
        break;
    case RR_PIT_LANE: {
        if (!inLane) { c.pitState = RR_PIT_NONE; break; }
        float boxLat = c.halfWidth + Track::kBoxCentre;
        bool atBox = std::fabs(wrapDs(c.trackS - c.pitBoxS)) < 2.5f && std::fabs(c.lateral * side - boxLat) < 2.0f;
        if (c.control.pit_request && !cfg_.pitsClosed && atBox && speed < 0.5f && !c.finished && !over_) {
            c.pitOrder = c.control;
            float fuel = c.phys.noRefuel > 0.5f ? 0.0f : clampf(c.pitOrder.pit_fuel, 0.0f, c.phys.fuelCapacity - c.state.fuel);
            c.pitOrder.pit_fuel = fuel;
            bool tyres = c.pitOrder.pit_tires >= RR_TIRE_SOFT && c.pitOrder.pit_tires <= RR_TIRE_HARD;
            if (!tyres) c.pitOrder.pit_tires = 0;
            c.serviceLeft = c.phys.pitServiceScale *
                            (kServiceBase + std::max(fuel / kFuelFlow, tyres ? kTireChange : 0.0f) +
                             (c.pitOrder.pit_repair ? kRepairPer1000 * c.state.damage / 1000.0f : 0.0f));
            c.pitState = RR_PIT_SERVICE;
            Car::StopLog log;
            log.lap = c.currentLap(cfg_.laps);
            log.time = time_;
            log.fuelBefore = c.state.fuel;
            log.fuelAdded = fuel;
            log.wear[0] = c.state.tireWear[0];
            log.wear[1] = c.state.tireWear[1];
            log.damage = c.state.damage;
            log.service = c.serviceLeft;
            log.tiresBefore = c.state.compound;
            log.tiresFitted = c.pitOrder.pit_tires;
            log.repair = c.pitOrder.pit_repair != 0;
            log.reason = std::string(c.control.status, strnlen(c.control.status, sizeof c.control.status));
            c.stopLog.push_back(log);
        }
        break;
    }
    case RR_PIT_SERVICE:
        c.serviceLeft -= cfg_.dt;
        if (c.serviceLeft <= 0) finishService(c);
        break;
    case RR_PIT_DONE:
        if (!inLane) c.pitState = RR_PIT_NONE;
        break;
    }
}

void Race::finishService(Car& c) {
    c.state.fuel = std::min(c.phys.fuelCapacity, c.state.fuel + c.pitOrder.pit_fuel);
    if (c.pitOrder.pit_tires) {
        c.state.compound = c.pitOrder.pit_tires;
        c.compoundsUsed |= 1 << c.state.compound;
        c.state.tireWear[0] = c.state.tireWear[1] = 0;
        for (float& t : c.state.wheelTemp) t = c.phys.blanketTemp;
        c.state.tireTemp[0] = c.state.tireTemp[1] = c.phys.blanketTemp;
        c.lapsOnTires = 0;
    }
    if (c.pitOrder.pit_repair) c.state.damage = 0;
    c.serviceLeft = 0;
    c.pitStops++;
    c.pitLaps.push_back(c.currentLap(cfg_.laps));
    c.pitState = RR_PIT_DONE;
}

void Race::updateOrder() {
    order_.resize(cars_.size());
    for (size_t i = 0; i < cars_.size(); ++i) order_[i] = (int)i;
    std::stable_sort(order_.begin(), order_.end(), [&](int ia, int ib) {
        const Car& a = cars_[ia];
        const Car& b = cars_[ib];
        if (a.finished != b.finished) return a.finished;
        if (a.finished) return a.raceTime() < b.raceTime();
        if (a.dnf != b.dnf) return !a.dnf;
        return a.distRaced > b.distRaced;
    });
    const Car& leader = cars_[order_[0]];
    for (size_t p = 0; p < order_.size(); ++p) {
        Car& c = cars_[order_[p]];
        c.position = (int)p + 1;
        c.lapsBehind = (int)std::floor((leader.distRaced - c.distRaced) / track_.length());
        if (c.finished && leader.finished) {
            c.gap = c.raceTime() - leader.raceTime();
            c.lapsBehind = 0;
        } else if (!c.checkpoints.empty() && leader.checkpoints.size() >= c.checkpoints.size()) {
            size_t k = c.checkpoints.size() - 1;
            c.gap = c.checkpoints[k] - leader.checkpoints[k];
        } else {
            c.gap = p == 0 ? 0.0 : -1.0;  // not timed yet (still behind the start line)
        }
    }
}

bool Race::cooledDown() const {
    if (!over_) return false;
    if (time_ > overTime_ + 200.0) return true;
    for (const Car& c : cars_)
        if (!c.dnf && !c.parked) return false;
    return true;
}

// Cool-down: once a car has taken the flag (or the race is over) its robot
// keeps driving at a gentle pace (the host caps the speed) until it nears the
// pit entry; then the host drives it into the pit lane and parks it there.
RRControl Race::coolDownControl(Car& c) {
    RRControl k{};
    k.gear = c.state.gear;
    if (c.parked) {
        k.brake = 1;
        return k;
    }
    const float kCoolSpeed = 42.0f;
    const float L = track_.length();
    const float v = std::max(0.0f, c.state.vx);
    const bool pit = track_.hasPit();
    const RRPitInfo& p = track_.pit();
    auto fwd = [&](float a, float b) { float d = std::fmod(b - a, L); return d < 0 ? d + L : d; };
    float laneLen = pit ? fwd(p.lane_start_s, p.lane_end_s) : 0.0f;
    // Commit to the pits 250 m before the entry (otherwise do another lap).
    if (pit && c.parkSlot < 0) {
        const float toEntry = fwd(c.trackS, p.entry_s);
        const float slowTo = 0.9f * p.speed_limit;
        const float need = std::max(0.0f, v * v - slowTo * slowTo) / (2 * 5.0f) + 40.0f;  // still in time to slow down
        if (toEntry < 250.0f && toEntry > need - fwd(p.entry_s, p.lane_start_s)) c.parkSlot = parkedSlots_++;
    }
    const bool committed = c.parkSlot >= 0;
    if (!committed) {
        // The robot drives; the host holds it to cool-down pace.
        k = c.control;
        k.pit_request = 0;
        // While others are still racing, keep racing pace: a slow car on the line gets in their way.
        const float over = c.state.vx - kCoolSpeed;
        if (over_ && over > 0) {
            k.accel = 0;
            k.brake = std::max(k.brake, clampf(over * 0.005f, 0.0f, 0.06f));  // ease off, no brake test for the cars still racing
        }
        if (!pit && over_ && time_ > overTime_ + 5.0) {
            // No pit lane: pull over to the side and stop.
            k.accel = 0;
            k.brake = std::max(k.brake, 0.3f);
            if (c.state.vx < 0.5f) c.parked = true;
        }
        return k;
    }
    const float spacing = 9.0f;
    const int slots = std::max(1, (int)((laneLen - 40.0f) / spacing));
    const float parkS = pit ? std::fmod(p.lane_end_s - 20.0f - spacing * (float)(std::max(0, c.parkSlot) % slots) + L, L) : 0.0f;

    // Lateral to drive at, as a function of track distance.
    auto lateralAt = [&](float s) -> float {
        if (!pit || !committed) return 0.0f;
        const float edge = p.side * (track_.at(track_.indexAt(s)).halfWidth - 2.5f);
        if (track_.inSpan(s, p.entry_s, p.lane_start_s)) {
            float u = fwd(p.entry_s, s) / std::max(10.0f, fwd(p.entry_s, p.lane_start_s));
            u = u * u * (3 - 2 * u);
            return edge + (p.lane_offset - edge) * u;
        }
        if (track_.inSpan(s, p.lane_start_s, p.lane_end_s)) {
            float toPark = fwd(s, parkS);
            if (toPark > laneLen) return p.box_offset;  // just past the spot
            if (toPark < 15.0f) {
                float u = 1.0f - toPark / 15.0f;
                u = u * u * (3 - 2 * u);
                return p.lane_offset + (p.box_offset - p.lane_offset) * u;
            }
            return p.lane_offset;
        }
        float toEntry = fwd(s, p.entry_s);
        if (toEntry < 250.0f) return edge * (1.0f - toEntry / 250.0f);
        return 0.0f;
    };

    // Pure pursuit on that path.
    const float ld = 8.0f + 0.25f * v;
    const float sAhead = std::fmod(c.trackS + ld, L);
    Vec2 tgt = track_.pointAt(sAhead, lateralAt(sAhead));
    Vec2 d = rotate(tgt - c.state.pos, -c.state.yaw);
    float alpha = std::atan2(d.y, d.x);
    float delta = std::atan(2.0f * c.phys.wheelbase() * std::sin(alpha) / std::max(length(d), 1.0f));
    k.steer = clampf(delta / c.phys.maxSteer, -1, 1);

    // Speed: an easy pace for the corners ahead, the pit limit in the lane,
    // then stop at the parking spot.
    // (Racing pace until the pit lane while the others are still racing.)
    float vT = over_ ? kCoolSpeed : 1e9f;
    const float lat = over_ ? 1.1f : 2.5f;
    for (float a = 0; a < (over_ ? 120.0f : 250.0f); a += 4.0f) {
        float kap = std::fabs(track_.at(track_.indexAt(c.trackS + a)).curvature);
        float vc = kap > 1e-4f ? std::sqrt(lat * 9.81f / kap) : 1e9f;
        vT = std::min(vT, std::sqrt(vc * vc + 2 * 6.0f * a));
    }
    if (pit) {
        const bool inLane = track_.inSpan(c.trackS, p.lane_start_s, p.lane_end_s);
        float toLane = inLane ? 0.0f : fwd(c.trackS, p.lane_start_s);
        if (c.parkSlot >= 0 || toLane < 300.0f)
            vT = std::min(vT, std::sqrt(p.speed_limit * 0.9f * p.speed_limit * 0.9f + 2 * 5.0f * toLane));
        if (inLane && c.parkSlot >= 0) {
            float toPark = fwd(c.trackS, parkS);
            if (toPark > laneLen) toPark = 0;  // overshot: stop here
            vT = std::min(vT, std::sqrt(2 * 3.0f * std::max(0.0f, toPark - 0.5f)));
            if (toPark < 1.0f && v < 0.5f) c.parked = true;
        }
    }
    // Gentle inputs: no wheelspin, no locked wheels.
    float err = vT - v;
    if (vT < 0.3f) k.brake = v > 2.0f ? 0.4f : 1.0f;
    else if (err > 0) k.accel = c.state.wheelSpin > 0 ? 0.0f : clampf(0.15f + 0.1f * err, 0, over_ ? 0.5f : 1.0f);
    else k.brake = clampf(-0.08f * err, 0, 0.4f);
    return k;
}

void Race::step() {
    if (over_ && cooledDown()) return;
    if (steps_ % robotPeriod_ == 0) callRobots();

    const float dt = cfg_.dt;
    const WearRates rates{cfg_.fuelRate, cfg_.wearRate, cfg_.ambient};
    for (Car& c : cars_) {
        Surface surf;
        if (!track_.paved(c.trackS, c.lateral, c.halfWidth)) {
            surf.muScale = 0.7f;
            surf.extraDrag = 250.0f;
        }
        wake(c);
        updateDrs(c);
        surf.dragScale = 1.0f - c.draft;
        surf.downforceScale = 1.0f - c.dirtyAir;
        surf.frontDownforceScale = 1.0f - 0.5f * c.dirtyAir;  // the front loses more: the car pushes
        if (track_.is3D()) {
            // The road's slope in the car's frame: grade along the track, tan(bank) across it.
            float grade, bank, vcurv;
            track_.shapeAt(c.trackS, &grade, &bank, &vcurv);
            Vec2 td = track_.dirAt(c.trackS), tn = perpLeft(td);
            Vec2 grad = td * grade + tn * std::tan(bank);
            Vec2 fwd = fromAngle(c.state.yaw), left = perpLeft(fwd);
            surf.slopeX = dot(grad, fwd);
            surf.slopeY = dot(grad, left);
            surf.bankY = bank * dot(tn, left);
            surf.vcurv = vcurv * dot(td, fwd) * dot(td, fwd);
        }
        RRControl in = c.control;
        if (!c.dnf && (c.finished || over_)) {
            in = coolDownControl(c);
        } else if (c.pitState == RR_PIT_SERVICE) {
            in = RRControl{};
            in.brake = 1;
            in.gear = c.state.gear;
        } else if (c.pitState == RR_PIT_LANE || c.pitState == RR_PIT_DONE) {
            // Pit limiter: no drive above the limit, braking well above it.
            float over = c.state.vx - track_.pit().speed_limit;
            if (over > 0) {
                in.accel = 0;
                in.brake = std::max(in.brake, clampf(over * 0.3f, 0.0f, 1.0f));
            }
        }
        stepCar(c.state, c.phys, in, (c.robotCfg.auto_gear != 0) || c.finished || over_, surf, rates, dt);
        if (!c.finished && !c.dnf) {
            for (int k = 0; k < 2; ++k) {
                c.tempSum[k] += c.state.tireTemp[k];
                c.tempMax[k] = std::max(c.tempMax[k], c.state.tireTemp[k]);
            }
            ++c.tempN;
        }
        if (c.pitState == RR_PIT_SERVICE || c.parked) {
            c.state.vx = c.state.vy = c.state.yawRate = 0;
        }
    }
    for (Car& c : cars_) resolveWalls(c);
    for (size_t i = 0; i < cars_.size(); ++i)
        for (size_t j = i + 1; j < cars_.size(); ++j)
            // A retired car is taken away by the marshals: the robots no longer see it
            // (computeSensors skips it), so it must not stay on the track as an obstacle.
            if (!cars_[i].dnf && !cars_[j].dnf) resolveCarPair(cars_[i], cars_[j]);

    time_ += dt;
    steps_++;
    for (Car& c : cars_) {
        updateProgress(c);
        updatePit(c);
    }
    if (over_) return;  // cool-down: the classification is final
    updateOrder();

    bool allDone = true;
    for (const Car& c : cars_)
        if (!c.finished && !c.dnf) allDone = false;
    // Once the leader finishes, the others get to complete their lap (bounded).
    bool timeout = leaderFinish_ >= 0 && time_ > leaderFinish_ + std::max(60.0, track_.length() / 10.0);
    if (allDone || timeout || time_ >= maxTime_) {
        over_ = true;
        overTime_ = time_;
        for (Car& c : cars_) endSession(c);
        for (Car& c : cars_)
            if (!c.finished && !c.dnf && time_ >= maxTime_) c.dnfReason = "time limit";
        for (Car& c : cars_)
            if (c.telemetry) std::fflush(c.telemetry);
    }
}

void Race::advance(double seconds) {
    long long n = (long long)std::floor(seconds / cfg_.dt + 1e-9);
    for (long long i = 0; i < n && !over_; ++i) step();
}

void Race::printResults(FILE* out) const {
    std::fprintf(out, "\n%s, %d lap%s, %.1f m\n", track_.name().c_str(), cfg_.laps, cfg_.laps > 1 ? "s" : "",
                 track_.length());
    bool anyDrs = false;
    for (const Car& c : cars_) anyDrs = anyDrs || c.phys.drsDragScale < 1.0f;
    if (anyDrs && !track_.drsZones().empty()) {
        std::fprintf(out, "DRS zones (detection, start-end):");
        for (const RRDrsZone& z : track_.drsZones()) std::fprintf(out, " %.0f, %.0f-%.0f m;", z.detect_s, z.start_s, z.end_s);
        std::fprintf(out, "%s\n", cfg_.wet ? " wet: DRS off" : "");
    }
    std::fprintf(out, " Pos  %-22s %-12s %10s %10s %6s %6s %s\n", "Driver", "Robot", "Time", "Best lap", "Laps",
                 "Hits", "Stops");
    for (int idx : order_) {
        const Car& c = cars_[idx];
        std::string t;
        if (c.finished) t = c.position == 1 ? fmtTime(c.raceTime()) : "+" + fmtTime(c.raceTime() - cars_[order_[0]].raceTime());
        else if (c.dnf) t = "DNF " + c.dnfReason;
        else t = c.dnfReason.empty() ? "running" : c.dnfReason;
        std::string stops = std::to_string(c.pitStops);
        for (size_t k = 0; k < c.pitLaps.size(); ++k) stops += (k ? "," : " (L") + std::to_string(c.pitLaps[k]);
        if (!c.pitLaps.empty()) stops += ")";
        if (c.penalties) stops += "  pen +" + std::to_string((int)c.penaltyTime) + "s";
        std::fprintf(out, " %3d  %-22s %-12s %10s %10s %6d %6d %s\n", c.position, c.name.c_str(),
                     c.robotName.c_str(), t.c_str(), fmtTime(c.bestLap).c_str(), c.lapsDone, c.collisions,
                     stops.c_str());
    }
}

bool Race::writeJson(const std::string& path, double wallSeconds) const {
    FILE* f = std::fopen(path.c_str(), "w");
    if (!f) return false;
    std::fprintf(f, "{\n  \"track\": \"%s\",\n  \"track_length\": %.2f,\n  \"laps\": %d,\n  \"seed\": %llu,\n",
                 jsonEscape(track_.name()).c_str(), track_.length(), cfg_.laps, (unsigned long long)cfg_.seed);
    std::fprintf(f, "  \"sim_time\": %.3f,\n  \"wall_time\": %.3f,\n  \"cars\": [\n", time_, wallSeconds);
    for (size_t p = 0; p < order_.size(); ++p) {
        const Car& c = cars_[order_[p]];
        std::fprintf(f, "    {\"position\": %d, \"car_index\": %d, \"name\": \"%s\", \"robot\": \"%s\", \"params\": \"%s\", ",
                     c.position, order_[p], jsonEscape(c.name).c_str(), jsonEscape(c.robotName).c_str(),
                     jsonEscape(c.params).c_str());
        std::fprintf(f, "\"finished\": %s, \"dnf\": %s, \"status\": \"%s\", \"total_time\": %.3f, \"best_lap\": %.3f, ",
                     c.finished ? "true" : "false", c.dnf ? "true" : "false",
                     c.finished ? "finished" : (c.dnf ? ("dnf " + c.dnfReason).c_str() : (c.dnfReason.empty() ? "running" : c.dnfReason.c_str())),
                     c.finished ? c.raceTime() : 0.0, c.bestLap);
        std::fprintf(f, "\"cpu_avg_ms\": %.4f, \"cpu_max_ms\": %.4f, \"cpu_overruns\": %d, ",
                     c.driveCalls ? 1000.0 * c.cpuTotal / (double)c.driveCalls : 0.0, 1000.0 * c.cpuMax, c.cpuOverruns);
        std::fprintf(f, "\"penalties\": %d, \"penalty_time\": %.1f, \"blue_flags\": %d, ", c.penalties, c.penaltyTime,
                     c.blueFlags);
        std::fprintf(f, "\"distance\": %.2f, \"collisions\": %d, \"damage\": %.1f, ", c.distRaced,
                     c.collisions, c.state.damage);
        std::fprintf(f, "\"fuel_left\": %.2f, \"tire_wear\": [%.3f, %.3f], \"tire_compound\": %d, \"pit_stops\": %d, "
                        "\"pit_lane_time\": %.2f, \"pit_laps\": [",
                     c.state.fuel, c.state.tireWear[0], c.state.tireWear[1], c.state.compound, c.pitStops, c.pitLaneTime);
        for (size_t k = 0; k < c.pitLaps.size(); ++k) std::fprintf(f, "%s%d", k ? ", " : "", c.pitLaps[k]);
        std::fprintf(f, "], \"lap_times\": [");
        for (size_t k = 0; k < c.lapTimes.size(); ++k) std::fprintf(f, "%s%.3f", k ? ", " : "", c.lapTimes[k]);
        std::fprintf(f, "], \"lap_positions\": [");
        for (size_t k = 0; k < c.lapPositions.size(); ++k) std::fprintf(f, "%s%d", k ? ", " : "", c.lapPositions[k]);
        std::fprintf(f, "], \"lap_tire_temps\": [");
        for (size_t k = 0; k < c.lapTemps.size(); ++k) {
            const Car::LapTemps& t = c.lapTemps[k];
            std::fprintf(f, "%s{\"lap\": %zu, \"front_avg\": %.1f, \"rear_avg\": %.1f, \"front_max\": %.1f, \"rear_max\": %.1f}",
                         k ? ", " : "", k + 1, t.avg[0], t.avg[1], t.max[0], t.max[1]);
        }
        std::fprintf(f, "], \"penalty_log\": [");
        for (size_t k = 0; k < c.penaltyLog.size(); ++k) {
            const Car::PenaltyLog& pl = c.penaltyLog[k];
            std::fprintf(f, "%s{\"time\": %.2f, \"lap\": %d, \"seconds\": %.1f, \"reason\": \"%s\"}", k ? ", " : "", pl.time,
                         pl.lap, pl.seconds, jsonEscape(pl.reason).c_str());
        }
        std::fprintf(f, "], \"stops\": [");
        static const char* tyre[] = {"", "soft", "medium", "hard"};
        for (size_t k = 0; k < c.stopLog.size(); ++k) {
            const Car::StopLog& s = c.stopLog[k];
            std::fprintf(f,
                         "%s\n      {\"lap\": %d, \"time\": %.2f, \"fuel_before\": %.2f, \"fuel_added\": %.2f, "
                         "\"tires_before\": \"%s\", \"tires_fitted\": \"%s\", \"wear\": [%.3f, %.3f], \"damage\": %.0f, "
                         "\"repair\": %s, \"service\": %.2f, \"reason\": \"%s\"}",
                         k ? "," : "", s.lap, s.time, s.fuelBefore, s.fuelAdded, tyre[s.tiresBefore & 3],
                         tyre[s.tiresFitted & 3], s.wear[0], s.wear[1], s.damage, s.repair ? "true" : "false", s.service,
                         jsonEscape(s.reason).c_str());
        }
        std::fprintf(f, "]}%s\n", p + 1 < order_.size() ? "," : "");
    }
    std::fprintf(f, "  ]\n}\n");
    std::fclose(f);
    return true;
}

}  // namespace rr
