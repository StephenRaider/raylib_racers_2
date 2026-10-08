// rr_trackview: RR2's track renderer on its own. Flies round a track's 3D road and
// terrain with PBR materials, an HDRI sky and a shadowing sun; no race, no cars yet.
//
//   rr_trackview [--track highmoor] [--sky kloofendal_partly_cloudy] [--yaw deg]
//                [--shots DIR] [--width W --height H] [--ssaa N]
//
// Keys: 1 drive, 2 chase, 3 free fly (WASD, Q/E, right mouse to look), 4 orbit; Space pauses;
// +/- speed; [ ] turns the sky; , . exposure; F2 next sky; F12 screenshot.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "gfx.hpp"
#include "raylib.h"
#include "raymath.h"
#include "track.hpp"
#include "track_scene.hpp"

namespace fs = std::filesystem;

namespace {

std::string findTrack(const std::string& t, const std::vector<std::string>& dirs) {
    std::error_code ec;
    if (fs::exists(t, ec) && !fs::is_directory(t, ec)) return t;
    for (const auto& d : dirs)
        for (const auto& cand : {t, t + ".trk"}) {
            fs::path p = fs::path(d) / cand;
            if (fs::exists(p, ec) && !fs::is_directory(p, ec)) return p.string();
        }
    return "";
}

enum Mode { DRIVE, CHASE, FREE, ORBIT };

// A camera riding along the track: eye a little above the road, looking ahead along it.
Camera3D driveCam(const TrackScene& sc, const rr::Track& tr, float s, float lateral, float height, float ahead) {
    Camera3D c{};
    c.position = sc.roadPoint(s, lateral, height);
    Vector3 t = sc.roadPoint(s + ahead, lateral * 0.6f, height * 0.8f);
    c.target = t;
    // lean with the banking
    const auto& smp = tr.at(tr.indexAt(s));
    Vector3 fwd = Vector3Normalize(Vector3Subtract(t, c.position));
    Vector3 left = {smp.n.x, 0, -smp.n.y};
    c.up = Vector3Normalize(Vector3Add({0, 1, 0}, Vector3Scale(left, -std::sin(smp.bank) * 0.8f)));
    (void)fwd;
    c.fovy = 62;
    c.projection = CAMERA_PERSPECTIVE;
    return c;
}

// A screenshot: eye and target on the road, each at a distance from a turn's apex (turn 0: the
// start line), a lateral offset and a height; or, with `world` set, the eye at that offset from
// the target.
struct Shot {
    const char* name;
    int turn;
    float dsEye, latEye, upEye;
    float dsTgt, latTgt, upTgt;
    float fov;
    Vector3 world = {0, 0, 0};
};

}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::string trackName = "highmoor", skyName = "kloofendal_partly_cloudy", shotsDir;
    int width = 1600, height = 900, ssaa = 1;
    float yawDeg = 0;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
        if (a == "--track") trackName = next();
        else if (a == "--sky") skyName = next();
        else if (a == "--shots") shotsDir = next();
        else if (a == "--width") width = std::atoi(next().c_str());
        else if (a == "--height") height = std::atoi(next().c_str());
        else if (a == "--ssaa") ssaa = std::max(1, std::atoi(next().c_str()));
        else if (a == "--yaw") yawDeg = (float)std::atof(next().c_str());
        else {
            std::fprintf(stderr, "unknown option %s\n", a.c_str());
            return 2;
        }
    }
    const std::string exeDir = GetApplicationDirectory();
    std::string assets;
    for (const std::string& d : {exeDir + "/assets", std::string(RR_SOURCE_DIR "/assets"), std::string("assets")})
        if (fs::exists(d + "/materials")) { assets = d; break; }
    if (assets.empty()) {
        std::fprintf(stderr, "cannot find the assets folder (assets/materials)\n");
        return 1;
    }
    std::string trackFile = findTrack(trackName, {exeDir + "/tracks", RR_SOURCE_DIR "/tracks", "tracks"});
    rr::Track track;
    std::string err;
    if (trackFile.empty() || !track.load(trackFile, &err)) {
        std::fprintf(stderr, "cannot load track %s: %s\n", trackName.c_str(), err.c_str());
        return 1;
    }
    for (const auto& w : track.warnings()) std::fprintf(stderr, "track: %s\n", w.c_str());
    for (const auto& t : track.turns())
        std::printf("T%-2d %-5s apex %5.0f m (%5.0f-%5.0f)  r %4.0f m  %3.0f deg  h %4.1f m\n", t.id,
                    t.direction > 0 ? "left" : "right", t.apex_s, t.start_s, t.end_s, t.min_radius, t.angle * RAD2DEG,
                    track.heightAt(t.apex_s));

    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_VSYNC_HINT | (shotsDir.empty() ? 0 : FLAG_WINDOW_HIDDEN));
    InitWindow(shotsDir.empty() ? width : 320, shotsDir.empty() ? height : 180, "Raylib Racers 2 - track view");
    SetTraceLogLevel(LOG_WARNING);
    std::fprintf(stderr, "window up\n");
    gfx::Renderer gr;
    if (!gr.init(width * ssaa, height * ssaa, &err) || !gr.loadSky(assets + "/sky", skyName, &err)) {
        std::fprintf(stderr, "%s\n", err.c_str());
        CloseWindow();
        return 1;
    }
    gr.skyYaw = yawDeg * DEG2RAD;
    std::fprintf(stderr, "renderer up\n");
    TrackScene scene;
    double t0 = GetTime();
    if (!scene.build(track, assets, 1, &err)) {
        std::fprintf(stderr, "%s\n", err.c_str());
        CloseWindow();
        return 1;
    }
    std::printf("%s: %.2f km, scene built in %.1f s\n", track.name().c_str(), track.length() / 1000, GetTime() - t0);

    auto render = [&](const Camera3D& cam) {
        Vector3 fwd = Vector3Normalize(Vector3Subtract(cam.target, cam.position));
        gr.beginShadows(Vector3Add(cam.position, Vector3Scale(fwd, 90.0f)), 140.0f);
        scene.drawShadows(gr);
        gr.endShadows();
        gr.beginScene(cam);
        scene.draw(gr);
        gr.drawSky();
        gr.endScene();
    };

    if (!shotsDir.empty()) {
        fs::create_directories(shotsDir);
        auto apex = [&](int id) {
            for (const auto& tn : track.turns())
                if (tn.id == id) return tn.apex_s;
            return 0.0f;
        };
        const Shot shots[] = {
            {"01_grid", 0, -60, -3.5f, 1.1f, 60, -1.0f, 1.0f, 62},
            {"02_plunge_approach", 1, -230, 2.5f, 1.1f, -40, -1.0f, 0.5f, 55},
            {"03_plunge_aerial", 1, 0, 0, 0, 0, 0, 0, 55, {-55, 70, -40}},
            {"04_ridge", 5, -170, -2.0f, 1.1f, -60, 0.0f, 1.0f, 60},
            {"05_summit_hairpin", 6, 0, 0, 0, 0, 0, 0, 50, {-45, 22, 45}},
            {"06_corkscrew_top", 7, 10, 0.0f, 1.1f, 60, 0.0f, -2.0f, 60},
            {"07_corkscrew_from_below", 8, 60, -14.0f, 1.6f, -110, 0.0f, 1.0f, 45},
            {"08_valley_hairpin", 9, -150, 0.0f, 1.1f, -20, 0.0f, 0.5f, 60},
            {"09_highmoor_climb", 13, -40, 3.0f, 1.1f, 100, 0.0f, 1.0f, 60},
            {"10_aerial", 7, 0, 0, 0, 0, 0, 0, 50, {-330, 240, 380}},
        };
        RenderTexture2D out = LoadRenderTexture(width, height);
        for (const Shot& sh : shots) {
            Camera3D cam{};
            float base = sh.turn ? apex(sh.turn) : 0.0f;
            cam.target = scene.roadPoint(base + sh.dsTgt, sh.latTgt, sh.upTgt);
            if (sh.world.x != 0 || sh.world.y != 0 || sh.world.z != 0) cam.position = Vector3Add(cam.target, sh.world);
            else cam.position = scene.roadPoint(base + sh.dsEye, sh.latEye, sh.upEye);
            cam.position.y = std::max(cam.position.y, scene.groundHeight(cam.position.x, cam.position.z) + 1.0f);
            cam.up = {0, 1, 0};
            cam.projection = CAMERA_PERSPECTIVE;
            cam.fovy = sh.fov;
            render(cam);
            BeginTextureMode(out);
            ClearBackground(BLACK);
            gr.present(0, 0, width, height);
            EndTextureMode();
            Image img = LoadImageFromTexture(out.texture);
            ImageFlipVertical(&img);
            std::string path = shotsDir + "/" + sh.name + ".png";
            ExportImage(img, path.c_str());
            UnloadImage(img);
            std::printf("wrote %s\n", path.c_str());
        }
        UnloadRenderTexture(out);
        scene.unload();
        gr.shutdown();
        CloseWindow();
        return 0;
    }

    const char* skies[] = {"kloofendal_partly_cloudy", "mud_road", "overcast_soil"};
    int skyIdx = 0;
    for (int i = 0; i < 3; ++i)
        if (skyName == skies[i]) skyIdx = i;
    Mode mode = DRIVE;
    float s = 0, speed = 45.0f;
    bool paused = false;
    Camera3D free{};
    free.position = Vector3Add(scene.centre(), {0, 120, scene.extent() * 0.6f});
    free.target = scene.centre();
    free.up = {0, 1, 0};
    free.fovy = 60;
    float yaw = PI, pitch = -0.3f, orbit = 0;
    while (!WindowShouldClose()) {
        float dt = GetFrameTime();
        if (IsWindowResized()) gr.resize(GetScreenWidth() * ssaa, GetScreenHeight() * ssaa);
        if (IsKeyPressed(KEY_ONE)) mode = DRIVE;
        if (IsKeyPressed(KEY_TWO)) mode = CHASE;
        if (IsKeyPressed(KEY_THREE)) mode = FREE;
        if (IsKeyPressed(KEY_FOUR)) mode = ORBIT;
        if (IsKeyPressed(KEY_SPACE)) paused = !paused;
        if (IsKeyDown(KEY_EQUAL) || IsKeyDown(KEY_KP_ADD)) speed = std::min(speed + 30 * dt, 120.0f);
        if (IsKeyDown(KEY_MINUS) || IsKeyDown(KEY_KP_SUBTRACT)) speed = std::max(speed - 30 * dt, 5.0f);
        if (IsKeyDown(KEY_LEFT_BRACKET)) gr.skyYaw -= dt * 0.5f;
        if (IsKeyDown(KEY_RIGHT_BRACKET)) gr.skyYaw += dt * 0.5f;
        if (IsKeyDown(KEY_COMMA)) gr.exposure *= 1.0f - dt;
        if (IsKeyDown(KEY_PERIOD)) gr.exposure *= 1.0f + dt;
        if (IsKeyPressed(KEY_F2)) {
            skyIdx = (skyIdx + 1) % 3;
            if (!gr.loadSky(assets + "/sky", skies[skyIdx], &err)) std::fprintf(stderr, "%s\n", err.c_str());
        }
        if (!paused) s = std::fmod(s + speed * dt, track.length());

        Camera3D cam;
        if (mode == DRIVE) {
            cam = driveCam(scene, track, s, 0, 1.1f, 30);
        } else if (mode == CHASE) {
            Vector3 car = scene.roadPoint(s, 0, 0.6f);
            Vector3 back = scene.roadPoint(s - 9, 0, 3.2f);
            cam = Camera3D{back, car, {0, 1, 0}, 60, CAMERA_PERSPECTIVE};
        } else if (mode == ORBIT) {
            orbit += dt * 0.05f;
            Vector3 c = scene.centre();
            float r = scene.extent() * 0.75f;
            cam = Camera3D{{c.x + std::cos(orbit) * r, c.y + r * 0.35f, c.z + std::sin(orbit) * r}, c, {0, 1, 0}, 50, CAMERA_PERSPECTIVE};
        } else {
            if (IsMouseButtonDown(MOUSE_BUTTON_RIGHT)) {
                Vector2 d = GetMouseDelta();
                yaw -= d.x * 0.003f;
                pitch = std::clamp(pitch - d.y * 0.003f, -1.5f, 1.5f);
            }
            Vector3 fwd = {std::cos(pitch) * std::sin(yaw), std::sin(pitch), std::cos(pitch) * std::cos(yaw)};
            Vector3 right = Vector3Normalize(Vector3CrossProduct(fwd, {0, 1, 0}));
            float v = (IsKeyDown(KEY_LEFT_SHIFT) ? 120.0f : 30.0f) * dt;
            if (IsKeyDown(KEY_W)) free.position = Vector3Add(free.position, Vector3Scale(fwd, v));
            if (IsKeyDown(KEY_S)) free.position = Vector3Subtract(free.position, Vector3Scale(fwd, v));
            if (IsKeyDown(KEY_D)) free.position = Vector3Add(free.position, Vector3Scale(right, v));
            if (IsKeyDown(KEY_A)) free.position = Vector3Subtract(free.position, Vector3Scale(right, v));
            if (IsKeyDown(KEY_E)) free.position.y += v;
            if (IsKeyDown(KEY_Q)) free.position.y -= v;
            free.position.y = std::max(free.position.y, scene.groundHeight(free.position.x, free.position.z) + 0.5f);
            free.target = Vector3Add(free.position, fwd);
            cam = free;
        }
        render(cam);
        BeginDrawing();
        ClearBackground(BLACK);
        gr.present(0, 0, GetScreenWidth(), GetScreenHeight());
        DrawText(TextFormat("%s  %.0f m  %.0f km/h  %d fps   [1-4 camera, F2 sky, F12 shot]", track.name().c_str(), s,
                            speed * 3.6f, GetFPS()),
                 12, 10, 18, RAYWHITE);
        EndDrawing();
        if (IsKeyPressed(KEY_F12)) {
            static int n = 0;
            TakeScreenshot(TextFormat("trackview_%03d.png", n++));
        }
    }
    scene.unload();
    gr.shutdown();
    CloseWindow();
    return 0;
}
