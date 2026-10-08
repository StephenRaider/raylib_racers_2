#pragma once
#ifdef RR2_RENDERER
#include "../viewer2/renderer2.hpp"  // Raylib Racers 2: the same interface on the PBR renderer
#else
#include <string>
#include <vector>

#include "car_model.hpp"
#include "effects.hpp"
#include "raylib.h"
#include "race.hpp"

// Follow, cinematic, trackside TV, helicopter, top-down, free orbit, whole-track overview.
// CAM_DIRECTOR is the viewer's TV director picking cars and shots; the renderer only sees the shot it picked.
enum CamMode { CAM_CHASE = 0, CAM_CINEMATIC, CAM_TV, CAM_HELI, CAM_TOP, CAM_ORBIT, CAM_OVERVIEW, CAM_DIRECTOR, CAM_COUNT };
const char* camName(CamMode mode);

struct ViewOptions {
    bool showPaths = true;    // robots' debug_path polylines
    bool showSensors = false; // focused car's range finders
    int quality = 2;          // 0 low (plain), 1 medium (no particles), 2 high
};

Color teamColor(int carIndex);
Color teamAccent(int carIndex);

// 3D scene: track, scenery and cars, lit by a sun with shadow mapping and fog.
class Renderer {
public:
    // The F1 car comes from assetsDir/cars/f1_gearari; without it, cars are drawn from boxes.
    bool init(const rr::Track& track, unsigned seed, const std::string& assetsDir, std::string* err);
    void shutdown();

    void updateCamera(const rr::Race& race, int focus, CamMode mode, float dt);
    void draw(const rr::Race& race, int focus, const ViewOptions& opt);
    void stepEffects(const rr::Race& race, float dt) { fx_.update(race, dt); }

    Camera3D camera{};

private:
    void buildTrack(const rr::Track& track);
    void buildScenery(const rr::Track& track, unsigned seed);  // scenery.cpp
    void drawProps(bool shadowPass);
    void drawTrees(bool shadowPass);  // scenery.cpp
    void drawScene(const rr::Race& race, bool shadowPass);
    void drawCar(const rr::Car& car, int index);
    void drawBoxCar(const rr::Car& car, int index);
    void resetCinematic(float clock);
    void drawModel(Model& m, Matrix transform, Color tint);
    void applyQuality(int quality);
    Effects fx_;
    int quality_ = -1;
    void drawBox(Vector3 center, Vector3 size, Color color, Matrix parent);

    Shader lit_{};
    Shader depth_{};
    Shader* current_ = nullptr;
    Shader litInst_{}, depthInst_{};  // instanced versions, for the trees
    int locLightVP_ = -1, locShadowMap_ = -1, locViewPos_ = -1, locSpec_ = -1, locFog_ = -1;
    RenderTexture2D shadowMap_{};
    const int shadowRes_ = 2048;
    Matrix lightVP_{};

    Texture2D texAsphalt_{}, texGrass_{}, texChecker_{}, texWhite_{};
    Model mdlAsphalt_{}, mdlMarkings_{}, mdlWalls_{}, mdlGround_{}, mdlStart_{};
    Texture2D texMask_{};   // grass mask: black on the track
    Model mdlShell_{};      // unit plane for the grass shells
    Vector4 maskRect_{};
    void drawGrass();
    Model mdlCube_{}, mdlWheel_{}, mdlSphere_{}, mdlCone_{}, mdlTrunk_{}, mdlPyramid_{};
    CarModel carModel_;

    // Static scenery, each prop one shape (or a tree made of a few). Boxes and spheres are
    // centred on pos; cylinders, cones and pyramids stand on it. size is (along, up, across)
    // before the prop is tilted about its long axis and turned by yaw.
    enum PropKind : unsigned char { P_BOX, P_CYL, P_CONE, P_SPHERE, P_PYRAMID, P_ROOF, P_MOUND };
    struct Prop {
        PropKind kind;
        Vector3 pos, size;
        float yaw = 0, tilt = 0;
        Color color{200, 200, 200, 255};
        float radius = 1;  // for culling
    };
    std::vector<Prop> props_;
    // Trees: instanced parts, grouped in square tiles so whole tiles are culled at once.
    enum TreePart { TP_TRUNK, TP_CONE, TP_BLOB, TP_FROND, TP_BLOB2, TP_COUNT };  // BLOB2: a second, smaller blob
    struct TreeTile {
        Vector3 centre{};
        float radius = 0;
        std::vector<std::vector<Matrix>> parts;  // per slot; colour in m3, m7, m11
    };
    std::vector<TreeTile> treeTiles_;
    std::vector<std::vector<Matrix>> treeBatch_;  // per-frame gather of the visible tiles
    Mesh treeMesh_[TP_COUNT]{};
    Material treeMat_{};
    // Trees from assets/scenery/trees (textured, leaf cut-outs). Each part of each model is
    // one instancing slot after the TP_COUNT procedural ones.
    struct TreeModel {
        std::string kind;  // tree, broadleaf, bush
        std::vector<int> slots;
    };
    std::vector<TreeModel> treeModels_;
    std::vector<Mesh> slotMesh_;            // model slots only (index slot - TP_COUNT)
    std::vector<Texture2D> slotTex_;
    std::vector<Texture2D> treeTextures_;   // owned
    void loadTrees(const std::string& assetsDir);  // scenery.cpp
    int treeSlots() const { return TP_COUNT + (int)slotMesh_.size(); }
    std::string theme_;  // the track's scenery theme
    std::vector<Vector3> tvSpots_;
    Vector3 shadowCentre_{};
    float shadowRadius_ = 400;

    // camera state
    Vector3 chasePos_{}, chaseTarget_{};  // smoothed offsets from the car (so speed adds no lag)
    bool chaseInit_ = false;
    int lastFocus_ = -1;
    CamMode lastMode_ = CAM_COUNT;
    Vector3 smoothDir_{1, 0, 0};
    // cinematic: the camera eases between keyframed spots around the car, each held for a few seconds
    struct CineKey { float azimuth, height, dist, fov, hold; };
    CineKey cineFrom_{}, cineTo_{};
    float cineClock_ = 0, cineSegStart_ = 0;
    unsigned cineSeed_ = 0;
    float heliYaw_ = 0, heliDist_ = 60.0f, topHeight_ = 105.0f;
    float orbitYaw_ = 0.6f, orbitPitch_ = 0.35f, orbitDist_ = 14.0f;
    Vector3 trackCenter_{};
    float trackExtent_ = 500;
};

#endif  // RR2_RENDERER
