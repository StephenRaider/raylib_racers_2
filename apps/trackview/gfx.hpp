#pragma once
// RR2's PBR renderer core: HDR render target, image-based lighting from an HDRI sky,
// a shadow-casting sun, metallic-roughness materials and ACES tone mapping.
// Nothing here knows about tracks or cars.
#include <string>
#include <utility>
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
TextureSet flatTextureSet(Texture2D albedo, float roughness, float metalness = 0.0f);
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
    // A decal (skid marks, rubber, dust): lit like the rest, but alpha blended over what is behind it, with
    // the vertex colour's alpha times the albedo texture's alpha as its opacity, and no depth writes.
    bool decal = false;
    float depthBias = 0.0f;       // pulls the surface towards the camera (decals over the road), ~1e-6 units
    float alphaCut = 0.0f;        // > 0: leaf cut-outs, layer 0's alpha below this is dropped
    float translucency = 0.0f;    // sunlight through leaves seen from the shady side
    bool detile = false;          // hide the texture's repetition (large flat surfaces: tarmac)
    float grime = 0.0f;           // 0..1: rubber marks, dust and grit on the surface (kerbs)
    bool vertexTint = false;      // one-layer materials: the vertex colour's rgb multiplies the albedo
    // Far-off ground: a baked colour map (sRGB, over world x/z) replaces the texture layers
    // between colorMapRange.x and .y metres from the camera (terrain).
    unsigned colorMap = 0;
    Vector4 colorMapRect = {0, 0, 1, 1};  // x0, z0 of the map, then 1 / width, 1 / depth (m)
    Vector2 colorMapRange = {110, 200};
};

struct Sun {
    Vector3 dir = {0.4f, 0.75f, -0.55f};  // towards the sun
    Vector3 irradiance = {4.4f, 4.4f, 4.0f};
};

class Renderer {
public:
    // msaa: samples per pixel for the scene (1 = off); 4 smooths edges, leaves and fences.
    bool init(int width, int height, std::string* err, int msaa = 4);
    void shutdown();
    void resize(int width, int height);

    // assets/sky/<name>.png + _spec.png + .json
    bool loadSky(const std::string& skyDir, const std::string& name, std::string* err);
    float skyYaw = 0.0f;        // rotates the sky (and its sun) about the vertical, radians
    float exposure = 0.8f;
    float skyIntensity = 1.0f;  // ambient and reflections
    float sunIntensity = 1.0f;
    float fogDensity = 0.00022f;
    // Contact shadows under cars: up to kBlobs footprints (centre x, z, yaw, ground height),
    // darkening the ground under and just around each car. Set each frame.
    static constexpr int kBlobs = 8;
    struct Blob { float x, z, yaw, y; };
    std::vector<Blob> blobs;
    Sun sun() const;            // after skyYaw and sunIntensity

    // A frame: shadow passes, then the colour pass into the HDR target, then present().
    // Shadow casters are drawn between beginShadows/endShadows with drawShadow(), once per
    // cascade: 0 sharp and close to the camera, 1 coarse and far.
    static constexpr int kCascades = 2;
    void beginShadows(Vector3 focus, float radius, int cascade = 0);
    void drawShadow(const Mesh& mesh, Matrix model, unsigned alphaTex = 0, float alphaCut = 0.5f, float uvScale = 1.0f);
    // Instanced: `count` model matrices (column major, 64 bytes each) in the vertex buffer
    // `instances`; each matrix's bottom row (m3, m7, m11) carries a colour tint.
    void drawShadowInstanced(const Mesh& mesh, unsigned instances, int count, unsigned alphaTex, float alphaCut);
    int cascade() const { return cascade_; }
    // Whether a world box is in the camera's view (after beginScene) or in the sun's (after beginShadows).
    bool inView(const BoundingBox& b) const { return boxInClip(b, viewProj_); }
    bool inShadowView(const BoundingBox& b) const { return boxInClip(b, lightVP_[cascade_]); }
    void endShadows();
    void beginScene(const Camera3D& cam);
    void draw(const Mesh& mesh, const Material& mat, Matrix model);
    void drawInstanced(const Mesh& mesh, const Material& mat, unsigned instances, int count);
    Vector3 cameraPosition() const { return cam_.position; }
    Vector3 cameraTarget() const { return cam_.target; }
    void drawSky();
    void endScene();
    // Post effects for the next present(): depth of field and a far haze blur, camera motion blur
    // and bloom. All off by default (a plain tone-mapped picture); the viewer sets them per frame.
    struct PostParams {
        // thin-lens depth of field, sizes in pixels at 1080 lines (scaled to the real height)
        float focusDist = 0;    // m to the plane in focus; 0 = no lens blur
        float focusTol = 0;     // 1/m: how far off focus (in dioptres) before blur starts: the subject's depth
        float lensK = 0;        // blur px per dioptre of defocus: the longer the lens, the larger
        float maxCoc = 0;       // the largest blur circle, px
        // blur of far-off things on any camera
        float hazeCoc = 0;      // blur px at hazeEnd
        float hazeStart = 80, hazeEnd = 500;
        int taps = 24;          // gather samples (quality)
        // motion blur of the camera's own movement (reprojected from the depth buffer)
        float shutter = 0;      // fraction of a frame's movement smeared; 0 = off
        float maxBlur = 0.03f;  // longest smear, as a fraction of the picture
        Vector4 keep = {0.5f, 0.5f, 0, 0};  // an ellipse (centre uv, radii uv) round the followed car, kept sharp
        float keepDepth = 0;    // m: anything nearer (the car's own nose) is not blurred
        bool cut = false;       // the camera jumped since the last frame: no smear
        // bloom
        float bloom = 0;        // strength; 0 = off
        float bloomThreshold = 1.0f;
    } post;

    // Tone map to the current framebuffer (the screen, or a texture for screenshots), after the post effects.
    void present(int x, int y, int width, int height);

    // Soft camera-facing puffs of smoke, drawn into the scene after the sky (depth tested, not written).
    struct Puff { Vector3 p; float size; float alpha; float rot; };
    void drawPuffs(const std::vector<Puff>& puffs);
    Vector3 ambientColor() const;  // what a white, matt thing looks like in the shade: the sky's horizon light

    int width() const { return w_; }
    int height() const { return h_; }

private:
    int w_ = 0, h_ = 0;
    unsigned fbo_ = 0, color_ = 0, depth_ = 0;
    unsigned shadowFbo_[kCascades] = {}, shadowTex_[kCascades] = {};
    int shadowRes_[kCascades] = {4096, 2048};
    int msaa_ = 4;
    unsigned msFbo_ = 0, msColor_ = 0, msDepth_ = 0;  // multisampled scene target, resolved into fbo_ (colour and depth)
    // post-processing: two full-size ping-pong targets and the bloom chain (down and up, half size and smaller)
    struct Target { unsigned fbo = 0, tex = 0; int w = 0, h = 0; };
    static constexpr int kBloomLevels = 6;
    Target ppA_, ppB_, bloomDown_[kBloomLevels], bloomUp_[kBloomLevels];
    int bloomLevels_ = 0;
    Shader dof_{}, mblur_{}, bloomDownS_{}, bloomUpS_{}, puff_{};
    Texture2D puffTex_{};
    Matrix prevViewProj_{};
    bool havePrev_ = false;
    void makeTarget(Target& t, int w, int h);
    void freeTarget(Target& t);
    void runPass(Target& dst, Shader& s, unsigned tex, const std::vector<std::pair<const char*, unsigned>>& samplers = {});
    int cascade_ = 0;
    Matrix lightVP_[kCascades] = {}, viewProj_{};
    static bool boxInClip(const BoundingBox& b, const Matrix& m);
    Shader pbr_{}, pbrInst_{}, depth_s_{}, depthCut_{}, depthInst_{}, sky_{}, tonemap_{};
    Texture2D skyTex_{}, specTex_{};
    Vector3 sh_[9]{};
    Sun skySun_{};
    Vector3 fogColor_{0.6f, 0.7f, 0.8f};
    bool hasSun_ = false;
    Mesh cube_{};
    Camera3D cam_{};
    Matrix view_{}, proj_{};
    struct Locs {
        int mvp, model, normalMat, viewPos, lightVP, sunDir, sunColor, sh, skyYaw, fog, exposure, specMax;
        int layerScale, tint, layerTint, depthBias, alphaCut, translucency, vertexTint, roughMul, metalMul, normalStrength, clearcoat, ccRough, macro, layers, decal;
        int tex[9], shadow[2], spec, skyMap, blob, blobCount, a2c, fogColor, detile, grime, colorMap, colorMapRect, colorMapRange, useColorMap;
    } L_{}, Li_{};  // plain and instanced
    void lookUp(Shader& s, Locs& l);
    int bindMaterial(const Material& mat, const Locs& l, Matrix model, Matrix mvp);
    void unbindMaterial(int slots, const Material& mat);
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
