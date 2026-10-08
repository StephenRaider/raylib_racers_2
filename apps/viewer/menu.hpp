#pragma once
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "championship.hpp"
#include "test_figures.hpp"
#include "vec2.hpp"

// The race setup screen shown before each race: track, race length, tyre life and
// number of cars. Tyre life is chosen in laps; the menu turns it into a wear rate
// using what a calibration lap on that track measured.
struct TrackStats {
    std::string file;          // name in tracks/ (no extension)
    std::string title;         // the track's own name
    float length = 0;          // m
    float lapTime = 60;        // s, a racingline flying lap
    float fuelPerLap = 2.3f;   // l
    float wearPerLap = 0.03f;  // medium tyres at wear rate 1, the faster-wearing axle
    bool measured = false;
    std::vector<rr::Vec2> outline;  // centreline, ~150 points (menu thumbnails)
};

// A driving algorithm the grid page offers: a robot library and its parameters.
struct Algorithm {
    std::string label;   // shown in the menu and used as the car's name
    std::string robot;   // name in bots/ or a path
    std::string params;
    std::string stats;   // the team stats that suit this style ("key=n,..."; "" = all neutral)
};

// The team stats the rules allow (specs/development.json).
struct StatRules {
    std::vector<std::string> keys, labels;
    int budget = 40, min = 0, max = 10, neutral = 5;
    bool empty() const { return keys.empty(); }
    std::vector<int> parse(const std::string& dev) const;   // unknown keys ignored, missing = neutral
    std::string format(const std::vector<int>& v) const;    // "" when all neutral
};

struct MenuState {
    std::vector<TrackStats> tracks;
    int track = 0;
    int laps = 10;
    int tyreLife = 6;  // index into kTyreLives
    int cars = 20, maxCars = 20;
    int teams = 10, drivers = 2;   // the grid: teams x drivers per team (cars is the product)
    int row = 0;       // selected row; the last is Start
    float tankLitres = 65;

    // Grid page: one livery slot, algorithm and starting tyre per car.
    bool gridPage = false;
    int gridRow = 0, gridCol = 0;  // car, and 0 = livery / 1 = algorithm / 2 = tyres
    static constexpr int kGridCols = 3;
    std::vector<int> carLivery, carAlgo;
    std::vector<int> carTires;  // RR_TIRE_*, 0 = the algorithm chooses
    std::vector<Algorithm> algos;
    int liveryCount = 1;

    // Teams: the livery slots of each team (a team's first driver, then its
    // second), and the team's stats. Teammates share them. Picking an algorithm
    // leaves the stats alone; the grid page's style button gives each team the
    // stats of the algorithm it changed most recently.
    std::vector<std::vector<int>> teamSlots;
    std::vector<int> slotTeam;  // livery slot -> team
    StatRules statRules;
    std::vector<std::vector<int>> teamStats;
    bool teamsPage = false;
    int teamRow = 0, statCol = 0;
    int teamOfCar(int car) const {
        const int slot = car < (int)carLivery.size() ? carLivery[car] : car;
        return slot >= 0 && slot < (int)slotTeam.size() ? slotTeam[slot] : -1;
    }
    int statSum(int team) const {
        int n = 0;
        for (int v : teamStats[team]) n += v;
        return n;
    }
    std::vector<int> raceTeams() const;  // teams with a car in the race, in grid order
    // Lays the grid out for teams x drivers: each team's first drivers, then the second ones.
    void layoutGrid();
    // The team takes the stats that suit this car's algorithm.
    void styleChanged(int car);
    // Per team: the car whose algorithm changed last (-1 = none since the last apply).
    std::vector<int> styleCar;
    void algoChanged(int car);
    int stylesPending() const;
    void applyStyles();  // the style button

    static constexpr int kTyreLives[] = {3, 5, 8, 10, 12, 15, 20, 25, 30, 40, 50, 75, 100, 0};  // 0 = no wear
    static constexpr int kNumTyreLives = sizeof(kTyreLives) / sizeof(kTyreLives[0]);
    // Session: 0 race only, 1 weekend (qualifying, each car alone, sets the
    // grid), 2 testing (one car alone, recorded for analysis), 3 championship.
    int session = 0;
    static constexpr int kSessions = 4;
    bool weekend() const { return session == 1; }
    bool testing() const { return session == 2; }
    bool champ() const { return session == 3; }
    int tyreRule = 0;      // two-compound rule: 0 automatic (races over 20 laps), 1 on, 2 off
    // Practice before qualifying (weekends and championship rounds): laps per car, alone.
    static constexpr int kPractice[] = {0, 3, 5, 10, 15, 20, 30};
    static constexpr int kNumPractice = sizeof(kPractice) / sizeof(kPractice[0]);
    int practice = 4;      // index into kPractice (15 laps)
    int practiceLaps() const { return kPractice[practice]; }

    // The rows of the setup page depend on the session.
    enum class Row { Track, Laps, TyreLife, Teams, Drivers, Session, TyreRule, Grid, Stats, Start,
                     TestCar, TestLivery, TestTyres, TestFuel, TestStats, TestRuns,
                     SaveLineup, LoadLineup,
                     ChampName, Round, AddRound, ChampDistance, ChampWear, ChampQuali, Season, Practice };
    std::vector<Row> rows() const;     // the rows shown (RR2 leaves out what it does not support yet)
    std::vector<Row> allRows() const;
    // Raylib Racers 2: one car model and stock 2013 cars, so no liveries or stats to pick, tyres
    // that wear like 2013's, and grand prix distance for weekends and championships.
    bool rr2 = false;
    int rowOf(Row r) const;     // index in rows(), -1 if not shown
    int rowIndex(int row) const;  // which Round / Season a row is (rows of the same kind before it)

    // ---- championship setup: the calendar and the rules for a new season
    std::vector<rr::ChampRound> calendar;  // laps come from the distance and each track's length
    int addTrack = 0;          // track the Add row adds
    int champKm = 4;           // index into kDistances
    int champWear = 3;         // index into kWears
    bool champQuali = true;
    std::string champName = "Season 1";
    static constexpr float kDistances[] = {15, 25, 40, 50, 75, 100, 150, 200, 305};  // km
    static constexpr int kNumDistances = sizeof(kDistances) / sizeof(kDistances[0]);
    static constexpr float kWears[] = {0, 0.5f, 0.75f, 1, 1.5f, 2, 3, 4, 6};          // x the normal wear
    static constexpr int kNumWears = sizeof(kWears) / sizeof(kWears[0]);
    float champDistance() const { return rr2 ? 305.0f : kDistances[champKm]; }
    float champWearRate() const { return rr2 ? 1.0f : kWears[champWear]; }
    const TrackStats* trackStats(const std::string& file) const;
    int roundLaps(const rr::ChampRound& r) const;  // from the distance
    // Laps a compound lasts on a track at the season's wear rate (0 = no wear).
    float compoundLaps(const std::string& file, int compound, float wearRate) const;
    void resetCalendar();  // the eight championship tracks
    // Saved seasons (championships/*.json), filled by the viewer.
    struct SeasonLine {
        std::string file, name, leader, next;
        int done = 0, total = 0;
    };
    std::vector<SeasonLine> seasons;
    int seasonPick = -1;  // the season a ContinueSeason action is for

    // ---- the season page: standings, calendar, next round
    bool seasonPage = false;
    const rr::Championship* season = nullptr;  // set by the viewer while the page is open
    int seasonTab = 0;   // 0 drivers, 1 constructors

    // ---- lineups: save (name it) and load (pick one)
    bool lineupSave = false, lineupLoad = false;
    bool champNaming = false;         // typing the new season's name
    bool typing() const { return lineupSave || champNaming; }
    std::string inputText;            // the name being typed
    std::vector<std::string> lineupFiles;  // names in lineups/, filled by the viewer
    int lineupRow = 0;
    std::string lineupPick;           // the lineup a LoadLineup action is for
    std::string toast;                // a short message at the bottom of the menu
    double toastUntil = 0;

    // ---- a drop-down list: the target is a setup row, or a grid cell (100 + ...)
    int popup = -1;
    int popupSel = 0, popupTop = 0;
    float popupX = 0, popupY = 0, popupW = 0;
    std::vector<std::string> popupOptions() const;
    void openPopup(int target, float x, float y, float w);
    void choose(int target, int option);  // sets the value the popup was for

    // ---- testing: one car, its stats, tyres and fuel (0 = chosen for the run length)
    int testAlgo = 0, testLivery = 0, testTires = 0;
    float testFuel = 0;
    std::vector<int> testStats;
    bool testStatsPage = false;
    int testStatRow = 0;
    // Filled in by the viewer: the automatic choices and what the stats do.
    int autoTires = 2;           // RR_TIRE_MEDIUM
    float autoFuel = 0;          // litres
    float fuelPerLapEst = 2.3f;  // with this car's stats
    float compoundLife[4] = {0, 0, 0, 0};  // laps per compound (index RR_TIRE_*), 0 = no wear
    std::vector<FigureLine> figures;
    std::vector<std::string> statAbout;
    int testTiresUsed() const { return testTires ? testTires : autoTires; }
    float testFuelUsed() const { return testFuel > 0 ? testFuel : autoFuel; }
    // Saved runs page.
    struct RunLine {
        int id = 0;
        std::string date, track, algo, stats, end;
        int compound = 0, laps = 0, lapsDone = 0;
        float fuel = 0, best = 0, average = 0, fuelPerLap = 0, wearPerLap = 0;
        bool telemetry = false, completed = false;
    };
    bool runsPage = false;
    int runsSort = 0;            // 0 newest first, 1 best lap first
    bool runsAllTracks = false;  // false: this track only
    int runsRow = 0, runsTop = 0;
    int runsTotal = 0;           // runs saved, all tracks
    std::vector<RunLine> runLines;  // filled by the viewer: filtered and sorted
    int runPick = 0;             // id of the run a LoadRun / ViewRun action is for

    const TrackStats& stats() const { return tracks[track]; }
    // Laps the medium tyre lasts at wear rate 1 on this track (to the 0.7 wear cliff).
    float baseTyreLife() const { return 0.7f / std::max(1e-4f, stats().wearPerLap); }
    // Laps the medium tyre lasts in this session (0 = no wear). RR2: the real wear, not a choice.
    float tyreLifeLaps() const { return rr2 ? baseTyreLife() : (float)kTyreLives[tyreLife]; }
    float wearRate() const {
        const float life = tyreLifeLaps();
        return life == 0 ? 0.0f : baseTyreLife() / life;
    }
    // A grand prix on this track: the fewest whole laps over 305 km.
    int gpLaps() const { return stats().length > 500 ? std::max(1, (int)std::ceil(305000.0f / stats().length)) : laps; }
    float lapsPerTank() const { return tankLitres / std::max(0.1f, stats().fuelPerLap); }
    // Picks the tyre-life option nearest the given wear rate.
    void setWearRate(float rate);
    bool twoCompoundRule() const { return tyreRule == 1 || (tyreRule == 0 && laps > 20); }
    int twoCompoundsArg() const { return tyreRule == 0 ? -1 : tyreRule == 1 ? 1 : 0; }  // RaceConfig::twoCompounds
};

enum class MenuAction { None, Start, Quit, TrackChanged, LoadRun, ViewRun,
                        SaveLineup, LoadLineup, ListLineups, NewSeason, ContinueSeason, StartRound, LeaveSeason,
                        ChampTab };

// Keyboard and mouse input for the menu. `hits` holds the clickable rectangles the
// last draw produced (row arrows and the start button).
// row: setup row, or 100 + car * kGridCols + column on the grid page, or
// 1000 + team * 16 + stat on the team stats page, 2000 + stat on the testing
// stats page, 3000 + line on the saved runs page; 99 is a page's Done button,
// 98 / 97 load / replay a saved run, 96 / 95 its sort / track filter.
// dir -1 / +1 = arrow, 0 = select.
// On the setup page, value >= 0 picks an option of a row (tabs, segmented
// buttons) and dir 2 opens the row's drop-down list. 4000 + round * 4 + k:
// remove / move up / move down a calendar round; 4500 + i continues saved season i;
// 5000 + i picks drop-down option i (5999 closes it); 6000 / 6001 confirm / cancel
// the name being typed; 6100 + i picks lineup i (6098 loads it, 6099 cancels);
// 7000 / 7001 start the round / leave the season page, 7010 + k its tabs.
struct MenuHit { float x, y, w, h; int row; int dir; int value = -1; };
MenuAction updateMenu(MenuState& m, const std::vector<MenuHit>& hits);
