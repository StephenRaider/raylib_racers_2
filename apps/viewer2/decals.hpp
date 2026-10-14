#pragma once
// RR2's drawing of the viewer's track effects (apps/viewer/effects.*) on the PBR renderer: the rubber on the
// racing line, skid marks, tyre tracks in the grass and gravel, and the dust and clods thrown up, as lit,
// alpha-blended decals over the track and billboards in the air.
#include <vector>

#include "effects.hpp"
#include "gfx.hpp"
#include "race.hpp"

class Decals {
public:
    ~Decals() { release(); }
    void release();  // frees the meshes and textures (needs the GL context)
    // Between gfx::Renderer::beginScene and endScene, after the opaque scene.
    void draw(gfx::Renderer& gr, const Effects& fx, const rr::Race& race, Vector3 camPos);

private:
    void init();
    void buildMarks(const std::vector<Effects::Mark>& marks, std::vector<Mesh>& out);
    void buildRubber(const rr::Race& race, const rr::TrackRubber& rub);
    void unload(std::vector<Mesh>& m);

    bool ready_ = false;
    gfx::TextureSet flat_{}, puff_{};
    gfx::Material matRubber_, matMarks_, matTracks_, matDust_;
    std::vector<Mesh> skids_, tracks_;
    unsigned skidVer_ = ~0u;
    int skidAge_ = 0;
    struct Chunk { Mesh mesh; BoundingBox box; };
    std::vector<Chunk> rubber_;
    const rr::Race* race_ = nullptr;
    unsigned rubberVer_ = ~0u;
    int rubberAge_ = 0;
};
