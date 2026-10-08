#pragma once
// RR2's PBR renderer core: HDR render target, image-based lighting from an HDRI sky,
// a shadow-casting sun, metallic-roughness materials and ACES tone mapping.
// Nothing here knows about tracks or cars.
#include <string>
#include <vector>

#include "raylib.h"

namespace gfx {

// One PBR texture set (assets/materials/<name>/albedo.jpg, normal.jpg, orm.jpg).
struct TextureSet {
    Texture2D albedo{}, normal{}, orm{};
    bool ok() const { return albedo.id != 0; }
};
bool loadTextureSet(const std::string& dir, TextureSet* out, std::string* err);
// A set from one albedo texture (with alpha), with a flat normal map and constant roughness.
TextureSet flatTextureSet(Texture2D albedo, float roughness);
void unloadTextureSet(TextureSet& t);

// How a mesh is shaded. Up to three texture layers, mixed per vertex by the vertex
// colour's rgb (weights); the vertex alpha is baked ambient occlusion. Layers repeat
// every `scale` metres of the mesh's UVs.
struct Material {
    const TextureSet* layer[3] = {nullptr, nullptr, nullptr};
    float scale[3] = {1, 1, 1};
    Vector3 tint = {1, 1, 1};     // multiplies the albedo (linear)
    Vector3 layerTint[3] = {{1, 1, 1}, {1, 1, 1}, {1, 1, 1}};
    float roughness = 1.0f;       // multiplies the roughness map
    float metalness = 1.0f;       // multiplies the metalness map
    float normalStrength = 1.0f;
    float clearcoat = 0.0f;       // a glossy varnish layer on top (car paint)
    float clearcoatRoughness = 0.05f;
    bool macroVariation = false;  // break up texture tiling over large areas (terrain)
    bool doubleSided = false;
    float depthBias = 0.0f;       // pulls the surface towards the camera (decals over the road), ~1e-6 units
    float alphaCut = 0.0f;        // > 0: leaf cut-outs, layer 0's alpha below this is dropped
    float translucency = 0.0f;    // sunlight through leaves seen from the shady side
    bool vertexTint = false;      // one-layer materials: the vertex colour's rgb multiplies the albedo
};

struct Sun {
    Vector3 dir = {0.4f, 0.75f, -0.55f};  // towards the sun
    Vector3 irradiance = {4.4f, 4.4f, 4.0f};
};

class Renderer {
public:
    bool init(int width, int height, std::string* err);
    void shutdown();
    void resize(int width, int height);

    // assets/sky/<name>.png + _spec.png + .json
    bool loadSky(const std::string& skyDir, const std::string& name, std::string* err);
    float skyYaw = 0.0f;        // rotates the sky (and its sun) about the vertical, radians
    float exposure = 0.8f;
    float skyIntensity = 1.0f;  // ambient and reflections
    float sunIntensity = 1.0f;
    float fogDensity = 0.00022f;
    Sun sun() const;            // after skyYaw and sunIntensity

    // A frame: shadow passes, then the colour pass into the HDR target, then present().
    // Shadow casters are drawn between beginShadows/endShadows with drawShadow(), once per
    // cascade: 0 sharp and close to the camera, 1 coarse and far.
    static constexpr int kCascades = 2;
    void beginShadows(Vector3 focus, float radius, int cascade = 0);
    void drawShadow(const Mesh& mesh, Matrix model, unsigned alphaTex = 0, float alphaCut = 0.5f, float uvScale = 1.0f);
    // Whether a world box is in the camera's view (after beginScene) or in the sun's (after beginShadows).
    bool inView(const BoundingBox& b) const { return boxInClip(b, viewProj_); }
    bool inShadowView(const BoundingBox& b) const { return boxInClip(b, lightVP_[cascade_]); }
    void endShadows();
    void beginScene(const Camera3D& cam);
    void draw(const Mesh& mesh, const Material& mat, Matrix model);
    void drawSky();
    void endScene();
    // Tone map to the current framebuffer (the screen, or a texture for screenshots).
    void present(int x, int y, int width, int height);

    int width() const { return w_; }
    int height() const { return h_; }

private:
    int w_ = 0, h_ = 0;
    unsigned fbo_ = 0, color_ = 0, depth_ = 0;
    unsigned shadowFbo_[kCascades] = {}, shadowTex_[kCascades] = {};
    int shadowRes_ = 4096;
    int cascade_ = 0;
    Matrix lightVP_[kCascades] = {}, viewProj_{};
    static bool boxInClip(const BoundingBox& b, const Matrix& m);
    Shader pbr_{}, depth_s_{}, depthCut_{}, sky_{}, tonemap_{};
    Texture2D skyTex_{}, specTex_{};
    Vector3 sh_[9]{};
    Sun skySun_{};
    bool hasSun_ = false;
    Mesh cube_{};
    Camera3D cam_{};
    Matrix view_{}, proj_{};
    struct Locs {
        int mvp, model, normalMat, viewPos, lightVP, sunDir, sunColor, sh, skyYaw, fog, exposure, specMax;
        int layerScale, tint, layerTint, depthBias, alphaCut, translucency, vertexTint, roughMul, metalMul, normalStrength, clearcoat, ccRough, macro, layers;
        int tex[9], shadow[2], spec;
    } L_{};
    void createTargets();
    void freeTargets();
};

// Mesh helpers: build meshes of any size as 16-bit indexed chunks.
struct Vertex {
    Vector3 p, n;
    Vector4 t;      // tangent (xyz) and bitangent sign (w)
    Vector2 uv;
    Vector2 uv2;    // second UV set (baked lighting, later)
    unsigned char c[4] = {255, 0, 0, 255};  // layer weights rgb, ambient occlusion a
};

class MeshBuilder {
public:
    // Adds a vertex and returns its index in the current chunk.
    int add(const Vertex& v);
    void tri(int a, int b, int c);
    // Starts a new chunk when fewer than `needed` vertices fit in the current one;
    // returns true when it did (indices restart at 0).
    bool reserve(int needed);
    std::vector<Mesh> build();  // uploads; the builder is left empty
    int vertexCount() const { return (int)v_.size(); }
    int chunk() const { return chunk_; }  // changes whenever a new chunk starts


private:
    struct Chunk { std::vector<Vertex> v; std::vector<unsigned short> i; };
    std::vector<Chunk> done_;
    std::vector<Vertex> v_;
    std::vector<unsigned short> i_;
    int chunk_ = 0;
    void flush();
};

// Tangents from positions, UVs and normals of an indexed triangle list (in place).
void computeTangents(std::vector<Vertex>& v, const std::vector<unsigned short>& idx);

}  // namespace gfx
