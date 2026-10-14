#include "decals.hpp"

#include <algorithm>
#include <cmath>

#include "raymath.h"

namespace {
using gfx::Vertex;

Vertex vert(Vector3 p, Vector2 uv, Color c) {
    Vertex v{};
    v.p = p;
    v.n = {0, 1, 0};
    v.t = {1, 0, 0, 1};
    v.uv = uv;
    v.uv2 = uv;
    v.c[0] = c.r; v.c[1] = c.g; v.c[2] = c.b; v.c[3] = c.a;
    return v;
}

void quad(gfx::MeshBuilder& mb, Vector3 a, Vector3 b, Vector3 c, Vector3 d, Color col) {
    mb.reserve(4);
    const int i0 = mb.add(vert(a, {0, 0}, col)), i1 = mb.add(vert(b, {1, 0}, col));
    const int i2 = mb.add(vert(c, {1, 1}, col)), i3 = mb.add(vert(d, {0, 1}, col));
    mb.tri(i0, i1, i2);
    mb.tri(i0, i2, i3);
    mb.tri(i0, i2, i1);  // both faces: the decals are seen from above whatever the winding
    mb.tri(i0, i3, i2);
}

constexpr int kRubberCols = 64;  // columns per rubber mesh (256 m)
}  // namespace

void Decals::release() {
    if (!ready_) return;
    ready_ = false;
    unload(skids_);
    unload(tracks_);
    for (Chunk& c : rubber_) UnloadMesh(c.mesh);
    gfx::unloadTextureSet(flat_);
    gfx::unloadTextureSet(puff_);
    rubber_.clear();
    rubberVer_ = skidVer_ = ~0u;
}

void Decals::unload(std::vector<Mesh>& m) {
    for (Mesh& x : m) UnloadMesh(x);
    m.clear();
}

void Decals::init() {
    ready_ = true;
    Image w = GenImageColor(1, 1, WHITE);
    flat_ = gfx::flatTextureSet(LoadTextureFromImage(w), 0.9f);
    UnloadImage(w);
    // a soft round puff: opaque in the middle, fading to nothing at the edge
    Image p = GenImageColor(64, 64, WHITE);
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x) {
            const float d = std::min(1.0f, std::hypot((x - 31.5f) / 32.0f, (y - 31.5f) / 32.0f));
            const float a = (1 - d) * (1 - d) * (3 - 2 * (1 - d));
            ImageDrawPixel(&p, x, y, Color{255, 255, 255, (unsigned char)(255 * a)});
        }
    puff_ = gfx::flatTextureSet(LoadTextureFromImage(p), 1.0f);
    UnloadImage(p);
    for (gfx::Material* m : {&matRubber_, &matMarks_, &matTracks_, &matDust_}) {
        m->layer[0] = &flat_;
        m->vertexTint = true;
        m->decal = true;
        m->doubleSided = true;
        m->roughness = 1.0f;
        m->metalness = 0.0f;
        m->normalStrength = 0.0f;
    }
    matRubber_.depthBias = 4.0f;
    matMarks_.depthBias = 6.0f;
    matTracks_.depthBias = 8.0f;
    matDust_.layer[0] = &puff_;
    matDust_.depthBias = 0.0f;
}

void Decals::buildMarks(const std::vector<Effects::Mark>& marks, std::vector<Mesh>& out) {
    unload(out);
    gfx::MeshBuilder mb;
    for (const Effects::Mark& m : marks) {
        const Vector3 d = Vector3Subtract(m.b, m.a);
        const float len = std::sqrt(d.x * d.x + d.z * d.z);
        if (len < 1e-3f) continue;
        const Vector3 n = {-d.z / len * m.width * 0.5f, 0, d.x / len * m.width * 0.5f};
        const Color c = {m.col.r, m.col.g, m.col.b, m.alpha};
        quad(mb, Vector3Add(m.a, n), Vector3Add(m.b, n), Vector3Subtract(m.b, n), Vector3Subtract(m.a, n), c);
    }
    out = mb.build();
}

void Decals::buildRubber(const rr::Race& race, const rr::TrackRubber& rub) {
    for (Chunk& c : rubber_) UnloadMesh(c.mesh);
    rubber_.clear();
    const rr::Track& tr = race.track();
    const float cs = rr::TrackRubber::kCellS, cl = rr::TrackRubber::kCellL;
    auto pt = [&](float s, float l) {
        const rr::Vec2 p = tr.pointAt(s, l);
        return Vector3{p.x, tr.heightAt(s, l) + 0.004f, -p.y};
    };
    for (int c0 = 0; c0 < rub.cols(); c0 += kRubberCols) {
        gfx::MeshBuilder mb;
        Vector3 lo{1e9f, 1e9f, 1e9f}, hi{-1e9f, -1e9f, -1e9f};
        for (int col = c0; col < std::min(rub.cols(), c0 + kRubberCols); ++col)
            for (int lane = 0; lane < rr::TrackRubber::kLanes; ++lane) {
                const float r = rub.cell(col, lane);
                if (r < 0.03f) continue;
                const float s0 = col * cs, s1 = s0 + cs;
                const float l0 = (lane - rr::TrackRubber::kLanes / 2) * cl, l1 = l0 + cl;
                const Vector3 a = pt(s0, l0), b = pt(s1, l0), c = pt(s1, l1), d = pt(s0, l1);
                quad(mb, a, b, c, d, Color{14, 14, 16, (unsigned char)(140 * std::pow(r, 0.6f))});
                for (const Vector3& v : {a, c}) {
                    lo = Vector3Min(lo, v);
                    hi = Vector3Max(hi, v);
                }
            }
        for (Mesh& m : mb.build()) rubber_.push_back({m, {Vector3Subtract(lo, {2, 2, 2}), Vector3Add(hi, {2, 2, 2})}});
    }
}

void Decals::draw(gfx::Renderer& gr, const Effects& fx, const rr::Race& race, Vector3 camPos) {
    if (fx.level() <= 0) return;
    if (!ready_) init();
    if (race_ != &race) {
        race_ = &race;
        rubberVer_ = skidVer_ = ~0u;
    }
    const Matrix id = MatrixIdentity();

    // the rubber on the racing line: the sim's own map, rebuilt a couple of times a second while cars lay it
    if (const rr::TrackRubber* rub = race.rubber()) {
        if ((rub->version() != rubberVer_ && ++rubberAge_ >= 20) || rubber_.empty()) {
            rubberVer_ = rub->version();
            rubberAge_ = 0;
            buildRubber(race, *rub);
        }
        for (const Chunk& c : rubber_)
            if (gr.inView(c.box)) gr.draw(c.mesh, matRubber_, id);
    }
    // skid marks and tyre tracks: rebuilt when one was added (at most a few times a second)
    if (fx.marksVersion() != skidVer_ && ++skidAge_ >= 6) {
        skidVer_ = fx.marksVersion();
        skidAge_ = 0;
        buildMarks(fx.tyreTracks(), tracks_);
        buildMarks(fx.skidMarks(), skids_);
    }
    for (const Mesh& m : tracks_) gr.draw(m, matTracks_, id);
    for (const Mesh& m : skids_) gr.draw(m, matMarks_, id);

    // dust, clods and sparks: camera-facing puffs near the camera
    if (fx.level() >= 2 && !fx.particles().empty()) {
        const Vector3 up = {0, 1, 0};
        const Vector3 f = Vector3Normalize(Vector3Subtract(gr.cameraTarget(), camPos));
        Vector3 right = Vector3CrossProduct(f, up);
        if (Vector3Length(right) < 1e-3f) right = {1, 0, 0};
        right = Vector3Normalize(right);
        const Vector3 upv = Vector3CrossProduct(right, f);
        gfx::MeshBuilder mb;
        int n = 0;
        for (const Effects::Particle& q : fx.particles()) {
            if (Vector3Distance(q.p, camPos) > 200 || n >= 700) continue;
            const float t = q.age / q.life;
            const float size = q.kind == 2 ? q.size * 3.0f : q.size;
            Color c = q.col;
            c.a = (unsigned char)(c.a * (1.0f - t));
            const Vector3 r = Vector3Scale(right, size), u = Vector3Scale(upv, size);
            quad(mb, Vector3Subtract(Vector3Subtract(q.p, r), u), Vector3Subtract(Vector3Add(q.p, r), u),
                 Vector3Add(Vector3Add(q.p, r), u), Vector3Add(Vector3Subtract(q.p, r), u), c);
            ++n;
        }
        std::vector<Mesh> meshes = mb.build();
        for (Mesh& m : meshes) {
            gr.draw(m, matDust_, id);
            UnloadMesh(m);
        }
    }
}
