#include "renderer2.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>

#include "car_render.hpp"
#include "gfx.hpp"
#include "liveries.hpp"
#include "raymath.h"
#include "rlgl.h"
#include "track_scene.hpp"

namespace fs = std::filesystem;

namespace {

Vector3 W2(rr::Vec2 v, float h = 0) { return {v.x, h, -v.y}; }
constexpr int kCarModels = 11;  // assets/cars/f1_2013_01 .. 11

const char* kCamNames[CAM_COUNT] = {"FOLLOW", "CINEMATIC", "TV", "HELICOPTER", "TOP DOWN", "ORBIT", "OVERVIEW", "DIRECTOR", "T-CAM", "NOSE"};

}  // namespace

const char* camName(CamMode mode) { return mode >= 0 && mode < CAM_COUNT ? kCamNames[mode] : "?"; }

// A car's colour on the HUD, minimap, pit box and debug lines: its team's.
Color teamColor(int i) {
    const auto& t = liveryTable();
    if (!t.empty()) return t[carLivery(i)].color;
    static const Color c[] = {{220, 35, 35, 255}, {20, 100, 215, 255}, {255, 128, 0, 255}, {0, 135, 95, 255}};
    return c[i % 4];
}

Color teamAccent(int i) { return i % 2 ? Color{30, 30, 35, 255} : Color{245, 245, 245, 255}; }

std::vector<float> loadExhaustImpulse(const std::string& assetsDir) {
    std::vector<float> ir;
    const std::string p = assetsDir + "/sound/exhaust_f1v10.wav";
    if (!fs::exists(p)) return ir;
    Wave wv = LoadWave(p.c_str());
    if (wv.frameCount > 0 && wv.sampleRate == 44100) {
        WaveFormat(&wv, 44100, 32, 1);
        const float* fp = (const float*)wv.data;
        const int n = std::min<int>((int)wv.frameCount, 44100 / 2);  // the first half second
        ir.assign(fp, fp + n);
        float e = 0;
        for (float v : ir) e += v * v;
        const float norm = e > 0 ? 1.0f / std::sqrt(e) : 1.0f;
        for (float& v : ir) v *= norm;  // unit energy: the sound's gain sets the level
        for (int i = n * 3 / 4; i < n; ++i) ir[i] *= 1.0f - (float)(i - n * 3 / 4) / (n - n * 3 / 4);
    }
    UnloadWave(wv);
    return ir;
}

struct Renderer::Impl {
    gfx::Renderer gr;
    TrackScene scene;
    std::string assets;
    std::vector<std::unique_ptr<CarRender>> cars;
    std::vector<int> carModel;
    std::vector<Color> carPaint;   // the team colour each car was painted with (model 0 only)
    std::vector<const rr::Car*> carPtrs;
    bool synced = false;
    double lastTime = -1;
    int frame = 0;
    int skyIdx = 0;

    // camera state
    Vector3 eye{}, look{};
    bool init = false;
    CamMode lastMode = CAM_COUNT;
    int lastFocus = -1;
    float orbitYaw = 0.6f, orbitPitch = 0.25f, orbitDist = 9.0f;
    std::vector<Vector3> tvSpots;
    int tvSpot = -1;
    float cineClock = 0;
    Vector3 cineOff{};

    // The model car i drives: its team's stock car, or the Mercedes (also the stand-in for a car the
    // installation does not have) when the team asked for it in its own colour.
    static int modelOf(int i) {
        const auto& t = liveryTable();
        const int m = t.empty() ? 0 : t[carLivery(i)].model;
        return m >= 1 && m <= kCarModels ? m : 2;
    }
    static bool ownColour(int i) {
        const auto& t = liveryTable();
        return t.empty() || t[carLivery(i)].model == 0 || !stockPresent(t[carLivery(i)].model);
    }

    // Glossy lacquer on every car; chrome on the Mercedes and McLaren in their own team livery
    // (not the Mercedes standing in for a team that asked for its own colour).
    void applyFinish(int i) {
        if (!cars[i]) return;
        CarRender::Finish f;
        f.roughness = 0.3f, f.metalness = 0.1f;
        if ((carModel[i] == 2 || carModel[i] == 8) && carPaint[i].a == 0) {
            f.metalness = 1.0f, f.roughness = 0.08f, f.clearcoat = 0.0f;
        }
        cars[i]->setFinish(f);
    }

    CarRender* car(int i) { return i >= 0 && i < (int)cars.size() ? cars[i].get() : nullptr; }

    void ensureCars(const rr::Race& race) {
        const int n = (int)race.cars().size();
        if ((int)cars.size() == n) {
            bool same = true;
            for (int i = 0; i < n; ++i)
                if (carModel[i] != modelOf(i)) same = false;
            if (same) {
                for (int i = 0; i < n; ++i) {
                    const Color c = ownColour(i) ? teamColor(i) : Color{0, 0, 0, 0};
                    if (cars[i] && (c.r != carPaint[i].r || c.g != carPaint[i].g || c.b != carPaint[i].b || c.a != carPaint[i].a)) {
                        if (c.a) cars[i]->setPaint(c);
                        else cars[i]->clearPaint();
                        carPaint[i] = c;
                        applyFinish(i);
                    }
                }
                return;
            }
        }
        unloadCars();
        carModel.assign(n, 0);
        carPaint.assign(n, Color{0, 0, 0, 0});
        std::vector<CarRender*> loaded(kCarModels + 1, nullptr);  // by model: the car that owns its meshes
        for (int i = 0; i < n; ++i) {
            const int want = modelOf(i);
            carModel[i] = want;
            // the models bundled with the game; any other team's car wears the Mercedes
            int m = want;
            char dir[64];
            std::snprintf(dir, sizeof dir, "/cars/f1_2013_%02d", m);
            if (!loaded[m] && !fs::exists(assets + dir + "/car.json")) {
                m = 2;
                std::snprintf(dir, sizeof dir, "/cars/f1_2013_%02d", m);
            }
            cars.push_back(std::make_unique<CarRender>());
            if (loaded[m]) {
                cars[i]->shareFrom(*loaded[m]);
                continue;
            }
            CarRender::Finish paint;
            std::string err;
            if (cars[i]->load(assets + dir, paint, &err)) {
                loaded[m] = cars[i].get();
            } else {
                std::fprintf(stderr, "car %d: %s\n", i, err.c_str());
                cars[i].reset();
            }
        }
        for (int i = 0; i < n; ++i)
            if (cars[i]) {
                carPaint[i] = ownColour(i) ? teamColor(i) : Color{0, 0, 0, 0};
                if (carPaint[i].a) cars[i]->setPaint(carPaint[i]);
                applyFinish(i);
            }
    }

    // the cars that share meshes first, then the ones that own them
    void unloadCars() {
        for (int pass = 0; pass < 2; ++pass)
            for (auto& c : cars)
                if (c && (c->isOwner() == (pass == 1))) c->unload();
        cars.clear();
    }

    void sync(const rr::Race& race, float dt) {
        if (synced) return;
        synced = true;
        ensureCars(race);
        carPtrs.clear();
        for (const rr::Car& c : race.cars()) carPtrs.push_back(&c);
        const double t = race.time();
        float step = lastTime < 0 ? 0.0f : (float)(t - lastTime);
        if (step < 0 || step > 0.25f) step = 0.0f;  // a jump (replay, scrub, restart): no spring time
        lastTime = t;
        (void)dt;
        for (int i = 0; i < (int)cars.size(); ++i)
            if (cars[i]) {
                cars[i]->update(race.cars()[i], race.track(), scene, step);
            }
    }

    void buildTvSpots(const rr::Track& tr) {
        tvSpots.clear();
        for (float s = 60; s < tr.length(); s += 240) {
            const auto& sm = tr.at(tr.indexAt(s));
            const int side = sm.curvature > 0 ? -1 : 1;  // the outside of any bend
            const float lat = side * (tr.barrierOffset(s, side, sm.halfWidth) + 8.0f);
            Vector3 w = W2(tr.pointAt(s, lat));
            w.y = scene.groundHeight(w.x, w.z) + 4.5f;
            tvSpots.push_back(w);
        }
    }
};

namespace {
Renderer::Impl* gImpl = nullptr;
}

float carHeightForSound(const rr::Car& car) {
    if (gImpl)
        for (size_t i = 0; i < gImpl->carPtrs.size(); ++i)
            if (gImpl->carPtrs[i] == &car && gImpl->cars[i]) return gImpl->cars[i]->origin().y + 0.6f;
    return 0.6f;
}

Renderer::Renderer() : p_(new Impl) {}
Renderer::~Renderer() = default;

bool Renderer::init(const rr::Track& track, unsigned seed, const std::string& assetsDir, std::string* err) {
    p_->assets = assetsDir;
    gImpl = p_.get();
    if (!p_->gr.init(std::max(2, GetScreenWidth()), std::max(2, GetScreenHeight()), err, 4)) return false;
    if (!p_->gr.loadSky(assetsDir + "/sky", "kloofendal_partly_cloudy", err)) return false;
    if (!p_->scene.build(track, assetsDir, seed ? seed : 1, err)) return false;
    return true;
}

void Renderer::adjustLighting(bool nextSky, float turn, float exposureFactor) {
    static const char* skies[] = {"kloofendal_partly_cloudy", "mud_road", "overcast_soil"};
    Impl& p = *p_;
    if (nextSky) {
        std::string err;
        const int idx = (p.skyIdx + 1) % 3;
        if (p.gr.loadSky(p.assets + "/sky", skies[idx], &err)) p.skyIdx = idx;
    }
    p.gr.skyYaw += turn;
    p.gr.exposure = std::clamp(p.gr.exposure * exposureFactor, 0.2f, 3.0f);
}

void Renderer::shutdown() {
    if (gImpl == p_.get()) gImpl = nullptr;
    p_->unloadCars();
    p_->scene.unload();
    p_->gr.shutdown();
}



void Renderer::updateCamera(const rr::Race& race, int focus, CamMode mode, float dt) {
    Impl& p = *p_;
    p.sync(race, dt);

    focus = std::clamp(focus, 0, std::max(0, (int)race.cars().size() - 1));
    CarRender* cr = p.car(focus);
    if (!cr) return;
    if (mode != p.lastMode || focus != p.lastFocus) {
        p.init = false;
        p.lastMode = mode;
        p.lastFocus = focus;
    }
    const Vector3 o = cr->origin(), f = cr->forward(), u = cr->up();
    auto local = [&](float x, float y, float z) {
        return Vector3Add(o, Vector3Add(Vector3Scale(cr->left(), x), Vector3Add(Vector3Scale(u, y), Vector3Scale(f, z))));
    };
    Camera3D c{};
    c.up = {0, 1, 0};
    c.projection = CAMERA_PERSPECTIVE;
    c.fovy = 55;
    const float k = 1.0f - std::exp(-std::min(dt, 0.1f) * 7.0f);
    const Vector3 flat = Vector3Normalize({f.x, 0.25f * f.y, f.z});
    switch (mode) {
        case CAM_CINEMATIC: {
            // a new framing every few seconds: low rear three-quarter, alongside, front three-quarter, high behind
            p.cineClock += dt;
            const int shot = (int)(p.cineClock / 6.0f) % 4;
            const float side = ((int)(p.cineClock / 12.0f) % 2) ? 1.0f : -1.0f;
            // The shot is an offset in the car's heading frame (yaw only, so the suspension's bobbing does
            // not shake it), eased from one shot to the next; it is not chased in world space, which
            // lags tens of metres behind at 90 m/s and makes the distance wobble.
            Vector3 want;  // (left, up, forward)
            switch (shot) {
                case 0: want = {side * 2.6f, 0.8f, -6.5f}; break;
                case 1: want = {side * 5.5f, 1.1f, 1.5f}; break;
                case 2: want = {side * 3.0f, 0.7f, 8.0f}; break;
                default: want = {side * 1.0f, 3.6f, -10.0f}; break;
            }
            if (!p.init) p.cineOff = want;
            p.cineOff = Vector3Lerp(p.cineOff, want, 1.0f - std::exp(-std::min(dt, 0.1f) * 2.5f));
            const Vector3 fh = Vector3Normalize({f.x, 0, f.z});
            const Vector3 lh = {fh.z, 0, -fh.x};
            p.eye = Vector3Add(o, Vector3Add(Vector3Scale(lh, p.cineOff.x), Vector3Add({0, p.cineOff.y, 0}, Vector3Scale(fh, p.cineOff.z))));
            p.eye.y = std::max(p.eye.y, p.scene.groundHeight(p.eye.x, p.eye.z) + 0.5f);
            c.position = p.eye;
            c.target = Vector3Add(o, {0, 0.5f, 0});
            c.fovy = shot == 2 ? 44.0f : 52.0f;
            break;
        }
        case CAM_TV: {
            if (p.tvSpots.empty()) p.buildTvSpots(race.track());
            int best = 0;
            float bestScore = 1e30f;
            for (int i = 0; i < (int)p.tvSpots.size(); ++i) {
                const float score = Vector3Distance(p.tvSpots[i], o) - (i == p.tvSpot ? 40.0f : 0.0f);  // hold a camera a little
                if (score < bestScore) bestScore = score, best = i;
            }
            p.tvSpot = best;
            c.position = p.tvSpots[best];
            c.target = Vector3Add(o, {0, 0.6f, 0});
            const float d = Vector3Distance(c.position, c.target);
            c.fovy = std::clamp(2.0f * std::atan(9.0f / d) * RAD2DEG, 6.0f, 60.0f);
            break;
        }
        case CAM_HELI: {
            const Vector3 want = Vector3Add(Vector3Subtract(o, Vector3Scale(Vector3Normalize({f.x, 0, f.z}), 30.0f)), {0, 22.0f, 0});
            if (!p.init) p.eye = want;
            p.eye = Vector3Lerp(p.eye, want, 1.0f - std::exp(-std::min(dt, 0.1f) * 2.0f));
            c.position = p.eye;
            c.target = Vector3Add(o, Vector3Scale(Vector3Normalize({f.x, 0, f.z}), 8.0f));
            c.fovy = 45;
            break;
        }
        case CAM_TOP: {
            c.position = Vector3Add(o, {0, 105.0f, 0});
            c.target = o;
            c.up = Vector3Normalize({f.x, 0, f.z});
            c.fovy = 40;
            break;
        }
        case CAM_ORBIT: {
            if (IsMouseButtonDown(MOUSE_BUTTON_LEFT) || IsMouseButtonDown(MOUSE_BUTTON_RIGHT)) {
                const Vector2 md = GetMouseDelta();
                p.orbitYaw -= md.x * 0.005f;
                p.orbitPitch = std::clamp(p.orbitPitch + md.y * 0.004f, -0.05f, 1.4f);
            }
            p.orbitDist = std::clamp(p.orbitDist - GetMouseWheelMove() * 0.8f, 3.5f, 40.0f);
            const float yaw = std::atan2(f.x, f.z) + p.orbitYaw;
            c.target = Vector3Add(o, {0, 0.5f, 0});
            c.position = Vector3Add(c.target, {std::sin(yaw) * std::cos(p.orbitPitch) * p.orbitDist, std::sin(p.orbitPitch) * p.orbitDist,
                                               std::cos(yaw) * std::cos(p.orbitPitch) * p.orbitDist});
            c.position.y = std::max(c.position.y, p.scene.groundHeight(c.position.x, c.position.z) + 0.3f);
            c.fovy = 45;
            break;
        }
        case CAM_OVERVIEW: {
            const Vector3 m = p.scene.centre();
            const float ext = p.scene.extent();
            c.target = m;
            c.position = Vector3Add(m, {0, ext * 0.95f, ext * 0.45f});
            c.fovy = 45;
            break;
        }
        case CAM_TCAM:  // above and just behind the driver, the airbox camera of the era
            c.position = local(0, 1.12f, -0.35f);
            c.target = local(0, 0.85f, 25.0f);
            c.up = u;
            c.fovy = 70;
            break;
        case CAM_NOSE:  // on the nose, looking down the road
            c.position = local(0, 0.42f, 2.35f);
            c.target = local(0, 0.35f, 30.0f);
            c.up = u;
            c.fovy = 72;
            break;
        case CAM_CHASE:
        default: {
            // behind and above, the heading flattened so the camera does not pitch with the car
            const Vector3 want = Vector3Add(Vector3Subtract(o, Vector3Scale(flat, 7.2f)), {0, 2.3f, 0});
            const Vector3 wantLook = Vector3Add(Vector3Add(o, Vector3Scale(flat, 3.0f)), {0, 0.9f, 0});
            if (!p.init) p.eye = want, p.look = wantLook;
            p.eye = Vector3Lerp(p.eye, want, k);
            p.look = Vector3Lerp(p.look, wantLook, std::min(1.0f, k * 2));
            p.eye.y = std::max(p.eye.y, p.scene.groundHeight(p.eye.x, p.eye.z) + 0.8f);
            c.position = p.eye;
            c.target = p.look;
            c.fovy = 58;
            break;
        }
    }
    p.init = true;
    camera = c;
}

void Renderer::draw(const rr::Race& race, int focus, const ViewOptions& opt) {
    Impl& p = *p_;
    p.sync(race, 0);

    (void)focus;
    (void)opt;
    if (GetScreenWidth() != p.gr.width() || GetScreenHeight() != p.gr.height()) p.gr.resize(std::max(2, GetScreenWidth()), std::max(2, GetScreenHeight()));
    const Camera3D& cam = camera;
    const Vector3 fwd = Vector3Normalize(Vector3Subtract(cam.target, cam.position));

    // shadows: a sharp cascade round the view's middle, a coarse one out to about a kilometre
    // (redrawn every third frame: it changes slowly)
    p.gr.beginShadows(Vector3Add(cam.position, Vector3Scale(fwd, 60.0f)), 110.0f, 0);
    p.scene.drawShadows(p.gr);
    for (auto& c : p.cars)
        if (c) c->drawShadow(p.gr);
    p.gr.endShadows();
    if (p.frame % 3 == 0) {
        p.gr.beginShadows(Vector3Add(cam.position, Vector3Scale(fwd, 550.0f)), 750.0f, 1);
        p.scene.drawShadows(p.gr);
        p.gr.endShadows();
    }
    ++p.frame;

    // contact shadows under the cars nearest the camera (not on the cars themselves)
    struct Near { float d; int i; };
    std::vector<Near> near;
    for (int i = 0; i < (int)p.cars.size(); ++i)
        if (p.cars[i]) near.push_back({Vector3Distance(p.cars[i]->origin(), cam.position), i});
    std::sort(near.begin(), near.end(), [](const Near& a, const Near& b) { return a.d < b.d; });
    p.gr.blobs.clear();
    for (int k = 0; k < (int)near.size() && k < gfx::Renderer::kBlobs; ++k) {
        const Vector3 o = p.cars[near[k].i]->origin();
        p.gr.blobs.push_back({o.x, o.z, race.cars()[near[k].i].state.yaw, o.y});
    }
    p.gr.beginScene(cam);
    p.scene.draw(p.gr);

    p.gr.blobs.clear();
    for (auto& c : p.cars)
        if (c) c->draw(p.gr);
    p.gr.drawSky();
    p.gr.endScene();

    rlViewport(0, 0, GetRenderWidth(), GetRenderHeight());
    p.gr.present(0, 0, GetScreenWidth(), GetScreenHeight());
    p.synced = false;
}
