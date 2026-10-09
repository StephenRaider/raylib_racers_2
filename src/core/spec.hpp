#pragma once
// Car specifications as data, and the development (token) system on top of them.
//
//  - A spec file (specs/<name>.json) sets any CarParams field by name; fields it
//    leaves out keep the built-in defaults (the 2004-2010 F1 car).
//  - The development rules (specs/development.json) define stats such as top
//    speed or pit stops, rated 0-10 with 5 the stock car, what one point does
//    to the car (linear), and how many points a team may spend in all. A car's
//    development is a string like "top_speed=8,downforce=3,pit_stop=6"; stats
//    left out stay at 5.
#include <string>
#include <vector>

#include "car.hpp"

namespace rr {

struct DevEffect {
    std::string field;   // CarParams field
    float perPoint = 0;  // relative change per point from neutral: value *= 1 + perPoint * (points - neutral)
    bool deficit = false;  // for scales below 1 (a DRS cut): the deficit 1 - value grows instead: value = 1 - (1 - value) * (1 + perPoint * d)
};

struct DevCategory {
    std::string key;     // e.g. "top_speed"
    std::string label;   // e.g. "Top speed"
    std::string about;
    std::vector<DevEffect> effects;
};

struct DevRules {
    int budget = 0;      // total points a team may spend over all stats
    int minPoints = 0, maxPoints = 10;  // per stat
    int neutral = 5;     // the stock car
    std::vector<DevCategory> categories;
    std::vector<std::string> retired;  // stat keys that no longer exist; parseDevelopment ignores them (old lineups)
    bool empty() const { return categories.empty(); }
};

// Searches dirs for name, name + ".json"; returns "" if not found. A path that exists is returned as is.
std::string findDataFile(const std::string& name, const std::vector<std::string>& dirs);

bool loadCarSpec(const std::string& path, CarParams& out, std::string* err);
bool loadDevRules(const std::string& path, DevRules& out, std::string* err);

// Parses "key=n,key=n" against the rules: unknown keys, points outside
// [minPoints, maxPoints] or a total over the budget are errors. Stats left out
// are neutral.
bool parseDevelopment(const DevRules& rules, const std::string& dev, std::vector<int>& tokens, std::string* err);
void applyDevelopment(const DevRules& rules, const std::vector<int>& tokens, CarParams& p);

}  // namespace rr
