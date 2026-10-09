#pragma once
// A car of the 2013 pack (assets/cars/<id>: body.glb, wheel_front.glb, wheel_rear.glb,
// car.json) drawn with the PBR renderer: clear-coat paint, rubber tyres, metal rims,
// front wheels that steer, and a body that rolls, pitches and heaves on its springs.
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
