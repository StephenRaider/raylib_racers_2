#include "renderer.hpp"

#include "liveries.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <random>

#include "raymath.h"
#include "rlgl.h"

using rr::Vec2;

namespace {

// ---------------------------------------------------------------- shaders

const char* kLitVS = R"(#version 330
in vec3 vertexPosition;
in vec2 vertexTexCoord;
in vec3 vertexNormal;
in vec4 vertexColor;
uniform mat4 mvp;
uniform mat4 matModel;
uniform mat4 matNormal;
out vec3 fragPosition;
out vec2 fragTexCoord;
out vec4 fragColor;
out vec3 fragNormal;
void main() {
    fragPosition = vec3(matModel * vec4(vertexPosition, 1.0));
    fragTexCoord = vertexTexCoord;
    fragColor = vertexColor;
    fragNormal = normalize(vec3(matNormal * vec4(vertexNormal, 0.0)));
    gl_Position = mvp * vec4(vertexPosition, 1.0);
}
)";

const char* kLitFS = R"(#version 330
in vec3 fragPosition;
in vec2 fragTexCoord;
in vec4 fragColor;
in vec3 fragNormal;
uniform sampler2D texture0;
uniform vec4 colDiffuse;
uniform vec3 lightDir;
uniform vec3 lightColor;
uniform vec3 ambientSky;
uniform vec3 ambientGround;
uniform vec3 viewPos;
uniform vec3 fogColor;
uniform float fogDensity;
uniform float specStrength;
uniform mat4 lightVP;
uniform sampler2D shadowMap;
uniform int shadowMapResolution;
uniform float alphaCut;  // leaf cut-outs: below this alpha the fragment is dropped
out vec4 finalColor;

float shadowFactor(vec3 n, vec3 l) {
    // normal offset avoids shadow acne on surfaces at grazing angles to the sun
    vec4 lp = lightVP * vec4(fragPosition + n * 0.12, 1.0);
    vec3 p = lp.xyz / lp.w * 0.5 + 0.5;
    if (p.x <= 0.0 || p.x >= 1.0 || p.y <= 0.0 || p.y >= 1.0 || p.z >= 1.0) return 0.0;
    // depth range of the sun camera is ~800 m: 0.00006 ~ 5 cm
    float bias = max(0.0003 * (1.0 - dot(n, l)), 0.00006);
    vec2 texel = vec2(1.0 / float(shadowMapResolution));
    float s = 0.0;
    for (int x = -1; x <= 1; ++x)
        for (int y = -1; y <= 1; ++y)
            s += (p.z - bias > texture(shadowMap, p.xy + vec2(x, y) * texel).r) ? 1.0 : 0.0;
    // fade out towards the edge of the shadow map
    vec2 edge = min(p.xy, 1.0 - p.xy);
    float fade = clamp(min(edge.x, edge.y) * 12.0, 0.0, 1.0);
    return s / 9.0 * fade;
}

uniform float detail;    // 1: procedural surface detail on flat ground (grass patches, asphalt grain)
uniform float shellFrac; // > 0: a grass shell this far up the blades (0..1)
uniform sampler2D grassMask;  // white where grass grows (not on the track)
uniform vec4 maskRect;   // x, y origin and size of the mask in sim metres

float hash(vec2 p) { return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453); }
float vnoise(vec2 p) {
    vec2 i = floor(p), f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(hash(i), hash(i + vec2(1, 0)), f.x), mix(hash(i + vec2(0, 1)), hash(i + vec2(1, 1)), f.x), f.y);
}
void main() {
    if (shellFrac > 0.0) {
        // shell grass: each shell keeps the blades that reach this high
        vec2 sim = vec2(fragPosition.x, -fragPosition.z);
        if (texture(grassMask, (sim - maskRect.xy) / maskRect.z).r < 0.5) discard;
        vec2 g = fragPosition.xz * 20.0;
        vec2 cell = floor(g);
        float clump = vnoise(fragPosition.xz * 0.35) * 0.7 + vnoise(fragPosition.xz * 2.1) * 0.3;
        float h = hash(cell) * (0.35 + 0.9 * clump);
        vec2 f = fract(g) - 0.5 + (vec2(hash(cell + 1.7), hash(cell + 5.3)) - 0.5) * 0.3 * shellFrac;
        if (h < shellFrac || length(f) > 0.5 * (1.0 - shellFrac * 0.8)) discard;
        float d = length(viewPos.xz - fragPosition.xz);
        if (hash(cell + 3.1) > clamp((75.0 - d) / 30.0, 0.0, 1.0)) discard;
    }
    vec4 tex = texture(texture0, shellFrac > 0.0 ? fragPosition.xz / 14.0 : fragTexCoord);
    if (tex.a < alphaCut) discard;
    vec3 base = tex.rgb * colDiffuse.rgb * fragColor.rgb;
    if (detail > 0.0 && fragNormal.y > 0.9 && fragPosition.y < 0.2) {
        vec2 wp = fragPosition.xz;
        float green = base.g - max(base.r, base.b);
        float grey = 1.0 - clamp((max(base.r, max(base.g, base.b)) - min(base.r, min(base.g, base.b))) * 8.0, 0.0, 1.0);
        if (green > 0.05) {
            // uneven grass: big dry and lush patches, smaller clumps
            float big = vnoise(wp * 0.02), mid = vnoise(wp * 0.11), small = vnoise(wp * 0.9);
            base *= 0.78 + 0.28 * mid + 0.14 * small;
            base = mix(base, base * vec3(1.25, 1.05, 0.7), smoothstep(0.55, 0.85, big) * 0.6);  // dry, yellower
            if (shellFrac > 0.0) base *= 0.55 + 0.6 * shellFrac;  // darker at the roots
        } else if (grey > 0.5) {
            // asphalt grain and darker worn patches
            float grain = hash(floor(wp * 18.0)) * 0.08 + vnoise(wp * 0.15) * 0.10;
            base *= 0.84 + grain;
        }
    }
    vec3 n = normalize(fragNormal);
    if (!gl_FrontFacing) n = -n;  // two-sided leaf cards
    vec3 l = -normalize(lightDir);
    float ndl = max(dot(n, l), 0.0);
    float sh = ndl > 0.0 ? shadowFactor(n, l) : 0.0;
    vec3 hemi = mix(ambientGround, ambientSky, n.y * 0.5 + 0.5);
    vec3 v = normalize(viewPos - fragPosition);
    vec3 h = normalize(l + v);
    float spec = pow(max(dot(n, h), 0.0), 48.0) * specStrength * ndl;
    vec3 col = base * (hemi + lightColor * ndl * (1.0 - sh)) + lightColor * spec * (1.0 - sh);
    float dist = length(viewPos - fragPosition);
    float fog = 1.0 - exp(-pow(dist * fogDensity, 2.0));
    // haze glows warm towards the sun
    float sunward = pow(max(dot(-v, l), 0.0), 6.0);
    vec3 haze = mix(fogColor, vec3(1.0, 0.93, 0.80), sunward * 0.6);
    col = mix(col, haze, clamp(fog, 0.0, 1.0));

    finalColor = vec4(col, tex.a * colDiffuse.a * fragColor.a);
}
)";

// Instanced trees: the per-instance model matrix carries the leaf colour in its unused
// bottom row (m3, m7, m11), so thousands of trees draw in a few calls.
const char* kLitInstVS = R"(#version 330
in vec3 vertexPosition;
in vec2 vertexTexCoord;
in vec3 vertexNormal;
in vec4 vertexColor;
layout(location = 10) in mat4 instanceTransform;  // clear of the mesh attributes (0-8)
uniform mat4 mvp;
out vec3 fragPosition;
out vec2 fragTexCoord;
out vec4 fragColor;
out vec3 fragNormal;
void main() {
    mat4 M = instanceTransform;
    vec3 tint = vec3(M[0][3], M[1][3], M[2][3]);
    M[0][3] = 0.0; M[1][3] = 0.0; M[2][3] = 0.0;
    vec4 wp = M * vec4(vertexPosition, 1.0);
    fragPosition = wp.xyz;
    fragTexCoord = vertexTexCoord;
    fragColor = vertexColor * vec4(tint, 1.0);
    fragNormal = normalize(transpose(inverse(mat3(M))) * vertexNormal);
    gl_Position = mvp * wp;
}
)";

const char* kDepthInstVS = R"(#version 330
in vec3 vertexPosition;
in vec2 vertexTexCoord;
layout(location = 10) in mat4 instanceTransform;  // clear of the mesh attributes (0-8)
uniform mat4 mvp;
out vec2 fragTexCoord;
void main() {
    mat4 M = instanceTransform;
    M[0][3] = 0.0; M[1][3] = 0.0; M[2][3] = 0.0;
    fragTexCoord = vertexTexCoord;
    gl_Position = mvp * M * vec4(vertexPosition, 1.0);
}
)";

// Leaf cut-outs cast their shape, not their card.
const char* kDepthInstFS = R"(#version 330
in vec2 fragTexCoord;
uniform sampler2D texture0;
uniform float alphaCut;
out vec4 finalColor;
void main() {
    if (texture(texture0, fragTexCoord).a < alphaCut) discard;
    finalColor = vec4(1.0);
}
)";

const char* kDepthVS = R"(#version 330
in vec3 vertexPosition;
uniform mat4 mvp;
void main() { gl_Position = mvp * vec4(vertexPosition, 1.0); }
)";

const char* kDepthFS = R"(#version 330
out vec4 finalColor;
void main() { finalColor = vec4(1.0); }
)";

// ---------------------------------------------------------------- helpers

const Vector3 kLightDir = Vector3Normalize({0.45f, -0.75f, 0.35f});
const Color kSkyTop = {78, 128, 200, 255};
const Color kSkyHorizon = {190, 210, 232, 255};

Vector3 W(Vec2 p, float h = 0) { return {p.x, h, -p.y}; }
Vector3 Wdir(Vec2 d) { return {d.x, 0, -d.y}; }

uint32_t hash3(int x, int y, int seed) {
    uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u + (uint32_t)seed * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}
float hashf(int x, int y, int seed) { return (hash3(x, y, seed) & 0xffffff) / 16777215.0f; }

// Tileable value noise on a period x period lattice.
float valueNoise(float x, float y, int period, int seed) {
    int x0 = (int)std::floor(x), y0 = (int)std::floor(y);
    float fx = x - x0, fy = y - y0;
    fx = fx * fx * (3 - 2 * fx);
    fy = fy * fy * (3 - 2 * fy);
    auto at = [&](int xi, int yi) {
        xi = ((xi % period) + period) % period;
        yi = ((yi % period) + period) % period;
        return hashf(xi, yi, seed);
    };
    float a = at(x0, y0), b = at(x0 + 1, y0), c = at(x0, y0 + 1), d = at(x0 + 1, y0 + 1);
    return (a + (b - a) * fx) + ((c + (d - c) * fx) - (a + (b - a) * fx)) * fy;
}

float fbm(float u, float v, int freq, int octaves, int seed) {
    float sum = 0, amp = 0.5f, norm = 0;
    for (int o = 0; o < octaves; ++o) {
        sum += amp * valueNoise(u * freq, v * freq, freq, seed + o * 17);
        norm += amp;
        amp *= 0.5f;
        freq *= 2;
    }
    return sum / norm;
}

Texture2D makeTexture(int size, const std::function<Color(int, int)>& f) {
    Color* px = (Color*)MemAlloc(size * size * sizeof(Color));
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x) px[y * size + x] = f(x, y);
    Image img{px, size, size, 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8};
    Texture2D t = LoadTextureFromImage(img);
    UnloadImage(img);
    GenTextureMipmaps(&t);
    SetTextureFilter(t, TEXTURE_FILTER_ANISOTROPIC_8X);
    SetTextureWrap(t, TEXTURE_WRAP_REPEAT);
    return t;
}

unsigned char c8(float v) { return (unsigned char)std::clamp(v, 0.0f, 255.0f); }

struct MeshBuilder {
    std::vector<float> pos, nrm, uv;
    std::vector<unsigned char> col;

    void vert(Vector3 p, Vector3 n, Vector2 t, Color c) {
        pos.insert(pos.end(), {p.x, p.y, p.z});
        nrm.insert(nrm.end(), {n.x, n.y, n.z});
        uv.insert(uv.end(), {t.x, t.y});
        col.insert(col.end(), {c.r, c.g, c.b, c.a});
    }
    // Quad a-b-c-d; the winding is fixed up so the front face points along n.
    void quad(Vector3 a, Vector3 b, Vector3 c, Vector3 d, Vector3 n, Vector2 ta, Vector2 tb, Vector2 tc, Vector2 td,
              Color col) {
        Vector3 cr = Vector3CrossProduct(Vector3Subtract(b, a), Vector3Subtract(c, a));
        if (Vector3DotProduct(cr, n) < 0) {
            std::swap(b, d);
            std::swap(tb, td);
        }
        vert(a, n, ta, col); vert(b, n, tb, col); vert(c, n, tc, col);
        vert(a, n, ta, col); vert(c, n, tc, col); vert(d, n, td, col);
    }
    Mesh build() {
        Mesh m{};
        m.vertexCount = (int)pos.size() / 3;
        m.triangleCount = m.vertexCount / 3;
        m.vertices = (float*)MemAlloc(pos.size() * sizeof(float));
        m.normals = (float*)MemAlloc(nrm.size() * sizeof(float));
        m.texcoords = (float*)MemAlloc(uv.size() * sizeof(float));
        m.colors = (unsigned char*)MemAlloc(col.size());
        std::memcpy(m.vertices, pos.data(), pos.size() * sizeof(float));
        std::memcpy(m.normals, nrm.data(), nrm.size() * sizeof(float));
        std::memcpy(m.texcoords, uv.data(), uv.size() * sizeof(float));
        std::memcpy(m.colors, col.data(), col.size());
        UploadMesh(&m, false);
        return m;
    }
};

// A barrier segment from a to b (ground level): inner face along inN, a top,
// and an outer face offset by thick (track-plane vector).
void addWall(MeshBuilder& mb, Vector3 a, Vector3 b, Vector3 inN, Vector3 outN, Vec2 thick, float hgt, Color face) {
    Vector3 t = {thick.x, 0, -thick.y};
    Vector3 up = {0, 1, 0};
    auto H = [&](Vector3 v, float h) { return Vector3{v.x, v.y + h, v.z}; };
    Vector3 a2 = Vector3Add(a, t), b2 = Vector3Add(b, t);
    mb.quad(a, b, H(b, hgt), H(a, hgt), inN, {0, 0}, {0, 0}, {0, 0}, {0, 0}, face);
    mb.quad(H(a, hgt), H(b, hgt), H(b2, hgt), H(a2, hgt), up, {0, 0}, {0, 0}, {0, 0}, {0, 0}, Color{150, 150, 155, 255});
    mb.quad(a2, b2, H(b2, hgt), H(a2, hgt), outN, {0, 0}, {0, 0}, {0, 0}, {0, 0}, Color{175, 175, 180, 255});
}

Model modelFrom(Mesh mesh, Texture2D tex) {
    Model m = LoadModelFromMesh(mesh);
    m.materials[0].maps[MATERIAL_MAP_DIFFUSE].texture = tex;
    return m;
}

Matrix boxTransform(Vector3 center, Vector3 size, float yaw) {
    return MatrixMultiply(MatrixMultiply(MatrixScale(size.x, size.y, size.z), MatrixRotateY(yaw)),
                          MatrixTranslate(center.x, center.y, center.z));
}

}  // namespace

// A car's colour on the HUD, minimap, pit box and debug lines: its team's.
Color teamColor(int i) {
    const auto& t = liveryTable();
    if (!t.empty()) return t[carLivery(i)].color;
    static const Color c[] = {{220, 35, 35, 255}, {20, 100, 215, 255}, {255, 128, 0, 255}, {0, 135, 95, 255}};
    return c[i % 4];
}

Color teamAccent(int i) { return i % 2 ? Color{30, 30, 35, 255} : Color{245, 245, 245, 255}; }

// ---------------------------------------------------------------- setup

bool Renderer::init(const rr::Track& track, unsigned seed, const std::string& assetsDir, std::string* err) {
    rlSetClipPlanes(0.5, 5000.0);

    lit_ = LoadShaderFromMemory(kLitVS, kLitFS);
    depth_ = LoadShaderFromMemory(kDepthVS, kDepthFS);
    litInst_ = LoadShaderFromMemory(kLitInstVS, kLitFS);
    depthInst_ = LoadShaderFromMemory(kDepthInstVS, kDepthInstFS);
    for (Shader* sh : {&lit_, &depth_, &litInst_, &depthInst_})
        if (sh->id == 0 || sh->id == rlGetShaderIdDefault()) {
            if (err) *err = "shader compilation failed (OpenGL 3.3 required)";
            return false;
        }
    lit_.locs[SHADER_LOC_MATRIX_MODEL] = GetShaderLocation(lit_, "matModel");
    lit_.locs[SHADER_LOC_MATRIX_NORMAL] = GetShaderLocation(lit_, "matNormal");
    litInst_.locs[SHADER_LOC_MATRIX_MODEL] = GetShaderLocationAttrib(litInst_, "instanceTransform");
    depthInst_.locs[SHADER_LOC_MATRIX_MODEL] = GetShaderLocationAttrib(depthInst_, "instanceTransform");
    locLightVP_ = GetShaderLocation(lit_, "lightVP");
    locShadowMap_ = GetShaderLocation(lit_, "shadowMap");
    locViewPos_ = GetShaderLocation(lit_, "viewPos");
    locSpec_ = GetShaderLocation(lit_, "specStrength");
    locFog_ = GetShaderLocation(lit_, "fogDensity");
    for (Shader* sh : {&lit_, &litInst_}) {
        auto set3 = [&](const char* name, Vector3 v) { SetShaderValue(*sh, GetShaderLocation(*sh, name), &v, SHADER_UNIFORM_VEC3); };
        set3("lightDir", kLightDir);
        set3("lightColor", {1.05f, 1.0f, 0.92f});
        set3("ambientSky", {0.42f, 0.50f, 0.62f});
        set3("ambientGround", {0.24f, 0.24f, 0.20f});
        set3("fogColor", {kSkyHorizon.r / 255.0f, kSkyHorizon.g / 255.0f, kSkyHorizon.b / 255.0f});
        float spec = sh == &lit_ ? 0.15f : 0.04f;
        SetShaderValue(*sh, GetShaderLocation(*sh, "specStrength"), &spec, SHADER_UNIFORM_FLOAT);
        int res = shadowRes_;
        SetShaderValue(*sh, GetShaderLocation(*sh, "shadowMapResolution"), &res, SHADER_UNIFORM_INT);
        float one = 1.0f;
        SetShaderValue(*sh, GetShaderLocation(*sh, "detail"), &one, SHADER_UNIFORM_FLOAT);
    }
    for (Shader* sh : {&litInst_, &depthInst_}) {
        float cut = 0.5f;
        SetShaderValue(*sh, GetShaderLocation(*sh, "alphaCut"), &cut, SHADER_UNIFORM_FLOAT);
    }
    {
        int slot = 10;  // where draw() binds the shadow map
        SetShaderValue(litInst_, GetShaderLocation(litInst_, "shadowMap"), &slot, SHADER_UNIFORM_INT);
    }

    // Depth-only render target for the sun's shadow map.
    shadowMap_.id = rlLoadFramebuffer();
    shadowMap_.texture.width = shadowRes_;
    shadowMap_.texture.height = shadowRes_;
    rlEnableFramebuffer(shadowMap_.id);
    shadowMap_.depth.id = rlLoadTextureDepth(shadowRes_, shadowRes_, false);
    shadowMap_.depth.width = shadowRes_;
    shadowMap_.depth.height = shadowRes_;
    shadowMap_.depth.format = 19;
    shadowMap_.depth.mipmaps = 1;
    rlFramebufferAttach(shadowMap_.id, shadowMap_.depth.id, RL_ATTACHMENT_DEPTH, RL_ATTACHMENT_TEXTURE2D, 0);
    bool ok = rlFramebufferComplete(shadowMap_.id);
    rlDisableFramebuffer();
    if (!ok) {
        if (err) *err = "could not create the shadow-map framebuffer";
        return false;
    }

    // Procedural textures (tileable).
    texAsphalt_ = makeTexture(512, [](int x, int y) {
        float u = x / 512.0f, v = y / 512.0f;
        float g = 66 + 22 * fbm(u, v, 16, 4, 3) + 8 * (hashf(x, y, 7) - 0.5f);
        if (hashf(x, y, 11) > 0.995f) g += 18;  // aggregate specks
        return Color{c8(g), c8(g), c8(g + 4), 255};
    });
    // grass: greener in the forest, paler on the airfield, sandy among the dunes
    theme_ = track.scenery();
    Vector3 grass = {66, 104, 48};
    float dry = 22;
    if (theme_ == "forest") grass = {52, 96, 44};
    else if (theme_ == "airfield") grass = {84, 118, 60};
    else if (theme_ == "hills") grass = {92, 112, 52}, dry = 30;
    else if (theme_ == "dunes") grass = {150, 146, 96}, dry = 40;
    else if (theme_ == "tropical") grass = {50, 108, 42};
    else if (theme_ == "parkland") grass = {62, 106, 46};
    texGrass_ = makeTexture(512, [grass, dry](int x, int y) {
        float u = x / 512.0f, v = y / 512.0f;
        float big = fbm(u, v, 4, 4, 21), fine = hashf(x, y, 5);
        float k = 0.72f + 0.45f * big + 0.12f * (fine - 0.5f);
        return Color{c8(grass.x * k + dry * big), c8(grass.y * k), c8(grass.z * k), 255};
    });
    {
        Image chk = GenImageChecked(64, 64, 8, 8, RAYWHITE, Color{25, 25, 25, 255});
        texChecker_ = LoadTextureFromImage(chk);
        UnloadImage(chk);
        SetTextureFilter(texChecker_, TEXTURE_FILTER_POINT);
        SetTextureWrap(texChecker_, TEXTURE_WRAP_REPEAT);
    }
    texWhite_ = Texture2D{rlGetTextureIdDefault(), 1, 1, 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8};

    mdlCube_ = LoadModelFromMesh(GenMeshCube(1, 1, 1));
    mdlWheel_ = LoadModelFromMesh(GenMeshCylinder(1, 1, 20));
    mdlSphere_ = LoadModelFromMesh(GenMeshSphere(1, 12, 16));
    mdlCone_ = LoadModelFromMesh(GenMeshCone(1, 1, 10));
    mdlTrunk_ = LoadModelFromMesh(GenMeshCylinder(1, 1, 8));
    mdlPyramid_ = LoadModelFromMesh(GenMeshCone(1, 1, 4));
    // low-poly tree parts, only ever drawn instanced
    treeMesh_[TP_TRUNK] = GenMeshCylinder(1, 1, 5);
    treeMesh_[TP_CONE] = GenMeshCone(1, 1, 7);
    treeMesh_[TP_BLOB] = GenMeshSphere(1, 6, 8);
    treeMesh_[TP_FROND] = GenMeshCube(1, 1, 1);
    treeMat_ = LoadMaterialDefault();

    std::string carErr;
    if (assetsDir.empty() || !carModel_.load(assetsDir, &carErr))
        TraceLog(LOG_WARNING, "F1 car model not loaded (%s); drawing box cars",
                 assetsDir.empty() ? "no assets folder" : carErr.c_str());

    buildTrack(track);
    loadTrees(assetsDir);
    buildScenery(track, seed);
    return true;
}

void Renderer::shutdown() {
    carModel_.unload();
    // Shared textures are owned here, not by the models.
    for (Model* m : {&mdlAsphalt_, &mdlMarkings_, &mdlWalls_, &mdlGround_, &mdlStart_, &mdlCube_, &mdlWheel_,
                     &mdlSphere_, &mdlCone_, &mdlTrunk_, &mdlPyramid_}) {
        if (m->meshCount == 0) continue;
        m->materials[0].maps[MATERIAL_MAP_DIFFUSE].texture.id = rlGetTextureIdDefault();
        m->materials[0].shader = Shader{rlGetShaderIdDefault(), rlGetShaderLocsDefault()};
        UnloadModel(*m);
    }
    for (Texture2D* t : {&texAsphalt_, &texGrass_, &texChecker_})
        if (t->id) UnloadTexture(*t);
    for (Mesh& m : treeMesh_)
        if (m.vertexCount) UnloadMesh(m);
    for (Mesh& m : slotMesh_) UnloadMesh(m);
    for (Texture2D& t : treeTextures_) UnloadTexture(t);
    if (treeMat_.maps) {
        treeMat_.shader = Shader{rlGetShaderIdDefault(), rlGetShaderLocsDefault()};
        UnloadMaterial(treeMat_);
    }
    for (Shader* sh : {&lit_, &depth_, &litInst_, &depthInst_}) UnloadShader(*sh);
    if (shadowMap_.id) {
        rlUnloadTexture(shadowMap_.depth.id);
        rlUnloadFramebuffer(shadowMap_.id);
    }
}

void Renderer::buildTrack(const rr::Track& tr) {
    const int n = tr.size();
    const Vector3 up = {0, 1, 0};

    // Kerbs where the track bends, widened a little either side.
    std::vector<char> kerb(n, 0);
    for (int i = 0; i < n; ++i)
        if (std::fabs(tr.at(i).curvature) > 1.0f / 220.0f)
            for (int k = -12; k <= 12; ++k) kerb[tr.wrap(i + k)] = 1;

    MeshBuilder asphalt, marks, walls, ground, start;
    float minX = 1e9f, maxX = -1e9f, minY = 1e9f, maxY = -1e9f;
    for (int i = 0; i < n; ++i) {
        const auto& a = tr.at(i);
        const auto& b = tr.at(i + 1);
        float sa = a.s, sb = (i + 1 == n) ? tr.length() : b.s;
        minX = std::min(minX, a.p.x); maxX = std::max(maxX, a.p.x);
        minY = std::min(minY, a.p.y); maxY = std::max(maxY, a.p.y);

        auto edge = [&](const rr::TrackSample& s, float lat, float h) { return W(s.p + s.n * lat, h); };

        // tarmac: u across (one tile per 8 m), v along
        float ua = a.halfWidth / 4.0f, ub = b.halfWidth / 4.0f;
        asphalt.quad(edge(a, -a.halfWidth, 0), edge(b, -b.halfWidth, 0), edge(b, b.halfWidth, 0), edge(a, a.halfWidth, 0),
                     up, {-ua, sa / 8}, {-ub, sb / 8}, {ub, sb / 8}, {ua, sa / 8}, WHITE);

        for (float side : {1.0f, -1.0f}) {
            // painted edge line
            Color white = {235, 235, 235, 255};
            marks.quad(edge(a, side * (a.halfWidth - 0.45f), 0.015f), edge(b, side * (b.halfWidth - 0.45f), 0.015f),
                       edge(b, side * (b.halfWidth - 0.2f), 0.015f), edge(a, side * (a.halfWidth - 0.2f), 0.015f), up,
                       {0, 0}, {0, 0}, {0, 0}, {0, 0}, white);
            // kerb stripes
            if (kerb[i]) {
                bool red = ((int)std::floor(sa / 2.5f)) % 2 == 0;
                Color c = red ? Color{200, 35, 35, 255} : Color{235, 235, 235, 255};
                marks.quad(edge(a, side * a.halfWidth, 0.03f), edge(b, side * b.halfWidth, 0.03f),
                           edge(b, side * (b.halfWidth + 1.2f), 0.07f), edge(a, side * (a.halfWidth + 1.2f), 0.07f), up,
                           {0, 0}, {0, 0}, {0, 0}, {0, 0}, c);
            }
            // barrier: inner face, top, outer face (pushed out round the pit area)
            int sd = side > 0 ? 1 : -1;
            const float extra = tr.barrierOffset(0.5f * (sa + sb), sd, 0.0f);  // beyond the tarmac edge
            const float ob = b.halfWidth + extra;
            float wa = side * (a.halfWidth + extra), wb = side * ob;
            float th = side * 0.4f, hgt = 1.0f;
            Vector3 inN = Wdir(a.n * -side), outN = Wdir(a.n * side);
            bool panel = ((int)std::floor(sa / 8.0f)) % 2 == 0;
            Color face = panel ? Color{232, 232, 236, 255} : Color{40, 85, 175, 255};
            addWall(walls, edge(a, wa, 0), edge(b, wb, 0), inN, outN, a.n * th, hgt, face);
            // Where the barrier steps out for the pit area, close the gap across.
            const float nextS = sb + 0.5f * tr.spacing();  // middle of the next segment
            float next = b.halfWidth + tr.barrierOffset(nextS, sd, 0.0f);
            if (std::fabs(next - ob) > 0.5f) {
                Vector3 fwdN = Wdir(b.t * (next > ob ? -1.0f : 1.0f));
                addWall(walls, edge(b, side * std::min(ob, next), 0), edge(b, side * std::max(ob, next), 0), fwdN,
                        Vector3Negate(fwdN), b.t * 0.4f * (next > ob ? -1.0f : 1.0f), hgt, face);
            }
        }

        // Pit area: paved apron out to the barrier, the pit wall along the lane,
        // the fast-lane line and the speed-limit lines.
        if (tr.hasPit() && tr.inPitArea(sa) && tr.inPitArea(sb)) {
            const float side = (float)tr.pit().side;
            const Color apron = {178, 178, 184, 255};
            float ia = a.halfWidth + 1.2f, ib = b.halfWidth + 1.2f;
            float oa = a.halfWidth + rr::Track::kPitBarrier, ob = b.halfWidth + rr::Track::kPitBarrier;
            asphalt.quad(edge(a, side * ia, 0.005f), edge(b, side * ib, 0.005f), edge(b, side * ob, 0.005f),
                         edge(a, side * oa, 0.005f), up, {ia / 4, sa / 8}, {ib / 4, sb / 8}, {ob / 4, sb / 8},
                         {oa / 4, sa / 8}, apron);
            if (tr.inPitLane(sa) && tr.inPitLane(sb)) {
                float da = side * (a.halfWidth + rr::Track::kDividerIn), db = side * (b.halfWidth + rr::Track::kDividerIn);
                bool red = ((int)std::floor(sa / 4.0f)) % 2 == 0;
                Color c = red ? Color{205, 40, 40, 255} : Color{238, 238, 240, 255};
                float thick = rr::Track::kDividerOut - rr::Track::kDividerIn;
                addWall(walls, edge(a, da, 0), edge(b, db, 0), Wdir(a.n * -side), Wdir(a.n * side), a.n * (side * thick),
                        1.1f, c);
                // line between the fast lane and the boxes
                float la = side * (a.halfWidth + 7.0f), lb = side * (b.halfWidth + 7.0f);
                marks.quad(edge(a, la - side * 0.1f, 0.02f), edge(b, lb - side * 0.1f, 0.02f), edge(b, lb + side * 0.1f, 0.02f),
                           edge(a, la + side * 0.1f, 0.02f), up, {0, 0}, {0, 0}, {0, 0}, {0, 0}, Color{235, 235, 235, 255});
            }
        }
    }

    // Pit speed-limit lines across the lane.
    if (tr.hasPit()) {
        const auto& p = tr.pit();
        for (float s : {p.lane_start_s, p.lane_end_s}) {
            const auto& sm = tr.at(tr.indexAt(s));
            float lo = sm.halfWidth + rr::Track::kDividerOut, hi = sm.halfWidth + rr::Track::kPitBarrier;
            Vec2 f = sm.t * 0.3f;
            auto P = [&](float along, float lat) { return W(sm.p + f * along + sm.n * (p.side * lat), 0.025f); };
            marks.quad(P(-1, lo), P(1, lo), P(1, hi), P(-1, hi), up, {0, 0}, {0, 0}, {0, 0}, {0, 0}, Color{235, 235, 235, 255});
        }
    }

    // start / finish line, 2 m deep, 0.5 m squares
    {
        const auto& s0 = tr.at(0);
        float hw = s0.halfWidth;
        Vec2 f = s0.t, l = s0.n;
        auto P = [&](float along, float lat) { return W(s0.p + f * along + l * lat, 0.02f); };
        float u = 2 * hw / 0.5f / 8.0f, v = 2.0f / 0.5f / 8.0f;
        start.quad(P(-1, -hw), P(1, -hw), P(1, hw), P(-1, hw), up, {0, 0}, {0, v}, {u, v}, {u, 0}, WHITE);
    }

    // ground
    trackCenter_ = {(minX + maxX) / 2, 0, -(minY + maxY) / 2};
    trackExtent_ = std::max(maxX - minX, maxY - minY);
    float half = trackExtent_ * 0.5f + 1500.0f;
    float tile = 14.0f;
    Vector3 c = trackCenter_;
    ground.quad({c.x - half, -0.08f, c.z - half}, {c.x + half, -0.08f, c.z - half}, {c.x + half, -0.08f, c.z + half},
                {c.x - half, -0.08f, c.z + half}, up, {0, 0}, {2 * half / tile, 0}, {2 * half / tile, 2 * half / tile},
                {0, 2 * half / tile}, WHITE);

    mdlAsphalt_ = modelFrom(asphalt.build(), texAsphalt_);
    mdlMarkings_ = modelFrom(marks.build(), texWhite_);
    mdlWalls_ = modelFrom(walls.build(), texWhite_);
    mdlGround_ = modelFrom(ground.build(), texGrass_);
    mdlStart_ = modelFrom(start.build(), texChecker_);

    // Where grass grows: everywhere but the track, its kerbs and the pit area (the lane and
    // the aprons in and out of it).
    {
        const int N = 2048;
        const float size = std::max(maxX - minX, maxY - minY) + 600.0f;
        maskRect_ = {minX - 300.0f, minY - 300.0f, size, 0};
        Image img = GenImageColor(N, N, WHITE);
        auto px = [&](Vec2 p) { return Vector2{(p.x - maskRect_.x) / size * N, (p.y - maskRect_.y) / size * N}; };
        for (int i = 0; i < n; ++i) {
            const auto& a = tr.at(i);
            const auto& b = tr.at(i + 1);
            float la = a.halfWidth + 2.0f, lb = b.halfWidth + 2.0f, ra = la, rb = lb;
            if (tr.hasPit() && tr.inPitArea(a.s)) {
                float& wa = tr.pit().side > 0 ? la : ra;
                float& wb = tr.pit().side > 0 ? lb : rb;
                wa = a.halfWidth + rr::Track::kPitBarrier + 1.0f;
                wb = b.halfWidth + rr::Track::kPitBarrier + 1.0f;
            }
            Vector2 p0 = px(a.p + a.n * la), p1 = px(b.p + b.n * lb), p2 = px(b.p - b.n * rb), p3 = px(a.p - a.n * ra);
            for (auto t : {std::array<Vector2, 3>{p0, p1, p2}, std::array<Vector2, 3>{p0, p2, p3},
                           std::array<Vector2, 3>{p0, p2, p1}, std::array<Vector2, 3>{p0, p3, p2}})
                ImageDrawTriangle(&img, t[0], t[1], t[2], BLACK);
        }
        texMask_ = LoadTextureFromImage(img);
        SetTextureFilter(texMask_, TEXTURE_FILTER_BILINEAR);
        UnloadImage(img);
        mdlShell_ = LoadModelFromMesh(GenMeshPlane(1, 1, 1, 1));
        mdlShell_.materials[0].maps[MATERIAL_MAP_DIFFUSE].texture = texGrass_;
    }
}

// ---------------------------------------------------------------- camera

const char* camName(CamMode mode) {
    static const char* names[] = {"FOLLOW", "CINEMATIC", "TV", "HELICOPTER", "TOP DOWN", "ORBIT", "OVERVIEW", "DIRECTOR", "T-CAM", "NOSE"};
    return mode >= 0 && mode < CAM_COUNT ? names[mode] : "?";
}

namespace {

float smootherstep(float x) {
    x = std::clamp(x, 0.0f, 1.0f);
    return x * x * x * (x * (x * 6 - 15) + 10);
}

float wrapPi(float a) {
    while (a > PI) a -= 2 * PI;
    while (a < -PI) a += 2 * PI;
    return a;
}

}  // namespace

// Cinematic shot n: one of a few classic framings, picked and varied by a hash so a replay looks the same.
// Azimuth 0 is straight behind the car, PI/2 its left side, PI in front of it.
static void cineKey(unsigned n, float* az, float* h, float* d, float* fov, float* hold) {
    auto r = [&](int k) { return hashf((int)n, k, 977); };
    const float side = (n % 2) ? 1.0f : -1.0f;  // alternate sides so the camera keeps swinging across
    switch (hash3((int)n, 0, 1031) % 5) {
        case 0:  // low rear three-quarter
            *az = side * (0.45f + 0.4f * r(1)); *h = 0.7f + 0.5f * r(2); *d = 6.5f + 2.5f * r(3); *fov = 52; break;
        case 1:  // tracking alongside
            *az = side * (1.45f + 0.3f * r(1)); *h = 1.0f + 1.0f * r(2); *d = 8.0f + 4.0f * r(3); *fov = 46; break;
        case 2:  // low front three-quarter, the car coming at the lens
            *az = side * (2.3f + 0.45f * r(1)); *h = 0.6f + 0.6f * r(2); *d = 9.0f + 4.0f * r(3); *fov = 44; break;
        case 3:  // high behind
            *az = side * 0.3f * r(1); *h = 4.0f + 3.0f * r(2); *d = 12.0f + 4.0f * r(3); *fov = 50; break;
        default:  // wide and high off to one side
            *az = side * (1.0f + 1.0f * r(1)); *h = 6.0f + 4.0f * r(2); *d = 18.0f + 7.0f * r(3); *fov = 38; break;
    }
    *hold = 5.0f + 4.0f * r(4);
}

void Renderer::resetCinematic(float clock) {
    // Walk the shot list up to `clock`, so the shot only depends on race time.
    cineSeed_ = 0;
    cineSegStart_ = 0;
    auto key = [](unsigned n) {
        CineKey k;
        cineKey(n, &k.azimuth, &k.height, &k.dist, &k.fov, &k.hold);
        return k;
    };
    cineFrom_ = key(0);
    cineTo_ = key(1);
    while (cineSegStart_ + cineFrom_.hold <= clock) {
        cineSegStart_ += cineFrom_.hold;
        ++cineSeed_;
        cineFrom_ = cineTo_;
        cineTo_ = key(cineSeed_ + 1);
    }
    cineClock_ = clock;
}

void Renderer::updateCamera(const rr::Race& race, int focus, CamMode mode, float dt) {
    fx_.update(race, dt);
    const rr::Car& c = race.cars()[focus];
    Vector3 p = W(c.state.pos);
    Vector3 fwd = Wdir(rr::fromAngle(c.state.yaw));
    Vec2 vel = c.state.velWorld();
    float speed = rr::length(vel);
    // Follow the direction of travel when moving, so spins do not whip the camera round.
    Vector3 dir = speed > 3 ? Vector3Normalize(Vector3Lerp(fwd, Wdir(rr::normalize(vel)), 0.6f)) : fwd;
    camera.up = {0, 1, 0};
    camera.projection = CAMERA_PERSPECTIVE;
    const bool cut = focus != lastFocus_ || mode != lastMode_;
    if (cut) chaseInit_ = false;
    lastFocus_ = focus;
    lastMode_ = mode;
    if (!chaseInit_) smoothDir_ = dir;
    smoothDir_ = Vector3Normalize(Vector3Lerp(smoothDir_, dir, 1 - std::exp(-dt * 3.0f)));

    // Mouse wheel zooms the cameras that have a distance to play with.
    const float wheel = GetMouseWheelMove();

    switch (mode) {
        case CAM_CHASE: {
            Vector3 wantPos = Vector3Add(Vector3Subtract(p, Vector3Scale(dir, 8.5f)), {0, 2.7f, 0});
            Vector3 wantTarget = Vector3Add(Vector3Add(p, Vector3Scale(dir, 4.0f)), {0, 0.9f, 0});
            // Smoothed relative to the car: the camera swings round in corners but
            // never drops back on the straights, however fast the car or the replay.
            const Vector3 wantOff = Vector3Subtract(wantPos, p), wantTgt = Vector3Subtract(wantTarget, p);
            if (!chaseInit_) {
                chasePos_ = wantOff;
                chaseTarget_ = wantTgt;
                chaseInit_ = true;
            }
            chasePos_ = Vector3Lerp(chasePos_, wantOff, 1 - std::exp(-dt * 6.0f));
            chaseTarget_ = Vector3Lerp(chaseTarget_, wantTgt, 1 - std::exp(-dt * 14.0f));
            camera.position = Vector3Add(p, chasePos_);
            camera.target = Vector3Add(p, chaseTarget_);
            camera.fovy = 55.0f + std::min(14.0f, speed * 0.18f);
            break;
        }
        case CAM_CINEMATIC: {
            if (!chaseInit_) resetCinematic((float)race.time());
            cineClock_ += dt;
            while (cineClock_ >= cineSegStart_ + cineFrom_.hold) {
                cineSegStart_ += cineFrom_.hold;
                ++cineSeed_;
                cineFrom_ = cineTo_;
                cineKey(cineSeed_ + 1, &cineTo_.azimuth, &cineTo_.height, &cineTo_.dist, &cineTo_.fov, &cineTo_.hold);
            }
            // Hold the framing for the first part of each segment, then sweep to the next one.
            float u = (cineClock_ - cineSegStart_) / cineFrom_.hold;
            float e = smootherstep((u - 0.4f) / 0.6f);
            float az = cineFrom_.azimuth + wrapPi(cineTo_.azimuth - cineFrom_.azimuth) * e;
            float h = cineFrom_.height + (cineTo_.height - cineFrom_.height) * e;
            float d = cineFrom_.dist + (cineTo_.dist - cineFrom_.dist) * e;
            float fov = cineFrom_.fov + (cineTo_.fov - cineFrom_.fov) * e;
            // a slow drift so held framings breathe instead of sitting still
            az += 0.12f * std::sin(cineClock_ * 0.37f) + 0.05f * std::sin(cineClock_ * 0.93f);
            h += 0.25f * std::sin(cineClock_ * 0.51f + 1.0f);
            Vector3 back = Vector3RotateByAxisAngle(Vector3Negate(smoothDir_), {0, 1, 0}, az);
            Vector3 wantPos = Vector3Add(p, Vector3Add(Vector3Scale(back, d), {0, h, 0}));
            Vector3 wantTarget = Vector3Add(Vector3Add(p, Vector3Scale(smoothDir_, 1.2f)), {0, 0.55f, 0});
            const Vector3 wantOff = Vector3Subtract(wantPos, p), wantTgt = Vector3Subtract(wantTarget, p);
            if (!chaseInit_) {
                chasePos_ = wantOff;
                chaseTarget_ = wantTgt;
                chaseInit_ = true;
            }
            chasePos_ = Vector3Lerp(chasePos_, wantOff, 1 - std::exp(-dt * 8.0f));
            chaseTarget_ = Vector3Lerp(chaseTarget_, wantTgt, 1 - std::exp(-dt * 16.0f));
            camera.position = Vector3Add(p, chasePos_);
            camera.position.y = std::max(camera.position.y, 0.35f);
            camera.target = Vector3Add(p, chaseTarget_);
            camera.fovy = fov;
            break;
        }
        case CAM_TV: {
            Vector3 best = tvSpots_.empty() ? Vector3Add(p, {20, 8, 20}) : tvSpots_[0];
            float bd = 1e30f;
            for (auto& s : tvSpots_) {
                float d = Vector3Distance(s, p);
                if (d < bd) { bd = d; best = s; }
            }
            camera.position = best;
            camera.target = Vector3Add(p, {0, 0.6f, 0});
            float dist = Vector3Distance(best, p);
            camera.fovy = std::clamp(2.0f * std::atan(11.0f / dist) * RAD2DEG, 6.0f, 60.0f);
            break;
        }
        case CAM_HELI: {
            // Hangs off to one side and behind, high up, drifting slowly round and lagging the car a little.
            heliDist_ = std::clamp(heliDist_ * (1.0f - wheel * 0.1f), 30.0f, 300.0f);
            if (!chaseInit_) heliYaw_ = std::atan2(-smoothDir_.z, -smoothDir_.x) + 0.7f;
            heliYaw_ += dt * 0.035f;
            Vector3 wantPos = Vector3Add(p, {std::cos(heliYaw_) * heliDist_, heliDist_ * 0.62f, std::sin(heliYaw_) * heliDist_});
            Vector3 wantTarget = Vector3Add(p, Vector3Scale(dir, std::min(speed, 60.0f) * 0.25f));
            const Vector3 wantOff = Vector3Subtract(wantPos, p), wantTgt = Vector3Subtract(wantTarget, p);
            if (!chaseInit_) {
                chasePos_ = wantOff;
                chaseTarget_ = wantTgt;
                chaseInit_ = true;
            }
            chasePos_ = Vector3Lerp(chasePos_, wantOff, 1 - std::exp(-dt * 1.2f));
            chaseTarget_ = Vector3Lerp(chaseTarget_, wantTgt, 1 - std::exp(-dt * 5.0f));
            camera.position = Vector3Add(p, chasePos_);
            camera.target = Vector3Add(p, chaseTarget_);
            camera.fovy = 34.0f;
            break;
        }
        case CAM_TOP: {
            // Straight down with north (the minimap's up) at the top of the screen.
            topHeight_ = std::clamp(topHeight_ * (1.0f - wheel * 0.1f), 30.0f, 600.0f);
            Vector3 wantTarget = Vector3Add(p, Vector3Scale(dir, std::min(speed, 60.0f) * 0.3f));
            const Vector3 wantTgt = Vector3Subtract(wantTarget, p);
            if (!chaseInit_) {
                chaseTarget_ = wantTgt;
                chaseInit_ = true;
            }
            chaseTarget_ = Vector3Lerp(chaseTarget_, wantTgt, 1 - std::exp(-dt * 4.0f));
            camera.target = Vector3Add(p, chaseTarget_);
            camera.position = Vector3Add(camera.target, {0, topHeight_, 0});
            camera.up = {0, 0, -1};
            camera.fovy = 45.0f;
            break;
        }
        case CAM_ORBIT: {
            if (IsMouseButtonDown(MOUSE_BUTTON_LEFT) || IsMouseButtonDown(MOUSE_BUTTON_RIGHT)) {
                Vector2 md = GetMouseDelta();
                orbitYaw_ -= md.x * 0.006f;
                orbitPitch_ = std::clamp(orbitPitch_ + md.y * 0.006f, 0.05f, 1.45f);
            }
            orbitDist_ = std::clamp(orbitDist_ * (1.0f - wheel * 0.1f), 5.0f, 250.0f);
            Vector3 off = {std::cos(orbitPitch_) * std::cos(orbitYaw_), std::sin(orbitPitch_),
                           std::cos(orbitPitch_) * std::sin(orbitYaw_)};
            camera.target = Vector3Add(p, {0, 0.6f, 0});
            camera.position = Vector3Add(camera.target, Vector3Scale(off, orbitDist_));
            camera.fovy = 50.0f;
            break;
        }
        default: {
            camera.target = trackCenter_;
            camera.position = Vector3Add(trackCenter_, {0, trackExtent_ * 0.95f, trackExtent_ * 0.55f});
            camera.fovy = 50.0f;
            break;
        }
    }
}

// ---------------------------------------------------------------- drawing

void Renderer::drawModel(Model& m, Matrix transform, Color tint) {
    m.materials[0].shader = *current_;
    m.transform = transform;
    DrawModel(m, {0, 0, 0}, 1.0f, tint);
}

void Renderer::drawBox(Vector3 center, Vector3 size, Color color, Matrix parent) {
    Matrix local = MatrixMultiply(MatrixScale(size.x, size.y, size.z), MatrixTranslate(center.x, center.y, center.z));
    drawModel(mdlCube_, MatrixMultiply(local, parent), color);
}

void Renderer::drawCar(const rr::Car& car, int index) {
    if (!carModel_.loaded()) {
        drawBoxCar(car, index);
        return;
    }
    const auto& st = car.state;
    CarModel::Pose pose;
    pose.world = MatrixMultiply(MatrixRotateY(st.yaw), MatrixTranslate(st.pos.x, 0, -st.pos.y));
    pose.centreOffset = 0.5f * (car.phys.cgToFront - car.phys.cgToRear);
    pose.steer = st.steerAngle;
    // the sim rolls a car.phys.wheelRadius wheel; scale so the model's tyres do not skid
    pose.wheelRot = st.wheelRot * car.phys.wheelRadius / carModel_.wheelRadius();
    pose.livery = carLivery(index);
    float spec = 0.55f;
    if (current_ == &lit_) SetShaderValue(lit_, locSpec_, &spec, SHADER_UNIFORM_FLOAT);
    carModel_.draw(pose, *current_);
    spec = 0.15f;
    if (current_ == &lit_) SetShaderValue(lit_, locSpec_, &spec, SHADER_UNIFORM_FLOAT);
}

// Fallback when the F1 model is missing: a car built from boxes and cylinders.
void Renderer::drawBoxCar(const rr::Car& car, int index) {
    const auto& st = car.state;
    Matrix M = MatrixMultiply(MatrixRotateY(st.yaw), MatrixTranslate(st.pos.x, 0, -st.pos.y));
    Color body = teamColor(index), accent = teamAccent(index);
    Color carbon = {32, 32, 36, 255};
    float spec = 0.6f;
    if (current_ == &lit_) SetShaderValue(lit_, locSpec_, &spec, SHADER_UNIFORM_FLOAT);

    // Local frame: x forward, y up, z to the right.
    drawBox({0.0f, 0.22f, 0}, {4.3f, 0.14f, 1.45f}, carbon, M);          // floor
    drawBox({0.25f, 0.42f, 0}, {2.9f, 0.34f, 0.78f}, body, M);            // tub
    drawBox({1.95f, 0.34f, 0}, {1.3f, 0.2f, 0.42f}, body, M);             // nose
    for (float z : {-0.62f, 0.62f}) drawBox({-0.15f, 0.4f, z}, {1.7f, 0.32f, 0.5f}, body, M);  // sidepods
    drawBox({-0.95f, 0.62f, 0}, {1.5f, 0.36f, 0.52f}, body, M);           // engine cover
    drawBox({-0.15f, 0.88f, 0}, {0.36f, 0.3f, 0.3f}, accent, M);          // airbox
    drawBox({2.42f, 0.13f, 0}, {0.5f, 0.06f, 1.9f}, accent, M);           // front wing
    for (float z : {-0.93f, 0.93f}) drawBox({2.42f, 0.22f, z}, {0.55f, 0.22f, 0.04f}, carbon, M);
    drawBox({-2.15f, 0.92f, 0}, {0.45f, 0.06f, 1.35f}, accent, M);        // rear wing
    drawBox({-2.15f, 0.78f, 0}, {0.3f, 0.04f, 1.25f}, body, M);
    for (float z : {-0.67f, 0.67f}) drawBox({-2.15f, 0.72f, z}, {0.55f, 0.48f, 0.04f}, carbon, M);
    drawBox({-1.95f, 0.62f, 0}, {0.12f, 0.4f, 0.08f}, carbon, M);         // wing pylon
    // helmet
    Matrix helmet = MatrixMultiply(MatrixScale(0.17f, 0.17f, 0.17f), MatrixTranslate(0.35f, 0.78f, 0));
    drawModel(mdlSphere_, MatrixMultiply(helmet, M), accent);

    // wheels: cylinder along +y, centred, turned onto the z axle, rolled, steered
    const float r = car.phys.wheelRadius;
    struct W { float x, z, width; bool front; };
    const W wheels[] = {{car.phys.cgToFront, -0.86f, 0.36f, true}, {car.phys.cgToFront, 0.86f, 0.36f, true},
                        {-car.phys.cgToRear, -0.84f, 0.44f, false}, {-car.phys.cgToRear, 0.84f, 0.44f, false}};
    for (const W& w : wheels) {
        Matrix m = MatrixMultiply(MatrixScale(r, w.width, r), MatrixTranslate(0, -w.width * 0.5f, 0));
        m = MatrixMultiply(m, MatrixRotateX(PI / 2));
        m = MatrixMultiply(m, MatrixRotateZ(-st.wheelRot));
        if (w.front) m = MatrixMultiply(m, MatrixRotateY(st.steerAngle));
        m = MatrixMultiply(m, MatrixTranslate(w.x, r, w.z));
        drawModel(mdlWheel_, MatrixMultiply(m, M), Color{22, 22, 24, 255});
        // rim
        Matrix rim = MatrixMultiply(MatrixScale(r * 0.62f, w.width + 0.02f, r * 0.62f),
                                    MatrixTranslate(0, -(w.width + 0.02f) * 0.5f, 0));
        rim = MatrixMultiply(rim, MatrixRotateX(PI / 2));
        if (w.front) rim = MatrixMultiply(rim, MatrixRotateY(st.steerAngle));
        rim = MatrixMultiply(rim, MatrixTranslate(w.x, r, w.z));
        drawModel(mdlWheel_, MatrixMultiply(rim, M), Color{150, 150, 158, 255});
    }
    spec = 0.15f;
    if (current_ == &lit_) SetShaderValue(lit_, locSpec_, &spec, SHADER_UNIFORM_FLOAT);
}

void Renderer::drawScene(const rr::Race& race, bool shadowPass) {
    const Matrix I = MatrixIdentity();
    if (!shadowPass) {
        drawModel(mdlGround_, I, WHITE);
        drawModel(mdlAsphalt_, I, WHITE);
        drawModel(mdlMarkings_, I, WHITE);
        drawModel(mdlStart_, I, WHITE);
    }
    drawModel(mdlWalls_, I, WHITE);

    drawProps(shadowPass);
    drawTrees(shadowPass);

    const rr::Track& tr = race.track();
    if (tr.hasPit()) {
        const RRPitInfo& p = tr.pit();
        // Painted boxes in team colours, and the garages behind the pit barrier.
        if (!shadowPass)
            for (size_t i = 0; i < race.cars().size(); ++i) {
                float bs = race.cars()[i].pitBoxS;
                const auto& sm = tr.at(tr.indexAt(bs));
                Vec2 c = tr.pointAt(bs, p.side * (sm.halfWidth + rr::Track::kBoxCentre));
                float yaw = std::atan2(sm.t.y, sm.t.x);
                drawModel(mdlCube_, boxTransform(W(c, 0.012f), {6.5f, 0.02f, 3.6f}, yaw), Color{235, 235, 235, 255});
                drawModel(mdlCube_, boxTransform(W(c, 0.02f), {6.1f, 0.02f, 3.2f}, yaw), Fade(teamColor((int)i), 1.0f));
            }
        // The garage block follows the lane in short sections, so a curved pit lane
        // doesn't have a straight building cutting across its boxes.
        const float len = std::fmod(p.lane_end_s - p.lane_start_s + tr.length(), tr.length());
        const int parts = std::max(1, (int)std::ceil((len - 16) / 12.0f));
        const float part = (len - 16) / parts;
        for (int k = 0; k < parts; ++k) {
            const float s = p.lane_start_s + 8 + part * (k + 0.5f);
            const auto& sm = tr.at(tr.indexAt(s));
            const float yaw = std::atan2(sm.t.y, sm.t.x);
            const float back = sm.halfWidth + rr::Track::kPitBarrier + 0.6f;
            const Vec2 c = tr.pointAt(s, p.side * (back + 4.5f));
            drawModel(mdlCube_, boxTransform(W(c, 2.5f), {part + 0.4f, 5.0f, 9.0f}, yaw), Color{200, 202, 208, 255});
            drawModel(mdlCube_, boxTransform(W(c, 5.2f), {part + 0.6f, 0.4f, 10.0f}, yaw), Color{40, 85, 175, 255});
        }
    }

    // Retired cars are taken away (they no longer collide), so they are not drawn either.
    for (size_t i = 0; i < race.cars().size(); ++i)
        if (!race.cars()[i].dnf) drawCar(race.cars()[i], (int)i);
}

// Shell grass: stacked see-through copies of the ground round the camera, each
// keeping only the blades that reach its height. Costs fill rate, not geometry.
void Renderer::drawGrass() {
    const int shells = quality_ >= 2 ? 12 : 6;
    const float height = 0.16f, size = 160.0f;
    Vector3 c = {std::round(camera.position.x / 2) * 2, 0, std::round(camera.position.z / 2) * 2};
    // centre the patch a little ahead of the camera, where it is looking
    Vector3 fwd = Vector3Subtract(camera.target, camera.position);
    fwd.y = 0;
    if (Vector3Length(fwd) > 0.01f) c = Vector3Add(c, Vector3Scale(Vector3Normalize(fwd), 40.0f));
    c.x = std::round(c.x / 2) * 2;
    c.z = std::round(c.z / 2) * 2;
    if (camera.position.y > 60.0f) return;  // too high to see blades
    const int maskLoc = GetShaderLocation(lit_, "grassMask"), rectLoc = GetShaderLocation(lit_, "maskRect"),
              fracLoc = GetShaderLocation(lit_, "shellFrac");
    rlEnableShader(lit_.id);
    int slot = 11;
    rlActiveTextureSlot(slot);
    rlEnableTexture(texMask_.id);
    rlSetUniform(maskLoc, &slot, SHADER_UNIFORM_INT, 1);
    rlActiveTextureSlot(0);
    SetShaderValue(lit_, rectLoc, &maskRect_, SHADER_UNIFORM_VEC4);
    rlDisableBackfaceCulling();
    for (int k = 1; k <= shells; ++k) {
        float f = (float)k / shells;
        SetShaderValue(lit_, fracLoc, &f, SHADER_UNIFORM_FLOAT);
        Matrix M = MatrixMultiply(MatrixScale(size, 1, size), MatrixTranslate(c.x, -0.08f + height * f, c.z));
        drawModel(mdlShell_, M, WHITE);
    }
    float zero = 0;
    SetShaderValue(lit_, fracLoc, &zero, SHADER_UNIFORM_FLOAT);
    rlEnableBackfaceCulling();
}

void Renderer::applyQuality(int quality) {
    quality_ = quality;
    fx_.setLevel(quality);
    const float detail = quality >= 1 ? 1.0f : 0.0f;
    for (Shader* sh : {&lit_, &litInst_}) SetShaderValue(*sh, GetShaderLocation(*sh, "detail"), &detail, SHADER_UNIFORM_FLOAT);
}

void Renderer::draw(const rr::Race& race, int focus, const ViewOptions& opt) {
    if (opt.quality != quality_) applyQuality(opt.quality);
    const rr::Car& fc = race.cars()[focus];

    // --- shadow pass: an orthographic sun camera centred between the car and the view target
    Vector3 centre = Vector3Lerp(W(fc.state.pos), camera.target, 0.5f);
    // wider sun view for the high cameras, so their whole view has shadows
    const float orthoSize = std::clamp(camera.position.y * 1.6f, 180.0f, 420.0f);
    const float texel = orthoSize / shadowRes_;
    centre.x = std::floor(centre.x / texel) * texel;
    centre.z = std::floor(centre.z / texel) * texel;
    centre.y = 0;
    Camera3D sun{};
    sun.target = centre;
    sun.position = Vector3Subtract(centre, Vector3Scale(kLightDir, 400.0f));
    sun.up = {0, 1, 0};
    sun.fovy = orthoSize;
    sun.projection = CAMERA_ORTHOGRAPHIC;
    shadowCentre_ = centre;
    shadowRadius_ = orthoSize * 0.75f;

    Matrix lightView, lightProj;
    rlSetClipPlanes(1.0, 800.0);  // tight depth range keeps the shadow bias small
    BeginTextureMode(shadowMap_);
    ClearBackground(WHITE);
    BeginMode3D(sun);
    lightView = rlGetMatrixModelview();
    lightProj = rlGetMatrixProjection();
    current_ = &depth_;
    drawScene(race, true);
    EndMode3D();
    EndTextureMode();
    rlSetClipPlanes(0.5, 5000.0);
    lightVP_ = MatrixMultiply(lightView, lightProj);

    // --- main pass (thin the fog for the high overview camera)
    float camHeight = std::max(1.0f, camera.position.y);
    float fogDensity = 0.0016f * std::clamp(40.0f / camHeight, 0.08f, 1.0f);
    SetShaderValue(lit_, locFog_, &fogDensity, SHADER_UNIFORM_FLOAT);
    SetShaderValue(litInst_, GetShaderLocation(litInst_, "fogDensity"), &fogDensity, SHADER_UNIFORM_FLOAT);
    DrawRectangleGradientV(0, 0, GetScreenWidth(), GetScreenHeight(), kSkyTop, kSkyHorizon);
    if (quality_ >= 1) {
        // sun glow in the sky, when the sun is in view
        Vector3 sunAt = Vector3Add(camera.position, Vector3Scale(kLightDir, -3000.0f));
        Vector3 fwd = Vector3Normalize(Vector3Subtract(camera.target, camera.position));
        if (Vector3DotProduct(fwd, Vector3Negate(kLightDir)) > 0.2f) {
            Vector2 sp = GetWorldToScreen(sunAt, camera);
            DrawCircleGradient((int)sp.x, (int)sp.y, GetScreenHeight() * 0.45f, Color{255, 244, 214, 120}, Color{255, 244, 214, 0});
            DrawCircleGradient((int)sp.x, (int)sp.y, 38, Color{255, 252, 240, 255}, Color{255, 248, 225, 0});
        }
    }
    BeginMode3D(camera);
    SetShaderValueMatrix(lit_, locLightVP_, lightVP_);
    SetShaderValue(lit_, locViewPos_, &camera.position, SHADER_UNIFORM_VEC3);
    SetShaderValueMatrix(litInst_, GetShaderLocation(litInst_, "lightVP"), lightVP_);
    SetShaderValue(litInst_, GetShaderLocation(litInst_, "viewPos"), &camera.position, SHADER_UNIFORM_VEC3);
    rlEnableShader(lit_.id);
    int slot = 10;
    rlActiveTextureSlot(slot);
    rlEnableTexture(shadowMap_.depth.id);
    rlSetUniform(locShadowMap_, &slot, SHADER_UNIFORM_INT, 1);
    rlActiveTextureSlot(0);
    current_ = &lit_;
    drawScene(race, false);
    if (quality_ >= 1) drawGrass();
    fx_.draw(race, camera.position);

    // --- debug overlays (unlit)
    if (opt.showPaths) {
        static std::vector<float> xy(2 * 8192);
        for (size_t i = 0; i < race.cars().size(); ++i) {
            const rr::Car& c = race.cars()[i];
            if (!c.driver || !c.driver->hasDebugPath() || c.dnf) continue;
            int count = c.driver->debugPath(xy.data(), (int)xy.size() / 2);
            Color col = Fade(teamColor((int)i), 0.85f);
            float h = 0.06f + 0.01f * i;
            for (int k = 0; k + 1 < count; ++k)
                DrawLine3D({xy[2 * k], h, -xy[2 * k + 1]}, {xy[2 * k + 2], h, -xy[2 * k + 3]}, col);
        }
    }
    if (opt.showSensors) {
        const auto& s = fc.sensors;
        Vector3 o = W(fc.state.pos, 0.5f);
        for (int k = 0; k < RR_NUM_TRACK_SENSORS; ++k) {
            if (s.track[k] < 0) continue;
            float ang = fc.state.yaw + fc.robotCfg.track_sensor_angles[k] * DEG2RAD;
            Vec2 d = rr::fromAngle(ang) * s.track[k];
            Vector3 e = W(fc.state.pos + d, 0.5f);
            DrawLine3D(o, e, Color{255, 230, 60, 200});
            DrawSphere(e, 0.25f, Color{255, 120, 40, 255});
        }
    }
    EndMode3D();
}
