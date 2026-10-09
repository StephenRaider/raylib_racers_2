// The General Championship's team loader: what it accepts, what it rejects and why, and the file lock.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "championship.hpp"
#include "gc.hpp"
#include "spec.hpp"

namespace fs = std::filesystem;
using namespace rr;

static int failures = 0;
#define CHECK(cond, ...)                                       \
    do {                                                       \
        if (!(cond)) {                                         \
            std::printf("FAIL line %d: %s  ", __LINE__, #cond); \
            std::printf(__VA_ARGS__);                          \
            std::printf("\n");                                 \
            ++failures;                                        \
        }                                                      \
    } while (0)

static std::string root;

static void write(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << text;
}

// A 2048 x 2048 (or other size) "PNG": only the header is looked at.
static std::string fakePng(unsigned w, unsigned h) {
    std::string s("\x89PNG\r\n\x1a\n", 8);
    s += std::string("\0\0\0\x0d", 4) + "IHDR";
    for (unsigned v : {w, h}) {
        s += (char)(v >> 24); s += (char)(v >> 16); s += (char)(v >> 8); s += (char)v;
    }
    return s + std::string(16, '\0');
}

static void team(const std::string& id, const std::string& name, const std::string& sh, const std::string& model, int n1, int n2,
                 const std::string& stats = "") {
    const fs::path d = fs::path(root) / "teams" / id;
    write(d / "livery_1.png", fakePng(2048, 2048));
    write(d / "livery_2.png", fakePng(2048, 2048));
    write(d / "bots/a/a.c", "#include <math.h>\n#include \"../common/rr_safety.h\"\nint x;\n");
    write(d / "team.json",
          "{\"team\":\"" + name + "\",\"short\":\"" + sh + "\",\"model\":\"" + model + "\",\"stats\":\"" + stats +
              "\",\"drivers\":[{\"number\":" + std::to_string(n1) + ",\"livery\":\"livery_1.png\",\"bot\":\"bots/a\"},"
              "{\"number\":" + std::to_string(n2) + ",\"livery\":\"livery_2.png\",\"bot\":\"bots/a\"}]}");
}

static std::string problems(const GcEvent& ev) {
    std::string s;
    for (const auto& p : ev.problems) s += p + "\n";
    return s;
}

int main(int argc, char** argv) {
    root = argc > 1 ? argv[1] : "gc_test";
    fs::remove_all(root);
    fs::create_directories(root + "/teams");
    DevRules rules;
    std::string err;
    CHECK(loadDevRules(RR_SOURCE_DIR "/specs/development.json", rules, &err), "%s", err.c_str());

    team("mech", "Mechanical Engineering", "MEC", "ferrari", 7, 77);
    team("cse", "Computer Science", "CSE", "redbull", 1, 11);
    GcEvent ev;
    gcLoad(root, rules, false, ev);
    CHECK(ev.teams.size() == 2 && ev.ok(), "%s", problems(ev).c_str());
    CHECK(ev.teams.size() == 2 && ev.teams[0].shortName == "CSE" && ev.teams[0].modelNo == 3, "sorted by short name, model number");
    CHECK(ev.teams.size() == 2 && ev.teams[1].drivers[1].name == "MEC 2" && ev.teams[1].drivers[1].number == 77, "driver names");
    const Lineup L = gcLineup(ev);
    CHECK(L.driverCount() == 4 && L.teams[0].drivers[1].livery == 1 && L.teams[1].drivers[0].livery == 2, "lineup slots");

    // each of these is rejected, with a reason that names the problem
    struct Bad { const char* id; std::string needle; };
    std::vector<Bad> bad;
    team("b1", "Bad One", "bad", "ferrari", 20, 21); bad.push_back({"b1", "\"short\""});
    team("b2", "Bad Two", "BAD", "brabham", 22, 23); bad.push_back({"b2", "not a car model"});
    team("zz3", "Bad Three", "BTH", "ferrari", 7, 24); bad.push_back({"zz3", "number 7"});
    team("b4", "Bad Four", "BFO", "ferrari", 25, 26, "top_speed=10,downforce=10,handling=10"); bad.push_back({"b4", "budget"});
    team("b5", "Bad Five", "BFI", "ferrari", 27, 28);
    write(fs::path(root) / "teams/b5/livery_2.png", fakePng(1024, 1024)); bad.push_back({"b5", "2048"});
    team("b6", "Bad Six", "BSI", "ferrari", 29, 30);
    write(fs::path(root) / "teams/b6/bots/a/prebuilt.so", "ELF"); bad.push_back({"b6", "file type"});
    team("b7", "Bad Seven", "BSE", "ferrari", 31, 32);
    write(fs::path(root) / "teams/b7/bots/a/a.c", "#include \"/etc/passwd\"\n"); bad.push_back({"b7", "leaves the bot's folder"});
    team("zz8", "Computer Science", "BEI", "ferrari", 33, 34); bad.push_back({"zz8", "team name"});
    team("b9", "Bad Nine", "BNI", "ferrari", 35, 35); bad.push_back({"b9", "both drivers"});
    team("b10", "Bad Ten", "BTE", "ferrari", 36, 37);
    write(fs::path(root) / "teams/b10/bots/a/a.c", "int x;\n__asm__(\".incbin \\\"/etc/passwd\\\"\");\n"); bad.push_back({"b10", ".incbin"});
    team("b11", "Bad Eleven", "BEL", "ferrari", 38, 39);
    fs::remove_all(fs::path(root) / "teams/b11/bots/a"); fs::create_directories(fs::path(root) / "teams/b11/bots/a");
    bad.push_back({"b11", "no .c or .cpp"});
    team("b12", "Bad Twelve", "BTW", "ferrari", 40, 41);
    std::error_code ec;
    fs::create_symlink("/etc/passwd", fs::path(root) / "teams/b12/link.txt", ec);
    if (!ec) bad.push_back({"b12", "link"});
    team("b13", "Bad Thirteen", "B13", "ferrari", 42, 43);
    write(fs::path(root) / "teams/b13/team.json", "{ not json");
    bad.push_back({"b13", "valid JSON"});
    team("b14", "Bad Fourteen", "B14", "ferrari", 44, 45, "top_speed=3");  // fine: under budget is allowed
    gcLoad(root, rules, false, ev);
    const std::string all = problems(ev);
    for (const Bad& b : bad) {
        const std::string line = "team '" + std::string(b.id) + "': ";
        const size_t at = all.find(line);
        const size_t end = at == std::string::npos ? at : all.find('\n', at);
        CHECK(at != std::string::npos && all.substr(at, end - at).find(b.needle) != std::string::npos, "%s should mention '%s'\n%s", b.id,
              b.needle.c_str(), all.c_str());
    }
    bool b14 = false;
    for (const GcTeam& t : ev.teams) b14 |= t.id == "b14";
    CHECK(b14, "a team under the stat budget is accepted");
    // a bot that is not built is a problem once the event wants built bots
    GcEvent ev2;
    gcLoad(root, rules, true, ev2);
    CHECK(problems(ev2).find("team 'mech': driver 1: the bot 'bots/a' is not built") != std::string::npos, "%s", problems(ev2).c_str());

    // clean up the bad ones; lock the good two
    for (const Bad& b : bad) fs::remove_all(fs::path(root) / "teams" / b.id);
    fs::remove_all(fs::path(root) / "teams/b14");
    write(fs::path(root) / "bots/cse__bots_a.so", "lib1");   // stand-ins for the built bots
    write(fs::path(root) / "bots/mech__bots_a.so", "lib2");
    gcLoad(root, rules, true, ev);
    CHECK(ev.ok() && ev.teams.size() == 2, "%s", problems(ev).c_str());
    Championship c;
    c.lineup = gcLineup(ev);
    c.rounds.push_back({"circuit", 1});
    c.gcLock = gcLock(ev);
    CHECK(c.gcLock.size() == 2 && c.gcLock[0].second.size() == 64, "digests");
    CHECK(gcVerify(root, c).empty(), "unchanged files verify");
    // round trip through the season file
    const std::string path = root + "/season.json";
    CHECK(c.save(path, &err), "%s", err.c_str());
    Championship d;
    CHECK(Championship::load(path, d, &err), "%s", err.c_str());
    CHECK(d.gcLock == c.gcLock && !d.gcInvalid, "lock survives save/load (%zu)", d.gcLock.size());
    // any change is caught: a bot source, a livery, the manifest, a new file, a removed team
    write(fs::path(root) / "teams/cse/bots/a/a.c", "#include <math.h>\nint x; /* tuned */\n");
    CHECK(gcVerify(root, c).find("'cse' has changed") != std::string::npos, "source change: %s", gcVerify(root, c).c_str());
    write(fs::path(root) / "teams/cse/bots/a/a.c", "#include <math.h>\n#include \"../common/rr_safety.h\"\nint x;\n");
    CHECK(gcVerify(root, c).empty(), "restoring the file restores the digest");
    write(fs::path(root) / "teams/mech/livery_1.png", fakePng(2048, 2048) + "x");
    CHECK(gcVerify(root, c).find("'mech' has changed") != std::string::npos, "livery change");
    write(fs::path(root) / "teams/mech/livery_1.png", fakePng(2048, 2048));
    write(fs::path(root) / "teams/mech/notes.txt", "hello");
    CHECK(gcVerify(root, c).find("'mech' has changed") != std::string::npos, "new file");
    fs::remove(fs::path(root) / "teams/mech/notes.txt");
    write(fs::path(root) / "bots/mech__bots_a.so", "lib-swapped");   // a built bot library counts too
    CHECK(gcVerify(root, c).find("'mech'") != std::string::npos, "built library");
    write(fs::path(root) / "bots/mech__bots_a.so", "lib2");
    CHECK(gcVerify(root, c).empty(), "all restored");
    fs::remove_all(fs::path(root) / "teams/cse");
    CHECK(gcVerify(root, c).find("removed") != std::string::npos, "removed team");

    // import: a folder holding the team folder is accepted, a zip without team.json is not
    fs::create_directories(root + "/x/inner");
    write(fs::path(root) / "x/inner/team.json", "{}");
    CHECK(gcImportFolder(root, root + "/x", "inner_team").empty() && fs::exists(root + "/teams/inner_team/team.json"), "nested import");
    CHECK(!gcImportFolder(root, root + "/x", "inner_team").empty(), "no overwrite");
    fs::create_directories(root + "/y");
    CHECK(!gcImportFolder(root, root + "/y", "other").empty(), "no team.json");
    CHECK(!gcImportFolder(root, root + "/x", "../evil").empty(), "bad id");

    // zips: a plain one passes; climbing paths, absolute paths and links do not
    struct E { std::string name; unsigned mode; };
    auto makeZip = [&](const std::string& file, const std::vector<E>& entries) {
        std::string data, dir;
        for (const E& e : entries) {
            auto le = [](std::string& s, unsigned v, int bytes) { for (int i = 0; i < bytes; ++i) s += (char)(v >> (8 * i)); };
            const unsigned off = (unsigned)data.size();
            data += "PK\x03\x04"; le(data, 20, 2); le(data, 0, 2); le(data, 0, 2); le(data, 0, 4); le(data, 0, 4); le(data, 0, 4); le(data, 0, 4);
            le(data, (unsigned)e.name.size(), 2); le(data, 0, 2); data += e.name;
            dir += "PK\x01\x02"; le(dir, 0x031e, 2); le(dir, 20, 2); le(dir, 0, 2); le(dir, 0, 2); le(dir, 0, 4); le(dir, 0, 4); le(dir, 0, 4);
            le(dir, 0, 4); le(dir, (unsigned)e.name.size(), 2); le(dir, 0, 2); le(dir, 0, 2); le(dir, 0, 2); le(dir, 0, 2);
            le(dir, e.mode << 16, 4); le(dir, off, 4); dir += e.name;
        }
        std::string end = "PK\x05\x06";
        auto le = [](std::string& s, unsigned v, int bytes) { for (int i = 0; i < bytes; ++i) s += (char)(v >> (8 * i)); };
        le(end, 0, 2); le(end, 0, 2); le(end, (unsigned)entries.size(), 2); le(end, (unsigned)entries.size(), 2);
        le(end, (unsigned)dir.size(), 4); le(end, (unsigned)data.size(), 4); le(end, 0, 2);
        write(file, data + dir + end);
    };
    makeZip(root + "/ok.zip", {{"Team/team.json", 0100644}, {"Team/bots/a/a.c", 0100644}});
    CHECK(gcCheckZip(root + "/ok.zip").empty(), "%s", gcCheckZip(root + "/ok.zip").c_str());
    makeZip(root + "/up.zip", {{"team.json", 0100644}, {"../../x.txt", 0100644}});
    CHECK(gcCheckZip(root + "/up.zip").find("climbs") != std::string::npos, "dotdot");
    makeZip(root + "/abs.zip", {{"/etc/x", 0100644}});
    CHECK(!gcCheckZip(root + "/abs.zip").empty(), "absolute");
    makeZip(root + "/win.zip", {{"C:/x", 0100644}});
    CHECK(!gcCheckZip(root + "/win.zip").empty(), "drive");
    makeZip(root + "/back.zip", {{"a\\..\\..\\x", 0100644}});
    CHECK(!gcCheckZip(root + "/back.zip").empty(), "backslashes");
    makeZip(root + "/link.zip", {{"l", 0120777}});
    CHECK(gcCheckZip(root + "/link.zip").find("link") != std::string::npos, "symlink");
    write(root + "/junk.zip", "hello");
    CHECK(!gcCheckZip(root + "/junk.zip").empty(), "not a zip");

    fs::remove_all(root);
    std::printf(failures ? "%d failure(s)\n" : "gc: all checks passed\n", failures);
    return failures ? 1 : 0;
}
