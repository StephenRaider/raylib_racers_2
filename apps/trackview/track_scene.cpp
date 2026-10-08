#include "track_scene.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

#include "raymath.h"

using gfx::Vertex;
using rr::Vec2;

namespace {

float smoothstepf(float e0, float e1, float x) {
    float t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3 - 2 * t);
}

uint32_t hash3(int x, int y, int seed) {
    uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u + (uint32_t)seed * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}
float hashf(int x, int y, int seed) { return (hash3(x, y, seed) & 0xffffff) / 16777215.0f; }

float vnoise(float x, float y, int seed) {
    int x0 = (int)std::floor(x), y0 = (int)std::floor(y);
    float fx = x - x0, fy = y - y0;
    fx = fx * fx * (3 - 2 * fx);
    fy = fy * fy * (3 - 2 * fy);
    float a = hashf(x0, y0, seed), b = hashf(x0 + 1, y0, seed);
    float c = hashf(x0, y0 + 1, seed), d = hashf(x0 + 1, y0 + 1, seed);
    return a + (b - a) * fx + (c - a) * fy + (a - b - c + d) * fx * fy;
}

float fbm(float x, float y, int octaves, int seed) {
    float sum = 0, amp = 0.5f, norm = 0;
    for (int o = 0; o < octaves; ++o) {
        sum += amp * vnoise(x, y, seed + o * 31);
        norm += amp;
        amp *= 0.5f;
        x = x * 2.03f + 17.1f;
        y = y * 2.03f - 9.7f;
    }
    return sum / norm;  // 0..1
}

// Ridged noise for distant hills: sharp crests, rounded valleys.
float ridged(float x, float y, int seed) {
    float sum = 0, amp = 0.5f, norm = 0;
    for (int o = 0; o < 5; ++o) {
        float n = 1.0f - std::fabs(vnoise(x, y, seed + o * 13) * 2 - 1);
        sum += amp * n * n;
        norm += amp;
        amp *= 0.5f;
        x *= 2.1f;
        y *= 2.1f;
    }
    return sum / norm;
}

Vector3 W(Vec2 p, float h) { return {p.x, h, -p.y}; }

// A ribbon along the track: one ring of vertices per sample, quads between consecutive rings.
struct Strip {
    gfx::MeshBuilder* mb;
    std::vector<Vertex> prevV;
    std::vector<int> prev;
    int chunk = -1;
    void ring(const std::vector<Vertex>& vs) {
        mb->reserve((int)vs.size() * 2);
        if (mb->chunk() != chunk) prev.clear();  // a new chunk (here or by other geometry): re-add the previous ring
        chunk = mb->chunk();
        if (prev.empty() && !prevV.empty())
            for (const Vertex& v : prevV) prev.push_back(mb->add(v));
        std::vector<int> cur;
        for (const Vertex& v : vs) cur.push_back(mb->add(v));
        if (!prev.empty() && prev.size() == cur.size())
            for (size_t j = 0; j + 1 < cur.size(); ++j) {
                mb->tri(prev[j], cur[j], cur[j + 1]);
                mb->tri(prev[j], cur[j + 1], prev[j + 1]);
            }
        prev = cur;
        prevV = vs;
    }
    void cut() { prev.clear(); prevV.clear(); }
};

Vertex vert(Vector3 p, Vector3 n, Vector2 uv, unsigned char grass = 255, unsigned char dirt = 0, unsigned char gravel = 0) {
    Vertex v{};
    v.p = p;
    v.n = n;
    v.uv = uv;
    v.c[0] = grass;
    v.c[1] = dirt;
    v.c[2] = gravel;
    v.c[3] = 255;
    return v;
}

// A quad with its own vertices and a flat normal (kerbs, wall ends, lines).
void quad(gfx::MeshBuilder& mb, Vector3 a, Vector3 b, Vector3 c, Vector3 d, Vector2 ta, Vector2 tb, Vector2 tc, Vector2 td) {
    mb.reserve(4);
    Vector3 n = Vector3Normalize(Vector3CrossProduct(Vector3Subtract(b, a), Vector3Subtract(c, a)));
    int ia = mb.add(vert(a, n, ta)), ib = mb.add(vert(b, n, tb)), ic = mb.add(vert(c, n, tc)), id = mb.add(vert(d, n, td));
    mb.tri(ia, ib, ic);
    mb.tri(ia, ic, id);
}

// A box standing on `base`, half-extents ax and az across the ground and h tall: four sides and a top.
void box(gfx::MeshBuilder& mb, Vector3 base, Vector3 ax, Vector3 az, float h) {
    Vector3 up = {0, h, 0};
    Vector3 c[4] = {Vector3Subtract(Vector3Subtract(base, ax), az), Vector3Subtract(Vector3Add(base, ax), az),
                    Vector3Add(Vector3Add(base, ax), az), Vector3Add(Vector3Subtract(base, ax), az)};
    for (int k = 0; k < 4; ++k) {
        Vector3 a = c[k], b = c[(k + 1) % 4];
        Vector3 out = Vector3Subtract(Vector3Scale(Vector3Add(a, b), 0.5f), base);
        Vector3 at = Vector3Add(a, up), bt = Vector3Add(b, up);
        Vector3 nrm = Vector3CrossProduct(Vector3Subtract(b, a), Vector3Subtract(bt, a));
        if (Vector3DotProduct(nrm, out) >= 0) quad(mb, a, b, bt, at, {0, 0}, {0.1f, 0}, {0.1f, h}, {0, h});
        else quad(mb, b, a, at, bt, {0, 0}, {0.1f, 0}, {0.1f, h}, {0, h});
    }
    Vector3 t[4];
    for (int k = 0; k < 4; ++k) t[k] = Vector3Add(c[k], up);
    Vector3 nrm = Vector3CrossProduct(Vector3Subtract(t[1], t[0]), Vector3Subtract(t[2], t[0]));
    if (nrm.y >= 0) quad(mb, t[0], t[1], t[2], t[3], {0, 0}, {0.1f, 0}, {0.1f, 0.1f}, {0, 0.1f});
    else quad(mb, t[0], t[3], t[2], t[1], {0, 0}, {0.1f, 0}, {0.1f, 0.1f}, {0, 0.1f});
}

}  // namespace

// ---------------------------------------------------------------- grids

bool TrackScene::Grid::inside(float x, float z) const {
    return x >= x0 && z >= z0 && x <= x0 + (nx - 1) * step && z <= z0 + (nz - 1) * step;
}

float TrackScene::Grid::at(float x, float z) const {
    float fx = std::clamp((x - x0) / step, 0.0f, nx - 1.001f), fz = std::clamp((z - z0) / step, 0.0f, nz - 1.001f);
    int ix = (int)fx, iz = (int)fz;
    fx -= ix;
    fz -= iz;
    const float* r0 = &h[(size_t)iz * nx + ix];
    const float* r1 = r0 + nx;
    return (r0[0] * (1 - fx) + r0[1] * fx) * (1 - fz) + (r1[0] * (1 - fx) + r1[1] * fx) * fz;
}

float TrackScene::groundHeight(float x, float z) const {
    if (nearGrid_.inside(x, z)) return nearGrid_.at(x, z);
    return farGrid_.at(x, z);
}

int TrackScene::nearest(Vec2 p, float maxDist, float* dist) const {
    int cx = (int)std::floor((p.x - hx0_) / cell_), cy = (int)std::floor((p.y - hy0_) / cell_);
    int best = -1;
    float bestD2 = maxDist * maxDist;
    int rings = (int)std::ceil(maxDist / cell_) + 1;
    for (int r = 0; r <= rings; ++r) {
        if (best >= 0 && (r - 1) * cell_ > std::sqrt(bestD2)) break;
        for (int y = cy - r; y <= cy + r; ++y)
            for (int x = cx - r; x <= cx + r; ++x) {
                if (std::max(std::abs(x - cx), std::abs(y - cy)) != r) continue;  // ring only
                if (x < 0 || y < 0 || x >= hw_ || y >= hh_) continue;
                for (int i : hash_[(size_t)y * hw_ + x]) {
                    Vec2 d = tr_->at(i).p - p;
                    float d2 = rr::dot(d, d);
                    if (d2 < bestD2) bestD2 = d2, best = i;
                }
            }
    }
    if (dist) *dist = std::sqrt(bestD2);
    return best;
}

Vector3 TrackScene::roadPoint(float s, float lateral, float up) const {
    Vec2 p = tr_->pointAt(s, lateral);
    return W(p, tr_->heightAt(s, lateral) + up);
}

// ---------------------------------------------------------------- build

bool TrackScene::build(const rr::Track& track, const std::string& assetsDir, unsigned seed, std::string* err) {
    tr_ = &track;
    const std::string m = assetsDir + "/materials/";
    struct { gfx::TextureSet* set; const char* name; } sets[] = {
        {&asphalt_, "asphalt"}, {&asphaltWorn_, "asphalt_worn"}, {&concrete_, "concrete"}, {&grass_, "grass"},
        {&gravel_, "gravel"}, {&dirt_, "dirt"}, {&metal_, "metal"}};
    for (auto& s : sets)
        if (!gfx::loadTextureSet(m + s.name, s.set, err)) return false;

    // spatial hash of the centreline samples
    float minX = 1e30f, minY = 1e30f, maxX = -1e30f, maxY = -1e30f;
    for (int i = 0; i < track.size(); ++i) {
        Vec2 p = track.at(i).p;
        minX = std::min(minX, p.x); maxX = std::max(maxX, p.x);
        minY = std::min(minY, p.y); maxY = std::max(maxY, p.y);
    }
    hx0_ = minX - 1000;
    hy0_ = minY - 1000;
    hw_ = (int)((maxX - hx0_ + 1000) / cell_) + 1;
    hh_ = (int)((maxY - hy0_ + 1000) / cell_) + 1;
    hash_.assign((size_t)hw_ * hh_, {});
    for (int i = 0; i < track.size(); ++i) {
        Vec2 p = track.at(i).p;
        hash_[(size_t)((int)((p.y - hy0_) / cell_)) * hw_ + (int)((p.x - hx0_) / cell_)].push_back(i);
    }
    centre_ = {(minX + maxX) / 2, 0, -(minY + maxY) / 2};
    extent_ = std::max(maxX - minX, maxY - minY);
    float zsum = 0;
    for (int i = 0; i < track.size(); ++i) zsum += track.at(i).z;
    centre_.y = zsum / track.size();

    // gravel traps on the outside of the faster corners, a little beyond them either way
    const int n = track.size();
    std::vector<float> k(n), g(n, 0.0f);
    for (int i = 0; i < n; ++i) {
        float sum = 0;
        for (int o = -20; o <= 20; ++o) sum += track.at(i + o).curvature;
        k[i] = sum / 41;
    }
    gravelSide_.assign(n, 0.0f);
    for (int i = 0; i < n; ++i) {
        float strength = smoothstepf(1.0f / 260.0f, 1.0f / 110.0f, std::fabs(k[i]));
        if (strength <= 0) continue;
        float side = k[i] > 0 ? -1.0f : 1.0f;  // outside of the bend
        for (int o = -10; o <= 45; ++o) {     // gravel runs on past the exit
            int j = track.wrap(i + o);
            float v = strength * (1.0f - std::max(0, o - 25) / 20.0f);
            if (std::fabs(gravelSide_[j]) < v) gravelSide_[j] = side * v;
        }
    }
    if (track.hasPit())
        for (int i = 0; i < n; ++i)
            if (track.inPitArea(track.at(i).s) && gravelSide_[i] * track.pit().side > 0) gravelSide_[i] = 0;

    buildTerrain(seed);
    buildRoad(seed);
    return true;
}

void TrackScene::unload() {
    for (Part& p : parts_)
        for (Mesh& m : p.meshes) UnloadMesh(m);
    parts_.clear();
    for (gfx::TextureSet* s : {&asphalt_, &asphaltWorn_, &concrete_, &grass_, &gravel_, &dirt_, &metal_})
        gfx::unloadTextureSet(*s);
}

// Height of the ground at a lateral offset from the centreline: the banked road, then
// a gentle drainage fall beyond the tarmac.
static float edgeProfile(float z, float bank, float hw, float lat) {
    float l = std::clamp(lat, -hw, hw);
    float h = z + l * std::tan(bank);
    if (std::fabs(lat) > hw) h -= (std::fabs(lat) - hw) * 0.025f;
    return h;
}

void TrackScene::buildRoad(unsigned seed) {
    const rr::Track& tr = *tr_;
    const int n = tr.size();
    const float L = tr.length();

    auto P = [&](int i, float lat, float dz) {
        const auto& a = tr.at(i);
        return W(a.p + a.n * lat, edgeProfile(a.z, a.bank, a.halfWidth, lat) + dz);
    };
    auto roadNormal = [&](int i) {
        const auto& a = tr.at(i);
        Vector3 t = Vector3Normalize({a.t.x, a.grade, -a.t.y});
        Vector3 l = Vector3Normalize({a.n.x, std::tan(a.bank), -a.n.y});
        return Vector3Normalize(Vector3CrossProduct(t, l));
    };
    auto sAt = [&](int i) { return i >= n ? L : tr.at(i).s; };

    std::vector<char> kerb(n, 0);
    for (int i = 0; i < n; ++i)
        if (std::fabs(tr.at(i).curvature) > 1.0f / 220.0f)
            for (int o = -12; o <= 12; ++o) kerb[tr.wrap(i + o)] = 1;

    gfx::MeshBuilder road, paint, kerbRed, kerbWhite, walls, armco, apron, skirt, startLine;

    // tarmac: five stations across, so the surface can carry camber later
    {
        Strip st{&road};
        for (int i = 0; i <= n; ++i) {
            const auto& a = tr.at(i);
            Vector3 nn = roadNormal(i);
            std::vector<Vertex> ring;
            for (float f : {-1.0f, -0.5f, 0.0f, 0.5f, 1.0f}) {
                float lat = f * a.halfWidth;
                ring.push_back(vert(P(i, lat, 0), nn, {-lat, sAt(i)}));
            }
            st.ring(ring);
        }
    }
    // painted edge lines, and a skirt below the road edge that hides any gap to the terrain
    for (float side : {1.0f, -1.0f}) {
        Strip line{&paint}, sk{&skirt};
        for (int i = 0; i <= n; ++i) {
            const auto& a = tr.at(i);
            Vector3 nn = roadNormal(i);
            float s = sAt(i);
            float in = side * (a.halfWidth - 0.45f), out = side * (a.halfWidth - 0.2f);
            std::vector<Vertex> r = {vert(P(i, in, 0.004f), nn, {in, s}), vert(P(i, out, 0.004f), nn, {out, s})};
            if (side < 0) std::swap(r[0], r[1]);
            line.ring(r);
            float e = side * a.halfWidth, e2 = side * (a.halfWidth + 0.5f);
            Vector3 up = {0, 1, 0};
            std::vector<Vertex> k2 = {vert(P(i, e, 0.0f), up, {e, s}), vert(P(i, e2, -0.35f), up, {e2, s})};
            if (side < 0) std::swap(k2[0], k2[1]);  // keep the front face up
            sk.ring(k2);
        }
    }
    // kerbs: red and white blocks, a low ramp off the tarmac edge
    for (int i = 0; i < n; ++i) {
        if (!kerb[i]) continue;
        bool red = ((int)std::floor(tr.at(i).s / 2.5f)) % 2 == 0;
        gfx::MeshBuilder& mb = red ? kerbRed : kerbWhite;
        float sa = sAt(i), sb = sAt(i + 1);
        for (float side : {1.0f, -1.0f}) {
            const float st[4] = {0.0f, 0.25f, 1.0f, 1.25f};
            const float dz[4] = {0.0f, 0.05f, 0.05f, -0.06f};
            for (int j = 0; j < 3; ++j) {
                float la0 = side * (tr.at(i).halfWidth + st[j]), la1 = side * (tr.at(i).halfWidth + st[j + 1]);
                float lb0 = side * (tr.at(i + 1).halfWidth + st[j]), lb1 = side * (tr.at(i + 1).halfWidth + st[j + 1]);
                // P() applies the drainage fall beyond the edge: add it back so the kerb follows the bank plane
                Vector3 a0 = P(i, la0, dz[j] + st[j] * 0.025f), a1 = P(i, la1, dz[j + 1] + st[j + 1] * 0.025f);
                Vector3 b0 = P(i + 1, lb0, dz[j] + st[j] * 0.025f), b1 = P(i + 1, lb1, dz[j + 1] + st[j + 1] * 0.025f);
                if (side > 0) quad(mb, a0, b0, b1, a1, {la0, sa}, {lb0, sb}, {lb1, sb}, {la1, sa});
                else quad(mb, a0, a1, b1, b0, {la0, sa}, {la1, sa}, {lb1, sb}, {lb0, sb});
            }
        }
    }
    // Barriers, set back by the runoff (further round the pit area): concrete walls (a
    // Jersey profile) along the pit straight, armco on posts everywhere else.
    {
        // (out from the barrier line, up), from the track side over the top
        const std::vector<std::array<float, 2>> jersey = {{0.0f, -0.15f}, {0.06f, 0.28f}, {0.16f, 0.95f}, {0.42f, 0.95f}, {0.52f, 0.28f}, {0.58f, -0.15f}};
        const std::vector<std::array<float, 2>> wbeam = {{0.0f, 0.45f}, {0.05f, 0.53f}, {0.01f, 0.61f}, {0.05f, 0.69f}, {0.0f, 0.77f}};
        auto concreteAt = [&](float s) {
            return tr.hasPit() && (tr.inPitArea(s) || tr.inPitArea(s + 60.0f) || tr.inPitArea(s - 60.0f));
        };
        for (float side : {1.0f, -1.0f}) {
            Strip wall{&walls}, rail{&armco};
            int sd = side > 0 ? 1 : -1;
            float prevOff = -1, nextPost = 0;
            for (int i = 0; i <= n; ++i) {
                const auto& a = tr.at(i);
                const float s = sAt(i);
                float off = tr.barrierOffset(s, sd, a.halfWidth);
                const bool concrete = concreteAt(s);
                if (concrete && prevOff >= 0 && std::fabs(off - prevOff) > 0.5f) {
                    // the barrier steps sideways (pit area): close the end and start a new run
                    wall.cut();
                    float lo = std::min(off, prevOff), hi = std::max(off, prevOff) + 0.58f;
                    float base = edgeProfile(a.z, a.bank, a.halfWidth, side * lo);
                    Vec2 p0 = a.p + a.n * (side * lo), p1 = a.p + a.n * (side * hi);
                    Vector3 q0 = W(p0, base - 0.15f), q1 = W(p1, base - 0.15f), q2 = W(p1, base + 0.95f), q3 = W(p0, base + 0.95f);
                    float facing = off > prevOff ? -1.0f : 1.0f;  // face the side the run continues from
                    if (facing * side > 0) quad(walls, q0, q1, q2, q3, {0, 0}, {hi - lo, 0}, {hi - lo, 1}, {0, 1});
                    else quad(walls, q1, q0, q3, q2, {0, 0}, {hi - lo, 0}, {hi - lo, 1}, {0, 1});
                }
                prevOff = off;
                const auto& prof = concrete ? jersey : wbeam;
                (concrete ? rail : wall).cut();
                float base = edgeProfile(a.z, a.bank, a.halfWidth, side * off);
                Vector3 lw = {a.n.x * side, 0, -a.n.y * side};
                std::vector<Vertex> ring;
                float u = 0;
                const int m = (int)prof.size();
                for (int j = 0; j < m; ++j) {
                    // profile normal from its neighbours
                    int j0 = std::max(j - 1, 0), j1 = std::min(j + 1, m - 1);
                    float dx = prof[j1][0] - prof[j0][0], dy = prof[j1][1] - prof[j0][1];
                    Vector3 nn = Vector3Normalize(Vector3Add(Vector3Scale(lw, -dy), Vector3{0, dx, 0}));
                    if (j > 0) u += std::hypot(prof[j][0] - prof[j - 1][0], prof[j][1] - prof[j - 1][1]);
                    Vec2 q = a.p + a.n * (side * (off + prof[j][0]));
                    ring.push_back(vert(W(q, base + prof[j][1]), nn, {u, s}));
                }
                if (side < 0) std::reverse(ring.begin(), ring.end());
                (concrete ? wall : rail).ring(ring);
                // armco posts every 4 m, just behind the rail
                if (!concrete && s >= nextPost) {
                    nextPost = s + 4.0f;
                    Vec2 c = a.p + a.n * (side * (off + 0.14f));
                    Vector3 ax = {a.t.x * 0.05f, 0, -a.t.y * 0.05f}, az = Vector3Scale(lw, 0.06f);
                    box(armco, W(c, base - 0.2f), ax, az, 1.0f);
                }
            }
        }
    }
    // pit area: apron out to the pit barrier, the pit wall along the lane
    if (tr.hasPit()) {
        const float side = (float)tr.pit().side;
        Strip ap{&apron};
        for (int i = 0; i <= n; ++i) {
            const auto& a = tr.at(i);
            if (!tr.inPitArea(sAt(i))) { ap.cut(); continue; }
            float in = side * (a.halfWidth + 1.25f), out = side * (a.halfWidth + rr::Track::kPitBarrier);
            std::vector<Vertex> r = {vert(P(i, in, 0.0f), {0, 1, 0}, {in, sAt(i)}), vert(P(i, out, 0.0f), {0, 1, 0}, {out, sAt(i)})};
            if (side < 0) std::swap(r[0], r[1]);
            ap.ring(r);
        }
        Strip pw{&walls};
        for (float face : {1.0f, -1.0f}) {
            for (int i = 0; i <= n; ++i) {
                const auto& a = tr.at(i);
                if (!tr.inPitLane(sAt(i))) { pw.cut(); continue; }
                float lat = side * (a.halfWidth + (face > 0 ? rr::Track::kDividerIn : rr::Track::kDividerOut));
                float base = edgeProfile(a.z, a.bank, a.halfWidth, lat);
                Vector3 nn = {-a.n.x * side * face, 0, a.n.y * side * face};
                std::vector<Vertex> r = {vert(W(a.p + a.n * lat, base - 0.1f), nn, {sAt(i), 0}),
                                         vert(W(a.p + a.n * lat, base + 1.1f), nn, {sAt(i), 1.2f})};
                if (face * side < 0) std::swap(r[0], r[1]);
                pw.ring(r);
            }
            pw.cut();
            // the top
        }
        for (int i = 0; i < n; ++i) {
            if (!tr.inPitLane(sAt(i)) || !tr.inPitLane(sAt(i + 1))) continue;
            const auto& a = tr.at(i);
            const auto& b = tr.at(i + 1);
            float ia = side * (a.halfWidth + rr::Track::kDividerIn), oa = side * (a.halfWidth + rr::Track::kDividerOut);
            float ib = side * (b.halfWidth + rr::Track::kDividerIn), ob = side * (b.halfWidth + rr::Track::kDividerOut);
            float ha = edgeProfile(a.z, a.bank, a.halfWidth, ia) + 1.1f, hb = edgeProfile(b.z, b.bank, b.halfWidth, ib) + 1.1f;
            Vector3 a0 = W(a.p + a.n * ia, ha), a1 = W(a.p + a.n * oa, ha), b0 = W(b.p + b.n * ib, hb), b1 = W(b.p + b.n * ob, hb);
            if (side > 0) quad(walls, a0, b0, b1, a1, {0, sAt(i)}, {0, sAt(i + 1)}, {0.6f, sAt(i + 1)}, {0.6f, sAt(i)});
            else quad(walls, a0, a1, b1, b0, {0, sAt(i)}, {0.6f, sAt(i)}, {0.6f, sAt(i + 1)}, {0, sAt(i + 1)});
        }
        // pit lane speed-limit lines and the fast-lane line
        for (float s : {tr.pit().lane_start_s, tr.pit().lane_end_s}) {
            int i = tr.indexAt(s);
            const auto& a = tr.at(i);
            float lo = a.halfWidth + rr::Track::kDividerOut, hi = a.halfWidth + rr::Track::kPitBarrier;
            auto Q = [&](float along, float lat) {
                Vec2 q = a.p + a.t * along + a.n * (side * lat);
                return W(q, edgeProfile(a.z, a.bank, a.halfWidth, side * lat) + 0.006f);
            };
            if (side > 0) quad(paint, Q(-0.3f, lo), Q(0.3f, lo), Q(0.3f, hi), Q(-0.3f, hi), {0, 0}, {0, 0}, {0, 0}, {0, 0});
            else quad(paint, Q(-0.3f, lo), Q(-0.3f, hi), Q(0.3f, hi), Q(0.3f, lo), {0, 0}, {0, 0}, {0, 0}, {0, 0});
        }
    }
    // start / finish line
    {
        const auto& a = tr.at(0);
        auto Q = [&](float along, float lat) { return W(a.p + a.t * along + a.n * lat, edgeProfile(a.z, a.bank, a.halfWidth, lat) + 0.006f); };
        for (int k = 0; k < 2; ++k)
            for (int j = 0; j < (int)(a.halfWidth * 2 / 0.5f); ++j) {
                if ((j + k) % 2) continue;
                float l0 = -a.halfWidth + j * 0.5f, l1 = l0 + 0.5f, f0 = -0.5f + k * 0.5f, f1 = f0 + 0.5f;
                quad(startLine, Q(f0, l0), Q(f1, l0), Q(f1, l1), Q(f0, l1), {0, 0}, {0, 0}, {0, 0}, {0, 0});
            }
        // white bars either side of the chequers
        for (float f0 : {-0.8f, 0.5f})
            quad(paint, Q(f0, -a.halfWidth), Q(f0 + 0.3f, -a.halfWidth), Q(f0 + 0.3f, a.halfWidth), Q(f0, a.halfWidth),
                 {0, 0}, {0, 0}, {0, 0}, {0, 0});
    }

    auto part = [&](gfx::MeshBuilder& mb, gfx::Material mat, bool shadows) {
        Part p;
        p.meshes = mb.build();
        p.mat = mat;
        p.shadows = shadows;
        if (!p.meshes.empty()) parts_.push_back(std::move(p));
    };
    gfx::Material mRoad;
    mRoad.layer[0] = &asphalt_;
    mRoad.scale[0] = 4.0f;
    mRoad.depthBias = 1.0f;
    mRoad.tint = {0.85f, 0.85f, 0.88f};
    part(road, mRoad, true);

    gfx::Material mPaint;
    mPaint.layer[0] = &concrete_;
    mPaint.scale[0] = 2.0f;
    mPaint.tint = {1.15f, 1.15f, 1.15f};
    mPaint.roughness = 0.75f;
    mPaint.normalStrength = 0.3f;
    mPaint.depthBias = 3.0f;
    part(paint, mPaint, false);

    gfx::Material mStart = mPaint;
    mStart.tint = {0.05f, 0.05f, 0.05f};
    part(startLine, mStart, false);

    gfx::Material mKerb = mPaint;
    mKerb.depthBias = 2.0f;
    mKerb.tint = {0.95f, 0.08f, 0.06f};
    mKerb.roughness = 0.6f;
    part(kerbRed, mKerb, true);
    mKerb.tint = {1.1f, 1.1f, 1.1f};
    part(kerbWhite, mKerb, true);

    gfx::Material mWall;
    mWall.layer[0] = &concrete_;
    mWall.scale[0] = 3.0f;
    mWall.tint = {0.9f, 0.9f, 0.88f};
    part(walls, mWall, true);

    gfx::Material mArmco;
    mArmco.layer[0] = &metal_;
    mArmco.scale[0] = 2.0f;
    mArmco.tint = {0.75f, 0.77f, 0.8f};
    mArmco.roughness = 0.7f;
    mArmco.doubleSided = true;
    part(armco, mArmco, true);

    gfx::Material mApron;
    mApron.layer[0] = &asphaltWorn_;
    mApron.scale[0] = 4.0f;
    mApron.depthBias = 1.0f;
    part(apron, mApron, true);

    gfx::Material mSkirt;
    mSkirt.layer[0] = &grass_;
    mSkirt.layer[1] = &dirt_;
    mSkirt.layer[2] = &gravel_;
    mSkirt.scale[0] = 3.5f;
    mSkirt.scale[1] = 3.0f;
    mSkirt.scale[2] = 2.5f;
    mSkirt.macroVariation = true;
    mSkirt.layerTint[0] = {0.6f, 0.68f, 0.46f};
    part(skirt, mSkirt, false);
    (void)seed;
}

void TrackScene::buildTerrain(unsigned seed) {
    const rr::Track& tr = *tr_;
    const int n = tr.size();
    const int sd = (int)seed;

    // Coarse knots of the track for the natural ground's base height.
    struct Knot { Vec2 p; float z; };
    std::vector<Knot> knots;
    for (int i = 0; i < n; i += 10) knots.push_back({tr.at(i).p, tr.at(i).z});
    auto baseAndDist = [&](Vec2 p, float* dmin) {
        float sw = 0, sh = 0, best = 1e30f;
        for (const Knot& k : knots) {
            Vec2 d = k.p - p;
            float d2 = rr::dot(d, d);
            best = std::min(best, d2);
            float w = 1.0f / ((d2 + 1600.0f) * (d2 + 1600.0f));
            sw += w;
            sh += w * k.z;
        }
        *dmin = std::sqrt(best);
        return sh / sw;
    };
    // The land away from the track: rolling moorland rising to hills in the distance.
    auto natural = [&](Vec2 p, float base, float d) {
        float hills = (fbm(p.x / 700.0f, p.y / 700.0f, 5, sd + 3) - 0.42f) * 120.0f * smoothstepf(30.0f, 320.0f, d);
        float swell = (fbm(p.x / 160.0f, p.y / 160.0f, 3, sd + 7) - 0.5f) * 14.0f * smoothstepf(20.0f, 120.0f, d);
        float far = ridged(p.x / 2600.0f, p.y / 2600.0f, sd + 9) * 300.0f * smoothstepf(900.0f, 3600.0f, d);
        return base + hills + swell + far;
    };

    // --- far grid: 25 m, out to ~7 km from the middle of the track
    Grid& F = farGrid_;
    F.step = 25.0f;
    const float farHalf = std::max(7000.0f, extent_ * 2.5f);
    F.nx = F.nz = (int)(2 * farHalf / F.step) + 1;
    F.x0 = centre_.x - farHalf;
    F.z0 = centre_.z - farHalf;
    F.h.assign((size_t)F.nx * F.nz, 0.0f);
    std::vector<float> farDist(F.h.size());
    for (int iz = 0; iz < F.nz; ++iz)
        for (int ix = 0; ix < F.nx; ++ix) {
            Vec2 p{F.x0 + ix * F.step, -(F.z0 + iz * F.step)};
            float d;
            float b = baseAndDist(p, &d);
            F.h[(size_t)iz * F.nx + ix] = natural(p, b, d);
            farDist[(size_t)iz * F.nx + ix] = d;
        }

    // --- near grid: 2 m around the track
    Grid& G = nearGrid_;
    G.step = 2.0f;
    float minX = 1e30f, minZ = 1e30f, maxX = -1e30f, maxZ = -1e30f;
    for (int i = 0; i < n; ++i) {
        Vector3 w = W(tr.at(i).p, 0);
        minX = std::min(minX, w.x); maxX = std::max(maxX, w.x);
        minZ = std::min(minZ, w.z); maxZ = std::max(maxZ, w.z);
    }
    const float margin = 320.0f;
    G.x0 = std::floor((minX - margin) / F.step) * F.step;
    G.z0 = std::floor((minZ - margin) / F.step) * F.step;
    G.nx = (int)((maxX + margin - G.x0) / G.step) + 1;
    G.nz = (int)((maxZ + margin - G.z0) / G.step) + 1;
    G.h.assign((size_t)G.nx * G.nz, 0.0f);
    std::vector<unsigned char> wDirt(G.h.size(), 0), wGravel(G.h.size(), 0);
    std::vector<int> hint(G.h.size(), -1);
    // a natural field without the near-track blend, from the far grid's
    Grid natF = F;
    for (int iz = 0; iz < G.nz; ++iz)
        for (int ix = 0; ix < G.nx; ++ix) {
            float x = G.x0 + ix * G.step, z = G.z0 + iz * G.step;
            Vec2 p{x, -z};
            float nat = natF.at(x, z);
            float d;
            int i = nearest(p, 360.0f, &d);
            float h = nat + (fbm(x / 23.0f, z / 23.0f, 3, sd + 21) - 0.5f) * 1.6f * smoothstepf(30.0f, 120.0f, d);
            float dirt = smoothstepf(0.62f, 0.78f, fbm(x / 60.0f, z / 60.0f, 4, sd + 5)) * 0.8f;
            float gravel = 0;
            if (i >= 0) {
                rr::TrackLoc loc = tr.locate(p, i, 4);
                const auto& a = tr.at(loc.idx);
                const auto& b = tr.at(loc.idx + 1);
                float f = std::clamp((loc.s - a.s) / tr.spacing(), 0.0f, 1.0f);
                float z0 = a.z + (b.z - a.z) * f, bank = a.bank + (b.bank - a.bank) * f;
                float lat = loc.lateral, al = std::fabs(lat);
                int side = lat > 0 ? 1 : -1;
                float barrier = tr.barrierOffset(loc.s, side, loc.halfWidth);
                float edge = edgeProfile(z0, bank, loc.halfWidth, lat);
                // embankments and cuttings no steeper than about 1 in 2.5
                float band = 25.0f + 2.5f * std::fabs(h - edge);
                float w = smoothstepf(barrier + 1.5f, barrier + 1.5f + band, al);
                h = edge + (h - edge) * w;
                bool paved = al < loc.halfWidth + 1.3f || (tr.hasPit() && tr.inPitArea(loc.s) && side == tr.pit().side &&
                                                           al < loc.halfWidth + rr::Track::kPitBarrier + 0.5f);
                if (paved) h -= 0.06f;
                float gs = gravelSide_[loc.idx];
                if (gs * lat > 0)
                    gravel = std::fabs(gs) * smoothstepf(loc.halfWidth + 1.4f, loc.halfWidth + 2.6f, al) *
                             (1.0f - smoothstepf(barrier - 1.6f, barrier - 0.6f, al));
                // worn earth along the foot of the barrier, none on the verge
                float foot = 1.0f - smoothstepf(0.3f, 1.4f, std::fabs(al - barrier - 0.3f));
                dirt = std::max(dirt * smoothstepf(barrier, barrier + 8.0f, al), foot * 0.7f);
            }
            size_t k = (size_t)iz * G.nx + ix;
            G.h[k] = h;
            wDirt[k] = (unsigned char)(std::clamp(dirt, 0.0f, 1.0f) * 255);
            wGravel[k] = (unsigned char)(std::clamp(gravel, 0.0f, 1.0f) * 255);
        }
    // the far grid hides under the near one
    for (int iz = 0; iz < F.nz; ++iz)
        for (int ix = 0; ix < F.nx; ++ix) {
            float x = F.x0 + ix * F.step, z = F.z0 + iz * F.step;
            if (!G.inside(x, z)) continue;
            bool border = x < G.x0 + F.step || z < G.z0 + F.step || x > G.x0 + (G.nx - 1) * G.step - F.step ||
                          z > G.z0 + (G.nz - 1) * G.step - F.step;
            F.h[(size_t)iz * F.nx + ix] = G.at(x, z) - (border ? 0.0f : 3.0f);
        }

    auto emitGrid = [&](const Grid& g, int chunk, const std::vector<unsigned char>* dirtW,
                        const std::vector<unsigned char>* gravelW, bool skipNear) {
        gfx::MeshBuilder mb;
        for (int cz = 0; cz < g.nz - 1; cz += chunk)
            for (int cx = 0; cx < g.nx - 1; cx += chunk) {
                int ex = std::min(cx + chunk, g.nx - 1), ez = std::min(cz + chunk, g.nz - 1);
                if (skipNear) {  // a far chunk entirely under the near grid
                    float x0 = g.x0 + cx * g.step, z0 = g.z0 + cz * g.step;
                    float x1 = g.x0 + ex * g.step, z1 = g.z0 + ez * g.step;
                    if (nearGrid_.inside(x0 - g.step, z0 - g.step) && nearGrid_.inside(x1 + g.step, z1 + g.step)) continue;
                }
                int w = ex - cx + 1, hgt = ez - cz + 1;
                mb.reserve(65535);  // one chunk per mesh, so whole meshes cull together later
                std::vector<int> idx((size_t)w * hgt);
                for (int iz = cz; iz <= ez; ++iz)
                    for (int ix = cx; ix <= ex; ++ix) {
                        size_t k = (size_t)iz * g.nx + ix;
                        float x = g.x0 + ix * g.step, z = g.z0 + iz * g.step;
                        auto H = [&](int xx, int zz) {
                            xx = std::clamp(xx, 0, g.nx - 1);
                            zz = std::clamp(zz, 0, g.nz - 1);
                            return g.h[(size_t)zz * g.nx + xx];
                        };
                        Vector3 nn = Vector3Normalize({H(ix - 1, iz) - H(ix + 1, iz), 2 * g.step, H(ix, iz - 1) - H(ix, iz + 1)});
                        float dirt = dirtW ? (*dirtW)[k] / 255.0f : smoothstepf(0.55f, 0.75f, fbm(x / 90.0f, z / 90.0f, 3, sd + 5));
                        dirt = std::max(dirt, smoothstepf(0.93f, 0.80f, nn.y));  // steep ground is bare
                        float gravel = gravelW ? (*gravelW)[k] / 255.0f : 0.0f;
                        float grass = std::max(0.0f, 1.0f - dirt - gravel);
                        idx[(size_t)(iz - cz) * w + (ix - cx)] =
                            mb.add(vert({x, g.h[k], z}, nn, {x, z}, (unsigned char)(grass * 255), (unsigned char)(dirt * 255),
                                        (unsigned char)(gravel * 255)));
                    }
                for (int iz = 0; iz < hgt - 1; ++iz)
                    for (int ix = 0; ix < w - 1; ++ix) {
                        int v00 = idx[(size_t)iz * w + ix], v10 = idx[(size_t)iz * w + ix + 1];
                        int v01 = idx[(size_t)(iz + 1) * w + ix], v11 = idx[(size_t)(iz + 1) * w + ix + 1];
                        mb.tri(v00, v01, v11);
                        mb.tri(v00, v11, v10);
                    }
            }
        return mb.build();
    };

    gfx::Material mTerrain;
    mTerrain.layer[0] = &grass_;
    mTerrain.layer[1] = &dirt_;
    mTerrain.layer[2] = &gravel_;
    mTerrain.scale[0] = 3.5f;
    mTerrain.scale[1] = 3.0f;
    mTerrain.scale[2] = 2.5f;
    mTerrain.macroVariation = true;
    mTerrain.layerTint[0] = {0.6f, 0.68f, 0.46f};  // a greener, darker moorland grass
    mTerrain.layerTint[1] = {0.62f, 0.56f, 0.55f};  // peat and heather rather than red earth
    mTerrain.roughness = 1.3f;  // no sun glints off the grass
    mTerrain.normalStrength = 0.7f;
    Part nearP;
    nearP.meshes = emitGrid(G, 128, &wDirt, &wGravel, false);
    nearP.mat = mTerrain;
    nearP.shadows = true;
    parts_.push_back(std::move(nearP));
    Part farP;
    farP.meshes = emitGrid(F, 128, nullptr, nullptr, true);
    farP.mat = mTerrain;
    farP.mat.scale[0] = 9.0f;  // the far grid only ever shows from a distance
    farP.mat.scale[1] = 9.0f;
    farP.shadows = false;
    parts_.push_back(std::move(farP));
}

// ---------------------------------------------------------------- draw

void TrackScene::drawShadows(gfx::Renderer& r) const {
    for (const Part& p : parts_)
        if (p.shadows)
            for (const Mesh& m : p.meshes) r.drawShadow(m, MatrixIdentity());
}

void TrackScene::draw(gfx::Renderer& r) const {
    for (const Part& p : parts_)
        for (const Mesh& m : p.meshes) r.draw(m, p.mat, MatrixIdentity());
}
