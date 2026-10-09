#include "spec.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "mini_json.hpp"

namespace fs = std::filesystem;

namespace rr {

namespace {

bool readJson(const std::string& path, mjson::Value& v, std::string* err) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        if (err) *err = "cannot read " + path;
        return false;
    }
    std::stringstream ss;
    ss << f.rdbuf();
    v = mjson::parse(ss.str());
    if (v.type != mjson::Value::Object) {
        if (err) *err = path + ": not a JSON object";
        return false;
    }
    return true;
}

}  // namespace

std::string findDataFile(const std::string& name, const std::vector<std::string>& dirs) {
    std::error_code ec;
    if (fs::exists(name, ec) && !fs::is_directory(name, ec)) return name;
    for (const auto& d : dirs)
        for (const auto& cand : {name, name + ".json"}) {
            fs::path p = fs::path(d) / cand;
            if (fs::exists(p, ec) && !fs::is_directory(p, ec)) return p.string();
        }
    return "";
}

bool loadCarSpec(const std::string& path, CarParams& out, std::string* err) {
    mjson::Value v;
    if (!readJson(path, v, err)) return false;
    CarParams p;
    for (const auto& kv : v["params"].obj) {
        float* f = p.field(kv.first);
        if (!f || kv.second.type != mjson::Value::Number) {
            if (err) *err = path + ": unknown or non-numeric parameter '" + kv.first + "'";
            return false;
        }
        *f = (float)kv.second.n;
    }
    const auto& gears = v["gear_ratios"];
    if (gears.type == mjson::Value::Array) {
        if (gears.size() < 1 || gears.size() > RR_MAX_GEARS) {
            if (err) *err = path + ": gear_ratios needs 1 to " + std::to_string(RR_MAX_GEARS) + " entries";
            return false;
        }
        p.numGears = (int)gears.size();
        for (int i = 0; i < RR_MAX_GEARS; ++i) p.gearRatios[i] = i < p.numGears ? (float)gears[i].num() : 0.0f;
    }
    out = p;
    return true;
}

bool loadDevRules(const std::string& path, DevRules& out, std::string* err) {
    mjson::Value v;
    if (!readJson(path, v, err)) return false;
    DevRules r;
    r.budget = (int)v["budget"].num(0);
    r.minPoints = (int)v["min"].num(0);
    r.maxPoints = (int)v["max"].num(10);
    r.neutral = (int)v["neutral"].num(5);
    CarParams probe;
    for (const auto& c : v["categories"].arr) {
        DevCategory cat;
        cat.key = c["key"].str();
        cat.label = c["label"].str().empty() ? cat.key : c["label"].str();
        cat.about = c["about"].str();
        for (const auto& kv : c["effects"].obj) {
            if (!probe.field(kv.first)) {
                if (err) *err = path + ": category '" + cat.key + "' changes unknown parameter '" + kv.first + "'";
                return false;
            }
            DevEffect e;
            e.field = kv.first;
            if (kv.second.type == mjson::Value::Object) {
                e.perPoint = (float)kv.second["deficit"].num(0);
                e.deficit = true;
            } else {
                e.perPoint = (float)kv.second.num(0);
            }
            cat.effects.push_back(e);
        }
        if (cat.key.empty()) {
            if (err) *err = path + ": a category has no key";
            return false;
        }
        r.categories.push_back(cat);
    }
    for (const auto& k : v["retired"].arr) r.retired.push_back(k.str());
    out = r;
    return true;
}

bool parseDevelopment(const DevRules& rules, const std::string& dev, std::vector<int>& tokens, std::string* err) {
    tokens.assign(rules.categories.size(), rules.neutral);
    std::stringstream ss(dev);
    std::string item;
    int spent = 0;
    while (std::getline(ss, item, ',')) {
        if (item.empty()) continue;
        size_t eq = item.find('=');
        std::string key = item.substr(0, eq);
        int n = eq == std::string::npos ? 0 : std::atoi(item.c_str() + eq + 1);
        size_t k = 0;
        while (k < rules.categories.size() && rules.categories[k].key != key) ++k;
        if (k == rules.categories.size() && std::find(rules.retired.begin(), rules.retired.end(), key) != rules.retired.end())
            continue;
        if (k == rules.categories.size()) {
            if (err) *err = "unknown development category '" + key + "'";
            return false;
        }
        if (n < rules.minPoints || n > rules.maxPoints) {
            if (err) *err = "development '" + key + "' must be between " + std::to_string(rules.minPoints) + " and " +
                            std::to_string(rules.maxPoints);
            return false;
        }
        tokens[k] = n;
    }
    for (int t : tokens) spent += t;
    if (spent > rules.budget) {
        if (err) *err = "development spends " + std::to_string(spent) + " points, the budget is " + std::to_string(rules.budget);
        return false;
    }
    return true;
}

void applyDevelopment(const DevRules& rules, const std::vector<int>& tokens, CarParams& p) {
    for (size_t k = 0; k < rules.categories.size() && k < tokens.size(); ++k)
        for (const auto& e : rules.categories[k].effects)
            if (float* f = p.field(e.field)) {
                const float d = (float)(tokens[k] - rules.neutral);
                if (e.deficit)
                    *f = std::min(1.0f, 1.0f - (1.0f - *f) * std::max(0.0f, 1.0f + e.perPoint * d));
                else
                    *f *= std::max(0.05f, 1.0f + e.perPoint * d);
            }
}

}  // namespace rr
