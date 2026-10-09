// rr_gc: the General Championship's team tool.
//   rr_gc check  [--gc DIR] [--built]   list the teams and what is wrong with the rest
//   rr_gc import [--gc DIR]             unpack GC/submissions/*.zip into GC/teams
//   rr_gc plan   [--gc DIR]             write GC/build/bots.cmake: how to compile every team's bots
//   rr_gc smoke  [--gc DIR]             a one-lap sandboxed race of all the bots: crashes and CPU use
// Exit code 1 when a team was rejected (check, plan) or a bot failed (smoke).
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>

#include "config.hpp"
#include "gc.hpp"
#include "race.hpp"

namespace fs = std::filesystem;
using namespace rr;

namespace {

std::string gRoot;

bool loadRules(DevRules& rules, const std::string& exeDirPath) {
    const std::vector<std::string> dirs = {RR_SOURCE_DIR "/specs", exeDirPath + "/specs", "specs"};
    const std::string path = findDataFile("development", dirs);
    std::string err;
    if (path.empty() || !loadDevRules(path, rules, &err)) {
        std::fprintf(stderr, "error: cannot read specs/development.json (%s)\n", err.c_str());
        return false;
    }
    return true;
}

void listTeams(const GcEvent& ev) {
    std::printf("%zu team(s) accepted:\n", ev.teams.size());
    for (const GcTeam& t : ev.teams)
        std::printf("  %-4s %-32s %-10s #%-2d %s / #%-2d %s\n", t.shortName.c_str(), t.name.c_str(), t.model.c_str(),
                    t.drivers[0].number, t.drivers[0].botName.c_str(), t.drivers[1].number, t.drivers[1].botName.c_str());
    for (const std::string& p : ev.problems) std::printf("REJECTED %s\n", p.c_str());
}

int cmdCheck(const DevRules& rules, bool built) {
    GcEvent ev;
    gcLoad(gRoot, rules, built, ev);
    listTeams(ev);
    return ev.problems.empty() ? 0 : 1;
}

int cmdImport() {
    const fs::path sub = fs::path(gRoot) / "submissions", tmp = fs::path(gRoot) / ".import";
    std::error_code ec;
    int bad = 0;
    if (!fs::is_directory(sub, ec)) { std::printf("no submissions folder\n"); return 0; }
    std::vector<fs::path> zips;
    for (const auto& e : fs::directory_iterator(sub, ec))
        if (e.is_regular_file(ec) && e.path().extension() == ".zip") zips.push_back(e.path());
    std::sort(zips.begin(), zips.end());
    for (const fs::path& z : zips) {
        const std::string id = z.stem().string();
        if (fs::exists(fs::path(gRoot) / "teams" / id, ec)) { std::printf("skip %s: already imported\n", z.filename().string().c_str()); continue; }
        if (const std::string bad_zip = gcCheckZip(z.string()); !bad_zip.empty()) {
            std::printf("REJECTED %s: %s\n", z.filename().string().c_str(), bad_zip.c_str());
            ++bad;
            continue;
        }
        fs::remove_all(tmp / id, ec);
        fs::create_directories(tmp / id, ec);
        // gcCheckZip has read the zip's directory (no ../, links or absolute paths); cmake -E tar unpacks it on every platform
        const std::string cmd = "cmake -E chdir \"" + (tmp / id).string() + "\" cmake -E tar xf \"" + fs::absolute(z).string() + "\"";
        if (std::system(cmd.c_str()) != 0) { std::printf("REJECTED %s: cannot unpack the zip\n", z.filename().string().c_str()); ++bad; continue; }
        const std::string why = gcImportFolder(gRoot, (tmp / id).string(), id);
        fs::remove_all(tmp / id, ec);
        if (!why.empty()) { std::printf("REJECTED %s: %s\n", z.filename().string().c_str(), why.c_str()); ++bad; }
        else std::printf("imported %s\n", z.filename().string().c_str());
    }
    fs::remove_all(tmp, ec);
    return bad ? 1 : 0;
}

int cmdPlan(const DevRules& rules) {
    GcEvent ev;
    gcLoad(gRoot, rules, false, ev);
    listTeams(ev);
    fs::create_directories(fs::path(gRoot) / "build");
    std::ofstream f(fs::path(gRoot) / "build" / "bots.cmake");
    f << "# written by rr_gc plan: every accepted team's bots\n";
    for (const GcTeam& t : ev.teams) {
        std::set<std::string> done;
        for (const GcDriver& d : t.drivers) {
            if (!done.insert(d.botName).second) continue;
            // the bot folder again from the manifest: its sources
            const fs::path botDir = fs::path(t.dir) / d.botDir;
            f << "rr_gc_bot(" << gcBotLib(t.id, d.botName);
            std::error_code ec;
            for (const auto& e : fs::recursive_directory_iterator(botDir, ec)) {
                const std::string x = e.path().extension().string();
                if (e.is_regular_file(ec) && (x == ".c" || x == ".cpp" || x == ".cc" || x == ".cxx"))
                    f << " \"" << e.path().generic_string() << "\"";
            }
            f << ")\n";
        }
    }
    return ev.problems.empty() ? 0 : 1;
}

int cmdSmoke(const DevRules& rules, const std::string& exe) {
    GcEvent ev;
    gcLoad(gRoot, rules, true, ev);
    listTeams(ev);
    if (ev.teams.empty()) return 1;
    const Lineup L = gcLineup(ev);
    RaceConfig cfg;
    cfg.track = "circuit";
    cfg.laps = 1;
    cfg.carSpec = "f1_2013";
    cfg.sandbox = true;
    cfg.cpuCapMs = 2;
    cfg.botHost = botHostPath(exe);
    cfg.quiet = true;
    for (int id = 0; id < L.driverCount(); ++id) cfg.entries.push_back(L.entry(id));
    Race race;
    std::string err;
    if (!race.setup(cfg, {gRoot + "/bots", exe + "/bots"}, {exe + "/tracks", RR_SOURCE_DIR "/tracks", "tracks"}, &err)) {
        std::fprintf(stderr, "error: %s\n", err.c_str());
        return 1;
    }
    while (!race.isOver()) race.step();
    int bad = 0;
    std::printf("\none-lap smoke race, sandboxed, CPU cap 2 ms:\n");
    for (const Car& c : race.cars()) {
        const bool ok = !c.dnf && c.finished && c.cpuOverruns == 0;
        std::printf("  %-8s %-10s cpu avg %.3f ms  max %.3f ms  over cap %d  %s\n", c.name.c_str(),
                    c.finished ? "finished" : c.dnf ? c.dnfReason.c_str() : "unfinished",
                    c.driveCalls ? 1000 * c.cpuTotal / c.driveCalls : 0.0, 1000 * c.cpuMax, c.cpuOverruns, ok ? "OK" : "PROBLEM");
        bad += !ok;
    }
    return bad ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
    const std::string exe = exeDir(argv[0]);
    gRoot = fs::exists(exe + "/teams") ? exe : (fs::exists("GC/teams") ? "GC" : exe);
    std::string cmd = argc > 1 ? argv[1] : "check";
    bool built = false;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--gc" && i + 1 < argc) gRoot = argv[++i];
        else if (a == "--built") built = true;
    }
    gRoot = fs::absolute(gRoot).string();
    if (cmd == "import") return cmdImport();
    DevRules rules;
    if (!loadRules(rules, exe)) return 2;
    if (cmd == "check") return cmdCheck(rules, built);
    if (cmd == "plan") return cmdPlan(rules);
    if (cmd == "smoke") return cmdSmoke(rules, exe);
    std::fprintf(stderr, "usage: rr_gc check|import|plan|smoke [--gc DIR] [--built]\n");
    return 2;
}
