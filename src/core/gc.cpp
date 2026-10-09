#include "gc.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

#include "mini_json.hpp"
#include "robot_loader.hpp"
#include "sha256.hpp"

namespace rr {

namespace fs = std::filesystem;

namespace {

constexpr uintmax_t kMaxTeamBytes = 60u << 20;   // everything in a team folder
constexpr uintmax_t kMaxLiveryBytes = 16u << 20;
constexpr int kMaxFiles = 300;
constexpr int kLiverySize = 2048;

bool readAll(const fs::path& p, std::string& out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    std::stringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

std::string lower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

bool safeName(const std::string& s) {
    if (s.empty() || s.size() > 48) return false;
    for (char c : s)
        if (!std::isalnum((unsigned char)c) && c != '_' && c != '-') return false;
    return true;
}

// A relative path that stays inside its folder.
bool safeRelative(const std::string& p) {
    if (p.empty() || p[0] == '/' || p[0] == '\\' || p.find(':') != std::string::npos) return false;
    fs::path q(p);
    for (const auto& part : q)
        if (part == "..") return false;
    return true;
}

bool sourceExt(const std::string& e) { return e == ".c" || e == ".cpp" || e == ".cc" || e == ".cxx"; }
bool allowedExt(const std::string& e) {
    static const char* ok[] = {".c", ".cpp", ".cc", ".cxx", ".h", ".hpp", ".hh", ".inc", ".png", ".json", ".txt", ".md"};
    for (const char* x : ok)
        if (e == x) return true;
    return false;
}

// 2048 x 2048 PNG, read from the header.
std::string checkPng(const fs::path& p) {
    std::error_code ec;
    const uintmax_t size = fs::file_size(p, ec);
    if (ec) return "cannot read " + p.filename().string();
    if (size > kMaxLiveryBytes) return p.filename().string() + " is over 16 MB";
    std::ifstream f(p, std::ios::binary);
    unsigned char h[24] = {};
    f.read((char*)h, sizeof h);
    static const unsigned char sig[8] = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
    if (!f || std::memcmp(h, sig, 8) != 0 || std::memcmp(h + 12, "IHDR", 4) != 0)
        return p.filename().string() + " is not a PNG";
    const unsigned w = (unsigned)h[16] << 24 | h[17] << 16 | h[18] << 8 | h[19];
    const unsigned ht = (unsigned)h[20] << 24 | h[21] << 16 | h[22] << 8 | h[23];
    if ((int)w != kLiverySize || (int)ht != kLiverySize)
        return p.filename().string() + " is " + std::to_string(w) + " x " + std::to_string(ht) + ", it must be 2048 x 2048";
    return "";
}

// Includes that reach outside the bot's folder, and directives that pull files into the binary.
std::string checkSource(const fs::path& file, const fs::path& base) {
    std::string text;
    if (!readAll(file, text)) return "cannot read " + file.filename().string();
    std::istringstream in(text);
    std::string line;
    int n = 0;
    while (std::getline(in, line)) {
        ++n;
        const size_t a = line.find_first_not_of(" \t");
        if (a == std::string::npos) continue;
        const std::string l = line.substr(a);
        const std::string where = fs::relative(file, base).generic_string() + ":" + std::to_string(n);
        if (l.rfind("#", 0) == 0) {
            if (l.find("incbin") != std::string::npos || l.find("embed") != std::string::npos)
                return where + ": #embed / .incbin is not allowed";
            const size_t inc = l.find("include");
            if (inc != std::string::npos && inc <= 2) {
                const size_t q = l.find_first_of("\"<", inc);
                if (q != std::string::npos) {
                    const size_t e = l.find_first_of("\">", q + 1);
                    const std::string target = l.substr(q + 1, e == std::string::npos ? std::string::npos : e - q - 1);
                    // "../common/rr_*.h" is the example bots' own shared folder (the SDK), found through bots/simple
                    const bool sdk = target.rfind("../common/", 0) == 0 && safeRelative(target.substr(10));
                    if (!sdk && !target.empty() && (target[0] == '/' || target[0] == '\\' || target.find(':') != std::string::npos ||
                                            !safeRelative(target)))
                        return where + ": #include \"" + target + "\" leaves the bot's folder";
                }
            }
        }
        if (l.find(".incbin") != std::string::npos) return where + ": .incbin is not allowed";
    }
    return "";
}

// Every regular file under dir, relative, sorted. Symlinks and other oddities are reported.
bool listFiles(const fs::path& dir, std::vector<std::string>& out, std::string* why) {
    std::error_code ec;
    for (fs::recursive_directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        const fs::directory_entry& e = *it;
        if (e.is_symlink(ec)) { if (why) *why = e.path().filename().string() + " is a link"; return false; }
        if (e.is_directory(ec)) continue;
        if (!e.is_regular_file(ec)) { if (why) *why = e.path().filename().string() + " is not a plain file"; return false; }
        out.push_back(fs::relative(e.path(), dir).generic_string());
    }
    std::sort(out.begin(), out.end());
    return true;
}

}  // namespace

const std::vector<std::pair<std::string, int>>& gcModels() {
    static const std::vector<std::pair<std::string, int>> m = {
        {"sauber", 1}, {"mercedes", 2}, {"redbull", 3}, {"forceindia", 4}, {"lotus", 5}, {"williams", 6},
        {"caterham", 7}, {"mclaren", 8}, {"ferrari", 9}, {"tororosso", 10}, {"marussia", 11}};
    return m;
}

int gcModelNumber(const std::string& id) {
    for (const auto& m : gcModels())
        if (m.first == id) return m.second;
    return 0;
}

std::string gcBotLib(const std::string& teamId, const std::string& botName) { return teamId + "__" + botName; }

bool gcLoad(const std::string& root, const DevRules& rules, bool requireBuilt, GcEvent& out) {
    out = GcEvent{};
    out.root = root;
    const fs::path teamsDir = fs::path(root) / "teams";
    std::error_code ec;
    std::vector<fs::path> dirs;
    for (const auto& e : fs::directory_iterator(teamsDir, ec))
        if (e.is_directory(ec) && !e.is_symlink(ec)) dirs.push_back(e.path());
    std::sort(dirs.begin(), dirs.end());

    const std::vector<std::string> libDirs = {(fs::path(root) / "bots").string()};
    for (const fs::path& dir : dirs) {
        const std::string id = dir.filename().string();
        if (id[0] == '.' || id[0] == '_') continue;   // _example, .import ...
        auto bad = [&](const std::string& why) { out.problems.push_back("team '" + id + "': " + why); };
        if (!safeName(id)) { bad("the folder name may only use letters, digits, _ and -"); continue; }

        std::vector<std::string> files;
        std::string why;
        if (!listFiles(dir, files, &why)) { bad(why); continue; }
        if ((int)files.size() > kMaxFiles) { bad("more than " + std::to_string(kMaxFiles) + " files"); continue; }
        uintmax_t total = 0;
        bool filesOk = true;
        for (const std::string& f : files) {
            total += fs::file_size(dir / f, ec);
            const std::string e = lower(fs::path(f).extension().string());
            if (!allowedExt(e)) { bad("'" + f + "': this file type is not allowed (source, headers, PNG, JSON and text only; no compiled files)"); filesOk = false; break; }
        }
        if (!filesOk) continue;
        if (total > kMaxTeamBytes) { bad("the folder is over 60 MB"); continue; }

        std::string text;
        if (!readAll(dir / "team.json", text)) { bad("team.json is missing"); continue; }
        const mjson::Value j = mjson::parse(text);
        if (j.type != mjson::Value::Object) { bad("team.json is not valid JSON"); continue; }

        GcTeam t;
        t.id = id;
        t.dir = dir.string();
        t.name = j["team"].str();
        t.shortName = j["short"].str();
        t.model = lower(j["model"].str());
        t.stats = j["stats"].str();
        std::string err;
        if (t.name.empty() || t.name.size() > 40) err = "\"team\" (the team's name, up to 40 characters) is missing";
        else if (t.shortName.size() < 2 || t.shortName.size() > 4 ||
                 !std::all_of(t.shortName.begin(), t.shortName.end(), [](char c) { return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'); }))
            err = "\"short\" must be 2 to 4 capital letters or digits, like MEC";
        else if (!(t.modelNo = gcModelNumber(t.model)))
            err = "\"model\" '" + t.model + "' is not a car model (see GC/README.md for the list)";
        else if (j["drivers"].size() != 2 || j["drivers"].type != mjson::Value::Array)
            err = "\"drivers\" must list exactly 2 drivers";
        if (err.empty()) {
            std::vector<int> tokens;
            if (!rules.empty() && !parseDevelopment(rules, t.stats, tokens, &err)) err = "\"stats\": " + err;
        }
        for (int s = 0; err.empty() && s < 2; ++s) {
            const mjson::Value& d = j["drivers"][s];
            GcDriver& g = t.drivers[s];
            g.name = t.shortName + " " + std::to_string(s + 1);
            g.number = (int)d["number"].num(0);
            const std::string lv = d["livery"].str(), bot = d["bot"].str();
            const std::string who = "driver " + std::to_string(s + 1) + ": ";
            if (g.number < 1 || g.number > 99 || d["number"].num() != g.number) err = who + "\"number\" must be a whole number from 1 to 99";
            else if (!safeRelative(lv) || lower(fs::path(lv).extension().string()) != ".png" || !fs::exists(dir / lv))
                err = who + "\"livery\" '" + lv + "' is not a PNG in the team folder";
            else if (!safeRelative(bot) || !fs::is_directory(dir / bot, ec)) err = who + "\"bot\" '" + bot + "' is not a folder in the team folder";
            if (!err.empty()) break;
            if (std::string e2 = checkPng(dir / lv); !e2.empty()) { err = who + e2; break; }
            // the bot: a folder of C or C++ source
            std::vector<std::string> bf;
            bool any = false;
            listFiles(dir / bot, bf, nullptr);
            for (const std::string& f : bf) {
                const std::string e = lower(fs::path(f).extension().string());
                if (sourceExt(e)) {
                    any = true;
                    if (std::string e3 = checkSource(dir / bot / f, dir / bot); !e3.empty()) { err = who + "bot " + e3; break; }
                }
            }
            if (!err.empty()) break;
            if (!any) { err = who + "the bot folder '" + bot + "' has no .c or .cpp file"; break; }
            g.livery = (dir / lv).string();
            g.botDir = fs::path(bot).generic_string();
            g.botName = g.botDir;
            for (char& c : g.botName) if (c == '/') c = '_';
            if (!safeName(g.botName)) { err = who + "the bot folder name may only use letters, digits, _ and -"; break; }
            g.robot = RobotModule::find(gcBotLib(id, g.botName), libDirs);
            if (g.robot.empty() && requireBuilt) { err = who + "the bot '" + bot + "' is not built yet: run the build script"; break; }
        }
        if (err.empty() && t.drivers[0].number == t.drivers[1].number) err = "both drivers have the number " + std::to_string(t.drivers[0].number);
        if (!err.empty()) { bad(err); continue; }
        out.teams.push_back(t);
    }

    // rules across the whole event
    std::map<std::string, std::string> shortOwner, nameOwner;
    std::map<int, std::string> numberOwner;
    std::vector<GcTeam> keep;
    for (const GcTeam& t : out.teams) {
        std::string clash;
        if (shortOwner.count(t.shortName)) clash = "the short name " + t.shortName + " is also used by '" + shortOwner[t.shortName] + "'";
        else if (nameOwner.count(lower(t.name))) clash = "the team name is also used by '" + nameOwner[lower(t.name)] + "'";
        for (int s = 0; clash.empty() && s < 2; ++s)
            if (numberOwner.count(t.drivers[s].number))
                clash = "the number " + std::to_string(t.drivers[s].number) + " is also used by '" + numberOwner[t.drivers[s].number] + "'";
        if (!clash.empty()) { out.problems.push_back("team '" + t.id + "': " + clash); continue; }
        shortOwner[t.shortName] = t.id;
        nameOwner[lower(t.name)] = t.id;
        for (int s = 0; s < 2; ++s) numberOwner[t.drivers[s].number] = t.id;
        keep.push_back(t);
    }
    out.teams = keep;
    std::sort(out.teams.begin(), out.teams.end(), [](const GcTeam& a, const GcTeam& b) { return a.shortName < b.shortName; });
    while ((int)out.teams.size() > kGcMaxTeams) {
        out.problems.push_back("team '" + out.teams.back().id + "': the grid holds " + std::to_string(kGcMaxTeams) + " teams, this one is left out");
        out.teams.pop_back();
    }
    return true;
}

std::string gcDigest(const std::string& root, const std::string& teamId) {
    const fs::path dir = fs::path(root) / "teams" / teamId;
    Sha256 h;
    std::vector<std::string> files;
    if (!listFiles(dir, files, nullptr)) return "unreadable";
    std::string data;
    for (const std::string& f : files) {
        readAll(dir / f, data);
        h.update("F" + f + '\0' + std::to_string(data.size()) + '\0');
        h.update(data);
    }
    // the built libraries of this team's bots
    std::error_code ec;
    std::vector<std::string> libs;
    for (const auto& e : fs::directory_iterator(fs::path(root) / "bots", ec))
        if (e.path().filename().string().rfind(teamId + "__", 0) == 0) libs.push_back(e.path().filename().string());
    std::sort(libs.begin(), libs.end());
    for (const std::string& l : libs) {
        readAll(fs::path(root) / "bots" / l, data);
        h.update("L" + l + '\0' + std::to_string(data.size()) + '\0');
        h.update(data);
    }
    return h.hex();
}

std::vector<std::pair<std::string, std::string>> gcLock(const GcEvent& ev) {
    std::vector<std::pair<std::string, std::string>> v;
    for (const GcTeam& t : ev.teams) v.emplace_back(t.id, gcDigest(ev.root, t.id));
    std::sort(v.begin(), v.end());   // the season file keeps them by id
    return v;
}

std::string gcVerify(const std::string& root, const Championship& c) {
    std::string out;
    for (const auto& kv : c.gcLock) {
        const std::string now = fs::exists(fs::path(root) / "teams" / kv.first) ? gcDigest(root, kv.first) : "missing";
        if (now == kv.second) continue;
        out += (out.empty() ? "" : "; ") + std::string("team '") + kv.first + (now == "missing" ? "' was removed" : "' has changed since the championship started");
    }
    return out;
}

Lineup gcLineup(const GcEvent& ev) {
    Lineup L;
    L.name = "General Championship";
    for (size_t i = 0; i < ev.teams.size(); ++i) {
        const GcTeam& t = ev.teams[i];
        LineupTeam lt;
        lt.name = t.name;
        lt.livery = (int)(2 * i);
        lt.stats = t.stats;
        for (int s = 0; s < 2; ++s) {
            const GcDriver& d = t.drivers[s];
            lt.drivers.push_back({d.name, d.robot, "", 0, (int)(2 * i + s)});
        }
        L.teams.push_back(lt);
    }
    return L;
}

std::string gcCheckZip(const std::string& path) {
    std::string z;
    if (!readAll(path, z)) return "cannot read the file";
    auto u16 = [&](size_t at) { return at + 2 <= z.size() ? (unsigned)(unsigned char)z[at] | (unsigned)(unsigned char)z[at + 1] << 8 : 0u; };
    auto u32 = [&](size_t at) { return u16(at) | u16(at + 2) << 16; };
    // the end-of-central-directory record is in the last 64 KB
    size_t eocd = std::string::npos;
    for (size_t i = z.size() >= 22 ? z.size() - 22 : 0, lim = z.size() > 70000 ? z.size() - 70000 : 0;; --i) {
        if (z.compare(i, 4, "PK\x05\x06") == 0) { eocd = i; break; }
        if (i == lim || i == 0) break;
    }
    if (eocd == std::string::npos) return "not a zip file";
    const unsigned n = u16(eocd + 10);
    size_t at = u32(eocd + 16);
    if (n == 0xFFFF || u32(eocd + 16) == 0xFFFFFFFFu) return "zip64 archives are not supported";
    if (n > 2000) return "more than 2000 files in the zip";
    uint64_t total = 0;
    for (unsigned k = 0; k < n; ++k) {
        if (at + 46 > z.size() || z.compare(at, 4, "PK\x01\x02") != 0) return "damaged zip directory";
        const unsigned nameLen = u16(at + 28), extraLen = u16(at + 30), commentLen = u16(at + 32);
        const unsigned mode = u32(at + 38) >> 16;
        total += u32(at + 24);
        if (at + 46 + nameLen > z.size()) return "damaged zip directory";
        const std::string name = z.substr(at + 46, nameLen);
        if (name.empty() || name[0] == '/' || name[0] == '\\' || name.find(':') != std::string::npos || name.find('\\') != std::string::npos ||
            name.find('\0') != std::string::npos)
            return "'" + name + "': absolute, drive or backslash path";
        std::string part;
        for (size_t i = 0; i <= name.size(); ++i) {
            if (i == name.size() || name[i] == '/') {
                if (part == "..") return "'" + name + "': climbs out of the folder";
                part.clear();
            } else part += name[i];
        }
        if ((mode & 0170000) == 0120000) return "'" + name + "': a symbolic link";
        at += 46 + nameLen + extraLen + commentLen;
    }
    if (total > (uint64_t)200 << 20) return "unpacks to more than 200 MB";
    return "";
}

std::string gcImportFolder(const std::string& root, const std::string& extracted, const std::string& id) {
    std::error_code ec;
    fs::path src = extracted;
    if (!fs::exists(src / "team.json", ec)) {
        // a zip made by zipping the folder itself holds one folder
        std::vector<fs::path> sub;
        for (const auto& e : fs::directory_iterator(src, ec))
            if (e.is_directory(ec) && e.path().filename().string()[0] != '_' && e.path().filename() != "__MACOSX") sub.push_back(e.path());
        if (sub.size() == 1 && fs::exists(sub[0] / "team.json", ec)) src = sub[0];
        else return "team.json not found in the zip";
    }
    if (!safeName(id)) return "the zip name '" + id + "' may only use letters, digits, _ and -; rename the zip";
    const fs::path dst = fs::path(root) / "teams" / id;
    if (fs::exists(dst, ec)) return "GC/teams/" + id + " already exists (delete that folder to replace the team)";
    std::vector<std::string> files;
    std::string why;
    if (!listFiles(src, files, &why)) return why;
    fs::create_directories(dst, ec);
    for (const std::string& f : files) {
        fs::create_directories((dst / f).parent_path(), ec);
        fs::copy_file(src / f, dst / f, ec);
        if (ec) return "cannot copy " + f + ": " + ec.message();
    }
    return "";
}

}  // namespace rr
