#include "menu.hpp"

#include <cstdlib>
#include <sstream>

#include "car.hpp"
#include "liveries.hpp"
#include "raylib.h"

constexpr int MenuState::kTyreLives[];
constexpr float MenuState::kDistances[];
constexpr float MenuState::kWears[];

void MenuState::setWearRate(float rate) {
    if (rate <= 0) {
        tyreLife = kNumTyreLives - 1;
        return;
    }
    const float want = baseTyreLife() / rate;
    int best = 0;
    for (int i = 0; i + 1 < kNumTyreLives; ++i)
        if (std::fabs(std::log(kTyreLives[i] / want)) < std::fabs(std::log(kTyreLives[best] / want))) best = i;
    tyreLife = best;
}

std::vector<MenuState::Row> MenuState::rows() const {
    std::vector<Row> all = allRows();
    if (!rr2) return all;
    std::vector<Row> out;
    for (Row x : all) {
        if (x == Row::TyreLife || x == Row::TestLivery || x == Row::ChampWear || x == Row::ChampDistance)
            continue;
        if (x == Row::Laps && weekend()) continue;
        out.push_back(x);
    }
    return out;
}

std::vector<MenuState::Row> MenuState::allRows() const {
    if (testing())
        return {Row::Session, Row::Track, Row::Laps, Row::TyreLife, Row::TestCar, Row::TestLivery, Row::TestTyres,
                Row::TestFuel, Row::TestStats, Row::TestRuns, Row::Start};
    if (champ()) {
        std::vector<Row> r = {Row::Session};
        for (size_t i = 0; i < calendar.size(); ++i) r.push_back(Row::Round);
        for (Row x : {Row::AddRound, Row::ChampName, Row::ChampDistance, Row::ChampWear, Row::TyreRule, Row::ChampQuali, Row::Practice,
                      Row::Teams, Row::Drivers, Row::Grid, Row::Stats, Row::SaveLineup, Row::LoadLineup})
            r.push_back(x);
        for (size_t i = 0; i < seasons.size(); ++i) r.push_back(Row::Season);
        r.push_back(Row::Start);
        return r;
    }
    if (weekend())
        return {Row::Session, Row::Track, Row::Laps, Row::TyreLife, Row::TyreRule, Row::Practice, Row::Teams, Row::Drivers,
                Row::Grid, Row::Stats, Row::SaveLineup, Row::LoadLineup, Row::Start};
    return {Row::Session, Row::Track, Row::Laps, Row::TyreLife, Row::TyreRule, Row::Teams, Row::Drivers, Row::Grid,
            Row::Stats, Row::SaveLineup, Row::LoadLineup, Row::Start};
}

int MenuState::rowIndex(int row) const {
    const std::vector<Row> rs = rows();
    if (row < 0 || row >= (int)rs.size()) return -1;
    int k = 0;
    for (int i = 0; i < row; ++i) k += rs[i] == rs[row];
    return k;
}

const TrackStats* MenuState::trackStats(const std::string& file) const {
    for (const TrackStats& t : tracks)
        if (t.file == file) return &t;
    return nullptr;
}

int MenuState::roundLaps(const rr::ChampRound& r) const {
    const TrackStats* t = trackStats(r.track);
    return t && t->length > 0 ? rr::Championship::lapsFor(champDistance(), t->length) : r.laps;
}

float MenuState::compoundLaps(const std::string& file, int compound, float rate) const {
    const TrackStats* t = trackStats(file);
    if (!t || rate <= 0) return 0;
    return 0.7f / std::max(1e-5f, t->wearPerLap * rate * rr::compoundWear(compound));
}

void MenuState::resetCalendar() {
    calendar.clear();
    for (const rr::ChampRound& r : rr::Championship::defaultCalendar(10))
        if (trackStats(r.track)) calendar.push_back(r);
}

std::vector<std::string> MenuState::popupOptions() const {
    std::vector<std::string> o;
    const auto& table = liveryTable();
    auto liveries = [&]() {
        for (int i = 0; i < std::max(1, liveryCount); ++i)
            o.push_back(i < (int)table.size() ? "#" + std::to_string(table[i].number) + "  " + table[i].team
                                              : "livery " + std::to_string(i + 1));
    };
    if (popup >= 100) {
        const int col = (popup - 100) % kGridCols;
        if (col == 0) liveries();
        else if (col == 1) for (const Algorithm& a : algos) o.push_back(a.label);
        else o = {"Auto (the algorithm picks)", "Soft", "Medium", "Hard"};
        return o;
    }
    const std::vector<Row> rs = rows();
    if (popup < 0 || popup >= (int)rs.size()) return o;
    switch (rs[popup]) {
        case Row::Track:
        case Row::AddRound:
            for (const TrackStats& t : tracks) o.push_back(t.title);
            break;
        case Row::TestCar: for (const Algorithm& a : algos) o.push_back(a.label); break;
        case Row::TestLivery: liveries(); break;
        default: break;
    }
    return o;
}

void MenuState::openPopup(int target, float x, float y, float w) {
    popup = target;
    popupX = x, popupY = y, popupW = w;
    popupSel = 0;
    if (target >= 100) {
        const int car = (target - 100) / kGridCols, col = (target - 100) % kGridCols;
        if (car < (int)carLivery.size())
            popupSel = col == 0 ? carLivery[car] : col == 1 ? carAlgo[car] : car < (int)carTires.size() ? carTires[car] : 0;
    } else {
        const std::vector<Row> rs = rows();
        if (target < (int)rs.size()) {
            const Row r = rs[target];
            popupSel = r == Row::Track ? track : r == Row::AddRound ? addTrack : r == Row::TestCar ? testAlgo
                     : r == Row::TestLivery ? testLivery : 0;
        }
    }
    popupTop = std::max(0, popupSel - 6);
}

int MenuState::rowOf(Row r) const {
    const std::vector<Row> rs = rows();
    for (int i = 0; i < (int)rs.size(); ++i)
        if (rs[i] == r) return i;
    return -1;
}

std::vector<int> StatRules::parse(const std::string& dev) const {
    std::vector<int> v(keys.size(), neutral);
    std::stringstream ss(dev);
    std::string item;
    while (std::getline(ss, item, ',')) {
        const size_t eq = item.find('=');
        if (eq == std::string::npos) continue;
        for (size_t k = 0; k < keys.size(); ++k)
            if (keys[k] == item.substr(0, eq)) v[k] = std::clamp(std::atoi(item.c_str() + eq + 1), min, max);
    }
    return v;
}

std::string StatRules::format(const std::vector<int>& v) const {
    std::string out;
    for (size_t k = 0; k < keys.size() && k < v.size(); ++k) {
        if (v[k] == neutral) continue;
        if (!out.empty()) out += ",";
        out += keys[k] + "=" + std::to_string(v[k]);
    }
    return out;
}

std::vector<int> MenuState::raceTeams() const {
    std::vector<int> out;
    for (int car = 0; car < cars; ++car) {
        const int t = teamOfCar(car);
        if (t >= 0 && std::find(out.begin(), out.end(), t) == out.end()) out.push_back(t);
    }
    return out;
}

void MenuState::layoutGrid() {
    if (teamSlots.empty()) return;
    teams = std::clamp(teams, 1, (int)teamSlots.size());
    drivers = std::clamp(drivers, 1, 2);
    cars = std::min(maxCars, teams * drivers);
    std::vector<int> slots;
    for (int d = 0; d < drivers; ++d)
        for (int t = 0; t < teams; ++t) slots.push_back(teamSlots[t][std::min<size_t>(d, teamSlots[t].size() - 1)]);
    // the slots nobody races keep their places after the grid (for livery swaps)
    for (int slot = 0; slot < liveryCount; ++slot)
        if (std::find(slots.begin(), slots.end(), slot) == slots.end()) slots.push_back(slot);
    slots.resize(std::max<size_t>(slots.size(), carLivery.size()), 0);
    for (size_t i = 0; i < carLivery.size(); ++i) carLivery[i] = slots[i];
    gridRow = std::min(gridRow, cars - 1);
}

void MenuState::styleChanged(int car) {
    const int t = teamOfCar(car);
    if (t < 0 || t >= (int)teamStats.size() || car >= (int)carAlgo.size()) return;
    const int a = carAlgo[car];
    if (a >= 0 && a < (int)algos.size()) teamStats[t] = statRules.parse(algos[a].stats);
}

void MenuState::algoChanged(int car) {
    const int t = teamOfCar(car);
    if (t < 0) return;
    if ((int)styleCar.size() <= t) styleCar.resize(t + 1, -1);
    styleCar[t] = car;
}

int MenuState::stylesPending() const {
    int n = 0;
    for (int c : styleCar) n += c >= 0;
    return n;
}

void MenuState::applyStyles() {
    for (int t = 0; t < (int)styleCar.size(); ++t) {
        const int car = styleCar[t];
        // the car may have moved to another team since (a livery swap)
        if (car >= 0 && teamOfCar(car) == t) styleChanged(car);
        styleCar[t] = -1;
    }
}

namespace {

// Picks a livery for a car; a livery another car wears is swapped with it.
void setLivery(MenuState& m, int car, int slot) {
    const int n = std::max(1, m.liveryCount);
    slot = ((slot % n) + n) % n;
    for (int i = 0; i < (int)m.carLivery.size(); ++i)
        if (i != car && m.carLivery[i] == slot) m.carLivery[i] = m.carLivery[car];
    m.carLivery[car] = slot;
}

void changeGrid(MenuState& m, int car, int col, int dir) {
    if (car < 0 || car >= (int)m.carLivery.size()) return;
    if (col == 0) {
        setLivery(m, car, m.carLivery[car] + dir);
    } else if (col == 1) {
        const int n = (int)m.algos.size();
        m.carAlgo[car] = ((m.carAlgo[car] + dir) % n + n) % n;
        m.algoChanged(car);
    } else if (car < (int)m.carTires.size()) {
        m.carTires[car] = ((m.carTires[car] + dir) % 4 + 4) % 4;  // auto, soft, medium, hard
    }
}

MenuAction updateGrid(MenuState& m, const std::vector<MenuHit>& hits) {
    const int n = m.cars;
    auto rep = [](int key) { return IsKeyPressed(key) || IsKeyPressedRepeat(key); };
    if (rep(KEY_UP)) m.gridRow = (m.gridRow + n - 1) % n;
    if (rep(KEY_DOWN)) m.gridRow = (m.gridRow + 1) % n;
    if (IsKeyPressed(KEY_TAB)) m.gridCol = (m.gridCol + 1) % MenuState::kGridCols;
    if (m.rr2 && m.gridCol == 0) m.gridCol = 1;  // no liveries to pick
    if (rep(KEY_LEFT)) changeGrid(m, m.gridRow, m.gridCol, -1);
    if (rep(KEY_RIGHT)) changeGrid(m, m.gridRow, m.gridCol, 1);
    if (IsKeyPressed(KEY_A)) m.applyStyles();
    if (IsKeyPressed(KEY_SPACE))  // the selected cell's list
        for (const MenuHit& h : hits)
            if (h.row == 100 + m.gridRow * MenuState::kGridCols + m.gridCol && h.dir == 2) {
                m.openPopup(h.row, h.x, h.y + h.h + 4, std::max(h.w, 280.0f));
                return MenuAction::None;
            }
    if (IsKeyPressed(KEY_ESCAPE) || IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER) || IsKeyPressed(KEY_BACKSPACE))
        m.gridPage = false;
    const Vector2 mp = GetMousePosition();
    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
        for (const MenuHit& h : hits) {
            if (!CheckCollisionPointRec(mp, {h.x, h.y, h.w, h.h})) continue;
            if (h.row == 99) { m.gridPage = false; break; }  // Done button
            if (h.row == 94) { m.applyStyles(); break; }     // style button
            if (h.row < 100 || h.row >= 1000) continue;
            const int car = (h.row - 100) / MenuState::kGridCols, col = (h.row - 100) % MenuState::kGridCols;
            m.gridRow = car;
            m.gridCol = col;
            if (h.dir == 2) m.openPopup(h.row, h.x, h.y + h.h + 4, std::max(h.w, 280.0f));
            else if (h.dir) changeGrid(m, car, col, h.dir);
            break;
        }
    return MenuAction::None;
}

void changeStat(MenuState& m, int team, int stat, int dir) {
    if (team < 0 || team >= (int)m.teamStats.size() || stat < 0 || stat >= (int)m.teamStats[team].size()) return;
    int& v = m.teamStats[team][stat];
    const StatRules& r = m.statRules;
    if (dir > 0 && v < r.max && m.statSum(team) < r.budget) ++v;
    if (dir < 0 && v > r.min) --v;
}

MenuAction updateTeams(MenuState& m, const std::vector<MenuHit>& hits) {
    const std::vector<int> teams = m.raceTeams();
    const int n = std::max(1, (int)teams.size()), ns = std::max(1, (int)m.statRules.keys.size());
    auto rep = [](int key) { return IsKeyPressed(key) || IsKeyPressedRepeat(key); };
    m.teamRow = std::min(m.teamRow, n - 1);
    if (rep(KEY_UP)) m.teamRow = (m.teamRow + n - 1) % n;
    if (rep(KEY_DOWN)) m.teamRow = (m.teamRow + 1) % n;
    const bool shift = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
    if (IsKeyPressed(KEY_TAB)) m.statCol = (m.statCol + (shift ? ns - 1 : 1)) % ns;
    const int team = m.teamRow < (int)teams.size() ? teams[m.teamRow] : -1;
    if (rep(KEY_LEFT)) changeStat(m, team, m.statCol, -1);
    if (rep(KEY_RIGHT)) changeStat(m, team, m.statCol, 1);
    if (IsKeyPressed(KEY_ESCAPE) || IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER) || IsKeyPressed(KEY_BACKSPACE))
        m.teamsPage = false;
    const Vector2 mp = GetMousePosition();
    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
        for (const MenuHit& h : hits) {
            if (!CheckCollisionPointRec(mp, {h.x, h.y, h.w, h.h})) continue;
            if (h.row == 99) { m.teamsPage = false; break; }
            const int t = (h.row - 1000) / 16, stat = (h.row - 1000) % 16;
            for (int i = 0; i < (int)teams.size(); ++i)
                if (teams[i] == t) m.teamRow = i;
            m.statCol = stat;
            if (h.dir) changeStat(m, t, stat, h.dir);
        }
    return MenuAction::None;
}

void changeTestStat(MenuState& m, int stat, int dir) {
    if (stat < 0 || stat >= (int)m.testStats.size()) return;
    int& v = m.testStats[stat];
    const StatRules& r = m.statRules;
    int sum = 0;
    for (int x : m.testStats) sum += x;
    if (dir > 0 && v < r.max && sum < r.budget) ++v;
    if (dir < 0 && v > r.min) --v;
}

MenuAction updateTestStats(MenuState& m, const std::vector<MenuHit>& hits) {
    const int n = std::max(1, (int)m.testStats.size());
    auto rep = [](int key) { return IsKeyPressed(key) || IsKeyPressedRepeat(key); };
    if (rep(KEY_UP)) m.testStatRow = (m.testStatRow + n - 1) % n;
    if (rep(KEY_DOWN)) m.testStatRow = (m.testStatRow + 1) % n;
    if (rep(KEY_LEFT)) changeTestStat(m, m.testStatRow, -1);
    if (rep(KEY_RIGHT)) changeTestStat(m, m.testStatRow, 1);
    if (IsKeyPressed(KEY_D) && m.testAlgo < (int)m.algos.size()) m.testStats = m.statRules.parse(m.algos[m.testAlgo].stats);
    if (IsKeyPressed(KEY_ZERO)) m.testStats = m.statRules.parse("");
    if (IsKeyPressed(KEY_ESCAPE) || IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER) || IsKeyPressed(KEY_BACKSPACE))
        m.testStatsPage = false;
    const Vector2 mp = GetMousePosition();
    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
        for (const MenuHit& h : hits) {
            if (!CheckCollisionPointRec(mp, {h.x, h.y, h.w, h.h})) continue;
            if (h.row == 99) { m.testStatsPage = false; break; }
            if (h.row == 98 && m.testAlgo < (int)m.algos.size()) { m.testStats = m.statRules.parse(m.algos[m.testAlgo].stats); break; }
            if (h.row == 97) { m.testStats = m.statRules.parse(""); break; }
            if (h.row < 2000 || h.row >= 3000) continue;
            m.testStatRow = h.row - 2000;
            if (h.dir) changeTestStat(m, m.testStatRow, h.dir);
            break;
        }
    return MenuAction::None;
}

MenuAction updateRuns(MenuState& m, const std::vector<MenuHit>& hits) {
    const int n = (int)m.runLines.size();
    auto rep = [](int key) { return IsKeyPressed(key) || IsKeyPressedRepeat(key); };
    MenuAction act = MenuAction::None;
    if (n > 0) {
        if (rep(KEY_UP)) m.runsRow = std::max(0, m.runsRow - 1);
        if (rep(KEY_DOWN)) m.runsRow = std::min(n - 1, m.runsRow + 1);
        if (rep(KEY_PAGE_UP)) m.runsRow = std::max(0, m.runsRow - 10);
        if (rep(KEY_PAGE_DOWN)) m.runsRow = std::min(n - 1, m.runsRow + 10);
        if (IsKeyPressed(KEY_HOME)) m.runsRow = 0;
        if (IsKeyPressed(KEY_END)) m.runsRow = n - 1;
    }
    m.runsRow = std::clamp(m.runsRow, 0, std::max(0, n - 1));
    auto pick = [&](MenuAction a) {
        if (m.runsRow >= n) return;
        if (a == MenuAction::ViewRun && !m.runLines[m.runsRow].telemetry) return;
        m.runPick = m.runLines[m.runsRow].id;
        act = a;
    };
    if (IsKeyPressed(KEY_S)) { m.runsSort = 1 - m.runsSort; m.runsRow = 0; }
    if (IsKeyPressed(KEY_T)) { m.runsAllTracks = !m.runsAllTracks; m.runsRow = 0; }
    if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER)) pick(MenuAction::LoadRun);
    if (IsKeyPressed(KEY_V)) pick(MenuAction::ViewRun);
    if (IsKeyPressed(KEY_ESCAPE) || IsKeyPressed(KEY_BACKSPACE)) m.runsPage = false;
    const float wheel = GetMouseWheelMove();
    if (wheel != 0) m.runsTop = std::max(0, m.runsTop - (int)wheel * 3);
    const Vector2 mp = GetMousePosition();
    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
        for (const MenuHit& h : hits) {
            if (!CheckCollisionPointRec(mp, {h.x, h.y, h.w, h.h})) continue;
            if (h.row == 99) m.runsPage = false;
            else if (h.row == 98) pick(MenuAction::LoadRun);
            else if (h.row == 97) pick(MenuAction::ViewRun);
            else if (h.row == 96) { m.runsSort = 1 - m.runsSort; m.runsRow = 0; }
            else if (h.row == 95) { m.runsAllTracks = !m.runsAllTracks; m.runsRow = 0; }
            else if (h.row >= 3000) m.runsRow = h.row - 3000;
            break;
        }
    return act;
}

void change(MenuState& m, MenuState::Row row, int dir, bool big, MenuAction& act) {
    using Row = MenuState::Row;
    switch (row) {
        case Row::Track: {
            const int n = (int)m.tracks.size();
            m.track = (m.track + dir + n) % n;
            act = MenuAction::TrackChanged;
            break;
        }
        case Row::Laps: {
            const int step = big ? 10 : (m.laps >= 20 && dir > 0) || (m.laps > 20 && dir < 0) ? 5 : 1;
            m.laps = std::clamp(m.laps + dir * step, 1, 200);
            break;
        }
        case Row::TyreLife: m.tyreLife = std::clamp(m.tyreLife + dir, 0, MenuState::kNumTyreLives - 1); break;
        case Row::Teams:
            if (m.teamSlots.empty()) {
                m.cars = std::clamp(m.cars + dir, 1, m.maxCars);
            } else {
                m.teams = std::clamp(m.teams + dir, 1, (int)m.teamSlots.size());
                m.layoutGrid();
            }
            break;
        case Row::Drivers:
            if (!m.teamSlots.empty()) {
                m.drivers = 3 - m.drivers;
                m.layoutGrid();
            }
            break;
        case Row::Stats: m.teamsPage = true; break;
        case Row::Session: {
            static const int order[] = {0, 1, 3, 2};  // as the tabs show them
            int k = 0;
            while (k < 3 && order[k] != m.session) ++k;
            m.session = order[(k + dir + 4) % 4];
            m.row = m.rowOf(Row::Session);
            if (m.champ()) act = MenuAction::ChampTab;
            break;
        }
        case Row::AddRound: {
            const int n = std::max(1, (int)m.tracks.size());
            m.addTrack = ((m.addTrack + dir) % n + n) % n;
            break;
        }
        case Row::ChampDistance: m.champKm = std::clamp(m.champKm + dir, 0, MenuState::kNumDistances - 1); break;
        case Row::ChampWear: m.champWear = std::clamp(m.champWear + dir, 0, MenuState::kNumWears - 1); break;
        case Row::ChampQuali: m.champQuali = !m.champQuali; break;
        case Row::Practice: m.practice = std::clamp(m.practice + dir, 0, MenuState::kNumPractice - 1); break;
        case Row::TyreRule: m.tyreRule = (m.tyreRule + dir + 3) % 3; break;
        case Row::Grid: m.gridPage = true; break;
        case Row::TestCar: {
            const int n = std::max(1, (int)m.algos.size());
            m.testAlgo = ((m.testAlgo + dir) % n + n) % n;
            break;
        }
        case Row::TestLivery: {
            const int n = std::max(1, m.liveryCount);
            m.testLivery = ((m.testLivery + dir) % n + n) % n;
            break;
        }
        case Row::TestTyres: m.testTires = ((m.testTires + dir) % 4 + 4) % 4; break;
        case Row::TestFuel: {
            const float step = big ? 5.0f : 1.0f;
            float f = m.testFuel > 0 ? m.testFuel : std::round(m.autoFuel);
            f += dir * step;
            m.testFuel = f < 1.0f ? 0.0f : std::min(f, m.tankLitres);  // below 1 L: back to automatic
            break;
        }
        case Row::TestStats: m.testStatsPage = true; break;
        case Row::TestRuns: m.runsPage = true; break;
        default: break;
    }
}

void moveRound(MenuState& m, int k, int dir) {
    const int j = k + dir;
    if (k < 0 || j < 0 || k >= (int)m.calendar.size() || j >= (int)m.calendar.size()) return;
    std::swap(m.calendar[k], m.calendar[j]);
}

// Enter or a click on a row's value: open its page, or start.
void select(MenuState& m, MenuState::Row row, MenuAction& act) {
    using Row = MenuState::Row;
    switch (row) {
        case Row::SaveLineup:
            m.lineupSave = true;
            m.inputText.clear();
            break;
        case Row::LoadLineup: act = MenuAction::ListLineups; break;
        case Row::ChampName:
            m.lineupSave = false;
            m.inputText = m.champName;
            m.champNaming = true;
            break;
        case Row::AddRound:
            if (m.addTrack < (int)m.tracks.size()) m.calendar.push_back({m.tracks[m.addTrack].file, 10});
            break;
        case Row::ChampQuali: m.champQuali = !m.champQuali; break;
        case Row::Round: break;
        case Row::Start: act = m.champ() ? MenuAction::NewSeason : MenuAction::Start; break;
        case Row::Grid: m.gridPage = true; break;
        case Row::Stats: m.teamsPage = true; break;
        case Row::TestStats: m.testStatsPage = true; break;
        case Row::TestRuns: m.runsPage = true; break;
        case Row::TestFuel: m.testFuel = 0; break;  // back to automatic
        default: break;  // only the Start button starts the race
    }
}

}  // namespace

void MenuState::choose(int target, int option) {
    if (target >= 100) {
        const int car = (target - 100) / kGridCols, col = (target - 100) % kGridCols;
        if (car < 0 || car >= (int)carLivery.size()) return;
        if (col == 0) setLivery(*this, car, option);
        else if (col == 1 && option < (int)algos.size()) {
            carAlgo[car] = option;
            algoChanged(car);
        } else if (col == 2 && car < (int)carTires.size()) carTires[car] = option & 3;
        return;
    }
    const std::vector<Row> rs = rows();
    if (target < 0 || target >= (int)rs.size()) return;
    switch (rs[target]) {
        case Row::Track: track = std::clamp(option, 0, (int)tracks.size() - 1); break;
        case Row::AddRound: addTrack = std::clamp(option, 0, (int)tracks.size() - 1); break;
        case Row::TestCar: testAlgo = std::clamp(option, 0, (int)algos.size() - 1); break;
        case Row::TestLivery: testLivery = std::clamp(option, 0, std::max(0, liveryCount - 1)); break;
        default: break;
    }
}

namespace {

MenuAction updatePopup(MenuState& m, const std::vector<MenuHit>& hits) {
    const int n = (int)m.popupOptions().size();
    auto rep = [](int key) { return IsKeyPressed(key) || IsKeyPressedRepeat(key); };
    MenuAction act = MenuAction::None;
    auto pick = [&](int i) {
        const std::vector<MenuState::Row> rs = m.rows();
        const bool isTrack = m.popup >= 0 && m.popup < (int)rs.size() && rs[m.popup] == MenuState::Row::Track;
        const int before = m.track;
        m.choose(m.popup, i);
        if (isTrack && m.track != before) act = MenuAction::TrackChanged;
        m.popup = -1;
    };
    if (rep(KEY_UP)) m.popupSel = std::max(0, m.popupSel - 1);
    if (rep(KEY_DOWN)) m.popupSel = std::min(n - 1, m.popupSel + 1);
    const float wheel = GetMouseWheelMove();
    if (wheel != 0) m.popupTop = std::max(0, m.popupTop - (int)wheel * 2);
    if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER) || IsKeyPressed(KEY_SPACE)) pick(m.popupSel);
    else if (IsKeyPressed(KEY_ESCAPE) || IsKeyPressed(KEY_BACKSPACE)) m.popup = -1;
    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && m.popup >= 0) {
        const Vector2 mp = GetMousePosition();
        bool hit = false;
        for (const MenuHit& h : hits) {
            if (h.row < 5000 || h.row > 5999 || !CheckCollisionPointRec(mp, {h.x, h.y, h.w, h.h})) continue;
            hit = true;
            if (h.row < 5999) pick(h.row - 5000);
            break;
        }
        if (!hit) m.popup = -1;  // a click outside closes it
    }
    for (const MenuHit& h : hits)
        if (h.row >= 5000 && h.row < 5999 && CheckCollisionPointRec(GetMousePosition(), {h.x, h.y, h.w, h.h}) &&
            (GetMouseDelta().x != 0 || GetMouseDelta().y != 0))
            m.popupSel = h.row - 5000;
    return act;
}

MenuAction updateTyping(MenuState& m, const std::vector<MenuHit>& hits) {
    MenuAction act = MenuAction::None;
    for (int c = GetCharPressed(); c > 0; c = GetCharPressed())
        if (c >= 32 && c < 127 && c != '/' && c != '\\' && m.inputText.size() < 32) m.inputText += (char)c;
    if ((IsKeyPressed(KEY_BACKSPACE) || IsKeyPressedRepeat(KEY_BACKSPACE)) && !m.inputText.empty()) m.inputText.pop_back();
    bool ok = IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER), cancel = IsKeyPressed(KEY_ESCAPE);
    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
        for (const MenuHit& h : hits)
            if (CheckCollisionPointRec(GetMousePosition(), {h.x, h.y, h.w, h.h})) {
                ok = ok || h.row == 6000;
                cancel = cancel || h.row == 6001;
            }
    const bool blank = m.inputText.find_first_not_of(' ') == std::string::npos;
    if (ok && !blank) {
        if (m.lineupSave) act = MenuAction::SaveLineup;
        else m.champName = m.inputText;
        m.lineupSave = m.champNaming = false;
    } else if (cancel) {
        m.lineupSave = m.champNaming = false;
    }
    return act;
}

MenuAction updateLineupList(MenuState& m, const std::vector<MenuHit>& hits) {
    const int n = (int)m.lineupFiles.size();
    auto rep = [](int key) { return IsKeyPressed(key) || IsKeyPressedRepeat(key); };
    MenuAction act = MenuAction::None;
    auto load = [&](int i) {
        if (i < 0 || i >= n) return;
        m.lineupPick = m.lineupFiles[i];
        m.lineupLoad = false;
        act = MenuAction::LoadLineup;
    };
    if (rep(KEY_UP)) m.lineupRow = std::max(0, m.lineupRow - 1);
    if (rep(KEY_DOWN)) m.lineupRow = std::min(n - 1, m.lineupRow + 1);
    if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER)) load(m.lineupRow);
    if (IsKeyPressed(KEY_ESCAPE) || IsKeyPressed(KEY_BACKSPACE)) m.lineupLoad = false;
    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
        for (const MenuHit& h : hits) {
            if (!CheckCollisionPointRec(GetMousePosition(), {h.x, h.y, h.w, h.h})) continue;
            if (h.row == 6099) m.lineupLoad = false;
            else if (h.row == 6098) load(m.lineupRow);
            else if (h.row >= 6100) {
                if (m.lineupRow == h.row - 6100) load(m.lineupRow);  // a second click loads
                else m.lineupRow = h.row - 6100;
            }
            break;
        }
    return act;
}

MenuAction updateSeason(MenuState& m, const std::vector<MenuHit>& hits) {
    MenuAction act = MenuAction::None;
    const bool over = !m.season || m.season->over();
    if ((IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER)) && !over) act = MenuAction::StartRound;
    if (IsKeyPressed(KEY_ESCAPE) || IsKeyPressed(KEY_BACKSPACE)) act = MenuAction::LeaveSeason;
    if (IsKeyPressed(KEY_TAB) || IsKeyPressed(KEY_LEFT) || IsKeyPressed(KEY_RIGHT)) m.seasonTab = 1 - m.seasonTab;
    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
        for (const MenuHit& h : hits) {
            if (!CheckCollisionPointRec(GetMousePosition(), {h.x, h.y, h.w, h.h})) continue;
            if (h.row == 7000 && !over) act = MenuAction::StartRound;
            else if (h.row == 7001) act = MenuAction::LeaveSeason;
            else if (h.row >= 7010 && h.row < 7020) m.seasonTab = h.row - 7010;
            break;
        }
    return act;
}

// A segmented button or tab picked option `v` of a row.
void setOption(MenuState& m, MenuState::Row row, int v, MenuAction& act) {
    using Row = MenuState::Row;
    switch (row) {
        case Row::Session:
            if (m.session != v) {
                m.session = std::clamp(v, 0, MenuState::kSessions - 1);
                m.row = m.rowOf(Row::Session);
                if (m.champ()) act = MenuAction::ChampTab;
            }
            break;
        case Row::TyreRule: m.tyreRule = std::clamp(v, 0, 2); break;
        case Row::Drivers:
            if (!m.teamSlots.empty() && m.drivers != v + 1) {
                m.drivers = std::clamp(v + 1, 1, 2);
                m.layoutGrid();
            }
            break;
        case Row::ChampQuali: m.champQuali = v == 1; break;
        case Row::TestTyres: m.testTires = std::clamp(v, 0, 3); break;
        default: break;
    }
}

bool isButtonRow(MenuState::Row r) {
    using Row = MenuState::Row;
    return r == Row::Grid || r == Row::Stats || r == Row::TestStats || r == Row::TestRuns || r == Row::SaveLineup ||
           r == Row::LoadLineup || r == Row::ChampName || r == Row::AddRound;
}

}  // namespace

namespace {

void applyPick(MenuState& m) {
    const Color c = ColorFromHSV(m.pickH, m.pickS, m.pickV);
    if (m.pickTeam >= 0 && m.pickTeam < (int)m.teamSlots.size())
        for (int slot : m.teamSlots[m.pickTeam]) setSlotCar(slot, 0, c);   // the Mercedes in this colour
}

void openPicker(MenuState& m, int team) {
    if (team < 0 || team >= (int)m.teamSlots.size() || m.teamSlots[team].empty()) return;
    const auto& table = liveryTable();
    const int slot = m.teamSlots[team][0];
    const Color c = slot < (int)table.size() ? table[slot].color : Color{200, 200, 200, 255};
    const Vector3 hsv = ColorToHSV(c);
    m.pickTeam = team;
    m.pickH = hsv.x;
    m.pickS = hsv.y;
    m.pickV = hsv.z;
    m.pickDrag = 0;
}

void chooseStock(MenuState& m, int model) {
    if (!stockPresent(model) || m.pickTeam < 0 || m.pickTeam >= (int)m.teamSlots.size()) return;
    for (int slot : m.teamSlots[m.pickTeam]) setSlotCar(slot, model, stockColour(model));
    const Vector3 hsv = ColorToHSV(stockColour(model));   // the picker starts from it if the colour is changed next
    m.pickH = hsv.x, m.pickS = hsv.y, m.pickV = hsv.z;
}

// The colour picker is modal: drag in the square (saturation across, value up) and on the hue bar,
// click a preset, Enter / Esc / Done / a click outside the card to finish.
MenuAction updatePicker(MenuState& m, const std::vector<MenuHit>& hits) {
    if (IsKeyPressed(KEY_ESCAPE) || IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER)) {
        m.pickTeam = -1;
        m.pickDrag = 0;
        return MenuAction::None;
    }
    const Vector2 mp = GetMousePosition();
    if (!IsMouseButtonDown(MOUSE_BUTTON_LEFT)) m.pickDrag = 0;
    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        bool onCard = false;
        for (const MenuHit& h : hits) {
            if (h.row < 8200 || !CheckCollisionPointRec(mp, {h.x, h.y, h.w, h.h})) continue;
            onCard = true;
            if (h.row == 8200) m.pickDrag = 1;
            else if (h.row == 8201) m.pickDrag = 2;
            else if (h.row == 8399) { m.pickTeam = -1; return MenuAction::None; }
            else if (h.row >= 8400 && h.row < 8400 + kStockCars + 1) chooseStock(m, h.row - 8400);
            else if (h.row >= 8300 && h.row < 8398) {
                const Vector3 hsv = ColorToHSV(presetColor(h.row - 8300));
                m.pickH = hsv.x, m.pickS = hsv.y, m.pickV = hsv.z;
                applyPick(m);
            }
        }
        if (!onCard) { m.pickTeam = -1; return MenuAction::None; }
    }
    if (m.pickDrag) {
        for (const MenuHit& h : hits) {
            if (h.row == 8200 && m.pickDrag == 1) {
                m.pickS = std::clamp((mp.x - h.x) / h.w, 0.0f, 1.0f);
                m.pickV = 1.0f - std::clamp((mp.y - h.y) / h.h, 0.0f, 1.0f);
                applyPick(m);
            } else if (h.row == 8201 && m.pickDrag == 2) {
                m.pickH = 359.99f * std::clamp((mp.y - h.y) / h.h, 0.0f, 1.0f);
                applyPick(m);
            }
        }
    }
    return MenuAction::None;
}

}  // namespace

MenuAction updateMenu(MenuState& m, const std::vector<MenuHit>& hits) {
    if (m.pickTeam >= 0) return updatePicker(m, hits);
    if (m.popup >= 0) return updatePopup(m, hits);
    if (m.typing()) return updateTyping(m, hits);
    if (m.lineupLoad) return updateLineupList(m, hits);
    if (m.seasonPage) return updateSeason(m, hits);
    if (m.gridPage) return updateGrid(m, hits);
    if (m.teamsPage) return updateTeams(m, hits);
    if (m.testStatsPage) return updateTestStats(m, hits);
    if (m.runsPage) return updateRuns(m, hits);
    using Row = MenuState::Row;
    const std::vector<Row> rows = m.rows();
    const int n = (int)rows.size();
    m.row = std::clamp(m.row, 0, n - 1);
    MenuAction act = MenuAction::None;
    const bool big = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
    const bool ctrl = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
    const Row cur = rows[m.row];
    if (cur == Row::Round && ctrl) {  // Ctrl+Up/Down moves the round
        const int k = m.rowIndex(m.row);
        if (IsKeyPressed(KEY_UP) && k > 0) { moveRound(m, k, -1); --m.row; }
        else if (IsKeyPressed(KEY_DOWN) && k + 1 < (int)m.calendar.size()) { moveRound(m, k, 1); ++m.row; }
    } else {
        if (IsKeyPressed(KEY_UP) || IsKeyPressedRepeat(KEY_UP)) m.row = (m.row + n - 1) % n;
        if (IsKeyPressed(KEY_DOWN) || IsKeyPressedRepeat(KEY_DOWN)) m.row = (m.row + 1) % n;
    }
    for (int key : {KEY_LEFT, KEY_RIGHT})
        if (IsKeyPressed(key) || IsKeyPressedRepeat(key)) change(m, rows[m.row], key == KEY_LEFT ? -1 : 1, big, act);
    if (IsKeyPressed(KEY_BACKSPACE) && rows[m.row] == Row::TestFuel) m.testFuel = 0;
    if ((IsKeyPressed(KEY_DELETE) || IsKeyPressed(KEY_BACKSPACE)) && cur == Row::Round) {
        const int k = m.rowIndex(m.row);
        if (k >= 0 && k < (int)m.calendar.size()) m.calendar.erase(m.calendar.begin() + k);
    }
    if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER)) {
        const Row r = rows[m.row];
        if (r == Row::Season) {
            m.seasonPick = m.rowIndex(m.row);
            act = MenuAction::ContinueSeason;
        } else if (isButtonRow(r) || r == Row::ChampQuali || r == Row::Start) {
            select(m, r, act);
        } else if (r != Row::Round) {
            act = m.champ() ? MenuAction::NewSeason : MenuAction::Start;  // Enter is the start shortcut; Space is not
        }
    }
    // Esc only ever steps back, so leaving a session can't close the viewer; Ctrl+Q quits.
    if (IsKeyPressed(KEY_Q) && (IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL))) act = MenuAction::Quit;

    const Vector2 mp = GetMousePosition();
    const bool moved = GetMouseDelta().x != 0 || GetMouseDelta().y != 0;
    for (const MenuHit& h : hits) {
        if (!CheckCollisionPointRec(mp, {h.x, h.y, h.w, h.h})) continue;
        if (h.row == 4400) {  // the default calendar
            if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) { m.resetCalendar(); break; }
            continue;
        }
        if (h.row >= 4000 && h.row < 4400) {  // calendar round buttons
            if (!IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) continue;
            const int k = (h.row - 4000) / 4, what = (h.row - 4000) % 4;
            if (what == 0 && k < (int)m.calendar.size()) m.calendar.erase(m.calendar.begin() + k);
            else if (what == 1) moveRound(m, k, -1);
            else if (what == 2) moveRound(m, k, 1);
            break;
        }
        if (h.row >= 4500 && h.row < 5000) {  // a saved season
            if (!IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) continue;
            m.seasonPick = h.row - 4500;
            act = MenuAction::ContinueSeason;
            break;
        }
        if (h.row >= 8000 && h.row < 8100) {  // a team's colour (RR2)
            if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) { openPicker(m, h.row - 8000); break; }
            continue;
        }
        if (h.row < 0 || h.row >= n) continue;
        const Row r = rows[h.row];
        if (h.dir == 0 && h.value < 0 && r != Row::Start && moved) m.row = h.row;  // hovering a row selects it
        if (!IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) continue;
        m.row = h.row;
        if (h.value >= 0) setOption(m, r, h.value, act);
        else if (h.dir == 2) m.openPopup(h.row, h.x, h.y + h.h + 4, std::max(h.w, 260.0f));
        else if (h.dir != 0) change(m, r, h.dir, big, act);
        else if (r == Row::Start || isButtonRow(r) || r == Row::TestFuel) select(m, r, act);
        else continue;
        break;
    }
    return act;
}
