#include "championship.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>

#include "mini_json.hpp"
#include "race.hpp"

namespace rr {

namespace {

std::string esc(const std::string& s) {
    std::string o;
    for (char c : s) {
        if (c == '"' || c == '\\') o += '\\', o += c;
        else if (c == '\n') o += "\\n";
        else if (c == '\t') o += "\\t";
        else if ((unsigned char)c < 0x20) o += ' ';
        else o += c;
    }
    return o;
}
std::string q(const std::string& s) { return "\"" + esc(s) + "\""; }

bool readFile(const std::string& path, std::string& text, std::string* err) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        if (err) *err = "cannot read " + path;
        return false;
    }
    std::stringstream ss;
    ss << f.rdbuf();
    text = ss.str();
    return true;
}
bool writeFile(const std::string& path, const std::string& text, std::string* err) {
    // write a temporary file, then replace: a crash never leaves half a season
    const std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary);
        if (!f || !(f << text)) {
            if (err) *err = "cannot write " + path;
            return false;
        }
    }
    std::remove(path.c_str());
    if (std::rename(tmp.c_str(), path.c_str()) != 0) {
        if (err) *err = "cannot write " + path;
        return false;
    }
    return true;
}

// a before b in the standings: points, then countback (most wins, most seconds, ...)
bool ahead(const Standing& a, const Standing& b) {
    if (a.points != b.points) return a.points > b.points;
    for (size_t k = 0; k < std::max(a.places.size(), b.places.size()); ++k) {
        const int x = k < a.places.size() ? a.places[k] : 0, y = k < b.places.size() ? b.places[k] : 0;
        if (x != y) return x > y;
    }
    return a.id < b.id;
}

}  // namespace

// ---------------------------------------------------------------- lineup

int Lineup::driverCount() const {
    int n = 0;
    for (const auto& t : teams) n += (int)t.drivers.size();
    return n;
}

const LineupDriver& Lineup::driver(int id) const {
    for (const auto& t : teams) {
        if (id < (int)t.drivers.size()) return t.drivers[id];
        id -= (int)t.drivers.size();
    }
    static const LineupDriver none;
    return none;
}

int Lineup::teamOf(int id) const {
    for (int k = 0; k < (int)teams.size(); ++k) {
        if (id < (int)teams[k].drivers.size()) return k;
        id -= (int)teams[k].drivers.size();
    }
    return -1;
}

EntrySpec Lineup::entry(int id) const {
    const LineupDriver& d = driver(id);
    EntrySpec e;
    e.robot = d.robot;
    e.params = d.params;
    e.name = d.name;
    e.tires = d.tires;
    const int t = teamOf(id);
    if (t >= 0) e.dev = teams[t].stats;
    return e;
}

std::vector<int> Lineup::defaultGrid() const {
    std::vector<int> grid, first;
    int id = 0;
    for (const auto& t : teams) {
        first.push_back(id);
        id += (int)t.drivers.size();
    }
    for (size_t seat = 0;; ++seat) {
        bool any = false;
        for (size_t k = 0; k < teams.size(); ++k)
            if (seat < teams[k].drivers.size()) grid.push_back(first[k] + (int)seat), any = true;
        if (!any) break;
    }
    return grid;
}

Lineup Lineup::fromEntries(const std::vector<EntrySpec>& entries, int perTeam) {
    Lineup l;
    perTeam = std::max(1, perTeam);
    for (size_t i = 0; i < entries.size(); ++i) {
        if (i % perTeam == 0) {
            l.teams.emplace_back();
            l.teams.back().name = "Team " + std::to_string(l.teams.size());
            l.teams.back().stats = entries[i].dev;
        }
        const EntrySpec& e = entries[i];
        l.teams.back().drivers.push_back({e.name.empty() ? e.robot : e.name, e.robot, e.params, e.tires, -1});
    }
    return l;
}

std::string Lineup::toJson(int indent) const {
    const std::string p(indent, ' ');
    std::string s = "{\n" + p + "  \"name\": " + q(name) + ",\n" + p + "  \"teams\": [";
    for (size_t k = 0; k < teams.size(); ++k) {
        const LineupTeam& t = teams[k];
        s += k ? ",\n" : "\n";
        s += p + "    {\"name\": " + q(t.name) + ", \"livery\": " + std::to_string(t.livery) + ", \"stats\": " +
             q(t.stats) + ", \"drivers\": [";
        for (size_t d = 0; d < t.drivers.size(); ++d) {
            const LineupDriver& v = t.drivers[d];
            s += (d ? ",\n" : "\n") + p + "      {\"name\": " + q(v.name) + ", \"robot\": " + q(v.robot) +
                 ", \"params\": " + q(v.params) + ", \"tires\": " + std::to_string(v.tires) +
                 ", \"livery\": " + std::to_string(v.livery) + "}";
        }
        s += "]}";
    }
    s += "\n" + p + "  ]\n" + p + "}";
    return s;
}

static bool lineupFrom(const mjson::Value& v, Lineup& l, std::string* err) {
    if (v.type != mjson::Value::Object || v["teams"].type != mjson::Value::Array) {
        if (err) *err = "not a lineup (no \"teams\" list)";
        return false;
    }
    l = Lineup{};
    l.name = v["name"].str();
    for (const auto& tv : v["teams"].arr) {
        LineupTeam t;
        t.name = tv["name"].str();
        t.livery = (int)tv["livery"].num(-1);
        t.stats = tv["stats"].str();
        for (const auto& dv : tv["drivers"].arr) {
            LineupDriver d;
            d.name = dv["name"].str();
            d.robot = dv["robot"].str();
            d.params = dv["params"].str();
            d.tires = (int)dv["tires"].num(0);
            d.livery = (int)dv["livery"].num(-1);
            if (d.robot.empty()) {
                if (err) *err = "a driver of team \"" + t.name + "\" has no robot";
                return false;
            }
            t.drivers.push_back(d);
        }
        l.teams.push_back(t);
    }
    if (l.driverCount() == 0) {
        if (err) *err = "the lineup has no drivers";
        return false;
    }
    return true;
}

bool Lineup::fromJson(const std::string& text, std::string* err) {
    return lineupFrom(mjson::parse(text), *this, err);
}

bool Lineup::save(const std::string& path, std::string* err) const {
    return writeFile(path, toJson(0) + "\n", err);
}

bool Lineup::load(const std::string& path, Lineup& out, std::string* err) {
    std::string text;
    if (!readFile(path, text, err)) return false;
    if (!out.fromJson(text, err)) {
        if (err) *err = path + ": " + *err;
        return false;
    }
    return true;
}

// ---------------------------------------------------------------- championship

int Championship::pointsFor(int position) {
    static const int kPoints[] = {25, 18, 15, 12, 10, 8, 6, 4, 2, 1};
    return position >= 1 && position <= 10 ? kPoints[position - 1] : 0;
}

int Championship::lapsFor(float km, float trackLength) {
    return std::max(1, (int)std::lround(km * 1000.0f / std::max(1.0f, trackLength)));
}

std::vector<ChampRound> Championship::defaultCalendar(int laps) {
    std::vector<ChampRound> r;
    for (const char* t : {"circuit", "sepang", "brands", "silverstone", "hungaroring", "spa", "zandvoort", "monza"})
        r.push_back({t, laps});
    return r;
}

bool Championship::parseRounds(const std::string& s, int defLaps, std::vector<ChampRound>& out, std::string* err) {
    out.clear();
    if (s.empty()) {
        out = defaultCalendar(defLaps);
        return true;
    }
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, ',')) {
        if (item.empty()) continue;
        ChampRound r;
        const size_t c = item.find(':');
        r.track = item.substr(0, c);
        r.laps = defLaps;
        if (c != std::string::npos) {
            r.laps = std::atoi(item.c_str() + c + 1);
            if (r.laps < 1) {
                if (err) *err = "bad laps in round \"" + item + "\"";
                return false;
            }
        }
        out.push_back(r);
    }
    if (out.empty()) {
        if (err) *err = "no rounds";
        return false;
    }
    return true;
}

std::vector<int> Championship::nextGrid() const {
    if (results.empty()) return lineup.defaultGrid();
    std::vector<int> grid;
    for (const Standing& s : driverStandings()) grid.push_back(s.id);
    return grid;
}

RaceConfig Championship::roundConfig(const RaceConfig& base, const std::vector<int>& grid) const {
    RaceConfig c = base;
    const ChampRound& r = rounds[std::min(roundsDone(), (int)rounds.size() - 1)];
    c.track = r.track;
    c.laps = r.laps;
    c.entries.clear();
    for (int id : grid) c.entries.push_back(lineup.entry(id));
    c.wearRate = wearRate;
    c.twoCompounds = twoCompounds;
    c.seed = seed + (uint64_t)roundsDone();
    c.pitsClosed = false;
    c.fuelLimit = 0;
    c.sandbox = base.sandbox || sandbox;
    if (cpuCapMs > 0) c.cpuCapMs = cpuCapMs;
    return c;
}

void Championship::record(const Race& race, const std::vector<int>& grid) {
    RoundResult r;
    r.grid = grid;
    const int laps = race.laps();
    const auto& cars = race.cars();
    int pos = 0;
    for (int ci : race.order()) {
        const Car& c = cars[ci];
        const int id = ci < (int)grid.size() ? grid[ci] : ci;
        // classified: finished, or still running with 90% of the distance done
        const bool classified = c.finished || (!c.dnf && c.lapsDone * 10 >= laps * 9);
        ++pos;
        r.order.push_back(id);
        r.points.push_back(classified ? pointsFor(pos) : 0);
        const int down = std::max(1, laps - c.lapsDone);
        if (c.finished && c.lapsDone >= laps) r.status.push_back("finished");
        else if (classified) r.status.push_back("+" + std::to_string(down) + (down > 1 ? " laps" : " lap"));
        else r.status.push_back(c.dnf ? "dnf " + c.dnfReason : "not classified");
        r.time.push_back(c.finished ? (float)c.raceTime() : 0.0f);
        if (c.bestLap > 0 && (r.fastest < 0 || c.bestLap < r.fastestLap)) {
            r.fastest = id;
            r.fastestLap = c.bestLap;
        }
    }
    results.push_back(r);
}

std::vector<Standing> Championship::driverStandings() const {
    const int n = lineup.driverCount();
    std::vector<Standing> s(n);
    for (int i = 0; i < n; ++i) s[i].id = i, s[i].places.assign(n, 0);
    for (const RoundResult& r : results) {
        for (size_t p = 0; p < r.order.size(); ++p) {
            const int id = r.order[p];
            if (id < 0 || id >= n) continue;
            s[id].points += r.points[p];
            if (r.status[p].rfind("dnf", 0) != 0 && r.status[p] != "not classified") {
                s[id].places[p]++;
                if (!s[id].best || (int)p + 1 < s[id].best) s[id].best = (int)p + 1;
            }
        }
    }
    std::sort(s.begin(), s.end(), ahead);
    return s;
}

std::vector<Standing> Championship::teamStandings() const {
    const int n = (int)lineup.teams.size(), cars = lineup.driverCount();
    std::vector<Standing> s(n);
    for (int i = 0; i < n; ++i) s[i].id = i, s[i].places.assign(cars, 0);
    for (const RoundResult& r : results) {
        for (size_t p = 0; p < r.order.size(); ++p) {
            const int t = lineup.teamOf(r.order[p]);
            if (t < 0) continue;
            s[t].points += r.points[p];
            if (r.status[p].rfind("dnf", 0) != 0 && r.status[p] != "not classified") {
                s[t].places[p]++;
                if (!s[t].best || (int)p + 1 < s[t].best) s[t].best = (int)p + 1;
            }
        }
    }
    std::sort(s.begin(), s.end(), ahead);
    return s;
}

int Championship::roundPoints(int round, int id) const {
    if (round < 0 || round >= roundsDone()) return -1;
    const RoundResult& r = results[round];
    for (size_t p = 0; p < r.order.size(); ++p)
        if (r.order[p] == id) return r.points[p];
    return 0;
}

int Championship::roundPlace(int round, int id) const {
    if (round < 0 || round >= roundsDone()) return 0;
    const RoundResult& r = results[round];
    for (size_t p = 0; p < r.order.size(); ++p)
        if (r.order[p] == id) return (int)p + 1;
    return 0;
}

bool Championship::save(const std::string& path, std::string* err) const {
    std::string s = "{\n  \"format\": 1,\n  \"name\": " + q(name) + ",\n";
    char buf[512];
    std::snprintf(buf, sizeof buf,
                  "  \"distance_km\": %.1f,\n  \"wear_rate\": %.4f,\n  \"two_compounds\": %d,\n  \"qualifying\": %s,\n"
                  "  \"practice_laps\": %d,\n  \"sandbox\": %s,\n  \"cpu_cap_ms\": %g,\n  \"seed\": %llu,\n",
                  distanceKm, wearRate, twoCompounds, qualifying ? "true" : "false", practiceLaps,
                  sandbox ? "true" : "false", (double)cpuCapMs, (unsigned long long)seed);
    s += buf;
    if (!gcLock.empty() || gcInvalid) {
        s += "  \"gc_lock\": {";
        for (size_t k = 0; k < gcLock.size(); ++k)
            s += (k ? ", " : "") + q(gcLock[k].first) + ": " + q(gcLock[k].second);
        s += std::string("},\n  \"gc_invalid\": ") + (gcInvalid ? "true" : "false") + ",\n  \"gc_invalid_why\": " +
             q(gcInvalidWhy) + ",\n";
    }
    s += "  \"lineup\": " + lineup.toJson(2) + ",\n  \"rounds\": [";
    for (size_t k = 0; k < rounds.size(); ++k)
        s += (k ? ", " : "") + std::string("{\"track\": ") + q(rounds[k].track) + ", \"laps\": " +
             std::to_string(rounds[k].laps) + "}";
    s += "],\n  \"results\": [";
    for (size_t k = 0; k < results.size(); ++k) {
        const RoundResult& r = results[k];
        auto ints = [](const std::vector<int>& v) {
            std::string o = "[";
            for (size_t i = 0; i < v.size(); ++i) o += (i ? ", " : "") + std::to_string(v[i]);
            return o + "]";
        };
        s += k ? ",\n" : "\n";
        s += "    {\"grid\": " + ints(r.grid) + ", \"order\": " + ints(r.order) + ", \"points\": " + ints(r.points) +
             ", \"status\": [";
        for (size_t i = 0; i < r.status.size(); ++i) s += (i ? ", " : "") + q(r.status[i]);
        s += "], \"time\": [";
        for (size_t i = 0; i < r.time.size(); ++i) {
            std::snprintf(buf, sizeof buf, "%s%.3f", i ? ", " : "", r.time[i]);
            s += buf;
        }
        std::snprintf(buf, sizeof buf, "], \"fastest\": %d, \"fastest_lap\": %.3f}", r.fastest, r.fastestLap);
        s += buf;
    }
    s += results.empty() ? "]\n}\n" : "\n  ]\n}\n";
    return writeFile(path, s, err);
}

bool Championship::load(const std::string& path, Championship& out, std::string* err) {
    std::string text;
    if (!readFile(path, text, err)) return false;
    const mjson::Value v = mjson::parse(text);
    if (v.type != mjson::Value::Object || v["rounds"].type != mjson::Value::Array) {
        if (err) *err = path + ": not a championship file";
        return false;
    }
    Championship c;
    c.name = v["name"].str();
    c.distanceKm = (float)v["distance_km"].num(0);
    c.wearRate = (float)v["wear_rate"].num(1);
    c.twoCompounds = (int)v["two_compounds"].num(-1);
    c.qualifying = v["qualifying"].b;
    c.practiceLaps = (int)v["practice_laps"].num(0);
    c.sandbox = v["sandbox"].b;
    c.cpuCapMs = (float)v["cpu_cap_ms"].num(0);
    c.seed = (uint64_t)v["seed"].num(1);
    for (const auto& kv : v["gc_lock"].obj) c.gcLock.emplace_back(kv.first, kv.second.str());
    c.gcInvalid = v["gc_invalid"].b;
    c.gcInvalidWhy = v["gc_invalid_why"].str();
    if (!lineupFrom(v["lineup"], c.lineup, err)) {
        if (err) *err = path + ": " + *err;
        return false;
    }
    for (const auto& rv : v["rounds"].arr) c.rounds.push_back({rv["track"].str(), (int)rv["laps"].num(10)});
    if (c.rounds.empty()) {
        if (err) *err = path + ": no rounds";
        return false;
    }
    auto ints = [](const mjson::Value& a) {
        std::vector<int> o;
        for (const auto& x : a.arr) o.push_back((int)x.num());
        return o;
    };
    for (const auto& rv : v["results"].arr) {
        RoundResult r;
        r.grid = ints(rv["grid"]);
        r.order = ints(rv["order"]);
        r.points = ints(rv["points"]);
        for (const auto& x : rv["status"].arr) r.status.push_back(x.str());
        for (const auto& x : rv["time"].arr) r.time.push_back((float)x.num());
        r.fastest = (int)rv["fastest"].num(-1);
        r.fastestLap = (float)rv["fastest_lap"].num(0);
        if (r.points.size() != r.order.size() || r.status.size() != r.order.size()) {
            if (err) *err = path + ": a round's results are incomplete";
            return false;
        }
        r.time.resize(r.order.size(), 0.0f);
        c.results.push_back(r);
    }
    out = c;
    return true;
}

void Championship::printStandings(FILE* f) const {
    std::fprintf(f, "\n%s after round %d of %d\n", name.c_str(), roundsDone(), (int)rounds.size());
    std::fprintf(f, "  Drivers\n");
    int p = 0;
    for (const Standing& s : driverStandings()) {
        const int t = lineup.teamOf(s.id);
        std::fprintf(f, "  %2d. %-24s %-16s %4d\n", ++p, lineup.driver(s.id).name.c_str(),
                     t >= 0 ? lineup.teams[t].name.c_str() : "", s.points);
    }
    std::fprintf(f, "  Constructors\n");
    p = 0;
    for (const Standing& s : teamStandings())
        std::fprintf(f, "  %2d. %-41s %4d\n", ++p, lineup.teams[s.id].name.c_str(), s.points);
}

}  // namespace rr
