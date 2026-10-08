#pragma once
// The 3D track: road, kerbs, paint, barriers, pit lane and the terrain around them,
// built from an rr::Track (its heights and banking) and drawn with gfx::Renderer.
#include <string>
#include <vector>

#include "gfx.hpp"
#include "track.hpp"

class TrackScene {
public:
    bool build(const rr::Track& track, const std::string& assetsDir, unsigned seed, std::string* err);
    void unload();

    void drawShadows(gfx::Renderer& r) const;
    void draw(gfx::Renderer& r) const;

    // World position (y up) of the road surface at track distance s, lateral offset (+ left), plus up.
    Vector3 roadPoint(float s, float lateral, float up = 0) const;
    // Terrain (or road) height under a world x, z.
    float groundHeight(float x, float z) const;
    Vector3 centre() const { return centre_; }
    float extent() const { return extent_; }

private:
    const rr::Track* tr_ = nullptr;
    struct Part {
        std::vector<Mesh> meshes;
        gfx::Material mat;
        bool shadows = true;
    };
    std::vector<Part> parts_;
    gfx::TextureSet asphalt_, asphaltWorn_, concrete_, grass_, gravel_, dirt_, metal_;

    // terrain heights, for groundHeight(): a fine grid near the track and a coarse one beyond
    struct Grid {
        float x0 = 0, z0 = 0, step = 1;
        int nx = 0, nz = 0;
        std::vector<float> h;
        bool inside(float x, float z) const;
        float at(float x, float z) const;  // bilinear
    };
    Grid nearGrid_, farGrid_;
    Vector3 centre_{};
    float extent_ = 500;

    // nearest centreline sample to a plan-view point (spatial hash over the samples)
    float cell_ = 24.0f;
    float hx0_ = 0, hy0_ = 0;
    int hw_ = 0, hh_ = 0;
    std::vector<std::vector<int>> hash_;
    int nearest(rr::Vec2 p, float maxDist, float* dist) const;

    void buildRoad(unsigned seed);
    void buildTerrain(unsigned seed);
    std::vector<float> gravelSide_;  // per sample: + gravel trap on the left, - on the right, 0 none
};
