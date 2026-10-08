// rr_viewer2: Raylib Racers 2's race viewer on the new renderer. For now one car on one
// track: John Fone (the racingline robot) driving a 2013 car round Highmoor Ridge.
//
//   rr_viewer2 [--track highmoor] [--car f1_2013_02] [--robot racingline] [--laps 30]
//              [--sky NAME] [--cam 1-6] [--msaa 1|2|4] [--vsync] [--shots DIR] [--bench]
//
// Keys: 1 chase, 2 T-cam, 3 nose, 4 TV, 5 helicopter, 6 orbit (drag to turn, wheel to zoom),
// C next camera; Space pauses; [ ] slower / faster; F2 next sky; F12 screenshot.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include "car_render.hpp"
#include "config.hpp"
#include "gfx.hpp"
#include "race.hpp"
#include "raylib.h"
#include "raymath.h"
#include "track_scene.hpp"

namespace fs = std::filesystem;
extern "C" void* glfwGetProcAddress(const char* name);

#ifdef _WIN32
// Ask laptops with two GPUs for the fast one (NVIDIA Optimus, AMD PowerXpress).
extern "C" {
__declspec(dllexport) unsigned long NvOptimusEnablement = 1;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}
#endif

namespace {

enum Cam { CHASE, TCAM, NOSE, TV, HELI, ORBIT, CAM_COUNT };
const char* kCamNames[CAM_COUNT] = {"CHASE", "T-CAM", "NOSE", "TV", "HELICOPTER", "ORBIT"};

std::string lapTime(double t) {
    if (t <= 0) return "-:--.---";
    int m = (int)(t / 60);
    return TextFormat("%d:%06.3f", m, t - m * 60);
}

Vector3 W2(rr::Vec2 v, float h = 0) { return {v.x, h, -v.y}; }

struct CameraRig {
    Cam mode = CHASE;
    Vector3 eye{}, look{};
    bool init = false;
    float orbitYaw = 0.6f, orbitPitch = 0.25f, orbitDist = 9.0f;
    std::vector<Vector3> tvSpots;
    int tvSpot = -1;

    Camera3D update(const CarRender& car, const TrackScene& scene, const rr::Track& tr, float carS, float dt) {
        const Vector3 o = car.origin(), f = car.forward(), u = car.up();
        auto local = [&](float x, float y, float z) {
            return Vector3Add(o, Vector3Add(Vector3Scale(car.left(), x), Vector3Add(Vector3Scale(u, y), Vector3Scale(f, z))));
        };
        Camera3D c{};
        c.up = {0, 1, 0};
        c.projection = CAMERA_PERSPECTIVE;
        c.fovy = 55;
        const float k = 1.0f - std::exp(-dt * 7.0f);
        switch (mode) {
            case CHASE: {
                // behind and above, the heading flattened so the camera does not pitch with the car
                Vector3 flat = Vector3Normalize({f.x, 0.25f * f.y, f.z});
                Vector3 want = Vector3Add(Vector3Subtract(o, Vector3Scale(flat, 7.2f)), {0, 2.3f, 0});
                Vector3 wantLook = Vector3Add(Vector3Add(o, Vector3Scale(flat, 3.0f)), {0, 0.9f, 0});
                if (!init) eye = want, look = wantLook;
                eye = Vector3Lerp(eye, want, k);
                look = Vector3Lerp(look, wantLook, std::min(1.0f, k * 2));
                eye.y = std::max(eye.y, scene.groundHeight(eye.x, eye.z) + 0.8f);
                c.position = eye;
                c.target = look;
                c.fovy = 58;
                break;
            }
            case TCAM:
                c.position = local(0, 1.12f, -0.35f);
                c.target = local(0, 0.85f, 25.0f);
                c.up = u;
                c.fovy = 70;
                break;
            case NOSE:
                c.position = local(0, 0.42f, 2.35f);
                c.target = local(0, 0.35f, 30.0f);
                c.up = u;
                c.fovy = 72;
                break;
            case TV: {
                // the trackside camera covering this part of the track, zooming as the car passes
                if (tvSpots.empty()) buildTvSpots(scene, tr);
                int best = 0;
                float bestScore = 1e30f;
                for (int i = 0; i < (int)tvSpots.size(); ++i) {
                    float d = Vector3Distance(tvSpots[i], o);
                    float score = d - (i == tvSpot ? 40.0f : 0.0f);  // hold the current camera a little
                    if (score < bestScore) bestScore = score, best = i;
                }
                tvSpot = best;
                c.position = tvSpots[best];
                c.target = Vector3Add(o, {0, 0.6f, 0});
                float d = Vector3Distance(c.position, c.target);
                c.fovy = std::clamp(2.0f * std::atan(9.0f / d) * RAD2DEG, 6.0f, 60.0f);
                break;
            }
            case HELI: {
                Vector3 flat = Vector3Normalize({f.x, 0, f.z});
                Vector3 want = Vector3Add(Vector3Subtract(o, Vector3Scale(flat, 30.0f)), {0, 22.0f, 0});
                if (!init) eye = want;
                eye = Vector3Lerp(eye, want, 1.0f - std::exp(-dt * 2.0f));
                c.position = eye;
                c.target = Vector3Add(o, Vector3Scale(flat, 8.0f));
                c.fovy = 45;
                break;
            }
            case ORBIT:
            default: {
                if (IsMouseButtonDown(MOUSE_BUTTON_LEFT) || IsMouseButtonDown(MOUSE_BUTTON_RIGHT)) {
                    Vector2 md = GetMouseDelta();
                    orbitYaw -= md.x * 0.005f;
                    orbitPitch = std::clamp(orbitPitch + md.y * 0.004f, -0.05f, 1.4f);
                }
                orbitDist = std::clamp(orbitDist - GetMouseWheelMove() * 0.8f, 3.5f, 40.0f);
                float yaw = std::atan2(f.x, f.z) + orbitYaw;
                c.target = Vector3Add(o, {0, 0.5f, 0});
                c.position = Vector3Add(c.target, {std::sin(yaw) * std::cos(orbitPitch) * orbitDist, std::sin(orbitPitch) * orbitDist,
                                                   std::cos(yaw) * std::cos(orbitPitch) * orbitDist});
                c.position.y = std::max(c.position.y, scene.groundHeight(c.position.x, c.position.z) + 0.3f);
                c.fovy = 45;
                break;
            }
        }
        init = true;
        (void)carS;
        return c;
    }

    // a camera on a pole every ~240 m, beyond the barrier on the outside of the track
    void buildTvSpots(const TrackScene& scene, const rr::Track& tr) {
        for (float s = 60; s < tr.length(); s += 240) {
            const auto& sm = tr.at(tr.indexAt(s));
            int side = sm.curvature > 0 ? -1 : 1;  // the outside of any bend
            float lat = side * (tr.barrierOffset(s, side, sm.halfWidth) + 8.0f);
            rr::Vec2 p = tr.pointAt(s, lat);
            Vector3 w = W2(p);
            w.y = scene.groundHeight(w.x, w.z) + 4.5f;
            tvSpots.push_back(w);
        }
    }
};

// A dark, translucent panel.
void panel(int x, int y, int w, int h) { DrawRectangle(x, y, w, h, Color{10, 12, 16, 170}); }

}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::string trackName = "highmoor", carId = "f1_2013_02", robot = "racingline", skyName = "kloofendal_partly_cloudy";
    std::string shotsDir;
    int width = 1600, height = 900, laps = 30, msaa = 4, startCam = 1;
    bool vsync = false, bench = false;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
        if (a == "--track") trackName = next();
        else if (a == "--car") carId = next();
        else if (a == "--robot") robot = next();
        else if (a == "--laps") laps = std::max(1, std::atoi(next().c_str()));
        else if (a == "--sky") skyName = next();
        else if (a == "--cam") startCam = std::atoi(next().c_str());
        else if (a == "--msaa") msaa = std::atoi(next().c_str());
        else if (a == "--width") width = std::atoi(next().c_str());
        else if (a == "--height") height = std::atoi(next().c_str());
        else if (a == "--vsync") vsync = true;
        else if (a == "--shots") shotsDir = next();
        else if (a == "--bench") bench = true;
        else {
            std::fprintf(stderr, "unknown option %s\n", a.c_str());
            return 2;
        }
    }
    const std::string dir = rr::exeDir(argv[0]);
    std::string assets;
    for (const std::string& d : {dir + "/assets", std::string(RR_SOURCE_DIR "/assets"), std::string("assets")})
        if (fs::exists(d + "/materials")) { assets = d; break; }
    if (assets.empty()) {
        std::fprintf(stderr, "cannot find the assets folder (assets/materials)\n");
        return 1;
    }

    // the race: one car
    rr::RaceConfig cfg;
    cfg.track = trackName;
    cfg.laps = laps;
    cfg.quiet = true;
    cfg.botHost = rr::botHostPath(dir);
    rr::EntrySpec e;
    e.robot = robot;
    e.name = "John Fone";
    cfg.entries.push_back(e);
    rr::Race race;
    std::string err;
    if (!race.setup(cfg, {dir + "/bots", dir, "bots", "."}, {dir + "/tracks", RR_SOURCE_DIR "/tracks", "tracks"}, &err)) {
        std::fprintf(stderr, "race setup failed: %s\n", err.c_str());
        return 1;
    }
    const rr::Track& track = race.track();

    SetConfigFlags(FLAG_WINDOW_RESIZABLE | (vsync && !bench ? FLAG_VSYNC_HINT : 0) | (shotsDir.empty() ? 0 : FLAG_WINDOW_HIDDEN));
    InitWindow(shotsDir.empty() ? width : 320, shotsDir.empty() ? height : 180, "Raylib Racers 2");
    SetTraceLogLevel(LOG_WARNING);
    std::string gpuName = "?";
    {
        auto getString = (const unsigned char* (*)(unsigned))glfwGetProcAddress("glGetString");
        if (getString && getString(0x1F01)) gpuName = (const char*)getString(0x1F01);
    }
    gfx::Renderer gr;
    if (!gr.init(width, height, &err, msaa) || !gr.loadSky(assets + "/sky", skyName, &err)) {
        std::fprintf(stderr, "%s\n", err.c_str());
        CloseWindow();
        return 1;
    }
    TrackScene scene;
    if (!scene.build(track, assets, 1, &err)) {
        std::fprintf(stderr, "%s\n", err.c_str());
        CloseWindow();
        return 1;
    }
    CarRender car;
    CarRender::Finish paint;
    if (carId == "f1_2013_02" || carId == "f1_2013_08") {
        // Mercedes and McLaren: chrome-like silver under the lacquer
        paint.metalness = 0.7f;
        paint.roughness = 0.28f;
    }
    if (!car.load(assets + "/cars/" + carId, paint, &err)) {
        std::fprintf(stderr, "%s\n", err.c_str());
        CloseWindow();
        return 1;
    }
    Font font = LoadFontEx((assets + "/fonts/DejaVuSans-Bold.ttf").c_str(), 40, nullptr, 0);
    Font mono = LoadFontEx((assets + "/fonts/DejaVuSansMono.ttf").c_str(), 32, nullptr, 0);
    SetTextureFilter(font.texture, TEXTURE_FILTER_BILINEAR);
    SetTextureFilter(mono.texture, TEXTURE_FILTER_BILINEAR);

    const rr::Car* me = &race.cars()[0];
    CameraRig rig;
    rig.mode = (Cam)std::clamp(startCam - 1, 0, CAM_COUNT - 1);
    int frameNo = 0;
    auto render = [&](const Camera3D& cam, bool allShadows) {
        Vector3 o = car.origin();
        gr.beginShadows(Vector3Add(o, Vector3Scale(Vector3Normalize(Vector3Subtract(cam.target, cam.position)), 30.0f)), 90.0f, 0);
        scene.drawShadows(gr);
        car.drawShadow(gr);
        gr.endShadows();
        if (allShadows || frameNo % 3 == 0) {
            Vector3 fwd = Vector3Normalize(Vector3Subtract(cam.target, cam.position));
            gr.beginShadows(Vector3Add(cam.position, Vector3Scale(fwd, 550.0f)), 750.0f, 1);
            scene.drawShadows(gr);
            gr.endShadows();
        }
        ++frameNo;
        // contact shadow under the car (but not on it: the car is drawn without)
        gr.blobs = {{o.x, o.z, me->state.yaw, o.y}};
        gr.beginScene(cam);
        scene.draw(gr);
        gr.blobs.clear();
        car.draw(gr);
        gr.drawSky();
        gr.endScene();
    };

    auto hud = [&](int W, int H, double fps) {
        const rr::Car& c = *me;
        const float scale = H / 900.0f;
        auto text = [&](const Font& f, const char* s, float x, float y, float size, Color col) {
            DrawTextEx(f, s, {x, y}, size * scale, 1, col);
        };
        // timing, top left
        panel((int)(16 * scale), (int)(16 * scale), (int)(330 * scale), (int)(132 * scale));
        text(font, TextFormat("%s", c.name.c_str()), 28 * scale, 24 * scale, 26, WHITE);
        text(mono, TextFormat("LAP  %d/%d", std::min(c.lapsDone + 1, race.laps()), race.laps()), 28 * scale, 56 * scale, 20, RAYWHITE);
        text(mono, TextFormat("NOW  %s", lapTime(race.time() - c.lapStart).c_str()), 28 * scale, 80 * scale, 20, RAYWHITE);
        text(mono, TextFormat("LAST %s", lapTime(c.lapTimes.empty() ? 0 : c.lapTimes.back()).c_str()), 28 * scale, 104 * scale, 20, RAYWHITE);
        text(mono, TextFormat("BEST %s", lapTime(c.bestLap).c_str()), 188 * scale, 104 * scale, 20, Color{180, 120, 255, 255});
        // speed, gear, revs and pedals, bottom right
        const float bx = W - 300 * scale, by = H - 150 * scale;
        panel((int)bx, (int)by, (int)(284 * scale), (int)(134 * scale));
        text(font, TextFormat("%d", (int)std::lround(std::fabs(c.state.vx) * 3.6f)), bx + 16 * scale, by + 10 * scale, 56, WHITE);
        text(font, "km/h", bx + 132 * scale, by + 40 * scale, 18, LIGHTGRAY);
        text(font, TextFormat("%d", c.state.gear), bx + 222 * scale, by + 10 * scale, 56, Color{255, 200, 40, 255});
        float rpmFrac = std::clamp(c.state.rpm / c.phys.maxRpm, 0.0f, 1.0f);
        DrawRectangle((int)(bx + 16 * scale), (int)(by + 78 * scale), (int)(252 * scale), (int)(8 * scale), Color{60, 60, 66, 255});
        DrawRectangle((int)(bx + 16 * scale), (int)(by + 78 * scale), (int)(252 * scale * rpmFrac), (int)(8 * scale),
                      rpmFrac > 0.95f ? RED : (rpmFrac > 0.85f ? ORANGE : Color{80, 200, 120, 255}));
        DrawRectangle((int)(bx + 16 * scale), (int)(by + 96 * scale), (int)(120 * scale * c.control.accel), (int)(10 * scale), Color{60, 200, 90, 255});
        DrawRectangle((int)(bx + 148 * scale), (int)(by + 96 * scale), (int)(120 * scale * c.control.brake), (int)(10 * scale), Color{230, 60, 50, 255});
        float steer = std::clamp(c.control.steer, -1.0f, 1.0f);
        DrawRectangle((int)(bx + 142 * scale + std::min(0.0f, -steer) * 126 * scale), (int)(by + 114 * scale),
                      (int)(std::fabs(steer) * 126 * scale), (int)(6 * scale), Color{90, 160, 255, 255});
        // camera, robot status and frame rate, bottom left
        panel((int)(16 * scale), (int)(H - 70 * scale), (int)(560 * scale), (int)(54 * scale));
        text(font, TextFormat("%s   %s", kCamNames[rig.mode], c.control.status), 28 * scale, H - 64 * scale, 18, WHITE);
        text(mono, TextFormat("%.0f fps  %s   1-6 camera  [ ] speed  space pause", fps, gpuName.c_str()), 28 * scale, H - 40 * scale, 14,
             LIGHTGRAY);
    };

    if (!shotsDir.empty()) {
        // drive and photograph: several moments of a lap from several cameras
        fs::create_directories(shotsDir);
        struct Moment { float t; Cam cam; const char* name; };
        const Moment moments[] = {{4, CHASE, "01_start_chase"}, {9, ORBIT, "02_orbit"},   {14, TCAM, "03_tcam"},
                                  {19, CHASE, "04_chase"},     {24, TV, "05_tv"},          {30, HELI, "06_heli"},
                                  {36, ORBIT, "07_orbit_corner"}, {42, NOSE, "08_nose"},   {50, CHASE, "09_chase_hill"},
                                  {58, TV, "10_tv"}};
        RenderTexture2D out = LoadRenderTexture(width, height);
        double t = 0;
        for (const Moment& mo : moments) {
            while (t < mo.t) {
                race.advance(1.0 / 60);
                t += 1.0 / 60;
                car.update(*me, track, scene, 1.0f / 60);
                rig.mode = mo.cam;
                rig.update(car, scene, track, me->trackS, 1.0f / 60);
            }
            Camera3D cam = rig.update(car, scene, track, me->trackS, 1.0f / 60);
            render(cam, true);
            BeginTextureMode(out);
            ClearBackground(BLACK);
            gr.present(0, 0, width, height);
            hud(width, height, 0);
            EndTextureMode();
            Image img = LoadImageFromTexture(out.texture);
            ImageFlipVertical(&img);
            std::string path = shotsDir + "/" + mo.name + ".png";
            ExportImage(img, path.c_str());
            UnloadImage(img);
            std::printf("wrote %s (t=%.0f s, %.0f km/h, s=%.0f m)\n", path.c_str(), race.time(), me->state.vx * 3.6f, me->trackS);
        }
        UnloadRenderTexture(out);
    } else {
        const char* skies[] = {"kloofendal_partly_cloudy", "mud_road", "overcast_soil"};
        int skyIdx = 0;
        float timeScale = 1.0f;
        bool paused = false;
        int frames = 0;
        double benchTime = 0;
        std::vector<float> frameTimes;
        while (!WindowShouldClose()) {
            float dt = std::min(GetFrameTime(), 0.05f);
            if (IsWindowResized()) gr.resize(GetScreenWidth(), GetScreenHeight());
            for (int k = 0; k < CAM_COUNT; ++k)
                if (IsKeyPressed(KEY_ONE + k)) rig.mode = (Cam)k, rig.init = false;
            if (IsKeyPressed(KEY_C)) rig.mode = (Cam)((rig.mode + 1) % CAM_COUNT), rig.init = false;
            if (IsKeyPressed(KEY_SPACE)) paused = !paused;
            if (IsKeyPressed(KEY_LEFT_BRACKET)) timeScale = std::max(0.125f, timeScale * 0.5f);
            if (IsKeyPressed(KEY_RIGHT_BRACKET)) timeScale = std::min(8.0f, timeScale * 2.0f);
            if (IsKeyPressed(KEY_F2)) {
                skyIdx = (skyIdx + 1) % 3;
                if (!gr.loadSky(assets + "/sky", skies[skyIdx], &err)) std::fprintf(stderr, "%s\n", err.c_str());
            }
            if (!paused && !race.isOver()) race.advance(dt * timeScale);
            car.update(*me, track, scene, paused ? 0.0f : dt * timeScale);
            Camera3D cam = rig.update(car, scene, track, me->trackS, dt);
            render(cam, false);
            BeginDrawing();
            ClearBackground(BLACK);
            gr.present(0, 0, GetScreenWidth(), GetScreenHeight());
            hud(GetScreenWidth(), GetScreenHeight(), GetFPS());
            if (timeScale != 1.0f || paused)
                DrawTextEx(font, paused ? "PAUSED" : TextFormat("x%g", timeScale), {GetScreenWidth() / 2.0f - 40, 20}, 30, 1, YELLOW);
            EndDrawing();
            if (IsKeyPressed(KEY_F12)) {
                static int n = 0;
                TakeScreenshot(TextFormat("viewer2_%03d.png", n++));
            }
            if (bench) {
                if (frames > 30) {
                    benchTime += GetFrameTime();
                    frameTimes.push_back(GetFrameTime());
                }
                if (++frames == 1230) {
                    std::sort(frameTimes.begin(), frameTimes.end());
                    std::printf("bench (%s camera): %.2f ms a frame (%.0f fps), 1%% low %.0f fps, at %dx%d on %s\n", kCamNames[rig.mode],
                                benchTime / frameTimes.size() * 1000, frameTimes.size() / benchTime,
                                1.0 / frameTimes[frameTimes.size() * 99 / 100], GetScreenWidth(), GetScreenHeight(), gpuName.c_str());
                    break;
                }
            }
        }
    }
    UnloadFont(font);
    UnloadFont(mono);
    car.unload();
    scene.unload();
    gr.shutdown();
    CloseWindow();
    return 0;
}
