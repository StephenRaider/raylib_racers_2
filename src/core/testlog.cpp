#include "testlog.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "mini_json.hpp"

namespace fs = std::filesystem;

namespace rr {

namespace {

constexpr float kG = 9.81f;
constexpr float kPi = 3.14159265f;

float lerpAngle(float a, float b, float t) {
    float d = b - a;
    while (d > kPi) d -= 2 * kPi;
    while (d < -kPi) d += 2 * kPi;
    return a + d * t;
}

std::string esc(const std::string& s) {
    std::string o;
    for (char ch : s) {
        if (ch == '"' || ch == '\\') { o += '\\'; o += ch; }
        else if (ch == '\n') o += "\\n";
        else if ((unsigned char)ch < 0x20) o += ' ';
        else o += ch;
    }
    return o;
}

std::string nowString() {
    char buf[32];
    const std::time_t now = std::time(nullptr);
    std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S", std::localtime(&now));
    return buf;
}

int compoundFromKey(const std::string& k) {
    return k == "soft" ? RR_TIRE_SOFT : k == "hard" ? RR_TIRE_HARD : k == "medium" ? RR_TIRE_MEDIUM : 0;
}

const char* kCsvHeader =
    "t,lap,lap_time,lap_dist,x,y,yaw,speed_kmh,vx,vy,yaw_rate,steer,steer_angle,throttle,brake,gear,rpm,"
    "accel_x,accel_y,load_fl,load_fr,load_rl,load_rr,grip_use_f,grip_use_r,slip_f,slip_r,wheel_spin,damage,"
    "fuel,wear_f,wear_r,temp_f,temp_r,compound,lateral,angle,on_track,wheel_rot,kers_charge,kers_power,drs_flap";

const char* kReadme = R"(# Test runs

Written by the viewer's Testing mode (and `rr_race --test-log DIR`). Plain
JSON and CSV, meant to be read by people and by AI tools alike.

- `runs.json`: every run ever saved: its setup (track, algorithm and its
  parameters, stats, tyres, fuel, laps, wear rate) and its results (best and
  average lap, fuel and tyre wear per lap, every lap time). Never pruned.
- `run_NNNN/summary.json`: one run in detail: the setup, a table of laps
  (lap and sector times, fuel used, tyre wear added, tyre temperatures, top
  and minimum speed, share of the lap at full throttle and on the brakes, peak
  lateral g, damage, off-track excursions and slides) and a list of events
  worth a look (off track, contact, oversteer, understeer, wheelspin, spin,
  stopped, out of fuel) with the lap, the distance into the lap and how long
  each lasted.
- `run_NNNN/telemetry.csv`: the car 25 times a second. Kept only for the 10
  newest runs of each algorithm; the summary stays.

Telemetry columns:

| column | meaning |
|---|---|
| t | race time, s |
| lap | lap number, 1-based (lap 1 is the standing start) |
| lap_time | time since the lap started, s |
| lap_dist | metres from the start line on this lap (negative on the grid) |
| x, y, yaw | world position (m) and heading (rad, counter-clockwise from +x) |
| speed_kmh | speed, km/h |
| vx, vy | body-frame velocity, m/s (y + = sliding left) |
| yaw_rate | rad/s |
| steer | robot's steering command, -1 right .. +1 left |
| steer_angle | road-wheel angle, rad |
| throttle, brake | robot's pedals, 0..1 |
| gear, rpm | |
| accel_x, accel_y | body-frame acceleration, m/s^2 (divide by 9.81 for g) |
| load_fl .. load_rr | wheel loads, N |
| grip_use_f, grip_use_r | force asked of the axle over what it can give: ~1 is the limit, > 1.1 sliding |
| slip_f, slip_r | axle slip angles, rad |
| wheel_spin | driven wheels past their grip, 0 = gripping |
| damage | accumulated damage (8000 = full loss) |
| fuel | litres left |
| wear_f, wear_r | tyre wear 0 new .. 1; grip falls off a cliff past 0.7 |
| temp_f, temp_r | tyre temperature, C |
| compound | 1 soft, 2 medium, 3 hard |
| lateral | metres from the centreline, + = left |
| angle | heading minus track direction, rad |
| on_track | 1 on the tarmac |
| wheel_rot | wheel rotation (for replays) |
| kers_charge | J in the KERS battery |
| kers_power | W, + deploying, - recovering |
| drs_flap | DRS flap, 0 closed .. 1 open |

Comparing runs: lap times only compare on the same track and wear rate.
`summary.json` events give the lap and `lap_dist` of each problem; look the
same stretch up in `telemetry.csv` (filter on `lap` and `lap_dist`).
)";

}  // namespace

const char* compoundKey(int compound) {
    return compound == RR_TIRE_SOFT ? "soft" : compound == RR_TIRE_HARD ? "hard" : "medium";
}

// ---------------------------------------------------------------- recorder

void TestRecorder::begin(const Race& race, int carIndex) {
    *this = TestRecorder{};
    car = carIndex;
    trackLength = race.track().length();
}

void TestRecorder::update(const Race& race) {
    const Car& c = race.cars()[car];
    if (done_) return;
    // the moment the run ends is always sampled, so its last lap closes
    if (race.time() + 1e-9 < next_ && !c.finished && !c.dnf && !race.isOver()) return;
    while (next_ <= race.time() + 1e-9) next_ += 1.0 / kRate;
    sample(race);
    if (c.finished || c.dnf) done_ = true;
}

void TestRecorder::sample(const Race& race) {
    const Car& c = race.cars()[car];
    TestSample x;
    x.t = (float)race.time();
    x.lap = c.lapsDone + 1;
    x.lapTime = (float)(race.time() - c.lapStart);
    x.lapDist = (float)(c.distRaced - c.lapsDone * (double)trackLength);
    x.steer = c.control.steer;
    x.accel = c.control.accel;
    x.brake = c.control.brake;
    x.lateral = c.lateral;
    x.angle = c.sensors.angle;
    x.onTrack = c.onTrack;
    x.s = c.state;
    push(x, c.dnf ? c.dnfReason : std::string());
}

void TestRecorder::push(const TestSample& x, const std::string& dnf) {
    if (samples.empty()) {
        lapStartT_ = x.t - x.lapTime;
        lapFuel_ = x.s.fuel;
        lapWear_[0] = x.s.tireWear[0];
        lapWear_[1] = x.s.tireWear[1];
        lapDamage_ = lastDamage_ = x.s.damage;
    }
    // a new lap: close the one before
    while (x.lap > (int)laps.size() + 1) {
        const float end = x.t - x.lapTime;
        closeLap((int)samples.size(), end - lapStartT_, x);
        lapStartT_ = end;
    }
    // sectors: thirds of the lap by distance, interpolated between samples
    if (!samples.empty() && samples.back().lap == x.lap && sectorDone_ < 2) {
        const TestSample& p = samples.back();
        const float thr = trackLength * (sectorDone_ + 1) / 3.0f;
        if (p.lapDist < thr && x.lapDist >= thr) {
            const float f = (thr - p.lapDist) / std::max(1e-3f, x.lapDist - p.lapDist);
            const float tc = p.lapTime + f * (x.lapTime - p.lapTime);
            lapSectors_[sectorDone_] = tc - sectorStart_;
            sectorStart_ = tc;
            ++sectorDone_;
        }
    }
    samples.push_back(x);

    // events
    const float speed = std::hypot(x.s.vx, x.s.vy);
    const float gf = x.s.gripUse[0], gr = x.s.gripUse[1];
    // the standing start always spins the wheels a little, and a spinning car
    // slides every way: neither is worth an event of its own
    const bool launch = x.t < 2.0f || spin_.on || std::fabs(x.angle) > 1.0f;
    flag(off_, !x.onTrack, std::fabs(x.lateral), 0.0f, "off_track", "widest: metres from the centreline", x);
    flag(over_, gr > 1.12f && gr > gf && !launch, gr, 0.2f, "oversteer", "rear grip use (1 = the limit)", x);
    flag(under_, gf > 1.25f && gf >= gr && !launch, gf, 0.3f, "understeer",
         x.brake > 0.3f ? "front grip use, under braking" : "front grip use (1 = the limit)", x);
    flag(wheelspin_, x.s.wheelSpin > 0.15f && x.accel > 0.3f && !launch, x.s.wheelSpin, 0.2f, "wheelspin",
         "driven wheels past their grip", x);
    flag(spin_, std::fabs(x.angle) > 1.0f, std::fabs(x.angle), 0.0f, "spin", "heading off the track direction, rad", x);
    flag(stopped_, speed < 3.0f && x.t > 8.0f, 0, 2.0f, "stopped", "below 3 m/s", x);
    if (x.s.damage > lastDamage_ + 1.0f) {
        const float add = x.s.damage - lastDamage_;
        TestEvent* last = nullptr;
        for (auto it = events.rbegin(); it != events.rend() && !last; ++it)
            if (it->kind == "contact") last = &*it;
        if (contactT_ >= 0 && x.t - contactT_ < 1.0f && last) {
            last->peak += add;
            last->duration = x.t - last->t;
        } else {
            TestEvent e;
            e.t = x.t;
            e.lap = x.lap;
            e.lapDist = x.lapDist;
            e.peak = add;
            e.kind = "contact";
            e.detail = "damage added";
            addEvent(e);
        }
        contactT_ = x.t;
    }
    lastDamage_ = x.s.damage;
    if (x.s.fuel <= 0 && !outOfFuel_) {
        outOfFuel_ = true;
        TestEvent e;
        e.t = x.t;
        e.lap = x.lap;
        e.lapDist = x.lapDist;
        e.kind = "out_of_fuel";
        e.detail = "the tank is empty";
        addEvent(e);
    }
    (void)dnf;
}

void TestRecorder::flag(Flag& f, bool cond, float value, float minDuration, const char* kind,
                        const std::string& detail, const TestSample& now) {
    if (cond) {
        if (!f.on) {
            f.on = true;
            f.t0 = now.t;
            f.peak = value;
            f.lap = now.lap;
            f.lapDist = now.lapDist;
        } else {
            f.peak = std::max(f.peak, value);
        }
        return;
    }
    if (!f.on) return;
    f.on = false;
    const float dur = now.t - f.t0;
    if (dur + 1e-4f < minDuration) return;
    // one event for a moment: the same thing again within a second extends it
    for (int i = (int)events.size() - 1; i >= 0 && i >= (int)events.size() - 12; --i) {
        TestEvent& p = events[i];
        if (p.kind != kind || p.t + p.duration < f.t0 - 1.0f) continue;
        p.duration = now.t - p.t;
        p.peak = std::max(p.peak, f.peak);
        return;
    }
    TestEvent e;
    e.t = f.t0;
    e.lap = f.lap;
    e.lapDist = f.lapDist;
    e.duration = dur;
    e.peak = f.peak;
    e.kind = kind;
    e.detail = detail;
    addEvent(e);
}

void TestRecorder::addEvent(const TestEvent& e) {
    auto it = std::upper_bound(events.begin(), events.end(), e.t, [](float t, const TestEvent& x) { return t < x.t; });
    events.insert(it, e);
}

void TestRecorder::closeLap(int endSample, float lapTime, const TestSample& now) {
    TestLap l;
    l.lap = (int)laps.size() + 1;
    l.time = lapTime;
    l.firstSample = lapStartSample_;
    l.endSample = endSample;
    for (int k = 0; k < 2; ++k) l.sectors[k] = sectorDone_ > k ? lapSectors_[k] : 0;
    l.sectors[2] = sectorDone_ >= 2 ? lapTime - lapSectors_[0] - lapSectors_[1] : 0;
    l.fuelEnd = now.s.fuel;
    l.fuelUsed = lapFuel_ - now.s.fuel;
    for (int k = 0; k < 2; ++k) {
        l.wearEnd[k] = now.s.tireWear[k];
        l.wear[k] = now.s.tireWear[k] - lapWear_[k];
    }
    l.damage = now.s.damage - lapDamage_;
    const int n = endSample - lapStartSample_;
    if (n > 0) {
        l.minSpeed = 1e9f;
        int full = 0, brk = 0;
        for (int i = lapStartSample_; i < endSample; ++i) {
            const TestSample& s = samples[i];
            const float v = std::hypot(s.s.vx, s.s.vy) * 3.6f;
            l.topSpeed = std::max(l.topSpeed, v);
            if (s.lapDist > 0) l.minSpeed = std::min(l.minSpeed, v);
            full += s.accel > 0.95f;
            brk += s.brake > 0.05f;
            l.maxLatG = std::max(l.maxLatG, std::fabs(s.s.ay) / kG);
            for (int k = 0; k < 2; ++k) {
                l.tempAvg[k] += s.s.tireTemp[k] / n;
                l.tempMax[k] = std::max(l.tempMax[k], s.s.tireTemp[k]);
            }
        }
        if (l.minSpeed > 1e8f) l.minSpeed = 0;
        l.fullThrottle = (float)full / n;
        l.braking = (float)brk / n;
    }
    laps.push_back(l);
    countEvents(laps.back());
    lapStartSample_ = endSample;
    lapFuel_ = now.s.fuel;
    lapWear_[0] = now.s.tireWear[0];
    lapWear_[1] = now.s.tireWear[1];
    lapDamage_ = now.s.damage;
    sectorDone_ = 0;
    sectorStart_ = 0;
}

void TestRecorder::countEvents(TestLap& l) const {
    l.offTracks = l.slides = 0;
    bool contact = false;
    for (const TestEvent& e : events) {
        if (e.lap != l.lap) continue;
        if (e.kind == "off_track") ++l.offTracks;
        if (e.kind == "oversteer" || e.kind == "understeer" || e.kind == "spin") ++l.slides;
        if (e.kind == "contact") contact = true;
    }
    l.clean = l.offTracks == 0 && !contact;
}

void TestRecorder::finish() {
    if (samples.empty()) return;
    TestSample last = samples.back();
    last.t += 1e-3f;
    for (Flag* f : {&off_, &over_, &under_, &wheelspin_, &spin_, &stopped_}) {
        if (!f->on) continue;
        const bool keep = f == &off_ || f == &spin_ || last.t - f->t0 >= (f == &stopped_ ? 2.0f : f == &under_ ? 0.3f : 0.2f);
        f->on = false;
        if (!keep) continue;
        TestEvent e;
        e.t = f->t0;
        e.lap = f->lap;
        e.lapDist = f->lapDist;
        e.duration = last.t - f->t0;
        e.peak = f->peak;
        e.kind = f == &off_ ? "off_track" : f == &over_ ? "oversteer" : f == &under_ ? "understeer"
               : f == &wheelspin_ ? "wheelspin" : f == &spin_ ? "spin" : "stopped";
        addEvent(e);
    }
    std::stable_sort(events.begin(), events.end(), [](const TestEvent& a, const TestEvent& b) { return a.t < b.t; });
    for (TestLap& l : laps) countEvents(l);
}

void TestRecorder::rebuild(float length) {
    std::vector<TestSample> all;
    all.swap(samples);
    TestRecorder fresh;
    fresh.trackLength = length;
    fresh.car = car;
    *this = fresh;
    for (const TestSample& x : all) push(x, std::string());
    finish();
}

int TestRecorder::indexAt(double t) const {
    if (samples.empty()) return -1;
    auto it = std::upper_bound(samples.begin(), samples.end(), t,
                               [](double v, const TestSample& s) { return v < s.t; });
    const int i = (int)(it - samples.begin()) - 1;
    return std::max(0, i);
}

TestSample TestRecorder::at(double t) const {
    const int i = indexAt(t);
    if (i < 0) return TestSample{};
    if (i + 1 >= (int)samples.size()) return samples[i];
    const TestSample& a = samples[i];
    const TestSample& b = samples[i + 1];
    const float f = (float)std::clamp((t - a.t) / std::max(1e-6f, b.t - a.t), 0.0, 1.0);
    TestSample o = f < 0.5f ? a : b;
    auto L = [f](float p, float q) { return p + (q - p) * f; };
    o.t = (float)t;
    if (a.lap == b.lap) {
        o.lap = a.lap;
        o.lapTime = L(a.lapTime, b.lapTime);
        o.lapDist = L(a.lapDist, b.lapDist);
    }
    o.steer = L(a.steer, b.steer);
    o.accel = L(a.accel, b.accel);
    o.brake = L(a.brake, b.brake);
    o.lateral = L(a.lateral, b.lateral);
    o.s.pos = {L(a.s.pos.x, b.s.pos.x), L(a.s.pos.y, b.s.pos.y)};
    o.s.yaw = lerpAngle(a.s.yaw, b.s.yaw, f);
    o.s.vx = L(a.s.vx, b.s.vx);
    o.s.vy = L(a.s.vy, b.s.vy);
    o.s.steerAngle = L(a.s.steerAngle, b.s.steerAngle);
    o.s.wheelRot = L(a.s.wheelRot, b.s.wheelRot);
    o.s.rpm = L(a.s.rpm, b.s.rpm);
    o.s.fuel = L(a.s.fuel, b.s.fuel);
    o.s.kersCharge = L(a.s.kersCharge, b.s.kersCharge);
    o.s.kersPowerNow = L(a.s.kersPowerNow, b.s.kersPowerNow);
    o.s.drsFlap = L(a.s.drsFlap, b.s.drsFlap);
    return o;
}

int TestRecorder::bestLap() const {
    int best = -1;
    for (int i = 0; i < (int)laps.size(); ++i)
        if (best < 0 || laps[i].time < laps[best].time) best = i;
    return best;
}

float TestRecorder::averageLap() const {
    std::vector<float> t;
    for (size_t i = laps.size() > 1 ? 1 : 0; i < laps.size(); ++i) t.push_back(laps[i].time);
    if (t.empty()) return 0;
    std::sort(t.begin(), t.end());
    return t.size() % 2 ? t[t.size() / 2] : 0.5f * (t[t.size() / 2 - 1] + t[t.size() / 2]);
}

float TestRecorder::fuelPerLap() const {
    if (laps.empty()) return 0;
    float f = 0;
    for (const TestLap& l : laps) f += l.fuelUsed;
    return f / laps.size();
}

void TestRecorder::wearPerLap(float out[2]) const {
    out[0] = out[1] = 0;
    if (laps.empty()) return;
    for (const TestLap& l : laps)
        for (int k = 0; k < 2; ++k) out[k] += l.wear[k] / laps.size();
}

void TestRecorder::lapRange(int lap, int& first, int& end) const {
    first = end = 0;
    if (lap < 1) return;
    if (lap <= (int)laps.size()) {
        first = laps[lap - 1].firstSample;
        end = laps[lap - 1].endSample;
    } else if (lap == (int)laps.size() + 1) {
        first = lapStartSample_;
        end = (int)samples.size();
    }
}

// ---------------------------------------------------------------- store

TestStore::TestStore(std::string dir) : dir_(std::move(dir)) {}

const TestRun* TestStore::find(int id) const {
    for (const TestRun& r : runs_)
        if (r.id == id) return &r;
    return nullptr;
}

bool TestStore::load(std::string* err) {
    runs_.clear();
    const std::string path = dir_ + "/runs.json";
    std::ifstream f(path);
    if (!f) return true;
    std::stringstream ss;
    ss << f.rdbuf();
    const mjson::Value v = mjson::parse(ss.str());
    if (v.type != mjson::Value::Object) {
        if (err) *err = path + ": not valid JSON";
        return false;
    }
    for (const mjson::Value& r : v["runs"].arr) {
        TestRun t;
        t.id = (int)r["id"].num();
        t.date = r["date"].str();
        t.lapsDone = (int)r["laps_done"].num();
        t.completed = r["completed"].b;
        t.end = r["end"].str();
        t.best = (float)r["best_lap"].num();
        t.average = (float)r["average_lap"].num();
        t.fuelPerLap = (float)r["fuel_per_lap"].num();
        t.wearPerLap[0] = (float)r["wear_per_lap"][0].num();
        t.wearPerLap[1] = (float)r["wear_per_lap"][1].num();
        for (const mjson::Value& x : r["lap_times"].arr) t.lapTimes.push_back((float)x.num());
        t.folder = r["folder"].str();
        t.telemetry = r["telemetry"].b;
        const mjson::Value& s = r["setup"];
        t.setup.track = s["track"].str();
        t.setup.trackTitle = s["track_name"].str();
        t.setup.robot = s["robot"].str();
        t.setup.label = s["algorithm"].str();
        t.setup.params = s["params"].str();
        t.setup.dev = s["stats"].str();
        t.setup.livery = (int)s["livery"].num();
        t.setup.laps = (int)s["laps"].num();
        t.setup.compound = compoundFromKey(s["tyres"].str());
        t.setup.fuel = (float)s["fuel"].num();
        t.setup.wearRate = (float)s["wear_rate"].num(1);
        t.setup.fuelRate = (float)s["fuel_rate"].num(1);
        t.setup.ambient = (float)s["ambient"].num(25);
        if (t.id > 0) runs_.push_back(t);
    }
    return true;
}

static void writeSetup(FILE* f, const TestSetup& s, const char* indent) {
    std::fprintf(f,
                 "{\n%s  \"track\": \"%s\", \"track_name\": \"%s\",\n"
                 "%s  \"algorithm\": \"%s\", \"robot\": \"%s\", \"params\": \"%s\",\n"
                 "%s  \"stats\": \"%s\", \"livery\": %d,\n"
                 "%s  \"laps\": %d, \"tyres\": \"%s\", \"fuel\": %.2f, \"wear_rate\": %.4f, \"fuel_rate\": %.3f, "
                 "\"ambient\": %.1f\n%s}",
                 indent, esc(s.track).c_str(), esc(s.trackTitle).c_str(), indent, esc(s.label).c_str(),
                 esc(s.robot).c_str(), esc(s.params).c_str(), indent, esc(s.dev).c_str(), s.livery, indent, s.laps,
                 compoundKey(s.compound), s.fuel, s.wearRate, s.fuelRate, s.ambient, indent);
}

bool TestStore::writeIndex(std::string* err) const {
    const std::string path = dir_ + "/runs.json";
    const std::string tmp = path + ".tmp";
    FILE* f = std::fopen(tmp.c_str(), "w");
    if (!f) {
        if (err) *err = "cannot write " + path;
        return false;
    }
    std::fprintf(f, "{\n  \"about\": \"Every test run: setup and results. Details per run in <folder>/summary.json, "
                    "telemetry in <folder>/telemetry.csv while kept. See README.md.\",\n  \"runs\": [");
    for (size_t i = 0; i < runs_.size(); ++i) {
        const TestRun& r = runs_[i];
        std::fprintf(f, "%s\n    {\"id\": %d, \"date\": \"%s\", \"folder\": \"%s\", \"telemetry\": %s,\n     \"setup\": ",
                     i ? "," : "", r.id, esc(r.date).c_str(), esc(r.folder).c_str(), r.telemetry ? "true" : "false");
        writeSetup(f, r.setup, "     ");
        std::fprintf(f,
                     ",\n     \"end\": \"%s\", \"completed\": %s, \"laps_done\": %d, \"best_lap\": %.3f, "
                     "\"average_lap\": %.3f, \"fuel_per_lap\": %.3f, \"wear_per_lap\": [%.4f, %.4f],\n     \"lap_times\": [",
                     esc(r.end).c_str(), r.completed ? "true" : "false", r.lapsDone, r.best, r.average, r.fuelPerLap,
                     r.wearPerLap[0], r.wearPerLap[1]);
        for (size_t k = 0; k < r.lapTimes.size(); ++k) std::fprintf(f, "%s%.3f", k ? ", " : "", r.lapTimes[k]);
        std::fprintf(f, "]}");
    }
    std::fprintf(f, "\n  ]\n}\n");
    std::fclose(f);
    std::error_code ec;
    fs::rename(tmp, path, ec);
    if (ec) {
        fs::remove(path, ec);
        fs::rename(tmp, path, ec);
    }
    return !ec;
}

void TestStore::writeReadme() const {
    std::ofstream f(dir_ + "/README.md");
    f << kReadme;
}

int TestStore::save(const TestSetup& setup, const TestRecorder& rec, const std::string& end, bool completed,
                    std::string* err) {
    std::error_code ec;
    fs::create_directories(dir_, ec);
    load(nullptr);  // another viewer may have saved since
    int id = 1;
    for (const TestRun& r : runs_) id = std::max(id, r.id + 1);
    char folder[32];
    std::snprintf(folder, sizeof folder, "run_%04d", id);
    const std::string runDir = dir_ + "/" + folder;
    fs::create_directories(runDir, ec);
    if (ec) {
        if (err) *err = "cannot create " + runDir;
        return 0;
    }

    TestRun run;
    run.id = id;
    run.date = nowString();
    run.setup = setup;
    run.lapsDone = (int)rec.laps.size();
    run.completed = completed;
    run.end = end;
    run.best = rec.bestTime();
    run.average = rec.averageLap();
    run.fuelPerLap = rec.fuelPerLap();
    rec.wearPerLap(run.wearPerLap);
    for (const TestLap& l : rec.laps) run.lapTimes.push_back(l.time);
    run.folder = folder;
    run.telemetry = true;

    // telemetry.csv
    FILE* f = std::fopen((runDir + "/telemetry.csv").c_str(), "w");
    if (!f) {
        if (err) *err = "cannot write " + runDir + "/telemetry.csv";
        return 0;
    }
    std::fprintf(f, "%s\n", kCsvHeader);
    for (const TestSample& x : rec.samples) {
        const CarState& s = x.s;
        std::fprintf(f,
                     "%.3f,%d,%.3f,%.1f,%.2f,%.2f,%.4f,%.1f,%.2f,%.2f,%.3f,%.3f,%.4f,%.2f,%.2f,%d,%.0f,%.2f,%.2f,"
                     "%.0f,%.0f,%.0f,%.0f,%.3f,%.3f,%.4f,%.4f,%.3f,%.0f,%.3f,%.4f,%.4f,%.1f,%.1f,%d,%.2f,%.3f,%d,%.2f,%.0f,%.0f,%.3f\n",
                     x.t, x.lap, x.lapTime, x.lapDist, s.pos.x, s.pos.y, s.yaw, std::hypot(s.vx, s.vy) * 3.6f, s.vx,
                     s.vy, s.yawRate, x.steer, s.steerAngle, x.accel, x.brake, s.gear, s.rpm, s.ax, s.ay,
                     s.wheelLoad[0], s.wheelLoad[1], s.wheelLoad[2], s.wheelLoad[3], s.gripUse[0], s.gripUse[1],
                     s.slipAngle[0], s.slipAngle[1], s.wheelSpin, s.damage, s.fuel, s.tireWear[0], s.tireWear[1],
                     s.tireTemp[0], s.tireTemp[1], s.compound, x.lateral, x.angle, x.onTrack ? 1 : 0, s.wheelRot, s.kersCharge, s.kersPowerNow, s.drsFlap);
    }
    std::fclose(f);

    // summary.json
    f = std::fopen((runDir + "/summary.json").c_str(), "w");
    if (!f) {
        if (err) *err = "cannot write " + runDir + "/summary.json";
        return 0;
    }
    const float worst = std::max(run.wearPerLap[0], run.wearPerLap[1]);
    const int bestIdx = rec.bestLap();
    std::fprintf(f, "{\n  \"id\": %d, \"date\": \"%s\", \"end\": \"%s\", \"completed\": %s,\n  \"setup\": ", id,
                 esc(run.date).c_str(), esc(end).c_str(), completed ? "true" : "false");
    writeSetup(f, setup, "  ");
    std::fprintf(f,
                 ",\n  \"results\": {\"laps_done\": %d, \"best_lap\": %.3f, \"best_lap_number\": %d, "
                 "\"average_lap\": %.3f,\n    \"fuel_per_lap\": %.3f, \"wear_per_lap\": [%.4f, %.4f], "
                 "\"tyre_life_laps\": %.1f, \"track_length\": %.1f},\n",
                 run.lapsDone, run.best, bestIdx + 1, run.average, run.fuelPerLap, run.wearPerLap[0],
                 run.wearPerLap[1], worst > 1e-5f ? 0.7f / worst : 0.0f, rec.trackLength);
    std::fprintf(f, "  \"laps\": [");
    for (size_t i = 0; i < rec.laps.size(); ++i) {
        const TestLap& l = rec.laps[i];
        std::fprintf(f,
                     "%s\n    {\"lap\": %d, \"time\": %.3f, \"sectors\": [%.3f, %.3f, %.3f], \"fuel_used\": %.3f, "
                     "\"fuel_end\": %.2f, \"wear\": [%.4f, %.4f], \"wear_end\": [%.4f, %.4f], "
                     "\"temp_avg\": [%.1f, %.1f], \"temp_max\": [%.1f, %.1f], \"top_speed\": %.1f, \"min_speed\": %.1f, "
                     "\"full_throttle\": %.3f, \"braking\": %.3f, \"max_lat_g\": %.2f, \"damage\": %.0f, "
                     "\"off_tracks\": %d, \"slides\": %d, \"clean\": %s}",
                     i ? "," : "", l.lap, l.time, l.sectors[0], l.sectors[1], l.sectors[2], l.fuelUsed, l.fuelEnd,
                     l.wear[0], l.wear[1], l.wearEnd[0], l.wearEnd[1], l.tempAvg[0], l.tempAvg[1], l.tempMax[0],
                     l.tempMax[1], l.topSpeed, l.minSpeed, l.fullThrottle, l.braking, l.maxLatG, l.damage, l.offTracks,
                     l.slides, l.clean ? "true" : "false");
    }
    std::fprintf(f, "\n  ],\n  \"events\": [");
    for (size_t i = 0; i < rec.events.size(); ++i) {
        const TestEvent& e = rec.events[i];
        std::fprintf(f,
                     "%s\n    {\"t\": %.2f, \"lap\": %d, \"lap_dist\": %.0f, \"kind\": \"%s\", \"duration\": %.2f, "
                     "\"peak\": %.3f, \"detail\": \"%s\"}",
                     i ? "," : "", e.t, e.lap, e.lapDist, esc(e.kind).c_str(), e.duration, e.peak, esc(e.detail).c_str());
    }
    std::fprintf(f, "\n  ],\n  \"telemetry\": \"telemetry.csv\"\n}\n");
    std::fclose(f);

    // keep telemetry for the newest runs of this algorithm only
    runs_.push_back(run);
    int kept = 0;
    for (auto it = runs_.rbegin(); it != runs_.rend(); ++it) {
        if (it->setup.label != setup.label || it->setup.robot != setup.robot || !it->telemetry) continue;
        if (++kept <= kKeepTelemetry) continue;
        fs::remove(dir_ + "/" + it->folder + "/telemetry.csv", ec);
        it->telemetry = false;
    }
    if (!writeIndex(err)) return 0;
    writeReadme();
    return id;
}

bool TestStore::loadTelemetry(const TestRun& run, TestRecorder& rec, std::string* err) const {
    const std::string path = dir_ + "/" + run.folder + "/telemetry.csv";
    std::ifstream f(path);
    if (!f) {
        if (err) *err = "no telemetry kept for run " + std::to_string(run.id);
        return false;
    }
    std::string line;
    std::getline(f, line);  // header
    std::vector<TestSample> samples;
    std::vector<float> v;
    while (std::getline(f, line)) {
        v.clear();
        const char* p = line.c_str();
        while (*p) {
            char* e = nullptr;
            v.push_back(std::strtof(p, &e));
            if (e == p) break;
            p = *e == ',' ? e + 1 : e;
        }
        if (v.size() < 39) continue;
        TestSample x;
        int k = 0;
        x.t = v[k++];
        x.lap = (int)v[k++];
        x.lapTime = v[k++];
        x.lapDist = v[k++];
        CarState& s = x.s;
        s.pos.x = v[k++];
        s.pos.y = v[k++];
        s.yaw = v[k++];
        k++;  // speed
        s.vx = v[k++];
        s.vy = v[k++];
        s.yawRate = v[k++];
        x.steer = v[k++];
        s.steerAngle = v[k++];
        x.accel = v[k++];
        x.brake = v[k++];
        s.gear = (int)v[k++];
        s.rpm = v[k++];
        s.ax = v[k++];
        s.ay = v[k++];
        for (int w = 0; w < 4; ++w) s.wheelLoad[w] = v[k++];
        s.gripUse[0] = v[k++];
        s.gripUse[1] = v[k++];
        s.slipAngle[0] = v[k++];
        s.slipAngle[1] = v[k++];
        s.wheelSpin = v[k++];
        s.damage = v[k++];
        s.fuel = v[k++];
        s.tireWear[0] = v[k++];
        s.tireWear[1] = v[k++];
        s.tireTemp[0] = v[k++];
        s.tireTemp[1] = v[k++];
        s.compound = (int)v[k++];
        x.lateral = v[k++];
        x.angle = v[k++];
        x.onTrack = v[k++] != 0;
        s.wheelRot = v[k++];
        if (v.size() >= 42) {  // KERS and DRS columns, absent in older runs
            s.kersCharge = v[k++];
            s.kersPowerNow = v[k++];
            s.drsFlap = v[k++];
        }
        samples.push_back(x);
    }
    if (samples.empty()) {
        if (err) *err = path + ": no samples";
        return false;
    }
    rec = TestRecorder{};
    rec.samples = std::move(samples);
    float length = 0;  // from the summary
    std::ifstream sf(dir_ + "/" + run.folder + "/summary.json");
    if (sf) {
        std::stringstream ss;
        ss << sf.rdbuf();
        length = (float)mjson::parse(ss.str())["results"]["track_length"].num();
    }
    rec.rebuild(length);
    return true;
}

}  // namespace rr
