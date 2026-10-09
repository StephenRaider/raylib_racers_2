#include "hud_icons.hpp"

#include <algorithm>
#include <cmath>

namespace icon {

namespace {

// raylib culls clockwise triangles: draw in whichever order shows.
void tri(Vector2 a, Vector2 b, Vector2 c, Color col) {
    const float cross = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
    if (cross < 0) DrawTriangle(a, b, c, col);
    else DrawTriangle(a, c, b, col);
}

void quad(Vector2 a, Vector2 b, Vector2 c, Vector2 d, Color col) {
    tri(a, b, c, col);
    tri(a, c, d, col);
}

Vector2 rot(Vector2 v, float deg) {
    const float r = deg * DEG2RAD, cs = std::cos(r), sn = std::sin(r);
    return {v.x * cs - v.y * sn, v.x * sn + v.y * cs};
}

}  // namespace

void fuelPump(Vector2 c, float s, Color col) {
    // body, window, base, hose
    const float w = s * 0.55f, h = s * 0.8f, x = c.x - s * 0.42f, y = c.y - h / 2;
    DrawRectangleRounded({x, y, w, h}, 0.25f, 4, col);
    DrawRectangleRec({x + w * 0.2f, y + h * 0.14f, w * 0.6f, h * 0.26f}, Fade(BLACK, 0.55f));
    DrawRectangleRec({x - s * 0.06f, y + h - s * 0.08f, w + s * 0.12f, s * 0.1f}, col);
    const float hx = x + w + s * 0.06f;
    DrawLineEx({x + w, y + h * 0.2f}, {hx + s * 0.12f, y + h * 0.32f}, std::max(1.5f, s * 0.08f), col);
    DrawLineEx({hx + s * 0.12f, y + h * 0.32f}, {hx + s * 0.12f, y + h * 0.78f}, std::max(1.5f, s * 0.08f), col);
    DrawLineEx({hx + s * 0.12f, y + h * 0.78f}, {hx, y + h * 0.78f}, std::max(1.5f, s * 0.08f), col);
}

void battery(Vector2 p, float w, float h, float frac, Color fill, Color outline) {
    const float nub = std::max(2.0f, w * 0.08f);
    const Rectangle body = {p.x, p.y, w - nub, h};
    DrawRectangleRoundedLinesEx(body, 0.3f, 4, 1.5f, outline);
    DrawRectangleRec({p.x + w - nub, p.y + h * 0.3f, nub, h * 0.4f}, outline);
    const float in = 2.5f;
    const float fw = (body.width - 2 * in) * std::clamp(frac, 0.0f, 1.0f);
    if (fw > 0.5f) DrawRectangleRec({p.x + in, p.y + in, fw, h - 2 * in}, fill);
}

void bolt(Vector2 c, float s, Color col) {
    const float u = s / 2;
    tri({c.x + u * 0.25f, c.y - u}, {c.x - u * 0.55f, c.y + u * 0.15f}, {c.x + u * 0.05f, c.y + u * 0.15f}, col);
    tri({c.x - u * 0.05f, c.y - u * 0.15f}, {c.x + u * 0.55f, c.y - u * 0.15f}, {c.x - u * 0.25f, c.y + u}, col);
}

void stopwatch(Vector2 c, float s, Color col) {
    const float r = s * 0.4f;
    const Vector2 o = {c.x, c.y + s * 0.06f};
    DrawRing(o, r - std::max(1.5f, s * 0.1f), r, 0, 360, 24, col);
    DrawRectangleRec({c.x - s * 0.1f, o.y - r - s * 0.16f, s * 0.2f, s * 0.1f}, col);
    DrawLineEx(o, {o.x, o.y - r * 0.65f}, std::max(1.5f, s * 0.09f), col);
    DrawLineEx(o, {o.x + r * 0.45f, o.y + r * 0.1f}, std::max(1.5f, s * 0.09f), col);
}

void steeringWheel(Vector2 c, float r, float angle, Color col, Color mark) {
    const float t = std::max(2.0f, r * 0.2f);
    DrawRing(c, r - t, r, 0, 360, 32, col);
    // the top-centre stripe, the way a real wheel shows lock
    DrawRing(c, r - t, r, -90 + angle - 9, -90 + angle + 9, 6, mark);
    // spokes: left, right and down, and the hub
    for (float a : {180.0f, 0.0f, 90.0f}) {
        const Vector2 d = rot({std::cos(a * DEG2RAD), std::sin(a * DEG2RAD)}, angle);
        DrawLineEx({c.x + d.x * r * 0.3f, c.y + d.y * r * 0.3f}, {c.x + d.x * (r - t * 0.5f), c.y + d.y * (r - t * 0.5f)},
                   t * 0.8f, col);
    }
    DrawCircleV(c, r * 0.32f, col);
}

void wrench(Vector2 c, float s, Color col) {
    const Vector2 d = rot({0, -1}, 45), n = {-d.y, d.x};
    const float L = s * 0.42f, w = std::max(1.5f, s * 0.11f);
    const Vector2 a = {c.x - d.x * L, c.y - d.y * L}, b = {c.x + d.x * L * 0.6f, c.y + d.y * L * 0.6f};
    quad({a.x + n.x * w, a.y + n.y * w}, {b.x + n.x * w, b.y + n.y * w}, {b.x - n.x * w, b.y - n.y * w},
         {a.x - n.x * w, a.y - n.y * w}, col);
    const Vector2 head = {c.x + d.x * L * 0.7f, c.y + d.y * L * 0.7f};
    // a ring spanner head, open at the far end
    const float ang = std::atan2(d.y, d.x) * RAD2DEG;
    DrawRing(head, s * 0.1f, s * 0.24f, ang + 40, ang + 320, 16, col);
}

void chequered(Vector2 p, float w, float h) {
    const int nx = 4, ny = 3;
    const float cw = w / nx, ch = h / ny;
    for (int i = 0; i < nx; ++i)
        for (int j = 0; j < ny; ++j)
            DrawRectangleRec({p.x + i * cw, p.y + j * ch, cw + 0.5f, ch + 0.5f}, (i + j) % 2 ? Color{20, 20, 24, 255} : WHITE);
}

void triangle(Vector2 c, float s, bool up, Color col) {
    const float h = s * 0.5f, k = up ? -1.0f : 1.0f;
    tri({c.x, c.y + k * h}, {c.x - s * 0.55f, c.y - k * h * 0.7f}, {c.x + s * 0.55f, c.y - k * h * 0.7f}, col);
}

void warning(Vector2 c, float s, Color col) {
    const float h = s * 0.9f;
    tri({c.x, c.y - h / 2}, {c.x - s / 2, c.y + h / 2}, {c.x + s / 2, c.y + h / 2}, col);
    const Color ink = {16, 18, 24, 255};
    DrawLineEx({c.x, c.y - h * 0.12f}, {c.x, c.y + h * 0.2f}, std::max(1.5f, s * 0.12f), ink);
    DrawCircleV({c.x, c.y + h * 0.34f}, std::max(1.0f, s * 0.07f), ink);
}

void slantBar(Vector2 p, float w, float h, float lean, float frac, Color fill, Color back) {
    auto par = [&](float y0, float y1, Color col) {  // the slice between heights y0 and y1 (from the top)
        const float l0 = lean * (1 - y0 / h), l1 = lean * (1 - y1 / h);
        quad({p.x + l0, p.y + y0}, {p.x + l0 + w, p.y + y0}, {p.x + l1 + w, p.y + y1}, {p.x + l1, p.y + y1}, col);
    };
    par(0, h, back);
    frac = std::clamp(frac, 0.0f, 1.0f);
    if (frac > 0.005f) par(h * (1 - frac), h, fill);
}

void arc(Vector2 c, float r0, float r1, float a0, float a1, Color col) {
    if (a1 <= a0) return;
    DrawRing(c, r0, r1, a0, a1, std::max(4, (int)((a1 - a0) / 4)), col);
}

}  // namespace icon
