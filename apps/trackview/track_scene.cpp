#include "track_scene.hpp"

#ifdef _MSC_VER
#pragma warning(disable : 4996)  // sscanf
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <map>

#include "mini_json.hpp"

#include "raymath.h"
#include "rlgl.h"

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
        {&gravel_, "gravel"}, {&dirt_, "dirt"}, {&metal_, "metal"}, {&rubber_, "rubber"}};
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

    seed_ = seed;
    buildTerrain(seed);
    buildRoad(seed);
    buildTrees(assetsDir, seed);
    buildProps(assetsDir);
    for (Part& p : parts_) {
        p.boxes.clear();
        for (Mesh& mesh : p.lod) {
            for (float** a : {&mesh.vertices, &mesh.normals, &mesh.tangents, &mesh.texcoords, &mesh.texcoords2}) {
                MemFree(*a);
                *a = nullptr;
            }
            MemFree(mesh.colors);
            mesh.colors = nullptr;
        }
        for (Mesh& mesh : p.meshes) {
            p.boxes.push_back(GetMeshBoundingBox(mesh));
            for (float** a : {&mesh.vertices, &mesh.normals, &mesh.tangents, &mesh.texcoords, &mesh.texcoords2}) {
                MemFree(*a);
                *a = nullptr;
            }
            MemFree(mesh.colors);
            mesh.colors = nullptr;
        }
    }
    return true;
}

void TrackScene::unload() {
    for (Part& p : parts_) {
        for (Mesh& m : p.meshes) UnloadMesh(m);
        for (Mesh& m : p.lod) UnloadMesh(m);
    }
    parts_.clear();
    for (TreeType& tt : treeTypes_) {
        if (tt.lod[1].vaoId != tt.lod[0].vaoId) UnloadMesh(tt.lod[1]);
        UnloadMesh(tt.lod[0]);
        for (auto& pass : tt.vbo)
            for (unsigned v : pass)
                if (v) rlUnloadVertexBuffer(v);
    }
    treeTypes_.clear();
    treeTiles_.clear();
    for (Texture2D* t : {&colorNear_, &colorFar_})
        if (t->id) UnloadTexture(*t), t->id = 0;
    for (gfx::TextureSet* s : {&asphalt_, &asphaltWorn_, &concrete_, &grass_, &gravel_, &dirt_, &metal_, &rubber_, &foliage_, &chain_})
        gfx::unloadTextureSet(*s);  // foliage_ owns the tree atlas
    treeAtlas_ = Texture2D{};
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
    for (int i = 0; i < n; ++i) kerb[i] = tr.kerbAt(i);

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
                if (sd == tr.pit().side && garagesAt(s) && garagesAt(sAt(std::max(i - 1, 0)))) {
                    wall.cut();
                    rail.cut();
                    prevOff = -1;
                    continue;
                }
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
            // joined to the tarmac edge, widening smoothly from the track at the pit entry to the
            // full lane, and narrowing back onto the track at the exit
            const float L = tr.length(), s = sAt(i);
            auto fwd = [&](float a0, float b0) { float d = std::fmod(b0 - a0 + 2 * L, L); return d; };
            const RRPitInfo& pi = tr.pit();
            float grow = std::min(fwd(pi.entry_s, s) / std::max(1.0f, fwd(pi.entry_s, pi.lane_start_s)),
                                  fwd(s, pi.exit_s) / std::max(1.0f, fwd(pi.lane_end_s, pi.exit_s)));
            grow = smoothstepf(0.0f, 1.0f, std::min(grow, 1.0f));
            float in = side * a.halfWidth, out = side * (a.halfWidth + 0.6f + (rr::Track::kPitBarrier - 0.6f) * grow);
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
    mRoad.detile = true;
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
    mApron.detile = true;
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
    std::vector<unsigned char> wDirt(G.h.size(), 0), wGravel(G.h.size(), 0), cover(G.h.size(), 0);
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
            float forest = plantation(p);
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
                float flat = flatUntil(loc.s, side, loc.halfWidth);
                float w = smoothstepf(flat + 1.5f, flat + 1.5f + band, al);
                h = edge + (h - edge) * w;
                bool paved = al < loc.halfWidth + 1.3f || (tr.hasPit() && tr.inPitArea(loc.s) && side == tr.pit().side &&
                                                           al < loc.halfWidth + rr::Track::kPitBarrier + 0.5f);
                if (garagesAt(loc.s) && side == tr.pit().side && al < flat) paved = true;  // under the paddock slab
                if (paved) h -= 0.06f;
                float gs = tr.gravelSide(loc.idx);
                if (gs * lat > 0)
                    gravel = std::fabs(gs) * smoothstepf(loc.halfWidth + 1.4f, loc.halfWidth + 2.6f, al) *
                             (1.0f - smoothstepf(barrier - 1.6f, barrier - 0.6f, al));
                // strips the track file names ("surface gravel left 100 200")
                const int zs = tr.zoneSurface(loc.s, lat, loc.halfWidth);
                if (zs == RR_SURF_GRAVEL) gravel = 1.0f;
                // worn earth along the foot of the barrier, none on the verge
                float foot = 1.0f - smoothstepf(0.3f, 1.4f, std::fabs(al - barrier - 0.3f));
                dirt = std::max(dirt * smoothstepf(barrier, barrier + 8.0f, al), foot * 0.7f);
                if (zs == RR_SURF_DIRT) dirt = 0.95f;
                forest *= smoothstepf(barrier + 10.0f, barrier + 16.0f, al);
                if (tr.hasPit() && tr.inPitArea(loc.s)) forest *= smoothstepf(88.0f, 96.0f, al);  // as the trees
            }
            dirt = std::max(dirt, forest * 0.9f);  // needles and bare earth under the trees
            cover[(size_t)iz * G.nx + ix] = (unsigned char)(forest * 255);
            size_t k = (size_t)iz * G.nx + ix;
            G.h[k] = h;
            wDirt[k] = (unsigned char)(std::clamp(dirt, 0.0f, 1.0f) * 255);
            wGravel[k] = (unsigned char)(std::clamp(gravel, 0.0f, 1.0f) * 255);
        }
    // Baked sky occlusion: how much of the sky each point sees past the hills around it
    // (horizon angles in 12 directions), and darker ground under the plantations.
    std::vector<unsigned char> aoNear(G.h.size(), 255);
    {
        const int dirs = 12;
        const float dist[] = {4, 8, 14, 24, 40, 64, 100, 160};
        for (int iz = 0; iz < G.nz; ++iz)
            for (int ix = 0; ix < G.nx; ++ix) {
                float x = G.x0 + ix * G.step, z = G.z0 + iz * G.step;
                size_t k = (size_t)iz * G.nx + ix;
                float h0 = G.h[k], vis = 0;
                for (int d = 0; d < dirs; ++d) {
                    float a = d * 2 * PI / dirs, ca = std::cos(a), sa = std::sin(a), tmax = 0;
                    for (float r : dist) {
                        float px = x + ca * r, pz = z + sa * r;
                        float hh = G.inside(px, pz) ? G.at(px, pz) : F.at(px, pz);
                        tmax = std::max(tmax, (hh - h0) / r);
                    }
                    vis += 1.0f / (1.0f + tmax * tmax);  // cos^2 of the horizon angle
                }
                vis /= dirs;
                vis *= 1.0f - 0.45f * cover[k] / 255.0f;
                aoNear[k] = (unsigned char)(std::clamp(vis, 0.0f, 1.0f) * 255);
            }
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

    // Chunks of chunk x chunk cells, each its own mesh; stride > 1 keeps every stride-th
    // vertex (a coarser level of detail). Skirts hang 3 m down from the chunk edges to
    // hide the cracks where chunks of different detail meet.
    auto emitGrid = [&](const Grid& g, int chunk, const std::vector<unsigned char>* dirtW,
                        const std::vector<unsigned char>* gravelW, const std::vector<unsigned char>* aoW, bool skipNear,
                        int stride = 1, bool skirts = false) {
        gfx::MeshBuilder mb;
        for (int cz = 0; cz < g.nz - 1; cz += chunk)
            for (int cx = 0; cx < g.nx - 1; cx += chunk) {
                int ex = std::min(cx + chunk, g.nx - 1), ez = std::min(cz + chunk, g.nz - 1);
                if (skipNear) {  // a far chunk entirely under the near grid
                    float x0 = g.x0 + cx * g.step, z0 = g.z0 + cz * g.step;
                    float x1 = g.x0 + ex * g.step, z1 = g.z0 + ez * g.step;
                    if (nearGrid_.inside(x0 - g.step, z0 - g.step) && nearGrid_.inside(x1 + g.step, z1 + g.step)) continue;
                }
                std::vector<int> xs, zs;
                for (int v = cx; v < ex; v += stride) xs.push_back(v);
                xs.push_back(ex);
                for (int v = cz; v < ez; v += stride) zs.push_back(v);
                zs.push_back(ez);
                int w = (int)xs.size(), hgt = (int)zs.size();
                mb.reserve(65535);  // one chunk per mesh, so whole meshes cull together later
                std::vector<int> idx((size_t)w * hgt);
                std::vector<Vertex> verts((size_t)w * hgt);
                for (int jz = 0; jz < hgt; ++jz)
                    for (int jx = 0; jx < w; ++jx) {
                        const int ix = xs[jx], iz = zs[jz];
                        size_t k = (size_t)iz * g.nx + ix;
                        float x = g.x0 + ix * g.step, z = g.z0 + iz * g.step;
                        auto H = [&](int xx, int zz) {
                            xx = std::clamp(xx, 0, g.nx - 1);
                            zz = std::clamp(zz, 0, g.nz - 1);
                            return g.h[(size_t)zz * g.nx + xx];
                        };
                        Vector3 nn = Vector3Normalize({H(ix - stride, iz) - H(ix + stride, iz), 2 * g.step * stride,
                                                       H(ix, iz - stride) - H(ix, iz + stride)});
                        float dirt = dirtW ? (*dirtW)[k] / 255.0f : smoothstepf(0.55f, 0.75f, fbm(x / 90.0f, z / 90.0f, 3, sd + 5));
                        dirt = std::max(dirt, smoothstepf(0.93f, 0.80f, nn.y));  // steep ground is bare
                        float gravel = gravelW ? (*gravelW)[k] / 255.0f : 0.0f;
                        float grass = std::max(0.0f, 1.0f - dirt - gravel);
                        Vertex v = vert({x, g.h[k], z}, nn, {x, z}, (unsigned char)(grass * 255), (unsigned char)(dirt * 255),
                                        (unsigned char)(gravel * 255));
                        if (aoW) v.c[3] = (*aoW)[k];
                        verts[(size_t)jz * w + jx] = v;
                        idx[(size_t)jz * w + jx] = mb.add(v);
                    }
                for (int iz = 0; iz < hgt - 1; ++iz)
                    for (int ix = 0; ix < w - 1; ++ix) {
                        int v00 = idx[(size_t)iz * w + ix], v10 = idx[(size_t)iz * w + ix + 1];
                        int v01 = idx[(size_t)(iz + 1) * w + ix], v11 = idx[(size_t)(iz + 1) * w + ix + 1];
                        mb.tri(v00, v01, v11);
                        mb.tri(v00, v11, v10);
                    }
                if (skirts) {
                    auto edge = [&](int j0, int step, int count) {
                        int prevTop = -1, prevBot = -1;
                        for (int q = 0; q < count; ++q) {
                            int j = j0 + q * step;
                            Vertex low = verts[j];
                            low.p.y -= 3.0f;
                            int top = idx[j], bot = mb.add(low);
                            if (prevTop >= 0) {  // both windings: the skirt shows from either side
                                mb.tri(prevTop, prevBot, bot);
                                mb.tri(prevTop, bot, top);
                                mb.tri(prevTop, bot, prevBot);
                                mb.tri(prevTop, top, bot);
                            }
                            prevTop = top;
                            prevBot = bot;
                        }
                    };
                    edge(0, 1, w);                    // z = first row
                    edge((hgt - 1) * w, 1, w);        // last row
                    edge(0, w, hgt);                  // x = first column
                    edge(w - 1, w, hgt);              // last column
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
    gfx::Material nearMat, farMat;
    // Baked colour maps for the distance: each vertex's layer mix of the layers' mean
    // colours (what the textures average to far away), one texel per grid vertex.
    {
        Vector3 mean[3];
        const gfx::TextureSet* sets[3] = {&grass_, &dirt_, &gravel_};
        for (int l = 0; l < 3; ++l) {
            Image img = LoadImageFromTexture(sets[l]->albedo);
            ImageFormat(&img, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
            double acc[3] = {0, 0, 0};
            const unsigned char* px = (const unsigned char*)img.data;
            const int n = img.width * img.height;
            for (int k = 0; k < n; k += 7)
                for (int c = 0; c < 3; ++c) acc[c] += std::pow(px[k * 4 + c] / 255.0, 2.2);
            UnloadImage(img);
            const int samples = (n + 6) / 7;
            mean[l] = {(float)(acc[0] / samples), (float)(acc[1] / samples), (float)(acc[2] / samples)};
            mean[l] = Vector3Multiply(mean[l], mTerrain.layerTint[l]);
        }
        auto bake = [&](const Grid& g, const std::vector<unsigned char>* dirtW, const std::vector<unsigned char>* gravelW,
                        Texture2D* out, gfx::Material* m) {
            Image img = GenImageColor(g.nx, g.nz, BLACK);
            Color* px = (Color*)img.data;
            for (int iz = 0; iz < g.nz; ++iz)
                for (int ix = 0; ix < g.nx; ++ix) {
                    size_t k = (size_t)iz * g.nx + ix;
                    float x = g.x0 + ix * g.step, z = g.z0 + iz * g.step;
                    auto H = [&](int xx, int zz) {
                        return g.h[(size_t)std::clamp(zz, 0, g.nz - 1) * g.nx + std::clamp(xx, 0, g.nx - 1)];
                    };
                    float ny = 2 * g.step / std::sqrt(std::pow(H(ix - 1, iz) - H(ix + 1, iz), 2.0f) +
                                                       std::pow(H(ix, iz - 1) - H(ix, iz + 1), 2.0f) + 4 * g.step * g.step);
                    float dirt = dirtW ? (*dirtW)[k] / 255.0f : smoothstepf(0.55f, 0.75f, fbm(x / 90.0f, z / 90.0f, 3, sd + 5));
                    dirt = std::max(dirt, smoothstepf(0.93f, 0.80f, ny));
                    float gravel = gravelW ? (*gravelW)[k] / 255.0f : 0.0f;
                    float grass = std::max(0.0f, 1.0f - dirt - gravel);
                    float sum = std::max(grass + dirt + gravel, 1e-4f);
                    Vector3 c = Vector3Scale(Vector3Add(Vector3Add(Vector3Scale(mean[0], grass), Vector3Scale(mean[1], dirt)),
                                                        Vector3Scale(mean[2], gravel)), 1.0f / sum);
                    px[iz * g.nx + ix] = Color{(unsigned char)(std::pow(std::min(c.x, 1.0f), 1 / 2.2f) * 255),
                                               (unsigned char)(std::pow(std::min(c.y, 1.0f), 1 / 2.2f) * 255),
                                               (unsigned char)(std::pow(std::min(c.z, 1.0f), 1 / 2.2f) * 255), 255};
                }
            *out = LoadTextureFromImage(img);
            UnloadImage(img);
            GenTextureMipmaps(out);
            SetTextureFilter(*out, TEXTURE_FILTER_TRILINEAR);
            SetTextureWrap(*out, TEXTURE_WRAP_CLAMP);
            m->colorMap = out->id;
            // texel centres on the grid vertices
            m->colorMapRect = {g.x0 - 0.5f * g.step, g.z0 - 0.5f * g.step, 1.0f / (g.nx * g.step), 1.0f / (g.nz * g.step)};
        };
        gfx::Material near = mTerrain, far = mTerrain;
        bake(G, &wDirt, &wGravel, &colorNear_, &near);
        bake(F, nullptr, nullptr, &colorFar_, &far);
        far.colorMapRange = {0, 1};  // the far grid is all distance
        nearMat = near;
        farMat = far;
    }
    Part nearP;
    nearP.meshes = emitGrid(G, 128, &wDirt, &wGravel, &aoNear, false, 1, true);
    nearP.lod = emitGrid(G, 128, &wDirt, &wGravel, &aoNear, false, 4, true);
    nearP.lodDist = 300.0f;
    nearP.mat = nearMat;
    nearP.shadows = true;
    parts_.push_back(std::move(nearP));
    Part farP;
    farP.meshes = emitGrid(F, 128, nullptr, nullptr, nullptr, true);
    farP.mat = farMat;
    farP.mat.scale[0] = 9.0f;  // the far grid only ever shows from a distance
    farP.mat.scale[1] = 9.0f;
    farP.shadows = false;
    parts_.push_back(std::move(farP));
}

// ---------------------------------------------------------------- trees

float TrackScene::plantation(Vec2 p) const {
    float m = fbm(p.x / 420.0f + 31.0f, p.y / 420.0f - 7.0f, 4, (int)seed_ + 40);
    return smoothstepf(0.57f, 0.63f, m);
}

namespace {

struct TreeMesh {
    std::vector<Vector3> p, n;
    std::vector<Vector2> uv;
    float height = 1;
};

// The tree OBJs written by tools/import_trees.py: one v/vt/vn per corner, in triangle order.
bool loadTreeObj(const std::string& path, TreeMesh* out) {
    char* text = LoadFileText(path.c_str());
    if (!text) return false;
    for (char* line = text; *line;) {
        char* end = line;
        while (*end && *end != '\n') ++end;
        float a = 0, b = 0, c = 0;
        if (line[0] == 'v' && line[1] == ' ' && std::sscanf(line + 2, "%f %f %f", &a, &b, &c) == 3) out->p.push_back({a, b, c});
        else if (line[0] == 'v' && line[1] == 't' && std::sscanf(line + 3, "%f %f", &a, &b) == 2) out->uv.push_back({a, 1 - b});
        else if (line[0] == 'v' && line[1] == 'n' && std::sscanf(line + 3, "%f %f %f", &a, &b, &c) == 3) out->n.push_back({a, b, c});
        line = *end ? end + 1 : end;
    }
    UnloadFileText(text);
    if (out->p.empty() || out->uv.size() != out->p.size() || out->n.size() != out->p.size()) return false;
    out->height = 0;
    for (const Vector3& v : out->p) out->height = std::max(out->height, v.y);
    return out->height > 0;
}

}  // namespace

void TrackScene::buildTrees(const std::string& assetsDir, unsigned seed) {
    const std::string dir = assetsDir + "/scenery/trees/";
    char* text = LoadFileText((dir + "trees.json").c_str());
    if (!text) return;
    const mjson::Value root = mjson::parse(text);
    UnloadFileText(text);
    std::vector<TreeMesh> meshes;
    std::vector<int> conifers, broadleaf, bushes;  // indices into meshes
    std::string atlas;
    const mjson::Value& vs = root["variants"];
    for (size_t i = 0; i < vs.size(); ++i) {
        const mjson::Value& part = vs[i]["parts"][0];
        TreeMesh tm;
        if (!loadTreeObj(dir + part["mesh"].str(), &tm)) continue;
        atlas = part["texture"].str();
        const std::string kind = vs[i]["kind"].str();
        // the tall pack trees (over 24 m in the source) are the spruces and pines
        int id = (int)meshes.size();
        meshes.push_back(tm);
        if (kind == "bush") bushes.push_back(id);
        else if (vs[i]["height"].num() > 24.0) conifers.push_back(id);
        else broadleaf.push_back(id);
    }
    if (conifers.empty() || atlas.empty()) return;
    treeAtlas_ = LoadTexture((dir + atlas).c_str());
    GenTextureMipmaps(&treeAtlas_);
    SetTextureFilter(treeAtlas_, TEXTURE_FILTER_TRILINEAR);
    foliage_ = gfx::flatTextureSet(treeAtlas_, 0.85f);
    if (broadleaf.empty()) broadleaf = conifers;
    if (bushes.empty()) bushes = broadleaf;

    const rr::Track& tr = *tr_;
    const Grid& G = nearGrid_;
    const int sd = (int)seed + 77;
    const float cellSize = 5.5f, tile = 96.0f;
    struct Inst { int m; Vector3 base; float yaw, scale; Vector3 tint; };
    std::map<std::pair<int, int>, std::vector<Inst>> tiles;
    int count = 0;
    for (float y = G.z0 + 10; y < G.z0 + (G.nz - 1) * G.step - 10; y += cellSize)
        for (float x = G.x0 + 10; x < G.x0 + (G.nx - 1) * G.step - 10; x += cellSize) {
            int cx = (int)std::floor(x / cellSize), cy = (int)std::floor(y / cellSize);
            float wx = x + (hashf(cx, cy, sd) - 0.5f) * cellSize * 0.9f;
            float wz = y + (hashf(cx, cy, sd + 1) - 0.5f) * cellSize * 0.9f;
            Vec2 p{wx, -wz};
            float forest = plantation(p);
            float dist;
            int i = nearest(p, 400.0f, &dist);
            if (i >= 0) {
                rr::TrackLoc loc = tr.locate(p, i, 4);
                float al = std::fabs(loc.lateral);
                float barrier = tr.barrierOffset(loc.s, loc.lateral > 0 ? 1 : -1, loc.halfWidth);
                if (al < barrier + 14.0f) continue;  // keep the runoff and the view clear
                if (tr.hasPit() && tr.inPitArea(loc.s) && al < 90.0f) continue;  // the paddock
                forest *= smoothstepf(barrier + 10.0f, barrier + 16.0f, al);
                if (tr.hasPit() && tr.inPitArea(loc.s)) forest *= smoothstepf(88.0f, 96.0f, al);  // as the trees
            }
            float r = hashf(cx, cy, sd + 2);
            int m = -1;
            float scale = 1;
            int pick = (int)(hashf(cx, cy, sd + 3) * 1000);
            if (r < forest * 0.92f) {
                m = conifers[pick % conifers.size()];
                scale = 13.0f + 8.0f * hashf(cx, cy, sd + 4);
            } else if (r < forest * 0.92f + 0.08f * smoothstepf(0.05f, 0.4f, forest) + 0.012f) {
                bool bush = hashf(cx, cy, sd + 5) < 0.55f;
                m = bush ? bushes[pick % bushes.size()] : broadleaf[pick % broadleaf.size()];
                scale = bush ? 1.4f + 2.2f * hashf(cx, cy, sd + 4) : 7.0f + 7.0f * hashf(cx, cy, sd + 4);
            }
            if (m < 0) continue;
            // not on steep banks
            float h0 = groundHeight(wx - 2, wz), h1 = groundHeight(wx + 2, wz);
            float h2 = groundHeight(wx, wz - 2), h3 = groundHeight(wx, wz + 2);
            if (std::max(std::fabs(h1 - h0), std::fabs(h3 - h2)) > 2.4f) continue;
            float hue = hashf(cx, cy, sd + 6);
            Vector3 tint = {0.82f + 0.2f * hue, 0.86f + 0.16f * hashf(cx, cy, sd + 7), 0.8f + 0.12f * hue};
            Inst in{m, {wx, std::min({h0, h1, h2, h3}) - 0.1f, wz}, hashf(cx, cy, sd + 8) * 2 * PI, scale / meshes[m].height, tint};
            tiles[{(int)std::floor(wx / tile), (int)std::floor(wz / tile)}].push_back(in);
            ++count;
        }
    // one mesh per kind of tree, in two levels of detail
    treeTypes_.assign(meshes.size(), TreeType{});
    for (size_t k = 0; k < meshes.size(); ++k) {
        const TreeMesh& m = meshes[k];
        TreeType& tt = treeTypes_[k];
        tt.bush = std::find(bushes.begin(), bushes.end(), (int)k) != bushes.end();
        // the lower branches inside a plantation get little light
        const float aoFloor = std::find(conifers.begin(), conifers.end(), (int)k) != conifers.end() ? 0.35f : 0.6f;
        for (int lod = 0; lod < 2; ++lod) {
            // far away only the big cards that span the tree's height are kept
            std::vector<int> tris;
            for (int q = 0; q + 2 < (int)m.p.size(); q += 3) {
                float lo = std::min({m.p[q].y, m.p[q + 1].y, m.p[q + 2].y}), hi = std::max({m.p[q].y, m.p[q + 1].y, m.p[q + 2].y});
                if (lod == 0 || hi - lo > 0.5f * m.height) tris.push_back(q);
            }
            if (lod == 1 && tris.size() < 2) {
                tt.lod[1] = tt.lod[0];
                continue;
            }
            gfx::MeshBuilder mb;
            for (int q : tris) {
                int first = -1;
                for (int c = 0; c < 3; ++c) {
                    Vertex vx{};
                    vx.p = m.p[q + c];
                    vx.n = m.n[q + c];
                    vx.uv = m.uv[q + c];
                    float up = std::sqrt(std::clamp(vx.p.y / m.height, 0.0f, 1.0f));
                    vx.c[0] = vx.c[1] = vx.c[2] = 255;
                    vx.c[3] = (unsigned char)((aoFloor + (1 - aoFloor) * up) * 255);
                    int id = mb.add(vx);
                    if (first < 0) first = id;
                }
                mb.tri(first, first + 1, first + 2);
            }
            std::vector<Mesh> built = mb.build();
            tt.lod[lod] = built.empty() ? Mesh{} : built[0];
        }
    }
    // tiles: instance matrices (yaw, uniform scale, position; the tint in the bottom row)
    for (auto& [key, list] : tiles) {
        TreeTile tile;
        tile.inst.assign(meshes.size(), {});
        tile.box = {{1e30f, 1e30f, 1e30f}, {-1e30f, -1e30f, -1e30f}};
        for (const Inst& in : list) {
            float c = std::cos(in.yaw) * in.scale, s = std::sin(in.yaw) * in.scale;
            Matrix M{};
            M.m0 = c; M.m1 = 0; M.m2 = -s; M.m3 = in.tint.x;
            M.m4 = 0; M.m5 = in.scale; M.m6 = 0; M.m7 = in.tint.y;
            M.m8 = s; M.m9 = 0; M.m10 = c; M.m11 = in.tint.z;
            M.m12 = in.base.x; M.m13 = in.base.y; M.m14 = in.base.z; M.m15 = 1;
            // raylib's Matrix is stored row by row; the shader reads columns
            tile.inst[in.m].push_back(MatrixTranspose(M));
            treeTypes_[in.m].total++;
            float h = meshes[in.m].height * in.scale, r = 0.5f * h;
            tile.box.min = Vector3Min(tile.box.min, {in.base.x - r, in.base.y, in.base.z - r});
            tile.box.max = Vector3Max(tile.box.max, {in.base.x + r, in.base.y + h, in.base.z + r});
        }
        treeTiles_.push_back(std::move(tile));
    }
    for (TreeType& tt : treeTypes_)
        for (auto& pass : tt.vbo)
            for (unsigned& v : pass) v = tt.total ? rlLoadVertexBuffer(nullptr, tt.total * (int)sizeof(Matrix), true) : 0;
    treeMat_.layer[0] = &foliage_;
    treeMat_.alphaCut = 0.45f;
    treeMat_.doubleSided = true;
    treeMat_.translucency = 0.7f;
    treeMat_.vertexTint = true;
    treeMat_.roughness = 1.0f;
    std::printf("trees: %d\n", count);
}

// ---------------------------------------------------------------- paddock and trackside

bool TrackScene::garagesAt(float s) const {
    const rr::Track& tr = *tr_;
    return tr.hasPit() && tr.inSpan(s, tr.pit().lane_start_s + 10.0f, tr.pit().lane_end_s - 10.0f);
}

bool TrackScene::grandstandAt(float s) const {
    const rr::Track& tr = *tr_;
    return tr.hasPit() && tr.inSpan(s, tr.pit().lane_start_s + 30.0f, tr.pit().lane_start_s + 200.0f);
}

float TrackScene::flatUntil(float s, int side, float hw) const {
    const rr::Track& tr = *tr_;
    if (tr.hasPit() && side == tr.pit().side && tr.inPitArea(s)) return hw + rr::Track::kPitBarrier + 45.0f;
    if (tr.hasPit() && side != tr.pit().side && grandstandAt(s)) return hw + tr.runoff() + 24.0f;
    return tr.barrierOffset(s, side, hw);
}

namespace {

// A six-sided block from its eight corners (bottom four, then the top four, in the same order),
// faces turned outwards; the bottom face only when asked (overhangs). ao darkens the lower edge.
void block(gfx::MeshBuilder& mb, const Vector3 c[8], bool bottom = false, float aoLow = 1.0f) {
    Vector3 mid = {0, 0, 0};
    for (int k = 0; k < 8; ++k) mid = Vector3Add(mid, Vector3Scale(c[k], 0.125f));
    auto face = [&](Vector3 a, Vector3 b, Vector3 cc, Vector3 d, float aoA, float aoB, float aoC, float aoD) {
        Vector3 n = Vector3CrossProduct(Vector3Subtract(b, a), Vector3Subtract(cc, a));
        Vector3 centre = Vector3Scale(Vector3Add(Vector3Add(a, b), Vector3Add(cc, d)), 0.25f);
        if (Vector3DotProduct(n, Vector3Subtract(centre, mid)) < 0) {
            std::swap(b, d);
            std::swap(aoB, aoD);
        }
        float lu = Vector3Distance(a, b), lv = Vector3Distance(a, d);
        mb.reserve(4);
        Vector3 nn = Vector3Normalize(Vector3CrossProduct(Vector3Subtract(b, a), Vector3Subtract(cc, a)));
        Vertex va = vert(a, nn, {0, 0}), vb = vert(b, nn, {lu, 0}), vc = vert(cc, nn, {lu, lv}), vd = vert(d, nn, {0, lv});
        va.c[3] = (unsigned char)(aoA * 255); vb.c[3] = (unsigned char)(aoB * 255);
        vc.c[3] = (unsigned char)(aoC * 255); vd.c[3] = (unsigned char)(aoD * 255);
        for (Vertex* v : {&va, &vb, &vc, &vd}) v->c[0] = v->c[1] = v->c[2] = 255;
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

// A vertical cylinder (tyre stacks): n sides, no caps but the top.
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
        // outward faces: counter-clockwise seen from outside
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

}  // namespace

void TrackScene::buildProps(const std::string& assetsDir) {
    const rr::Track& tr = *tr_;
    const float L = tr.length();
    // a point beside the track: s along it, lateral offset, absolute height
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
    gfx::MeshBuilder white, glass, steel, seats, slab, tyres, fence, orange;

    if (tr.hasPit()) {
        const int ps = tr.pit().side;
        const float hw = tr.at(tr.indexAt(tr.pit().lane_start_s)).halfWidth;
        const float F = ps * (hw + rr::Track::kPitBarrier + 0.3f);  // garage fronts
        const float D = ps * 14.0f;                                 // depth
        float s0 = tr.pit().lane_start_s + 10.0f, s1 = tr.pit().lane_end_s - 10.0f;
        float len = std::fmod(s1 - s0 + L, L);
        int bays = std::max(1, (int)(len / 8.0f));
        float bay = len / bays;
        // The pit straight slopes: each bay sits on its own ground (the lowest corner of
        // its footprint), so the building steps down the hill with deep foundations.
        auto bayGround = [&](float a, float e) {
            return std::max({ground(a, F), ground(e, F), ground(a, F + D), ground(e, F + D)}) + 0.04f;
        };
        for (int b = 0; b < bays; ++b) {
            float a = s0 + b * bay, e = a + bay;
            const float g = bayGround(a, e);
            // pillar and side wall between garages, the back wall, the floor
            blk(white, a, a + 0.6f, F, F + D, g - 4.0f, g + 4.4f, false, 0.6f);
            blk(white, a, e, F + D * 0.93f, F + D, g - 4.0f, g + 4.4f, false, 0.5f);
            blk(white, a, e, F, F + D, g - 4.0f, g - 0.3f, false, 0.6f);  // foundation, under the floor
            // the garage floor (inside, dark)
            blk(slab, a + 0.6f, e, F, F + D * 0.93f, g - 0.3f, g + 0.01f, false, 0.4f);
            // half-open roller door
            blk(steel, a + 0.6f, e, F + ps * 0.25f, F + ps * 0.35f, g + 2.9f, g + 4.4f);
            // upper floor: glass front, solid block behind, roof slab overhanging the pit lane
            blk(glass, a, e, F, F + ps * 0.25f, g + 4.4f, g + 7.4f);
            blk(white, a, e, F + ps * 0.25f, F + D, g + 4.4f, g + 7.4f);
            blk(white, a, e, F - ps * 2.2f, F + D + ps * 0.3f, g + 7.4f, g + 7.9f, true, 0.8f);
            // a team colour band along the fascia
            blk(orange, a, e, F - ps * 2.25f, F - ps * 2.2f, g + 7.45f, g + 7.85f);
        }
        // the closed ends of the building
        {
            const float g = bayGround(s1 - bay, s1);
            blk(white, s1, s1 + 0.6f, F, F + D, g - 4.0f, g + 7.4f, false, 0.6f);
            const float g0 = bayGround(s0, s0 + bay);
            blk(white, s0 - 0.6f, s0, F, F + D, g0 - 4.0f, g0 + 7.4f, false, 0.6f);
        }
        // the paddock behind: worn asphalt out to where the land begins
        {
            Strip st{&slab};
            for (int i = 0; i <= tr.size(); ++i) {
                float s = i >= tr.size() ? L : tr.at(i).s;
                if (!tr.inPitArea(s)) { st.cut(); continue; }
                const auto& sm = tr.at(i);
                float in = ps * (sm.halfWidth + rr::Track::kPitBarrier + (garagesAt(s) ? 0.3f + 14.0f : 0.3f));
                float out = ps * (sm.halfWidth + rr::Track::kPitBarrier + 45.0f);
                Vertex v0 = vert(W(sm.p + sm.n * in, ground(s, in)), {0, 1, 0}, {in, s});
                Vertex v1 = vert(W(sm.p + sm.n * out, ground(s, out)), {0, 1, 0}, {out, s});
                for (Vertex* v : {&v0, &v1}) v->c[0] = v->c[1] = v->c[2] = 255;
                std::vector<Vertex> r = {v0, v1};
                if (ps < 0) std::swap(r[0], r[1]);
                st.ring(r);
            }
        }

        // grandstand across the pit straight: stepped concrete, coloured seats, a cantilever roof
        {
            const int os = -ps;
            const float L0 = hw + tr.runoff() + 2.0f;
            float a0 = tr.pit().lane_start_s + 40.0f, a1 = tr.pit().lane_start_s + 190.0f;
            const int rows = 16;
            const float depth = 0.9f, rise = 0.48f, base = 1.4f;
            for (float a = a0; a < a1 - 0.1f; a += 10.0f) {
                float e = std::min(a + 10.0f, a1);
                int sec = (int)((a - a0) / 10.0f);
                // each 10 m section on its own ground, stepping with the slope
                const float g0 = std::min({ground(a, os * L0), ground(e, os * L0), ground(a, os * (L0 + rows * depth)),
                                           ground(e, os * (L0 + rows * depth))}) + 0.02f;
                for (int r = 0; r < rows; ++r) {
                    float l0 = os * (L0 + r * depth), l1 = os * (L0 + (r + 1) * depth);
                    float top = g0 + base + r * rise;
                    blk(white, a, e, l0, l1, g0 - 3.0f, top, false, 0.7f);
                    // seats: blocks of colour, a lettered pattern across sections
                    blk(seats, a + 0.2f, e - 0.2f, os * (L0 + r * depth + 0.25f), os * (L0 + r * depth + 0.7f), top, top + 0.42f);
                    (void)sec;
                }
                float back = os * (L0 + rows * depth);
                float topRow = g0 + base + rows * rise;
                blk(white, a, e, back, back + os * 0.4f, g0 - 3.0f, topRow + 3.2f, false, 0.7f);  // back wall
                blk(steel, a, a + 0.4f, back - os * 0.2f, back + os * 0.2f, topRow, topRow + 3.6f);  // roof column
                // the roof slopes up towards the track
                Vector3 c[8] = {TP(a, back + os * 0.4f, topRow + 3.6f), TP(e, back + os * 0.4f, topRow + 3.6f),
                                TP(e, os * (L0 - 1.5f), topRow + 4.6f), TP(a, os * (L0 - 1.5f), topRow + 4.6f),
                                TP(a, back + os * 0.4f, topRow + 3.85f), TP(e, back + os * 0.4f, topRow + 3.85f),
                                TP(e, os * (L0 - 1.5f), topRow + 4.85f), TP(a, os * (L0 - 1.5f), topRow + 4.85f)};
                block(steel, c, true, 0.7f);
            }
            for (float end : {a0 - 0.4f, a1}) {
                const float g0 = std::min(ground(end, os * L0), ground(end, os * (L0 + rows * depth))) + 0.02f;
                blk(white, end, end + 0.4f, os * L0, os * (L0 + rows * depth + 0.4f), g0 - 3.0f, g0 + base + rows * rise + 3.2f);
            }
        }

        // catch fences on the concrete walls of the pit straight
        for (int side : {1, -1}) {
            if (side == ps) continue;  // the garages face the pit lane
            float a0 = tr.pit().entry_s, a1 = tr.pit().exit_s;
            float span = std::fmod(a1 - a0 + L, L);
            for (float d = 0; d < span; d += 3.0f) {
                float s = a0 + d, e = a0 + std::min(d + 3.0f, span);
                if (!tr.inPitArea(s) || !tr.inPitArea(e)) continue;  // on the concrete only
                const auto& sm = tr.at(tr.indexAt(s));
                float off = side * (tr.barrierOffset(s, side, sm.halfWidth) + 0.29f);
                float h0 = ground(s, off) + 0.95f, h1 = ground(e, off) + 0.95f;
                // the mesh, both sides
                Vector3 p0 = TP(s, off, h0), p1 = TP(e, off, h1), p2 = TP(e, off, h1 + 3.0f), p3 = TP(s, off, h0 + 3.0f);
                quad(fence, p0, p1, p2, p3, {s, 0}, {e, 0}, {e, 3.0f}, {s, 3.0f});
                // a post
                blk(steel, s, s + 0.08f, off - 0.04f, off + 0.04f, h0 - 0.3f, h0 + 3.1f);
            }
        }
    }

    // marshal posts outside every corner, tyre walls in front of the armco at the slow ones
    for (const auto& turn : tr.turns()) {
        int out = -turn.direction;
        float s = turn.apex_s;
        const auto& sm = tr.at(tr.indexAt(s));
        float off = tr.barrierOffset(s, out, sm.halfWidth);
        float lat = out * (off + 3.5f);
        float g = ground(s, lat) + 0.02f;
        blk(white, s - 1.1f, s + 1.1f, lat - 1.1f, lat + 1.1f, g - 0.5f, g + 2.4f, false, 0.6f);
        blk(orange, s - 1.4f, s + 1.4f, lat - 1.4f, lat + 1.4f, g + 2.4f, g + 2.65f, true);
        if (turn.min_radius < 45.0f) {
            for (float d = -25.0f; d <= 45.0f; d += 0.62f) {
                float st = turn.apex_s + d;
                const auto& a = tr.at(tr.indexAt(st));
                float lo = out * (tr.barrierOffset(st, out, a.halfWidth) - 0.4f);
                Vector3 b = TP(st, lo, ground(st, lo) - 0.05f);
                int stack = (int)std::floor((d + 25.0f) / 0.62f);
                unsigned char tint = (stack / 4) % 2 ? 255 : 40;  // white-banded every fourth stack
                cylinder(tyres, b, 0.3f, 0.95f, 10, tint);
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
    gfx::Material mWhite;
    mWhite.layer[0] = &concrete_;
    mWhite.scale[0] = 3.0f;
    mWhite.tint = {1.05f, 1.05f, 1.05f};
    mWhite.normalStrength = 0.5f;
    part(white, mWhite, true);
    gfx::Material mSlab;
    mSlab.layer[0] = &asphaltWorn_;
    mSlab.scale[0] = 4.0f;
    mSlab.depthBias = 1.0f;
    mSlab.detile = true;
    part(slab, mSlab, false);
    gfx::Material mGlass;
    mGlass.layer[0] = &metal_;
    mGlass.scale[0] = 6.0f;
    mGlass.tint = {0.25f, 0.32f, 0.38f};
    mGlass.roughness = 0.08f;
    mGlass.normalStrength = 0.0f;
    part(glass, mGlass, true);
    gfx::Material mSteel;
    mSteel.layer[0] = &metal_;
    mSteel.scale[0] = 2.0f;
    mSteel.tint = {0.7f, 0.72f, 0.75f};
    mSteel.roughness = 0.6f;
    part(steel, mSteel, true);
    gfx::Material mOrange = mWhite;
    mOrange.tint = {1.0f, 0.38f, 0.05f};
    mOrange.roughness = 0.6f;
    part(orange, mOrange, true);
    gfx::Material mSeats;
    mSeats.layer[0] = &concrete_;
    mSeats.scale[0] = 1.0f;
    mSeats.tint = {0.08f, 0.22f, 0.6f};  // RR blue
    mSeats.roughness = 0.45f;
    mSeats.normalStrength = 0.2f;
    part(seats, mSeats, true);
    gfx::Material mTyres;
    mTyres.layer[0] = &rubber_;
    mTyres.scale[0] = 0.6f;
    mTyres.vertexTint = true;
    mTyres.tint = {0.35f, 0.35f, 0.35f};
    part(tyres, mTyres, true);
    if (fence.vertexCount() > 0) {
        Texture2D ct = LoadTexture((assetsDir + "/materials/chainlink/albedo.png").c_str());
        if (ct.id) {
            GenTextureMipmaps(&ct);
            SetTextureFilter(ct, TEXTURE_FILTER_TRILINEAR);
            SetTextureWrap(ct, TEXTURE_WRAP_REPEAT);
            chain_ = gfx::flatTextureSet(ct, 0.5f);
            gfx::Material mFence;
            mFence.layer[0] = &chain_;
            mFence.scale[0] = 1.0f;
            mFence.alphaCut = 0.35f;
            mFence.doubleSided = true;
            mFence.metalness = 0.0f;
            mFence.tint = {1.8f, 1.8f, 1.85f};
            part(fence, mFence, true, ct.id);
        }
    }
}

// ---------------------------------------------------------------- draw

namespace {
float boxDistance(const BoundingBox& b, Vector3 p) {
    float dx = std::max({b.min.x - p.x, 0.0f, p.x - b.max.x});
    float dz = std::max({b.min.z - p.z, 0.0f, p.z - b.max.z});
    return std::sqrt(dx * dx + dz * dz);
}
}  // namespace

const Mesh& TrackScene::pick(const Part& p, size_t i, Vector3 cam, bool coarse) const {
    if (p.lod.empty()) return p.meshes[i];
    return (coarse || boxDistance(p.boxes[i], cam) > p.lodDist) ? p.lod[i] : p.meshes[i];
}

void TrackScene::drawShadows(gfx::Renderer& r) const {
    const Vector3 cam = r.cameraPosition();
    for (const Part& p : parts_) {
        if (!p.shadows) continue;
        for (size_t i = 0; i < p.meshes.size(); ++i)
            if (r.inShadowView(p.boxes[i]))
                r.drawShadow(pick(p, i, cam, r.cascade() == 1), MatrixIdentity(), p.alphaTex, p.mat.alphaCut, p.mat.scale[0]);
    }
    drawTrees(r, 1 + r.cascade());
}

void TrackScene::draw(gfx::Renderer& r) const {
    const Vector3 cam = r.cameraPosition();
    for (const Part& p : parts_)
        for (size_t i = 0; i < p.meshes.size(); ++i)
            if (r.inView(p.boxes[i])) r.draw(pick(p, i, cam, false), p.mat, MatrixIdentity());
    drawTrees(r, 0);
}

// pass 0: the camera's view; 1: the near shadow cascade; 2: the far one.
void TrackScene::drawTrees(gfx::Renderer& r, int pass) const {
    const Vector3 cam = r.cameraPosition();
    const float kFull = pass == 0 ? 220.0f : 260.0f, kFar = 3200.0f, kBush = 500.0f;
    for (size_t k = 0; k < treeTypes_.size(); ++k) {
        const TreeType& tt = treeTypes_[k];
        if (!tt.total) continue;
        gather_[0].clear();
        gather_[1].clear();
        for (const TreeTile& tile : treeTiles_) {
            const auto& list = tile.inst[k];
            if (list.empty()) continue;
            if (pass == 0 ? !r.inView(tile.box) : !r.inShadowView(tile.box)) continue;
            // distance from the camera to the tile (0 inside it)
            float dx = std::max({tile.box.min.x - cam.x, 0.0f, cam.x - tile.box.max.x});
            float dz = std::max({tile.box.min.z - cam.z, 0.0f, cam.z - tile.box.max.z});
            float d = std::sqrt(dx * dx + dz * dz);
            int lod = (pass == 2 || d > kFull) ? 1 : 0;
            if (tt.bush && (d > kBush || pass == 2)) continue;
            if (d > kFar) continue;
            gather_[lod].insert(gather_[lod].end(), list.begin(), list.end());
        }
        for (int lod = 0; lod < 2; ++lod) {
            const int n = (int)gather_[lod].size();
            if (!n) continue;
            rlUpdateVertexBuffer(tt.vbo[pass][lod], gather_[lod].data(), n * (int)sizeof(Matrix), 0);
            if (pass == 0) r.drawInstanced(tt.lod[lod], treeMat_, tt.vbo[pass][lod], n);
            else r.drawShadowInstanced(tt.lod[lod], tt.vbo[pass][lod], n, treeAtlas_.id, treeMat_.alphaCut);
        }
    }
}
