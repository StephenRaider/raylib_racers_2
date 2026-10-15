// What a track file's "feature" lines build beside the track: tyre walls, catch fences, marshal
// posts, grandstands with crowds, buildings, footbridges, hoardings and distant flat skylines (the
// houses of a village, a row of towers) that fake a town beyond the hills. All of it is generated here
// from plain boxes and a few small textures drawn in code, so there are no model files to credit.
#include "track_scene.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "raymath.h"
#include "rlgl.h"

using gfx::Vertex;
using rr::Vec2;

namespace {

uint32_t hash3(int x, int y, int seed) {
    uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u + (uint32_t)seed * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}
float rnd(int x, int y, int seed) { return (hash3(x, y, seed) & 0xffffff) / 16777215.0f; }

Vector3 W(Vec2 p, float h) { return {p.x, h, -p.y}; }

// Same shape as the road builder's: height of the ground at a lateral offset from the centreline.
float edgeProfile(float z, float bank, float hw, float lat) {
    float l = std::clamp(lat, -hw, hw);
    float h = z + l * std::tan(bank);
    if (std::fabs(lat) > hw) h -= (std::fabs(lat) - hw) * 0.025f;
    return h;
}

Vertex vert(Vector3 p, Vector3 n, Vector2 uv) {
    Vertex v{};
    v.p = p;
    v.n = n;
    v.uv = uv;
    v.c[0] = v.c[1] = v.c[2] = v.c[3] = 255;
    return v;
}

void quad(gfx::MeshBuilder& mb, Vector3 a, Vector3 b, Vector3 c, Vector3 d, Vector2 ta, Vector2 tb, Vector2 tc, Vector2 td) {
    mb.reserve(4);
    Vector3 n = Vector3Normalize(Vector3CrossProduct(Vector3Subtract(b, a), Vector3Subtract(c, a)));
    int ia = mb.add(vert(a, n, ta)), ib = mb.add(vert(b, n, tb)), ic = mb.add(vert(c, n, tc)), id = mb.add(vert(d, n, td));
    mb.tri(ia, ib, ic);
    mb.tri(ia, ic, id);
}

// A quad turned so that its normal points upwards (roofs).
void quadUp(gfx::MeshBuilder& mb, Vector3 a, Vector3 b, Vector3 c, Vector3 d, float u, float v) {
    Vector3 n = Vector3CrossProduct(Vector3Subtract(b, a), Vector3Subtract(c, a));
    if (n.y < 0) {
        std::swap(b, d);
        quad(mb, a, b, c, d, {0, 0}, {0, v}, {u, v}, {u, 0});
    } else {
        quad(mb, a, b, c, d, {0, 0}, {u, 0}, {u, v}, {0, v});
    }
}

// A triangle whose normal faces `outward` (the end of a gable).
void tri(gfx::MeshBuilder& mb, Vector3 a, Vector3 b, Vector3 c, Vector3 outward) {
    Vector3 n = Vector3CrossProduct(Vector3Subtract(b, a), Vector3Subtract(c, a));
    if (Vector3DotProduct(n, outward) < 0) std::swap(b, c);
    n = Vector3Normalize(Vector3CrossProduct(Vector3Subtract(b, a), Vector3Subtract(c, a)));
    mb.reserve(3);
    int ia = mb.add(vert(a, n, {0, 0})), ib = mb.add(vert(b, n, {Vector3Distance(a, b), 0})),
        ic = mb.add(vert(c, n, {0, Vector3Distance(a, c)}));
    mb.tri(ia, ib, ic);
}

// A six-sided block from its eight corners (bottom four, then the top four), faces outwards. `rgb` is the
// vertex colour (for the vertex-tinted crowd); ao darkens the lower edge.
void block(gfx::MeshBuilder& mb, const Vector3 c[8], bool bottom = false, float aoLow = 1.0f, unsigned char rgb = 255,
           unsigned char g = 255, unsigned char b = 255) {
    Vector3 mid = {0, 0, 0};
    for (int k = 0; k < 8; ++k) mid = Vector3Add(mid, Vector3Scale(c[k], 0.125f));
    auto face = [&](Vector3 a, Vector3 bb, Vector3 cc, Vector3 d, float aoA, float aoB, float aoC, float aoD) {
        Vector3 n = Vector3CrossProduct(Vector3Subtract(bb, a), Vector3Subtract(cc, a));
        Vector3 centre = Vector3Scale(Vector3Add(Vector3Add(a, bb), Vector3Add(cc, d)), 0.25f);
        if (Vector3DotProduct(n, Vector3Subtract(centre, mid)) < 0) {
            std::swap(bb, d);
            std::swap(aoB, aoD);
        }
        float lu = Vector3Distance(a, bb), lv = Vector3Distance(a, d);
        mb.reserve(4);
        Vector3 nn = Vector3Normalize(Vector3CrossProduct(Vector3Subtract(bb, a), Vector3Subtract(cc, a)));
        Vertex va = vert(a, nn, {0, 0}), vb = vert(bb, nn, {lu, 0}), vc = vert(cc, nn, {lu, lv}), vd = vert(d, nn, {0, lv});
        va.c[3] = (unsigned char)(aoA * 255); vb.c[3] = (unsigned char)(aoB * 255);
        vc.c[3] = (unsigned char)(aoC * 255); vd.c[3] = (unsigned char)(aoD * 255);
        for (Vertex* v : {&va, &vb, &vc, &vd}) { v->c[0] = rgb; v->c[1] = g; v->c[2] = b; }
        int ia = mb.add(va), ib = mb.add(vb), ic = mb.add(vc), id = mb.add(vd);
        mb.tri(ia, ib, ic);
        mb.tri(ia, ic, id);
    };
    for (int k = 0; k < 4; ++k) {
        int j = (k + 1) % 4;
        face(c[k], c[j], c[j + 4], c[k + 4], aoLow, aoLow, 1, 1);
    }
    face(c[4], c[5], c[6], c[7], 1, 1, 1, 1);
    if (bottom) face(c[0], c[1], c[2], c[3], aoLow, aoLow, aoLow, aoLow);
}

// A vertical cylinder (a stack of tyres): n sides and a top.
void cylinder(gfx::MeshBuilder& mb, Vector3 base, float r, float h, int n, unsigned char tint) {
    mb.reserve(n * 4 + n + 2);
    for (int k = 0; k < n; ++k) {
        float a0 = 2 * PI * k / n, a1 = 2 * PI * (k + 1) / n;
        Vector3 d0 = {std::cos(a0), 0, std::sin(a0)}, d1 = {std::cos(a1), 0, std::sin(a1)};
        Vector3 p0 = Vector3Add(base, Vector3Scale(d0, r)), p1 = Vector3Add(base, Vector3Scale(d1, r));
        Vertex v[4] = {vert(p0, d0, {r * a0, 0}), vert(p1, d1, {r * a1, 0}), vert(Vector3Add(p1, {0, h, 0}), d1, {r * a1, h}),
                       vert(Vector3Add(p0, {0, h, 0}), d0, {r * a0, h})};
        int id[4];
        for (int q = 0; q < 4; ++q) {
            v[q].c[0] = v[q].c[1] = v[q].c[2] = tint;
            v[q].c[3] = q < 2 ? 150 : 255;
            id[q] = mb.add(v[q]);
        }
        mb.tri(id[0], id[2], id[1]);
        mb.tri(id[0], id[3], id[2]);
    }
    Vertex c = vert(Vector3Add(base, {0, h, 0}), {0, 1, 0}, {0, 0});
    c.c[0] = c.c[1] = c.c[2] = tint;
    int ic = mb.add(c);
    int first = -1;
    for (int k = 0; k <= n; ++k) {
        float a = 2 * PI * k / n;
        Vertex v = vert(Vector3Add(base, {std::cos(a) * r, h, std::sin(a) * r}), {0, 1, 0}, {std::cos(a) * r, std::sin(a) * r});
        v.c[0] = v.c[1] = v.c[2] = tint;
        int id = mb.add(v);
        if (first >= 0) mb.tri(ic, id, id - 1);
        first = id;
    }
}

// ---- the skyline texture: rows of flat house or tower silhouettes, alpha outside them

void rect(Image* im, int x, int y, int w, int h, Color c) { ImageDrawRectangle(im, x, y, w, h, c); }

// A gable roof as a stack of horizontal runs.
void gable(Image* im, int x, int baseY, int w, int rise, Color c) {
    for (int r = 0; r < rise; ++r) {
        int inset = (int)(w * 0.5f * (float)r / rise);
        rect(im, x + inset, baseY - r - 1, w - 2 * inset, 1, c);
    }
}

Color shade(Color c, float k) {
    return {(unsigned char)std::clamp(c.r * k, 0.0f, 255.0f), (unsigned char)std::clamp(c.g * k, 0.0f, 255.0f),
            (unsigned char)std::clamp(c.b * k, 0.0f, 255.0f), c.a};
}

Image makeSkyline(int style, int seed) {
    const int W = 2048, H = 256;
    Image im = GenImageColor(W, H, BLANK);
    if (style == 0) {
        // A village: hedges and trees along the bottom, pitched-roof houses in rows, a church with a spire.
        const Color walls[] = {{226, 214, 190, 255}, {212, 196, 168, 255}, {236, 228, 214, 255}, {190, 150, 120, 255},
                               {205, 190, 175, 255}};
        const Color roofs[] = {{150, 70, 48, 255}, {120, 60, 44, 255}, {88, 92, 100, 255}, {138, 80, 56, 255}};
        // a back row of houses and trees, hazier
        for (int pass = 0; pass < 2; ++pass) {
            const float k = pass == 0 ? 0.78f : 1.0f;
            int x = (int)(rnd(pass, 1, seed) * 30);
            const int ground = H - 1 - (pass == 0 ? 22 : 0);
            while (x < W - 30) {
                int w = 34 + (int)(rnd(x, pass, seed) * 56), h = 28 + (int)(rnd(x, pass + 3, seed) * 38);
                if (pass == 0) h = (int)(h * 0.8f);
                if (rnd(x, pass + 7, seed) < 0.22f) {  // a tree or a clump of them
                    int tw = 26 + (int)(rnd(x, 9, seed) * 30), th = 46 + (int)(rnd(x, 11, seed) * 40);
                    Color g = shade({52, 78, 44, 255}, k * (0.8f + 0.4f * rnd(x, 13, seed)));
                    for (int r = 0; r < th; ++r) {  // a conifer
                        int rw = (int)(tw * 0.5f * (float)(r + 6) / (th + 6));
                        rect(&im, x + tw / 2 - rw / 2, ground - th + r, std::max(2, rw), 1, g);
                    }
                    x += tw + 3;
                    continue;
                }
                const Color wall = shade(walls[(int)(rnd(x, 15, seed) * 5) % 5], k);
                const Color roof = shade(roofs[(int)(rnd(x, 17, seed) * 4) % 4], k);
                rect(&im, x, ground - h, w, h, wall);
                gable(&im, x - 3, ground - h, w + 6, 14 + (int)(rnd(x, 19, seed) * 10), roof);
                // windows
                for (int wy = ground - h + 8; wy < ground - 8; wy += 14)
                    for (int wx = x + 7; wx < x + w - 9; wx += 16) rect(&im, wx, wy, 6, 8, shade({70, 80, 92, 255}, k));
                x += w + 2 + (int)(rnd(x, 21, seed) * 10);
            }
        }
        // the church, a little right of the middle
        const int cx = 1220, ground = H - 1 - 10;
        rect(&im, cx, ground - 70, 46, 70, {214, 204, 186, 255});
        gable(&im, cx - 3, ground - 70, 52, 20, {96, 62, 50, 255});
        rect(&im, cx + 12, ground - 120, 22, 52, {208, 198, 180, 255});
        for (int r = 0; r < 52; ++r) {  // the spire widens down to the tower
            const int sw = 2 + r * 22 / 52;
            rect(&im, cx + 23 - sw / 2, ground - 172 + r, sw, 1, {70, 76, 84, 255});
        }
        // a hedge of trees across the whole foot of it
        for (int x = 0; x < W; x += 9) {
            int th = 14 + (int)(rnd(x, 23, seed) * 14);
            rect(&im, x, H - th, 12, th, shade({46, 70, 40, 255}, 0.8f + 0.4f * rnd(x, 25, seed)));
        }
    } else {
        // A row of towers: flat boxes with lit window grids, a few with a crown or an antenna.
        const Color bodies[] = {{110, 128, 150, 255}, {150, 160, 172, 255}, {92, 104, 122, 255}, {180, 186, 192, 255}, {120, 120, 132, 255}};
        int x = 0;
        while (x < W - 40) {
            int w = 54 + (int)(rnd(x, 31, seed) * 90), h = 70 + (int)(rnd(x, 33, seed) * 170);
            if (rnd(x, 35, seed) < 0.3f) h = (int)(h * 0.55f);
            Color body = bodies[(int)(rnd(x, 37, seed) * 5) % 5];
            rect(&im, x, H - h, w, h, body);
            rect(&im, x, H - h, 3, h, shade(body, 1.2f));
            for (int wy = H - h + 8; wy < H - 6; wy += 9)
                for (int wx = x + 7; wx < x + w - 6; wx += 8)
                    if (rnd(wx, wy, seed) < 0.8f) rect(&im, wx, wy, 4, 5, rnd(wx + 1, wy, seed) < 0.1f ? Color{236, 220, 150, 255} : shade(body, 0.62f));
            if (rnd(x, 39, seed) < 0.35f) rect(&im, x + w / 2 - 1, H - h - 28, 3, 28, shade(body, 0.8f));  // antenna
            x += w + 2 + (int)(rnd(x, 41, seed) * 14);
        }
    }
    return im;
}

}  // namespace

// ---------------------------------------------------------------- what the feature lines cover

const rr::Track::Feature* TrackScene::featureAt(const char* kind, float s, int side) const {
    for (const rr::Track::Feature& f : tr_->features())
        if (f.kind == kind && (f.side == 0 || f.side == side) && tr_->inSpan(s, f.s0, f.s1)) return &f;
    return nullptr;
}

float TrackScene::forestBias(float s, int side) const {
    float v = -1.0f;
    for (const rr::Track::Feature& f : tr_->features()) {
        if (f.kind != "forest" && f.kind != "clearing") continue;
        if ((f.side != 0 && f.side != side) || !tr_->inSpan(s, f.s0, f.s1)) continue;
        v = f.kind == "clearing" ? 0.0f : (f.args.empty() ? 1.0f : f.args[0]);
    }
    return v;
}

// ---------------------------------------------------------------- build

void TrackScene::buildFeatures(const std::string& assetsDir) {
    const rr::Track& tr = *tr_;
    if (tr.features().empty()) return;
    const float L = tr.length();
    auto TP = [&](float s, float lat, float h) { return W(tr.pointAt(s, lat), h); };
    auto ground = [&](float s, float lat) {
        const auto& a = tr.at(tr.indexAt(s));
        return edgeProfile(a.z, a.bank, a.halfWidth, lat);
    };
    // a block between two track distances and two lateral offsets (any order), heights h0..h1
    auto blk = [&](gfx::MeshBuilder& mb, float s0, float s1, float l0, float l1, float h0, float h1, bool bottom = false,
                   float ao = 1.0f) {
        Vector3 c[8] = {TP(s0, l0, h0), TP(s1, l0, h0), TP(s1, l1, h0), TP(s0, l1, h0),
                        TP(s0, l0, h1), TP(s1, l0, h1), TP(s1, l1, h1), TP(s0, l1, h1)};
        block(mb, c, bottom, ao);
    };
    auto person = [&](gfx::MeshBuilder& mb, float s0, float s1, float l0, float l1, float h0, float h1, Color col) {
        Vector3 c[8] = {TP(s0, l0, h0), TP(s1, l0, h0), TP(s1, l1, h0), TP(s0, l1, h0),
                        TP(s0, l0, h1), TP(s1, l0, h1), TP(s1, l1, h1), TP(s0, l1, h1)};
        block(mb, c, false, 0.8f, col.r, col.g, col.b);
    };
    gfx::MeshBuilder wallA, wallB, roofA, roofB, roofW, glassF, steelF, whiteF, tyresF, fenceF, orangeF, seatsA, seatsB, seatsC, crowd,
        ad[4], skylineMb;
    auto spanOf = [&](const rr::Track::Feature& f) {
        float d = std::fmod(f.s1 - f.s0, L);
        return d < 0 ? d + L : d;
    };
    auto arg = [](const rr::Track::Feature& f, size_t i, float dflt) { return i < f.args.size() ? f.args[i] : dflt; };
    int stand = 0, bld = 0, nSky = 0;
    // muted, so a crowd reads as people and not as confetti
    const Color shirts[] = {{150, 50, 48, 255}, {58, 76, 128, 255}, {170, 170, 166, 255}, {176, 144, 62, 255}, {58, 96, 70, 255},
                            {40, 40, 46, 255}, {160, 96, 52, 255}, {110, 70, 112, 255}, {120, 118, 112, 255}, {200, 196, 186, 255}};
    const Color skins[] = {{196, 150, 120, 255}, {160, 112, 84, 255}, {220, 180, 150, 255}, {110, 76, 58, 255}};
    for (const rr::Track::Feature& f : tr.features()) {
        const float len = spanOf(f);
        for (int side : {1, -1}) {
            if (f.side != 0 && f.side != side) continue;
            // ---- tyre walls in front of the barrier: columns of stacked tyres, white-banded every fourth
            if (f.kind == "tyrewall") {
                const int rows = std::max(1, (int)arg(f, 0, 1));
                const float h = arg(f, 1, 0.95f);
                for (float d = 0; d <= len; d += 0.62f) {
                    const float st = f.s0 + d;
                    const auto& a = tr.at(tr.indexAt(st));
                    const float base = side * (tr.barrierOffset(st, side, a.halfWidth) - 0.4f);
                    const int stack = (int)std::floor(d / 0.62f);
                    for (int r = 0; r < rows; ++r) {
                        const float lo = base - side * r * 0.56f;
                        const float so = st + (r % 2) * 0.31f;
                        cylinder(tyresF, TP(so, lo, ground(so, lo) - 0.05f), 0.3f, h, 10, (stack / 4) % 2 ? 255 : 40);
                    }
                }
            }
            // ---- a catch fence behind the barrier
            else if (f.kind == "fence") {
                const float h = arg(f, 0, 4.0f);
                for (float d = 0; d < len; d += 4.0f) {
                    const float s = f.s0 + d, e = f.s0 + std::min(d + 4.0f, len);
                    const float off = side * (tr.barrierOffset(s, side, tr.at(tr.indexAt(s)).halfWidth) + 0.29f);
                    const float offE = side * (tr.barrierOffset(e, side, tr.at(tr.indexAt(e)).halfWidth) + 0.29f);
                    const float h0 = ground(s, off), h1 = ground(e, offE);
                    quad(fenceF, TP(s, off, h0), TP(e, offE, h1), TP(e, offE, h1 + h), TP(s, off, h0 + h), {s, 0}, {e, 0}, {e, h}, {s, h});
                    blk(steelF, s, s + 0.08f, off - 0.04f, off + 0.04f, h0 - 0.3f, h0 + h + 0.1f);
                }
            }
            // ---- marshal posts: a white hut with an orange roof, a window to the track and a flag pole
            else if (f.kind == "marshal") {
                const float spacing = arg(f, 0, 250.0f);
                for (float d = (side > 0 ? 0.3f : 0.8f) * spacing; d < len; d += spacing) {
                    const float st = f.s0 + d;
                    const auto& sm = tr.at(tr.indexAt(st));
                    const float lat = side * (tr.barrierOffset(st, side, sm.halfWidth) + 3.4f);
                    const float g = ground(st, lat) + 0.02f;
                    blk(whiteF, st - 1.1f, st + 1.1f, lat - 1.1f, lat + 1.1f, g - 0.5f, g + 2.4f, false, 0.6f);
                    blk(orangeF, st - 1.4f, st + 1.4f, lat - 1.4f, lat + 1.4f, g + 2.4f, g + 2.65f, true);
                    blk(glassF, st - 0.7f, st + 0.7f, lat - side * 1.1f, lat - side * 1.16f, g + 1.1f, g + 1.95f);
                    blk(steelF, st + 1.6f, st + 1.7f, lat - 0.05f, lat + 0.05f, g - 0.2f, g + 5.0f);
                    blk(orangeF, st + 1.7f, st + 2.9f, lat - 0.015f, lat + 0.015f, g + 4.1f, g + 5.0f);
                }
            }
            // ---- a grandstand: stepped concrete, rows of coloured seats and a crowd, a cantilever roof when tall
            else if (f.kind == "grandstand") {
                const int rows = std::clamp((int)arg(f, 0, 8), 3, 24);
                const float setback = arg(f, 1, 6.0f);
                const float depth = 0.9f, rise = 0.46f, base = 0.9f;
                gfx::MeshBuilder& seats = (stand % 3 == 0) ? seatsA : (stand % 3 == 1) ? seatsB : seatsC;
                const int sid = stand++;
                float lastG = 0, firstG = 0;
                for (float a = f.s0; a < f.s0 + len - 0.1f; a += 10.0f) {
                    const float e = std::min(a + 10.0f, f.s0 + len);
                    const float L0 = tr.barrierOffset(a, side, tr.at(tr.indexAt(a)).halfWidth) + setback;
                    auto lat = [&](float r) { return side * (L0 + r * depth); };
                    const float g0 = std::min({ground(a, lat(0)), ground(e, lat(0)), ground(a, lat(rows)), ground(e, lat(rows))}) + 0.02f;
                    if (a == f.s0) firstG = g0;
                    lastG = g0;
                    for (int r = 0; r < rows; ++r) {
                        const float top = g0 + base + r * rise;
                        blk(whiteF, a, e, lat((float)r), lat(r + 1.0f), g0 - 3.0f, top, false, 0.7f);
                        blk(seats, a + 0.2f, e - 0.2f, lat(r + 0.28f), lat(r + 0.78f), top, top + 0.42f);
                        // the crowd, in rows of shirts
                        for (float x = a + 0.3f; x < e - 0.45f; x += 0.5f) {
                            const int ix = (int)std::floor(x * 2.0f);
                            if (rnd(ix, r, 90 + sid) > 0.5f) continue;
                            const float sh = 0.7f + 0.3f * rnd(ix, r, 290 + sid);  // some taller than others
                            person(crowd, x, x + 0.34f, lat(r + 0.4f), lat(r + 0.64f), top + 0.42f, top + 0.42f + 0.52f * sh,
                                   shirts[(int)(rnd(ix, r, 190 + sid) * 10) % 10]);
                            person(crowd, x + 0.07f, x + 0.27f, lat(r + 0.44f), lat(r + 0.6f), top + 0.42f + 0.52f * sh,
                                   top + 0.42f + 0.52f * sh + 0.2f, skins[(int)(rnd(ix, r, 390 + sid) * 4) % 4]);
                        }
                    }
                    const float back = lat((float)rows);
                    const float topRow = g0 + base + rows * rise;
                    blk(whiteF, a, e, back, back + side * 0.4f, g0 - 3.0f, topRow + (rows >= 6 ? 3.2f : 1.2f), false, 0.7f);
                    if (rows >= 6) {
                        blk(whiteF, a, a + 0.4f, back - side * 0.2f, back + side * 0.2f, topRow, topRow + 3.6f);
                        Vector3 c[8] = {TP(a, back + side * 0.4f, topRow + 3.6f), TP(e, back + side * 0.4f, topRow + 3.6f),
                                        TP(e, lat(-1.5f / depth), topRow + 4.6f), TP(a, lat(-1.5f / depth), topRow + 4.6f),
                                        TP(a, back + side * 0.4f, topRow + 3.85f), TP(e, back + side * 0.4f, topRow + 3.85f),
                                        TP(e, lat(-1.5f / depth), topRow + 4.85f), TP(a, lat(-1.5f / depth), topRow + 4.85f)};
                        block(roofW, c, true, 0.7f);
                    }
                }
                // the closed ends
                for (int end = 0; end < 2; ++end) {
                    const float s = end ? f.s0 + len : f.s0 - 0.4f;
                    const float L0 = tr.barrierOffset(s, side, tr.at(tr.indexAt(s)).halfWidth) + setback;
                    const float g = (end ? lastG : firstG);
                    blk(whiteF, s, s + 0.4f, side * L0, side * (L0 + rows * depth + 0.4f), g - 3.0f, g + base + rows * rise + (rows >= 6 ? 3.2f : 1.2f));
                }
            }
            // ---- a building: walls with window panes by the floor, a pitched roof
            else if (f.kind == "building") {
                const float h = arg(f, 0, 12.0f), setback = arg(f, 1, 14.0f), dep = arg(f, 2, 16.0f);
                const int id = bld++;
                const float mid = f.s0 + len * 0.5f;
                const float L0 = tr.barrierOffset(mid, side, tr.at(tr.indexAt(mid)).halfWidth) + setback;
                const float l0 = side * L0, l1 = side * (L0 + dep);
                const float g = std::min({ground(f.s0, l0), ground(f.s0 + len, l0), ground(f.s0, l1), ground(f.s0 + len, l1)}) + 0.02f;
                gfx::MeshBuilder& wall = (id % 2) ? wallB : wallA;
                gfx::MeshBuilder& roof = ((id / 2) % 2) ? roofB : roofA;
                blk(wall, f.s0, f.s0 + len, l0, l1, g - 3.0f, g + h, false, 0.7f);
                // windows: panes in rows facing the track and out the back
                for (int face = 0; face < 2; ++face) {
                    const float lf = face ? l1 : l0;
                    const float out = face ? side : -side;
                    for (float fy = g + 1.3f; fy + 1.6f < g + h; fy += 3.3f)
                        for (float x = f.s0 + 1.6f; x + 1.4f < f.s0 + len - 0.8f; x += 3.0f)
                            blk(glassF, x, x + 1.4f, lf, lf + out * 0.07f, fy, fy + 1.6f);
                }
                // a gable roof along the track with a small overhang
                const float o = 0.7f, rise = std::max(2.5f, dep * 0.26f);
                const float lm = (l0 + l1) * 0.5f, lo0 = l0 - side * o, lo1 = l1 + side * o;
                const float s0o = f.s0 - o, s1o = f.s0 + len + o;
                quadUp(roof, TP(s0o, lo0, g + h), TP(s1o, lo0, g + h), TP(s1o, lm, g + h + rise), TP(s0o, lm, g + h + rise), len + 2 * o, dep * 0.6f);
                quadUp(roof, TP(s0o, lo1, g + h), TP(s1o, lo1, g + h), TP(s1o, lm, g + h + rise), TP(s0o, lm, g + h + rise), len + 2 * o, dep * 0.6f);
                tri(wall, TP(f.s0, l0, g + h), TP(f.s0, l1, g + h), TP(f.s0, lm, g + h + rise), TP(f.s0 - 1, lm, g + h));
                tri(wall, TP(f.s0 + len, l0, g + h), TP(f.s0 + len, l1, g + h), TP(f.s0 + len, lm, g + h + rise), TP(f.s0 + len + 1, lm, g + h));
                // a chimney
                blk(wallB, f.s0 + len * 0.7f, f.s0 + len * 0.7f + 0.8f, lm - 0.4f, lm + 0.4f, g + h, g + h + rise + 1.4f);
            }
            // ---- a footbridge over the track: a deck on pillars with railings and a banner
            else if (f.kind == "bridge") {
                if (side < 0) continue;  // one bridge spans both sides: built once
                const float s = f.s0;
                const auto& sm = tr.at(tr.indexAt(s));
                const float lw = tr.barrierOffset(s, 1, sm.halfWidth) + 2.6f, rw = tr.barrierOffset(s, -1, sm.halfWidth) + 2.6f;
                const float deckY = sm.z + 7.2f;
                const float hw = 2.6f;  // half the deck's length along the track
                // pillars (two a side), a little below the ground out there
                for (int sd : {1, -1}) {
                    const float lat = sd * (sd > 0 ? lw : rw);
                    for (float so : {-hw + 0.5f, hw - 0.5f}) {
                        const float gb = ground(s + so, lat) - 3.0f;
                        blk(whiteF, s + so - 0.5f, s + so + 0.5f, lat - 0.5f, lat + 0.5f, gb, deckY, false, 0.8f);
                    }
                }
                blk(whiteF, s - hw, s + hw, -rw - 0.8f, lw + 0.8f, deckY, deckY + 0.7f, true, 0.8f);
                // railings and a banner
                for (float so : {-hw, hw - 0.12f}) blk(steelF, s + so, s + so + 0.12f, -rw - 0.8f, lw + 0.8f, deckY + 0.7f, deckY + 1.5f);
                blk(orangeF, s - hw - 0.1f, s - hw + 0.02f, -rw - 0.8f, lw + 0.8f, deckY + 1.5f, deckY + 3.3f);
                blk(whiteF, s + hw - 0.02f, s + hw + 0.1f, -rw - 0.8f, lw + 0.8f, deckY + 1.5f, deckY + 3.3f);
            }
            // ---- hoardings: advertising boards along the fence, a few colours
            else if (f.kind == "billboard") {
                const float bl = arg(f, 0, 6.0f);
                int k = 0;
                for (float d = 0; d + bl <= len; d += bl + 0.5f, ++k) {
                    const float st = f.s0 + d;
                    const float lat = side * (tr.barrierOffset(st, side, tr.at(tr.indexAt(st)).halfWidth) + 0.9f);
                    const float g = ground(st, lat);
                    blk(ad[(k + (int)std::floor(f.s0 / 37.0f)) % 4], st, st + bl, lat - 0.05f, lat + 0.05f, g + 0.5f, g + 2.0f);
                    blk(whiteF, st + bl * 0.1f, st + bl * 0.9f, lat - side * 0.056f, lat - side * 0.06f, g + 0.95f, g + 1.1f);
                    blk(steelF, st + 0.3f, st + 0.4f, lat - 0.04f, lat + 0.04f, g - 0.2f, g + 0.5f);
                    blk(steelF, st + bl - 0.4f, st + bl - 0.3f, lat - 0.04f, lat + 0.04f, g - 0.2f, g + 0.5f);
                }
            }
            // ---- the paddock behind the garages: team tents and transporters in two rows
            else if (f.kind == "paddock") {
                int k = 0;
                for (float d = 6.0f; d + 14.0f < len; d += 16.0f, ++k) {
                    const float st = f.s0 + d;
                    const auto& sm = tr.at(tr.indexAt(st));
                    for (int row = 0; row < 2; ++row) {
                        const float r = rnd(k, row, 71);
                        if (r < 0.15f) continue;
                        const float l0 = side * (sm.halfWidth + rr::Track::kPitBarrier + 21.0f + row * 15.0f);
                        const float l1 = l0 + side * (r < 0.55f ? 6.0f : 2.6f);
                        const float g = ground(st, l0) + 0.02f;
                        if (r < 0.55f) {  // a tent: white walls, a low roof with an overhang, a team stripe
                            blk(whiteF, st, st + 9.0f, l0, l1, g, g + 2.7f, false, 0.7f);
                            blk(ad[k % 4], st - 0.5f, st + 9.5f, l0 - side * 0.5f, l1 + side * 0.5f, g + 2.7f, g + 3.05f, true);  // a team-coloured roof
                            blk(glassF, st + 1.0f, st + 8.0f, l0 - side * 0.02f, l0 + side * 0.04f, g + 0.2f, g + 2.2f);   // the open front
                        } else {  // a transporter: box trailer and a cab
                            blk(ad[(k + 1) % 4], st + 2.8f, st + 14.0f, l0, l1, g + 0.7f, g + 3.9f, true, 0.7f);
                            blk(whiteF, st + 2.8f, st + 14.0f, l0 - side * 0.01f, l1 + side * 0.01f, g + 2.3f, g + 3.0f);
                            for (float wx : {st + 4.0f, st + 11.8f}) blk(tyresF, wx, wx + 1.0f, l0 - side * 0.1f, l1 + side * 0.1f, g, g + 0.9f);
                            blk(steelF, st, st + 2.7f, l0, l1, g + 0.7f, g + 3.0f, true);
                            blk(glassF, st - 0.02f, st + 0.1f, l0 + side * 0.2f, l1 - side * 0.2f, g + 1.7f, g + 2.7f);
                            blk(steelF, st, st + 14.0f, l0 + side * 0.1f, l1 - side * 0.1f, g + 0.15f, g + 0.7f);  // the chassis
                        }
                    }
                }
            }
            // ---- brake markers before the slower corners: 150, 100 and 50 m boards with one to three bars
            else if (f.kind == "brakeboards") {
                for (const RRTurn& turn : tr.turns()) {
                    if (turn.min_radius > 110.0f || !tr.inSpan(turn.start_s, f.s0, f.s1)) continue;
                    for (int k = 0; k < 3; ++k) {
                        const float st = turn.start_s - 150.0f + 50.0f * k;
                        const float lat = side * (tr.at(tr.indexAt(st)).halfWidth + 3.0f);
                        const float g = ground(st, lat);
                        blk(whiteF, st - 0.05f, st + 0.05f, lat - 0.45f, lat + 0.45f, g + 0.9f, g + 1.9f);
                        for (int b = 0; b <= k; ++b) {  // 150: one bar, 100: two, 50: three
                            const float bs = st - 0.075f;
                            const float ll = lat - 0.3f + b * 0.25f;
                            blk(orangeF, bs, bs + 0.03f, ll, ll + 0.12f, g + 1.0f, g + 1.8f);
                            blk(orangeF, st + 0.045f, st + 0.075f, ll, ll + 0.12f, g + 1.0f, g + 1.8f);
                        }
                        blk(steelF, st - 0.03f, st + 0.03f, lat - 0.04f, lat + 0.04f, g - 0.2f, g + 0.9f);
                    }
                }
            }
            // ---- a flat skyline far off: houses or towers on a cut-out, to fake a town beyond the hills
            else if (f.kind == "skyline") {
                const float dist = arg(f, 0, 900.0f), height = arg(f, 1, 60.0f);
                const int style = (int)arg(f, 2, 0);
                if (!skylineTex_.id) {
                    Image im = makeSkyline(style, 5);
                    skylineTex_ = LoadTextureFromImage(im);
                    UnloadImage(im);
                    GenTextureMipmaps(&skylineTex_);
                    SetTextureFilter(skylineTex_, TEXTURE_FILTER_TRILINEAR);
                    SetTextureWrap(skylineTex_, TEXTURE_WRAP_CLAMP);
                    skyline_ = gfx::flatTextureSet(skylineTex_, 1.0f);
                }
                const float tile = height * 8.0f;  // the texture is 8:1
                const int n = std::max(1, (int)std::ceil(len / tile));
                for (int t = 0; t < n; ++t) {
                    const int sub = 6;
                    for (int q = 0; q < sub; ++q) {
                        const float sA = f.s0 + (t + (float)q / sub) * len / n, sB = f.s0 + (t + (float)(q + 1) / sub) * len / n;
                        const Vec2 pa = tr.pointAt(sA, side * dist), pb = tr.pointAt(sB, side * dist);
                        const float ya = groundHeight(pa.x, -pa.y) - 6.0f, yb = groundHeight(pb.x, -pb.y) - 6.0f;
                        const float u0 = (float)q / sub, u1 = (float)(q + 1) / sub;
                        // facing the track
                        Vector3 a0 = W(pa, ya), a1 = W(pb, yb), a2 = W(pb, yb + height), a3 = W(pa, ya + height);
                        quad(skylineMb, a0, a1, a2, a3, {u0, 1}, {u1, 1}, {u1, 0}, {u0, 0});
                    }
                }
                ++nSky;
            }
        }
    }

    auto part = [&](gfx::MeshBuilder& mb, gfx::Material mat, bool shadows, unsigned alphaTex = 0) {
        Part p;
        p.meshes = mb.build();
        p.mat = mat;
        p.shadows = shadows;
        p.alphaTex = alphaTex;
        if (!p.meshes.empty()) parts_.push_back(std::move(p));
    };
    auto concrete = [&](Vector3 tint, float rough = 0.85f, float scale = 3.0f) {
        gfx::Material m;
        m.layer[0] = &concrete_;
        m.scale[0] = scale;
        m.tint = tint;
        m.roughness = rough;
        m.normalStrength = 0.5f;
        return m;
    };
    part(whiteF, concrete({1.05f, 1.05f, 1.05f}), true);
    part(wallA, concrete({0.95f, 0.88f, 0.76f}), true);
    part(wallB, concrete({0.66f, 0.34f, 0.26f}), true);
    {
        gfx::Material m = concrete({0.62f, 0.27f, 0.16f}, 0.8f, 2.0f);
        m.doubleSided = true;
        part(roofA, m, true);
        m.tint = {0.2f, 0.22f, 0.25f};
        part(roofB, m, true);
    }
    part(orangeF, concrete({1.0f, 0.38f, 0.05f}, 0.6f), true);
    part(roofW, concrete({0.82f, 0.85f, 0.9f}, 0.55f, 4.0f), true);
    part(seatsA, concrete({0.08f, 0.22f, 0.6f}, 0.45f, 1.0f), true);
    part(seatsB, concrete({0.62f, 0.07f, 0.06f}, 0.45f, 1.0f), true);
    part(seatsC, concrete({0.06f, 0.38f, 0.16f}, 0.45f, 1.0f), true);
    {
        gfx::Material m = concrete({1.0f, 1.0f, 1.0f}, 0.9f, 1.0f);
        m.vertexTint = true;
        part(crowd, m, false);
    }
    {
        const Vector3 tints[4] = {{0.9f, 0.1f, 0.1f}, {0.08f, 0.3f, 0.85f}, {0.95f, 0.78f, 0.08f}, {0.9f, 0.9f, 0.92f}};
        for (int k = 0; k < 4; ++k) part(ad[k], concrete(tints[k], 0.5f, 2.0f), true);
    }
    {
        gfx::Material m;
        m.layer[0] = &metal_;
        m.scale[0] = 6.0f;
        m.tint = {0.25f, 0.32f, 0.38f};
        m.roughness = 0.08f;
        m.normalStrength = 0.0f;
        part(glassF, m, true);
        m.scale[0] = 2.0f;
        m.tint = {0.7f, 0.72f, 0.75f};
        m.roughness = 0.6f;
        m.normalStrength = 1.0f;
        part(steelF, m, true);
    }
    {
        gfx::Material m;
        m.layer[0] = &rubber_;
        m.scale[0] = 0.6f;
        m.vertexTint = true;
        m.tint = {0.35f, 0.35f, 0.35f};
        part(tyresF, m, true);
    }
    if (fenceF.vertexCount() > 0) {
        if (!chain_.ok()) {
            Texture2D ct = LoadTexture((assetsDir + "/materials/chainlink/albedo.png").c_str());
            if (ct.id) {
                GenTextureMipmaps(&ct);
                SetTextureFilter(ct, TEXTURE_FILTER_TRILINEAR);
                SetTextureWrap(ct, TEXTURE_WRAP_REPEAT);
                chain_ = gfx::flatTextureSet(ct, 0.5f);
            }
        }
        if (chain_.ok()) {
            gfx::Material m;
            m.layer[0] = &chain_;
            m.scale[0] = 1.0f;
            m.alphaCut = 0.35f;
            m.doubleSided = true;
            m.metalness = 0.0f;
            m.tint = {1.8f, 1.8f, 1.85f};
            part(fenceF, m, true, chain_.albedo.id);
        }
    }
    if (nSky > 0 && skyline_.ok()) {
        gfx::Material m;
        m.layer[0] = &skyline_;
        m.scale[0] = 1.0f;
        m.alphaCut = 0.5f;
        m.doubleSided = true;
        m.metalness = 0.0f;
        m.roughness = 1.0f;
        m.normalStrength = 0.0f;
        m.tint = {1.0f, 1.0f, 1.0f};
        part(skylineMb, m, false);
    }
}
