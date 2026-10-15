#include "renderer2.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>

#include "car_render.hpp"
#include "decals.hpp"
#include "effects.hpp"
#include "gfx.hpp"
#include "liveries.hpp"
#include "raymath.h"
#include "rlgl.h"
#include "track_scene.hpp"

namespace fs = std::filesystem;

namespace {

Vector3 W2(rr::Vec2 v, float h = 0) { return {v.x, h, -v.y}; }
constexpr int kCarModels = 11;  // assets/cars/f1_2013_01 .. 11

const char* kCamNames[CAM_COUNT] = {"FOLLOW", "CINEMATIC", "TV", "HELICOPTER", "TOP DOWN", "ORBIT", "OVERVIEW", "T-CAM", "NOSE", "DIRECTOR"};

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
    Effects fx;
    Decals decals;

    // camera state
    Vector3 eye{}, look{};
    bool init = false;
    CamMode lastMode = CAM_COUNT;
    int lastFocus = -1;
    float orbitYaw = 0.6f, orbitPitch = 0.25f, orbitDist = 9.0f;
    std::vector<Vector3> tvSpots;
    int tvSpot = -1;
    Vector3 lastCamPos{};
    int lastTvSpot = -1;
    bool haveCamPos = false;
    bool camCut = true;  // the camera jumped this frame (a new shot, car or camera): no motion blur

    // Tyre smoke on a full lock-up: puffs laid down the path of the locked wheels. Viewer only; the
    // sim never sees it.
    struct Smoke { Vector3 p, v; float age, life, size0, size1, alpha0, rot, spin; };
    struct WheelTrail { Vector3 last[4]; float carry[4] = {0, 0, 0, 0}; bool valid = false; };
    std::vector<Smoke> smoke;
    std::vector<WheelTrail> trails;
    double smokeTime = -1;
    unsigned smokeRng = 2463534242u;
    float rnd() {
        smokeRng ^= smokeRng << 13, smokeRng ^= smokeRng >> 17, smokeRng ^= smokeRng << 5;
        return (smokeRng >> 8) / 16777216.0f;
    }

    // Post effects for the camera the frame was drawn with (see Renderer::draw).
    void setPost(const Camera3D& cam, const ViewOptions& opt);
    void updateSmoke(const rr::Race& race, float dt);
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

    // Glossy lacquer on every car; chrome on the McLaren and metallic silver on the Mercedes in their
    // own team livery (not the Mercedes standing in for a team that asked for its own colour).
    void applyFinish(int i) {
        if (!cars[i]) return;
        CarRender::Finish f;
        f.roughness = 0.3f, f.metalness = 0.1f;
        if (carPaint[i].a == 0 && carModel[i] == 8) {
            f.metalness = 1.0f, f.roughness = 0.08f, f.clearcoat = 0.0f;
        } else if (carPaint[i].a == 0 && carModel[i] == 2) {  // the Mercedes' metallic silver under the lacquer
            f.metalness = 0.7f, f.roughness = 0.28f;
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

    // Trackside TV cameras on the outside of the bends, on the track side of the barrier (in front of any catch
    // fence, tyre wall and tree, so the picture never looks through them), a little above the wall.
    void buildTvSpots(const rr::Track& tr) {
        tvSpots.clear();
        for (float s = 60; s < tr.length(); s += 220) {
            const auto& sm = tr.at(tr.indexAt(s));
            if (tr.inPitArea(s)) continue;                                       // not in the pit lane
            bool underBridge = false;                                            // nor beneath a footbridge
            for (const auto& f : tr.features())
                if (f.kind == "bridge" && std::fabs(std::remainder(f.s0 - s, tr.length())) < 25.0f) underBridge = true;
            if (underBridge) continue;
            const int side = sm.curvature > 0 ? -1 : 1;  // the outside of any bend
            const float lat = side * (tr.barrierOffset(s, side, sm.halfWidth) - 2.4f);
            Vector3 w = W2(tr.pointAt(s, lat));
            w.y = tr.heightAt(s, lat) + 2.3f;
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
    p_->fx.setUseHeights(true);
    gImpl = p_.get();
    if (!p_->gr.init(std::max(2, GetScreenWidth()), std::max(2, GetScreenHeight()), err, 4)) return false;
    if (!p_->gr.loadSky(assetsDir + "/sky", "kloofendal_partly_cloudy", err)) return false;
    if (!p_->scene.build(track, assetsDir, seed ? seed : 1, err)) return false;
    return true;
}

void Renderer::stepEffects(const rr::Race& race, float dt) {
    p_->fx.update(race, dt);
    p_->updateSmoke(race, dt > 0 ? dt : -1.0f);  // (shot mode calls this as it runs the race up to the saved frame)
}

void Renderer::adjustLighting(bool nextSky, float turn, float exposureFactor) {
    static const char* skies[] = {"kloofendal_partly_cloudy", "mud_road", "overcast_soil"};
    Impl& p = *p_;
    if (nextSky) {
        std::string err;
        // four looks: the three skies, then the partly cloudy sky again as a hazy low-sun look
        const int look = (p.skyIdx + 1) % 4;
        if (p.gr.loadSky(p.assets + "/sky", skies[look == 3 ? 0 : look], &err)) {
            p.skyIdx = look;
            p.gr.haze = look == 3 ? 1.0f : 0.0f;
        }
    }
    p.gr.skyYaw += turn;
    p.gr.exposure = std::clamp(p.gr.exposure * exposureFactor, 0.2f, 3.0f);
}

void Renderer::shutdown() {
    if (gImpl == p_.get()) gImpl = nullptr;
    p_->unloadCars();
    p_->decals.release();
    p_->scene.unload();
    p_->gr.shutdown();
}



void Renderer::updateCamera(const rr::Race& race, int focus, CamMode mode, float dt) {
    Impl& p = *p_;
    p.sync(race, dt);
    p.fx.update(race, dt);

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
    p.camCut = !p.init || !p.haveCamPos || Vector3Distance(c.position, p.lastCamPos) > 30.0f ||
               (mode == CAM_TV && p.tvSpot != p.lastTvSpot);
    p.lastTvSpot = p.tvSpot;
    p.lastCamPos = c.position;
    p.haveCamPos = true;
    p.init = true;
    camera = c;
}

// What each camera gets, in the picture's own terms:
//  - lens depth of field where the shot is a real lens (cinematic, TV): the car in focus, the
//    background and any fence in front of the lens soft; a longer lens blurs more.
//  - a slight blur of far-off things on every camera that looks along the ground.
//  - camera motion blur where the camera moves or pans (not the top-down, overview and orbit views),
//    with the followed car kept sharp.
//  - a little bloom everywhere.
void Renderer::Impl::setPost(const Camera3D& cam, const ViewOptions& opt) {
    gfx::Renderer::PostParams P;
    if (opt.quality <= 0 || lastFocus < 0) {
        gr.post = P;
        return;
    }
    const bool full = opt.quality >= 2;
    CarRender* cr = car(lastFocus);
    const bool lens = lastMode == CAM_CINEMATIC || lastMode == CAM_TV;
    const bool ground = lastMode != CAM_TOP && lastMode != CAM_OVERVIEW;
    const float tanHalf = std::tan(cam.fovy * DEG2RAD * 0.5f);
    P.bloom = 0.35f + 0.1f * gr.haze;
    P.ao = full ? 0.7f : 0.5f;
    P.aoRadius = 0.8f;
    P.grade = 1.0f;
    P.bloomThreshold = 0.7f + 0.3f * gr.haze;
    P.glare = gr.haze;

    if (ground) {
        P.hazeCoc = 2.4f;
        P.hazeStart = 70.0f;
        P.hazeEnd = 350.0f;
    }
    if (lens && cr) {
        const Vector3 target = Vector3Add(cr->origin(), {0, 0.5f, 0});
        const float f = std::max(Vector3Distance(cam.position, target), 3.0f);
        constexpr float kHalfCar = 2.7f;  // m: the car stays sharp from nose to tail
        P.focusDist = f;
        P.focusTol = 0.6f * kHalfCar / (f * std::max(f - kHalfCar, 1.0f));
        P.lensK = 70.0f / tanHalf;  // px per dioptre of defocus: a long lens is shallow
        P.maxCoc = 14.0f;
    }
    P.taps = lens ? (full ? 32 : 16) : (full ? 14 : 8);

    // motion blur
    float shutter = 0;
    switch (lastMode) {
        case CAM_CHASE: shutter = 0.45f; break;
        case CAM_CINEMATIC: shutter = 0.35f; break;
        case CAM_TV: shutter = 0.5f; break;
        case CAM_HELI: shutter = 0.3f; break;
        case CAM_TCAM:
        case CAM_NOSE: shutter = 0.5f; break;
        default: break;
    }
    P.shutter = shutter;
    P.cut = camCut;
    if (shutter > 0 && cr) {
        if (lastMode == CAM_TCAM || lastMode == CAM_NOSE) {
            P.keepDepth = 3.5f;  // the car's own nose and wheels
        } else {
            const Vector3 centre = Vector3Add(cr->origin(), {0, 0.5f, 0});
            const Vector2 sp = GetWorldToScreen(centre, cam);
            const float W = (float)GetScreenWidth(), H = (float)GetScreenHeight();
            const float d = std::max(Vector3Distance(cam.position, centre), 1.0f);
            const float ppm = (H * 0.5f) / (tanHalf * d);  // pixels per metre at the car
            P.keep = {sp.x / W, 1.0f - sp.y / H, 3.6f * ppm / W, 2.4f * ppm / H};
        }
    }
    gr.post = P;
}

// Smoke from the front or rear tyres when they lock under heavy braking: the brake force asked of the
// axle well past what its tyres can give (not just hard braking), the pedal nearly flat and the car
// still fast. dt < 0 ages the smoke by the sim time since the last call.
void Renderer::Impl::updateSmoke(const rr::Race& race, float dt) {
    const double now = race.time();
    if (dt < 0) dt = smokeTime >= 0 && now > smokeTime ? (float)std::min(now - smokeTime, 0.1) : 0.0f;
    for (auto& s : smoke) {
        s.age += dt;
        s.p = Vector3Add(s.p, Vector3Scale(s.v, dt));
        s.v = Vector3Scale(s.v, 1.0f - std::min(1.0f, 1.4f * dt));
    }
    smoke.erase(std::remove_if(smoke.begin(), smoke.end(), [](const Smoke& s) { return s.age >= s.life; }), smoke.end());

    const bool moved = now > smokeTime;
    const bool jump = smokeTime < 0 || now < smokeTime || now - smokeTime > 0.5;  // a restart, replay scrub or fast-forward
    smokeTime = now;
    if (trails.size() != race.cars().size()) trails.assign(race.cars().size(), WheelTrail{});
    if (jump) {
        smoke.clear();
        for (auto& t : trails) t.valid = false;
    }
    if (!moved) return;
    constexpr float kHalfBase = 1.7f, kHalfTrack = 0.72f, kSpacing = 1.3f;
    constexpr float kLockUse = 1.3f, kFullLock = 2.2f;  // brake force asked / what the axle's tyres can give
    for (size_t i = 0; i < race.cars().size(); ++i) {
        const rr::Car& c = race.cars()[i];
        WheelTrail& tr = trails[i];
        const float cy = std::cos(c.state.yaw), sy = std::sin(c.state.yaw);
        Vector3 now4[4];
        for (int w = 0; w < 4; ++w) {
            const float lx = w < 2 ? kHalfBase : -kHalfBase, ly = (w % 2 == 0) ? kHalfTrack : -kHalfTrack;
            now4[w] = W2(c.state.pos + rr::Vec2{lx * cy - ly * sy, lx * sy + ly * cy});
            now4[w].y = scene.groundHeight(now4[w].x, now4[w].z) + 0.1f;
        }
        const float speed = std::hypot(c.state.vx, c.state.vy);
        const bool braking = c.control.brake > 0.9f && speed > 12.0f && !c.dnf && c.pitState == RR_PIT_NONE;
        if (tr.valid && braking) {
            const rr::Vec2 vw = c.state.velWorld();
            const Vector3 vel = {vw.x, 0, -vw.y};
            const float fast = std::clamp((speed - 8.0f) / 25.0f, 0.0f, 1.0f);  // a car crawling to a stop barely smokes
            for (int axle = 0; axle < 2; ++axle) {
                const float k = std::clamp((c.state.gripUse[axle] - kLockUse) / (kFullLock - kLockUse), 0.0f, 1.0f) * fast;
                if (k <= 0) continue;
                for (int w = 2 * axle; w < 2 * axle + 2; ++w) {
                    const Vector3 a = tr.last[w], b = now4[w];
                    const float len = Vector3Distance(a, b);
                    if (len > 25.0f || len < 1e-4f) continue;
                    tr.carry[w] += len;
                    while (tr.carry[w] >= kSpacing && smoke.size() < 600) {
                        tr.carry[w] -= kSpacing;
                        const float u = 1.0f - tr.carry[w] / len;  // where along this step the puff falls
                        Smoke s;
                        s.p = Vector3Lerp(a, b, std::clamp(u, 0.0f, 1.0f));
                        s.p.x += (rnd() - 0.5f) * 0.3f;
                        s.p.z += (rnd() - 0.5f) * 0.3f;
                        s.p.y += 0.1f + rnd() * 0.25f;
                        s.v = Vector3Add(Vector3Scale(vel, 0.12f), {(rnd() - 0.5f) * 1.6f, 0.9f + rnd() * 1.2f, (rnd() - 0.5f) * 1.6f});
                        s.age = 0;
                        s.life = 1.2f + rnd() * 0.8f + 0.5f * k;
                        s.size0 = 0.5f + 0.3f * rnd();
                        s.size1 = 2.2f + 1.2f * k + 1.0f * rnd();
                        s.alpha0 = 0.14f + 0.26f * k;
                        s.rot = rnd() * 6.2832f;
                        s.spin = (rnd() - 0.5f) * 0.8f;
                        smoke.push_back(s);
                    }
                }
            }
        } else {
            for (float& v : tr.carry) v = 0;
        }
        for (int w = 0; w < 4; ++w) tr.last[w] = now4[w];
        tr.valid = true;
    }
}

void Renderer::draw(const rr::Race& race, int focus, const ViewOptions& opt) {
    Impl& p = *p_;
    p.sync(race, 0);

    (void)focus;
    if (GetScreenWidth() != p.gr.width() || GetScreenHeight() != p.gr.height()) p.gr.resize(std::max(2, GetScreenWidth()), std::max(2, GetScreenHeight()));
    const Camera3D& cam = camera;
    const Vector3 fwd = Vector3Normalize(Vector3Subtract(cam.target, cam.position));
    // a retired car has been taken off the track: no body, no shadow
    auto shown = [&](int i) { return p.cars[i] && i < (int)race.cars().size() && !race.cars()[i].dnf; };

    // shadows: a sharp cascade round the view's middle, a coarse one out to about a kilometre
    // (redrawn every third frame: it changes slowly), and a fine one just for the cars round
    // the one in view (the middle cascade's 5 cm texels and bias lose an F1 car's shadow under it)
    const float kCarRadius = 12.0f;
    int lookAt = -1;
    {
        float best = 1e9f;
        for (int i = 0; i < (int)p.cars.size(); ++i)
            if (shown(i)) {
                const float d = Vector3Distance(p.cars[i]->origin(), cam.target);
                if (d < best) best = d, lookAt = i;
            }
        if (lookAt >= 0 && (best > 8.0f || Vector3Distance(p.cars[lookAt]->origin(), cam.position) > 150.0f)) lookAt = -1;
    }
    std::vector<char> fine(p.cars.size(), 0);
    if (lookAt >= 0) {
        const Vector3 c = p.cars[lookAt]->origin();
        for (int i = 0; i < (int)p.cars.size(); ++i)
            fine[i] = shown(i) && Vector3Distance(p.cars[i]->origin(), c) < kCarRadius - 3.5f;
        p.gr.beginShadows(c, kCarRadius, gfx::Renderer::kCarCascade);
        for (int i = 0; i < (int)p.cars.size(); ++i)
            if (fine[i]) p.cars[i]->drawShadow(p.gr);
        p.gr.endShadows();
    }
    p.gr.beginShadows(Vector3Add(cam.position, Vector3Scale(fwd, 60.0f)), 110.0f, 0);
    p.scene.drawShadows(p.gr);
    for (int i = 0; i < (int)p.cars.size(); ++i)
        if (shown(i) && !fine[i]) p.cars[i]->drawShadow(p.gr);
    p.gr.endShadows();
    if (p.frame % 3 == 0) {
        p.gr.beginShadows(Vector3Add(cam.position, Vector3Scale(fwd, 550.0f)), 750.0f, 1);
        p.scene.drawShadows(p.gr);
        p.gr.endShadows();
    }
    ++p.frame;

    // contact shadows under the cars nearest the camera, and on their own lower bodies
    struct Near { float d; int i; };
    std::vector<Near> near;
    for (int i = 0; i < (int)p.cars.size(); ++i)
        if (shown(i)) near.push_back({Vector3Distance(p.cars[i]->origin(), cam.position), i});
    std::sort(near.begin(), near.end(), [](const Near& a, const Near& b) { return a.d < b.d; });
    p.gr.blobs.clear();
    for (int k = 0; k < (int)near.size() && k < gfx::Renderer::kBlobs; ++k) {
        const Vector3 o = p.cars[near[k].i]->origin();
        p.gr.blobs.push_back({o.x, o.z, race.cars()[near[k].i].state.yaw, o.y});
    }
    p.gr.beginScene(cam);
    p.scene.draw(p.gr);
    for (int i = 0; i < (int)p.cars.size(); ++i)
        if (shown(i)) p.cars[i]->draw(p.gr);
    p.gr.blobs.clear();
    p.fx.setLevel(opt.quality);
    p.decals.draw(p.gr, p.fx, race, cam.position);
    p.gr.drawSky();
    p.updateSmoke(race, std::min(GetFrameTime(), 0.1f));
    {
        std::vector<gfx::Renderer::Puff> puffs;
        puffs.reserve(p.smoke.size());
        for (const auto& s : p.smoke) {
            const float t = s.age / s.life;
            const float fade = std::min(1.0f, s.age / 0.12f) * std::pow(1.0f - t, 1.5f);
            puffs.push_back({s.p, s.size0 + (s.size1 - s.size0) * std::sqrt(t), s.alpha0 * fade, s.rot + s.spin * s.age});
        }
        p.gr.drawPuffs(puffs);
    }
    p.gr.endScene();
    p.setPost(camera, opt);

    rlViewport(0, 0, GetRenderWidth(), GetRenderHeight());
    p.gr.present(0, 0, GetScreenWidth(), GetScreenHeight());
    p.synced = false;
}
