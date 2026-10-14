#include "short_names.hpp"

#include <cctype>
#include <cstdio>
#include <map>

#include "mini_json.hpp"

namespace {

std::map<std::string, std::string> gCodes;

std::string withoutNumber(const std::string& name) {
    size_t i = 0;
    while (i < name.size() && std::isdigit((unsigned char)name[i])) ++i;
    if (i == 0 || i >= name.size() || name[i] != ' ') return name;
    return name.substr(i + 1);
}

}  // namespace

void loadShortNames(const std::string& assetsDir) {
    gCodes.clear();
    std::string text;
    if (FILE* f = std::fopen((assetsDir + "/short_names.json").c_str(), "rb")) {
        char buf[4096];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n);
        std::fclose(f);
    }
    const mjson::Value names = mjson::parse(text)["names"];
    for (const auto& [key, v] : names.obj)
        if (!v.str().empty()) gCodes[key] = v.str();
}

std::string shortName(const std::string& name, const std::string& robot) {
    const std::string bare = withoutNumber(name);
    for (const std::string* k : {&bare, &name, &robot}) {
        auto it = gCodes.find(*k);
        if (it != gCodes.end()) return it->second;
    }
    // the last word's first three letters, upper case
    const size_t end = bare.find_last_not_of(' ');
    if (end == std::string::npos) return "---";
    const size_t space = bare.find_last_of(' ', end);
    std::string code;
    for (size_t i = space == std::string::npos ? 0 : space + 1; i <= end && code.size() < 3; ++i)
        if (std::isalnum((unsigned char)bare[i])) code += (char)std::toupper((unsigned char)bare[i]);
    return code.empty() ? "---" : code;
}

int carNumber(const std::string& name) {
    int n = 0;
    size_t i = 0;
    while (i < name.size() && std::isdigit((unsigned char)name[i])) n = n * 10 + (name[i++] - '0');
    return i > 0 && i < name.size() && name[i] == ' ' ? n : 0;
}
