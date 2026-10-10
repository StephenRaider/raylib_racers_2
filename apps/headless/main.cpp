// rr_race: runs a race without graphics as fast as the CPU allows.
#include <chrono>
#include <cstdio>

#include "../../src/core/championship.hpp"
#include "../../src/core/race.hpp"
#include "../../src/core/testlog.hpp"
#include "../../src/core/weekend.hpp"

#ifndef RR_SOURCE_DIR
#define RR_SOURCE_DIR "."
#endif

static std::string lapStr(float t) {
    char b[32];
    std::snprintf(b, sizeof b, "%d:%06.3f", (int)(t / 60), t - 60 * (int)(t / 60));
    return b;
}

// The weekend before a race: gives every car its weekend memory, runs practice
// (each car alone) and qualifying (each car alone, the times set the grid).
// Reorders rc.entries, and `ids` alongside them, into the grid. False on error.
static bool runWeekend(rr::RaceConfig& rc, std::vector<int>& ids, int practiceLaps, bool qualifying,
                       const std::vector<std::string>& bots, const std::vector<std::string>& tracks) {
    rr::startWeekend(rc.entries);
    if (rc.rubber) rc.rubberMap = std::make_shared<rr::TrackRubber>();  // practice, qualifying and the race share the rubber
    std::string err;
    auto label = [&](size_t i) { return rc.entries[i].name.empty() ? rc.entries[i].robot : rc.entries[i].name; };
    if (practiceLaps > 0) {
        std::printf("Practice, up to %d laps each\n", practiceLaps);
        for (size_t i = 0; i < rc.entries.size(); ++i) {
            const float best = rr::runAlone(rr::practiceConfig(rc, rc.entries[i], practiceLaps), bots, tracks, &err);
            if (best < 0) {
                std::fprintf(stderr, "error: practice: %s\n", err.c_str());
                return false;
            }
            std::printf("  %-24s best %s\n", label(i).c_str(), best > 0 ? lapStr(best).c_str() : "no time");
        }
    }
    if (!qualifying) return true;
    std::printf("Qualifying\n");
    std::vector<float> times;
    for (size_t i = 0; i < rc.entries.size(); ++i) {
        const float best = rr::runAlone(rr::qualiConfig(rc, rc.entries[i], 0), bots, tracks, &err);
        if (best < 0) {
            std::fprintf(stderr, "error: qualifying: %s\n", err.c_str());
            return false;
        }
        times.push_back(best);
    }
    std::vector<rr::EntrySpec> entries;
    std::vector<int> grid;
    int p = 0;
    for (int i : rr::gridOrder(times)) {
        std::printf("  %2d. %-24s %s\n", ++p, label(i).c_str(), times[i] > 0 ? lapStr(times[i]).c_str() : "no time");
        entries.push_back(rc.entries[i]);
        grid.push_back(ids[i]);
    }
    rc.entries = entries;
    ids = grid;
    return true;
}

// --championship: races the season's next round (or all of them) and saves the file.
static int runChampionship(const rr::RaceConfig& cfg, const std::vector<std::string>& bots,
                           const std::vector<std::string>& tracks) {
    std::string err;
    rr::Championship ch;
    if (std::FILE* f = std::fopen(cfg.championship.c_str(), "r")) {
        std::fclose(f);
        if (!rr::Championship::load(cfg.championship, ch, &err)) {
            std::fprintf(stderr, "error: %s\n", err.c_str());
            return 1;
        }
    } else {
        if (!cfg.lineup.empty()) {
            if (!rr::Lineup::load(cfg.lineup, ch.lineup, &err)) {
                std::fprintf(stderr, "error: %s\n", err.c_str());
                return 1;
            }
        } else if (!cfg.entries.empty()) {
            ch.lineup = rr::Lineup::fromEntries(cfg.entries);
        } else {
            std::fprintf(stderr, "error: a new championship needs --lineup FILE or --car entries\n");
            return 2;
        }
        if (!rr::Championship::parseRounds(cfg.rounds, cfg.laps, ch.rounds, &err)) {
            std::fprintf(stderr, "error: --rounds: %s\n", err.c_str());
            return 2;
        }
        if (cfg.distance > 0) {
            ch.distanceKm = cfg.distance;
            for (rr::ChampRound& r : ch.rounds) {
                rr::Track t;
                const std::string path = rr::trackFile(r.track, tracks);
                if (path.empty() || !t.load(path, &err)) {
                    std::fprintf(stderr, "error: track '%s': %s\n", r.track.c_str(), path.empty() ? "not found" : err.c_str());
                    return 1;
                }
                r.laps = rr::Championship::lapsFor(cfg.distance, t.length());
            }
        }
        ch.wearRate = cfg.wearRate;
        ch.twoCompounds = cfg.twoCompounds;
        ch.qualifying = cfg.qualifying;
        ch.practiceLaps = cfg.practiceLaps;
        ch.sandbox = cfg.sandbox;
        ch.cpuCapMs = cfg.cpuCapMs;
        ch.seed = cfg.seed;
    }
    if (ch.over()) {
        std::printf("%s is over\n", ch.name.c_str());
        ch.printStandings(stdout);
        return 0;
    }
    do {
        std::vector<int> grid = ch.nextGrid();
        rr::RaceConfig rc = ch.roundConfig(cfg, grid);
        if ((ch.practiceLaps > 0 || ch.qualifying) && !runWeekend(rc, grid, ch.practiceLaps, ch.qualifying, bots, tracks))
            return 1;
        rr::Race race;
        if (!race.setup(rc, bots, tracks, &err)) {
            std::fprintf(stderr, "error: round %d: %s\n", ch.roundsDone() + 1, err.c_str());
            return 1;
        }
        std::printf("Round %d of %d: %s, %d laps\n", ch.roundsDone() + 1, (int)ch.rounds.size(),
                    race.track().name().c_str(), rc.laps);
        while (!race.isOver()) race.step();
        ch.record(race, grid);
        const rr::RoundResult& r = ch.results.back();
        for (size_t p = 0; p < r.order.size(); ++p)
            std::printf("  %2zu. %-24s %-16s %3d\n", p + 1, ch.lineup.driver(r.order[p]).name.c_str(),
                        r.status[p].c_str(), r.points[p]);
        if (r.fastest >= 0)
            std::printf("  fastest lap: %s %.3f\n", ch.lineup.driver(r.fastest).name.c_str(), r.fastestLap);
        if (!ch.save(cfg.championship, &err)) {
            std::fprintf(stderr, "error: %s\n", err.c_str());
            return 1;
        }
    } while (cfg.allRounds && !ch.over());
    ch.printStandings(stdout);
    return 0;
}

int main(int argc, char** argv) {
    rr::RaceConfig cfg;
    bool help = false;
    std::string err;
    if (!rr::parseArgs(argc, argv, cfg, false, help, &err)) {
        std::fprintf(stderr, "error: %s\n\n%s", err.c_str(), rr::usage(argv[0], false).c_str());
        return 2;
    }
    if (help) {
        std::printf("%s", rr::usage(argv[0], false).c_str());
        return 0;
    }
    const std::string dir = rr::exeDir(argv[0]);
    cfg.botHost = rr::botHostPath(dir);
    const std::vector<std::string> botDirs = {dir + "/bots", dir, "bots", "."};
    const std::vector<std::string> trackDirs = {dir + "/tracks", RR_SOURCE_DIR "/tracks", "tracks"};
    if (!cfg.saveLineup.empty()) {
        if (cfg.entries.empty()) {
            std::fprintf(stderr, "error: --save-lineup needs --car entries\n");
            return 2;
        }
        if (!rr::Lineup::fromEntries(cfg.entries).save(cfg.saveLineup, &err)) {
            std::fprintf(stderr, "error: %s\n", err.c_str());
            return 1;
        }
        std::printf("lineup saved in %s\n", cfg.saveLineup.c_str());
        return 0;
    }
    if (!cfg.championship.empty()) return runChampionship(cfg, botDirs, trackDirs);
    if (cfg.entries.empty()) {
        for (const char* r : {"racingline", "gapfollow", "simple"}) {
            rr::EntrySpec e;
            e.robot = r;
            cfg.entries.push_back(e);
        }
        if (!cfg.quiet) std::printf("no --car given, racing the example robots\n");
    }

    if (cfg.practiceLaps > 0 || cfg.qualifying) {
        std::vector<int> ids(cfg.entries.size());
        for (size_t i = 0; i < ids.size(); ++i) ids[i] = (int)i;
        if (!runWeekend(cfg, ids, cfg.practiceLaps, cfg.qualifying, botDirs, trackDirs)) return 1;
    }
    rr::Race race;
    if (!race.setup(cfg, botDirs, trackDirs, &err)) {
        std::fprintf(stderr, "error: %s\n", err.c_str());
        return 1;
    }
    for (const auto& w : race.track().warnings()) std::fprintf(stderr, "track warning: %s\n", w.c_str());
    if (!cfg.quiet) {
        std::printf("%s: %.0f m, %zu cars, %d laps\n", race.track().name().c_str(), race.track().length(),
                    race.cars().size(), cfg.laps);
    }

    auto t0 = std::chrono::steady_clock::now();
    double nextReport = 30.0;
    rr::TestRecorder rec;
    const bool testLog = !cfg.testLog.empty();
    if (testLog) rec.begin(race, 0);
    while (!race.isOver()) {
        race.step();
        if (testLog) rec.update(race);
        if (!cfg.quiet && race.time() >= nextReport) {
            const auto& lead = race.cars()[race.order()[0]];
            std::printf("  t=%5.0fs  leader %-16s lap %d/%d\n", race.time(), lead.name.c_str(),
                        lead.currentLap(cfg.laps), cfg.laps);
            nextReport += 30.0;
        }
    }
    if (cfg.coolDown) {
        double flag = race.time();
        while (!race.cooledDown()) race.step();
        int parked = 0, running = 0;
        for (const auto& c : race.cars()) {
            if (c.dnf) continue;
            ++running;
            if (c.parked) ++parked;
        }
        std::printf("cool-down: %d/%d cars parked in the pit lane %.0f s after the flag\n", parked, running,
                    race.time() - flag);
    }
    double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    if (testLog) {
        // the first car's run, saved like the viewer's Testing mode does
        rec.finish();
        const rr::Car& c = race.cars()[0];
        const rr::RaceConfig& rc = race.config();
        rr::TestSetup su;
        su.track = rc.track;
        su.trackTitle = race.track().name();
        su.robot = rc.entries[0].robot;
        su.label = rc.entries[0].name.empty() ? c.robotName : rc.entries[0].name;
        su.params = rc.entries[0].params;
        su.dev = rc.entries[0].dev;
        su.laps = rc.laps;
        su.compound = c.robotCfg.tire_compound;
        su.fuel = c.robotCfg.initial_fuel;
        su.wearRate = rc.wearRate;
        su.fuelRate = rc.fuelRate;
        su.ambient = rc.ambient;
        rr::TestStore store(cfg.testLog);
        const std::string end = c.dnf ? c.dnfReason : c.finished ? "finished" : "time limit";
        const int id = store.save(su, rec, end, c.finished, &err);
        if (!id) {
            std::fprintf(stderr, "error: %s\n", err.c_str());
            return 1;
        }
        std::printf("test run %d saved in %s/run_%04d\n", id, cfg.testLog.c_str(), id);
    }

    race.printResults(stdout);
    if (!cfg.quiet)
        std::printf("\nsimulated %.1f s in %.2f s of CPU (%.0fx real time)\n", race.time(), wall,
                    wall > 0 ? race.time() / wall : 0.0);
    if (!cfg.jsonOut.empty() && !race.writeJson(cfg.jsonOut, wall)) {
        std::fprintf(stderr, "error: cannot write %s\n", cfg.jsonOut.c_str());
        return 1;
    }
    return 0;
}
