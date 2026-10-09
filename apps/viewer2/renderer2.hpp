#pragma once
// RR2's drop-in for the RR1 viewer's Renderer (apps/viewer/renderer.hpp includes this when built
// with RR2_RENDERER): the same few calls, but the picture comes from the PBR renderer, the 3D
// track and the 2013 cars. The menu, HUD, testing screens, director and sound are RR1's, unchanged.
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "raylib.h"
#include "race.hpp"

// Follow, cinematic, trackside TV, helicopter, top-down, free orbit, whole-track overview.
// CAM_DIRECTOR is the viewer's TV director picking cars and shots; the renderer only sees the shot it picked.
enum CamMode { CAM_CHASE = 0, CAM_CINEMATIC, CAM_TV, CAM_HELI, CAM_TOP, CAM_ORBIT, CAM_OVERVIEW, CAM_DIRECTOR, CAM_TCAM, CAM_NOSE, CAM_COUNT };
const char* camName(CamMode mode);

struct ViewOptions {
    bool showPaths = true;    // robots' debug_path polylines (not drawn by this renderer yet)
    bool showSensors = false; // focused car's range finders (not drawn yet)
    int quality = 2;          // 0 low .. 2 high: the MSAA level and shadow detail
};

Color teamColor(int carIndex);
Color teamAccent(int carIndex);

// The loaded exhaust impulse response for the V8's sound (empty if the file is missing).
std::vector<float> loadExhaustImpulse(const std::string& assetsDir);
// The car's height above the road's datum, for sound positioning on hills (set once a renderer exists).
float carHeightForSound(const rr::Car& car);

class Renderer {
public:
    // The cars come from assetsDir/cars/f1_2013_NN; the track, materials and sky from assetsDir.
    bool init(const rr::Track& track, unsigned seed, const std::string& assetsDir, std::string* err);
    void shutdown();

    void updateCamera(const rr::Race& race, int focus, CamMode mode, float dt);
    void draw(const rr::Race& race, int focus, const ViewOptions& opt);
    void stepEffects(const rr::Race&, float) {}
    // Sky and lighting, as in rr_trackview: the next sky (and its sun), a turn of the sky about the
    // vertical (radians) and a factor on the exposure.
    void adjustLighting(bool nextSky, float turn, float exposureFactor);

    Camera3D camera{};

    Renderer();
    ~Renderer();

    struct Impl;  // (public so the sound's car-height hook can see it)

private:
    std::unique_ptr<Impl> p_;
};
