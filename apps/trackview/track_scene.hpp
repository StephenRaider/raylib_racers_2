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
        std::vector<BoundingBox> boxes;  // one per mesh, for culling
        std::vector<Mesh> lod;           // optional coarser copies of meshes, used past lodDist
        float lodDist = 0;
        gfx::Material mat;
        bool shadows = true;
        unsigned alphaTex = 0;           // cut-out shadows (leaves)
    };
    std::vector<Part> parts_;
    gfx::TextureSet asphalt_, asphaltWorn_, concrete_, grass_, gravel_, dirt_, metal_;
    Texture2D treeAtlas_{};
    gfx::TextureSet foliage_, rubber_, chain_;
    Texture2D colorNear_{}, colorFar_{};  // baked terrain colour for the distance

    // Trees are instanced: one mesh per kind of tree in two levels of detail (the whole
    // tree up close, only its big cards further out), placed per 96 m tile so whole
    // tiles cull together. Instance matrices are gathered and uploaded each pass.
    struct TreeType {
        Mesh lod[2]{};
        bool bush = false;
        int total = 0;
        unsigned vbo[3][2] = {};  // per pass (colour, near shadows, far shadows) and level of detail
    };
    std::vector<TreeType> treeTypes_;
    struct TreeTile {
        BoundingBox box{};
        std::vector<std::vector<Matrix>> inst;  // per tree type
    };
    std::vector<TreeTile> treeTiles_;
    gfx::Material treeMat_;
    mutable std::vector<Matrix> gather_[2];
    void drawTrees(gfx::Renderer& r, int pass) const;
    const Mesh& pick(const Part& p, size_t i, Vector3 cam, bool coarse) const;

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
    void buildTrees(const std::string& assetsDir, unsigned seed);
    void buildProps(const std::string& assetsDir);
    // The paddock: pit garages along the pit lane, a grandstand across from them.
    bool garagesAt(float s) const;     // pit side, along the pit lane
    bool grandstandAt(float s) const;  // the other side of the pit straight
    // How far out (lateral, from the centreline) the ground stays level with the track
    // before blending into the land: the barrier, or the paddock and grandstand behind it.
    float flatUntil(float s, int side, float halfWidth) const;
    // 0..1: how much a plan-view point lies in a conifer plantation (before keeping clear of the track)
    float plantation(rr::Vec2 p) const;
    unsigned seed_ = 1;
};
