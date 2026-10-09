#pragma once
// The General Championship (GC) edition: teams arrive as folders in GC/teams/<team id>/ and
// each one adds a team and its two bots as two drivers, nothing wired by hand.
//
//   GC/teams/<id>/team.json        the manifest (see GC/README.md)
//   GC/teams/<id>/livery_1.png ... 2048 x 2048 paint sheets of the team's car model
//   GC/teams/<id>/bots/<name>/*.c  bot source, compiled on the hosting PC into GC/bots/<id>__<name>.so
//
// gcLoad checks every team and keeps the good ones; the rest are reported in `problems`.
// A championship stores gcLock(): a digest of every team's files, checked before each round.
#include <string>
#include <utility>
#include <vector>

#include "championship.hpp"
#include "spec.hpp"

namespace rr {

struct GcDriver {
    std::string name;      // "EXA 1": the team's short name and the seat
    int number = 0;        // race number, 1..99
    std::string livery;    // full path of the 2048 x 2048 PNG
    std::string botDir;    // the bot folder, relative to the team folder
    std::string botName;   // that path as a plain name ("bots/alpha" = "bots_alpha")
    std::string robot;     // full path of the built bot library ("" until built)
};

struct GcTeam {
    std::string id;        // folder name
    std::string name;      // "Mechanical Engineering"
    std::string shortName; // "MEC"
    std::string model;     // car model id, e.g. "ferrari"
    int modelNo = 2;       // 1..11, the game's car slot (assets/cars/f1_2013_NN)
    std::string stats;     // "key=n,..." ("" = all neutral)
    std::string dir;
    GcDriver drivers[2];
};

struct GcEvent {
    std::string root;                  // the GC folder
    std::vector<GcTeam> teams;         // valid teams, sorted by short name
    std::vector<std::string> problems; // one line per rejected team or rule broken
    bool ok() const { return problems.empty(); }
};

// Car model ids a manifest may name (sauber, mercedes, redbull ...) and their game slot 1..11.
const std::vector<std::pair<std::string, int>>& gcModels();
int gcModelNumber(const std::string& id);  // 0 = unknown

constexpr int kGcMaxTeams = 10;            // 20 cars, the size of the grid

// Library name of a team's bot: "<team id>__<bot folder>", without the extension.
std::string gcBotLib(const std::string& teamId, const std::string& botName);

// Reads GC/teams/*. `rules` checks each team's stats (the stat budget). `requireBuilt`: a bot with
// no library in GC/bots is a problem (off for checking sources before the first build).
bool gcLoad(const std::string& root, const DevRules& rules, bool requireBuilt, GcEvent& out);

// Digest of everything that makes up a team: manifest, sources, liveries and built libraries.
std::string gcDigest(const std::string& root, const std::string& teamId);
std::vector<std::pair<std::string, std::string>> gcLock(const GcEvent& ev);
// "" if the championship's teams are exactly as locked, else what changed.
std::string gcVerify(const std::string& root, const Championship& c);

// The event as a lineup: team i has its two drivers; livery slot = 2 * team + seat.
Lineup gcLineup(const GcEvent& ev);

// Reads a zip's directory without unpacking it: "" if it is safe to unpack (no path leaving the folder,
// no links, no absolute or drive paths, a sane size), else the reason.
std::string gcCheckZip(const std::string& path);

// Copies one extracted submission (a folder holding team.json, here or one level down) into
// GC/teams/<id>; the id comes from the folder or zip name. Returns "" or the reason.
std::string gcImportFolder(const std::string& root, const std::string& extracted, const std::string& id);

}  // namespace rr
