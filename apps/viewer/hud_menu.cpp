// The setup menu: session tabs, cards of settings, the grid and team stats pages,
// championship setup and the season page, and the drop-downs and dialogs.
#include "hud.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "liveries.hpp"

namespace {

const Color kText = {238, 240, 245, 255};
const Color kDim = {150, 157, 172, 255};
const Color kFaint = {98, 104, 118, 255};
const Color kAccent = {255, 196, 40, 255};
const Color kInk = {20, 20, 24, 255};       // text on the accent colour
const Color kCard = {16, 19, 27, 255};
const Color kWell = {30, 34, 46, 255};      // inputs and buttons
const Color kWellHi = {44, 50, 66, 255};
const Color kLine = {255, 255, 255, 22};
const Color kGood = {90, 200, 120, 255};
const Color kBad = {240, 110, 70, 255};

bool hover(Rectangle r) { return CheckCollisionPointRec(GetMousePosition(), r); }

float rnd(Rectangle r, float px) { return px / std::max(1.0f, std::min(r.width, r.height)); }

// Tabs in display order: quick race, weekend, championship, testing.
const int kTabSession[] = {0, 1, 3, 2};
const char* kTabName[] = {"Quick race", "Race weekend", "Championship", "Testing"};

std::string driverLabel(int slot, const std::string& name) {
    const auto& t = liveryTable();
    return slot >= 0 && slot < (int)t.size() ? "#" + std::to_string(t[slot].number) + "  " + name : name;
}
Color slotColor(int slot) {
    const auto& t = liveryTable();
    return slot >= 0 && slot < (int)t.size() ? t[slot].color : Color{170, 170, 170, 255};
}

}  // namespace

// ---------------------------------------------------------------- widgets

void Hud::card(Rectangle r, const char* title, const char* sub) {
    DrawRectangleRounded(r, rnd(r, 14), 8, Fade(kCard, 0.93f));
    DrawRectangleRoundedLinesEx(r, rnd(r, 14), 8, 1.0f, kLine);
    if (!title) return;
    text(title, r.x + 22, r.y + 18, 13, kAccent, true);
    if (sub) textRight(sub, r.x + r.width - 22, r.y + 18, 13, kDim);
}

void Hud::button(Rectangle r, const char* label, int style, bool focus, int row, std::vector<MenuHit>& hits, int value) {
    const bool hot = hover(r) || focus;
    Color fill = style == 0 ? (hot ? Color{255, 214, 90, 255} : kAccent) : style == 1 ? (hot ? kWellHi : kWell)
                                                                                     : (hot ? Fade(WHITE, 0.08f) : BLANK);
    DrawRectangleRounded(r, rnd(r, 10), 8, fill);
    if (style == 2) DrawRectangleRoundedLinesEx(r, rnd(r, 10), 8, 1.2f, hot ? Fade(kText, 0.5f) : Fade(kDim, 0.35f));
    if (focus && style != 0) DrawRectangleRoundedLinesEx(r, rnd(r, 10), 8, 2.0f, kAccent);
    const float size = style == 0 ? std::min(24.0f, r.height * 0.42f) : 16;
    float sz = size;
    while (sz > 11 && width(label, sz, true) > r.width - 16) sz -= 1;
    text(label, r.x + (r.width - width(label, sz, true)) / 2, r.y + (r.height - sz) / 2 - 1, sz,
         style == 0 ? kInk : kText, true);
    hits.push_back({r.x, r.y, r.width, r.height, row, 0, value});
}

void Hud::stepper(Rectangle r, const char* value, bool focus, int row, std::vector<MenuHit>& hits) {
    DrawRectangleRounded(r, rnd(r, 10), 8, kWell);
    if (focus) DrawRectangleRoundedLinesEx(r, rnd(r, 10), 8, 2.0f, kAccent);
    const float b = r.height;
    for (int side = 0; side < 2; ++side) {
        Rectangle k = {side ? r.x + r.width - b : r.x, r.y, b, b};
        const bool hot = hover(k);
        DrawRectangleRounded({k.x + 4, k.y + 4, k.width - 8, k.height - 8}, 0.4f, 6, hot ? kWellHi : Fade(WHITE, 0.04f));
        const float cx = k.x + b / 2, cy = k.y + b / 2;
        DrawRectangle((int)(cx - 6), (int)(cy - 1), 12, 2, hot || focus ? kText : kDim);  // minus
        if (side) DrawRectangle((int)(cx - 1), (int)(cy - 6), 2, 12, hot || focus ? kText : kDim);  // plus
        hits.push_back({k.x, k.y, k.width, k.height, row, side ? 1 : -1});
    }
    float sz = 19;
    while (sz > 12 && width(value, sz, true) > r.width - 2 * b - 8) sz -= 1;
    text(value, r.x + (r.width - width(value, sz, true)) / 2, r.y + (r.height - sz) / 2 - 1, sz, kText, true);
    hits.push_back({r.x + b, r.y, r.width - 2 * b, r.height, row, 0});
}

void Hud::segmented(Rectangle r, const std::vector<const char*>& options, int sel, bool focus, int row,
                    std::vector<MenuHit>& hits) {
    DrawRectangleRounded(r, rnd(r, 10), 8, kWell);
    if (focus) DrawRectangleRoundedLinesEx(r, rnd(r, 10), 8, 2.0f, kAccent);
    const int n = (int)options.size();
    const float w = r.width / n;
    for (int i = 0; i < n; ++i) {
        Rectangle k = {r.x + i * w + 3, r.y + 3, w - 6, r.height - 6};
        if (i == sel) DrawRectangleRounded(k, rnd(k, 8), 8, kAccent);
        else if (hover(k)) DrawRectangleRounded(k, rnd(k, 8), 8, kWellHi);
        float sz = 15;
        while (sz > 10 && width(options[i], sz, true) > k.width - 8) sz -= 1;
        text(options[i], k.x + (k.width - width(options[i], sz, true)) / 2, k.y + (k.height - sz) / 2 - 1, sz,
             i == sel ? kInk : kText, true);
        hits.push_back({k.x, k.y, k.width, k.height, row, 0, i});
    }
}

void Hud::dropdown(Rectangle r, const char* value, bool focus, int row, std::vector<MenuHit>& hits, Color chip) {
    const bool hot = hover(r);
    DrawRectangleRounded(r, rnd(r, 10), 8, hot ? kWellHi : kWell);
    if (focus) DrawRectangleRoundedLinesEx(r, rnd(r, 10), 8, 2.0f, kAccent);
    float x = r.x + 14;
    if (chip.a) {
        DrawRectangleRounded({x, r.y + r.height * 0.25f, 6, r.height * 0.5f}, 0.5f, 4, chip);
        x += 16;
    }
    float sz = std::min(18.0f, r.height * 0.45f);
    while (sz > 11 && width(value, sz, true) > r.x + r.width - x - 34) sz -= 1;
    text(value, x, r.y + (r.height - sz) / 2 - 1, sz, kText, true);
    // chevron
    const float cx = r.x + r.width - 18, cy = r.y + r.height / 2;
    DrawTriangle({cx - 6, cy - 3}, {cx, cy + 4}, {cx + 6, cy - 3}, focus || hot ? kText : kDim);
    hits.push_back({r.x, r.y, r.width, r.height, row, 2});
}

void Hud::field(float x, float y, float w, const char* label, const char* note, bool focus) {
    text(label, x, y, 16, focus ? kText : Fade(kText, 0.85f), true);
    if (note && note[0]) {
        float sz = 13;
        while (sz > 10 && width(note, sz) > w) sz -= 1;
        text(note, x, y + 21, sz, kDim);
    }
}

void Hud::trackShape(const std::vector<rr::Vec2>& pts, Rectangle box, Color c, float thick) {
    if (pts.size() < 3) return;
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
    for (const rr::Vec2& p : pts) x0 = std::min(x0, p.x), x1 = std::max(x1, p.x), y0 = std::min(y0, p.y), y1 = std::max(y1, p.y);
    const float s = std::min(box.width / std::max(1.0f, x1 - x0), box.height / std::max(1.0f, y1 - y0));
    const float ox = box.x + (box.width - (x1 - x0) * s) / 2, oy = box.y + (box.height - (y1 - y0) * s) / 2;
    auto at = [&](const rr::Vec2& p) { return Vector2{ox + (p.x - x0) * s, oy + (y1 - p.y) * s}; };
    for (size_t i = 0; i < pts.size(); ++i) DrawLineEx(at(pts[i]), at(pts[(i + 1) % pts.size()]), thick, c);
    if (thick >= 3) {
        const Vector2 a = at(pts[0]);
        DrawCircleV(a, thick + 2, kAccent);
    }
}

// ---------------------------------------------------------------- the setup page

void Hud::drawTopBar(const MenuState& m, std::vector<MenuHit>& hits, float& top) {
    const float sw = (float)GetScreenWidth();
    const float mx = std::max(24.0f, sw * 0.03f);
#ifdef RR2_RENDERER
    text("RAYLIB RACERS 2", mx, 26, 15, kAccent, true);
#else
    text("RAYLIB RACERS", mx, 26, 15, kAccent, true);
#endif
    const char* title = m.testing() ? "Testing" : m.champ() ? "Championship" : m.weekend() ? "Race weekend" : "Quick race";
    text(title, mx, 46, 34, kText, true);
    // session tabs
    const int row = m.rowOf(MenuState::Row::Session);
    const float tw = std::min(620.0f, sw - 2 * mx - 320);
    Rectangle r = {sw - mx - tw, 34, tw, 46};
    std::vector<const char*> names(kTabName, kTabName + 4);
    int sel = 0;
    for (int i = 0; i < 4; ++i)
        if (kTabSession[i] == m.session) sel = i;
    const size_t before = hits.size();
    segmented(r, names, sel, m.row == row, row, hits);
    for (size_t i = before; i < hits.size(); ++i) hits[i].value = kTabSession[hits[i].value];
    top = 108;
}

void Hud::drawTrackCard(const MenuState& m, std::vector<MenuHit>& hits, Rectangle r) {
    using Row = MenuState::Row;
    const TrackStats& ts = m.stats();
    char buf[160];
    std::snprintf(buf, sizeof buf, "%.2f km", ts.length / 1000.0f);
    card(r, "TRACK", buf);
    const int row = m.rowOf(Row::Track);
    dropdown({r.x + 20, r.y + 44, r.width - 40, 48}, ts.title.c_str(), m.row == row, row, hits);
    // the layout, start line marked
    Rectangle box = {r.x + 30, r.y + 110, r.width - 60, r.height - 110 - 92};
    const std::vector<rr::Vec2>& shape = ts.outline.empty() ? outline_ : ts.outline;
    trackShape(shape, box, Fade(WHITE, 0.16f), 11);
    trackShape(shape, box, kText, 3.5f);
    // facts
    const float fy = r.y + r.height - 76;
    DrawRectangle((int)r.x + 20, (int)fy - 10, (int)r.width - 40, 1, kLine);
    const float cw = (r.width - 40) / 3;
    const char* labels[] = {"LAP ABOUT", "FUEL / LAP", "TANK LASTS"};
    char vals[3][32];
    std::snprintf(vals[0], 32, "%d:%02d", (int)ts.lapTime / 60, (int)ts.lapTime % 60);
    std::snprintf(vals[1], 32, "%.1f L", ts.fuelPerLap);
    std::snprintf(vals[2], 32, "%.0f laps", m.lapsPerTank());
    for (int i = 0; i < 3; ++i) {
        text(labels[i], r.x + 20 + i * cw, fy + 4, 11, kDim, true);
        text(vals[i], r.x + 20 + i * cw, fy + 22, 22, kText, true);
    }
}

void Hud::drawGridCard(const MenuState& m, std::vector<MenuHit>& hits, Rectangle r, bool teamList) {
    using Row = MenuState::Row;
    char buf[160];
    std::snprintf(buf, sizeof buf, "%d cars", m.cars);
    card(r, "GRID", buf);
    float y = r.y + 50;
    const float lw = r.width - 40;
    const float cw = 190;  // control width
    int row = m.rowOf(Row::Teams);
    field(r.x + 20, y + 4, lw - cw - 10, "Teams", m.teamSlots.empty() ? "" : "each with its own stats", m.row == row);
    std::snprintf(buf, sizeof buf, "%d", m.teamSlots.empty() ? m.cars : m.teams);
    stepper({r.x + r.width - 20 - cw, y, cw, 42}, buf, m.row == row, row, hits);
    y += 58;
    row = m.rowOf(Row::Drivers);
    field(r.x + 20, y + 4, lw - cw - 10, "Drivers per team", m.drivers == 2 ? "teammates share stats" : "one car each",
          m.row == row);
    segmented({r.x + r.width - 20 - cw, y, cw, 42}, {"1", "2"}, m.drivers - 1, m.row == row, row, hits);
    y += 62;
    // the teams and who drives for them
    const auto& table = liveryTable();
    const float listBottom = r.y + r.height - 132;
    if (teamList && !m.teamSlots.empty()) {
        DrawRectangle((int)r.x + 20, (int)y - 6, (int)lw, 1, kLine);
        const std::vector<int> teams = m.raceTeams();
        const float rowH = std::min(34.0f, (listBottom - y) / std::max<size_t>(1, teams.size()));
        for (size_t k = 0; k < teams.size() && y + rowH <= listBottom + 2; ++k, y += rowH) {
            const int t = teams[k];
            const int slot = m.teamSlots[t].empty() ? -1 : m.teamSlots[t][0];
            DrawRectangleRounded({r.x + 22, y + 5, 5, rowH - 10}, 0.5f, 4, slotColor(slot));
            const char* name = slot >= 0 && slot < (int)table.size() ? table[slot].team.c_str() : "Team";
            text(name, r.x + 36, y + (rowH - 15) / 2 - 1, 15, kText, true);
            std::string who;
            for (int car = 0; car < m.cars; ++car)
                if (m.teamOfCar(car) == t && car < (int)m.carAlgo.size()) {
                    if (!who.empty()) who += "  /  ";
                    who += m.algos[m.carAlgo[car]].label;
                }
            float sz = 13;
            while (sz > 10 && width(who.c_str(), sz) > lw * 0.52f) sz -= 1;
            textRight(who.c_str(), r.x + r.width - 20, y + (rowH - sz) / 2, sz, kDim);
        }
    }
    // buttons
    const float by = r.y + r.height - 116, bw = (lw - 12) / 2;
    row = m.rowOf(Row::Grid);
    button({r.x + 20, by, bw, 44}, "Edit grid", 1, m.row == row, row, hits);
    row = m.rowOf(Row::Stats);
    button({r.x + 32 + bw, by, bw, 44}, "Team stats", 1, m.row == row, row, hits);
    row = m.rowOf(Row::SaveLineup);
    button({r.x + 20, by + 56, bw, 40}, "Save lineup", 2, m.row == row, row, hits);
    row = m.rowOf(Row::LoadLineup);
    button({r.x + 32 + bw, by + 56, bw, 40}, "Load lineup", 2, m.row == row, row, hits);
}

void Hud::drawRaceSetup(const MenuState& m, std::vector<MenuHit>& hits, Rectangle a) {
    using Row = MenuState::Row;
    const float gap = 22, cw = (a.width - 2 * gap) / 3;
    drawTrackCard(m, hits, {a.x, a.y, cw, a.height});
    // race rules
    Rectangle r = {a.x + cw + gap, a.y, cw, a.height};
    card(r, m.weekend() ? "RACE WEEKEND" : "RACE", m.weekend() ? "qualifying sets the grid" : "grid as set");
    const TrackStats& ts = m.stats();
    char val[64], note[200];
    float y = r.y + 50;
    const float ctl = 190, lw = r.width - 40 - ctl - 10;
    int row = m.rowOf(Row::Laps);
    const int mins = (int)std::lround(m.laps * ts.lapTime / 60.0f);
    const float tank = m.lapsPerTank();
    const int stops = (int)std::ceil(m.laps / tank - 1e-3f) - 1;
    std::snprintf(note, sizeof note, "about %d min, %s", std::max(1, mins),
                  stops <= 0 ? "no fuel stop" : stops == 1 ? "1 fuel stop" : (std::to_string(stops) + " fuel stops").c_str());
    field(r.x + 20, y + 2, lw, "Race length", note, m.row == row);
    std::snprintf(val, sizeof val, "%d lap%s", m.laps, m.laps == 1 ? "" : "s");
    stepper({r.x + r.width - 20 - ctl, y, ctl, 44}, val, m.row == row, row, hits);
    y += 70;
    row = m.rowOf(Row::TyreLife);
    const int life = MenuState::kTyreLives[m.tyreLife];
    if (life == 0) std::snprintf(note, sizeof note, "tyres never wear out");
    else
        std::snprintf(note, sizeof note, "soft %d, medium %d, hard %d laps",
                      std::max(1, (int)std::lround(life / rr::compoundWear(RR_TIRE_SOFT))), life,
                      (int)std::lround(life / rr::compoundWear(RR_TIRE_HARD)));
    field(r.x + 20, y + 2, lw, "Tyre life", note, m.row == row);
    if (life == 0) std::snprintf(val, sizeof val, "no wear");
    else std::snprintf(val, sizeof val, "%d laps", life);
    stepper({r.x + r.width - 20 - ctl, y, ctl, 44}, val, m.row == row, row, hits);
    y += 70;
    row = m.rowOf(Row::TyreRule);
    field(r.x + 20, y, r.width - 40, "Tyre rule",
          m.twoCompoundRule() ? "two different compounds or +30 s"
          : m.tyreRule == 0   ? "two compounds only in races over 20 laps"
                              : "any tyres, any number of stops",
          m.row == row);
    segmented({r.x + 20, y + 46, r.width - 40, 42}, {"Auto", "Two compounds", "Free"}, m.tyreRule, m.row == row, row, hits);
    y += 110;
    if (m.weekend()) {
        row = m.rowOf(Row::Practice);
        field(r.x + 20, y + 2, lw, "Practice", m.practiceLaps() ? "each car alone, any tyres" : "straight to qualifying",
              m.row == row);
        if (m.practiceLaps()) std::snprintf(val, sizeof val, "%d laps", m.practiceLaps());
        else std::snprintf(val, sizeof val, "off");
        stepper({r.x + r.width - 20 - ctl, y, ctl, 44}, val, m.row == row, row, hits);
        y += 66;
    }
    // the session explained
    DrawRectangle((int)r.x + 20, (int)y, (int)r.width - 40, 1, kLine);
    const char* about = m.weekend()
        ? "Each car runs alone, in practice and in qualifying (an out lap and two flying laps). The fastest qualifying lap takes pole."
        : "The race starts in the order of the grid page. Pick Race weekend for qualifying.";
    // wrap
    std::string line, word, txt = about;
    float ty = y + 14;
    for (size_t i = 0; i <= txt.size(); ++i) {
        if (i == txt.size() || txt[i] == ' ') {
            const std::string next = line.empty() ? word : line + " " + word;
            if (width(next.c_str(), 14) > r.width - 40) {
                text(line.c_str(), r.x + 20, ty, 14, kDim);
                ty += 20;
                line = word;
            } else {
                line = next;
            }
            word.clear();
        } else {
            word += txt[i];
        }
    }
    if (!line.empty()) text(line.c_str(), r.x + 20, ty, 14, kDim);
    // what one set of each compound covers in this race
    ty += 44;
    text("ONE SET OF TYRES LASTS", r.x + 20, ty, 11, kDim, true);
    ty += 24;
    const float bw = r.width - 40;
    for (int c = RR_TIRE_SOFT; c <= RR_TIRE_HARD && ty + 30 < r.y + r.height; ++c, ty += 40) {
        const float laps = life == 0 ? (float)m.laps : life / rr::compoundWear(c);
        DrawCircleV({r.x + 32, ty + 12}, 11, compoundColor(c));
        DrawCircleV({r.x + 32, ty + 12}, 6.5f, kCard);
        text(compoundName(c), r.x + 52, ty + 3, 15, kText, true);
        const float bx = r.x + 140, bl = bw - 120 - 70;
        DrawRectangleRounded({bx, ty + 8, bl, 8}, 0.5f, 4, Fade(WHITE, 0.08f));
        DrawRectangleRounded({bx, ty + 8, bl * std::min(1.0f, laps / std::max(1, m.laps)), 8}, 0.5f, 4,
                             Fade(compoundColor(c), 0.85f));
        if (life == 0) std::snprintf(val, sizeof val, "no wear");
        else if (laps >= m.laps) std::snprintf(val, sizeof val, "the race");
        else std::snprintf(val, sizeof val, "%.0f laps", laps);
        textRight(val, r.x + r.width - 20, ty + 3, 15, laps >= m.laps ? kGood : kText, true);
    }
    drawGridCard(m, hits, {a.x + 2 * (cw + gap), a.y, cw, a.height}, true);
}

void Hud::drawChampSetup(const MenuState& m, std::vector<MenuHit>& hits, Rectangle a) {
    using Row = MenuState::Row;
    const float gap = 22, cw = (a.width - 2 * gap) / 3;
    char buf[200], note[200];
    // ---- calendar
    Rectangle r = {a.x, a.y, cw, a.height};
    float km = 0;
    for (const auto& rd : m.calendar) {
        const TrackStats* t = m.trackStats(rd.track);
        if (t) km += m.roundLaps(rd) * t->length / 1000.0f;
    }
    std::snprintf(buf, sizeof buf, "%zu rounds", m.calendar.size());
    card(r, "CALENDAR", buf);
    const float rowH = std::min(50.0f, (r.height - 170) / std::max<size_t>(8, m.calendar.size()));
    float y = r.y + 46;
    const int first = m.rowOf(Row::Round);
    for (size_t k = 0; k < m.calendar.size(); ++k, y += rowH) {
        const int row = first + (int)k;
        const bool sel = m.row == row;
        Rectangle line = {r.x + 12, y, r.width - 24, rowH - 4};
        if (sel || hover(line)) DrawRectangleRounded(line, rnd(line, 8), 6, Fade(WHITE, sel ? 0.08f : 0.04f));
        if (sel) DrawRectangleRounded({line.x, line.y + 6, 3, line.height - 12}, 0.5f, 4, kAccent);
        hits.push_back({line.x, line.y, line.width - 100, line.height, row, 0});
        const TrackStats* t = m.trackStats(m.calendar[k].track);
        std::snprintf(buf, sizeof buf, "%zu", k + 1);
        textRight(buf, line.x + 26, line.y + (line.height - 16) / 2, 16, kDim, true);
        if (t) trackShape(t->outline, {line.x + 34, line.y + 5, 44, line.height - 10}, kText, 1.6f);
        text(t ? t->title.c_str() : m.calendar[k].track.c_str(), line.x + 88, line.y + 6, 16, kText, true);
        const float soft = m.compoundLaps(m.calendar[k].track, RR_TIRE_SOFT, m.champWearRate());
        if (soft > 0) std::snprintf(note, sizeof note, "%d laps   softs ~%.0f", m.roundLaps(m.calendar[k]), soft);
        else std::snprintf(note, sizeof note, "%d laps", m.roundLaps(m.calendar[k]));
        text(note, line.x + 88, line.y + 26, 13, kDim);
        // move and remove
        const char* icons[] = {"x", "^", "v"};
        for (int b = 0; b < 3; ++b) {
            const int what = b == 0 ? 0 : 3 - b;  // right to left: remove, down, up
            Rectangle k2 = {line.x + line.width - 30 - b * 32, line.y + (line.height - 28) / 2, 28, 28};
            const bool hot = hover(k2);
            if (hot) DrawRectangleRounded(k2, 0.4f, 6, kWellHi);
            const float cx = k2.x + 14, cy = k2.y + 14;
            const Color ic = hot ? kText : kFaint;
            if (what == 0) {
                DrawLineEx({cx - 5, cy - 5}, {cx + 5, cy + 5}, 2, hot ? kBad : ic);
                DrawLineEx({cx - 5, cy + 5}, {cx + 5, cy - 5}, 2, hot ? kBad : ic);
            } else if (what == 1) {
                DrawTriangle({cx, cy - 5}, {cx - 6, cy + 4}, {cx + 6, cy + 4}, ic);
            } else {
                DrawTriangle({cx - 6, cy - 4}, {cx, cy + 5}, {cx + 6, cy - 4}, ic);
            }
            (void)icons;
            hits.push_back({k2.x, k2.y, k2.width, k2.height, 4000 + (int)k * 4 + what, 0});
        }
    }
    if (m.calendar.empty()) text("Add tracks to build a season.", r.x + 22, y + 10, 15, kDim);
    // add a track
    const float ay = r.y + r.height - 112;
    DrawRectangle((int)r.x + 20, (int)ay - 12, (int)r.width - 40, 1, kLine);
    const int add = m.rowOf(Row::AddRound);
    const std::string addName = m.addTrack < (int)m.tracks.size() ? m.tracks[m.addTrack].title : "?";
    dropdown({r.x + 20, ay, r.width - 40 - 112, 44}, addName.c_str(), m.row == add, add, hits);
    button({r.x + r.width - 20 - 100, ay, 100, 44}, "+ Add", 1, false, add, hits);
    hits.back().dir = 0;
    Rectangle reset = {r.x + 20, ay + 56, r.width - 40, 36};
    button(reset, "Reset to the 8-track calendar", 2, false, 4400, hits);
    std::snprintf(buf, sizeof buf, "season distance about %.0f km", km);
    (void)buf;

    // ---- rules and grid
    Rectangle rr2 = {a.x + cw + gap, a.y, cw, a.height * 0.52f - gap / 2};
    card(rr2, "RULES");
    y = rr2.y + 44;
    const float ctl = 180, lw = rr2.width - 40 - ctl - 10;
    int row = m.rowOf(Row::ChampName);
    field(rr2.x + 20, y + 10, lw, "Name", "", m.row == row);
    button({rr2.x + rr2.width - 20 - 240, y, 240, 40}, m.champName.c_str(), 1, m.row == row, row, hits);
    y += 52;
    row = m.rowOf(Row::ChampDistance);
    int lo = 1 << 30, hi = 0;
    for (const auto& rd : m.calendar) lo = std::min(lo, m.roundLaps(rd)), hi = std::max(hi, m.roundLaps(rd));
    if (hi) std::snprintf(note, sizeof note, "%d to %d laps a race", lo, hi);
    else note[0] = 0;
    field(rr2.x + 20, y + 2, lw, "Race distance", note, m.row == row);
    std::snprintf(buf, sizeof buf, "%.0f km", m.champDistance());
    stepper({rr2.x + rr2.width - 20 - ctl, y, ctl, 42}, buf, m.row == row, row, hits);
    y += 56;
    row = m.rowOf(Row::ChampWear);
    float slo = 1e9f, shi = 0;
    for (const auto& rd : m.calendar) {
        const float s = m.compoundLaps(rd.track, RR_TIRE_SOFT, m.champWearRate());
        if (s > 0) slo = std::min(slo, s), shi = std::max(shi, s);
    }
    if (m.champWearRate() <= 0) std::snprintf(note, sizeof note, "tyres never wear out");
    else if (shi > 0) std::snprintf(note, sizeof note, "softs last %.0f to %.0f laps, by track", slo, shi);
    else note[0] = 0;
    field(rr2.x + 20, y + 2, lw, "Tyre wear", note, m.row == row);
    if (m.champWearRate() <= 0) std::snprintf(buf, sizeof buf, "off");
    else std::snprintf(buf, sizeof buf, "%gx", m.champWearRate());
    stepper({rr2.x + rr2.width - 20 - ctl, y, ctl, 42}, buf, m.row == row, row, hits);
    y += 56;
    row = m.rowOf(Row::TyreRule);
    field(rr2.x + 20, y + 10, lw - 60, "Tyre rule", "", m.row == row);
    segmented({rr2.x + rr2.width - 20 - ctl - 60, y, ctl + 60, 40}, {"Auto", "Two", "Free"}, m.tyreRule, m.row == row,
              row, hits);
    y += 52;
    row = m.rowOf(Row::ChampQuali);
    field(rr2.x + 20, y + 10, lw, "Qualifying", "", m.row == row);
    segmented({rr2.x + rr2.width - 20 - ctl, y, ctl, 40}, {"Off", "On"}, m.champQuali ? 1 : 0, m.row == row, row, hits);
    y += 52;
    row = m.rowOf(Row::Practice);
    field(rr2.x + 20, y + 2, lw, "Practice", m.practiceLaps() ? "laps per car, before qualifying" : "", m.row == row);
    if (m.practiceLaps()) std::snprintf(buf, sizeof buf, "%d laps", m.practiceLaps());
    else std::snprintf(buf, sizeof buf, "off");
    stepper({rr2.x + rr2.width - 20 - ctl, y, ctl, 42}, buf, m.row == row, row, hits);
    drawGridCard(m, hits, {a.x + cw + gap, rr2.y + rr2.height + gap, cw, a.height - rr2.height - gap}, false);

    // ---- saved seasons
    Rectangle s = {a.x + 2 * (cw + gap), a.y, cw, a.height};
    card(s, "SAVED CHAMPIONSHIPS");
    y = s.y + 48;
    const int firstSeason = m.rowOf(Row::Season);
    if (m.seasons.empty()) {
        text("None yet. Set up the calendar and", s.x + 22, y + 6, 15, kDim);
        text("the grid, then start a championship.", s.x + 22, y + 28, 15, kDim);
    }
    for (size_t i = 0; i < m.seasons.size() && y + 96 < s.y + s.height; ++i, y += 104) {
        const MenuState::SeasonLine& l = m.seasons[i];
        const int row2 = firstSeason + (int)i;
        Rectangle box = {s.x + 16, y, s.width - 32, 94};
        const bool sel = m.row == row2;
        DrawRectangleRounded(box, rnd(box, 10), 8, sel || hover(box) ? kWellHi : kWell);
        if (sel) DrawRectangleRoundedLinesEx(box, rnd(box, 10), 8, 2.0f, kAccent);
        text(l.name.c_str(), box.x + 16, box.y + 12, 18, kText, true);
        const bool over = l.done >= l.total;
        std::snprintf(buf, sizeof buf, over ? "Finished, %d rounds" : "Round %d of %d", over ? l.total : l.done + 1, l.total);
        textRight(buf, box.x + box.width - 16, box.y + 15, 14, over ? kGood : kAccent, true);
        std::snprintf(note, sizeof note, "%s%s", l.leader.empty() ? "" : (over ? "Champion: " : "Leader: "), l.leader.c_str());
        text(note[0] ? note : "No races yet", box.x + 16, box.y + 40, 14, kDim);
        if (!over && !l.next.empty()) {
            std::snprintf(note, sizeof note, "Next: %s", l.next.c_str());
            text(note, box.x + 16, box.y + 62, 14, kDim);
        }
        button({box.x + box.width - 116, box.y + box.height - 46, 100, 34}, over ? "Open" : "Continue", 2, false,
               4500 + (int)i, hits);
        hits.push_back({box.x, box.y, box.width - 120, box.height, row2, 0});
    }
}

void Hud::drawTestSetup(const MenuState& m, std::vector<MenuHit>& hits, Rectangle a) {
    using Row = MenuState::Row;
    const float gap = 22, cw = (a.width - 2 * gap) / 3;
    drawTrackCard(m, hits, {a.x, a.y, cw, a.height});
    char val[96], note[200];
    const float ctl = 200;
    // the run
    Rectangle r = {a.x + cw + gap, a.y, cw, a.height};
    card(r, "TEST CAR");
    float y = r.y + 46;
    const float lw = r.width - 40 - ctl - 10;
    int row = m.rowOf(Row::TestCar);
    const Algorithm* al = m.testAlgo < (int)m.algos.size() ? &m.algos[m.testAlgo] : nullptr;
    field(r.x + 20, y + 12, lw, "Algorithm", "", m.row == row);
    dropdown({r.x + r.width - 20 - ctl - 40, y, ctl + 40, 44}, al ? al->label.c_str() : "?", m.row == row, row, hits);
    y += 58;
    row = m.rowOf(Row::TestLivery);
    const auto& table = liveryTable();
    const int slot = m.testLivery;
    if (slot < (int)table.size()) std::snprintf(val, sizeof val, "#%d  %s", table[slot].number, table[slot].team.c_str());
    else std::snprintf(val, sizeof val, "livery %d", slot + 1);
    field(r.x + 20, y + 12, lw, "Car", "", m.row == row);
    dropdown({r.x + r.width - 20 - ctl - 40, y, ctl + 40, 44}, val, m.row == row, row, hits, slotColor(slot));
    y += 62;
    row = m.rowOf(Row::TestTyres);
    static const char* names[] = {"Auto", "Soft", "Medium", "Hard"};
    const int used = m.testTiresUsed();
    if (m.compoundLife[RR_TIRE_MEDIUM] <= 0) std::snprintf(note, sizeof note, "no tyre wear in this session");
    else
        std::snprintf(note, sizeof note, "soft %.0f, medium %.0f, hard %.0f laps%s%s", m.compoundLife[RR_TIRE_SOFT],
                      m.compoundLife[RR_TIRE_MEDIUM], m.compoundLife[RR_TIRE_HARD], m.testTires ? "" : ", auto: ",
                      m.testTires ? "" : names[used & 3]);
    field(r.x + 20, y, r.width - 40, "Tyres", note, m.row == row);
    segmented({r.x + 20, y + 44, r.width - 40, 40}, {"Auto", "Soft", "Medium", "Hard"}, m.testTires, m.row == row, row,
              hits);
    y += 100;
    row = m.rowOf(Row::TestFuel);
    const float f = m.testFuelUsed();
    const float laps = f / std::max(0.1f, m.fuelPerLapEst);
    std::snprintf(note, sizeof note, "%.2f L/lap: %.1f laps%s", m.fuelPerLapEst, laps, laps < m.laps ? " (runs dry!)" : "");
    field(r.x + 20, y + 2, lw, "Fuel", note, m.row == row);
    if (m.testFuel > 0) std::snprintf(val, sizeof val, "%.0f L", f);
    else std::snprintf(val, sizeof val, "auto %.1f L", f);
    stepper({r.x + r.width - 20 - ctl, y, ctl, 44}, val, m.row == row, row, hits);
    y += 70;
    row = m.rowOf(Row::Laps);
    const int mins = (int)std::lround(m.laps * m.stats().lapTime / 60.0f);
    std::snprintf(note, sizeof note, "about %d min alone, no pit stops", std::max(1, mins));
    field(r.x + 20, y + 2, lw, "Run length", note, m.row == row);
    std::snprintf(val, sizeof val, "%d lap%s", m.laps, m.laps == 1 ? "" : "s");
    stepper({r.x + r.width - 20 - ctl, y, ctl, 44}, val, m.row == row, row, hits);
    y += 70;
    row = m.rowOf(Row::TyreLife);
    const int life = MenuState::kTyreLives[m.tyreLife];
    field(r.x + 20, y + 2, lw, "Tyre life", "medium tyre, stock car", m.row == row);
    if (life == 0) std::snprintf(val, sizeof val, "no wear");
    else std::snprintf(val, sizeof val, "%d laps", life);
    stepper({r.x + r.width - 20 - ctl, y, ctl, 44}, val, m.row == row, row, hits);
    // stats and saved runs
    Rectangle s = {a.x + 2 * (cw + gap), a.y, cw, a.height};
    card(s, "CAR STATS");
    y = s.y + 50;
    const StatRules& rules = m.statRules;
    for (size_t k = 0; k < rules.keys.size() && k < m.testStats.size(); ++k, y += 34) {
        text(rules.labels[k].c_str(), s.x + 22, y, 15, kText);
        const int v = m.testStats[k];
        const Color vc = v > rules.neutral ? kGood : v < rules.neutral ? kBad : kText;
        const float bx = s.x + s.width * 0.5f, bw = s.width * 0.5f - 60;
        DrawRectangleRounded({bx, y + 6, bw, 6}, 0.5f, 4, Fade(WHITE, 0.10f));
        DrawRectangleRounded({bx, y + 6, bw * v / std::max(1, rules.max), 6}, 0.5f, 4, vc);
        std::snprintf(val, sizeof val, "%d", v);
        textRight(val, s.x + s.width - 22, y, 16, vc, true);
    }
    const float by = s.y + s.height - 116;
    row = m.rowOf(Row::TestStats);
    button({s.x + 20, by, s.width - 40, 44}, "Edit car stats", 1, m.row == row, row, hits);
    row = m.rowOf(Row::TestRuns);
    std::snprintf(val, sizeof val, "Saved runs (%d)", m.runsTotal);
    button({s.x + 20, by + 56, s.width - 40, 40}, val, 2, m.row == row, row, hits);
}

void Hud::drawMenu(const MenuState& m, std::vector<MenuHit>& hits) {
    hits.clear();
    const float sw = (float)GetScreenWidth(), sh = (float)GetScreenHeight();
    if (m.seasonPage) drawSeasonPage(m, hits);
    else if (m.gridPage) drawGridPage(m, hits);
    else if (m.teamsPage) drawTeamsPage(m, hits);
    else if (m.testStatsPage) drawTestStatsPage(m, hits);
    else if (m.runsPage) drawRunsPage(m, hits);
    else {
        DrawRectangleGradientV(0, 0, (int)sw, (int)sh, Fade(Color{6, 8, 14, 255}, 0.80f), Fade(Color{6, 8, 14, 255}, 0.55f));
        float top = 0;
        drawTopBar(m, hits, top);
        const float mx = std::max(24.0f, sw * 0.03f);
        Rectangle area = {mx, top, sw - 2 * mx, sh - top - 100};
        if (m.testing()) drawTestSetup(m, hits, area);
        else if (m.champ()) drawChampSetup(m, hits, area);
        else drawRaceSetup(m, hits, area);
        // start
        using Row = MenuState::Row;
        const int row = m.rowOf(Row::Start);
        const char* go = m.testing() ? "START TEST" : m.champ() ? "START CHAMPIONSHIP" : m.weekend() ? "START WEEKEND" : "START RACE";
        const float bw = 360, bh = 60;
        Rectangle b = {sw - mx - bw, sh - 82, bw, bh};
        button(b, go, 0, m.row == row, row, hits);
        if (m.row == row) DrawRectangleRoundedLinesEx({b.x - 4, b.y - 4, b.width + 8, b.height + 8}, rnd(b, 14), 8, 2, kAccent);
        const char* help = m.champ()
            ? "Up/Down choose   Left/Right change   Enter select   Ctrl+Up/Down move a round   Del remove   Ctrl+Q quit"
            : "Up/Down choose   Left/Right change (Shift: bigger steps)   Enter start   Ctrl+Q quit";
        text(help, mx, sh - 58, 14, kDim);
    }
    if (!m.toast.empty() && GetTime() < m.toastUntil) {
        const float w = width(m.toast.c_str(), 16, true) + 40;
        Rectangle t = {(sw - w) / 2, sh - 150, w, 40};
        DrawRectangleRounded(t, 0.5f, 8, Fade(kWellHi, 0.97f));
        text(m.toast.c_str(), t.x + 20, t.y + 11, 16, kText, true);
    }
    if (m.typing()) drawNameBox(m, hits);
    if (m.lineupLoad) drawLineupList(m, hits);
    if (m.popup >= 0) drawPopup(m, hits);
}

// ---------------------------------------------------------------- overlays

void Hud::drawPopup(const MenuState& m, std::vector<MenuHit>& hits) {
    const std::vector<std::string> opts = m.popupOptions();
    const float sh = (float)GetScreenHeight();
    const float rowH = 34;
    const int vis = std::min((int)opts.size(), std::max(3, (int)((sh - 40) / rowH) - 2));
    const int top = std::clamp(std::max(m.popupTop, m.popupSel - vis + 1), 0, std::max(0, (int)opts.size() - vis));
    const float h = vis * rowH + 12;
    float y = m.popupY;
    if (y + h > sh - 10) y = std::max(10.0f, sh - 10 - h);
    Rectangle r = {m.popupX, y, m.popupW, h};
    DrawRectangleRounded({r.x + 3, r.y + 5, r.width, r.height}, rnd(r, 10), 8, Fade(BLACK, 0.4f));
    DrawRectangleRounded(r, rnd(r, 10), 8, Color{26, 30, 41, 255});
    DrawRectangleRoundedLinesEx(r, rnd(r, 10), 8, 1.0f, Fade(WHITE, 0.15f));
    const auto& table = liveryTable();
    const bool liveries = (m.popup >= 100 && (m.popup - 100) % MenuState::kGridCols == 0) ||
                          (m.popup >= 0 && m.popup < 100 && m.rows()[m.popup] == MenuState::Row::TestLivery);
    for (int i = top; i < top + vis; ++i) {
        Rectangle k = {r.x + 6, r.y + 6 + (i - top) * rowH, r.width - 12, rowH - 2};
        if (i == m.popupSel) DrawRectangleRounded(k, rnd(k, 8), 6, Fade(kAccent, 0.22f));
        float x = k.x + 12;
        if (liveries && i < (int)table.size()) {
            DrawRectangleRounded({x, k.y + 8, 5, k.height - 16}, 0.5f, 4, table[i].color);
            x += 14;
        }
        text(opts[i].c_str(), x, k.y + (k.height - 16) / 2 - 1, 16, kText, i == m.popupSel);
        hits.push_back({k.x, k.y, k.width, k.height, 5000 + i, 0});
    }
}

void Hud::drawNameBox(const MenuState& m, std::vector<MenuHit>& hits) {
    const float sw = (float)GetScreenWidth(), sh = (float)GetScreenHeight();
    DrawRectangle(0, 0, (int)sw, (int)sh, Fade(BLACK, 0.5f));
    Rectangle r = {(sw - 560) / 2, (sh - 230) / 2, 560, 230};
    DrawRectangleRounded(r, rnd(r, 14), 8, kCard);
    card(r, m.lineupSave ? "SAVE LINEUP" : "CHAMPIONSHIP NAME");
    text(m.lineupSave ? "Name this lineup" : "Name the championship", r.x + 22, r.y + 42, 24, kText, true);
    Rectangle f = {r.x + 22, r.y + 86, r.width - 44, 50};
    DrawRectangleRounded(f, rnd(f, 10), 8, kWell);
    DrawRectangleRoundedLinesEx(f, rnd(f, 10), 8, 2.0f, kAccent);
    text(m.inputText.c_str(), f.x + 16, f.y + 14, 20, kText, true);
    if (std::fmod(GetTime(), 1.0) < 0.55) {
        const float cx = f.x + 18 + width(m.inputText.c_str(), 20, true);
        DrawRectangle((int)cx, (int)f.y + 12, 2, 26, kAccent);
    }
    if (m.lineupSave) text("Teams, team stats, algorithms, liveries and starting tyres", r.x + 22, r.y + 144, 13, kDim);
    button({r.x + r.width - 22 - 140, r.y + r.height - 60, 140, 42}, m.lineupSave ? "Save" : "OK", 0, false, 6000, hits);
    button({r.x + r.width - 22 - 290, r.y + r.height - 60, 140, 42}, "Cancel", 2, false, 6001, hits);
}

void Hud::drawLineupList(const MenuState& m, std::vector<MenuHit>& hits) {
    const float sw = (float)GetScreenWidth(), sh = (float)GetScreenHeight();
    DrawRectangle(0, 0, (int)sw, (int)sh, Fade(BLACK, 0.5f));
    const int n = (int)m.lineupFiles.size();
    const float rowH = 40;
    const int vis = std::min(n, 10);
    const float h = 150 + std::max(1, vis) * rowH;
    Rectangle r = {(sw - 560) / 2, (sh - h) / 2, 560, h};
    DrawRectangleRounded(r, rnd(r, 14), 8, kCard);
    card(r, "LOAD LINEUP");
    text("Pick a saved lineup", r.x + 22, r.y + 42, 24, kText, true);
    const int top = std::clamp(m.lineupRow - vis + 1, 0, std::max(0, n - vis));
    if (n == 0) text("No saved lineups yet: use Save lineup first.", r.x + 22, r.y + 92, 15, kDim);
    for (int i = top; i < top + vis; ++i) {
        Rectangle k = {r.x + 16, r.y + 84 + (i - top) * rowH, r.width - 32, rowH - 4};
        const bool sel = i == m.lineupRow;
        if (sel || hover(k)) DrawRectangleRounded(k, rnd(k, 8), 6, sel ? Fade(kAccent, 0.22f) : Fade(WHITE, 0.05f));
        text(m.lineupFiles[i].c_str(), k.x + 14, k.y + 9, 17, kText, sel);
        hits.push_back({k.x, k.y, k.width, k.height, 6100 + i, 0});
    }
    button({r.x + r.width - 22 - 140, r.y + r.height - 58, 140, 42}, "Load", 0, false, 6098, hits);
    button({r.x + r.width - 22 - 290, r.y + r.height - 58, 140, 42}, "Cancel", 2, false, 6099, hits);
}

// ---------------------------------------------------------------- grid and team stats pages

void Hud::drawGridPage(const MenuState& m, std::vector<MenuHit>& hits) {
    const float sw = (float)GetScreenWidth(), sh = (float)GetScreenHeight();
    DrawRectangleGradientV(0, 0, (int)sw, (int)sh, Fade(Color{6, 8, 14, 255}, 0.85f), Fade(Color{6, 8, 14, 255}, 0.65f));
    const int n = m.cars;
    const int cols = n > 10 ? 2 : 1;
    const int perCol = (n + cols - 1) / cols;
    const float mx = std::max(24.0f, sw * 0.03f);
    text("SETUP", mx, 26, 15, kAccent, true);
    text("Grid", mx, 46, 34, kText, true);
    text("Click a box to pick a livery, a driving algorithm or the starting tyres.", mx + 120, 62, 15, kDim);
    const float gap = 22;
    const float colW = (sw - 2 * mx - (cols - 1) * gap) / cols;
    const float top = 112, rowH = std::min(52.0f, (sh - top - 120) / std::max(1, perCol));
    const auto& table = liveryTable();
    static const char* tyreNames[] = {"Auto", "Soft", "Medium", "Hard"};
    const int K = MenuState::kGridCols;
    for (int c = 0; c < cols; ++c) {
        Rectangle cr = {mx + c * (colW + gap), top, colW, perCol * rowH + 50};
        card(cr, nullptr);
        const float x0 = cr.x + 16, liveryW = colW * 0.36f, algoW = colW * 0.33f, tyreW = colW - 60 - liveryW - algoW - 32;
        text("LIVERY", x0 + 44, cr.y + 16, 11, kDim, true);
        text("ALGORITHM", x0 + 44 + liveryW + 8, cr.y + 16, 11, kDim, true);
        text("START TYRES", x0 + 44 + liveryW + algoW + 16, cr.y + 16, 11, kDim, true);
        for (int car = c * perCol; car < std::min(n, (c + 1) * perCol); ++car) {
            const float y = cr.y + 38 + (car - c * perCol) * rowH;
            const bool selRow = car == m.gridRow;
            char buf[96];
            std::snprintf(buf, sizeof buf, "%d", car + 1);
            textRight(buf, x0 + 30, y + (rowH - 8 - 17) / 2, 17, selRow ? kAccent : kDim, true);
            const int slot = car < (int)m.carLivery.size() ? m.carLivery[car] : car;
            if (slot >= 0 && slot < (int)table.size())
                std::snprintf(buf, sizeof buf, "#%d  %s", table[slot].number, table[slot].team.c_str());
            else std::snprintf(buf, sizeof buf, "livery %d", slot + 1);
            const float bh = rowH - 8;
            dropdown({x0 + 40, y, liveryW, bh}, buf, selRow && m.gridCol == 0, 100 + car * K, hits, slotColor(slot));
            const int a = car < (int)m.carAlgo.size() ? m.carAlgo[car] : 0;
            dropdown({x0 + 48 + liveryW, y, algoW, bh}, a < (int)m.algos.size() ? m.algos[a].label.c_str() : "?",
                     selRow && m.gridCol == 1, 100 + car * K + 1, hits);
            const int tyre = car < (int)m.carTires.size() ? m.carTires[car] : 0;
            dropdown({x0 + 56 + liveryW + algoW, y, tyreW, bh}, tyreNames[tyre & 3], selRow && m.gridCol == 2,
                     100 + car * K + 2, hits, tyre ? compoundColor(tyre) : BLANK);
        }
    }
    // buttons
    const int pending = m.stylesPending();
    Rectangle sb = {mx, sh - 82, 300, 52};
    button(sb, "Apply style stats (A)", pending ? 1 : 2, false, 94, hits);
    if (pending) DrawRectangleRoundedLinesEx(sb, rnd(sb, 10), 8, 2.0f, kAccent);
    char buf[128];
    if (pending) std::snprintf(buf, sizeof buf, "%d team%s changed algorithm: their stats are not applied yet", pending,
                               pending == 1 ? "" : "s");
    else std::snprintf(buf, sizeof buf, "Changing an algorithm keeps the team's stats.");
    text(buf, sb.x + sb.width + 18, sb.y + 17, 15, pending ? kAccent : kDim);
    button({sw - mx - 220, sh - 82, 220, 52}, "Done", 0, false, 99, hits);
    text("Up/Down car   Tab column   Left/Right change   Space open list   Enter done", mx, sh - 22, 13, kDim);
}

void Hud::drawTeamsPage(const MenuState& m, std::vector<MenuHit>& hits) {
    const float sw = (float)GetScreenWidth(), sh = (float)GetScreenHeight();
    DrawRectangleGradientV(0, 0, (int)sw, (int)sh, Fade(Color{6, 8, 14, 255}, 0.85f), Fade(Color{6, 8, 14, 255}, 0.65f));
    const std::vector<int> teams = m.raceTeams();
    const StatRules& r = m.statRules;
    const int ns = (int)r.keys.size();
    const float mx = std::max(24.0f, sw * 0.03f);
    text("SETUP", mx, 26, 15, kAccent, true);
    text("Team stats", mx, 46, 34, kText, true);
    char about[256];
    std::snprintf(about, sizeof about, "Each stat 0-%d (%d is the stock car), %d points per team. Teammates share them.",
                  r.max, r.neutral, r.budget);
    text(about, mx + 210, 62, 15, kDim);
    const float top = 112;
    const float rowH = std::min(56.0f, (sh - top - 160) / std::max<size_t>(1, teams.size()));
    Rectangle cr = {mx, top, sw - 2 * mx, rowH * teams.size() + 56};
    card(cr, nullptr);
    const float nameW = 240, leftW = 80;
    const float cellW = (cr.width - 32 - nameW - leftW) / std::max(1, ns);
    const float hy = cr.y + 16;
    for (int k = 0; k < ns; ++k) {
        const char* label = r.labels[k].c_str();
        float size = 12;
        while (size > 9 && width(label, size, true) > cellW - 8) size -= 1;
        text(label, cr.x + 16 + nameW + k * cellW + (cellW - width(label, size, true)) / 2, hy, size, kDim, true);
    }
    textRight("POINTS LEFT", cr.x + cr.width - 16, hy, 11, kDim, true);
    const auto& table = liveryTable();
    char buf[64];
    for (int i = 0; i < (int)teams.size(); ++i) {
        const int t = teams[i];
        const float ry = cr.y + 40 + i * rowH;
        const bool selRow = i == m.teamRow;
        if (selRow) DrawRectangleRounded({cr.x + 8, ry, cr.width - 16, rowH - 4}, rnd({0, 0, 100, rowH}, 8), 6, Fade(WHITE, 0.06f));
        const int slot = t < (int)m.teamSlots.size() && !m.teamSlots[t].empty() ? m.teamSlots[t][0] : -1;
        DrawRectangleRounded({cr.x + 20, ry + 8, 5, rowH - 20}, 0.5f, 4, slotColor(slot));
        text(slot >= 0 && slot < (int)table.size() ? table[slot].team.c_str() : "team", cr.x + 34, ry + (rowH - 4 - 17) / 2,
             17, kText, true);
        const std::vector<int>& v = m.teamStats[t];
        for (int k = 0; k < ns; ++k) {
            const float cx = cr.x + 16 + nameW + k * cellW;
            const bool sel = selRow && k == m.statCol;
            Rectangle cell = {cx + 4, ry + 4, cellW - 8, rowH - 12};
            DrawRectangleRounded(cell, rnd(cell, 8), 6, sel ? kWellHi : kWell);
            if (sel) DrawRectangleRoundedLinesEx(cell, rnd(cell, 8), 6, 2.0f, kAccent);
            const int val = k < (int)v.size() ? v[k] : r.neutral;
            const Color vc = val > r.neutral ? kGood : val < r.neutral ? kBad : kText;
            std::snprintf(buf, sizeof buf, "%d", val);
            text(buf, cell.x + (cell.width - width(buf, 18, true)) / 2, cell.y + 4, 18, vc, true);
            const float bw = cell.width - 56, bx = cell.x + 28;
            DrawRectangle((int)bx, (int)(cell.y + cell.height - 9), (int)bw, 3, Fade(WHITE, 0.12f));
            DrawRectangle((int)bx, (int)(cell.y + cell.height - 9), (int)(bw * val / std::max(1, r.max)), 3, vc);
            // minus and plus
            for (int side = 0; side < 2; ++side) {
                Rectangle k2 = {side ? cell.x + cell.width - 26 : cell.x + 2, cell.y + 2, 24, cell.height - 4};
                const bool hot = hover(k2);
                const float ccx = k2.x + 12, ccy = k2.y + k2.height / 2;
                const Color ic = hot ? kAccent : sel ? kText : kFaint;
                DrawRectangle((int)ccx - 5, (int)ccy - 1, 10, 2, ic);
                if (side) DrawRectangle((int)ccx - 1, (int)ccy - 5, 2, 10, ic);
                hits.push_back({k2.x, k2.y, k2.width, k2.height, 1000 + t * 16 + k, side ? 1 : -1});
            }
        }
        std::snprintf(buf, sizeof buf, "%d", r.budget - m.statSum(t));
        textRight(buf, cr.x + cr.width - 24, ry + (rowH - 4 - 18) / 2, 18, r.budget - m.statSum(t) > 0 ? kAccent : kDim, true);
    }
    button({sw - mx - 220, sh - 82, 220, 52}, "Done", 0, false, 99, hits);
    text("Up/Down team   Tab next stat   Left/Right change (lower one stat to raise another)   Enter done", mx, sh - 58, 14,
         kDim);
}

// ---------------------------------------------------------------- the season page

void Hud::drawSeasonPage(const MenuState& m, std::vector<MenuHit>& hits) {
    const float sw = (float)GetScreenWidth(), sh = (float)GetScreenHeight();
    DrawRectangleGradientV(0, 0, (int)sw, (int)sh, Fade(Color{6, 8, 14, 255}, 0.88f), Fade(Color{6, 8, 14, 255}, 0.7f));
    const rr::Championship* c = m.season;
    if (!c) return;
    const rr::Lineup& L = c->lineup;
    const float mx = std::max(24.0f, sw * 0.03f);
    char buf[200];
    text("CHAMPIONSHIP", mx, 26, 15, kAccent, true);
    text(c->name.c_str(), mx, 46, 34, kText, true);
    const bool over = c->over();
    if (over) std::snprintf(buf, sizeof buf, "Finished after %d rounds", (int)c->rounds.size());
    else std::snprintf(buf, sizeof buf, "Round %d of %d", c->roundsDone() + 1, (int)c->rounds.size());
    text(buf, mx + width(c->name.c_str(), 34, true) + 24, 60, 18, over ? kGood : kDim, true);
    segmented({sw - mx - 380, 34, 380, 46}, {"Drivers", "Constructors"}, m.seasonTab, false, -1, hits);
    for (MenuHit& h : hits)
        if (h.row == -1) h.row = 7010 + h.value;

    const float top = 108, gap = 22;
    const float calW = std::min(460.0f, (sw - 2 * mx) * 0.32f);
    Rectangle tr = {mx, top, sw - 2 * mx - calW - gap, sh - top - 100};
    const int nr = (int)c->rounds.size();
    auto trackTitle = [&](int k) {
        const TrackStats* t = m.trackStats(c->rounds[k].track);
        return t ? t->title : c->rounds[k].track;
    };
    // ---- standings
    const bool teams = m.seasonTab == 1;
    const std::vector<rr::Standing> st = teams ? c->teamStandings() : c->driverStandings();
    card(tr, teams ? "CONSTRUCTORS" : "DRIVERS", over ? "final standings" : "standings");
    const float rowH = std::clamp((tr.height - 80) / std::max<size_t>(1, st.size()), 22.0f, 34.0f);
    const float ptsX = tr.x + tr.width - 24;
    const float nameW = teams ? 260 : 330;
    const float rx0 = tr.x + 60 + nameW + 20;
    const float rw = std::max(26.0f, std::min(48.0f, (ptsX - 80 - rx0) / std::max(1, nr)));
    float y = tr.y + 48;
    for (int k = 0; k < nr; ++k) {
        std::string ab = trackTitle(k).substr(0, 3);
        for (char& ch : ab) ch = (char)std::toupper((unsigned char)ch);
        text(ab.c_str(), rx0 + k * rw + (rw - width(ab.c_str(), 11, true)) / 2, y, 11, k < c->roundsDone() ? kDim : kFaint, true);
    }
    textRight("PTS", ptsX, y, 11, kDim, true);
    y += 22;
    const auto& table = liveryTable();
    for (size_t p = 0; p < st.size() && y + rowH <= tr.y + tr.height - 8; ++p, y += rowH) {
        const rr::Standing& s = st[p];
        if (p % 2 == 0) DrawRectangle((int)tr.x + 10, (int)y, (int)tr.width - 20, (int)rowH, Fade(WHITE, 0.025f));
        std::snprintf(buf, sizeof buf, "%zu", p + 1);
        const Color pc = p == 0 && c->roundsDone() ? kAccent : kText;
        textRight(buf, tr.x + 44, y + (rowH - 17) / 2, 17, pc, true);
        int slot = -1;
        std::string name, sub;
        if (teams) {
            slot = L.teams[s.id].livery;
            if (slot < 0 && !L.teams[s.id].drivers.empty()) slot = L.teams[s.id].drivers[0].livery;
            name = L.teams[s.id].name;
        } else {
            slot = L.driver(s.id).livery;
            name = driverLabel(slot, L.driver(s.id).name);
            const int t = L.teamOf(s.id);
            if (t >= 0) sub = L.teams[t].name;
        }
        DrawRectangleRounded({tr.x + 54, y + 5, 5, rowH - 10}, 0.5f, 4, slotColor(slot));
        float sz = std::min(17.0f, rowH * 0.55f);
        text(name.c_str(), tr.x + 68, y + (rowH - sz) / 2 - 1, sz, kText, true);
        if (!sub.empty()) {
            const float nx = tr.x + 68 + width(name.c_str(), sz, true) + 12;
            if (nx + 60 < rx0) {
                std::string sb = sub;
                while (sb.size() > 3 && nx + width(sb.c_str(), 13) > rx0 - 10) sb.pop_back();
                text(sb.c_str(), nx, y + (rowH - 13) / 2, 13, kDim);
            }
        }
        // per-round finishing places (drivers) or points (constructors)
        for (int k = 0; k < c->roundsDone(); ++k) {
            const float cx = rx0 + k * rw;
            int place = 0, pts = 0;
            if (teams) {
                for (int d = 0; d < L.driverCount(); ++d)
                    if (L.teamOf(d) == s.id) pts += c->roundPoints(k, d);
                std::snprintf(buf, sizeof buf, "%d", pts);
            } else {
                place = c->roundPlace(k, s.id);
                pts = c->roundPoints(k, s.id);
                const auto& r = c->results[k];
                const bool out = place > 0 && r.status[place - 1].rfind("dnf", 0) == 0;
                if (out) std::snprintf(buf, sizeof buf, "DNF");
                else std::snprintf(buf, sizeof buf, "%d", place);
            }
            Color bg = BLANK;
            if (!teams && place >= 1 && place <= 3) bg = place == 1 ? Color{212, 175, 55, 255} : place == 2 ? Color{170, 176, 186, 255} : Color{176, 112, 64, 255};
            else if (pts > 0) bg = Fade(kGood, 0.25f);
            Rectangle cell = {cx + 2, y + 3, rw - 4, rowH - 6};
            if (bg.a) DrawRectangleRounded(cell, 0.3f, 4, bg);
            const float fs = std::min(13.0f, rowH * 0.45f);
            text(buf, cell.x + (cell.width - width(buf, fs, true)) / 2, cell.y + (cell.height - fs) / 2 - 1, fs,
                 !teams && place >= 1 && place <= 3 ? kInk : pts > 0 ? kText : kDim, true);
        }
        std::snprintf(buf, sizeof buf, "%d", s.points);
        textRight(buf, ptsX, y + (rowH - 18) / 2, 18, pc, true);
    }
    (void)table;

    // ---- calendar
    Rectangle cal = {tr.x + tr.width + gap, top, calW, sh - top - 100};
    card(cal, "CALENDAR");
    const float ch = std::min(62.0f, (cal.height - 56) / std::max(1, nr));
    y = cal.y + 44;
    for (int k = 0; k < nr; ++k, y += ch) {
        const bool done = k < c->roundsDone(), next = k == c->roundsDone();
        Rectangle line = {cal.x + 12, y, cal.width - 24, ch - 4};
        if (next) {
            DrawRectangleRounded(line, rnd(line, 8), 6, Fade(kAccent, 0.14f));
            DrawRectangleRounded({line.x, line.y + 6, 3, line.height - 12}, 0.5f, 4, kAccent);
        }
        const TrackStats* t = m.trackStats(c->rounds[k].track);
        std::snprintf(buf, sizeof buf, "%d", k + 1);
        textRight(buf, line.x + 26, line.y + (line.height - 16) / 2, 16, next ? kAccent : kDim, true);
        if (t) trackShape(t->outline, {line.x + 34, line.y + 5, 40, line.height - 10}, done || next ? kText : kFaint, 1.5f);
        const float fs = std::min(16.0f, ch * 0.3f);
        text(trackTitle(k).c_str(), line.x + 84, line.y + 5, fs, done || next ? kText : kDim, true);
        std::string sub = std::to_string(c->rounds[k].laps) + " laps";
        if (done && !c->results[k].order.empty()) {
            const int w = c->results[k].order[0];
            sub += "   winner " + driverLabel(L.driver(w).livery, L.driver(w).name);
        } else if (next) {
            sub += "   next";
        }
        text(sub.c_str(), line.x + 84, line.y + 5 + fs + 4, std::min(13.0f, ch * 0.25f), next ? kAccent : kDim);
    }

    // ---- buttons
    button({mx, sh - 82, 220, 52}, "Back to menu", 2, false, 7001, hits);
    if (over) {
        const std::vector<rr::Standing> d = c->driverStandings(), cs = c->teamStandings();
        if (!d.empty() && !cs.empty()) {
            std::snprintf(buf, sizeof buf, "Champion: %s   Constructors: %s",
                          driverLabel(L.driver(d[0].id).livery, L.driver(d[0].id).name).c_str(), L.teams[cs[0].id].name.c_str());
            textRight(buf, sw - mx, sh - 66, 22, kAccent, true);
        }
    } else {
        std::snprintf(buf, sizeof buf, "START ROUND %d: %s", c->roundsDone() + 1, trackTitle(c->roundsDone()).c_str());
        button({sw - mx - 460, sh - 82, 460, 60}, buf, 0, true, 7000, hits);
    }
    text("Enter start round   Tab drivers / constructors   Esc back", mx + 240, sh - 62, 14, kDim);
}
