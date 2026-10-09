#pragma once
// Championships: a season of races on a calendar of tracks, scored with the modern F1
// points (25-18-15-12-10-8-6-4-2-1, no fastest-lap point). The lineup is fixed for the
// whole season, team stats included. Saved as JSON so a season can be resumed.
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "config.hpp"

namespace rr {

class Race;

// A saved grid: the teams, each team's stats and its drivers (algorithms).
struct LineupDriver {
    std::string name;    // display name
    std::string robot;   // name in bots/ or a path
    std::string params;
    int tires = 0;       // starting compound (RR_TIRE_*), 0 = the algorithm chooses
    int livery = -1;     // the viewer's livery slot (-1 = the team's)
};
struct LineupTeam {
    std::string name;
    int livery = -1;     // livery slot in the viewer's teams.json (-1 = by order)
    std::string stats;   // team stats, "key=n,..." ("" = all neutral)
    std::vector<LineupDriver> drivers;
};
struct Lineup {
    std::string name;
    std::vector<LineupTeam> teams;

    int driverCount() const;
    // Driver ids number the drivers team by team: team 0's drivers, then team 1's...
    const LineupDriver& driver(int id) const;
    int teamOf(int id) const;
    EntrySpec entry(int id) const;  // the driver's car, with the team's stats
    // The first drivers of every team, then the second ones (the viewer's default grid).
    std::vector<int> defaultGrid() const;
    // Pairs consecutive cars into teams; each team takes its first car's stats.
    static Lineup fromEntries(const std::vector<EntrySpec>& entries, int perTeam = 2);

    bool save(const std::string& path, std::string* err) const;
    static bool load(const std::string& path, Lineup& out, std::string* err);
    std::string toJson(int indent) const;
    bool fromJson(const std::string& text, std::string* err);
};

struct ChampRound {
    std::string track;  // name in tracks/
    int laps = 10;
};

// One race's classification. Driver ids by finishing position.
struct RoundResult {
    std::vector<int> grid;          // driver ids by grid slot
    std::vector<int> order;         // every driver, by position
    std::vector<int> points;        // per position
    std::vector<std::string> status;  // per position: "finished", "+1 lap", "dnf ..."
    std::vector<float> time;        // per position: race time (finishers), else 0
    int fastest = -1;               // driver id with the fastest lap
    float fastestLap = 0;
};

struct Standing {
    int id = 0;                // driver id, or team index
    int points = 0;
    std::vector<int> places;   // count of 1st, 2nd, ... places (countback)
    int best = 0;              // best finish (1-based, 0 = none)
};

struct Championship {
    std::string name = "Championship";
    Lineup lineup;
    std::vector<ChampRound> rounds;
    std::vector<RoundResult> results;  // one per round raced, in calendar order
    // Rules, the same for every round. One tyre wear rate for the whole season: how many
    // laps a tyre lasts then depends on the track.
    float wearRate = 1.0f;
    float distanceKm = 0;     // > 0: each round's laps were set to cover about this distance
    int twoCompounds = -1;    // RaceConfig::twoCompounds
    bool qualifying = false;  // a qualifying session sets each grid
    int practiceLaps = 0;     // practice before each round, laps per car (0 = none)
    bool sandbox = false;     // competition rules: robots in their own locked-down processes
    float cpuCapMs = 0;       // ...and a CPU cap per drive() call (0 = none)
    uint64_t seed = 1;
    // General Championship: the teams' files are locked when the season starts. gcLock holds
    // (team id, digest of its files); if any digest changes, gcInvalid is set for good.
    std::vector<std::pair<std::string, std::string>> gcLock;
    bool gcInvalid = false;
    std::string gcInvalidWhy;

    static int pointsFor(int position);  // 1-based; 0 outside the top ten
    // Laps that cover about km on a track of this length (m), at least 1.
    static int lapsFor(float km, float trackLength);
    // Circuit Raylib and the seven F1-inspired tracks.
    static std::vector<ChampRound> defaultCalendar(int laps);
    // "monza:10,spa:8" or "monza,spa" (laps from defLaps); empty = the default calendar.
    static bool parseRounds(const std::string& s, int defLaps, std::vector<ChampRound>& out, std::string* err);

    int roundsDone() const { return (int)results.size(); }
    bool over() const { return roundsDone() >= (int)rounds.size(); }
    // Grid for the next round when there is no qualifying: the lineup's order for the
    // first round, then the championship order.
    std::vector<int> nextGrid() const;
    // The race config for the next round: base with this round's track, laps, cars
    // (in grid order) and rules.
    RaceConfig roundConfig(const RaceConfig& base, const std::vector<int>& grid) const;
    // Records a finished race whose cars were entered in grid order.
    void record(const Race& race, const std::vector<int>& grid);

    std::vector<Standing> driverStandings() const;
    std::vector<Standing> teamStandings() const;  // constructors: both cars score
    // Points a driver scored in a round (-1 = not raced yet).
    int roundPoints(int round, int id) const;
    int roundPlace(int round, int id) const;  // 1-based, 0 = not raced

    bool save(const std::string& path, std::string* err) const;
    static bool load(const std::string& path, Championship& out, std::string* err);
    void printStandings(FILE* f) const;
};

}  // namespace rr
