#pragma once
// A car of the 2013 pack (assets/cars/<id>: body.glb, wheel_front.glb, wheel_rear.glb,
// car.json) drawn with the PBR renderer: clear-coat paint, rubber tyres, metal rims,
// front wheels that steer, and a body that rolls, pitches and heaves on its springs.
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "gfx.hpp"
#include "race.hpp"
#include "raymath.h"

class TrackScene;

class CarRender {
public:
    struct Finish {           // the paint
        float metalness = 0.0f, roughness = 0.3f, clearcoat = 1.0f, clearcoatRoughness = 0.03f;
    };
    bool load(const std::string& dir, const Finish& paint, std::string* err);
    // Another car of the same model: shares this one's meshes and textures, has its own suspension.
    void shareFrom(const CarRender& src);
    bool isOwner() const { return owner_; }
    void unload();

    // Follows the car: wheels on the road, the body on its suspension. dt in seconds.
    void update(const rr::Car& car, const rr::Track& track, const TrackScene& scene, float dt);
    // The DRS flap (car.json "drs"): 0 closed, 1 fully open; it turns about its hinge by
    // up to drs.max_angle_deg. update() sets it from the simulated flap (CarState::drsFlap), so this is
    // only for a viewer without a race.
    void setDrsOpen(float open) { drsOpen_ = open < 0 ? 0 : open > 1 ? 1 : open; }
    bool hasDrs() const { return !drs_parts_.empty(); }
    // The team colour: the body's paint in this colour, parts that are dark in the model's own paint
    // (floor, diffuser, cockpit housing, shoulder covers) plain black (livery_default.png and livery_mask.png in the model's
    // folder). Does nothing without them, or when the folder has a livery.png of its own.
    void setPaint(Color c);
    void clearPaint();   // back to the model's own livery
    // A painted livery sheet (an opaque 2048 x 2048 PNG on the model's UV layout) in place of the model's
    // paint. The textures are cached by file in the car that owns the meshes. False if the file won't load.
    bool setLiveryFile(const std::string& file);
    // The tyre compound fitted (RR_TIRE_*, update() follows the car's): the lettering on the sidewall is
    // red on the softs, yellow on the mediums, white on the hards, green on intermediates (4) and blue
    // on wets (5), which also have a grooved tread.
    static int tyreLook(int compound);
    void draw(gfx::Renderer& r) const;
    void drawShadow(gfx::Renderer& r) const;

    // The body's frame (after suspension): origin on the ground under the middle of the
    // wheelbase, x to the car's left, y up, z forward.
    Matrix body() const { return body_; }
    Vector3 origin() const { return {body_.m12, body_.m13, body_.m14}; }
    Vector3 forward() const { return Vector3Normalize({body_.m8, body_.m9, body_.m10}); }
    Vector3 up() const { return Vector3Normalize({body_.m4, body_.m5, body_.m6}); }
    Vector3 left() const { return Vector3Normalize({body_.m0, body_.m1, body_.m2}); }
    float wheelbase() const { return wheelbase_; }

private:
    struct Part {
        Mesh mesh{};
        gfx::Material mat;
        const gfx::TextureSet* set = nullptr;
        bool livery = false;            // wears the team paint
        unsigned char tyre = 0;         // wheel parts: 1 the sidewall (lettering), 2 the tread
    };
    std::vector<Part> body_parts_;
    std::vector<Part> steer_parts_;     // the steering wheel, turned about its column
    Vector3 steerHub_{-0.028f, 0.50f, 0.61f};  // body frame
    Vector3 steerAxis_{0.0f, 0.2f, 1.0f};
    Matrix steerM_ = MatrixIdentity();
    std::vector<Part> drs_parts_;       // the rear wing's flap, turned about its hinge
    Vector3 drsPivot_{0, 0, 0};         // body frame
    Vector3 drsAxis_{1, 0, 0};
    float drsMax_ = 0, drsOpen_ = 0;
    Matrix drsM_ = MatrixIdentity();
    std::vector<Part> wheel_parts_[2];  // front, rear (left-side wheel)
    std::vector<gfx::TextureSet*> sets_;
    std::vector<Model> models_;         // keep the glTF textures alive
    std::string liveryFile_;            // dir/livery.png, if there is one
    Texture2D liveryTex_{};
    // team paints: the model's paint and mask, and a texture set per colour made from them
    CarRender* origin_ = this;          // the car that owns the meshes (and these)
    Image paintBase_{}, paintMask_{};
    std::map<uint32_t, gfx::TextureSet*> paintSets_;
    gfx::TextureSet* paintSet(Color c);
    std::map<std::string, gfx::TextureSet*> fileSets_;   // livery sheets by file
    gfx::TextureSet* fileSet(const std::string& file);
    // tyre looks: 0 as modelled (yellow lettering), 1 soft, 2 hard, 3 inter, 4 wet
    int look_ = -1;
    Image sideImg_[2]{};                // each wheel model's sidewall texture, read back once (owner)
    std::map<int, gfx::TextureSet*> tyreSets_;
    std::vector<Texture2D> tyreTex_;    // what those sets own
    gfx::TextureSet* tyreSet(int kind, int role, int look, const gfx::TextureSet* base);
    void setLook(int look);

    struct Wheel { Vector3 hub; float radius; bool front, mirrored; };
    Wheel wheels_[4]{};                 // FL, FR, RL, RR
    float wheelbase_ = 3.45f, track_ = 1.45f;

    Matrix body_ = MatrixIdentity();
    Matrix wheel_[4]{};
    // suspension: body pitch (+ nose down), roll (+ left side up), heave (m, + up),
    // each a damped spring chasing what the loads ask for
    float pitch_ = 0, roll_ = 0, heave_ = 0, vp_ = 0, vr_ = 0, vh_ = 0;
    bool first_ = true;
    bool owner_ = true;  // false: the meshes belong to the car this one shares them with
    int hint_ = 0;

    bool loadPart(const std::string& file, const Finish& paint, int liveryMaterial, std::vector<Part>* out, bool wheel,
                  float wheelRadius, std::string* err, std::vector<Part>* steerOut = nullptr, int steerMaterial = -1);
};
