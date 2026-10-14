// The testing screen: telemetry graphs, the session's laps, the timeline, and
// the testing pages of the setup menu (car stats with their effects, saved runs).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "hud.hpp"
#include "liveries.hpp"

namespace {

const Color kText = {238, 240, 245, 255};
const Color kDim = {160, 166, 180, 255};
const Color kFaint = {95, 100, 115, 255};
const Color kAccent = {255, 196, 40, 255};
const Color kBest = {190, 110, 255, 255};   // personal best, F1 purple
const Color kGood = {90, 200, 120, 255};
const Color kBad = {240, 90, 70, 255};
const Color kThrottle = {80, 210, 110, 255};
const Color kBrake = {235, 70, 60, 255};
const Color kFront = {80, 190, 255, 255};
const Color kRear = {255, 120, 200, 255};
const Color kRef = {120, 150, 190, 255};
const Color kPanel = {12, 14, 20, 255};

float speedOf(const rr::TestSample& s) { return std::hypot(s.s.vx, s.s.vy) * 3.6f; }

float progressOf(const rr::TestSample& s, float L) {
    return (s.lap - 1) + std::clamp(s.lapDist / std::max(1.0f, L), 0.0f, 1.0f);
}

Color speedColour(float kmh) {
    const float t = std::clamp((kmh - 60.0f) / 260.0f, 0.0f, 1.0f);
    const Color stops[] = {{60, 90, 255, 255}, {40, 210, 230, 255}, {90, 220, 90, 255}, {250, 220, 40, 255}, {245, 60, 50, 255}};
    const float f = t * 4;
    const int i = std::min(3, (int)f);
    const float u = f - i;
    const Color a = stops[i], b = stops[i + 1];
    return {(unsigned char)(a.r + (b.r - a.r) * u), (unsigned char)(a.g + (b.g - a.g) * u),
            (unsigned char)(a.b + (b.b - a.b) * u), 255};
}

Color eventColour(const std::string& kind) {
    if (kind == "contact" || kind == "out_of_fuel" || kind == "stopped") return kBad;
    if (kind == "spin") return Color{255, 70, 200, 255};
    if (kind == "off_track") return Color{255, 150, 40, 255};
    if (kind == "wheelspin") return Color{150, 220, 120, 255};
    return Color{240, 220, 90, 255};  // oversteer, understeer
}

const char* eventName(const std::string& kind) {
    if (kind == "off_track") return "off track";
    if (kind == "out_of_fuel") return "out of fuel";
    return kind.c_str();
}

// The lap to compare with: the one asked for, else the best lap (another lap
// than `lap` when there is one).
int refLapFor(const rr::TestRecorder& rec, int compare, int lap) {
    const int n = (int)rec.laps.size();
    if (compare > 0 && compare <= n) return compare;
    int best = 0;
    for (int i = 0; i < n; ++i) {
        if (i + 1 == lap && n > 1) continue;
        if (best == 0 || rec.laps[i].time < rec.laps[best - 1].time) best = i + 1;
    }
    return best;
}

// Time into the lap at distance d on the samples [f, e) (lapDist rises along the lap).
float timeAtDist(const rr::TestRecorder& rec, int f, int e, float d, bool* ok = nullptr) {
    if (ok) *ok = false;
    if (e - f < 2) return 0;
    const auto& S = rec.samples;
    if (d < S[f].lapDist || d > S[e - 1].lapDist) return 0;
    int lo = f, hi = e - 1;
    while (hi - lo > 1) {
        const int mid = (lo + hi) / 2;
        (S[mid].lapDist <= d ? lo : hi) = mid;
    }
    const float span = S[hi].lapDist - S[lo].lapDist;
    const float u = span > 1e-3f ? (d - S[lo].lapDist) / span : 0;
    if (ok) *ok = true;
    return S[lo].lapTime + u * (S[hi].lapTime - S[lo].lapTime);
}

int sampleAtDist(const rr::TestRecorder& rec, int f, int e, float d) {
    if (e <= f) return -1;
    const auto& S = rec.samples;
    int lo = f, hi = e - 1;
    if (d <= S[lo].lapDist) return lo;
    if (d >= S[hi].lapDist) return hi;
    while (hi - lo > 1) {
        const int mid = (lo + hi) / 2;
        (S[mid].lapDist <= d ? lo : hi) = mid;
    }
    return d - S[lo].lapDist < S[hi].lapDist - d ? lo : hi;
}

rr::TestSample cursorSample(const TestView& tv) {
    const rr::TestRecorder& rec = *tv.rec;
    if (rec.empty()) return rr::TestSample{};
    const int n = (int)rec.samples.size();
    rr::TestSample s = tv.live ? rec.samples.back() : rec.at(tv.cursor);
    // the moment the car crossed the line for the last time belongs to the last lap
    if (s.lap > (int)rec.laps.size() && n >= 2 && rec.samples[n - 2].lap < s.lap && (tv.runOver || tv.replay) &&
        s.t >= rec.samples[n - 1].t - 1e-4f)
        s = rec.samples[n - 2];
    return s;
}

}  // namespace

// ---------------------------------------------------------------- plots

Hud::Plot Hud::plotFrame(Rectangle box, const char* title, float x0, float x1, float y0, float y1, const char* yFmt,
                         int yTicks, bool xLaps) {
    const bool small = box.height < 220;
    const float titleH = small ? 20 : 24, left = small ? 40 : 52, bottom = small ? 16 : 20;
    {
        float size = small ? 14 : 16;
        while (size > 10 && width(title, size, true) > box.width - 12) size -= 0.5f;
        text(title, box.x + 6, box.y + 2 + ((small ? 14 : 16) - size) / 2, size, kText, true);
    }
    Plot p;
    p.r = {box.x + left, box.y + titleH + 4, box.width - left - 8, box.height - titleH - 4 - bottom};
    if (x1 <= x0) x1 = x0 + 1;
    if (y1 <= y0) y1 = y0 + 1;
    p.x0 = x0;
    p.x1 = x1;
    p.y0 = y0;
    p.y1 = y1;
    DrawRectangleRec(p.r, Fade(BLACK, 0.25f));
    char buf[32];
    const float fs = small ? 11 : 12;
    for (int k = 0; k <= yTicks; ++k) {
        const float v = y0 + (y1 - y0) * k / yTicks;
        const Vector2 a = p.at(x0, v);
        DrawLineEx({p.r.x, a.y}, {p.r.x + p.r.width, a.y}, 1, Fade(WHITE, k == 0 ? 0.25f : 0.08f));
        std::snprintf(buf, sizeof buf, yFmt, v);
        textRight(buf, p.r.x - 4, a.y - fs / 2 - 1, fs, kDim, false, true);
    }
    // x ticks: laps, or distance in km
    if (xLaps) {
        const float span = x1 - x0;
        const int step = span > 40 ? 10 : span > 16 ? 5 : span > 8 ? 2 : 1;
        for (int l = (int)std::ceil(x0); l <= (int)x1; ++l) {
            if (l % step) continue;
            const Vector2 a = p.at((float)l, y0);
            DrawLineEx({a.x, p.r.y}, {a.x, p.r.y + p.r.height}, 1, Fade(WHITE, 0.06f));
            std::snprintf(buf, sizeof buf, "%d", l);
            text(buf, a.x - width(buf, fs, false, true) / 2, p.r.y + p.r.height + 2, fs, kFaint, false, true);
        }
    } else {
        for (float d = 0; d <= x1; d += 500) {
            if (d < x0) continue;
            const Vector2 a = p.at(d, y0);
            DrawLineEx({a.x, p.r.y}, {a.x, p.r.y + p.r.height}, 1, Fade(WHITE, 0.06f));
            if (!small || (int)d % 1000 == 0) {
                std::snprintf(buf, sizeof buf, "%.1f", d / 1000);
                text(buf, a.x - width(buf, fs, false, true) / 2, p.r.y + p.r.height + 2, fs, kFaint, false, true);
            }
        }
    }
    return p;
}

void Hud::plotSeries(const Plot& p, const std::vector<Vector2>& pts, Color c, float thick) {
    if (pts.size() < 2) return;
    BeginScissorMode((int)p.r.x, (int)p.r.y - 1, (int)p.r.width + 1, (int)p.r.height + 2);
    Vector2 prev = p.at(pts[0].x, pts[0].y);
    for (size_t i = 1; i < pts.size(); ++i) {
        const Vector2 q = p.at(pts[i].x, pts[i].y);
        if (std::fabs(q.x - prev.x) < 0.4f && std::fabs(q.y - prev.y) < 0.4f && i + 1 < pts.size()) continue;
        DrawLineEx(prev, q, thick, c);
        prev = q;
    }
    EndScissorMode();
}

// ---------------------------------------------------------------- distance graphs

void Hud::distGraph(const TestView& tv, Rectangle box, int which, int lap, int ref, bool big, std::vector<TestHit>& hits) {
    const rr::TestRecorder& rec = *tv.rec;
    int f = 0, e = 0, rf = 0, re = 0;
    rec.lapRange(lap, f, e);
    if (ref > 0 && ref != lap) rec.lapRange(ref, rf, re);
    const float L = std::max(1.0f, rec.trackLength);
    const float x0 = std::min(0.0f, e > f ? rec.samples[f].lapDist : 0.0f);
    const rr::TestSample cur = cursorSample(tv);
    const bool cursorHere = cur.lap == lap;
    const int ci = cursorHere ? sampleAtDist(rec, f, e, cur.lapDist) : -1;
    const int ri = cursorHere && re > rf ? sampleAtDist(rec, rf, re, cur.lapDist) : -1;
    char title[160];
    auto vals = [&](const char* name, const char* fmt, float a, float b, bool hasB) {
        char va[32] = "", vb[48] = "";
        if (ci >= 0) std::snprintf(va, sizeof va, fmt, a);
        if (hasB && ri >= 0) {
            char t[32];
            std::snprintf(t, sizeof t, fmt, b);
            std::snprintf(vb, sizeof vb, "   lap %d: %s", ref, t);
        }
        std::snprintf(title, sizeof title, "%s   %s%s", name, va, vb);
    };
    std::vector<Vector2> a, b, c, d;
    auto series = [&](int s0, int s1, std::vector<Vector2>& out, float (*fn)(const rr::TestSample&)) {
        out.clear();
        for (int i = s0; i < s1; ++i) out.push_back({rec.samples[i].lapDist, fn(rec.samples[i])});
    };
    Plot p;
    const auto& S = rec.samples;
    switch (which) {
        case 0: {  // speed, with the gear in the big view
            vals("Speed", "%.0f km/h", ci >= 0 ? speedOf(S[ci]) : 0, ri >= 0 ? speedOf(S[ri]) : 0, true);
            p = plotFrame(box, title, x0, L, 0, 340, "%.0f", big ? 4 : 2, false);
            series(rf, re, b, [](const rr::TestSample& s) { return speedOf(s); });
            series(f, e, a, [](const rr::TestSample& s) { return speedOf(s); });
            if (big) {
                series(f, e, c, [](const rr::TestSample& s) { return s.s.gear * 340.0f / 8.0f; });
                plotSeries(p, c, Fade(kDim, 0.5f), 1.2f);
            }
            plotSeries(p, b, kRef, 1.5f);
            plotSeries(p, a, kText, 2.0f);
            if (big && ci >= 0) {
                char g[16];
                std::snprintf(g, sizeof g, "gear %d", S[ci].s.gear);
                textRight(g, box.x + box.width - 10, box.y + 4, 13, kDim);
            }
            break;
        }
        case 1: {  // pedals
            std::snprintf(title, sizeof title, "Throttle / brake");
            if (ci >= 0)
                std::snprintf(title, sizeof title, "Throttle %3.0f%%   Brake %3.0f%%", S[ci].accel * 100, S[ci].brake * 100);
            p = plotFrame(box, title, x0, L, 0, 1, "%.1f", 2, false);
            series(rf, re, b, [](const rr::TestSample& s) { return s.accel; });
            series(rf, re, d, [](const rr::TestSample& s) { return s.brake; });
            plotSeries(p, b, Fade(kThrottle, 0.35f), 1.5f);
            plotSeries(p, d, Fade(kBrake, 0.35f), 1.5f);
            series(f, e, a, [](const rr::TestSample& s) { return s.accel; });
            series(f, e, c, [](const rr::TestSample& s) { return s.brake; });
            plotSeries(p, a, kThrottle, 2.0f);
            plotSeries(p, c, kBrake, 2.0f);
            break;
        }
        case 2: {  // steering
            vals("Steering (+ left)", "%+.2f", ci >= 0 ? S[ci].steer : 0, ri >= 0 ? S[ri].steer : 0, true);
            p = plotFrame(box, title, x0, L, -1, 1, "%+.1f", 2, false);
            series(rf, re, b, [](const rr::TestSample& s) { return s.steer; });
            series(f, e, a, [](const rr::TestSample& s) { return s.steer; });
            plotSeries(p, b, kRef, 1.5f);
            plotSeries(p, a, kAccent, 2.0f);
            break;
        }
        case 3: {  // grip use
            std::snprintf(title, sizeof title, "Grip use (1 = limit)");
            if (ci >= 0)
                std::snprintf(title, sizeof title, "Grip use  front %.2f  rear %.2f", S[ci].s.gripUse[0], S[ci].s.gripUse[1]);
            p = plotFrame(box, title, x0, L, 0, 1.6f, "%.1f", big ? 4 : 2, false);
            const Vector2 l0 = p.at(x0, 1.0f);
            DrawLineEx({p.r.x, l0.y}, {p.r.x + p.r.width, l0.y}, 1, Fade(kBad, 0.6f));
            series(f, e, a, [](const rr::TestSample& s) { return s.s.gripUse[0]; });
            series(f, e, c, [](const rr::TestSample& s) { return s.s.gripUse[1]; });
            plotSeries(p, a, kFront, 1.6f);
            plotSeries(p, c, kRear, 1.6f);
            break;
        }
        case 5: {  // KERS: energy in the battery
            std::snprintf(title, sizeof title, "KERS battery (MJ)");
            if (ci >= 0) std::snprintf(title, sizeof title, "KERS battery  %.2f MJ", S[ci].s.kersCharge / 1e6f);
            float hi = 1.0f;
            for (int i = f; i < e; ++i) hi = std::max(hi, S[i].s.kersCharge / 1e6f);
            p = plotFrame(box, title, x0, L, 0, std::ceil(hi * 2) / 2, "%.1f", big ? 4 : 2, false);
            series(rf, re, b, [](const rr::TestSample& s) { return s.s.kersCharge / 1e6f; });
            series(f, e, a, [](const rr::TestSample& s) { return s.s.kersCharge / 1e6f; });
            plotSeries(p, b, kRef, 1.5f);
            plotSeries(p, a, Color{255, 170, 60, 255}, 2.0f);
            break;
        }
        case 6: {  // KERS: power, up = deploying, down = harvesting
            std::snprintf(title, sizeof title, "KERS power (kW)");
            if (ci >= 0) {
                const float kw = S[ci].s.kersPowerNow / 1000.0f;
                std::snprintf(title, sizeof title, "KERS power  %s %.0f kW", kw > 1 ? "deploying" : kw < -1 ? "harvesting" : "idle", std::fabs(kw));
            }
            float hi = 60.0f;
            for (int i = f; i < e; ++i) hi = std::max(hi, std::fabs(S[i].s.kersPowerNow) / 1000.0f);
            hi = std::ceil(hi / 30.0f) * 30.0f;
            p = plotFrame(box, title, x0, L, -hi, hi, "%.0f", big ? 4 : 2, false);
            const Vector2 z0 = p.at(x0, 0.0f);
            DrawLineEx({p.r.x, z0.y}, {p.r.x + p.r.width, z0.y}, 1, Fade(kDim, 0.6f));
            series(f, e, a, [](const rr::TestSample& s) { return std::max(0.0f, s.s.kersPowerNow) / 1000.0f; });
            series(f, e, c, [](const rr::TestSample& s) { return std::min(0.0f, s.s.kersPowerNow) / 1000.0f; });
            plotSeries(p, a, Color{255, 170, 60, 255}, 2.0f);
            plotSeries(p, c, kGood, 2.0f);
            break;
        }
        case 7: {  // DRS flap
            std::snprintf(title, sizeof title, "DRS flap");
            if (ci >= 0) std::snprintf(title, sizeof title, "DRS flap  %.0f%% open", S[ci].s.drsFlap * 100.0f);
            p = plotFrame(box, title, x0, L, 0, 1, "%.1f", 2, false);
            series(rf, re, b, [](const rr::TestSample& s) { return s.s.drsFlap; });
            series(f, e, a, [](const rr::TestSample& s) { return s.s.drsFlap; });
            plotSeries(p, b, kRef, 1.5f);
            plotSeries(p, a, kAccent, 2.0f);
            break;
        }
        default: {  // tyre temperatures, with the compound's working window shaded
            std::snprintf(title, sizeof title, "Tyre temperature");
            if (ci >= 0)
                std::snprintf(title, sizeof title, "Tyre temp  front %.0f C  rear %.0f C", S[ci].s.tireTemp[0], S[ci].s.tireTemp[1]);
            float lo = 1e9f, hi = -1e9f;
            for (int i = f; i < e; ++i)
                for (int k = 0; k < 2; ++k) {
                    lo = std::min(lo, S[i].s.tireTemp[k]);
                    hi = std::max(hi, S[i].s.tireTemp[k]);
                }
            const int comp = e > f ? S[f].s.compound : 0;
            const rr::Compound& win = rr::compoundInfo(comp);
            lo = std::floor((std::min(lo, win.tempLo) - 5) / 10) * 10;
            hi = std::ceil((std::max(hi, win.tempHi) + 5) / 10) * 10;
            if (e <= f) lo = 60, hi = 140;
            p = plotFrame(box, title, x0, L, lo, hi, "%.0f", big ? 4 : 2, false);
            const Vector2 w0 = p.at(x0, win.tempHi), w1 = p.at(x0, win.tempLo);
            DrawRectangleRec({p.r.x, w0.y, p.r.width, w1.y - w0.y}, Fade(kGood, 0.16f));
            series(rf, re, b, [](const rr::TestSample& s) { return s.s.tireTemp[0]; });
            series(rf, re, d, [](const rr::TestSample& s) { return s.s.tireTemp[1]; });
            plotSeries(p, b, Fade(kFront, 0.3f), 1.2f);
            plotSeries(p, d, Fade(kRear, 0.3f), 1.2f);
            series(f, e, a, [](const rr::TestSample& s) { return s.s.tireTemp[0]; });
            series(f, e, c, [](const rr::TestSample& s) { return s.s.tireTemp[1]; });
            plotSeries(p, a, kFront, 1.8f);
            plotSeries(p, c, kRear, 1.8f);
            break;
        }
    }
    if (ci >= 0) {
        const Vector2 q = p.at(cur.lapDist, p.y0);
        DrawLineEx({q.x, p.r.y}, {q.x, p.r.y + p.r.height}, 1.5f, Fade(kAccent, 0.9f));
    }
    if (big) hits.push_back({p.r, TestHit::Dist, lap, p.x0, p.x1});
}

// ---------------------------------------------------------------- session graphs

void Hud::sessionGraph(const TestView& tv, Rectangle box, int which, bool big) {
    const rr::TestRecorder& rec = *tv.rec;
    const auto& S = rec.samples;
    const float L = std::max(1.0f, rec.trackLength);
    const rr::TestSample cur = cursorSample(tv);
    const float curX = progressOf(cur, L);
    const float xEnd = std::max((float)tv.laps, S.empty() ? 1.0f : progressOf(S.back(), L));
    const int n = (int)rec.laps.size();
    char title[160];
    Plot p;
    auto cursorLine = [&](const Plot& pl, float x) {
        const Vector2 q = pl.at(x, pl.y0);
        DrawLineEx({q.x, pl.r.y}, {q.x, pl.r.y + pl.r.height}, 1.5f, Fade(kAccent, 0.9f));
    };
    // samples thinned to about two per pixel
    auto overTime = [&](float (*fn)(const rr::TestSample&, int), int k, std::vector<Vector2>& out, float width) {
        out.clear();
        const int stride = std::max(1, (int)(S.size() / std::max(1.0f, 2 * width)));
        for (size_t i = 0; i < S.size(); i += stride) out.push_back({progressOf(S[i], L), fn(S[i], k)});
        if (!S.empty()) out.push_back({progressOf(S.back(), L), fn(S.back(), k)});
    };
    std::vector<Vector2> a, b;
    switch (which) {
        case 0: {  // lap times
            const float avg = rec.averageLap();
            const int best = rec.bestLap();
            if (n == 0) std::snprintf(title, sizeof title, "Lap times: no lap completed yet");
            else std::snprintf(title, sizeof title, "Lap times   best %s (lap %d)   average %s", lapTime(rec.laps[best].time).c_str(),
                               best + 1, lapTime(avg).c_str());
            float lo = 1e9f, slow = 0;
            for (const rr::TestLap& l : rec.laps) {
                lo = std::min(lo, l.time);
                slow = std::max(slow, l.time);
            }
            if (n == 0) lo = slow = 50;
            const float hi = std::max(lo + 1.0f, std::min(lo * 1.08f, slow));
            p = plotFrame(box, title, 0.5f, std::max(2.5f, (float)tv.laps + 0.5f), lo - 0.2f, hi + 0.2f, "%.1f", big ? 4 : 2, true);
            if (avg > 0) {
                const Vector2 q = p.at(p.x0, avg);
                for (float x = p.r.x; x < p.r.x + p.r.width; x += 10) DrawLineEx({x, q.y}, {x + 5, q.y}, 1, kDim);
            }
            std::vector<Vector2> pts;
            for (int i = 0; i < n; ++i) pts.push_back({(float)(i + 1), rec.laps[i].time});
            plotSeries(p, pts, Fade(kText, 0.5f), 1.2f);
            BeginScissorMode((int)p.r.x - 6, (int)p.r.y - 6, (int)p.r.width + 12, (int)p.r.height + 12);
            for (int i = 0; i < n; ++i) {
                const float t = std::min(rec.laps[i].time, p.y1);
                const Vector2 q = p.at((float)(i + 1), t);
                const Color c = i == best ? kBest : !rec.laps[i].clean ? kBad : kText;
                DrawCircleV(q, i == best ? 5.0f : 3.5f, c);
                if (rec.laps[i].time > p.y1) DrawTriangle({q.x, q.y - 9}, {q.x - 4, q.y - 3}, {q.x + 4, q.y - 3}, c);
            }
            EndScissorMode();
            cursorLine(p, (float)cur.lap);
            break;
        }
        case 1: {  // sector times against the best of each sector
            float bestS[3] = {1e9f, 1e9f, 1e9f};
            for (const rr::TestLap& l : rec.laps)
                for (int k = 0; k < 3; ++k)
                    if (l.sectors[k] > 0) bestS[k] = std::min(bestS[k], l.sectors[k]);
            std::snprintf(title, sizeof title, "Sectors: time lost to the best of each   S1 %s  S2 %s  S3 %s",
                          bestS[0] < 1e8f ? lapTime(bestS[0]).c_str() + 2 : "-", bestS[1] < 1e8f ? lapTime(bestS[1]).c_str() + 2 : "-",
                          bestS[2] < 1e8f ? lapTime(bestS[2]).c_str() + 2 : "-");
            p = plotFrame(box, title, 0.5f, std::max(2.5f, (float)tv.laps + 0.5f), 0, 1.5f, "+%.1f", 3, true);
            const Color sc[3] = {Color{255, 90, 90, 255}, Color{90, 200, 255, 255}, Color{250, 210, 60, 255}};
            for (int k = 0; k < 3; ++k) {
                std::vector<Vector2> pts;
                for (int i = 0; i < n; ++i)
                    if (rec.laps[i].sectors[k] > 0) pts.push_back({(float)(i + 1), rec.laps[i].sectors[k] - bestS[k]});
                plotSeries(p, pts, sc[k], 2);
                for (const Vector2& v : pts) DrawCircleV(p.at(v.x, std::min(v.y, p.y1)), 3, sc[k]);
                char lab[8];
                std::snprintf(lab, sizeof lab, "S%d", k + 1);
                text(lab, p.r.x + p.r.width - 90 + k * 30, p.r.y + 4, 12, sc[k], true);
            }
            cursorLine(p, (float)cur.lap);
            break;
        }
        case 2: {  // fuel
            const float fpl = rec.fuelPerLap();
            const float start = S.empty() ? 1 : S.front().s.fuel;
            float dry = 0;
            if (fpl > 0) dry = curX + cur.s.fuel / fpl;
            if (fpl <= 0) std::snprintf(title, sizeof title, "Fuel   %.1f L", cur.s.fuel);
            else if (dry >= tv.laps)
                std::snprintf(title, sizeof title, "Fuel   %.1f L   avg %.2f L/lap   %.1f L spare at the flag", cur.s.fuel, fpl,
                              cur.s.fuel - (tv.laps - curX) * fpl);
            else std::snprintf(title, sizeof title, "Fuel   %.1f L   avg %.2f L/lap   runs dry at lap %.1f", cur.s.fuel, fpl, dry);
            p = plotFrame(box, title, 0, xEnd, 0, std::max(5.0f, std::ceil(start / 5) * 5), "%.0f", big ? 4 : 2, true);
            overTime([](const rr::TestSample& s, int) { return s.s.fuel; }, 0, a, p.r.width);
            plotSeries(p, a, Color{90, 170, 235, 255}, 2);
            if (fpl > 0) {  // projection at the average rate
                const float xe = std::min(xEnd, dry);
                std::vector<Vector2> pr = {{curX, cur.s.fuel}, {xe, cur.s.fuel - (xe - curX) * fpl}};
                plotSeries(p, pr, Fade(Color{90, 170, 235, 255}, 0.45f), 1.5f);
            }
            cursorLine(p, curX);
            break;
        }
        case 3: {  // tyre wear
            float w[2];
            rec.wearPerLap(w);
            const float worst = std::max(w[0], w[1]);
            const float cliff = worst > 1e-5f ? curX + (0.7f - std::max(cur.s.tireWear[0], cur.s.tireWear[1])) / worst : 0;
            if (worst <= 1e-5f)
                std::snprintf(title, sizeof title, "Tyre wear   F %.3f  R %.3f", cur.s.tireWear[0], cur.s.tireWear[1]);
            else
                std::snprintf(title, sizeof title, "Tyre wear   avg +%.3f F  +%.3f R a lap   cliff (0.7) at lap %.1f", w[0], w[1], cliff);
            float top = 0.8f;
            for (const rr::TestSample& s : S) top = std::max(top, std::max(s.s.tireWear[0], s.s.tireWear[1]) * 1.05f);
            p = plotFrame(box, title, 0, xEnd, 0, std::min(1.0f, top), "%.1f", big ? 4 : 2, true);
            const Vector2 c0 = p.at(0, 0.7f);
            DrawLineEx({p.r.x, c0.y}, {p.r.x + p.r.width, c0.y}, 1, Fade(kBad, 0.7f));
            overTime([](const rr::TestSample& s, int k) { return s.s.tireWear[k]; }, 0, a, p.r.width);
            overTime([](const rr::TestSample& s, int k) { return s.s.tireWear[k]; }, 1, b, p.r.width);
            plotSeries(p, a, kFront, 2);
            plotSeries(p, b, kRear, 2);
            if (worst > 1e-5f)
                for (int k = 0; k < 2; ++k) {
                    std::vector<Vector2> pr = {{curX, cur.s.tireWear[k]}, {xEnd, cur.s.tireWear[k] + (xEnd - curX) * w[k]}};
                    plotSeries(p, pr, Fade(k ? kRear : kFront, 0.4f), 1.2f);
                }
            text("F", p.r.x + p.r.width - 40, p.r.y + 4, 12, kFront, true);
            text("R", p.r.x + p.r.width - 24, p.r.y + 4, 12, kRear, true);
            cursorLine(p, curX);
            break;
        }
        default: {  // tyre temperatures and the compound's window
            const rr::Compound& cw = rr::compoundInfo(cur.s.compound);
            std::snprintf(title, sizeof title, "Tyre temperature   F %.0f C  R %.0f C   window %.0f-%.0f C (%s)", cur.s.tireTemp[0],
                          cur.s.tireTemp[1], cw.tempLo, cw.tempHi, compoundName(cur.s.compound));
            p = plotFrame(box, title, 0, xEnd, 60, 140, "%.0f", 4, true);
            const Vector2 w0 = p.at(0, cw.tempHi), w1 = p.at(0, cw.tempLo);
            DrawRectangleRec({p.r.x, w0.y, p.r.width, w1.y - w0.y}, Fade(kGood, 0.12f));
            overTime([](const rr::TestSample& s, int k) { return s.s.tireTemp[k]; }, 0, a, p.r.width);
            overTime([](const rr::TestSample& s, int k) { return s.s.tireTemp[k]; }, 1, b, p.r.width);
            plotSeries(p, a, kFront, 1.5f);
            plotSeries(p, b, kRear, 1.5f);
            cursorLine(p, curX);
            break;
        }
    }
}

// ---------------------------------------------------------------- track map

void Hud::drawTrackMap(const TestView& tv, Rectangle box, int lap, bool big) {
    const rr::TestRecorder& rec = *tv.rec;
    const float pad = big ? 30 : 12;
    const float sx = maxX_ - minX_, sy = maxY_ - minY_;
    const float scale = std::min((box.width - 2 * pad) / std::max(1.0f, sx), (box.height - 2 * pad) / std::max(1.0f, sy));
    const float ox = box.x + (box.width - sx * scale) / 2, oy = box.y + (box.height - sy * scale) / 2;
    auto P = [&](rr::Vec2 p) { return Vector2{ox + (p.x - minX_) * scale, oy + (maxY_ - p.y) * scale}; };
    const float thick = std::max(3.0f, trackWidth_ * scale);
    for (size_t i = 0; i < outline_.size(); ++i)
        DrawLineEx(P(outline_[i]), P(outline_[(i + 1) % outline_.size()]), thick, Color{60, 64, 74, 255});
    int f = 0, e = 0;
    rec.lapRange(lap, f, e);
    const auto& S = rec.samples;
    for (int i = f + 1; i < e; ++i) {
        const rr::TestSample& s = S[i];
        Color c;
        if (tv.mapColour == 1) c = s.brake > 0.05f ? kBrake : s.accel > 0.95f ? kThrottle : s.accel > 0.05f ? Color{170, 200, 90, 255} : kDim;
        else if (tv.mapColour == 2) {
            const float g = std::max(s.s.gripUse[0], s.s.gripUse[1]);
            c = g > 1.1f ? kBad : g > 0.9f ? kAccent : g > 0.6f ? kGood : Color{70, 120, 220, 255};
        } else c = speedColour(speedOf(s));
        DrawLineEx(P(S[i - 1].s.pos), P(s.s.pos), big ? 3.5f : 2.5f, c);
    }
    for (const rr::TestEvent& ev : rec.events) {
        if (ev.lap != lap && !big) continue;
        const int k = rec.indexAt(ev.t);
        if (k < 0) continue;
        const Vector2 q = P(S[k].s.pos);
        const bool here = ev.lap == lap;
        DrawCircleLinesV(q, here ? (big ? 7.0f : 5.0f) : 4.0f, Fade(eventColour(ev.kind), here ? 1.0f : 0.35f));
    }
    // start line and the car
    if (!S.empty()) {
        const rr::TestSample cur = cursorSample(tv);
        const Vector2 q = P(cur.s.pos);
        DrawCircleV(q, big ? 7 : 5, kText);
        DrawCircleV(q, big ? 4.5f : 3, kAccent);
    }
    // live delta to the fastest lap (another lap than this one) at this point
    const int best = refLapFor(rec, 0, lap);
    const float rx = box.x + box.width - (big ? 14 : 8), ry = box.y + (big ? 10 : 2);
    char buf[64];
    bool ok = false;
    float d = 0;
    if (best > 0 && best != lap && !S.empty()) {
        int bf, be;
        rec.lapRange(best, bf, be);
        const rr::TestSample cur = cursorSample(tv);
        if (cur.lap == lap) d = cur.lapTime - timeAtDist(rec, bf, be, cur.lapDist, &ok);
        // at the line of a finished lap: the lap times themselves
        if (!ok && cur.lap == lap && lap <= (int)rec.laps.size() && be > bf && cur.lapDist >= S[be - 1].lapDist) {
            d = rec.laps[lap - 1].time - rec.laps[best - 1].time;
            ok = true;
        }
    }
    if (ok) {
        std::snprintf(buf, sizeof buf, "%+.3f", d);
        textRight(buf, rx, ry, big ? 34 : 20, d <= 0 ? kGood : kBad, true, true);
        std::snprintf(buf, sizeof buf, "to best (lap %d)", best);
        textRight(buf, rx, ry + (big ? 38 : 22), big ? 13 : 11, kDim);
    } else {
        textRight("no best lap yet", rx, ry + 2, big ? 14 : 11, kFaint);
    }
}

// ---------------------------------------------------------------- panels

void Hud::drawTestSession(const rr::Race& race, const TestView& tv, std::vector<TestHit>& hits) {
    const rr::TestRecorder& rec = *tv.rec;
    const float x = 16, y = 16, w = 372;
    const float bottom = GetScreenHeight() - 90.0f - 276 - 18;
    const float content = 196 + 20.0f * (rec.laps.size() + 1) + (tv.message.empty() ? 0 : 36) + 10;
    const float h = std::max(200.0f, std::min(bottom - y, content));
    panel({x, y, w, h}, 0.72f);
    char buf[160];
    text(tv.replay ? "TESTING  REPLAY" : "TESTING", x + 14, y + 10, 14, kAccent, true);
    textRight(race.track().name().c_str(), x + w - 14, y + 10, 14, kDim);
    {
        float size = 18;
        while (size > 12 && width(tv.title.c_str(), size, true) > w - 28) size -= 1;
        text(tv.title.c_str(), x + 14, y + 30, size, kText, true);
        size = 13;
        while (size > 10 && width(tv.setup.c_str(), size) > w - 28) size -= 1;
        text(tv.setup.c_str(), x + 14, y + 54, size, kDim);
    }
    const rr::TestSample cur = cursorSample(tv);
    const int lap = rec.empty() ? 1 : cur.lap;
    std::snprintf(buf, sizeof buf, "LAP %d / %d", std::min(lap, std::max(1, tv.laps)), tv.laps);
    text(buf, x + 14, y + 76, 24, kText, true);
    if (!rec.empty() && lap <= tv.laps) textRight(lapTime(std::max(0.0f, cur.lapTime)).c_str(), x + w - 14, y + 80, 20, kText, false, true);
    // delta to the comparison lap at this point of the lap
    const int ref = refLapFor(rec, tv.compareLap, lap);
    if (ref > 0 && ref != lap) {
        int rf, re;
        rec.lapRange(ref, rf, re);
        bool ok = false;
        const float tr = timeAtDist(rec, rf, re, cur.lapDist, &ok);
        if (ok) {
            const float d = cur.lapTime - tr;
            std::snprintf(buf, sizeof buf, "%+.3f to lap %d", d, ref);
            textRight(buf, x + w - 14, y + 104, 14, d <= 0 ? kGood : kBad, true, true);
        }
    }
    float ly = y + 128;
    const int best = rec.bestLap();
    float wpl[2];
    rec.wearPerLap(wpl);
    std::snprintf(buf, sizeof buf, "BEST %s", best >= 0 ? lapTime(rec.laps[best].time).c_str() : "-:--.---");
    text(buf, x + 14, ly, 14, best >= 0 ? kBest : kDim, true, true);
    std::snprintf(buf, sizeof buf, "AVG %s", lapTime(rec.averageLap()).c_str());
    textRight(buf, x + w - 14, ly, 14, kDim, false, true);
    ly += 20;
    std::snprintf(buf, sizeof buf, "FUEL %.2f L/lap", rec.fuelPerLap());
    text(buf, x + 14, ly, 13, kDim, false, true);
    std::snprintf(buf, sizeof buf, "WEAR %.3f / %.3f", wpl[0], wpl[1]);
    textRight(buf, x + w - 14, ly, 13, kDim, false, true);
    ly += 26;
    // laps: newest at the bottom; click one to jump to it
    text("LAP", x + 14, ly, 12, kFaint, true);
    text("TIME", x + 56, ly, 12, kFaint, true);
    text("GAP", x + 150, ly, 12, kFaint, true);
    text("FUEL", x + 222, ly, 12, kFaint, true);
    text("WEAR", x + 278, ly, 12, kFaint, true);
    ly += 18;
    const float rowH = 20;
    const float msgH = tv.message.empty() ? 0 : 36;
    const int room = std::max(1, (int)((y + h - msgH - 10 - ly) / rowH));
    const int n = (int)rec.laps.size();
    const int first = std::max(0, n + 1 - room);
    const Vector2 mp = GetMousePosition();
    const int rows = tv.runOver || tv.replay ? n - 1 : n;
    for (int i = first; i <= rows && ly + rowH <= y + h - msgH; ++i, ly += rowH) {
        const Rectangle rr_ = {x + 6, ly - 2, w - 12, rowH};
        const bool hover = CheckCollisionPointRec(mp, rr_);
        if (i + 1 == lap) DrawRectangleRec(rr_, Fade(WHITE, 0.10f));
        else if (hover) DrawRectangleRec(rr_, Fade(WHITE, 0.05f));
        hits.push_back({rr_, TestHit::Lap, i + 1});
        std::snprintf(buf, sizeof buf, "%d", i + 1);
        textRight(buf, x + 40, ly, 14, kDim, false, true);
        if (i == n) {
            text(tv.runOver ? "" : "running", x + 56, ly, 14, kFaint);
            continue;
        }
        const rr::TestLap& l = rec.laps[i];
        text(lapTime(l.time).c_str(), x + 56, ly, 14, i == best ? kBest : l.clean ? kText : kBad, false, true);
        if (best >= 0 && i != best) {
            std::snprintf(buf, sizeof buf, "+%.3f", l.time - rec.laps[best].time);
            text(buf, x + 150, ly, 14, kDim, false, true);
        }
        std::snprintf(buf, sizeof buf, "%.2f", l.fuelUsed);
        text(buf, x + 222, ly, 14, kDim, false, true);
        std::snprintf(buf, sizeof buf, "%.3f", std::max(l.wear[0], l.wear[1]));
        text(buf, x + 278, ly, 14, kDim, false, true);
        if (!l.clean) text("!", x + w - 22, ly, 14, kBad, true);
    }
    if (!tv.message.empty()) {
        float size = 13;
        while (size > 9 && width(tv.message.c_str(), size) > w - 28) size -= 1;
        text(tv.message.c_str(), x + 14, y + h - 30, size, kAccent);
    }
}

void Hud::drawTimeline(const TestView& tv, std::vector<TestHit>& hits) {
    const rr::TestRecorder& rec = *tv.rec;
    const float sw = (float)GetScreenWidth(), sh = (float)GetScreenHeight();
    const Rectangle box = {16, sh - 82, sw - 32, 68};
    panel(box, 0.8f);
    const float t1 = std::max(1.0, rec.endTime());
    const Rectangle bar = {box.x + 16, box.y + 34, box.width - 32, 12};
    auto X = [&](double t) { return bar.x + (float)(t / t1) * bar.width; };
    const rr::TestSample cur = cursorSample(tv);
    const double ct = tv.live ? rec.endTime() : tv.cursor;
    char buf[200];
    std::snprintf(buf, sizeof buf, "%s   lap %d   %.0f m", ct > 0 ? lapTime(ct).c_str() : "0:00.000", cur.lap,
                  std::max(0.0f, cur.lapDist));
    text(buf, box.x + 16, box.y + 8, 15, kText, true, true);
    const char* help = tv.runOver || tv.replay
                           ? "Space play / pause   Left/Right 1 s (Shift 10 s)   PgUp/PgDn lap   Home/End   G graphs   Tab windows   Esc menu"
                           : "Space pause   drag or Left/Right to scrub   PgUp/PgDn lap   End live   F fast forward   G graphs   Tab windows   R restart";
    float size = 13;
    while (size > 10 && width(help, size) > box.width - 360) size -= 1;
    textRight(help, box.x + box.width - 16, box.y + 10, size, kDim);
    DrawRectangleRec(bar, Fade(WHITE, 0.10f));
    DrawRectangleRec({bar.x, bar.y, X(ct) - bar.x, bar.height}, Fade(kAccent, 0.35f));
    // laps
    for (int i = 0; i < (int)rec.laps.size(); ++i) {
        const int k = rec.laps[i].endSample;
        if (k >= (int)rec.samples.size()) continue;
        const float lx = X(rec.samples[k].t);
        DrawLineEx({lx, bar.y - 4}, {lx, bar.y + bar.height + 2}, 1, Fade(WHITE, 0.5f));
    }
    // events below the bar
    for (const rr::TestEvent& e : rec.events) {
        const float ex = X(e.t), ew = std::max(2.0f, X(e.t + e.duration) - ex);
        DrawRectangleRec({ex, bar.y + bar.height + 4, ew, 6}, eventColour(e.kind));
    }
    // the cursor
    const float cx = X(ct);
    DrawLineEx({cx, bar.y - 8}, {cx, bar.y + bar.height + 10}, 2, kText);
    DrawCircleV({cx, bar.y + bar.height / 2}, 6, tv.live ? kGood : kAccent);
    hits.push_back({{bar.x - 6, box.y + 26, bar.width + 12, 40}, TestHit::Timeline, 0, 0, (float)t1});
}

void Hud::drawDashboard(const TestView& tv, Rectangle area, std::vector<TestHit>& hits) {
    const int cols = 2, rows = 5;
    const float gap = 8;
    const float tw = (area.width - gap * (cols - 1)) / cols, th = (area.height - gap * (rows - 1)) / rows;
    const rr::TestRecorder& rec = *tv.rec;
    const int lap = rec.empty() ? 1 : cursorSample(tv).lap;
    const int ref = refLapFor(rec, tv.compareLap, lap);
    const Vector2 mp = GetMousePosition();
    for (int k = 0; k < cols * rows; ++k) {
        const Rectangle r = {area.x + (k % cols) * (tw + gap), area.y + (k / cols) * (th + gap), tw, th};
        const bool hover = CheckCollisionPointRec(mp, r);
        DrawRectangleRounded(r, 6.0f / std::min(r.width, r.height), 6, Fade(kPanel, hover ? 0.85f : 0.72f));
        const Rectangle in = {r.x + 4, r.y + 4, r.width - 8, r.height - 8};
        int window = 1;
        switch (k) {
            case 0: distGraph(tv, in, 0, lap, ref, false, hits); break;
            case 1: distGraph(tv, in, 1, lap, ref, false, hits); break;
            case 2: distGraph(tv, in, 3, lap, ref, false, hits); break;
            case 3: distGraph(tv, in, 4, lap, ref, false, hits); break;
            case 4: distGraph(tv, in, 5, lap, ref, false, hits); break;
            case 5: distGraph(tv, in, 6, lap, ref, false, hits); break;
            case 6: sessionGraph(tv, in, 0, false); window = 2; break;
            case 7: sessionGraph(tv, in, 2, false); window = 2; break;
            case 8: sessionGraph(tv, in, 3, false); window = 2; break;
            default:
                text("Track: speed this lap", in.x + 6, in.y + 2, 14, kText, true);
                drawTrackMap(tv, {in.x, in.y + 20, in.width, in.height - 20}, lap, false);
                window = 3;
                break;
        }
        hits.push_back({r, TestHit::Tile, window});
    }
}

void Hud::drawDrivingWindow(const TestView& tv, Rectangle area, std::vector<TestHit>& hits) {
    const rr::TestRecorder& rec = *tv.rec;
    const int lap = rec.empty() ? 1 : cursorSample(tv).lap;
    const int ref = refLapFor(rec, tv.compareLap, lap);
    char buf[200];
    if (ref > 0 && ref != lap)
        std::snprintf(buf, sizeof buf, "DRIVING   lap %d against lap %d%s", lap, ref, tv.compareLap == 0 ? " (best)" : "");
    else std::snprintf(buf, sizeof buf, "DRIVING   lap %d", lap);
    text(buf, area.x + 16, area.y + 12, 20, kAccent, true);
    textRight("[ / ] lap to compare   PgUp/PgDn lap shown   click a graph to move there", area.x + area.width - 60, area.y + 16, 14, kDim);
    const int n = 8;
    const float top = area.y + 44, gap = 6, gh = (area.height - 44 - 10 - gap * (n - 1)) / n;
    const int order[n] = {0, 1, 2, 3, 4, 5, 6, 7};
    for (int i = 0; i < n; ++i)
        distGraph(tv, {area.x + 10, top + i * (gh + gap), area.width - 20, gh}, order[i], lap, ref, true, hits);
}

void Hud::drawSessionWindow(const TestView& tv, Rectangle area, std::vector<TestHit>& hits) {
    text("SESSION", area.x + 16, area.y + 12, 20, kAccent, true);
    textRight("lap times, sectors, fuel and tyres over the run (dashed: the average rate carried on)", area.x + area.width - 60,
              area.y + 16, 14, kDim);
    const int n = 5;
    const float top = area.y + 44, gap = 6, gh = (area.height - 44 - 10 - gap * (n - 1)) / n;
    for (int i = 0; i < n; ++i) sessionGraph(tv, {area.x + 10, top + i * (gh + gap), area.width - 20, gh}, i, true);
    (void)hits;
}

void Hud::drawTrackWindow(const TestView& tv, Rectangle area, std::vector<TestHit>& hits) {
    const rr::TestRecorder& rec = *tv.rec;
    const int lap = rec.empty() ? 1 : cursorSample(tv).lap;
    static const char* modes[] = {"speed", "throttle and brake", "grip use"};
    char buf[200];
    std::snprintf(buf, sizeof buf, "TRACK AND EVENTS   lap %d, coloured by %s", lap, modes[tv.mapColour % 3]);
    text(buf, area.x + 16, area.y + 12, 20, kAccent, true);
    textRight("M colour   click an event to go there", area.x + area.width - 60, area.y + 16, 14, kDim);
    const Rectangle map = {area.x + 10, area.y + 44, area.width * 0.52f, area.height - 54};
    DrawRectangleRec(map, Fade(BLACK, 0.2f));
    drawTrackMap(tv, map, lap, true);
    // legend
    float lx = map.x + 12, lyy = map.y + map.height - 22;
    if (tv.mapColour == 0) {
        for (int k = 0; k <= 26; ++k) DrawRectangle((int)(lx + k * 6), (int)lyy, 6, 10, speedColour(60 + k * 10.0f));
        text("60", lx, lyy - 16, 12, kDim);
        textRight("320 km/h", lx + 162, lyy - 16, 12, kDim);
    } else if (tv.mapColour == 1) {
        const std::pair<Color, const char*> l[] = {{kThrottle, "full throttle"}, {Color{170, 200, 90, 255}, "part"},
                                                    {kDim, "coasting"}, {kBrake, "braking"}};
        for (auto& [c, s] : l) {
            DrawRectangle((int)lx, (int)lyy, 12, 10, c);
            text(s, lx + 16, lyy - 2, 12, kDim);
            lx += width(s, 12) + 34;
        }
    } else {
        const std::pair<Color, const char*> l[] = {{Color{70, 120, 220, 255}, "< 0.6"}, {kGood, "0.6-0.9"}, {kAccent, "at the limit"},
                                                    {kBad, "sliding"}};
        for (auto& [c, s] : l) {
            DrawRectangle((int)lx, (int)lyy, 12, 10, c);
            text(s, lx + 16, lyy - 2, 12, kDim);
            lx += width(s, 12) + 34;
        }
    }
    // events list
    const float ex = map.x + map.width + 16, ew = area.x + area.width - 10 - ex;
    float ey = area.y + 48;
    std::snprintf(buf, sizeof buf, "EVENTS (%d)", (int)rec.events.size());
    text(buf, ex, ey, 15, kText, true);
    ey += 22;
    text("TIME", ex, ey, 12, kFaint, true);
    text("LAP", ex + 80, ey, 12, kFaint, true);
    text("AT", ex + 116, ey, 12, kFaint, true);
    text("WHAT", ex + 172, ey, 12, kFaint, true);
    text("FOR", ex + ew - 150, ey, 12, kFaint, true);
    text("PEAK", ex + ew - 80, ey, 12, kFaint, true);
    ey += 18;
    const float listH = (area.y + area.height) * 0.5f - ey + area.y * 0.5f;
    const float rowH = 19;
    const int room = std::max(1, (int)(listH / rowH));
    const int ne = (int)rec.events.size();
    const int top = std::clamp(tv.eventTop, 0, std::max(0, ne - room));
    const Vector2 mp = GetMousePosition();
    for (int i = top; i < ne && i < top + room; ++i, ey += rowH) {
        const rr::TestEvent& e = rec.events[i];
        const Rectangle r = {ex - 4, ey - 2, ew, rowH};
        const bool here = !tv.live && tv.cursor >= e.t && tv.cursor <= e.t + std::max(0.5f, e.duration);
        if (here) DrawRectangleRec(r, Fade(WHITE, 0.10f));
        else if (CheckCollisionPointRec(mp, r)) DrawRectangleRec(r, Fade(WHITE, 0.05f));
        hits.push_back({r, TestHit::Event, i, e.t});
        text(lapTime(e.t).c_str(), ex, ey, 13, kDim, false, true);
        std::snprintf(buf, sizeof buf, "%d", e.lap);
        text(buf, ex + 80, ey, 13, kDim, false, true);
        std::snprintf(buf, sizeof buf, "%.0f m", std::max(0.0f, e.lapDist));
        text(buf, ex + 116, ey, 13, kDim, false, true);
        DrawRectangle((int)ex + 172, (int)ey + 3, 8, 10, eventColour(e.kind));
        text(eventName(e.kind), ex + 186, ey, 13, kText);
        std::snprintf(buf, sizeof buf, "%.2f s", e.duration);
        text(buf, ex + ew - 150, ey, 13, kDim, false, true);
        std::snprintf(buf, sizeof buf, "%.2f", e.peak);
        text(buf, ex + ew - 80, ey, 13, kDim, false, true);
    }
    if (ne == 0) text("nothing flagged: no off-track moments, contact, slides or spins", ex, ey, 13, kDim);
    if (ne > room) {
        std::snprintf(buf, sizeof buf, "%d-%d of %d (mouse wheel)", top + 1, std::min(ne, top + room), ne);
        text(buf, ex, ey + 4, 12, kFaint);
    }
    // lap table
    float ty = area.y + area.height * 0.5f + 20;
    text("LAPS", ex, ty, 15, kText, true);
    ty += 22;
    const char* cols[] = {"LAP", "TIME", "S1", "S2", "S3", "FUEL", "WEAR F/R", "TEMP F/R", "TOP", "FLAGS"};
    const float cx[] = {0, 34, 110, 170, 230, 290, 340, 430, 510, 560};
    for (int k = 0; k < 10; ++k) text(cols[k], ex + cx[k], ty, 12, kFaint, true);
    ty += 18;
    const int nl = (int)rec.laps.size();
    const int roomL = std::max(1, (int)((area.y + area.height - 10 - ty) / 18));
    const int firstL = std::max(0, nl - roomL);
    const int best = rec.bestLap();
    for (int i = firstL; i < nl; ++i, ty += 18) {
        const rr::TestLap& l = rec.laps[i];
        const Rectangle r = {ex - 4, ty - 2, ew, 18};
        if (i + 1 == lap) DrawRectangleRec(r, Fade(WHITE, 0.10f));
        hits.push_back({r, TestHit::Lap, i + 1});
        std::snprintf(buf, sizeof buf, "%d", l.lap);
        text(buf, ex, ty, 13, kDim, false, true);
        text(lapTime(l.time).c_str(), ex + 34, ty, 13, i == best ? kBest : kText, false, true);
        for (int k = 0; k < 3; ++k) {
            std::snprintf(buf, sizeof buf, "%.2f", l.sectors[k]);
            text(buf, ex + cx[2 + k], ty, 13, kDim, false, true);
        }
        std::snprintf(buf, sizeof buf, "%.2f", l.fuelUsed);
        text(buf, ex + cx[5], ty, 13, kDim, false, true);
        std::snprintf(buf, sizeof buf, "%.3f/%.3f", l.wear[0], l.wear[1]);
        text(buf, ex + cx[6], ty, 13, kDim, false, true);
        std::snprintf(buf, sizeof buf, "%.0f/%.0f", l.tempAvg[0], l.tempAvg[1]);
        text(buf, ex + cx[7], ty, 13, kDim, false, true);
        std::snprintf(buf, sizeof buf, "%.0f", l.topSpeed);
        text(buf, ex + cx[8], ty, 13, kDim, false, true);
        buf[0] = 0;
        if (l.offTracks) std::snprintf(buf + std::strlen(buf), sizeof buf - std::strlen(buf), "off %d ", l.offTracks);
        if (l.slides) std::snprintf(buf + std::strlen(buf), sizeof buf - std::strlen(buf), "slides %d ", l.slides);
        if (l.damage > 1) std::snprintf(buf + std::strlen(buf), sizeof buf - std::strlen(buf), "dmg %.0f", l.damage);
        text(buf, ex + cx[9], ty, 13, l.clean ? kDim : kBad);
    }
}

void Hud::drawTest(const rr::Race& race, const HudState& st, const TestView& tv, std::vector<TestHit>& hits) {
    hits.clear();
    if (!tv.rec) return;
    animate(race);
    const float sw = (float)GetScreenWidth(), sh = (float)GetScreenHeight();
    if (st.showHud) {
        if (tv.window == 0) {
            drawTestSession(race, tv, hits);
            if (!tv.replay) drawCarPanel(race, st, 16, sh - 90 - 276);
            else drawCarPanel(race, st, 16, sh - 90 - 276);
            const float dashX = std::max(400.0f, sw * (2.0f / 3.0f));  // the graphs take a third of the screen
            if (tv.dashboard) drawDashboard(tv, {dashX, 16, sw - 16 - dashX, sh - 90 - 16 - 8}, hits);
            // state, top centre of the 3D view
            char buf[120];
            const char* what = tv.replay    ? (tv.playing ? "REPLAY  PLAYING" : "REPLAY")
                               : tv.live    ? (st.paused ? "PAUSED" : "LIVE")
                               : tv.playing ? "PLAYBACK"
                               : tv.runOver ? "RUN FINISHED"
                                            : "SCRUBBING";
            if (tv.fastForward && tv.live && !st.paused) std::snprintf(buf, sizeof buf, "%s   FAST FORWARD", what);
            else std::snprintf(buf, sizeof buf, "%s   x%g   %s CAM", what, st.timeScale, camName(st.camera));
            const float cxm = tv.dashboard ? (388 + dashX) / 2 : sw / 2;
            const float w = width(buf, 15, true) + 28;
            panel({cxm - w / 2, 12, w, 30});
            text(buf, cxm - w / 2 + 14, 18, 15, tv.live && !st.paused ? kGood : kAccent, true);
        } else {
            const Rectangle area = {16, 16, sw - 32, sh - 90 - 24};
            panel(area, 0.93f);
            if (tv.window == 1) drawDrivingWindow(tv, area, hits);
            else if (tv.window == 2) drawSessionWindow(tv, area, hits);
            else drawTrackWindow(tv, area, hits);
            const Rectangle close = {area.x + area.width - 42, area.y + 10, 30, 26};
            DrawRectangleRounded(close, 0.3f, 6, Fade(WHITE, 0.12f));
            text("X", close.x + 10, close.y + 4, 16, kText, true);
            hits.push_back({close, TestHit::Close, 0});
        }
    }
    drawTimeline(tv, hits);
    if (st.showHelp) drawHelp();
}

// ---------------------------------------------------------------- menu pages

void Hud::drawTestStatsPage(const MenuState& m, std::vector<MenuHit>& hits) {
    const float sw = (float)GetScreenWidth(), sh = (float)GetScreenHeight();
    DrawRectangle(0, 0, (int)sw, (int)sh, Fade(Color{8, 10, 16, 255}, 0.5f));
    const StatRules& r = m.statRules;
    const int ns = (int)r.keys.size();
    const float w = std::min(sw - 20, 1240.0f), rowH = 46;
    const float h = std::min(sh - 20, 150 + std::max(ns * rowH + 150, (float)m.figures.size() * 30 + 80) + 70);
    const float x = (sw - w) / 2, y = std::max(10.0f, (sh - h) / 2);
    panel({x, y, w, h}, 0.92f);
    text("TESTING SETUP", x + 28, y + 22, 18, kAccent, true);
    text("Car stats", x + 28, y + 44, 32, kText, true);
    int sum = 0;
    for (int v : m.testStats) sum += v;
    char buf[200];
    std::snprintf(buf, sizeof buf, "%d of %d points used. Each stat 0-%d, %d is the stock car. Highlighted figures are the ones the selected stat changes.",
                  sum, r.budget, r.max, r.neutral);
    text(buf, x + 28, y + 88, 14, kDim);
    // stats
    const float lx = x + 28, lw = 470;
    float ry = y + 150;
    for (int k = 0; k < ns; ++k, ry += rowH) {
        const bool sel = k == m.testStatRow;
        const Rectangle row = {lx - 8, ry - 6, lw, rowH - 6};
        if (sel) DrawRectangleRounded(row, 0.25f, 6, Fade(kAccent, 0.16f));
        const int v = k < (int)m.testStats.size() ? m.testStats[k] : r.neutral;
        text(r.labels[k].c_str(), lx, ry + 4, 18, sel ? kText : kDim, true);
        const float vx = lx + 240;
        // minus and plus buttons
        for (int side = 0; side < 2; ++side) {
            const Rectangle b = {side ? vx + 54 : vx - 6, ry - 2, 30, 30};
            const bool hot = CheckCollisionPointRec(GetMousePosition(), b);
            DrawRectangleRounded(b, 0.35f, 6, hot ? Color{44, 50, 66, 255} : Color{30, 34, 46, 255});
            const float cx = b.x + 15, cy = b.y + 15;
            const Color ic = hot || sel ? kText : kDim;
            DrawRectangle((int)cx - 6, (int)cy - 1, 12, 2, ic);
            if (side) DrawRectangle((int)cx - 1, (int)cy - 6, 2, 12, ic);
        }
        std::snprintf(buf, sizeof buf, "%d", v);
        const Color vc = v > r.neutral ? kGood : v < r.neutral ? kBad : kText;
        text(buf, vx + 36 - width(buf, 20, true) / 2, ry + 3, 20, vc, true);
        const float bx = vx + 96, bw = lw - (bx - lx) - 20;
        DrawRectangle((int)bx, (int)ry + 12, (int)bw, 6, Fade(WHITE, 0.12f));
        DrawRectangle((int)bx, (int)ry + 12, (int)(bw * v / std::max(1, r.max)), 6, vc);
        DrawRectangle((int)(bx + bw * r.neutral / std::max(1, r.max)) - 1, (int)ry + 8, 2, 14, Fade(WHITE, 0.5f));
        hits.push_back({vx - 8, ry - 6, 40, rowH - 6, 2000 + k, -1});
        hits.push_back({vx + 50, ry - 6, 40, rowH - 6, 2000 + k, 1});
        hits.push_back({row.x, row.y, vx - 8 - row.x, row.height, 2000 + k, 0});
    }
    // about the selected stat
    if (m.testStatRow < (int)m.statAbout.size()) {
        ry += 6;
        const std::string& about = m.statAbout[m.testStatRow];
        // wrap to the column
        std::string line, word;
        float yy = ry;
        auto flush = [&]() { text(line.c_str(), lx, yy, 15, kDim); yy += 20; line.clear(); };
        for (size_t i = 0; i <= about.size(); ++i) {
            const char ch = i < about.size() ? about[i] : ' ';
            if (ch != ' ') { word += ch; continue; }
            const std::string next = line.empty() ? word : line + " " + word;
            if (width(next.c_str(), 15) > lw - 10 && !line.empty()) { flush(); line = word; }
            else line = next;
            word.clear();
        }
        if (!line.empty()) flush();
    }
    std::snprintf(buf, sizeof buf, "%d", r.budget - sum);
    text("Points left", lx, y + h - 120, 16, kDim, true);
    text(buf, lx + 110, y + h - 122, 20, r.budget - sum > 0 ? kAccent : kDim, true);
    // figures
    const float fx = lx + lw + 30, fw = x + w - 28 - fx;
    float fy = y + 150;
    text("FIGURE", fx, fy - 26, 12, kFaint, true);
    textRight("STOCK", fx + fw - 230, fy - 26, 12, kFaint, true);
    textRight("THIS CAR", fx + fw - 110, fy - 26, 12, kFaint, true);
    textRight("CHANGE", fx + fw - 6, fy - 26, 12, kFaint, true);
    const unsigned selMask = 1u << std::max(0, m.testStatRow);
    for (const FigureLine& f : m.figures) {
        const bool hl = (f.statMask & selMask) != 0;
        if (hl) DrawRectangleRounded({fx - 8, fy - 4, fw + 12, 26}, 0.3f, 6, Fade(kAccent, 0.14f));
        std::snprintf(buf, sizeof buf, "%s%s", f.name.c_str(), f.approx ? "  (est.)" : "");
        text(buf, fx, fy, 15, hl ? kText : kDim, hl);
        char fmt[16];
        std::snprintf(fmt, sizeof fmt, "%%.%df %%s", f.decimals);
        std::snprintf(buf, sizeof buf, fmt, f.stock, f.unit.c_str());
        textRight(buf, fx + fw - 230, fy, 15, kDim, false, true);
        std::snprintf(buf, sizeof buf, fmt, f.now, f.unit.c_str());
        textRight(buf, fx + fw - 110, fy, 15, kText, true, true);
        const float d = f.now - f.stock;
        const float rel = std::fabs(f.stock) > 1e-6f ? d / f.stock : 0;
        if (std::fabs(rel) > 5e-4f) {
            std::snprintf(buf, sizeof buf, "%+.1f%%", rel * 100);
            const bool better = (d > 0) == f.higherBetter;
            textRight(buf, fx + fw - 6, fy, 15, better ? kGood : kBad, true, true);
        } else {
            textRight("-", fx + fw - 6, fy, 15, kFaint, false, true);
        }
        fy += 30;
    }
    // buttons
    auto button = [&](const char* label, float bx, int code, bool primary) {
        const float bw = width(label, 18, true) + 40;
        const Rectangle b = {bx - bw, y + h - 58, bw, 40};
        DrawRectangleRounded(b, 0.25f, 8, primary ? kAccent : Fade(WHITE, 0.14f));
        text(label, b.x + 20, b.y + 10, 18, primary ? Color{20, 20, 24, 255} : kText, true);
        hits.push_back({b.x, b.y, b.width, b.height, code, 0});
        return b.x - 12;
    };
    float bx = x + w - 28;
    bx = button("DONE", bx, 99, true);
    bx = button("Style defaults", bx, 98, false);
    button("All stock", bx, 97, false);
    text("Up/Down stat    Left/Right change    D style defaults    0 all stock    Enter done", x + 28, y + h - 46, 15, kDim);
}

void Hud::drawRunsPage(const MenuState& m, std::vector<MenuHit>& hits) {
    const float sw = (float)GetScreenWidth(), sh = (float)GetScreenHeight();
    DrawRectangle(0, 0, (int)sw, (int)sh, Fade(Color{8, 10, 16, 255}, 0.5f));
    const float w = std::min(sw - 20, 1500.0f), h = sh - 20;
    const float x = (sw - w) / 2, y = 10;
    panel({x, y, w, h}, 0.93f);
    text("TESTING", x + 28, y + 20, 18, kAccent, true);
    text("Saved runs", x + 28, y + 42, 32, kText, true);
    char buf[200];
    auto button = [&](const char* label, float bx, float by, int code, bool primary, bool enabled = true) {
        const float bw = width(label, 16, true) + 32;
        const Rectangle b = {bx, by, bw, 34};
        DrawRectangleRounded(b, 0.25f, 8, primary ? (enabled ? kAccent : Fade(kAccent, 0.3f)) : Fade(WHITE, enabled ? 0.14f : 0.05f));
        text(label, b.x + 16, b.y + 8, 16, primary ? Color{20, 20, 24, 255} : enabled ? kText : kFaint, true);
        if (enabled) hits.push_back({b.x, b.y, b.width, b.height, code, 0});
        return bx + bw + 10;
    };
    float bx = x + 300;
    std::snprintf(buf, sizeof buf, "Sort: %s", m.runsSort ? "best lap" : "newest");
    bx = button(buf, bx, y + 46, 96, false);
    std::snprintf(buf, sizeof buf, "Showing: %s", m.runsAllTracks ? "all tracks" : "this track");
    button(buf, bx, y + 46, 95, false);
    std::snprintf(buf, sizeof buf, "%d of %d runs", (int)m.runLines.size(), m.runsTotal);
    textRight(buf, x + w - 28, y + 54, 15, kDim);
    // table
    const char* cols[] = {"RUN", "DATE", "TRACK", "ALGORITHM", "STATS", "TYRES", "FUEL", "LAPS", "BEST", "AVERAGE", "L/LAP", "WEAR/LAP", "DATA"};
    const float cx[] = {0, 52, 210, 330, 520, 800, 870, 930, 1000, 1090, 1180, 1240, 1320};
    const float scale = std::min(1.0f, (w - 56) / 1380.0f);
    const float tx = x + 28;
    float ty = y + 100;
    for (int k = 0; k < 13; ++k) text(cols[k], tx + cx[k] * scale, ty, 12, kFaint, true);
    ty += 20;
    const float rowH = 26;
    const int vis = std::max(1, (int)((y + h - 120 - ty) / rowH));
    const int n = (int)m.runLines.size();
    int top = std::clamp(m.runsTop, 0, std::max(0, n - vis));
    if (m.runsRow < top) top = m.runsRow;
    if (m.runsRow >= top + vis) top = m.runsRow - vis + 1;
    float bestTime = 0;
    for (const auto& l : m.runLines)
        if (l.best > 0 && (bestTime == 0 || l.best < bestTime)) bestTime = l.best;
    const float fs = 14 * std::max(0.85f, scale);
    for (int i = top; i < n && i < top + vis; ++i, ty += rowH) {
        const MenuState::RunLine& l = m.runLines[i];
        const Rectangle r = {tx - 8, ty - 4, w - 40, rowH - 2};
        if (i == m.runsRow) DrawRectangleRounded(r, 0.3f, 6, Fade(kAccent, 0.16f));
        hits.push_back({r.x, r.y, r.width, r.height, 3000 + i, 0});
        auto col = [&](int k, const std::string& s, Color c, bool mono = false) {
            float size = fs;
            const float room = (k + 1 < 13 ? (cx[k + 1] - cx[k]) * scale : 80) - 8;
            while (size > 11 && width(s.c_str(), size, false, mono) > room) size -= 0.5f;
            std::string t = s;  // still too long: cut it short
            while (t.size() > 3 && width((t + "...").c_str(), size, false, mono) > room) t.pop_back();
            if (t.size() < s.size()) t += "...";
            text(t.c_str(), tx + cx[k] * scale, ty + (fs - size) / 2, size, c, false, mono);
        };
        std::snprintf(buf, sizeof buf, "%d", l.id);
        col(0, buf, kDim, true);
        col(1, l.date, kDim);
        col(2, l.track, kText);
        col(3, l.algo, kText);
        col(4, l.stats.empty() ? "stock" : l.stats, kDim);
        const float cyc = ty + fs / 2 + 1;
        DrawCircleV({tx + cx[5] * scale + 7, cyc}, 6.5f, compoundColor(l.compound));
        DrawCircleV({tx + cx[5] * scale + 7, cyc}, 4.0f, Color{25, 25, 30, 255});
        std::snprintf(buf, sizeof buf, "%.0f L", l.fuel);
        col(6, buf, kDim, true);
        std::snprintf(buf, sizeof buf, "%d/%d", l.lapsDone, l.laps);
        col(7, buf, l.completed ? kDim : kBad, true);
        col(8, l.best > 0 ? lapTime(l.best) : "-", l.best > 0 && l.best == bestTime ? kBest : kText, true);
        col(9, l.average > 0 ? lapTime(l.average) : "-", kDim, true);
        std::snprintf(buf, sizeof buf, "%.2f", l.fuelPerLap);
        col(10, buf, kDim, true);
        std::snprintf(buf, sizeof buf, "%.3f", l.wearPerLap);
        col(11, buf, kDim, true);
        col(12, l.telemetry ? "telemetry" : "summary", l.telemetry ? kGood : kFaint);
    }
    if (n == 0) text(m.runsTotal ? "No runs on this track yet (T shows all tracks)." : "No runs saved yet: start a test.", tx, ty + 10, 16, kDim);
    // actions
    const bool can = m.runsRow < n;
    const bool tele = can && m.runLines[m.runsRow].telemetry;
    float ax = x + 28;
    const float ay = y + h - 60;
    ax = button("Load this setup", ax, ay, 98, true, can);
    ax = button("Replay telemetry", ax, ay, 97, false, tele);
    button("Back", ax, ay, 99, false);
    textRight("Up/Down select   Enter load setup   V replay   S sort   T this track / all   Esc back", x + w - 28, ay + 9, 14, kDim);
}
