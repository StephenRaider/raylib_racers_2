#include "liveries.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>

#include "mini_json.hpp"

namespace {

std::vector<CarLivery> gTable;
std::vector<int> gCarSlot;

// HUD colours for the known schemes; dark liveries use their accent so they read on the HUD.
Color schemeColor(const std::string& key) {
    struct K { const char* key; Color c; };
    static const K known[] = {
        {"rosso", {220, 35, 35, 255}},        {"papaya", {255, 128, 0, 255}},
        {"silver_teal", {0, 205, 185, 255}},  {"midnight", {60, 80, 175, 255}},
        {"racing_green", {0, 140, 95, 255}},  {"blue_pink", {255, 90, 170, 255}},
        {"white_navy", {235, 235, 240, 255}}, {"black_gold", {205, 165, 70, 255}},
        {"sunburst", {250, 205, 0, 255}},     {"steel_red", {150, 158, 170, 255}},
    };
    for (const K& k : known)
        if (key == k.key) return k.c;
    // unknown scheme: a stable colour from the name
    unsigned h = 2166136261u;
    for (char ch : key) h = (h ^ (unsigned char)ch) * 16777619u;
    return ColorFromHSV((float)(h % 360), 0.7f, 0.95f);
}

std::string readFile(const std::string& path) {
    std::string s;
    if (FILE* f = std::fopen(path.c_str(), "rb")) {
        char buf[4096];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) s.append(buf, n);
        std::fclose(f);
    }
    return s;
}

}  // namespace

std::vector<CarLivery> loadLiveries(const std::string& assetsDir) {
    namespace fs = std::filesystem;
    const std::string dir = assetsDir + "/cars/f1_gearari";
    std::vector<CarLivery> out;
    const mjson::Value teams = mjson::parse(readFile(dir + "/teams.json"))["teams"];
    // Car 1 of every team first, then car 2: the default grid of N cars spreads over the teams.
    for (int seat = 0; seat < 2; ++seat)
        for (size_t t = 0; t < teams.size(); ++t) {
            const mjson::Value& team = teams[t];
            const mjson::Value& car = team["cars"][seat];
            if (car.type != mjson::Value::Object) continue;
            CarLivery l;
            l.team = team["team"].str();
            l.key = fs::path(team["livery"].str()).stem().string();
            l.file = dir + "/" + car["livery"].str();
            l.number = (int)car["number"].num();
            l.color = schemeColor(l.key);
            if (fs::exists(l.file)) out.push_back(l);
        }
    if (!out.empty()) return out;

    std::vector<fs::path> files;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir + "/liveries", ec))
        if (e.path().extension() == ".png") files.push_back(e.path());
    std::sort(files.begin(), files.end());
    for (const auto& f : files) {
        CarLivery l;
        l.key = l.team = f.stem().string();
        l.file = f.string();
        l.number = (int)out.size() + 1;
        l.color = schemeColor(l.key);
        out.push_back(l);
    }
    return out;
}

void setLiveryTable(const std::vector<CarLivery>& table) { gTable = table; }
const std::vector<CarLivery>& liveryTable() { return gTable; }
void setCarLiveries(const std::vector<int>& slotOfCar) { gCarSlot = slotOfCar; }

int carLivery(int carIndex) {
    const int n = std::max<int>(1, (int)gTable.size());
    if (carIndex >= 0 && carIndex < (int)gCarSlot.size()) return ((gCarSlot[carIndex] % n) + n) % n;
    return ((carIndex % n) + n) % n;
}

namespace {
struct Preset { const char* name; Color c; };
const Preset kPresets[] = {
    {"red", {200, 25, 30, 255}},     {"blue", {25, 70, 190, 255}},     {"orange", {240, 120, 15, 255}},
    {"green", {25, 140, 60, 255}},   {"yellow", {245, 205, 20, 255}},  {"purple", {110, 45, 160, 255}},
    {"teal", {0, 150, 140, 255}},    {"white", {240, 240, 240, 255}},  {"navy", {15, 25, 70, 255}},
    {"black", {22, 22, 24, 255}},
};
}  // namespace

int presetCount() { return (int)(sizeof kPresets / sizeof kPresets[0]); }
Color presetColor(int i) { return kPresets[((i % presetCount()) + presetCount()) % presetCount()].c; }
const char* presetName(int i) { return kPresets[((i % presetCount()) + presetCount()) % presetCount()].name; }

void neutralTeams(std::vector<CarLivery>& table) {
    std::vector<std::string> seen;
    for (CarLivery& l : table) {
        int k = (int)(std::find(seen.begin(), seen.end(), l.team) - seen.begin());
        if (k == (int)seen.size()) seen.push_back(l.team);
        l.team = "Team " + std::to_string(k + 1);
        l.key = "team" + std::to_string(k + 1);
        l.color = presetColor(k);
    }
}

void setSlotColor(int slot, Color c) {
    if (slot >= 0 && slot < (int)gTable.size()) gTable[slot].color = c;
}
