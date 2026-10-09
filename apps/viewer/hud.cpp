#include "hud.hpp"

#include "hud_icons.hpp"
#include "rlgl.h"
#include "liveries.hpp"
#include "short_names.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>

namespace {

const Color kText = {238, 240, 245, 255};
const Color kDim = {160, 166, 180, 255};
const Color kAccent = {255, 196, 40, 255};
const Color kBlueFlag = {40, 110, 255, 255};
const Color kYellowFlag = {250, 215, 30, 255};
const Color kInk = {16, 18, 24, 255};          // the race HUD's dark panels
const Color kYellowLit = {255, 212, 0, 255};   // lit indicators, the position chip
const Color kRed = {225, 20, 20, 255};         // the logo, the start line
const Color kGreen = {80, 210, 110, 255};
const Color kHot = {240, 70, 60, 255};

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
    loadShortNames(assetsDir);
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

void Hud::textS(const char* s, float x, float y, float size, Color c, bool bold, bool mono) {
    text(s, x + 1, y + 1.5f, size, Fade(BLACK, 0.55f * c.a / 255.0f), bold, mono);
    text(s, x, y, size, c, bold, mono);
}

void Hud::band(Rectangle r, Color c, float slant) {
    icon::slantBar({r.x, r.y}, r.width - slant, r.height, slant, 0, c, c);
}

void Hud::compoundBadge(Vector2 c, float r, int compound, float alpha) {
    const Color col = compoundColor(compound);
    DrawCircleV(c, r, anim::alpha(kInk, 0.9f * alpha));
    DrawRing(c, r - std::max(2.0f, r * 0.26f), r, 0, 360, 28, anim::alpha(col, alpha));
    const char* l = compound == RR_TIRE_SOFT ? "S" : compound == RR_TIRE_HARD ? "H" : "M";
    const float size = r * 1.15f;
    text(l, c.x - width(l, size, true) / 2, c.y - size * 0.55f, size, anim::alpha(col, alpha), true);
}

void Hud::animate(const rr::Race& race) {
    // the 3D renderer leaves depth testing on: 2D shapes would hide each other at random
    rlDrawRenderBatchActive();
    rlDisableDepthTest();
    dt_ = settle ? 10.0f : std::min(GetFrameTime(), 0.1f);
    clock_ += dt_;
    // a new race (or a restart) starts with everything settled
    if (animRace_ != &race || race.time() < animTime_ - 0.5) {
        towerRows_.clear();
        qualiRows_.clear();
        panelAnim_ = PanelAnim{};
    }
    animRace_ = &race;
    animTime_ = race.time();
}

void Hud::draw(const rr::Race& race, const HudState& st) {
    animate(race);
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
        textS(hint, 18, GetScreenHeight() - 30.0f, 15, Fade(kText, 0.7f));
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
        float w = width(buf, 14, true) + 40;
        float x = (GetScreenWidth() - w) / 2;
        band({x, 16, w, 26}, Fade(kInk, 0.82f), 8);
        text(buf, x + 20, 21, 14, st.followLeader ? kAccent : kText, true);
        // race control: the virtual safety car, or the yellow flag the followed car is under
        if (!st.qualifying && !results && race.config().session == RR_SESSION_RACE && race.config().neutral) {
            const rr::Car* fc = st.focus >= 0 && st.focus < (int)race.cars().size() ? &race.cars()[st.focus] : nullptr;
            const char* msg = nullptr;
            if (race.vscState() == RR_VSC_ACTIVE) msg = "VIRTUAL SAFETY CAR   NO OVERTAKING";
            else if (race.vscState() == RR_VSC_ENDING) msg = "VSC ENDING   GREEN FLAG SOON";
            else if (fc && fc->flagState == RR_FLAG_DOUBLE_YELLOW) msg = "DOUBLE YELLOW";
            else if (fc && fc->flagState == RR_FLAG_YELLOW) msg = "YELLOW FLAG";
            if (msg) {
                const float bw = width(msg, 18, true) + 48;
                const Rectangle r = {(GetScreenWidth() - bw) / 2, 50, bw, 32};
                band(r, race.vscState() == RR_VSC_ENDING ? Color{90, 200, 120, 255} : kYellowFlag, 10);
                text(msg, r.x + 24, r.y + 6, 18, kInk, true);
            }
        }
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
        textS(p, (GetScreenWidth() - w) / 2, GetScreenHeight() * 0.2f, 44, kText, true);
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

// Timing tower layout, shared by drawing and clicking: a header, then one row per position.
static const float kTowerX = 16, kTowerW = 318, kTowerRowH = 29, kTowerTop = 94, kTowerPosW = 34;

int Hud::towerCarAt(const rr::Race& race, const HudState& st, Vector2 p) const {
    if (!st.showHud || st.qualifying) return -1;
    if (race.isOver() && st.showResults) return -1;  // hidden under the race-end window
    if (p.x < kTowerX || p.x > kTowerX + kTowerW || p.y < kTowerTop) return -1;
    const int row = (int)((p.y - kTowerTop) / kTowerRowH);
    return row < (int)race.order().size() ? race.order()[row] : -1;
}

void Hud::towerHeader(float x, float w, const char* top, const char* line2, const char* clock, const char* corner) {
    // the logo and the big line (LAP 3 / 10)
    DrawRectangleRec({x, 16, w, 44}, Fade(kInk, 0.92f));
    const Rectangle logo = {x + 8, 24, 56, 28};
    band(logo, kRed, 8);
#ifdef RR2_RENDERER
    const char* name = "RR2";
#else
    const char* name = "RR";
#endif
    text(name, logo.x + (logo.width - width(name, 19, true)) / 2 + 2, logo.y + 4, 19, WHITE, true);
    text(top, x + 76, 25, 23, kText, true);
    if (corner) textRight(corner, x + w - 10, 31, 14, kDim, true);
    // the track and the race clock
    DrawRectangleRec({x, 60, w, 28}, Fade(Color{32, 35, 44, 255}, 0.9f));
    text(line2, x + 10, 66, 15, kDim);
    if (clock) textRight(clock, x + w - 10, 66, 15, kText, false, true);
}

void Hud::drawTower(const rr::Race& race, const HudState& st) {
    const auto& cars = race.cars();
    const auto& order = race.order();
    const rr::Car& leader = cars[order[0]];
    const float x = kTowerX, w = kTowerW, rowH = kTowerRowH, h = rowH - 2;
    const int hover = towerCarAt(race, st, GetMousePosition());
    char lap[32], corner[32], buf[96];
    std::snprintf(lap, sizeof lap, "LAP %d / %d", leader.currentLap(race.laps()), race.laps());
    if (st.paused) std::snprintf(corner, sizeof corner, "PAUSED");
    else std::snprintf(corner, sizeof corner, "x%g", st.timeScale);
    towerHeader(x, w, lap, race.track().name().c_str(), lapTime(race.time()).c_str(), corner);

    // Position numbers stay put; each car's row slides to its new place when it gains or loses one.
    for (size_t p = 0; p < order.size(); ++p) {
        const float y = kTowerTop + p * rowH;
        DrawRectangleRec({x, y, kTowerPosW, h}, Fade(kInk, 0.92f));
        std::snprintf(buf, sizeof buf, "%zu", p + 1);
        text(buf, x + (kTowerPosW - width(buf, 17, true)) / 2, y + 4, 17, kText, true);
        const int idx = order[p];
        RowAnim& a = towerRows_[idx];
        if (a.lastRow >= 0 && a.lastRow != (int)p) {
            a.delta = a.lastRow - (int)p;
            a.changed = 0;
        }
        a.lastRow = (int)p;
        a.y.set((float)p, 0.55f);
        a.y.update(dt_);
        a.changed += dt_;
        a.focus.update(idx == st.focus, dt_, 0.15f, 0.25f);
        a.hover.update(idx == hover, dt_, 0.08f, 0.2f);
    }
    // still rows first, then the ones on the move, the followed car on top
    std::vector<int> drawOrder(order.begin(), order.end());
    std::stable_sort(drawOrder.begin(), drawOrder.end(), [&](int a, int b) {
        auto key = [&](int i) { return (i == st.focus ? 2 : 0) + (towerRows_[i].y.moving() ? 1 : 0); };
        return key(a) < key(b);
    });
    for (int idx : drawOrder) {
        const rr::Car& c = cars[idx];
        RowAnim& a = towerRows_[idx];
        const int p = a.lastRow;
        const float y = kTowerTop + a.y.value() * rowH, sx = x + kTowerPosW + 2, sw = w - kTowerPosW - 2;
        const float f = a.focus.value();
        DrawRectangleRec({sx, y, sw, h}, anim::mix(Fade(kInk, 0.86f), Color{238, 240, 244, 245}, f));
        if (a.hover.value() > 0) DrawRectangleRec({sx, y, sw, h}, Fade(WHITE, 0.08f * a.hover.value()));
        if (c.blueCar >= 0) DrawRectangleGradientH((int)sx, (int)y, (int)sw, (int)h, Fade(kBlueFlag, 0.8f), Fade(kBlueFlag, 0.15f));
        if (c.held) DrawRectangleGradientH((int)sx, (int)y, (int)sw, (int)h, Fade(kYellowFlag, 0.7f), Fade(kYellowFlag, 0.1f));
        const Color ink = anim::mix(kText, kInk, f), dim = anim::mix(kDim, Color{88, 92, 104, 255}, f);
        DrawRectangleRec({sx, y, 5, h}, teamColor(idx));
        // three-letter code, race number, chequered flag
        const std::string code = shortName(c.name, c.robotName);
        text(code.c_str(), sx + 14, y + 4, 18, c.dnf ? dim : ink, true);
        float cx = sx + 14 + width(code.c_str(), 18, true) + 6;
        if (const int num = carNumber(c.name)) {
            std::snprintf(buf, sizeof buf, "%d", num);
            text(buf, cx, y + 8, 12, dim, false, true);
            cx += width(buf, 12, false, true) + 7;
        }
        if (c.finished) {
            icon::chequered({cx, y + 8}, 15, 11);
            cx += 21;
        }
        if (a.changed < 3 && a.delta != 0 && !c.dnf) {  // gained or lost places: a fading arrow
            const float fade = 1 - anim::clamp01((a.changed - 2) / 1.0f);
            icon::triangle({cx + 5, y + h / 2}, 10, a.delta > 0, Fade(a.delta > 0 ? kGreen : kHot, fade));
            cx += 16;
        }
        if (c.penalties > 0 && !c.dnf) {  // seconds added, or still to serve in the pits
            const float pen = c.penaltyTime + c.penaltyOwed;
            if (pen > 0) std::snprintf(buf, sizeof buf, "+%.0fs", pen);
            else std::snprintf(buf, sizeof buf, "PEN");
            const float pw = width(buf, 11, true) + 10;
            DrawRectangleRounded({cx, y + 6, pw, h - 12}, 0.4f, 4, Color{220, 60, 40, 255});
            text(buf, cx + 5, y + 8, 11, WHITE, true);
        }
        // tyre: compound and its age in laps, pit stops as pips
        const float tx = x + w - 108, ty = y + h / 2;
        for (int i = 0; i < std::min(c.pitStops, 3); ++i)
            DrawRectangleRec({tx - 17 - i * 5.0f, y + 8, 3, h - 16}, dim);
        if (c.pitStops > 3) {
            std::snprintf(buf, sizeof buf, "%d", c.pitStops);
            textRight(buf, tx - 13, y + 7, 12, dim, false, true);
        }
        compoundBadge({tx, ty}, 9.5f, c.state.compound);
        std::snprintf(buf, sizeof buf, "%d", c.lapsOnTires);
        text(buf, tx + 14, y + 6, 13, anim::mix(Fade(kText, 0.8f), kInk, f), false, true);
        // gap to the leader, or what the car is doing
        std::string gap;
        if (p == 0) gap = c.finished ? "WINNER" : "LEADER";
        else if (c.dnf) gap = "DNF";
        else if (c.lapsBehind > 0 && !c.finished) gap = "+" + std::to_string(c.lapsBehind) + (c.lapsBehind > 1 ? " LAPS" : " LAP");
        else if (c.gap < 0) gap = "-";
        else {
            std::snprintf(buf, sizeof buf, "+%.3f", c.gap);
            gap = buf;
        }
        Color gapCol = c.dnf ? Color{230, 90, 80, 255} : p == 0 ? dim : ink;
        bool word = p == 0 || c.dnf;
        if (c.pitState != RR_PIT_NONE && !c.finished && !c.dnf) {
            gap = c.pitState == RR_PIT_SERVICE ? "IN BOX" : "PIT";
            gapCol = anim::mix(kAccent, Color{170, 110, 0, 255}, f);
            word = true;
        }
        if (word) textRight(gap.c_str(), x + w - 10, y + 6, 14, gapCol, true);
        else textRight(gap.c_str(), x + w - 10, y + 5, 15, gapCol, false, true);
    }
}

void Hud::drawMinimap(const rr::Race& race, const HudState& st) {
    // No panel: the track floats over the view, outlined so it reads on sky and grass alike.
    const float box = 300, pad = 14;
    const float sx = maxX_ - minX_, sy = maxY_ - minY_;
    const float scale = (box - 2 * pad) / std::max(sx, sy);
    const float w = sx * scale + 2 * pad;
    const float x0 = GetScreenWidth() - w - 16, y0 = 16;
    auto P = [&](rr::Vec2 p) { return Vector2{x0 + pad + (p.x - minX_) * scale, y0 + pad + (maxY_ - p.y) * scale}; };
    const float thick = std::clamp(trackWidth_ * scale, 6.0f, 9.0f);
    auto stroke = [&](float t, Color c) {
        for (size_t i = 0; i < outline_.size(); ++i) {
            const Vector2 a = P(outline_[i]), b = P(outline_[(i + 1) % outline_.size()]);
            DrawLineEx(a, b, t, c);
            DrawCircleV(a, t / 2, c);  // round joints, no notches at the corners
        }
    };
    stroke(thick + 5, Fade(BLACK, 0.45f));
    stroke(thick, Color{236, 238, 242, 240});
    // DRS zones in green along the line
    const rr::Track& tr = race.track();
    for (const RRDrsZone& z : tr.drsZones()) {
        float len = z.end_s - z.start_s;
        if (len < 0) len += tr.length();
        Vector2 prev = P(tr.pointAt(z.start_s, 0));
        for (float d = 8; d <= len + 0.1f; d += 8) {
            const Vector2 cur = P(tr.pointAt(std::fmod(z.start_s + std::min(d, len), tr.length()), 0));
            DrawLineEx(prev, cur, thick * 0.5f, Color{40, 190, 90, 255});
            prev = cur;
        }
    }
    // start line: a red bar across the track
    const rr::Vec2 s0 = tr.at(0).p, n0 = tr.at(0).n;
    const float half = (thick / 2 + 4) / scale;
    DrawLineEx(P(s0 + n0 * half), P(s0 - n0 * half), 3, kRed);

    const auto& cars = race.cars();
    for (size_t i = 0; i < cars.size(); ++i) {
        if ((int)i == st.focus) continue;
        const Vector2 c = P(cars[i].state.pos);
        DrawCircleV(c, 6.5f, Fade(BLACK, 0.7f));
        DrawCircleV(c, 5, teamColor((int)i));
    }
    // The followed car on top: a yellow and black ring (yellow next to a dark team colour, black
    // next to a light one), a slow pulse, and its code.
    if (st.focus >= 0 && st.focus < (int)cars.size()) {
        const Vector2 c = P(cars[st.focus].state.pos);
        const Color team = teamColor(st.focus);
        const bool dark = anim::luminance(team) < 0.5f;
        const Color inner = dark ? kYellowLit : kInk, outer = dark ? kInk : kYellowLit;
        const float pulse = 0.5f + 0.5f * std::sin(clock_ * 4.0f);
        DrawRing(c, 11, 13 + 4 * pulse, 0, 360, 32, Fade(kYellowLit, 0.35f * (1 - pulse) + 0.1f));
        DrawCircleV(c, 11, outer);
        DrawCircleV(c, 9, inner);
        DrawCircleV(c, 6.5f, team);
        const std::string code = shortName(cars[st.focus].name, cars[st.focus].robotName);
        const float cw = width(code.c_str(), 13, true) + 12;
        const bool left = c.x + 16 + cw > GetScreenWidth() - 8;
        const Rectangle chip = {left ? c.x - 16 - cw : c.x + 16, c.y - 10, cw, 20};
        DrawRectangleRounded(chip, 0.3f, 4, Fade(kInk, 0.9f));
        DrawRectangleRec({chip.x, chip.y, 3, chip.height}, team);
        text(code.c_str(), chip.x + 7, chip.y + 3, 13, kText, true);
    }
}

void Hud::drawCarPanel(const rr::Race& race, const HudState& st, float atX, float atY) {
    const rr::Car& c = race.cars()[st.focus];
    const RRControl& k = c.control;
    const bool hasKers = c.phys.kersPower > 0, hasDrs = c.phys.drsDragScale < 1.0f;
    const float w = 452, h = 268;
    const float x = atX >= 0 ? atX : GetScreenWidth() - w - 16, y = atY >= 0 ? atY : GetScreenHeight() - h - 16;
    PanelAnim& pa = panelAnim_;
    char buf[128];
    const float rpmFrac = std::clamp(c.state.rpm / c.phys.maxRpm, 0.0f, 1.0f);
    if (!pa.started) pa.rpm = rpmFrac;
    pa.started = true;

    // Banners above the panel (blue flag, yellow flag, penalty, pit lane) slide and fade in and out.
    if (c.blueCar >= 0) pa.blueMsg = "BLUE FLAG   let " + shortName(race.cars()[c.blueCar].name, race.cars()[c.blueCar].robotName) + " by";
    if (c.held || c.flagState != RR_FLAG_GREEN)
        pa.yellowMsg = c.held ? "HELD BY THE MARSHALS   wait for a clear track"
                     : c.flagState == RR_FLAG_DOUBLE_YELLOW ? "DOUBLE YELLOW   be ready to stop" : "YELLOW FLAG   no overtaking";
    if (c.penalties > 0) {
        if (c.penaltyOwed > 0)
            std::snprintf(buf, sizeof buf, "PENALTY  +%.0f s  (%d)   %.0f s TO SERVE", c.penaltyTime, c.penalties, c.penaltyOwed);
        else std::snprintf(buf, sizeof buf, "PENALTY  +%.0f s  (%d)", c.penaltyTime, c.penalties);
        pa.penaltyMsg = buf;
    }
    if (c.pitState == RR_PIT_SERVICE) {
        std::snprintf(buf, sizeof buf, "IN THE BOX   %.1f s", c.serviceLeft);
        pa.pitMsg = buf;
    } else if (c.pitState != RR_PIT_NONE) {
        pa.pitMsg = "PIT LANE";
    }
    pa.blue.update(c.blueCar >= 0, dt_);
    pa.yellow.update(c.held || c.flagState != RR_FLAG_GREEN, dt_);
    pa.penalty.update(c.penalties > 0, dt_);
    pa.pit.update(c.pitState != RR_PIT_NONE, dt_);
    float fy = y - 34;
    auto banner = [&](const anim::Light& l, Color bg, Color fg, const std::string& msg) {
        const float v = l.value();
        if (v <= 0.01f) return;
        const float off = (1 - v) * 30, bw = width(msg.c_str(), 16, true) + 44;
        band({x + w - bw + off, fy, bw, 28}, anim::alpha(bg, v), 10);
        text(msg.c_str(), x + w - bw + off + 22, fy + 5, 16, anim::alpha(fg, v), true);
        fy -= 34 * v;
    };
    banner(pa.blue, kBlueFlag, WHITE, pa.blueMsg);
    banner(pa.yellow, kYellowFlag, kInk, pa.yellowMsg);
    banner(pa.penalty, Color{220, 60, 40, 255}, WHITE, pa.penaltyMsg);
    banner(pa.pit, kAccent, kInk, pa.pitMsg);

    // Header: team colour, name, team and algorithm; the position on the right.
    if (pa.car.started && pa.car.current != st.focus) {  // another car: its position and gear just appear
        pa.pos = {};
        pa.gear = {};
    }
    pa.car.set(st.focus);
    pa.car.update(dt_, 0.35f);
    const float carIn = anim::easeOutCubic(pa.car.t);
    band({x, y, w, 40}, Fade(kInk, 0.88f), 10);
    DrawRectangleRec({x + 14, y + 6, 5, 28}, teamColor(st.focus));
    text(c.name.c_str(), x + 28 + (1 - carIn) * 12, y + 3, 19, anim::alpha(kText, carIn), true);
    const auto& lt = liveryTable();
    const std::string team = lt.empty() ? std::string() : lt[carLivery(st.focus)].team + "   ";
    std::snprintf(buf, sizeof buf, "%s%s%s%s", team.c_str(), c.robotName.c_str(), c.params.empty() ? "" : "  ", c.params.c_str());
    {  // team, algorithm and parameters, smaller when long
        float size = 13;
        while (size > 9 && width(buf, size) > w - 28 - 150) size -= 1;
        text(buf, x + 28, y + 24, size, anim::alpha(kDim, carIn));
    }
    if (c.state.damage > 0) {
        std::snprintf(buf, sizeof buf, "%.0f", c.state.damage);
        const float dx = x + w - 92 - width(buf, 14, true);
        icon::warning({dx - 13, y + 19}, 17, Color{240, 110, 90, 255});
        text(buf, dx, y + 12, 14, Color{240, 110, 90, 255}, true);
    }
    pa.pos.set(c.position);
    pa.pos.update(dt_, 0.4f);
    {  // the position: the old number slides out, the new one in
        const Rectangle pc = {x + w - 76, y, 76, 40};
        band(pc, kYellowLit, 10);
        const float t = anim::easeInOutCubic(pa.pos.t);
        const bool up = pa.pos.current < pa.pos.previous;
        BeginScissorMode((int)pc.x, (int)pc.y, (int)pc.width, (int)pc.height);
        auto num = [&](int pos, float dy, float a) {
            std::snprintf(buf, sizeof buf, "P%d", pos);
            text(buf, pc.x + (pc.width - width(buf, 26, true)) / 2 + 3, pc.y + 6 + dy, 26, anim::alpha(kInk, a), true);
        };
        if (t < 1) num(pa.pos.previous, (up ? 1 : -1) * t * 30, 1 - t);
        num(pa.pos.current, (up ? -1 : 1) * (1 - t) * 30, t);
        EndScissorMode();
    }

    const float by = y + 46;  // the body: tyres, rev gauge, driver inputs
    // Tyres and brakes: a top view of the car, each tyre coloured by its temperature and filled
    // to the life it has left, its temperature outboard, the brake disc's temperature under it.
    {
        DrawRectangleRounded({x, by, 156, 140}, 0.08f, 6, Fade(kInk, 0.55f));
        compoundBadge({x + 18, by + 15}, 10, c.state.compound);
        text(compoundName(c.state.compound), x + 34, by + 8, 13, kText, true);
        std::snprintf(buf, sizeof buf, "%d LAPS", c.lapsOnTires);
        textRight(buf, x + 148, by + 9, 12, kDim, false, true);
        const rr::Compound& comp = rr::compoundInfo(c.state.compound);
        const Color body = Fade(WHITE, 0.18f);
        DrawRectangleRounded({x + 70, by + 30, 16, 104}, 0.6f, 6, body);   // chassis
        DrawRectangleRec({x + 44, by + 28, 68, 4}, body);                  // front wing
        DrawRectangleRec({x + 48, by + 128, 60, 5}, body);                 // rear wing
        DrawLineEx({x + 60, by + 48}, {x + 96, by + 48}, 2, body);         // axles
        DrawLineEx({x + 62, by + 110}, {x + 94, by + 110}, 2, body);
        static const Rectangle kTyre[4] = {{38, 30, 22, 36}, {96, 30, 22, 36}, {34, 90, 26, 40}, {96, 90, 26, 40}};
        for (int i = 0; i < 4; ++i) {
            const int axle = i / 2, side = i % 2;
            const Rectangle r = {x + kTyre[i].x, by + kTyre[i].y, kTyre[i].width, kTyre[i].height};
            const float temp = c.state.wheelTemp[i], life = 1 - std::clamp(c.state.tireWear[axle], 0.0f, 1.0f);
            const Color tc = tempColor(temp, comp.tempLo, comp.tempHi, 10);
            DrawRectangleRounded(r, 0.35f, 6, Color{34, 36, 44, 235});
            const float fh = (r.height - 4) * life;
            if (fh > 1) DrawRectangleRounded({r.x + 2, r.y + 2 + (r.height - 4 - fh), r.width - 4, fh}, 0.3f, 4, Fade(tc, 0.9f));
            DrawRectangleRoundedLinesEx(r, 0.35f, 6, 1.5f, tc);
            std::snprintf(buf, sizeof buf, "%.0f", temp);
            if (side == 0) textRight(buf, r.x - 3, r.y, 14, tc, true, true);
            else text(buf, r.x + r.width + 3, r.y, 14, tc, true, true);
            // brake disc: a dot inboard, and its temperature
            const float bt = c.state.brakeTemp[i];
            const Color bc = tempColor(bt, c.phys.brakeTempLo, c.phys.brakeTempHi, 100);
            DrawCircleV({side == 0 ? r.x + r.width + 6 : r.x - 6, r.y + r.height / 2}, 4, bc);
            std::snprintf(buf, sizeof buf, "%.0f", bt);
            if (side == 0) textRight(buf, r.x - 3, r.y + r.height - 13, 12, bc, false, true);
            else text(buf, r.x + r.width + 3, r.y + r.height - 13, 12, bc, false, true);
        }
    }

    // Rev gauge: a ring of segments, the gear in the middle, the speed under it.
    {
        const Vector2 g = {x + 240, by + 70};
        const float R = 66;
        pa.rpm = anim::approach(pa.rpm, rpmFrac, dt_, 0.08f);
        pa.shift = anim::ramp(pa.shift, rpmFrac > 0.95f ? 1.0f : 0.0f, dt_, 0.12f);
        DrawCircleV(g, R + 6, Fade(kInk, 0.62f));
        if (pa.shift > 0) DrawRing(g, R + 2, R + 6, 135, 405, 48, Fade(kHot, 0.8f * anim::easeInOutCubic(pa.shift)));
        const int n = 34;
        const float span = 270.0f / n;
        for (int i = 0; i < n; ++i) {
            const float f = (i + 0.5f) / n, a0 = 135 + i * span + 1.2f;
            const Color lit = f > 0.92f ? kHot : f > 0.8f ? kYellowLit : kGreen;
            const bool on = f <= pa.rpm;
            icon::arc(g, R - 11, R, a0, a0 + span - 2.4f, on ? lit : Fade(WHITE, 0.1f));
        }
        std::snprintf(buf, sizeof buf, "%.0f RPM", c.state.rpm);
        text(buf, g.x - width(buf, 12, false, true) / 2, g.y - 46, 12, kDim, false, true);
        pa.gear.set(c.state.gear);
        pa.gear.update(dt_, 0.25f);
        const int gear = pa.gear.current;
        std::snprintf(buf, sizeof buf, "%s", gear < 0 ? "R" : gear == 0 ? "N" : std::to_string(gear).c_str());
        const float gs = 50 * (0.8f + 0.2f * anim::easeOutBack(pa.gear.t));
        text(buf, g.x - width(buf, gs, true) / 2, g.y - 8 - gs * 0.55f, gs, anim::mix(kText, kYellowLit, anim::easeInOutCubic(pa.shift)), true);
        const float speed = rr::length(c.state.velWorld()) * 3.6f;
        std::snprintf(buf, sizeof buf, "%.0f", speed);
        text(buf, g.x - width(buf, 26, true) / 2, g.y + 20, 26, kText, true);
        text("km/h", g.x - width("km/h", 11) / 2, g.y + 48, 11, kDim);
    }

    // Driver inputs: throttle and brake, the steering wheel; KERS under them.
    {
        const float rx = x + 322;
        DrawRectangleRounded({rx, by, 130, 140}, 0.08f, 6, Fade(kInk, 0.55f));
        icon::slantBar({rx + 12, by + 12}, 14, 74, 7, k.accel, kGreen, Fade(WHITE, 0.12f));
        icon::slantBar({rx + 36, by + 12}, 14, 74, 7, k.brake, kHot, Fade(WHITE, 0.12f));
        text("THR", rx + 9, by + 90, 11, kDim, true);
        text("BRK", rx + 34, by + 90, 11, kDim, true);
        const float steer = std::clamp(k.steer, -1.0f, 1.0f);  // + = left
        icon::steeringWheel({rx + 94, by + 42}, 24, -steer * 90, kText, kAccent);
        text("STEER", rx + 94 - width("STEER", 11, true) / 2, by + 72, 11, kDim, true);
        if (hasKers) {
            const float frac = std::clamp(c.state.kersCharge / std::max(1.0f, c.phys.kersStore), 0.0f, 1.0f);
            const float kw = c.state.kersPowerNow / 1000.0f;
            const Color orange{255, 170, 60, 255};
            icon::battery({rx + 10, by + 112}, 38, 18, frac, orange, kDim);
            icon::bolt({rx + 26, by + 121}, 13, kw > 1 ? WHITE : Fade(WHITE, 0.6f));
            std::snprintf(buf, sizeof buf, "%.1f MJ", c.state.kersCharge / 1e6f);
            text(buf, rx + 56, by + 104, 12, kText, false, true);
            std::snprintf(buf, sizeof buf, "%s%.0f kW", kw > 1 ? "+" : "", kw);
            text(buf, rx + 56, by + 120, 12, kw > 1 ? orange : kw < -1 ? kGreen : kDim, false, true);
        }
    }

    // Lap times.
    float sy = by + 146;
    band({x, sy, w, 24}, Fade(kInk, 0.82f), 8);
    if (c.finished && st.lapClock < 0) {
        icon::chequered({x + 14, sy + 6}, 16, 12);
        std::snprintf(buf, sizeof buf, "FINISHED  %s", lapTime(c.finishTime).c_str());
        text(buf, x + 38, sy + 4, 15, kText, true, true);
    } else {
        icon::stopwatch({x + 22, sy + 12}, 17, kText);
        text(lapTime(st.lapClock >= 0 ? st.lapClock : race.time() - c.lapStart).c_str(), x + 38, sy + 3, 16, kText, true, true);
    }
    text("LAST", x + 200, sy + 7, 10, kDim, true);
    text(lapTime(st.lastLap >= 0 ? st.lastLap : c.lapTimes.empty() ? 0 : c.lapTimes.back()).c_str(), x + 230, sy + 4, 14, kText, false, true);
    text("BEST", x + 322, sy + 7, 10, kDim, true);
    text(lapTime(st.bestLap >= 0 ? st.bestLap : c.bestLap).c_str(), x + 352, sy + 4, 14, Color{190, 120, 255, 255}, false, true);

    // Fuel and pit stops; DRS and the DRS zone light up yellow.
    sy += 27;
    band({x, sy, w, 24}, Fade(kInk, 0.82f), 8);
    {
        const float fuelFrac = std::clamp(c.state.fuel / c.phys.fuelCapacity, 0.0f, 1.0f);
        icon::fuelPump({x + 22, sy + 12}, 16, kText);
        const Rectangle fb = {x + 38, sy + 8, 80, 8};
        DrawRectangleRounded(fb, 0.5f, 4, Fade(WHITE, 0.14f));
        if (fuelFrac > 0.01f)
            DrawRectangleRounded({fb.x, fb.y, fb.width * fuelFrac, fb.height}, 0.5f, 4, fuelFrac < 0.1f ? kHot : Color{90, 170, 235, 255});
        std::snprintf(buf, sizeof buf, "%.1f L", c.state.fuel);
        text(buf, fb.x + fb.width + 8, sy + 4, 14, kText, false, true);
        if (c.pitStops > 0) {
            icon::wrench({x + 206, sy + 12}, 15, kDim);
            std::snprintf(buf, sizeof buf, "%d %s", c.pitStops, c.pitStops == 1 ? "STOP" : "STOPS");
            text(buf, x + 218, sy + 5, 12, kDim, true);
        }
    }
    if (hasDrs) {
        const bool open = c.state.drsOpen, zone = c.drsZone >= 0, ready = c.drsState == RR_DRS_AVAILABLE || c.drsState == RR_DRS_ARMED;
        pa.zone = anim::ramp(pa.zone, zone ? 1.0f : 0.0f, dt_, 0.25f);
        pa.drs = anim::ramp(pa.drs, open ? 1.0f : ready ? 0.45f : 0.0f, dt_, 0.25f);
        auto lamp = [&](const char* label, float right, float level) {
            const float v = anim::easeInOutCubic(level), lw = width(label, 16, true);
            if (v > 0.01f) band({right - lw - 12, sy + 2, lw + 20, 20}, Fade(kYellowLit, 0.22f * v), 6);
            text(label, right - lw, sy + 2, 16, anim::mix(Fade(kDim, 0.55f), kYellowLit, v), true);
        };
        lamp("ZONE", x + w - 74, pa.zone);
        lamp("DRS", x + w - 16, pa.drs);
    }

    // The algorithm's status and its strategy: next planned stop, the two-compound rule.
    sy += 27;
    std::string status = k.status[0] ? k.status : "";
    std::string plan;
    if (k.pit_window[0] > 0 && !c.finished) {
        if (k.pit_window[1] > k.pit_window[0]) std::snprintf(buf, sizeof buf, "NEXT STOP  LAP %d-%d", k.pit_window[0], k.pit_window[1]);
        else std::snprintf(buf, sizeof buf, "NEXT STOP  LAP %d", k.pit_window[0]);
        plan = buf;
    }
    const bool due = race.twoCompoundRule() && !c.finished && !c.dnf && (c.compoundsUsed & (c.compoundsUsed - 1)) == 0;
    if (!status.empty() || !plan.empty() || due) {
        band({x, sy, w, 20}, Fade(kInk, 0.7f), 6);
        float right = x + w - 12;
        if (due) {
            textRight("2ND COMPOUND DUE", right, sy + 4, 11, kAccent, true);
            right -= width("2ND COMPOUND DUE", 11, true) + 14;
        }
        if (!plan.empty()) {
            if (k.pit_plan_tires >= RR_TIRE_SOFT && k.pit_plan_tires <= RR_TIRE_HARD) {
                compoundBadge({right - 7, sy + 10}, 7, k.pit_plan_tires);
                right -= 20;
            }
            textRight(plan.c_str(), right, sy + 4, 11, kText, false, true);
            right -= width(plan.c_str(), 11, false, true) + 14;
        }
        float size = 12;
        while (size > 8 && width(status.c_str(), size) > right - x - 24) size -= 1;
        text(status.c_str(), x + 14, sy + 3, size, kDim);
    }
}

void Hud::drawHelp() {
    const char* lines[] = {
        "Tab / Right   next car",       "Left          previous car",       "1-9           focus car by position",
        "L             follow the leader", "C / Shift+C   next / prev camera", "T / B         T-cam (roll hoop) / nose camera",
        "F2-F9         follow, cinematic, TV, heli, top, orbit, overview, director",
        "Mouse drag    orbit (orbit cam)", "Wheel         zoom (orbit, heli, top)",
        "Space         pause",          "+ / -         simulation speed",  "N             single step (paused)",
        "R             restart race",   "P             robot paths",       "S             range finders",
        "M             engine sound on / off", "Esc           race setup menu",
        "H             hide HUD",       "F10           graphics quality", "K             next sky and lighting", ";  '          turn the sun",   ",  .          exposure", "F12           screenshot",        "F1            close help"};
    const int n = sizeof(lines) / sizeof(lines[0]);
    float w = 760, h = 70 + n * 24.0f;
    float x = (GetScreenWidth() - w) / 2, y = (GetScreenHeight() - h) / 2;
    panel({x, y, w, h}, 0.85f);
    text("CONTROLS", x + 24, y + 20, 22, kAccent, true);
    for (int i = 0; i < n; ++i) text(lines[i], x + 24, y + 58 + i * 24.0f, 17, kText, false, true);
}

void Hud::drawQualiTower(const rr::Race& race, const HudState& st) {
    const float x = kTowerX, w = kTowerW, rowH = kTowerRowH, h = rowH - 2;
    char run[32], lap[32], buf[96];
    std::snprintf(run, sizeof run, "RUN %d / %d", st.qualiRun, st.qualiRuns);
    const rr::Car& car = race.cars()[0];
    std::snprintf(lap, sizeof lap, "LAP %d / %d", car.currentLap(race.laps()), race.laps());
    std::snprintf(buf, sizeof buf, "%s   %s", st.sessionTitle.c_str(), race.track().name().c_str());
    towerHeader(x, w, run, buf, lap, nullptr);
    const float pole = st.quali.empty() ? 0 : st.quali[0].time;
    for (size_t p = 0; p < st.quali.size(); ++p) {
        const float y = kTowerTop + p * rowH;
        DrawRectangleRec({x, y, kTowerPosW, h}, Fade(kInk, 0.92f));
        std::snprintf(buf, sizeof buf, "%zu", p + 1);
        text(buf, x + (kTowerPosW - width(buf, 17, true)) / 2, y + 4, 17, st.quali[p].time > 0 ? kText : kDim, true);
        RowAnim& a = qualiRows_[st.quali[p].name];
        a.lastRow = (int)p;
        a.y.set((float)p, 0.55f);
        a.y.update(dt_);
        a.focus.update(st.quali[p].running, dt_, 0.15f, 0.25f);
    }
    for (size_t p = 0; p < st.quali.size(); ++p) {
        const QualiLine& q = st.quali[p];
        RowAnim& a = qualiRows_[q.name];
        const float y = kTowerTop + a.y.value() * rowH, sx = x + kTowerPosW + 2, sw = w - kTowerPosW - 2;
        const float f = a.focus.value();
        DrawRectangleRec({sx, y, sw, h}, anim::mix(Fade(kInk, 0.86f), Color{238, 240, 244, 245}, f));
        const Color ink = anim::mix(q.time > 0 ? kText : kDim, kInk, f), dim = anim::mix(kDim, Color{88, 92, 104, 255}, f);
        DrawRectangleRec({sx, y, 5, h}, q.color);
        const std::string code = shortName(q.name);
        text(code.c_str(), sx + 14, y + 4, 18, ink, true);
        if (const int num = carNumber(q.name)) {
            std::snprintf(buf, sizeof buf, "%d", num);
            text(buf, sx + 20 + width(code.c_str(), 18, true), y + 8, 12, dim, false, true);
        }
        std::string t;
        if (q.running) t = "ON TRACK";
        else if (q.time <= 0) t = "-";
        else if (p == 0) t = lapTime(q.time);
        else {
            std::snprintf(buf, sizeof buf, "+%.3f", q.time - pole);
            t = buf;
        }
        if (q.running) textRight(t.c_str(), x + w - 10, y + 6, 14, anim::mix(kAccent, Color{170, 110, 0, 255}, f), true);
        else textRight(t.c_str(), x + w - 10, y + 5, 15, p == 0 ? Color{190, 120, 255, 255} : dim, false, true);
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
