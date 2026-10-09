#include "config.hpp"
#include "rr/robot_api.h"

#include <algorithm>
#include <filesystem>
#include <stdexcept>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

namespace fs = std::filesystem;

namespace rr {

std::string usage(const char* prog, bool viewer) {
    std::string u = std::string("usage: ") + prog + " [options] --car ROBOT [--car ROBOT ...]\n\n" +
        "Race setup\n"
        "  --track NAME|FILE      track name in tracks/ or a .trk file (default circuit)\n"
        "  --laps N               race length (default 3)\n"
        "  --car ROBOT            add a car; ROBOT is a name in bots/ or a path to a robot library\n"
        "  --params STR           parameters for the last --car, e.g. \"speed=1.1,line=0.3\"\n"
        "  --name STR             display name for the last --car\n"
        "  --spec NAME|FILE       car spec for the last --car (specs/*.json; default: the built-in F1 car)\n"
        "  --dev STR              team stats (0-10, 40 points) for the last --car, e.g. \"top_speed=8,handling=2\"\n"
        "  --tires soft|medium|hard  starting tyres for the last --car (default: the robot decides)\n"
        "  --fuel LITRES          starting fuel for the last --car (default: the robot decides)\n"
        "  --dev-rules NAME|FILE  team stat rules (default specs/development.json)\n"
        "  --seed N               random seed (sensor noise)\n"
        "  --noise X              range-finder noise, relative std-dev (default 0)\n"
        "  --dt SECONDS           physics step (default 0.002)\n"
        "  --robot-hz N           how often robots drive (default 50)\n"
        "  --max-time SECONDS     abort the race after this long (default: automatic)\n"
        "  --fuel-rate X          fuel consumption multiplier (default 1)\n"
        "  --wear-rate X          tyre wear multiplier (default 1; raise it to force stops in short races)\n"
        "  --ambient C            air and track temperature (default 25): hotter days overheat the tyres\n"
        "  --two-compounds on|off|auto  every car must use two compounds (auto: races over 20 laps)\n"
        "  --no-pits              pit requests are ignored (robots are told, ABI 6)\n"
        "  --vsc off|auto         incident holds, yellow flags and the virtual safety car in races (default auto)\n"
        "  --wet                  a wet track: no DRS (there is no rain model yet)\n"
        "Competition\n"
        "  --sandbox              run each robot in its own locked-down process (rr_bothost): no file\n"
        "                         access, and a robot that crashes or hangs only loses its car\n"
        "  --cpu-cap MS           CPU one drive() call may use; a later answer is ignored (the car keeps\n"
        "                         its last controls) and over 50 such calls put the car out\n"
        "Output\n"
        "  --json FILE            write results as JSON\n"
        "  --telemetry DIR        write one CSV per car at the robot rate\n"
        "  --test-log DIR         record the first car like the viewer's Testing mode and save the run\n"
        "                         in DIR (runs.json, run_N/summary.json, run_N/telemetry.csv)\n"
        "  --quiet                print only the results\n"
        "  --cool-down            after the flag, run on until the cars have parked in the pit lane\n";
    if (!viewer) {
        u += "Weekend (each car practises and qualifies alone; robots keep a weekend memory, ABI 8)\n"
             "  --practice [LAPS]      practice before the race, up to LAPS laps per car (default 15)\n"
             "  --qualifying           qualifying sets the grid (an out lap and two flying laps per car)\n"
             "                         (with --championship: only when starting a new season)\n";
        u += "Championship\n"
             "  --championship FILE    race the next round of the season saved in FILE, then save it;\n"
             "                         a new FILE starts a season with --lineup or the --car entries\n"
             "                         (consecutive pairs are teams; a team takes its first car's --dev)\n"
             "  --lineup FILE          the teams, their stats and drivers for a new season\n"
             "  --rounds LIST          calendar of a new season, e.g. monza:10,spa:8 (default: the eight\n"
             "                         championship tracks at --laps)\n"
             "  --distance KM          race length of a new season: each round gets the laps that\n"
             "                         cover about KM on its track (instead of the laps in --rounds)\n"
             "  --wear-rate X          one tyre wear rate for the whole season\n"
             "  --all-rounds           race every remaining round\n"
             "  --save-lineup FILE     save the --car entries as a lineup and exit\n";
    }
    if (viewer) {
        u += "Viewer\n"
             "  --width N --height N  window size (default 1600x900)\n"
             "  --fullscreen\n"
             "  --speed X             start at X times real time\n"
             "  --camera N            0 follow, 1 cinematic, 2 TV, 3 helicopter, 4 top down, 5 orbit, 6 overview, 7 director\n"
             "  --quality N           graphics 0 low, 1 medium, 2 high (default)\n"
             "  --focus N             follow car N (0 = first --car); default: whoever leads\n"
             "  --screenshot FILE     save a screenshot at --at seconds of race time, then exit\n"
             "  --at SECONDS\n"
             "  --no-menu             skip the race setup menu\n"
             "  --mute                no engine sound (M toggles it)\n"
             "  --sound-test FILE     render 25 s of engine sound from --at seconds to a WAV, no window\n"
             "  --test                open the Testing session for the first --car (with --no-menu: start the run)\n"
             "  --test-view N         testing screen: 0 dashboard, 1 driving, 2 session, 3 track and events\n"
             "  --results N           race-end window: 1 results, 2 positions, 3 lap chart, 4 lap times,\n"
             "                        5 strategy, 6 incidents\n"
             "  --scrub SECONDS       testing screen: show that moment of the run (for screenshots)\n"
             "  --page grid|stats|runs|champ|season|lineups  open a setup page (for screenshots)\n"
             "Keys: Tab/Left/Right focus car, L follow leader, C or F2-F8 camera, Space pause, +/- speed, M mute, R restart, Esc menu, H HUD, F1 help\n";
    }
    return u;
}

bool parseArgs(int argc, char** argv, RaceConfig& cfg, bool viewer, bool& wantHelp, std::string* err) {
    wantHelp = false;
    auto need = [&](int& i, const std::string& opt) -> const char* {
        if (i + 1 >= argc) throw std::runtime_error(opt + " needs a value");
        return argv[++i];
    };
    try {
        for (int i = 1; i < argc; ++i) {
            std::string a = argv[i];
            if (a == "-h" || a == "--help") { wantHelp = true; return true; }
            else if (a == "--track") cfg.track = need(i, a);
            else if (a == "--laps") cfg.laps = std::stoi(need(i, a));
            else if (a == "--car") { cfg.entries.emplace_back(); cfg.entries.back().robot = need(i, a); }
            else if (a == "--tires") {
                std::string v = need(i, a);
                if (cfg.entries.empty()) throw std::runtime_error(a + " must follow a --car");
                int t = v == "soft" || v == "1" ? RR_TIRE_SOFT : v == "medium" || v == "2" ? RR_TIRE_MEDIUM
                        : v == "hard" || v == "3" ? RR_TIRE_HARD : 0;
                if (!t) throw std::runtime_error("--tires takes soft, medium or hard");
                cfg.entries.back().tires = t;
            }
            else if (a == "--fuel") {
                const float v = std::stof(need(i, a));
                if (cfg.entries.empty()) throw std::runtime_error(a + " must follow a --car");
                if (v <= 0) throw std::runtime_error("--fuel must be positive");
                cfg.entries.back().fuel = v;
            }
            else if (a == "--no-pits") cfg.pitsClosed = true;
            else if (a == "--wet") cfg.wet = true;
            else if (a == "--vsc") {
                const std::string v = need(i, a);
                if (v == "off") cfg.neutral = false;
                else if (v == "auto" || v == "on") cfg.neutral = true;
                else throw std::runtime_error("--vsc takes off or auto");
            }
            else if (a == "--sandbox") cfg.sandbox = true;
            else if (a == "--cpu-cap") {
                cfg.cpuCapMs = std::stof(need(i, a));
                if (cfg.cpuCapMs < 0) throw std::runtime_error("--cpu-cap must not be negative");
            }
            else if (!viewer && a == "--practice") {
                cfg.practiceLaps = 15;
                if (i + 1 < argc && argv[i + 1][0] != '-') cfg.practiceLaps = std::stoi(argv[++i]);
                if (cfg.practiceLaps < 1) throw std::runtime_error("--practice takes a lap count of 1 or more");
            }
            else if (!viewer && a == "--qualifying") cfg.qualifying = true;
            else if (a == "--test-log") cfg.testLog = need(i, a);
            else if (a == "--two-compounds") {
                std::string v = need(i, a);
                cfg.twoCompounds = v == "on" ? 1 : v == "off" ? 0 : v == "auto" ? -1 : -2;
                if (cfg.twoCompounds == -2) throw std::runtime_error("--two-compounds takes on, off or auto");
            }
            else if (a == "--params" || a == "--name" || a == "--spec" || a == "--dev") {
                const char* v = need(i, a);
                if (cfg.entries.empty()) throw std::runtime_error(a + " must follow a --car");
                auto& e = cfg.entries.back();
                (a == "--params" ? e.params : a == "--name" ? e.name : a == "--spec" ? e.spec : e.dev) = v;
            }
            else if (a == "--dev-rules") cfg.devRules = need(i, a);
            else if (a == "--seed") cfg.seed = std::stoull(need(i, a));
            else if (a == "--noise") cfg.sensorNoise = std::stof(need(i, a));
            else if (a == "--dt") cfg.dt = std::stof(need(i, a));
            else if (a == "--robot-hz") cfg.robotHz = std::stoi(need(i, a));
            else if (a == "--fuel-rate") cfg.fuelRate = std::stof(need(i, a));
            else if (a == "--wear-rate") cfg.wearRate = std::stof(need(i, a));
            else if (a == "--ambient") cfg.ambient = std::stof(need(i, a));
            else if (a == "--max-time") cfg.maxTime = std::stof(need(i, a));
            else if (a == "--json") cfg.jsonOut = need(i, a);
            else if (a == "--telemetry") cfg.telemetryDir = need(i, a);
            else if (a == "--quiet") cfg.quiet = true;
            else if (a == "--cool-down") cfg.coolDown = true;
            else if (!viewer && a == "--championship") cfg.championship = need(i, a);
            else if (!viewer && a == "--lineup") cfg.lineup = need(i, a);
            else if (!viewer && a == "--rounds") cfg.rounds = need(i, a);
            else if (!viewer && a == "--distance") cfg.distance = std::stof(need(i, a));
            else if (!viewer && a == "--all-rounds") cfg.allRounds = true;
            else if (!viewer && a == "--save-lineup") cfg.saveLineup = need(i, a);
            else if (viewer && a == "--width") cfg.width = std::stoi(need(i, a));
            else if (viewer && a == "--height") cfg.height = std::stoi(need(i, a));
            else if (viewer && a == "--fullscreen") cfg.fullscreen = true;
            else if (viewer && a == "--speed") cfg.timeScale = std::stof(need(i, a));
            else if (viewer && a == "--camera") cfg.camera = std::stoi(need(i, a));
            else if (viewer && a == "--quality") cfg.quality = std::clamp(std::stoi(need(i, a)), 0, 2);
            else if (viewer && a == "--focus") cfg.focus = std::stoi(need(i, a));
            else if (viewer && a == "--no-menu") cfg.noMenu = true;
            else if (viewer && a == "--mute") cfg.mute = true;
            else if (viewer && a == "--sound-test") cfg.soundTest = need(i, a);
            else if (viewer && a == "--test") cfg.test = true;
            else if (viewer && a == "--test-view") cfg.testView = std::stoi(need(i, a));
            else if (viewer && a == "--results") cfg.resultsView = std::stoi(need(i, a)) - 1;
            else if (viewer && a == "--scrub") cfg.scrubAt = std::stof(need(i, a));
            else if (viewer && a == "--page") cfg.page = need(i, a);
            else if (viewer && a == "--screenshot") cfg.screenshot = need(i, a);
            else if (viewer && a == "--at") cfg.screenshotAt = std::stof(need(i, a));
            else throw std::runtime_error("unknown option " + a);
        }
    } catch (const std::exception& e) {
        if (err) *err = e.what();
        return false;
    }
    if (cfg.laps < 1) { if (err) *err = "--laps must be at least 1"; return false; }
    if (cfg.dt <= 0 || cfg.dt > 0.02f) { if (err) *err = "--dt must be in (0, 0.02]"; return false; }
    if (cfg.fuelRate < 0 || cfg.wearRate < 0) { if (err) *err = "--fuel-rate and --wear-rate must not be negative"; return false; }
    if (cfg.robotHz < 1) { if (err) *err = "--robot-hz must be positive"; return false; }
    return true;
}

std::string botHostPath(const std::string& dir) {
#if defined(_WIN32)
    return dir + "/rr_bothost.exe";
#else
    return dir + "/rr_bothost";
#endif
}

std::string exeDir(const char* argv0) {
    std::error_code ec;
#if defined(_WIN32)
    char buf[MAX_PATH];
    DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    if (n > 0) return fs::path(std::string(buf, n)).parent_path().string();
#elif defined(__APPLE__)
    char buf[4096];
    uint32_t size = sizeof buf;
    if (_NSGetExecutablePath(buf, &size) == 0) return fs::canonical(buf, ec).parent_path().string();
#else
    auto p = fs::read_symlink("/proc/self/exe", ec);
    if (!ec) return p.parent_path().string();
#endif
    return fs::absolute(argv0, ec).parent_path().string();
}

}  // namespace rr
