#include "hud.hpp"

#include "liveries.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>

namespace {

const Color kText = {238, 240, 245, 255};
const Color kDim = {160, 166, 180, 255};
const Color kAccent = {255, 196, 40, 255};
const Color kBlueFlag = {40, 110, 255, 255};

}  // namespace

std::string lapTime(double t) {
    if (t <= 0) return "-:--.---";
    int m = (int)(t / 60);
    char buf[32];
    std::snprintf(buf, sizeof buf, "%d:%06.3f", m, t - m * 60);
    return buf;
}

namespace {

Font loadFont(const std::string& path, int size) {
    if (!std::filesystem::exists(path)) return GetFontDefault();
    Font f = LoadFontEx(path.c_str(), size, nullptr, 0);
    if (f.texture.id == 0) return GetFontDefault();
    GenTextureMipmaps(&f.texture);
    SetTextureFilter(f.texture, TEXTURE_FILTER_TRILINEAR);
    return f;
}

}  // namespace

void Hud::init(const rr::Track& track, const std::string& assetsDir) {
    std::string dir = assetsDir + "/fonts/";
    regular_ = loadFont(dir + "DejaVuSans.ttf", 48);
    bold_ = loadFont(dir + "DejaVuSans-Bold.ttf", 64);
    mono_ = loadFont(dir + "DejaVuSansMono.ttf", 48);
    ownFonts_ = true;

    outline_.clear();
    minX_ = minY_ = 1e9f;
    maxX_ = maxY_ = -1e9f;
    for (int i = 0; i < track.size(); i += 4) {
        rr::Vec2 p = track.at(i).p;
        outline_.push_back(p);
        minX_ = std::min(minX_, p.x); maxX_ = std::max(maxX_, p.x);
        minY_ = std::min(minY_, p.y); maxY_ = std::max(maxY_, p.y);
    }
    trackWidth_ = track.at(0).halfWidth * 2;
}

void Hud::shutdown() {
    if (!ownFonts_) return;
    for (Font* f : {&regular_, &bold_, &mono_})
        if (f->texture.id != GetFontDefault().texture.id) UnloadFont(*f);
}

void Hud::text(const char* s, float x, float y, float size, Color c, bool bold, bool mono) {
    const Font& f = mono ? mono_ : (bold ? bold_ : regular_);
    DrawTextEx(f, s, {std::round(x), std::round(y)}, size, 0, c);
}

float Hud::width(const char* s, float size, bool bold, bool mono) {
    const Font& f = mono ? mono_ : (bold ? bold_ : regular_);
    return MeasureTextEx(f, s, size, 0).x;
}

void Hud::textRight(const char* s, float right, float y, float size, Color c, bool bold, bool mono) {
    text(s, right - width(s, size, bold, mono), y, size, c, bold, mono);
}

void Hud::panel(Rectangle r, float alpha) {
    DrawRectangleRounded(r, 8.0f / std::min(r.width, r.height), 8, Fade(Color{12, 14, 20, 255}, alpha));
}

void Hud::draw(const rr::Race& race, const HudState& st) {
    // the race-end windows take the screen; G hides them to show the tower again
    const bool results = race.isOver() && st.showResults && !st.qualifying;
    if (st.showHud) {
        if (st.qualifying) drawQualiTower(race, st);
        else if (!results) drawTower(race, st);
        if (!results) {
            drawMinimap(race, st);
            drawCarPanel(race, st);
        }
        const char* hint = "F1 help   Tab next car   L follow leader   C camera   M sound   Space pause   +/- speed   Esc menu";
        text(hint, 18, GetScreenHeight() - 30.0f, 16, Fade(kText, 0.75f));
        // camera and focus mode, top centre
        char buf[96];
        if (st.qualifying)
            std::snprintf(buf, sizeof buf, "%s  RUN %d / %d   %s CAM   Enter: skip run   Shift+Enter: skip the rest%s",
                          st.sessionTitle.c_str(), st.qualiRun, st.qualiRuns, camName(st.camera), st.muted ? "   SOUND OFF" : "");
        else if (st.camera == CAM_DIRECTOR)
            std::snprintf(buf, sizeof buf, "DIRECTOR CAM   %s%s", st.directorCaption.c_str(), st.muted ? "   SOUND OFF" : "");
        else
            std::snprintf(buf, sizeof buf, "%s CAM   %s%s", camName(st.camera), st.followLeader ? "FOLLOWING LEADER" : "CAR SELECTED",
                          st.muted ? "   SOUND OFF" : "");
        float w = width(buf, 15, true) + 28;
        float x = (GetScreenWidth() - w) / 2;
        panel({x, 12, w, 30});
        text(buf, x + 14, 18, 15, st.followLeader ? kAccent : kText, true);
    }
    if (st.lights > -1.5f) {
        // Start lights: one red light a second, then all out and GO.
        const int on = st.lights > 0 ? std::min(5, (int)(6.0f - st.lights)) : 0;
        const float r = 22, gap = 16, w = 5 * (2 * r) + 4 * gap + 40;
        const float x = (GetScreenWidth() - w) / 2, y = GetScreenHeight() * 0.16f;
        panel({x, y, w, 2 * r + 40});
        for (int i = 0; i < 5; ++i) {
            const Vector2 c = {x + 20 + r + i * (2 * r + gap), y + 20 + r};
            DrawCircleV(c, r, Color{30, 30, 34, 255});
            if (i < on) DrawCircleV(c, r - 3, Color{235, 30, 30, 255});
        }
        if (st.lights <= 0) {
            const char* go = "GO";
            text(go, (GetScreenWidth() - width(go, 44, true)) / 2, y + 2 * r + 52, 44, Color{80, 220, 110, 255}, true);
        }
    }
    if (st.paused && !race.isOver()) {
        const char* p = "PAUSED";
        float w = width(p, 44, true);
        text(p, (GetScreenWidth() - w) / 2, GetScreenHeight() * 0.2f, 44, kText, true);
    }
    if (results) drawResults(race, st);
    else resultTabs_.clear(), resultRows_.clear(), resultsBox_ = {};
    if (race.isOver() && !st.notice.empty()) {
        const float w = width(st.notice.c_str(), 17, true) + 44;
        Rectangle r = {(GetScreenWidth() - w) / 2, GetScreenHeight() - 96.0f, w, 42};
        DrawRectangleRounded(r, 0.5f, 8, kAccent);
        text(st.notice.c_str(), r.x + 22, r.y + 11, 17, Color{20, 20, 24, 255}, true);
    }
    if (st.showHelp) drawHelp();
}

// Blue cold, green inside the window, amber just over it, red hot.
Color tempColor(float t, float lo, float hi, float amber) {
    return t < lo ? Color{110, 170, 240, 255}
         : t <= hi ? Color{90, 200, 120, 255}
         : t <= hi + amber ? kAccent : Color{240, 70, 60, 255};
}

Color compoundColor(int compound) {
    return compound == RR_TIRE_SOFT ? Color{230, 50, 50, 255}
         : compound == RR_TIRE_HARD ? Color{235, 235, 240, 255}
                                    : Color{245, 200, 30, 255};
}

const char* compoundName(int compound) {
    return compound == RR_TIRE_SOFT ? "SOFT" : compound == RR_TIRE_HARD ? "HARD" : "MEDIUM";
}

// Timing tower layout, shared by drawing and clicking.
static const float kTowerX = 16, kTowerW = 400, kTowerRowH = 28, kTowerTop = 112;

int Hud::towerCarAt(const rr::Race& race, const HudState& st, Vector2 p) const {
    if (!st.showHud || st.qualifying) return -1;
    if (race.isOver() && st.showResults) return -1;  // hidden under the race-end window
    if (p.x < kTowerX || p.x > kTowerX + kTowerW || p.y < kTowerTop - 3) return -1;
    const int row = (int)((p.y - (kTowerTop - 3)) / kTowerRowH);
    return row < (int)race.order().size() ? race.order()[row] : -1;
}

void Hud::drawTower(const rr::Race& race, const HudState& st) {
    const auto& cars = race.cars();
    const auto& order = race.order();
    const rr::Car& leader = cars[order[0]];
    const float x = kTowerX, w = kTowerW, rowH = kTowerRowH;
    const int hover = towerCarAt(race, st, GetMousePosition());
    float h = 104 + rowH * cars.size() + 8;
    panel({x, 16, w, h});

#ifdef RR2_RENDERER
    text("RAYLIB RACERS 2", x + 16, 26, 15, kAccent, true);
#else
    text("RAYLIB RACERS", x + 16, 26, 15, kAccent, true);
#endif
    text(race.track().name().c_str(), x + 16, 46, 19, kText);
    char buf[96];
    std::snprintf(buf, sizeof buf, "LAP %d / %d", leader.currentLap(race.laps()), race.laps());
    text(buf, x + 16, 72, 26, kText, true);
    textRight(lapTime(race.time()).c_str(), x + w - 16, 76, 21, kText, false, true);
    if (st.paused) std::snprintf(buf, sizeof buf, "paused");
    else std::snprintf(buf, sizeof buf, "x%g", st.timeScale);
    textRight(buf, x + w - 16, 50, 16, kDim, false, true);

    float y = kTowerTop;
    for (size_t p = 0; p < order.size(); ++p) {
        int idx = order[p];
        const rr::Car& c = cars[idx];
        if (idx == st.focus) DrawRectangle((int)x + 6, (int)y - 3, (int)w - 12, (int)rowH - 2, Fade(WHITE, 0.12f));
        else if (idx == hover) DrawRectangle((int)x + 6, (int)y - 3, (int)w - 12, (int)rowH - 2, Fade(WHITE, 0.06f));
        if (c.blueCar >= 0) DrawRectangle((int)x + 6, (int)y - 3, (int)w - 12, (int)rowH - 2, Fade(kBlueFlag, 0.5f));
        std::snprintf(buf, sizeof buf, "%zu", p + 1);
        textRight(buf, x + 38, y, 19, kText, true);
        DrawRectangle((int)x + 46, (int)y + 1, 5, 19, teamColor(idx));
        {  // shrink long names to fit before the tyre column
            const float room = w - 170 - 12 - 60;
            float size = 19;
            while (size > 13 && width(c.name.c_str(), size) > room) size -= 1;
            text(c.name.c_str(), x + 60, y + (19 - size) * 0.5f, size, kText);
        }
        std::string gap;
        if (p == 0) gap = c.finished ? "FINISH" : "LEADER";
        else if (c.dnf) gap = "DNF";
        else if (c.lapsBehind > 0 && !c.finished) gap = "+" + std::to_string(c.lapsBehind) + (c.lapsBehind > 1 ? " LAPS" : " LAP");
        else if (c.gap < 0) gap = "-";
        else {
            std::snprintf(buf, sizeof buf, "+%.3f", c.gap);
            gap = buf;
        }
        if (c.finished && p > 0) gap = gap + " F";
        if (c.penalties > 0 && !c.dnf) {
            std::snprintf(buf, sizeof buf, "+%.0fs ", c.penaltyTime);
            gap = buf + gap;
        }
        Color gapCol = c.dnf ? Color{230, 90, 80, 255} : kDim;
        if (c.pitState != RR_PIT_NONE && !c.finished && !c.dnf) {
            gap = c.pitState == RR_PIT_SERVICE ? "IN BOX" : "PIT";
            gapCol = kAccent;
        }
        textRight(gap.c_str(), x + w - 16, y + 1, 17, gapCol, false, true);
        // tyre compound and its age in laps
        float cx = x + w - 170, cy = y + 11;
        DrawCircleV({cx, cy}, 8.5f, compoundColor(c.state.compound));
        DrawCircleV({cx, cy}, 5.5f, Color{25, 25, 30, 255});
        std::snprintf(buf, sizeof buf, "%d", c.lapsOnTires);
        text(buf, cx + 12, y + 2, 14, kDim, false, true);
        if (c.pitStops > 0) {
            std::snprintf(buf, sizeof buf, "%d", c.pitStops);
            textRight(buf, cx - 13, y + 2, 14, kDim, false, true);
        }
        y += rowH;
    }
}

void Hud::drawMinimap(const rr::Race& race, const HudState& st) {
    const float box = 270, pad = 18;
    float sx = maxX_ - minX_, sy = maxY_ - minY_;
    float scale = (box - 2 * pad) / std::max(sx, sy);
    float w = sx * scale + 2 * pad, h = sy * scale + 2 * pad;
    float x0 = GetScreenWidth() - w - 16, y0 = 16;
    panel({x0, y0, w, h});
    auto P = [&](rr::Vec2 p) { return Vector2{x0 + pad + (p.x - minX_) * scale, y0 + pad + (maxY_ - p.y) * scale}; };
    float thick = std::max(3.0f, trackWidth_ * scale);
    for (size_t i = 0; i < outline_.size(); ++i) {
        Vector2 a = P(outline_[i]), b = P(outline_[(i + 1) % outline_.size()]);
        DrawLineEx(a, b, thick + 2, Fade(BLACK, 0.5f));
    }
    for (size_t i = 0; i < outline_.size(); ++i) {
        Vector2 a = P(outline_[i]), b = P(outline_[(i + 1) % outline_.size()]);
        DrawLineEx(a, b, thick, Color{120, 125, 135, 255});
    }
    // start line tick
    rr::Vec2 s0 = race.track().at(0).p, n0 = race.track().at(0).n;
    DrawLineEx(P(s0 + n0 * 12.0f), P(s0 - n0 * 12.0f), 2, kText);

    const auto& cars = race.cars();
    for (int pass = 0; pass < 2; ++pass)
        for (size_t i = 0; i < cars.size(); ++i) {
            bool focus = (int)i == st.focus;
            if ((pass == 1) != focus) continue;  // focused car on top
            Vector2 c = P(cars[i].state.pos);
            if (focus) DrawCircleV(c, 8, kText);
            DrawCircleV(c, focus ? 6 : 5, teamColor((int)i));
        }
}

void Hud::drawCarPanel(const rr::Race& race, const HudState& st, float atX, float atY) {
    const rr::Car& c = race.cars()[st.focus];
    // cars with KERS or DRS get one more row, added above so the panel's bottom stays put
    const bool hasKers = c.phys.kersPower > 0, hasDrs = c.phys.drsDragScale < 1.0f;
    const float extra = hasKers || hasDrs ? 28.0f : 0.0f;
    const float w = 380, h = 296 + extra;
    const float x = atX >= 0 ? atX : GetScreenWidth() - w - 16, y = (atY >= 0 ? atY : GetScreenHeight() - h - 16) - (atY >= 0 ? extra : 0.0f);
    panel({x, y, w, h});
    char buf[128];

    // flags above the panel: blue flag, penalties
    float fy = y - 38;
    if (c.blueCar >= 0) {
        DrawRectangleRounded({x, fy, w, 30}, 0.3f, 6, kBlueFlag);
        std::snprintf(buf, sizeof buf, "BLUE FLAG   let %s by", race.cars()[c.blueCar].name.c_str());
        text(buf, x + 14, fy + 6, 17, WHITE, true);
        fy -= 36;
    }
    if (c.penalties > 0) {
        panel({x, fy, w, 30}, 0.8f);
        std::snprintf(buf, sizeof buf, "PENALTY  +%.0f s  (%d)", c.penaltyTime, c.penalties);
        text(buf, x + 14, fy + 6, 17, Color{240, 110, 70, 255}, true);
    }

    DrawRectangle((int)x + 14, (int)y + 16, 5, 40, teamColor(st.focus));
    text(c.name.c_str(), x + 28, y + 12, 22, kText, true);
    const auto& lt = liveryTable();
    const std::string team = lt.empty() ? std::string() : lt[carLivery(st.focus)].team + "   ";
    std::snprintf(buf, sizeof buf, "%s%s%s%s", team.c_str(), c.robotName.c_str(), c.params.empty() ? "" : "  ", c.params.c_str());
    {  // team, algorithm and parameters, smaller when long
        float size = 15;
        while (size > 11 && width(buf, size) > w - 44) size -= 1;
        text(buf, x + 28, y + 38, size, kDim);
    }
    std::snprintf(buf, sizeof buf, "P%d", c.position);
    textRight(buf, x + w - 16, y + 12, 30, kAccent, true);

    // speed and gear
    float speed = rr::length(c.state.velWorld()) * 3.6f;
    std::snprintf(buf, sizeof buf, "%.0f", speed);
    textRight(buf, x + 150, y + 66, 54, kText, true);
    text("km/h", x + 156, y + 96, 15, kDim);
    std::snprintf(buf, sizeof buf, "%s", c.state.gear < 0 ? "R" : (c.state.gear == 0 ? "N" : std::to_string(c.state.gear).c_str()));
    text("GEAR", x + 212, y + 72, 13, kDim);
    text(buf, x + 214, y + 86, 30, kAccent, true);

    // rpm bar
    float rpmFrac = std::clamp(c.state.rpm / c.phys.maxRpm, 0.0f, 1.0f);
    Rectangle rb = {x + 16, y + 128, 230, 8};
    DrawRectangleRec(rb, Fade(WHITE, 0.12f));
    Color rpmCol = rpmFrac > 0.92f ? Color{240, 70, 60, 255} : (rpmFrac > 0.8f ? kAccent : Color{90, 200, 120, 255});
    DrawRectangleRec({rb.x, rb.y, rb.width * rpmFrac, rb.height}, rpmCol);

    // pedals and steering
    const RRControl& k = c.control;
    float px = x + 268, ph = 64, py = y + 66;
    DrawRectangle((int)px, (int)py, 14, (int)ph, Fade(WHITE, 0.12f));
    DrawRectangle((int)px, (int)(py + ph * (1 - k.accel)), 14, (int)(ph * k.accel), Color{80, 210, 110, 255});
    DrawRectangle((int)px + 20, (int)py, 14, (int)ph, Fade(WHITE, 0.12f));
    DrawRectangle((int)px + 20, (int)(py + ph * (1 - k.brake)), 14, (int)(ph * k.brake), Color{235, 70, 60, 255});
    float sxc = px + 70, sw = 34;
    DrawRectangle((int)(sxc - sw), (int)py + 26, (int)(2 * sw), 12, Fade(WHITE, 0.12f));
    float steer = std::clamp(k.steer, -1.0f, 1.0f);  // + = left: draw towards the left
    float s0 = sxc, s1 = sxc - steer * sw;
    DrawRectangle((int)std::min(s0, s1), (int)py + 26, (int)std::max(2.0f, std::fabs(s1 - s0)), 12, kAccent);
    text("THR  BRK   STEER", px - 2, py + ph + 4, 11, kDim);

    // laps
    float ly = y + 146;
    std::snprintf(buf, sizeof buf, "LAP %s", lapTime(st.lapClock >= 0 ? st.lapClock : race.time() - c.lapStart).c_str());
    if (c.finished && st.lapClock < 0) std::snprintf(buf, sizeof buf, "FINISHED %s", lapTime(c.finishTime).c_str());
    text(buf, x + 16, ly, 15, kText, false, true);
    std::snprintf(buf, sizeof buf, "LAST %s",
                  lapTime(st.lastLap >= 0 ? st.lastLap : c.lapTimes.empty() ? 0 : c.lapTimes.back()).c_str());
    text(buf, x + 196, ly, 15, kDim, false, true);
    std::snprintf(buf, sizeof buf, "BEST %s", lapTime(st.bestLap >= 0 ? st.bestLap : c.bestLap).c_str());
    text(buf, x + 196, ly + 20, 15, kAccent, false, true);
    std::snprintf(buf, sizeof buf, "%s", k.status[0] ? k.status : "");
    text(buf, x + 16, ly + 20, 14, kDim);
    // consumables
    float cy = y + 196;
    text("FUEL", x + 16, cy, 13, kDim);
    float fuelFrac = std::clamp(c.state.fuel / c.phys.fuelCapacity, 0.0f, 1.0f);
    Rectangle fb = {x + 60, cy + 3, 120, 9};
    DrawRectangleRec(fb, Fade(WHITE, 0.12f));
    DrawRectangleRec({fb.x, fb.y, fb.width * fuelFrac, fb.height},
                     fuelFrac < 0.1f ? Color{240, 70, 60, 255} : Color{90, 170, 235, 255});
    std::snprintf(buf, sizeof buf, "%.1f L", c.state.fuel);
    text(buf, x + 188, cy - 1, 14, kText, false, true);
    if (c.pitStops > 0) {
        std::snprintf(buf, sizeof buf, "STOPS %d", c.pitStops);
        textRight(buf, x + w - 16, cy - 1, 14, kDim, false, true);
    }
    float ty = cy + 22;
    DrawCircleV({x + 22, ty + 7}, 7, compoundColor(c.state.compound));
    DrawCircleV({x + 22, ty + 7}, 4.5f, Color{25, 25, 30, 255});
    text(compoundName(c.state.compound), x + 34, ty, 13, kDim);
    const rr::Compound& comp = rr::compoundInfo(c.state.compound);
    for (int axle = 0; axle < 2; ++axle) {
        float wear = c.state.tireWear[axle];
        float bx = x + 96 + axle * 104;
        text(axle == 0 ? "F" : "R", bx, ty, 13, kDim);
        Rectangle wb = {bx + 14, ty + 3, 34, 9};
        DrawRectangleRec(wb, Fade(WHITE, 0.12f));
        Color wc = wear > 0.7f ? Color{240, 70, 60, 255} : (wear > 0.45f ? kAccent : Color{90, 200, 120, 255});
        DrawRectangleRec({wb.x, wb.y, wb.width * (1 - wear), wb.height}, wc);  // tyre life left
        // each tyre's temperature (left, right) against the compound's window
        for (int side = 0; side < 2; ++side) {
            const float t = c.state.wheelTemp[2 * axle + side];
            std::snprintf(buf, sizeof buf, "%.0f", t);
            text(buf, bx + 54 + side * 26, ty - 1, 14, tempColor(t, comp.tempLo, comp.tempHi, 10), false, true);
        }
    }
    // brake discs: FL FR RL RR
    const float by = ty + 20;
    text("BRAKES", x + 16, by, 13, kDim);
    for (int wh = 0; wh < 4; ++wh) {
        const float t = c.state.brakeTemp[wh];
        static const char* kWheel[4] = {"FL", "FR", "RL", "RR"};
        std::snprintf(buf, sizeof buf, "%s %.0f", kWheel[wh], t);
        text(buf, x + 80 + wh * 70, by - 1, 13, tempColor(t, c.phys.brakeTempLo, c.phys.brakeTempHi, 100), false, true);
    }
    std::snprintf(buf, sizeof buf, "%d laps", c.lapsOnTires);
    textRight(buf, x + w - 16, ty - 1, 14, kDim, false, true);
    // strategy: the algorithm's next planned stop, and the two-compound rule
    float sy = ty + 44;
    if (k.pit_window[0] > 0 && !c.finished) {
        if (k.pit_window[1] > k.pit_window[0]) std::snprintf(buf, sizeof buf, "NEXT STOP  LAP %d-%d", k.pit_window[0], k.pit_window[1]);
        else std::snprintf(buf, sizeof buf, "NEXT STOP  LAP %d", k.pit_window[0]);
        text(buf, x + 16, sy, 13, kText, false, true);
        if (k.pit_plan_tires >= RR_TIRE_SOFT && k.pit_plan_tires <= RR_TIRE_HARD) {
            const float bx = x + 16 + width(buf, 13, false, true) + 16;
            DrawCircleV({bx, sy + 7}, 7, compoundColor(k.pit_plan_tires));
            DrawCircleV({bx, sy + 7}, 4.5f, Color{25, 25, 30, 255});
            text(compoundName(k.pit_plan_tires), bx + 12, sy, 13, kDim);
        }
    }
    if (race.twoCompoundRule() && !c.finished && !c.dnf && (c.compoundsUsed & (c.compoundsUsed - 1)) == 0)
        textRight("2ND COMPOUND DUE", x + w - 16, sy, 13, kAccent, true);
    if (c.pitState == RR_PIT_SERVICE) {
        std::snprintf(buf, sizeof buf, "IN THE BOX  %.1f s", c.serviceLeft);
        textRight(buf, x + w - 16, y + 46, 14, kAccent, true);
    } else if (c.pitState != RR_PIT_NONE) {
        textRight("PIT LANE", x + w - 16, y + 46, 14, kAccent, true);
    }

    if (extra > 0) {  // KERS battery and power, DRS: in a zone, and the flap open or not
        const float ey = y + h - 26;
        const Color orange{255, 170, 60, 255};
        DrawLineEx({x + 14, ey - 4}, {x + w - 14, ey - 4}, 1, Fade(WHITE, 0.10f));
        if (hasKers) {
            text("KERS", x + 16, ey, 13, kDim);
            const float frac = std::clamp(c.state.kersCharge / std::max(1.0f, c.phys.kersStore), 0.0f, 1.0f);
            const Rectangle kb = {x + 56, ey + 3, 80, 9};
            DrawRectangleRec(kb, Fade(WHITE, 0.12f));
            DrawRectangleRec({kb.x, kb.y, kb.width * frac, kb.height}, orange);
            const float kw = c.state.kersPowerNow / 1000.0f;
            std::snprintf(buf, sizeof buf, "%.1f MJ  %s%.0f kW", c.state.kersCharge / 1e6f, kw > 1 ? "+" : "", kw);
            text(buf, x + 144, ey - 1, 13, kw > 1 ? orange : kw < -1 ? Color{90, 200, 120, 255} : kText, false, true);
        }
        if (hasDrs) {
            const bool open = c.state.drsOpen, zone = c.drsZone >= 0, ready = c.drsState == RR_DRS_AVAILABLE || c.drsState == RR_DRS_ARMED;
            const Rectangle zr = {x + w - 134, ey - 2, 56, 20}, dr = {x + w - 72, ey - 2, 56, 20};
            DrawRectangleRounded(zr, 0.3f, 6, zone ? Color{70, 140, 235, 255} : Fade(WHITE, 0.10f));
            text("ZONE", zr.x + 9, zr.y + 3, 13, zone ? WHITE : kDim, true);
            DrawRectangleRounded(dr, 0.3f, 6, open ? Color{80, 210, 110, 255} : ready ? Fade(orange, 0.55f) : Fade(WHITE, 0.10f));
            text("DRS", dr.x + 13, dr.y + 3, 13, open || ready ? Color{20, 20, 24, 255} : kDim, true);
        }
    }
    if (c.state.damage > 0) {
        std::snprintf(buf, sizeof buf, "dmg %.0f", c.state.damage);
        textRight(buf, x + w - 70, y + 20, 13, Color{230, 120, 100, 255});
    }
}

void Hud::drawHelp() {
    const char* lines[] = {
        "Tab / Right   next car",       "Left          previous car",       "1-9           focus car by position",
        "L             follow the leader", "C / Shift+C   next / prev camera",
        "F2-F9         follow, cinematic, TV, heli, top, orbit, overview, director",
        "Mouse drag    orbit (orbit cam)", "Wheel         zoom (orbit, heli, top)",
        "Space         pause",          "+ / -         simulation speed",  "N             single step (paused)",
        "R             restart race",   "P             robot paths",       "S             range finders",
        "M             engine sound on / off", "Esc           race setup menu",
        "H             hide HUD",       "F10           graphics quality", "F12           screenshot",        "F1            close help"};
    const int n = sizeof(lines) / sizeof(lines[0]);
    float w = 760, h = 70 + n * 24.0f;
    float x = (GetScreenWidth() - w) / 2, y = (GetScreenHeight() - h) / 2;
    panel({x, y, w, h}, 0.85f);
    text("CONTROLS", x + 24, y + 20, 22, kAccent, true);
    for (int i = 0; i < n; ++i) text(lines[i], x + 24, y + 58 + i * 24.0f, 17, kText, false, true);
}

void Hud::drawQualiTower(const rr::Race& race, const HudState& st) {
    const float x = 16, w = 340, rowH = 28;
    const float h = 104 + rowH * st.quali.size() + 8;
    panel({x, 16, w, h});
    text(st.sessionTitle.c_str(), x + 16, 26, 15, kAccent, true);
    text(race.track().name().c_str(), x + 16, 46, 19, kText);
    char buf[96];
    std::snprintf(buf, sizeof buf, "RUN %d / %d", st.qualiRun, st.qualiRuns);
    text(buf, x + 16, 72, 26, kText, true);
    const rr::Car& c = race.cars()[0];
    std::snprintf(buf, sizeof buf, "LAP %d / %d", c.currentLap(race.laps()), race.laps());
    textRight(buf, x + w - 16, 78, 17, kDim, false, true);
    float y = 112;
    const float pole = st.quali.empty() ? 0 : st.quali[0].time;
    for (size_t p = 0; p < st.quali.size(); ++p, y += rowH) {
        const QualiLine& q = st.quali[p];
        if (q.running) DrawRectangle((int)x + 6, (int)y - 3, (int)w - 12, (int)rowH - 2, Fade(WHITE, 0.12f));
        std::snprintf(buf, sizeof buf, "%zu", p + 1);
        textRight(buf, x + 38, y, 19, q.time > 0 ? kText : kDim, true);
        DrawRectangle((int)x + 46, (int)y + 1, 5, 19, q.color);
        float size = 19;
        while (size > 13 && width(q.name.c_str(), size) > w - 60 - 100) size -= 1;
        text(q.name.c_str(), x + 60, y + (19 - size) * 0.5f, size, q.time > 0 || q.running ? kText : kDim);
        std::string t;
        if (q.running) t = "ON TRACK";
        else if (q.time <= 0) t = "-";
        else if (p == 0) t = lapTime(q.time);
        else {
            std::snprintf(buf, sizeof buf, "+%.3f", q.time - pole);
            t = buf;
        }
        textRight(t.c_str(), x + w - 16, y + 1, 17, q.running ? kAccent : kDim, false, true);
    }
}

void Hud::drawQualiResults(const HudState& st) {
    const float sw = (float)GetScreenWidth(), sh = (float)GetScreenHeight();
    DrawRectangle(0, 0, (int)sw, (int)sh, Fade(Color{8, 10, 16, 255}, 0.45f));
    const int n = (int)st.quali.size();
    const int perCol = n > 10 ? (n + 1) / 2 : n, cols = n > 10 ? 2 : 1;
    const float colW = 460, rowH = 32;
    const float w = cols * colW + 40, h = 120 + perCol * rowH + 70;
    const float x = (sw - w) / 2, y = std::max(10.0f, (sh - h) / 2);
    panel({x, y, w, h}, 0.9f);
    const bool practice = st.sessionTitle == "PRACTICE";
    text(st.sessionTitle.c_str(), x + 28, y + 22, 18, kAccent, true);
    text(practice ? "Best laps" : "Starting grid", x + 28, y + 44, 32, kText, true);
    char buf[96];
    const float pole = n ? st.quali[0].time : 0;
    for (int p = 0; p < n; ++p) {
        const QualiLine& q = st.quali[p];
        const float cx = x + 20 + (p / perCol) * colW, cy = y + 104 + (p % perCol) * rowH;
        std::snprintf(buf, sizeof buf, "%d", p + 1);
        textRight(buf, cx + 30, cy, 19, kText, true);
        DrawRectangle((int)cx + 40, (int)cy + 1, 5, 19, q.color);
        text(q.name.c_str(), cx + 54, cy, 18, kText);
        if (q.time <= 0) std::snprintf(buf, sizeof buf, "no time");
        else if (p == 0) std::snprintf(buf, sizeof buf, "%s", lapTime(q.time).c_str());
        else std::snprintf(buf, sizeof buf, "+%.3f", q.time - pole);
        textRight(buf, cx + colW - 40, cy + 1, 17, p == 0 ? kAccent : kDim, false, true);
    }
    const char* go = practice ? "Enter: qualifying    Esc: back to setup" : "Enter: start the race    Esc: back to setup";
    text(go, x + (w - width(go, 18, true)) / 2, y + h - 46, 18, kAccent, true);
}
