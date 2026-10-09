#include "car_render.hpp"

#include <algorithm>
#include <cmath>

#include "mini_json.hpp"
#include "raymath.h"
#include "track_scene.hpp"

namespace {

Vector3 W2(rr::Vec2 v) { return {v.x, 0, -v.y}; }

// A basis (columns: x, y, z, origin) as a raylib matrix (which keeps them row by row).
Matrix basis(Vector3 x, Vector3 y, Vector3 z, Vector3 o) {
    Matrix m{};
    m.m0 = x.x; m.m1 = x.y; m.m2 = x.z;
    m.m4 = y.x; m.m5 = y.y; m.m6 = y.z;
    m.m8 = z.x; m.m9 = z.y; m.m10 = z.z;
    m.m12 = o.x; m.m13 = o.y; m.m14 = o.z;
    m.m15 = 1;
    return m;
}

}  // namespace

bool CarRender::loadPart(const std::string& file, const Finish& paint, int liveryMaterial, std::vector<Part>* out, bool wheel,
                         float wheelRadius, std::string* err, std::vector<Part>* steerOut, int steerMaterial) {
    Model m = LoadModel(file.c_str());
    if (m.meshCount == 0) {
        if (err) *err = "cannot load " + file;
        return false;
    }
    models_.push_back(m);
    for (int i = 0; i < m.meshCount; ++i) {
        const Mesh& src = m.meshes[i];
        const int mi = m.meshMaterial[i];
        Texture2D tex = m.materials[mi].maps[MATERIAL_MAP_ALBEDO].texture;
        if (mi == liveryMaterial && !liveryFile_.empty()) {
            // a repaint (livery.png next to car.json) in place of the embedded paint, shared by the body and the flap
            if (!liveryTex_.id) {
                Image img = LoadImage(liveryFile_.c_str());
                if (img.data) {
                    ImageFormat(&img, PIXELFORMAT_UNCOMPRESSED_R8G8B8);  // paint is opaque
                    liveryTex_ = LoadTextureFromImage(img);
                    GenTextureMipmaps(&liveryTex_);
                    SetTextureFilter(liveryTex_, TEXTURE_FILTER_ANISOTROPIC_16X);
                }
                UnloadImage(img);
            }
            if (liveryTex_.id) tex = liveryTex_;
        } else if (tex.id) {
            GenTextureMipmaps(&tex);
            SetTextureFilter(tex, TEXTURE_FILTER_ANISOTROPIC_16X);
        }
        // the wheels split into tyre (outer) and rim (inner) by distance from the axle
        gfx::MeshBuilder mb[2];
        const int tris = src.indices ? src.triangleCount : src.vertexCount / 3;
        for (int t = 0; t < tris; ++t) {
            int id[3];
            for (int k = 0; k < 3; ++k) id[k] = src.indices ? src.indices[t * 3 + k] : t * 3 + k;
            int which = 0;
            if (wheel) {
                float r = 0;
                for (int k : id) r += std::hypot(src.vertices[k * 3 + 1], src.vertices[k * 3 + 2]) / 3;
                which = r > 0.74f * wheelRadius ? 0 : 1;
            }
            gfx::MeshBuilder& b = mb[which];
            b.reserve(3);
            int first = -1;
            for (int k : id) {
                gfx::Vertex v{};
                v.p = {src.vertices[k * 3], src.vertices[k * 3 + 1], src.vertices[k * 3 + 2]};
                v.n = src.normals ? Vector3{src.normals[k * 3], src.normals[k * 3 + 1], src.normals[k * 3 + 2]} : Vector3{0, 1, 0};
                v.uv = src.texcoords ? Vector2{src.texcoords[k * 2], src.texcoords[k * 2 + 1]} : Vector2{0, 0};
                v.c[0] = v.c[1] = v.c[2] = v.c[3] = 255;
                int n = b.add(v);
                if (first < 0) first = n;
            }
            b.tri(first, first + 1, first + 2);
        }
        for (int which = 0; which < 2; ++which) {
            if (!wheel && which == 1) break;
            // the finish of this part
            float rough = 0.35f, metal = 0.1f, cc = 0.6f, ccRough = 0.05f;
            if (wheel) {
                if (which == 0) rough = 0.82f, metal = 0.0f, cc = 0.0f;   // rubber
                else rough = 0.28f, metal = 0.85f, cc = 0.0f;             // magnesium rims
            } else if (mi == liveryMaterial) {
                rough = paint.roughness, metal = paint.metalness, cc = paint.clearcoat, ccRough = paint.clearcoatRoughness;
            }
            auto* set = new gfx::TextureSet(gfx::flatTextureSet(tex, rough, metal));
            sets_.push_back(set);
            for (Mesh& mesh : mb[which].build()) {
                Part p;
                p.mesh = mesh;
                p.set = set;
                p.mat.layer[0] = set;
                p.mat.clearcoat = cc;
                p.mat.clearcoatRoughness = ccRough;
                p.mat.normalStrength = 0.0f;
                (steerOut && mi == steerMaterial ? steerOut : out)->push_back(p);
            }
        }
    }
    return true;
}

bool CarRender::load(const std::string& dir, const Finish& paint, std::string* err) {
    char* text = LoadFileText((dir + "/car.json").c_str());
    if (!text) {
        if (err) *err = "cannot read " + dir + "/car.json";
        return false;
    }
    mjson::Value j = mjson::parse(text);
    UnloadFileText(text);
    const char* names[4] = {"FL", "FR", "RL", "RR"};
    for (int i = 0; i < 4; ++i) {
        const mjson::Value& w = j["wheels"][names[i]];
        wheels_[i].hub = {(float)w["hub"][0].num(), (float)w["hub"][1].num(), (float)w["hub"][2].num()};
        wheels_[i].radius = (float)w["radius"].num(0.36);
        wheels_[i].front = i < 2;
        wheels_[i].mirrored = w["mirrored"].type == mjson::Value::Bool && w["mirrored"].b;
    }
    wheelbase_ = wheels_[0].hub.z - wheels_[2].hub.z;
    track_ = wheels_[0].hub.x - wheels_[1].hub.x;
    // the livery: the source material named in car.json, after raylib's default material 0
    int livery = -1;
    const mjson::Value& mats = j["materials"];
    for (size_t k = 0; k < mats.size(); ++k)
        if (mats[k].str() == j["source_material"].str()) livery = (int)k + 1;
    // the steering wheel is the body's "Wheel1" material (the cockpit's own texture)
    int steerMat = -1;
    for (size_t k = 0; k < mats.size(); ++k)
        if (mats[k].str().rfind("Wheel1Mtl", 0) == 0) steerMat = (int)k + 1;
    liveryFile_ = FileExists((dir + "/livery.png").c_str()) ? dir + "/livery.png" : std::string();
    if (!loadPart(dir + "/body.glb", paint, livery, &body_parts_, false, 0, err, &steer_parts_, steerMat) ||
        !loadPart(dir + "/wheel_front.glb", paint, -1, &wheel_parts_[0], true, wheels_[0].radius, err) ||
        !loadPart(dir + "/wheel_rear.glb", paint, -1, &wheel_parts_[1], true, wheels_[2].radius, err))
        return false;
    // the DRS flap: its own file, whose node sits at the hinge (raylib bakes that into the
    // vertices, so the flap loads closed, in the body's frame)
    const mjson::Value& drs = j["drs"];
    if (drs.type == mjson::Value::Object) {
        drsPivot_ = {(float)drs["pivot"][0].num(), (float)drs["pivot"][1].num(), (float)drs["pivot"][2].num()};
        drsAxis_ = {(float)drs["axis"][0].num(1), (float)drs["axis"][1].num(), (float)drs["axis"][2].num()};
        drsMax_ = (float)drs["max_angle_deg"].num() * DEG2RAD;
        int drsLivery = -1;
        for (size_t k = 0; k < drs["materials"].size(); ++k)
            if (drs["materials"][k].str() == j["source_material"].str()) drsLivery = (int)k + 1;
        if (!loadPart(dir + "/" + drs["file"].str(), paint, drsLivery, &drs_parts_, false, 0, err)) return false;
    }
    return true;
}

void CarRender::shareFrom(const CarRender& src) {
    body_parts_ = src.body_parts_;
    steer_parts_ = src.steer_parts_;
    drs_parts_ = src.drs_parts_;
    drsPivot_ = src.drsPivot_;
    drsAxis_ = src.drsAxis_;
    drsMax_ = src.drsMax_;
    wheel_parts_[0] = src.wheel_parts_[0];
    wheel_parts_[1] = src.wheel_parts_[1];
    for (int i = 0; i < 4; ++i) wheels_[i] = src.wheels_[i];
    wheelbase_ = src.wheelbase_;
    track_ = src.track_;
    steerHub_ = src.steerHub_;
    steerAxis_ = src.steerAxis_;
    owner_ = false;
}

void CarRender::unload() {
    if (!owner_) {
        body_parts_.clear();
        steer_parts_.clear();
        drs_parts_.clear();
        wheel_parts_[0].clear();
        wheel_parts_[1].clear();
        return;
    }
    for (auto* list : {&body_parts_, &steer_parts_, &drs_parts_, &wheel_parts_[0], &wheel_parts_[1]}) {
        for (Part& p : *list) UnloadMesh(p.mesh);
        list->clear();
    }
    if (liveryTex_.id) UnloadTexture(liveryTex_);
    liveryTex_ = {};
    for (gfx::TextureSet* s : sets_) {
        UnloadTexture(s->normal);
        UnloadTexture(s->orm);
        delete s;
    }
    sets_.clear();
    for (Model& m : models_) UnloadModel(m);
    models_.clear();
}

void CarRender::update(const rr::Car& car, const rr::Track& track, const TrackScene& scene, float dt) {
    const rr::CarState& st = car.state;
    const rr::Vec2 f2 = rr::fromAngle(st.yaw), l2 = rr::perpLeft(f2);
    // the road (or ground) under each tyre
    auto groundAt = [&](rr::Vec2 p) {
        rr::TrackLoc loc = track.locate(p, hint_, 40);
        hint_ = loc.idx;
        if (std::fabs(loc.lateral) <= loc.halfWidth + 1.3f) return track.heightAt(loc.s, loc.lateral);
        return scene.groundHeight(p.x, -p.y);
    };
    float h[4];
    for (int i = 0; i < 4; ++i) h[i] = groundAt(st.pos + f2 * wheels_[i].hub.z + l2 * wheels_[i].hub.x);
    const float hf = 0.5f * (h[0] + h[1]), hr = 0.5f * (h[2] + h[3]);
    const float hl = 0.5f * (h[0] + h[2]), hri = 0.5f * (h[1] + h[3]);
    const Vector3 up0 = {0, 1, 0};
    Vector3 F = Vector3Normalize(Vector3Add(Vector3Scale(W2(f2), wheelbase_), Vector3Scale(up0, hf - hr)));
    Vector3 L = Vector3Normalize(Vector3Add(Vector3Scale(W2(l2), track_), Vector3Scale(up0, hl - hri)));
    Vector3 U = Vector3Normalize(Vector3CrossProduct(F, L));
    L = Vector3CrossProduct(U, F);
    Vector3 o = W2(st.pos);
    o.y = 0.25f * (h[0] + h[1] + h[2] + h[3]);
    const Matrix road = basis(L, U, F, o);

    // Suspension: the body chases what the loads ask for on stiff, lightly damped springs.
    const float g = 9.81f;
    const float kPitch = 0.6f * DEG2RAD, kRoll = 0.75f * DEG2RAD;  // per g of braking / cornering
    const float mass = car.phys.mass + std::max(0.0f, st.fuel) * car.phys.fuelDensity;
    float load = 0;
    for (float w : st.wheelLoad) load += w;
    const float tPitch = std::clamp(-st.ax / g * kPitch, -0.06f, 0.06f);
    const float tRoll = std::clamp(st.ay / g * kRoll, -0.08f, 0.08f);
    // downforce and compressions squat the car, crests let it rise (about 1.5 cm per g)
    const float tHeave = load > 0 ? std::clamp(-0.015f * (load / (mass * g) - 1.0f), -0.05f, 0.03f) : 0.0f;
    if (first_) {
        pitch_ = tPitch, roll_ = tRoll, heave_ = tHeave;
        first_ = false;
    }
    const float w = 2 * PI * 2.6f, zeta = 0.45f;
    for (float left = std::min(dt, 0.1f); left > 0; left -= 1.0f / 480) {
        const float h1 = std::min(left, 1.0f / 480);
        vp_ += (w * w * (tPitch - pitch_) - 2 * zeta * w * vp_) * h1;
        vr_ += (w * w * (tRoll - roll_) - 2 * zeta * w * vr_) * h1;
        vh_ += (w * w * (tHeave - heave_) - 2 * zeta * w * vh_) * h1;
        pitch_ += vp_ * h1;
        roll_ += vr_ * h1;
        heave_ += vh_ * h1;
    }
    // body: pitch and roll about a point 0.25 m up, then heave, then onto the road
    const float pivot = 0.25f;
    Matrix susp = MatrixMultiply(MatrixMultiply(MatrixTranslate(0, -pivot, 0), MatrixMultiply(MatrixRotateX(pitch_), MatrixRotateZ(roll_))),
                                 MatrixTranslate(0, pivot + heave_, 0));
    body_ = MatrixMultiply(susp, road);

    // The road-wheel angle is small at racing speeds (a couple of degrees in a fast bend),
    // which reads as the car turning more than its wheels do; show it 2.5 times larger, up
    // to about 24 degrees.
    const float visualSteer = std::clamp(st.steerAngle * 2.5f, -0.42f, 0.42f);
    // the steering wheel turns with the road wheels, about 6 times as far: 17 degrees of lock
    // is about 100 degrees at the wheel. A left turn is counter-clockwise for the driver.
    {
        const float a = -std::clamp(st.steerAngle * 6.0f, -1.9f, 1.9f);
        const Vector3 hub = steerHub_, ax = Vector3Normalize(steerAxis_);
        steerM_ = MatrixMultiply(MatrixMultiply(MatrixTranslate(-hub.x, -hub.y, -hub.z), MatrixRotate(ax, a)),
                                 MatrixTranslate(hub.x, hub.y, hub.z));
    }
    // the DRS flap: about its hinge, as far open as the simulated flap is (car.state.drsFlap, 0..1;
    // the race opens it in a zone on request and it closes on the brakes)
    drsOpen_ = std::clamp(car.state.drsFlap, 0.0f, 1.0f);
    {
        const Vector3 p = drsPivot_;
        drsM_ = MatrixMultiply(MatrixMultiply(MatrixTranslate(-p.x, -p.y, -p.z), MatrixRotate(Vector3Normalize(drsAxis_), drsOpen_ * drsMax_)),
                               MatrixTranslate(p.x, p.y, p.z));
    }
    // wheels: on the road, front ones steered, all spinning with the car's speed
    for (int i = 0; i < 4; ++i) {
        const Wheel& wh = wheels_[i];
        const float spin = wh.mirrored ? -st.wheelRot : st.wheelRot;
        Matrix m = MatrixRotateX(spin);
        if (wh.mirrored) m = MatrixMultiply(m, MatrixRotateY(PI));
        if (wh.front) m = MatrixMultiply(m, MatrixRotateY(visualSteer));
        m = MatrixMultiply(m, MatrixTranslate(wh.hub.x, wh.hub.y, wh.hub.z));
        wheel_[i] = MatrixMultiply(m, road);
    }
}

void CarRender::draw(gfx::Renderer& r) const {
    for (const Part& p : body_parts_) r.draw(p.mesh, p.mat, body_);
    for (const Part& p : steer_parts_) r.draw(p.mesh, p.mat, MatrixMultiply(steerM_, body_));
    for (const Part& p : drs_parts_) r.draw(p.mesh, p.mat, MatrixMultiply(drsM_, body_));
    for (int i = 0; i < 4; ++i)
        for (const Part& p : wheel_parts_[wheels_[i].front ? 0 : 1]) r.draw(p.mesh, p.mat, wheel_[i]);
}

void CarRender::drawShadow(gfx::Renderer& r) const {
    for (const Part& p : body_parts_) r.drawShadow(p.mesh, body_);
    for (const Part& p : steer_parts_) r.drawShadow(p.mesh, MatrixMultiply(steerM_, body_));
    for (const Part& p : drs_parts_) r.drawShadow(p.mesh, MatrixMultiply(drsM_, body_));
    for (int i = 0; i < 4; ++i)
        for (const Part& p : wheel_parts_[wheels_[i].front ? 0 : 1]) r.drawShadow(p.mesh, wheel_[i]);
}
