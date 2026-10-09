// rr_viewer: runs a race and shows it in 3D. Same command line as rr_race.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <memory>
#include <set>

#include "weekend.hpp"
#include "engine_sound.hpp"
#include "hud.hpp"
#include "liveries.hpp"
#include "menu.hpp"
#include "race_audio.hpp"
#include "race.hpp"
#include "raylib.h"
#include "director.hpp"
#include "renderer.hpp"
#include "rlgl.h"
#include "spec.hpp"
#include "test_figures.hpp"
#include "testlog.hpp"

#ifndef RR_SOURCE_DIR
#define RR_SOURCE_DIR "."
#endif

namespace {

struct Paths {
    std::vector<std::string> bots, tracks;
    std::string assets;
};

std::unique_ptr<rr::Race> makeRace(const rr::RaceConfig& cfg, const Paths& paths) {
    auto race = std::make_unique<rr::Race>();
    std::string err;
    if (!race->setup(cfg, paths.bots, paths.tracks, &err)) {
        std::fprintf(stderr, "error: %s\n", err.c_str());
        return nullptr;
    }
    return race;
}

// Algorithms the grid page offers: the example robots with a few settings, then any
// other robot library found in the bot folders (your own robots show up here).
std::vector<Algorithm> listAlgorithms(const Paths& paths) {
    std::vector<Algorithm> algos = {
        // The racingline family first: the default grid uses these four. Each has
        // the team stats that suit its style (50 points over 10 stats), applied with
        // the menu's style button: Spongebob needs tyre management, Granny Doris can
        // spend on speed.
        {"John F One", "racingline", "", ""},
        {"Spongebob", "spongebob", "", "tire_management=8,pit_stop=4,brakes=3"},
        {"Dave", "dave", "", ""},
        {"Granny Doris", "granny", "", "tire_management=3,top_speed=6,acceleration=6"},
        {"gapfollow", "gapfollow", ""},
        {"gapfollow safe", "gapfollow", "speed=0.85"},
        {"gapfollow steady", "gapfollow", "speed=0.8"},
        {"simple", "simple", ""},
    };
    std::set<std::string> found;
    for (const std::string& dir : paths.bots) {
        std::error_code ec;
        for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
            const std::string ext = e.path().extension().string();
            if (ext == ".so" || ext == ".dll" || ext == ".dylib") found.insert(e.path().stem().string());
        }
    }
    for (const std::string& name : found) {
        bool known = false;
        for (const Algorithm& a : algos) known = known || a.robot == name;
        if (!known) algos.push_back({name, name, ""});
    }
    return algos;
}

// The `name` line of a track file, or "" if it has none.
std::string trackTitle(const std::string& file) {
    std::ifstream in(file);
    std::string line;
    while (std::getline(in, line))
        if (line.rfind("name ", 0) == 0) {
            std::string t = line.substr(5);
            while (!t.empty() && (t.back() == '\r' || t.back() == ' ')) t.pop_back();
            return t;
        }
    return "";
}

// Every *.trk in the track folders, by file name.
std::vector<std::string> listTracks(const Paths& paths) {
    std::set<std::string> names;
    for (const std::string& dir : paths.tracks) {
        std::error_code ec;
        for (const auto& e : std::filesystem::directory_iterator(dir, ec))
            if (e.path().extension() == ".trk") names.insert(e.path().stem().string());
    }
    return {names.begin(), names.end()};
}

// Drives three laps of `track` with the racingline robot alone (no pit stops) and
// measures a flying lap: time, fuel and tyre wear. The menu uses it to show tyre life
// and fuel range in laps of this track.
std::string gCarSpec;  // the car spec the calibration laps use ("" = the built-in car)

TrackStats calibrate(const std::string& track, const Paths& paths) {
    TrackStats t;
    t.file = t.title = track;
    rr::RaceConfig c;
    c.track = track;
    c.laps = 3;
    c.quiet = true;
    c.carSpec = gCarSpec;
    c.entries = {{"racingline", "pit=0", ""}};
    c.entries[0].tires = RR_TIRE_MEDIUM;  // the wear figures are for the medium tyre
    rr::Race r;
    std::string err;
    if (!r.setup(c, paths.bots, paths.tracks, &err)) return t;
    t.title = r.track().name();
    t.length = r.track().length();
    const int step = std::max(1, r.track().size() / 150);
    for (int i = 0; i < r.track().size(); i += step) t.outline.push_back(r.track().at(i).p);
    // fallbacks, scaled from the circuit, in case the robot does not finish
    const float k = t.length / 3176.0f;
    t.lapTime = 55 * k;
    t.fuelPerLap = 2.3f * k;
    t.wearPerLap = 0.03f * k;
    const auto wear = [](const rr::Car& car) { return std::max(car.state.tireWear[0], car.state.tireWear[1]); };
    float fuel1 = 0, wear1 = 0;
    int laps = 0;
    while (!r.isOver() && r.time() < 900) {
        r.step();
        const rr::Car& car = r.cars()[0];
        if (car.lapsDone == laps) continue;
        laps = car.lapsDone;
        if (laps == 1) {
            fuel1 = car.state.fuel;
            wear1 = wear(car);
        } else if (laps == 3) {
            t.fuelPerLap = (fuel1 - car.state.fuel) / 2;
            t.wearPerLap = (wear(car) - wear1) / 2;
            t.lapTime = car.bestLap;
            t.measured = true;
            break;
        }
    }
    return t;
}

// --sound-test: no window; races to --at, then records 25 s of the focused car's
// engine heard from just behind it, with the other cars passing by.
int runSoundTest(const rr::RaceConfig& cfg, const Paths& paths) {
    auto race = makeRace(cfg, paths);
    if (!race) return 1;
    const double t0 = std::max(0.0f, cfg.screenshotAt);
    while (!race->isOver() && race->time() < t0) race->step();
    EngineSynth synth;
    std::vector<float> pcm, buf;
    const int fps = 60, frames = EngineSynth::kRate / fps;
    buf.resize(2 * frames);
    for (int i = 0; i < 25 * fps && !race->isOver(); ++i) {
        race->advance(1.0 / fps);
        int focus = cfg.focus >= 0 && cfg.focus < (int)race->cars().size() ? cfg.focus : race->order()[0];
        const rr::Car& c = race->cars()[focus];
        rr::Vec2 h = rr::fromAngle(c.state.yaw), v = c.state.velWorld();
        Vector3 fwd = {h.x, 0, -h.y}, right = {h.y, 0, h.x};
        Vector3 pos = {c.state.pos.x - fwd.x * 7, 2.0f, -c.state.pos.y - fwd.z * 7};
        synth.setVoices(RaceAudio::listen(*race, pos, {v.x, 0, -v.y}, right, focus), 1.1f);
        synth.render(buf.data(), frames);
        pcm.insert(pcm.end(), buf.begin(), buf.end());
    }
    if (!writeWav(cfg.soundTest.c_str(), pcm, EngineSynth::kRate)) {
        std::fprintf(stderr, "error: could not write %s\n", cfg.soundTest.c_str());
        return 1;
    }
    std::printf("saved %s (%.1f s from t=%.0f s)\n", cfg.soundTest.c_str(), pcm.size() / 2.0 / EngineSynth::kRate, t0);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    rr::RaceConfig cfg;
    bool help = false;
    std::string err;
    if (!rr::parseArgs(argc, argv, cfg, true, help, &err)) {
        std::fprintf(stderr, "error: %s\n\n%s", err.c_str(), rr::usage(argv[0], true).c_str());
        return 2;
    }
    if (help) {
        std::printf("%s", rr::usage(argv[0], true).c_str());
        return 0;
    }
    const std::vector<rr::EntrySpec> cliEntries = cfg.entries;
#ifdef RR2_RENDERER
    // Raylib Racers 2: the 2013 car, on Highmoor Ridge unless a track is asked for
    if (cfg.carSpec.empty()) cfg.carSpec = "f1_2013";
    bool trackGiven = false;
    for (int i = 1; i < argc; ++i)
        if (std::string(argv[i]) == "--track" || std::string(argv[i]) == "-t") trackGiven = true;
    if (!trackGiven) cfg.track = "highmoor";
    gCarSpec = cfg.carSpec;
#endif

    const std::string dir = rr::exeDir(argv[0]);
    cfg.botHost = rr::botHostPath(dir);
    Paths paths;
    paths.bots = {dir + "/bots", dir, "bots", "."};
    paths.tracks = {dir + "/tracks", RR_SOURCE_DIR "/tracks", "tracks"};
    for (const std::string& a : {dir + "/assets", std::string(RR_SOURCE_DIR "/assets"), std::string("assets")})
        if (std::filesystem::exists(a + "/fonts")) { paths.assets = a; break; }

    {
        std::vector<CarLivery> liveries = loadLiveries(paths.assets);
#ifdef RR2_RENDERER
        neutralTeams(liveries);  // Team 1, Team 2 ...: the teams' colours are the player's to pick
        loadStockCars(paths.assets);
#endif
        setLiveryTable(liveries);
    }
    const int liveryCount = std::max(1, (int)liveryTable().size());

    // The grid: one livery slot and algorithm per car. Without --car, a full field over all the teams.
    MenuState menu;
#ifdef RR2_RENDERER
    menu.rr2 = true;
#endif
    menu.algos = listAlgorithms(paths);
    menu.liveryCount = liveryCount;
    if (cliEntries.empty()) {
        const int n = liveryTable().empty() ? 7 : liveryCount;
        for (int i = 0; i < n; ++i) menu.carAlgo.push_back(i % 4);  // the four racingline presets
    } else {
        for (const rr::EntrySpec& e : cliEntries) {
            int found = -1;
            for (int a = 0; a < (int)menu.algos.size(); ++a)
                if (menu.algos[a].robot == e.robot && menu.algos[a].params == e.params) found = a;
            if (found < 0) {
                menu.algos.push_back({e.name.empty() ? e.robot : e.name, e.robot, e.params});
                found = (int)menu.algos.size() - 1;
            }
            menu.carAlgo.push_back(found);
            menu.carTires.push_back(e.tires);
        }
    }
    menu.maxCars = (int)menu.carAlgo.size();
    while ((int)menu.carAlgo.size() < liveryCount) menu.carAlgo.push_back((int)menu.carAlgo.size() % 4);
    menu.maxCars = std::max(menu.maxCars, liveryCount);
    for (int i = 0; i < menu.maxCars; ++i) menu.carLivery.push_back(i % liveryCount);
    menu.carTires.resize(menu.maxCars, 0);

    // Teams (liveries grouped by team name) and their stats.
    rr::DevRules devRules;
    {
        std::vector<std::string> names;
        menu.slotTeam.assign(liveryTable().size(), -1);
        for (int slot = 0; slot < (int)liveryTable().size(); ++slot) {
            const std::string& team = liveryTable()[slot].team;
            int t = (int)(std::find(names.begin(), names.end(), team) - names.begin());
            if (t == (int)names.size()) {
                names.push_back(team);
                menu.teamSlots.emplace_back();
            }
            menu.teamSlots[t].push_back(slot);
            menu.slotTeam[slot] = t;
        }
        std::vector<std::string> specDirs;
        for (const auto& d : paths.tracks) specDirs.push_back((std::filesystem::path(d).parent_path() / "specs").string());
        specDirs.push_back("specs");
        rr::DevRules& rules = devRules;
        const std::string rulesPath = rr::findDataFile(cfg.devRules, specDirs);
        if (!rulesPath.empty() && rr::loadDevRules(rulesPath, rules, &err)) {
            for (const auto& c : rules.categories) {
                menu.statRules.keys.push_back(c.key);
                menu.statRules.labels.push_back(c.label);
                menu.statAbout.push_back(c.about);
            }
            menu.statRules.budget = rules.budget;
            menu.statRules.min = rules.minPoints;
            menu.statRules.max = rules.maxPoints;
            menu.statRules.neutral = rules.neutral;
        }
        menu.teamStats.assign(menu.teamSlots.size(), menu.statRules.parse(""));
        menu.testStats = menu.statRules.parse(menu.algos.empty() ? "" : menu.algos[0].stats);
    }
    menu.tyreRule = cfg.twoCompounds < 0 ? 0 : cfg.twoCompounds ? 1 : 2;
    menu.cars = cliEntries.empty() ? (int)std::min<size_t>(menu.maxCars, liveryTable().empty() ? 7 : liveryCount)
                                   : (int)cliEntries.size();
    if (cliEntries.empty() && !menu.teamSlots.empty()) {
        menu.teams = (int)menu.teamSlots.size();
        menu.drivers = 2;
        menu.layoutGrid();
    } else {
        menu.teams = std::max(1, (menu.cars + 1) / 2);
    }
    // Team stats: from --dev on the command line, else the style of each team's last driver.
    for (int i = 0; i < menu.cars; ++i) {
        const int t = menu.teamOfCar(i);
        if (t < 0) continue;
        if (i < (int)cliEntries.size() && !cliEntries[i].dev.empty()) menu.teamStats[t] = menu.statRules.parse(cliEntries[i].dev);
        else if (i >= (int)cliEntries.size()) menu.styleChanged(i);
    }
    // Entries for the first `cars` cars of the grid; names carry the race number.
    auto gridEntries = [&]() {
        std::vector<rr::EntrySpec> entries;
        std::vector<int> slots;
        for (int i = 0; i < menu.cars; ++i) {
            const Algorithm& a = menu.algos[menu.carAlgo[i]];
            const int slot = menu.carLivery[i];
            std::string name = a.label;
            if (!liveryTable().empty()) name = std::to_string(liveryTable()[slot].number) + " " + name;
            rr::EntrySpec e{a.robot, a.params, name};
            e.tires = menu.carTires[i];
            const int team = menu.teamOfCar(i);
            if (team >= 0) e.dev = menu.statRules.format(menu.teamStats[team]);
            else if (i < (int)cliEntries.size()) e.dev = cliEntries[i].dev;
            entries.push_back(e);
            slots.push_back(slot);
        }
        setCarLiveries(slots);
        return entries;
    };
    cfg.entries = gridEntries();

    if (!cfg.soundTest.empty()) return runSoundTest(cfg, paths);

    const bool shotMode = !cfg.screenshot.empty();
    // a screenshot without --at shows the menu
    bool inMenu = !cfg.noMenu && (!shotMode || cfg.screenshotAt < 0);

    // Menu: tracks, with the one asked for first selected, and the command line as defaults.
    for (const std::string& t : listTracks(paths)) {
        TrackStats ts;
        ts.file = ts.title = t;
        const std::string title = trackTitle(rr::trackFile(t, paths.tracks));
        if (!title.empty()) ts.title = title;  // the real name before calibration fills the rest
        menu.tracks.push_back(ts);
        if (t == cfg.track) menu.track = (int)menu.tracks.size() - 1;
    }
    if (menu.tracks.empty() || menu.tracks[menu.track].file != cfg.track) {  // a path, or nothing found
        TrackStats ts;
        ts.file = ts.title = cfg.track;
        menu.tracks.insert(menu.tracks.begin(), ts);
        menu.track = 0;
    }
    menu.laps = cfg.laps == 3 ? 10 : cfg.laps;  // 3 is the headless default; a race to watch is longer
#ifdef RR2_RENDERER
    // a 2013 grand prix: the fewest whole laps over 305 km (unless --laps was given)
    auto gpLaps = [&]() {
        const float len = menu.tracks[menu.track].length;
        if (cfg.laps == 3 && len > 500) menu.laps = std::max(1, (int)std::ceil(305000.0f / len));
    };
#endif
    menu.tankLitres = rr::CarParams{}.fuelCapacity;
#ifdef RR2_RENDERER
    {   // the 2013 car's fixed start load (no refuelling)
        rr::CarParams p;
        if (rr::loadCarSpec(rr::findDataFile(cfg.carSpec, {RR_SOURCE_DIR "/specs", dir + "/specs", "specs"}), p, nullptr))
            menu.tankLitres = p.fuelCapacity;
    }
#endif
    auto ensureStats = [&]() {
        TrackStats& ts = menu.tracks[menu.track];
        if (!ts.measured) ts = calibrate(ts.file, paths);
    };
    if (inMenu) {
        // every track, for the drop-downs, the championship calendar and its thumbnails
        for (TrackStats& t : menu.tracks)
            if (!t.measured) t = calibrate(t.file, paths);
        menu.setWearRate(cfg.wearRate);
        menu.resetCalendar();
#ifdef RR2_RENDERER
        gpLaps();
#endif
    }

    auto race = makeRace(cfg, paths);
    if (!race) return 1;
    for (const auto& w : race->track().warnings()) std::fprintf(stderr, "track warning: %s\n", w.c_str());

    SetTraceLogLevel(LOG_WARNING);
    unsigned flags = FLAG_MSAA_4X_HINT | FLAG_WINDOW_RESIZABLE;
    if (!shotMode) flags |= FLAG_VSYNC_HINT;
    SetConfigFlags(flags);
    #ifdef RR2_RENDERER
    InitWindow(cfg.width, cfg.height, "Raylib Racers 2");
#else
    InitWindow(cfg.width, cfg.height, "Raylib Racers");
#endif
    if (cfg.fullscreen) ToggleFullscreen();
    SetTargetFPS(shotMode ? 0 : 120);
    SetExitKey(KEY_NULL);  // Esc goes back to the menu

    // The renderer and HUD are built for one track; a new track gets new ones.
    std::unique_ptr<Renderer> renderer;
    Director director;
    const rr::Race* directorRace = nullptr;
    std::unique_ptr<Hud> hud;
    std::string sceneTrack;
    auto buildScene = [&]() -> bool {
        if (renderer && sceneTrack == race->config().track) return true;
        if (hud) hud->shutdown();
        if (renderer) renderer->shutdown();
        renderer = std::make_unique<Renderer>();
        hud = std::make_unique<Hud>();
        hud->settle = shotMode;
        if (!renderer->init(race->track(), (unsigned)cfg.seed, paths.assets, &err)) return false;
        hud->init(race->track(), paths.assets);
        sceneTrack = race->config().track;
        return true;
    };
    if (!buildScene()) {
        std::fprintf(stderr, "error: %s\n", err.c_str());
        CloseWindow();
        return 1;
    }

    RaceAudio audio;
#ifdef RR2_RENDERER
    // the 2013 V8 on its F1 exhaust, in a little trackside room
    audio.engine = EngineSynth::V8;
    audio.heightOf = carHeightForSound;
    audio.setReverb(0.22f);
    audio.setExhaustImpulse(loadExhaustImpulse(paths.assets), 1.5f);
    audio.setMaster(0.6f);
#endif
    if (!shotMode && !audio.init()) std::fprintf(stderr, "note: no audio device, running without sound\n");

    HudState st;
    st.resultsWindow = std::clamp(cfg.resultsView, 0, 5);
    st.timeScale = cfg.timeScale;
    st.muted = cfg.mute;
    st.camera = (CamMode)(std::max(0, cfg.camera) % CAM_COUNT);
    st.view.quality = cfg.quality;
    double simDebt = 0;
    int shotFrames = 0;
    std::vector<MenuHit> menuHits;

    if (shotMode && !inMenu && !cfg.test)
        for (int n = 0; !race->cooledDown() && race->time() < cfg.screenshotAt; ++n) {
            race->step();
            if (renderer && n % 4 == 3) renderer->stepEffects(*race, 0.0f);  // lay the rubber and skid marks down
        }
    // --focus N picks a car; without it the camera follows whoever leads.
    if (cfg.focus >= 0 && cfg.focus < (int)race->cars().size()) {
        st.focus = cfg.focus;
        st.followLeader = false;
    }

    // Applies the menu's choices and starts a fresh race (or, for a new track, a fresh grid to look at).
    auto applyMenu = [&]() -> bool {
        const TrackStats& ts = menu.stats();
        cfg.track = ts.file;
        cfg.laps = menu.laps;
        cfg.wearRate = menu.wearRate();
        cfg.twoCompounds = menu.twoCompoundsArg();
        cfg.entries = gridEntries();
        auto fresh = makeRace(cfg, paths);
        if (!fresh) return false;
        race = std::move(fresh);
        simDebt = 0;
        st.focus = std::min(st.focus, (int)race->cars().size() - 1);
        if (!buildScene()) {
            std::fprintf(stderr, "error: %s\n", err.c_str());
            return false;
        }
        return true;
    };
    if (inMenu) applyMenu();

    // ---- lineups: the grid, team stats included, saved by name in lineups/
    const std::string lineupDir = dir + "/lineups", seasonDir = dir + "/championships";
    auto fileName = [](std::string name) {
        for (char& c : name)
            if (!std::isalnum((unsigned char)c) && c != '-' && c != '_') c = '_';
        return name;
    };
    auto toast = [&](const std::string& msg) {
        menu.toast = msg;
        menu.toastUntil = GetTime() + 3.5;
    };
    auto menuLineup = [&](const std::string& name) {
        rr::Lineup L;
        L.name = name;
        if (menu.teamSlots.empty()) {
            L = rr::Lineup::fromEntries(gridEntries());
            L.name = name;
            return L;
        }
        const auto& table = liveryTable();
        for (int t : menu.raceTeams()) {
            rr::LineupTeam lt;
            const int slot0 = menu.teamSlots[t][0];
            lt.name = slot0 < (int)table.size() ? table[slot0].team : "Team " + std::to_string(L.teams.size() + 1);
            lt.livery = slot0;
            lt.stats = menu.statRules.format(menu.teamStats[t]);
            for (int car = 0; car < menu.cars; ++car) {
                if (menu.teamOfCar(car) != t) continue;
                const Algorithm& a = menu.algos[menu.carAlgo[car]];
                lt.drivers.push_back({a.label, a.robot, a.params, menu.carTires[car], menu.carLivery[car]});
            }
            L.teams.push_back(lt);
        }
        return L;
    };
    // Puts a lineup on the menu's grid: its teams, liveries, algorithms, tyres and stats.
    auto applyLineup = [&](const rr::Lineup& L) {
        if (menu.teamSlots.empty() || L.teams.empty()) return false;
        int drivers = 1;
        for (const auto& t : L.teams) drivers = std::max(drivers, (int)t.drivers.size());
        menu.teams = std::min((int)L.teams.size(), (int)menu.teamSlots.size());
        menu.drivers = std::min(drivers, 2);
        menu.layoutGrid();
        std::vector<int> grid = L.defaultGrid(), used;
        int car = 0;
        for (int id : grid) {
            if (car >= menu.maxCars) break;
            const rr::LineupDriver& d = L.driver(id);
            const rr::LineupTeam& team = L.teams[L.teamOf(id)];
            int slot = d.livery >= 0 && d.livery < menu.liveryCount ? d.livery : team.livery;
            if (slot < 0 || slot >= menu.liveryCount || std::count(used.begin(), used.end(), slot)) {
                slot = -1;  // keep the place layoutGrid gave
                for (int s2 = 0; s2 < menu.liveryCount && slot < 0; ++s2)
                    if (!std::count(used.begin(), used.end(), s2) && menu.slotTeam[s2] == menu.slotTeam[std::max(0, team.livery)]) slot = s2;
                if (slot < 0) slot = menu.carLivery[car];
            }
            used.push_back(slot);
            menu.carLivery[car] = slot;
            int found = -1;
            for (int a = 0; a < (int)menu.algos.size(); ++a)
                if (menu.algos[a].robot == d.robot && menu.algos[a].params == d.params) found = a;
            if (found < 0) {
                menu.algos.push_back({d.name, d.robot, d.params});
                found = (int)menu.algos.size() - 1;
            }
            menu.carAlgo[car] = found;
            menu.carTires[car] = d.tires;
            const int t = menu.slotTeam[slot];
            if (t >= 0) menu.teamStats[t] = menu.statRules.parse(team.stats);
            ++car;
        }
        menu.cars = car;
        // the slots nobody races follow, so livery swaps still work
        int k = car;
        for (int s2 = 0; s2 < menu.liveryCount && k < (int)menu.carLivery.size(); ++s2)
            if (!std::count(used.begin(), used.end(), s2)) menu.carLivery[k++] = s2;
        menu.styleCar.clear();
        return true;
    };
    auto listLineups = [&]() {
        menu.lineupFiles.clear();
        std::error_code ec;
        for (const auto& e : std::filesystem::directory_iterator(lineupDir, ec))
            if (e.path().extension() == ".json") menu.lineupFiles.push_back(e.path().stem().string());
        std::sort(menu.lineupFiles.begin(), menu.lineupFiles.end());
    };

    // ---- championships: saved in championships/, one file per season
    rr::Championship season;
    std::string seasonPath;
    bool inSeason = false, seasonRecorded = false;
    std::vector<int> raceIds;  // championship driver id of each car in the race
    auto driverName = [&](const rr::Championship& c, int id) {
        const rr::LineupDriver& d = c.lineup.driver(id);
        const auto& table = liveryTable();
        return d.livery >= 0 && d.livery < (int)table.size() ? std::to_string(table[d.livery].number) + " " + d.name : d.name;
    };
    auto listSeasons = [&]() {
        menu.seasons.clear();
        std::vector<std::filesystem::path> files;
        std::error_code ec;
        for (const auto& e : std::filesystem::directory_iterator(seasonDir, ec))
            if (e.path().extension() == ".json") files.push_back(e.path());
        // newest first
        std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) {
            std::error_code e1;
            return std::filesystem::last_write_time(a, e1) > std::filesystem::last_write_time(b, e1);
        });
        for (const auto& f : files) {
            rr::Championship c;
            if (!rr::Championship::load(f.string(), c, nullptr)) continue;
            MenuState::SeasonLine l;
            l.file = f.string();
            l.name = c.name;
            l.done = c.roundsDone();
            l.total = (int)c.rounds.size();
            if (l.done > 0) l.leader = driverName(c, c.driverStandings()[0].id);
            if (!c.over()) {
                const TrackStats* t = menu.trackStats(c.rounds[l.done].track);
                l.next = t ? t->title : c.rounds[l.done].track;
            }
            menu.seasons.push_back(l);
        }
    };
    auto openSeason = [&]() {
        menu.season = &season;
        menu.seasonPage = true;
        menu.seasonTab = 0;
        inSeason = true;
    };
    auto newSeason = [&]() -> bool {
        if (menu.calendar.empty()) {
            toast("Add at least one track to the calendar");
            return false;
        }
        rr::Championship c;
        c.name = menu.champName;
        c.lineup = menuLineup(menu.champName);
        for (const rr::ChampRound& r : menu.calendar) c.rounds.push_back({r.track, menu.roundLaps(r)});
        c.distanceKm = menu.champDistance();
        c.wearRate = menu.champWearRate();
        c.twoCompounds = menu.twoCompoundsArg();
        c.qualifying = menu.champQuali;
        c.practiceLaps = menu.practiceLaps();
        c.sandbox = cfg.sandbox;
        c.cpuCapMs = cfg.cpuCapMs;
        c.seed = cfg.seed;
        std::error_code ec;
        std::filesystem::create_directories(seasonDir, ec);
        std::string base = seasonDir + "/" + fileName(c.name), path = base + ".json";
        for (int k = 2; std::filesystem::exists(path, ec); ++k) path = base + "_" + std::to_string(k) + ".json";
        if (!c.save(path, &err)) {
            toast("Could not save: " + err);
            return false;
        }
        season = c;
        seasonPath = path;
        openSeason();
        // the next new season gets the next number
        int n = 1;
        if (std::sscanf(menu.champName.c_str(), "Season %d", &n) == 1) menu.champName = "Season " + std::to_string(n + 1);
        return true;
    };

    // ---- weekend: practice and qualifying run one car at a time, then the race
    // starts in qualifying order. Every car keeps one weekend memory throughout.
    enum class Phase { Race, Practice, PracticeDone, Quali, QualiDone, Test } phase = Phase::Race;
    const rr::Race* lightsFor = nullptr;
    bool wasInMenu = false;
    double menuQuietUntil = 0;  // the race the start lights were shown for
    const rr::Race* loggedRace = nullptr;  // the race whose log has been written
    std::vector<rr::EntrySpec> weekendEntries;
    std::vector<int> weekendSlots, weekendIds;  // each entry's livery and championship driver id
    std::vector<float> qualiTime;
    int qualiCar = 0;
    int weekendPractice = 0;       // practice laps per car this weekend
    bool skipRun = false, skipAll = false;  // fast-forwarding the session's runs
    auto qualiConfig = [&](int k) {
        if (phase == Phase::Practice) return rr::practiceConfig(cfg, weekendEntries[k], weekendPractice);
        return rr::qualiConfig(cfg, weekendEntries[k], menu.stats().fuelPerLap * 3.6f);
    };
    auto refreshQualiLines = [&]() {
        std::vector<int> idx(weekendEntries.size());
        for (int i = 0; i < (int)idx.size(); ++i) idx[i] = i;
        std::stable_sort(idx.begin(), idx.end(), [&](int a, int b) {
            const bool ta = qualiTime[a] > 0, tb = qualiTime[b] > 0;
            if (ta != tb) return ta;
            return ta && qualiTime[a] < qualiTime[b];
        });
        st.quali.clear();
        for (int i : idx) {
            QualiLine l;
            l.name = weekendEntries[i].name;
            l.color = liveryTable().empty() ? teamColor(i) : liveryTable()[weekendSlots[i]].color;
            l.time = qualiTime[i];
            l.running = (phase == Phase::Quali || phase == Phase::Practice) && i == qualiCar;
            st.quali.push_back(l);
        }
        st.qualiRun = qualiCar + 1;
        st.qualiRuns = (int)weekendEntries.size();
        return idx;
    };
    auto startQualiRun = [&](int k) -> bool {
        qualiCar = k;
        setCarLiveries({weekendSlots[k]});
        auto fresh = makeRace(qualiConfig(k), paths);
        if (!fresh) return false;
        race = std::move(fresh);
        simDebt = 0;
        st.focus = 0;
        st.qualifying = true;
        refreshQualiLines();
        return true;
    };
    // The current run is over: its time, then the next car or the end of the session.
    auto endRun = [&]() {
        qualiTime[qualiCar] = race->cars()[0].bestLap;
        skipRun = false;
        if (qualiCar + 1 < (int)weekendEntries.size()) {
            startQualiRun(qualiCar + 1);
        } else {
            skipAll = false;
            phase = phase == Phase::Practice ? Phase::PracticeDone : Phase::QualiDone;
            st.qualifying = false;
            refreshQualiLines();
        }
    };
    auto setSessionTitle = [&]() {
        st.sessionTitle = phase == Phase::Practice || phase == Phase::PracticeDone ? "PRACTICE" : "QUALIFYING";
    };
    auto startQuali = [&]() -> bool {
        phase = Phase::Quali;
        setSessionTitle();
        qualiTime.assign(weekendEntries.size(), 0.0f);
        return startQualiRun(0);
    };
    // Gives each car its weekend memory, then practice (if any) or qualifying.
    auto beginWeekend = [&](int practiceLaps) -> bool {
        rr::startWeekend(weekendEntries);
        weekendPractice = practiceLaps;
        skipRun = skipAll = false;
        if (practiceLaps <= 0) return startQuali();
        phase = Phase::Practice;
        setSessionTitle();
        qualiTime.assign(weekendEntries.size(), 0.0f);
        return startQualiRun(0);
    };
    auto startWeekendRace = [&]() -> bool {
        std::vector<int> order = refreshQualiLines();
        cfg.entries.clear();
        std::vector<int> slots;
        raceIds.clear();
        for (int i : order) {
            cfg.entries.push_back(weekendEntries[i]);
            slots.push_back(weekendSlots[i]);
            raceIds.push_back(weekendIds[i]);
        }
        setCarLiveries(slots);
        auto fresh = makeRace(cfg, paths);
        if (!fresh) return false;
        race = std::move(fresh);
        simDebt = 0;
        phase = Phase::Race;
        st.qualifying = false;
        st.followLeader = true;
        return true;
    };

    // A championship round: the season's track, laps, rules and grid (qualifying first if the season has it).
    auto startRound = [&]() -> bool {
        if (season.over()) return false;
        const rr::ChampRound& r = season.rounds[season.roundsDone()];
        for (int i = 0; i < (int)menu.tracks.size(); ++i)
            if (menu.tracks[i].file == r.track) menu.track = i;
        ensureStats();
        const std::vector<int> grid = season.nextGrid();
        cfg = season.roundConfig(cfg, grid);
        std::vector<int> slots;
        for (int i = 0; i < (int)grid.size(); ++i) {
            cfg.entries[i].name = driverName(season, grid[i]);
            const int slot = season.lineup.driver(grid[i]).livery;
            slots.push_back(slot >= 0 ? slot : i % std::max(1, menu.liveryCount));
        }
        setCarLiveries(slots);
        auto fresh = makeRace(cfg, paths);
        if (!fresh) return false;
        race = std::move(fresh);
        if (!buildScene()) return false;
        simDebt = 0;
        seasonRecorded = false;
        st.paused = false;
        st.followLeader = true;
        st.resultsWindow = 0;
        if (season.qualifying) {
            // qualifying goes in the lineup's order; the race grid comes from the times
            weekendEntries.clear();
            weekendSlots.clear();
            weekendIds.clear();
            for (int id : season.lineup.defaultGrid()) {
                for (int i = 0; i < (int)grid.size(); ++i)
                    if (grid[i] == id) {
                        weekendEntries.push_back(cfg.entries[i]);
                        weekendSlots.push_back(slots[i]);
                        weekendIds.push_back(id);
                    }
            }
            return beginWeekend(season.practiceLaps);
        }
        raceIds = grid;
        phase = Phase::Race;
        return true;
    };

    // ---- testing: one car alone, recorded; every run saved under test_runs/
    rr::TestStore store(dir + "/test_runs");
    if (!store.load(&err)) std::fprintf(stderr, "warning: %s\n", err.c_str());
    menu.runsTotal = (int)store.runs().size();
    rr::TestRecorder rec;
    rr::TestSetup testSetup;
    TestView tv;
    tv.rec = &rec;
    std::vector<TestHit> testHits;
    bool testSaved = false, dragging = false;
    std::vector<int> figuresFor;  // the stats the figures were computed for
    int figuresTrack = -1;
    float figuresLife = -1;
    // The automatic tyres and fuel for the run, and what the stats do.
    auto refreshTesting = [&]() {
        const TrackStats& ts = menu.stats();
        const rr::CarParams p0, p1 = carWithStats(devRules, menu.testStats);
        menu.fuelPerLapEst = ts.fuelPerLap * cfg.fuelRate * p1.fuelPerJoule / p0.fuelPerJoule;
        const float life = menu.tyreLifeLaps();
        const float medium = life * p0.wearPerJoule / p1.wearPerJoule;
        for (int c = RR_TIRE_SOFT; c <= RR_TIRE_HARD; ++c) menu.compoundLife[c] = life == 0 ? 0 : medium / rr::compoundWear(c);
        menu.autoTires = RR_TIRE_HARD;
        if (life == 0 || menu.compoundLife[RR_TIRE_SOFT] >= menu.laps) menu.autoTires = RR_TIRE_SOFT;
        else if (menu.compoundLife[RR_TIRE_MEDIUM] >= menu.laps) menu.autoTires = RR_TIRE_MEDIUM;
        menu.autoFuel = std::min(menu.tankLitres, std::ceil(menu.fuelPerLapEst * (menu.laps + 1) * 10) / 10);
        if (menu.testStatsPage && (figuresFor != menu.testStats || figuresTrack != menu.track || figuresLife != (float)life)) {
            FigureInputs in;
            in.fuelPerLap = ts.fuelPerLap * cfg.fuelRate;
            in.tyreLifeLaps = (float)life;
            menu.figures = carFigures(devRules, menu.testStats, in);
            figuresFor = menu.testStats;
            figuresTrack = menu.track;
            figuresLife = (float)life;
        }
        if (menu.runsPage) {
            menu.runLines.clear();
            for (const rr::TestRun& r : store.runs()) {
                if (!menu.runsAllTracks && r.setup.track != ts.file) continue;
                MenuState::RunLine l;
                l.id = r.id;
                l.date = r.date;
                l.track = r.setup.trackTitle.empty() ? r.setup.track : r.setup.trackTitle;
                l.algo = r.setup.label;
                l.stats = r.setup.dev;
                l.end = r.end;
                l.compound = r.setup.compound;
                l.laps = r.setup.laps;
                l.lapsDone = r.lapsDone;
                l.fuel = r.setup.fuel;
                l.best = r.best;
                l.average = r.average;
                l.fuelPerLap = r.fuelPerLap;
                l.wearPerLap = std::max(r.wearPerLap[0], r.wearPerLap[1]);
                l.telemetry = r.telemetry;
                l.completed = r.completed;
                menu.runLines.push_back(l);
            }
            if (menu.runsSort == 0)
                std::stable_sort(menu.runLines.begin(), menu.runLines.end(), [](auto& a, auto& b) { return a.id > b.id; });
            else
                std::stable_sort(menu.runLines.begin(), menu.runLines.end(), [](auto& a, auto& b) {
                    if ((a.best > 0) != (b.best > 0)) return a.best > 0;
                    return a.best < b.best;
                });
        }
    };
    auto describeSetup = [&](const rr::TestSetup& su) {
        static const char* names[] = {"", "Soft", "Medium", "Hard"};
        char buf[256];
        std::snprintf(buf, sizeof buf, "%s tyres   %.1f L   %d laps   %s", names[su.compound & 3], su.fuel, su.laps,
                      su.dev.empty() ? "stock stats" : su.dev.c_str());
        tv.setup = buf;
        const auto& lt = liveryTable();
        const bool have = su.livery >= 0 && su.livery < (int)lt.size();
        tv.title = (have ? "#" + std::to_string(lt[su.livery].number) + " " : std::string()) + su.label +
                   (have ? "   " + lt[su.livery].team : std::string());
        tv.laps = su.laps;
    };
    auto testEntry = [&](const rr::TestSetup& su) {
        rr::EntrySpec e{su.robot, su.params, su.label};
        const auto& lt = liveryTable();
        if (su.livery >= 0 && su.livery < (int)lt.size()) e.name = std::to_string(lt[su.livery].number) + " " + su.label;
        e.dev = su.dev;
        e.tires = su.compound;
        e.fuel = su.fuel;
        return e;
    };
    auto testConfig = [&](const rr::TestSetup& su) {
        rr::RaceConfig t = cfg;
        t.track = su.track;
        t.laps = su.laps;
        t.wearRate = su.wearRate;
        t.fuelRate = su.fuelRate;
        t.ambient = su.ambient;
        t.twoCompounds = 0;
        t.pitsClosed = true;
        t.session = RR_SESSION_TEST;
        t.fuelLimit = 0;
        t.entries = {testEntry(su)};
        return t;
    };
    // Saves the run once: its setup and times always, its telemetry for the newest runs.
    auto saveTest = [&](const std::string& end, bool completed) {
        if (testSaved || tv.replay || shotMode || rec.empty() || (rec.laps.empty() && !completed)) return;
        rec.finish();
        const int id = store.save(testSetup, rec, end, completed, &err);
        testSaved = true;
        if (id) {
            tv.runId = id;
            char buf[64];
            std::snprintf(buf, sizeof buf, "run_%04d", id);
            tv.message = "Saved as run " + std::to_string(id) + ": " +
                         (std::filesystem::path(store.dir()) / buf).lexically_normal().string();
            std::printf("test run saved: %s\n", tv.message.c_str());
        } else {
            tv.message = "Could not save the run: " + err;
        }
        menu.runsTotal = (int)store.runs().size();
    };
    auto startTest = [&]() -> bool {
        refreshTesting();
        const TrackStats& ts = menu.stats();
        const Algorithm& a = menu.algos[std::min(menu.testAlgo, (int)menu.algos.size() - 1)];
        rr::TestSetup su;
        su.track = ts.file;
        su.trackTitle = ts.title;
        su.robot = a.robot;
        su.label = a.label;
        su.params = a.params;
        su.dev = menu.statRules.format(menu.testStats);
        su.livery = menu.testLivery;
        su.laps = menu.laps;
        su.compound = menu.testTiresUsed();
        su.fuel = menu.testFuelUsed();
        su.wearRate = menu.wearRate();
        su.fuelRate = cfg.fuelRate;
        su.ambient = cfg.ambient;
        setCarLiveries({su.livery});
        auto fresh = makeRace(testConfig(su), paths);
        if (!fresh) return false;
        race = std::move(fresh);
        if (!buildScene()) return false;
        testSetup = su;
        rec.begin(*race, 0);
        rec.update(*race);
        const int window = tv.window, compare = tv.compareLap, colour = tv.mapColour;
        const bool dash = tv.dashboard;
        tv = TestView{};
        tv.rec = &rec;
        tv.window = window;
        tv.compareLap = compare;
        tv.mapColour = colour;
        tv.dashboard = dash;
        describeSetup(su);
        testSaved = false;
        simDebt = 0;
        st.focus = 0;
        st.followLeader = false;
        st.paused = false;
        return true;
    };
    // A saved run's telemetry, on a car that stands still: everything comes from the recording.
    auto startReplay = [&](int id) -> bool {
        const rr::TestRun* run = store.find(id);
        if (!run) return false;
        if (!store.loadTelemetry(*run, rec, &err)) {
            std::fprintf(stderr, "error: %s\n", err.c_str());
            return false;
        }
        rr::RaceConfig rc = testConfig(run->setup);
        setCarLiveries({run->setup.livery});
        auto fresh = makeRace(rc, paths);
        if (!fresh) {  // the robot is gone: any car will do to show the recording
            rc.entries[0].robot = "simple";
            rc.entries[0].params.clear();
            fresh = makeRace(rc, paths);
        }
        if (!fresh) return false;
        race = std::move(fresh);
        if (!buildScene()) return false;
        if (rec.trackLength <= 0) rec.rebuild(race->track().length());
        tv = TestView{};
        tv.rec = &rec;
        tv.replay = true;
        tv.live = false;
        tv.runOver = true;
        tv.runId = id;
        tv.cursor = 0;
        describeSetup(run->setup);
        tv.message = "Run " + std::to_string(id) + ", " + run->date + ": " + run->end;
        testSaved = true;
        st.focus = 0;
        st.followLeader = false;
        st.paused = true;
        return true;
    };
    // Puts a saved run's setup into the testing menu.
    auto loadSetup = [&](int id) {
        const rr::TestRun* run = store.find(id);
        if (!run) return;
        const rr::TestSetup& su = run->setup;
        for (int i = 0; i < (int)menu.tracks.size(); ++i)
            if (menu.tracks[i].file == su.track && menu.track != i) {
                menu.track = i;
                ensureStats();
                applyMenu();
            }
        menu.laps = su.laps;
        menu.setWearRate(su.wearRate);
        int found = -1;
        for (int a = 0; a < (int)menu.algos.size(); ++a)
            if (menu.algos[a].robot == su.robot && menu.algos[a].params == su.params) found = a;
        if (found < 0) {
            menu.algos.push_back({su.label, su.robot, su.params});
            found = (int)menu.algos.size() - 1;
        }
        menu.testAlgo = found;
        menu.testLivery = std::clamp(su.livery, 0, std::max(0, menu.liveryCount - 1));
        menu.testTires = su.compound;
        menu.testFuel = su.fuel;
        menu.testStats = menu.statRules.parse(su.dev);
        menu.session = 2;
        menu.runsPage = false;
    };
    // Moves the cursor (and leaves live view).
    auto scrubTo = [&](double t) {
        if (rec.empty()) return;
        tv.cursor = std::clamp(t, 0.0, rec.endTime());
        tv.live = false;
    };
    // The same point of the track on another lap.
    auto jumpLap = [&](int lap) {
        if (rec.empty()) return;
        const rr::TestSample cur = tv.live ? rec.samples.back() : rec.at(tv.cursor);
        const int last = rec.samples.back().lap;
        lap = std::clamp(lap, 1, last);
        int f, e;
        rec.lapRange(lap, f, e);
        if (e <= f) return;
        int k = f;
        while (k + 1 < e && rec.samples[k + 1].lapDist <= cur.lapDist) ++k;
        scrubTo(rec.samples[k].t);
    };

    // --test: the Testing session for the first car of the command line
    if (cfg.test) {
        menu.session = 2;
        if (!menu.carAlgo.empty()) {
            menu.testAlgo = menu.carAlgo[0];
            menu.testStats = menu.statRules.parse(menu.algos[menu.testAlgo].stats);
            if (!cliEntries.empty()) {
                if (!cliEntries[0].dev.empty()) menu.testStats = menu.statRules.parse(cliEntries[0].dev);
                menu.testTires = cliEntries[0].tires;
                menu.testFuel = cliEntries[0].fuel;
            }
        }
        if (!inMenu || cfg.noMenu) {
            ensureStats();
            menu.setWearRate(cfg.wearRate);
            if (startTest()) {
                inMenu = false;
                phase = Phase::Test;
                tv.window = std::clamp(cfg.testView, 0, 3);
            }
        }
    }
    if (inMenu && cfg.page == "stats") { menu.session = 2; menu.testStatsPage = true; }
    if (inMenu && (cfg.page == "champ" || cfg.page == "season" || cfg.page == "lineups")) {
        menu.session = 3;
        listSeasons();
        if (cfg.page == "season" && !menu.seasons.empty() &&
            rr::Championship::load(menu.seasons[0].file, season, &err)) {
            seasonPath = menu.seasons[0].file;
            openSeason();
        }
        if (cfg.page == "lineups") {
            menu.session = 0;
            listLineups();
            menu.lineupLoad = true;
        }
    }
    if (inMenu && cfg.page == "runs") { menu.session = 2; menu.runsPage = true; }
    if (inMenu && cfg.page == "grid") {
        menu.gridPage = true;
        menu.gridRow = 1;
        menu.gridCol = 1;
        menu.carAlgo[1] = 0;  // show a pending style change
        menu.algoChanged(1);
    }

    bool quit = false;
    while (!WindowShouldClose() && !quit) {
        const float frameDt = std::min(GetFrameTime(), 0.1f);
        const int n = (int)race->cars().size();

        if (!inMenu) wasInMenu = false;
        if (inMenu) {
            // ---- race setup
            // Ignore input for a moment after the menu opens, so a key or click still held from
            // launching the viewer or leaving a race can't start one.
            if (!wasInMenu) menuQuietUntil = GetTime() + 0.4;
            wasInMenu = true;
            MenuAction act = GetTime() < menuQuietUntil ? MenuAction::None : updateMenu(menu, menuHits);
            if (menu.rr2) {  // a grand prix distance for weekends
                if (menu.weekend()) menu.laps = menu.gpLaps();
            }
            if (menu.testing()) refreshTesting();
            if (act == MenuAction::Quit) quit = true;
            if (act == MenuAction::LoadRun) loadSetup(menu.runPick);
            if (act == MenuAction::ViewRun) {
                if (startReplay(menu.runPick)) {
                    inMenu = false;
                    phase = Phase::Test;
                } else {
                    applyMenu();
                }
            }
            if (act == MenuAction::Start && menu.testing()) {
                act = MenuAction::None;
                if (startTest()) {
                    inMenu = false;
                    phase = Phase::Test;
                } else {
                    applyMenu();
                }
            }
            if (act == MenuAction::TrackChanged) {
                ensureStats();
#ifdef RR2_RENDERER
                gpLaps();
#endif
                if (!applyMenu()) quit = true;
            }
            if (act == MenuAction::ListLineups) {
                listLineups();
                menu.lineupLoad = true;
                menu.lineupRow = 0;
            }
            if (act == MenuAction::SaveLineup) {
                std::error_code ec;
                std::filesystem::create_directories(lineupDir, ec);
                const std::string path = lineupDir + "/" + fileName(menu.inputText) + ".json";
                if (menuLineup(menu.inputText).save(path, &err)) toast("Lineup saved: " + fileName(menu.inputText));
                else toast("Could not save: " + err);
            }
            if (act == MenuAction::LoadLineup) {
                rr::Lineup L;
                if (!rr::Lineup::load(lineupDir + "/" + menu.lineupPick + ".json", L, &err)) toast(err);
                else if (!applyLineup(L)) toast("This lineup does not fit the liveries");
                else {
                    toast("Lineup loaded: " + menu.lineupPick);
                    if (!applyMenu()) quit = true;
                }
            }
            if (act == MenuAction::ChampTab) {
                listSeasons();
                // a name no saved season has yet
                for (bool taken = true; taken;) {
                    taken = false;
                    for (const auto& l : menu.seasons) taken = taken || l.name == menu.champName;
                    int n = 0;
                    if (taken && std::sscanf(menu.champName.c_str(), "Season %d", &n) == 1) menu.champName = "Season " + std::to_string(n + 1);
                    else if (taken) menu.champName += " 2";
                }
                if (menu.calendar.empty()) menu.resetCalendar();
            }
            if (act == MenuAction::NewSeason) newSeason();
            if (act == MenuAction::ContinueSeason && menu.seasonPick >= 0 && menu.seasonPick < (int)menu.seasons.size()) {
                const std::string path = menu.seasons[menu.seasonPick].file;
                if (rr::Championship::load(path, season, &err)) {
                    seasonPath = path;
                    openSeason();
                } else {
                    toast(err);
                }
            }
            if (act == MenuAction::LeaveSeason) {
                menu.seasonPage = false;
                menu.season = nullptr;
                inSeason = false;
                listSeasons();
                applyMenu();
            }
            if (act == MenuAction::StartRound) {
                if (startRound()) inMenu = false;
                else toast("Could not start the round: " + err);
            }
            if (act == MenuAction::Start) {
                if (!applyMenu()) quit = true;
                inMenu = false;
                st.paused = false;
                if (menu.weekend() && !quit) {
                    weekendEntries = cfg.entries;
                    // the grid's liveries, in entry order (a season round fills these in startRound)
                    const int n = (int)weekendEntries.size();
                    weekendSlots.assign(menu.carLivery.begin(), menu.carLivery.begin() + n);
                    weekendIds.resize(n);
                    for (int i = 0; i < n; ++i) weekendIds[i] = i;
                    if (!beginWeekend(menu.practiceLaps())) quit = true;
                }
            }
            renderer->updateCamera(*race, race->order()[0], CAM_CINEMATIC, frameDt);
        } else if (phase == Phase::Test) {
            // ---- testing: one car, the timeline and the graphs
            const bool shift = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
            const bool ctrl = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
            const bool liveRun = !tv.replay && !tv.runOver;
            auto rep = [](int key) { return IsKeyPressed(key) || IsKeyPressedRepeat(key); };
            auto leaveLive = [&]() {
                if (!tv.live) return;
                tv.cursor = rec.endTime();
                tv.live = false;
                tv.playing = false;
            };
            if (IsKeyPressed(KEY_SPACE)) {
                if (tv.live) st.paused = !st.paused;
                else {
                    if (!tv.playing && !liveRun && tv.cursor >= rec.endTime() - 1e-3) tv.cursor = 0;  // from the start again
                    tv.playing = !tv.playing;
                    if (tv.playing) st.paused = false;
                }
            }
            const double step = ctrl ? 1.0 / rr::TestRecorder::kRate : shift ? 10.0 : 1.0;
            if (rep(KEY_LEFT)) { leaveLive(); tv.playing = false; scrubTo(tv.cursor - step); }
            if (rep(KEY_RIGHT)) {
                leaveLive();
                tv.playing = false;
                scrubTo(tv.cursor + step);
            }
            if (rep(KEY_PAGE_UP) && !rec.empty()) { const int lap = tv.live ? rec.samples.back().lap : rec.at(tv.cursor).lap; tv.playing = false; jumpLap(lap - 1); }
            if (rep(KEY_PAGE_DOWN) && !rec.empty() && !tv.live) { tv.playing = false; jumpLap(rec.at(tv.cursor).lap + 1); }
            if (IsKeyPressed(KEY_HOME)) { tv.playing = false; scrubTo(0); }
            if (IsKeyPressed(KEY_END)) {
                tv.playing = false;
                if (liveRun) tv.live = true;
                else scrubTo(rec.endTime());
            }
            const int nl = (int)rec.laps.size();
            if (IsKeyPressed(KEY_LEFT_BRACKET)) tv.compareLap = (tv.compareLap + nl) % (nl + 1);
            if (IsKeyPressed(KEY_RIGHT_BRACKET)) tv.compareLap = (tv.compareLap + 1) % (nl + 1);
            if (IsKeyPressed(KEY_G)) tv.dashboard = !tv.dashboard;
            if (IsKeyPressed(KEY_TAB)) tv.window = (tv.window + (shift ? 3 : 1)) % 4;
            if (IsKeyPressed(KEY_F) && liveRun) tv.fastForward = !tv.fastForward;
            if (IsKeyPressed(KEY_M)) {
                if (tv.window == 3) tv.mapColour = (tv.mapColour + 1) % 3;
                else st.muted = !st.muted;
            }
            if (IsKeyPressed(KEY_C)) st.camera = (CamMode)((st.camera + (shift ? CAM_COUNT - 1 : 1)) % CAM_COUNT);
            for (int k = 0; k < CAM_COUNT && k < 8; ++k)
                if (IsKeyPressed(KEY_F2 + k)) st.camera = (CamMode)k;
            if (IsKeyPressed(KEY_T)) st.camera = CAM_TCAM;
            if (IsKeyPressed(KEY_B)) st.camera = CAM_NOSE;
            if (IsKeyPressed(KEY_EQUAL) || IsKeyPressed(KEY_KP_ADD)) st.timeScale = std::min(64.0f, st.timeScale * 2);
            if (IsKeyPressed(KEY_MINUS) || IsKeyPressed(KEY_KP_SUBTRACT)) st.timeScale = std::max(0.125f, st.timeScale / 2);
            if (IsKeyPressed(KEY_P)) st.view.showPaths = !st.view.showPaths;
            if (IsKeyPressed(KEY_F10)) st.view.quality = (st.view.quality + 1) % 3;
            if (IsKeyPressed(KEY_S)) st.view.showSensors = !st.view.showSensors;
            if (IsKeyPressed(KEY_H)) st.showHud = !st.showHud;
            if (IsKeyPressed(KEY_F1)) st.showHelp = !st.showHelp;
            // mouse: the timeline, graphs, laps and events
            const Vector2 mp = GetMousePosition();
            if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                for (const TestHit& h : testHits) {
                    if (!CheckCollisionPointRec(mp, h.r)) continue;
                    switch (h.kind) {
                        case TestHit::Close: tv.window = 0; break;
                        case TestHit::Tile: tv.window = h.value; break;
                        case TestHit::Timeline: dragging = true; break;
                        case TestHit::Event: tv.playing = false; scrubTo(h.a); break;
                        case TestHit::Lap: {
                            int f, e;
                            rec.lapRange(h.value, f, e);
                            tv.playing = false;
                            if (e > f) scrubTo(rec.samples[f].t);
                            break;
                        }
                        case TestHit::Dist: {
                            int f, e;
                            rec.lapRange(h.value, f, e);
                            const float d = h.a + (mp.x - h.r.x) / std::max(1.0f, h.r.width) * (h.b - h.a);
                            int k = f;
                            while (k + 1 < e && rec.samples[k + 1].lapDist <= d) ++k;
                            tv.playing = false;
                            if (e > f) scrubTo(rec.samples[k].t);
                            break;
                        }
                        default: break;
                    }
                    break;
                }
            }
            if (!IsMouseButtonDown(MOUSE_BUTTON_LEFT)) dragging = false;
            if (dragging) {
                for (const TestHit& h : testHits)
                    if (h.kind == TestHit::Timeline) {
                        tv.playing = false;
                        scrubTo((mp.x - h.r.x - 6) / std::max(1.0f, h.r.width - 12) * h.b);
                    }
            }
            const float wheel = GetMouseWheelMove();
            if (wheel != 0 && tv.window == 3) tv.eventTop = std::max(0, tv.eventTop - (int)wheel * 3);
            if (IsKeyPressed(KEY_R) && !tv.replay) {
                saveTest("restarted", false);
                if (!startTest()) quit = true;
            }
            if (IsKeyPressed(KEY_ESCAPE) && !shotMode) {
                if (tv.window) tv.window = 0;
                else {
                    saveTest("stopped", false);
                    inMenu = true;
                    phase = Phase::Race;
                    applyMenu();
                }
            }

            // ---- simulation (live) or playback (from the cursor)
            if (phase == Phase::Test && !inMenu) {
                if (tv.live && liveRun && !st.paused && !shotMode) {
                    if (tv.fastForward) {
                        const double until = GetTime() + 0.025;
                        while (GetTime() < until && !race->isOver())
                            for (int i = 0; i < 200 && !race->isOver(); ++i) { race->step(); rec.update(*race); }
                    } else {
                        simDebt += frameDt * st.timeScale;
                        long long steps = (long long)(simDebt / race->dt());
                        simDebt -= steps * race->dt();
                        for (long long i = 0; i < steps && !race->isOver(); ++i) { race->step(); rec.update(*race); }
                    }
                } else if (shotMode && tv.live && liveRun) {
                    while (!race->isOver() && race->time() < cfg.screenshotAt) { race->step(); rec.update(*race); }
                    if (cfg.scrubAt >= 0) scrubTo(cfg.scrubAt);
                }
                if (tv.playing) {
                    tv.cursor += frameDt * st.timeScale;
                    if (tv.cursor >= rec.endTime()) {
                        tv.playing = false;
                        if (liveRun) { tv.live = true; st.paused = false; }
                        else tv.cursor = rec.endTime();
                    }
                }
                if (liveRun && race->isOver()) {
                    const rr::Car& c = race->cars()[0];
                    rec.update(*race);
                    saveTest(c.dnf ? c.dnfReason : c.finished ? "finished" : "time limit", c.finished);
                    tv.runOver = true;
                    tv.fastForward = false;
                    tv.live = false;
                    tv.cursor = rec.endTime();
                }
            }
        } else {
            // ---- input
            const bool shift = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
            auto pick = [&](int car) { st.focus = car; st.followLeader = false; };
            if (IsKeyPressed(KEY_TAB) || IsKeyPressed(KEY_RIGHT)) pick((st.focus + 1) % n);
            if (IsKeyPressed(KEY_LEFT)) pick((st.focus + n - 1) % n);
            for (int k = 0; k < 9 && k < n; ++k)
                if (IsKeyPressed(KEY_ONE + k)) pick(race->order()[k]);
            if (IsKeyPressed(KEY_L)) st.followLeader = !st.followLeader;
            if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                const int tab = race->isOver() ? hud->resultsTabAt(GetMousePosition()) : -1;
                // The race-end window covers the timing tower: its rows pick cars, not the tower's.
                bool onResults = false;
                const int rowCar = race->isOver() ? hud->resultsCarAt(GetMousePosition(), &onResults) : -1;
                const int car = onResults ? rowCar : hud->towerCarAt(*race, st, GetMousePosition());
                if (tab >= 0) st.resultsWindow = tab;
                else if (car >= 0) pick(car);
            }
            // race-end windows
            if (race->isOver()) {
                if (IsKeyPressed(KEY_RIGHT_BRACKET) || IsKeyPressed(KEY_PAGE_DOWN)) st.resultsWindow = (st.resultsWindow + 1) % 6;
                if (IsKeyPressed(KEY_LEFT_BRACKET) || IsKeyPressed(KEY_PAGE_UP)) st.resultsWindow = (st.resultsWindow + 5) % 6;
                if (IsKeyPressed(KEY_G)) st.showResults = !st.showResults;
            }
            if (IsKeyPressed(KEY_C)) st.camera = (CamMode)((st.camera + (shift ? CAM_COUNT - 1 : 1)) % CAM_COUNT);
            for (int k = 0; k < CAM_COUNT && k < 8; ++k)
                if (IsKeyPressed(KEY_F2 + k)) st.camera = (CamMode)k;
            if (IsKeyPressed(KEY_T)) st.camera = CAM_TCAM;
            if (IsKeyPressed(KEY_B)) st.camera = CAM_NOSE;
            if (IsKeyPressed(KEY_SPACE)) st.paused = !st.paused;
            if (IsKeyPressed(KEY_EQUAL) || IsKeyPressed(KEY_KP_ADD)) st.timeScale = std::min(64.0f, st.timeScale * 2);
            if (IsKeyPressed(KEY_MINUS) || IsKeyPressed(KEY_KP_SUBTRACT)) st.timeScale = std::max(0.125f, st.timeScale / 2);
            if (IsKeyPressed(KEY_P)) st.view.showPaths = !st.view.showPaths;
            if (IsKeyPressed(KEY_F10)) st.view.quality = (st.view.quality + 1) % 3;
            if (IsKeyPressed(KEY_S)) st.view.showSensors = !st.view.showSensors;
            if (IsKeyPressed(KEY_M)) st.muted = !st.muted;
            if (IsKeyPressed(KEY_H)) st.showHud = !st.showHud;
            if (IsKeyPressed(KEY_F1)) st.showHelp = !st.showHelp;
            const bool enter = IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER);
            if ((phase == Phase::Quali || phase == Phase::Practice) && enter) {
                skipRun = true;
                skipAll = skipAll || shift;
            }
            else if (phase == Phase::PracticeDone && enter && !startQuali()) quit = true;
            else if (phase == Phase::QualiDone && enter && !startWeekendRace()) quit = true;
            else if (inSeason && phase == Phase::Race && enter && race->isOver()) {
                inMenu = true;  // the season page, with the new standings
                st.notice.clear();
            }
            if (IsKeyPressed(KEY_ESCAPE) && !shotMode) {
                inMenu = true;
                phase = Phase::Race;
                st.qualifying = false;
                st.notice.clear();
                if (!inSeason) applyMenu();  // back to the grid
            }
            if (IsKeyPressed(KEY_R) && phase == Phase::Race && !inSeason) {
                auto fresh = makeRace(cfg, paths);
                if (fresh) {
                    race = std::move(fresh);
                    simDebt = 0;
                    st.focus = std::min(st.focus, (int)race->cars().size() - 1);
                }
            }

            // ---- simulation: fixed steps, as many as real time x speed asks for
            if (!shotMode) {
                // Start lights before every race: the cars wait on the grid, then go when the lights go out.
                if (phase == Phase::Race && race.get() != lightsFor) {
                    lightsFor = race.get();
                    st.lights = race->time() <= 0 ? 6.0f : -2.0f;
                } else if (phase != Phase::Race) {
                    st.lights = -2.0f;
                }
                if (st.lights > 0 && !st.paused) {
                    st.lights -= frameDt;
                    if (st.lights <= 0) simDebt = 0;
                } else if (st.lights > -2.0f && !st.paused) {
                    st.lights -= frameDt;  // "GO" shows for a moment while the cars pull away
                }
                if (st.lights > 0) {
                } else if ((skipRun || skipAll) && (phase == Phase::Quali || phase == Phase::Practice)) {
                    // fast-forward: as much of the run as fits in a few milliseconds a frame
                    const double until = GetTime() + 0.03;
                    while (!race->isOver() && GetTime() < until)
                        for (int k = 0; k < 200 && !race->isOver(); ++k) race->step();
                    simDebt = 0;
                } else if (!st.paused) {
                    simDebt += frameDt * st.timeScale;
                    long long steps = (long long)(simDebt / race->dt());
                    simDebt -= steps * race->dt();
                    for (long long i = 0; i < steps && !race->cooledDown(); ++i) race->step();
                } else if (IsKeyPressed(KEY_N)) {
                    race->advance(1.0 / race->config().robotHz);
                }
            }

            if ((phase == Phase::Quali || phase == Phase::Practice) && race->isOver()) endRun();
            // The race log: results, lap times and positions, and every pit stop
            // with the algorithm's reason, saved once when the race ends.
            if (phase == Phase::Race && race->isOver() && loggedRace != race.get() && !shotMode) {
                loggedRace = race.get();
                std::string path = cfg.jsonOut;
                if (path.empty()) {
                    const std::string logDir = dir + "/race_logs";
                    std::error_code ec;
                    std::filesystem::create_directories(logDir, ec);
                    char stamp[32];
                    const std::time_t now = std::time(nullptr);
                    std::strftime(stamp, sizeof stamp, "%Y%m%d_%H%M%S", std::localtime(&now));
                    path = logDir + "/race_" + stamp + "_" + race->config().track + ".json";
                }
                if (race->writeJson(path, 0)) {
                    st.logPath = std::filesystem::path(path).lexically_normal().string();
                    std::printf("race log: %s\n", st.logPath.c_str());
                } else {
                    st.logPath.clear();
                }
            }
            if (inSeason && phase == Phase::Race && race->isOver() && !seasonRecorded && !shotMode) {
                seasonRecorded = true;
                season.record(*race, raceIds);
                const int done = season.roundsDone();
                if (season.save(seasonPath, &err))
                    st.notice = "Round " + std::to_string(done) + " of " + std::to_string(season.rounds.size()) +
                                " saved to the championship.   Enter: standings";
                else
                    st.notice = "Could not save the championship: " + err;
            }
            if (!race->isOver() && loggedRace == race.get()) loggedRace = nullptr;
            if (phase != Phase::Race) st.focus = 0;
            else if (st.followLeader) st.focus = race->order()[0];
            CamMode shot = st.camera;
            if (st.camera == CAM_DIRECTOR && phase == Phase::Race) {
                if (directorRace != race.get()) director.reset(), directorRace = race.get();
                director.update(*race, shotMode ? 1.0f / 60 : frameDt);
                if (director.focus() >= 0) st.focus = director.focus();
                shot = director.shot();
                st.directorCaption = director.caption();
            }
            renderer->updateCamera(*race, st.focus, shot, shotMode ? 1.0f / 60 : frameDt);
        }
        // Testing away from live: show the car as it was at the cursor.
        const bool testing = phase == Phase::Test && !inMenu;
        const bool showRecorded = testing && !tv.live && !rec.empty();
        struct Shown { rr::CarState state; RRControl control; float lateral; bool onTrack; } liveCar{};
        st.lapClock = st.lastLap = st.bestLap = -1;
        if (showRecorded) {
            rr::Car& c = race->carsForReplay()[0];
            liveCar = {c.state, c.control, c.lateral, c.onTrack};
            const rr::TestSample x = rec.at(tv.cursor);
            c.state = x.s;
            c.control.steer = x.steer;
            c.control.accel = x.accel;
            c.control.brake = x.brake;
            c.control.status[0] = 0;
            c.control.pit_window[0] = c.control.pit_window[1] = 0;
            c.lateral = x.lateral;
            c.onTrack = x.onTrack;
            st.lapClock = std::max(0.0f, x.lapTime);
            st.lastLap = x.lap >= 2 && x.lap - 2 < (int)rec.laps.size() ? rec.laps[x.lap - 2].time : 0;
            st.bestLap = 0;
            for (int i = 0; i + 1 < x.lap && i < (int)rec.laps.size(); ++i)
                if (st.bestLap == 0 || rec.laps[i].time < st.bestLap) st.bestLap = rec.laps[i].time;
        }
        if (testing) renderer->updateCamera(*race, 0, st.camera, shotMode ? 1.0f / 60 : frameDt);

        // engine sound only while racing at (close to) real time
        const bool liveSound = testing ? (tv.live && !tv.runOver) || tv.playing : true;
        audio.update(*race, renderer->camera, st.focus,
                     !inMenu && !st.paused && !st.muted && st.timeScale <= 2.0f && !race->cooledDown() && liveSound,
                     frameDt);

#ifdef RR2_RENDERER
        if (!inMenu) {  // sky and lighting, as in rr_trackview: K next sky, ; ' turn the sun, , . exposure
            const float ldt = std::min(frameDt, 0.1f);
            renderer->adjustLighting(IsKeyPressed(KEY_K), (IsKeyDown(KEY_APOSTROPHE) - IsKeyDown(KEY_SEMICOLON)) * ldt * 0.5f,
                                     1.0f + (IsKeyDown(KEY_PERIOD) - IsKeyDown(KEY_COMMA)) * ldt);
        }
#endif

        BeginDrawing();
        ClearBackground(BLACK);
        renderer->draw(*race, inMenu ? race->order()[0] : st.focus, st.view);
        if (inMenu) hud->drawMenu(menu, menuHits);
        else if (phase == Phase::QualiDone || phase == Phase::PracticeDone) hud->drawQualiResults(st);
        else if (testing) hud->drawTest(*race, st, tv, testHits);
        else hud->draw(*race, st);
        if (shotMode && ++shotFrames == 3) {
            rlDrawRenderBatchActive();
            Image img = LoadImageFromScreen();
            bool ok = ExportImage(img, cfg.screenshot.c_str());
            UnloadImage(img);
            EndDrawing();
            std::printf("%s %s at t=%.1fs\n", ok ? "saved" : "FAILED to save", cfg.screenshot.c_str(), race->time());
            break;
        }
        EndDrawing();
        if (showRecorded) {
            rr::Car& c = race->carsForReplay()[0];
            c.state = liveCar.state;
            c.control = liveCar.control;
            c.lateral = liveCar.lateral;
            c.onTrack = liveCar.onTrack;
        }
    }

    if (phase == Phase::Test) saveTest("closed", false);
    if (!shotMode && race->isOver() && !cfg.quiet && phase != Phase::Test) race->printResults(stdout);
    audio.shutdown();
    hud->shutdown();
    renderer->shutdown();
    CloseWindow();
    return 0;
}
