#include "gfx.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

#include "mini_json.hpp"
#include "raymath.h"
#include "rlgl.h"

namespace gfx {

namespace {

// ---------------------------------------------------------------- shaders

const char* kPbrVS = R"(#version 330
in vec3 vertexPosition;
in vec2 vertexTexCoord;
in vec3 vertexNormal;
in vec4 vertexColor;
in vec4 vertexTangent;
uniform mat4 mvp;
uniform mat4 matModel;
uniform mat4 matNormal;
uniform float depthBias;
out vec3 fragPos;
out vec3 fragNormal;
out vec4 fragTangent;
out vec2 fragUV;
out vec4 fragColor;
void main() {
    fragPos = vec3(matModel * vec4(vertexPosition, 1.0));
    fragNormal = normalize(mat3(matNormal) * vertexNormal);
    fragTangent = vec4(normalize(mat3(matModel) * vertexTangent.xyz), vertexTangent.w);
    fragUV = vertexTexCoord;
    fragColor = vertexColor;
    gl_Position = mvp * vec4(vertexPosition, 1.0);
    gl_Position.z -= depthBias * 1e-6 * gl_Position.w;
}
)";

// Shared sky lookups: equirectangular, row 0 at the zenith, u = 0.5 + atan(x, -z) / 2pi.
const char* kSkyCommon = R"(
const float PI = 3.14159265;
uniform float skyYaw;
vec3 skyRotate(vec3 d) {
    float c = cos(skyYaw), s = sin(skyYaw);
    return vec3(c * d.x + s * d.z, d.y, -s * d.x + c * d.z);
}
vec2 equirect(vec3 d) {
    d = skyRotate(normalize(d));
    return vec2(0.5 + atan(d.x, -d.z) / (2.0 * PI), acos(clamp(d.y, -1.0, 1.0)) / PI);
}
)";

const char* kPbrFS = R"(
in vec3 fragPos;
in vec3 fragNormal;
in vec4 fragTangent;
in vec2 fragUV;
in vec4 fragColor;
uniform sampler2D albedo0, normal0, orm0, albedo1, normal1, orm1, albedo2, normal2, orm2;
uniform sampler2D shadowMap;
uniform sampler2D specAtlas;
uniform int layers;
uniform vec3 layerScale;
uniform vec3 tint;
uniform vec3 layerTint[3];
uniform float roughMul, metalMul, normalStrength, clearcoat, ccRough;
uniform int macroVariation;
uniform vec3 viewPos;
uniform mat4 lightVP;
uniform vec3 sunDir;      // towards the sun
uniform vec3 sunColor;    // irradiance
uniform vec3 sh[9];       // sky irradiance, spherical harmonics (already scaled)
uniform float specScale;  // sky intensity for reflections
uniform float fogDensity;
out vec4 finalColor;

vec3 irradianceSH(vec3 n) {
    n = skyRotate(n);
    float x = n.x, y = n.y, z = n.z;
    vec3 e = sh[0] * 0.282095 + sh[1] * 0.488603 * y + sh[2] * 0.488603 * z + sh[3] * 0.488603 * x
           + sh[4] * 1.092548 * x * y + sh[5] * 1.092548 * y * z + sh[6] * 0.315392 * (3.0 * z * z - 1.0)
           + sh[7] * 1.092548 * x * z + sh[8] * 0.546274 * (x * x - y * y);
    return max(e, vec3(0.0));
}

vec3 specLevel(vec2 uv, int k) {
    float w = float(512 >> k), h = float(256 >> k), y0 = float(512 - (512 >> k));
    vec2 px = vec2(clamp(uv.x * w, 0.5, w - 0.5), y0 + clamp(uv.y * h, 0.5, h - 0.5));
    return textureLod(specAtlas, px / 512.0, 0.0).rgb;
}
vec3 prefiltered(vec3 r, float rough) {
    vec2 uv = equirect(r);
    float l = clamp(rough, 0.0, 1.0) * 5.0;
    int k = int(floor(l));
    return mix(specLevel(uv, k), specLevel(uv, min(k + 1, 5)), l - float(k)) * specScale;
}

// Karis' analytic fit of the split-sum environment BRDF.
vec2 envBRDF(float rough, float NoV) {
    const vec4 c0 = vec4(-1.0, -0.0275, -0.572, 0.022);
    const vec4 c1 = vec4(1.0, 0.0425, 1.04, -0.04);
    vec4 r = rough * c0 + c1;
    float a004 = min(r.x * r.x, exp2(-9.28 * NoV)) * r.x + r.y;
    return vec2(-1.04, 1.04) * a004 + r.zw;
}

float D_GGX(float NoH, float a) {
    float a2 = a * a;
    float d = NoH * NoH * (a2 - 1.0) + 1.0;
    return a2 / (PI * d * d);
}
float V_Smith(float NoV, float NoL, float a) {
    float k = a * 0.5;
    return 0.25 / ((NoV * (1.0 - k) + k) * (NoL * (1.0 - k) + k));
}
vec3 F_Schlick(vec3 f0, float VoH) { return f0 + (1.0 - f0) * pow(1.0 - VoH, 5.0); }

float shadow(vec3 n) {
    vec4 lp = lightVP * vec4(fragPos + n * 0.08, 1.0);
    vec3 p = lp.xyz / lp.w * 0.5 + 0.5;
    if (p.x <= 0.0 || p.x >= 1.0 || p.y <= 0.0 || p.y >= 1.0 || p.z >= 1.0) return 1.0;
    float bias = 0.00008;
    vec2 texel = 1.0 / vec2(textureSize(shadowMap, 0));
    float lit = 0.0;
    for (int x = -2; x <= 2; ++x)
        for (int y = -2; y <= 2; ++y)
            lit += (p.z - bias > texture(shadowMap, p.xy + vec2(x, y) * texel).r) ? 0.0 : 1.0;
    lit /= 25.0;
    vec2 edge = min(p.xy, 1.0 - p.xy);
    return mix(1.0, lit, clamp(min(edge.x, edge.y) * 20.0, 0.0, 1.0));
}

float hash(vec2 p) { return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453); }
float vnoise(vec2 p) {
    vec2 i = floor(p), f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(hash(i), hash(i + vec2(1, 0)), f.x), mix(hash(i + vec2(0, 1)), hash(i + vec2(1, 1)), f.x), f.y);
}

struct Surface { vec3 albedo; vec3 n; vec3 orm; };

void sampleLayer(sampler2D a, sampler2D nm, sampler2D o, float scale, vec3 lt, float w, inout Surface s) {
    if (w <= 0.001) return;
    vec2 uv = fragUV / scale;
    vec3 al = texture(a, uv).rgb, nn = texture(nm, uv).rgb, or = texture(o, uv).rgb;
    if (macroVariation != 0) {
        // a second, rotated sample at a larger scale hides the tiling
        vec2 uv2 = mat2(0.8, -0.6, 0.6, 0.8) * uv * 0.31 + 0.37;
        al = mix(al, texture(a, uv2).rgb, 0.45);
        nn = mix(nn, texture(nm, uv2).rgb, 0.45);
        or = mix(or, texture(o, uv2).rgb, 0.45);
    }
    s.albedo += pow(al, vec3(2.2)) * lt * w;
    s.n += (nn * 2.0 - 1.0) * w;
    s.orm += or * w;
}

void main() {
    vec3 wts = layers > 1 ? fragColor.rgb : vec3(1.0, 0.0, 0.0);
    wts /= max(wts.r + wts.g + wts.b, 1e-4);
    Surface s = Surface(vec3(0.0), vec3(0.0), vec3(0.0));
    sampleLayer(albedo0, normal0, orm0, layerScale.x, layerTint[0], wts.r, s);
    if (layers > 1) sampleLayer(albedo1, normal1, orm1, layerScale.y, layerTint[1], wts.g, s);
    if (layers > 2) sampleLayer(albedo2, normal2, orm2, layerScale.z, layerTint[2], wts.b, s);
    vec3 albedo = s.albedo * tint;
    if (macroVariation != 0) {
        float m = vnoise(fragPos.xz * 0.025) * 0.6 + vnoise(fragPos.xz * 0.11) * 0.4;
        albedo *= 0.82 + 0.36 * m;
    }
    float ao = s.orm.r * fragColor.a;
    float rough = clamp(s.orm.g * roughMul, 0.04, 1.0);
    float metal = clamp(s.orm.b * metalMul, 0.0, 1.0);

    vec3 N = normalize(fragNormal);
    if (!gl_FrontFacing) N = -N;
    vec3 T = normalize(fragTangent.xyz - N * dot(N, fragTangent.xyz));
    vec3 B = cross(N, T) * fragTangent.w;
    vec3 tn = s.n;
    tn.xy *= normalStrength;
    vec3 n = normalize(mat3(T, B, N) * tn);

    vec3 V = normalize(viewPos - fragPos);
    float NoV = max(dot(n, V), 1e-4);
    vec3 f0 = mix(vec3(0.04), albedo, metal);
    vec3 diffuseColor = albedo * (1.0 - metal);
    float a = rough * rough;

    // sun
    vec3 L = normalize(sunDir);
    vec3 H = normalize(L + V);
    float NoL = max(dot(n, L), 0.0);
    float NoH = max(dot(n, H), 0.0), VoH = max(dot(V, H), 0.0);
    vec3 col = vec3(0.0);
    float sh = NoL > 0.0 ? shadow(normalize(fragNormal)) : 0.0;
    // the shading normal can face the sun when the surface does not: no light there
    sh *= smoothstep(-0.05, 0.05, dot(normalize(fragNormal), L));
    vec3 F = F_Schlick(f0, VoH);
    vec3 spec = D_GGX(NoH, a) * V_Smith(NoV, NoL, a) * F;
    vec3 kd = (1.0 - F) * diffuseColor / PI;
    float Fc = 0.04 + 0.96 * pow(1.0 - VoH, 5.0);
    float ac = ccRough * ccRough;
    float cc = clearcoat * D_GGX(NoH, ac) * V_Smith(NoV, NoL, ac) * Fc;
    col += (kd + spec) * (1.0 - clearcoat * Fc) * sunColor * NoL * sh + cc * sunColor * NoL * sh;

    // sky: diffuse from the SH, reflections from the prefiltered atlas
    vec3 E = irradianceSH(n);
    vec2 ab = envBRDF(rough, NoV);
    vec3 R = reflect(-V, n);
    // reflections of the ground below the horizon are darker than the sky there
    float horizon = clamp(1.0 + dot(R, normalize(fragNormal)) * 1.5, 0.0, 1.0);
    vec3 envSpec = prefiltered(R, rough) * (f0 * ab.x + ab.y) * horizon * horizon;
    float specAO = clamp(pow(NoV + ao, exp2(-16.0 * a - 1.0)) - 1.0 + ao, 0.0, 1.0);
    vec3 amb = diffuseColor * E / PI * ao + envSpec * specAO;
    if (clearcoat > 0.0) {
        float FcV = 0.04 + 0.96 * pow(1.0 - NoV, 5.0);
        vec3 Rc = reflect(-V, normalize(fragNormal));
        amb = amb * (1.0 - clearcoat * FcV) + clearcoat * FcV * prefiltered(Rc, ccRough) * specAO;
    }
    col += amb;

    // aerial perspective: fade to the blurred sky along the view ray, warmer towards the sun
    float dist = length(viewPos - fragPos);
    float fog = 1.0 - exp(-dist * fogDensity);
    vec3 fogCol = prefiltered(-V, 1.0) * 0.9 + sunColor * 0.05 * pow(max(dot(-V, L), 0.0), 8.0);
    col = mix(col, fogCol, fog);
    finalColor = vec4(col, 1.0);
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

const char* kSkyVS = R"(#version 330
in vec3 vertexPosition;
uniform mat4 mvp;   // projection * rotation-only view
out vec3 dir;
void main() {
    dir = vertexPosition;
    vec4 p = mvp * vec4(vertexPosition, 1.0);
    gl_Position = p.xyww;  // on the far plane
}
)";
const char* kSkyFS = R"(
in vec3 dir;
uniform sampler2D skyTex;
uniform float skyScale;
uniform vec3 sunDir;
uniform vec3 sunColor;
out vec4 finalColor;
void main() {
    vec3 d = normalize(dir);
    vec3 c = texture(skyTex, equirect(d)).rgb * skyScale;
    // the sun disc (it was taken out of the image), about 0.53 degrees across
    float cosA = dot(d, normalize(sunDir));
    float disc = smoothstep(0.99996, 0.99999, cosA);
    c += sunColor * disc * 20000.0 + sunColor * pow(max(cosA, 0.0), 600.0) * 2.0;
    finalColor = vec4(c, 1.0);
}
)";

const char* kTonemapFS = R"(#version 330
in vec2 fragTexCoord;
uniform sampler2D texture0;
uniform float exposure;
out vec4 finalColor;
vec3 aces(vec3 x) {
    const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}
void main() {
    vec3 hdr = texture(texture0, fragTexCoord).rgb * exposure;
    vec3 c = aces(hdr);
    c = pow(c, vec3(1.0 / 2.2));
    vec2 q = fragTexCoord - 0.5;
    c *= 1.0 - dot(q, q) * 0.35;  // a gentle vignette
    finalColor = vec4(c, 1.0);
}
)";

std::string withSkyCommon(const char* body) { return std::string("#version 330\n") + kSkyCommon + body; }

// RGBE PNG (see tools/import_assets.py) -> float RGB.
bool loadRGBE(const std::string& path, std::vector<float>* rgb, int* w, int* h) {
    Image img = LoadImage(path.c_str());
    if (!img.data) return false;
    ImageFormat(&img, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
    *w = img.width;
    *h = img.height;
    const unsigned char* p = (const unsigned char*)img.data;
    rgb->resize((size_t)img.width * img.height * 3);
    for (int i = 0; i < img.width * img.height; ++i) {
        const unsigned char* q = p + i * 4;
        float f = q[3] ? std::ldexp(1.0f, q[3] - 136) : 0.0f;
        (*rgb)[i * 3 + 0] = q[0] * f;
        (*rgb)[i * 3 + 1] = q[1] * f;
        (*rgb)[i * 3 + 2] = q[2] * f;
    }
    UnloadImage(img);
    return true;
}

Texture2D floatTexture(const std::vector<float>& rgb, int w, int h, bool repeatU) {
    Texture2D t{};
    t.id = rlLoadTexture(rgb.data(), w, h, PIXELFORMAT_UNCOMPRESSED_R32G32B32, 1);
    t.width = w;
    t.height = h;
    t.mipmaps = 1;
    t.format = PIXELFORMAT_UNCOMPRESSED_R32G32B32;
    rlTextureParameters(t.id, RL_TEXTURE_MIN_FILTER, RL_TEXTURE_FILTER_LINEAR);
    rlTextureParameters(t.id, RL_TEXTURE_MAG_FILTER, RL_TEXTURE_FILTER_LINEAR);
    rlTextureParameters(t.id, RL_TEXTURE_WRAP_S, repeatU ? RL_TEXTURE_WRAP_REPEAT : RL_TEXTURE_WRAP_CLAMP);
    rlTextureParameters(t.id, RL_TEXTURE_WRAP_T, RL_TEXTURE_WRAP_CLAMP);
    return t;
}

Texture2D loadTex(const std::string& path) {
    Texture2D t = LoadTexture(path.c_str());
    if (t.id) {
        GenTextureMipmaps(&t);
        SetTextureFilter(t, TEXTURE_FILTER_ANISOTROPIC_16X);
        SetTextureWrap(t, TEXTURE_WRAP_REPEAT);
    }
    return t;
}

Vector3 vec3Of(const mjson::Value& v, Vector3 def) {
    if (v.type != mjson::Value::Array || v.size() < 3) return def;
    return {(float)v[0].num(), (float)v[1].num(), (float)v[2].num()};
}

}  // namespace

// ---------------------------------------------------------------- textures

bool loadTextureSet(const std::string& dir, TextureSet* out, std::string* err) {
    out->albedo = loadTex(dir + "/albedo.jpg");
    out->normal = loadTex(dir + "/normal.jpg");
    out->orm = loadTex(dir + "/orm.jpg");
    if (!out->albedo.id || !out->normal.id || !out->orm.id) {
        if (err) *err = "missing textures in " + dir;
        unloadTextureSet(*out);
        return false;
    }
    return true;
}

void unloadTextureSet(TextureSet& t) {
    for (Texture2D* x : {&t.albedo, &t.normal, &t.orm})
        if (x->id) UnloadTexture(*x), x->id = 0;
}

// ---------------------------------------------------------------- renderer

bool Renderer::init(int width, int height, std::string* err) {
    pbr_ = LoadShaderFromMemory(kPbrVS, withSkyCommon(kPbrFS).c_str());
    depth_s_ = LoadShaderFromMemory(kDepthVS, kDepthFS);
    sky_ = LoadShaderFromMemory(kSkyVS, withSkyCommon(kSkyFS).c_str());
    tonemap_ = LoadShaderFromMemory(nullptr, kTonemapFS);
    for (Shader* s : {&pbr_, &depth_s_, &sky_, &tonemap_})
        if (s->id == 0 || s->id == rlGetShaderIdDefault()) {
            if (err) *err = "shader compilation failed (OpenGL 3.3 required); see the log above";
            return false;
        }
    auto U = [&](const char* n) { return GetShaderLocation(pbr_, n); };
    L_.mvp = U("mvp"); L_.model = U("matModel"); L_.normalMat = U("matNormal"); L_.viewPos = U("viewPos");
    L_.lightVP = U("lightVP"); L_.sunDir = U("sunDir"); L_.sunColor = U("sunColor"); L_.sh = U("sh");
    L_.skyYaw = U("skyYaw"); L_.fog = U("fogDensity"); L_.specMax = U("specScale");
    L_.layerScale = U("layerScale"); L_.tint = U("tint"); L_.layerTint = U("layerTint"); L_.roughMul = U("roughMul"); L_.metalMul = U("metalMul");
    L_.normalStrength = U("normalStrength"); L_.clearcoat = U("clearcoat"); L_.ccRough = U("ccRough");
    L_.macro = U("macroVariation"); L_.layers = U("layers"); L_.depthBias = U("depthBias");
    const char* texNames[9] = {"albedo0", "normal0", "orm0", "albedo1", "normal1", "orm1", "albedo2", "normal2", "orm2"};
    for (int i = 0; i < 9; ++i) L_.tex[i] = U(texNames[i]);
    L_.shadow = U("shadowMap");
    L_.spec = U("specAtlas");

    // Sun shadow map: a depth texture.
    shadowFbo_ = rlLoadFramebuffer();
    shadowTex_ = rlLoadTextureDepth(shadowRes_, shadowRes_, false);
    rlFramebufferAttach(shadowFbo_, shadowTex_, RL_ATTACHMENT_DEPTH, RL_ATTACHMENT_TEXTURE2D, 0);
    if (!rlFramebufferComplete(shadowFbo_)) {
        if (err) *err = "could not create the shadow-map framebuffer";
        return false;
    }
    rlTextureParameters(shadowTex_, RL_TEXTURE_MIN_FILTER, RL_TEXTURE_FILTER_NEAREST);
    rlTextureParameters(shadowTex_, RL_TEXTURE_MAG_FILTER, RL_TEXTURE_FILTER_NEAREST);

    cube_ = GenMeshCube(2, 2, 2);
    w_ = width;
    h_ = height;
    createTargets();
    if (!rlFramebufferComplete(fbo_)) {
        if (err) *err = "could not create the HDR framebuffer";
        return false;
    }
    return true;
}

void Renderer::createTargets() {
    fbo_ = rlLoadFramebuffer();
    color_ = rlLoadTexture(nullptr, w_, h_, PIXELFORMAT_UNCOMPRESSED_R16G16B16A16, 1);
    rlTextureParameters(color_, RL_TEXTURE_MIN_FILTER, RL_TEXTURE_FILTER_LINEAR);
    rlTextureParameters(color_, RL_TEXTURE_MAG_FILTER, RL_TEXTURE_FILTER_LINEAR);
    depth_ = rlLoadTextureDepth(w_, h_, true);
    rlFramebufferAttach(fbo_, color_, RL_ATTACHMENT_COLOR_CHANNEL0, RL_ATTACHMENT_TEXTURE2D, 0);
    rlFramebufferAttach(fbo_, depth_, RL_ATTACHMENT_DEPTH, RL_ATTACHMENT_RENDERBUFFER, 0);
}

void Renderer::freeTargets() {
    if (fbo_) rlUnloadFramebuffer(fbo_);  // also frees the attachments
    fbo_ = color_ = depth_ = 0;
}

void Renderer::resize(int width, int height) {
    if (width == w_ && height == h_) return;
    freeTargets();
    w_ = width;
    h_ = height;
    createTargets();
}

void Renderer::shutdown() {
    freeTargets();
    if (shadowFbo_) rlUnloadFramebuffer(shadowFbo_);
    shadowFbo_ = 0;
    for (Texture2D* t : {&skyTex_, &specTex_})
        if (t->id) UnloadTexture(*t), t->id = 0;
    for (Shader* s : {&pbr_, &depth_s_, &sky_, &tonemap_})
        if (s->id) UnloadShader(*s), s->id = 0;
    if (cube_.vertexCount) UnloadMesh(cube_), cube_ = Mesh{};
}

bool Renderer::loadSky(const std::string& dir, const std::string& name, std::string* err) {
    std::vector<float> rgb;
    int w = 0, h = 0;
    if (!loadRGBE(dir + "/" + name + ".png", &rgb, &w, &h)) {
        if (err) *err = "cannot load sky " + dir + "/" + name + ".png";
        return false;
    }
    if (skyTex_.id) UnloadTexture(skyTex_);
    skyTex_ = floatTexture(rgb, w, h, true);
    if (!loadRGBE(dir + "/" + name + "_spec.png", &rgb, &w, &h) || w != 512 || h != 512) {
        if (err) *err = "cannot load " + dir + "/" + name + "_spec.png (512 x 512 expected)";
        return false;
    }
    if (specTex_.id) UnloadTexture(specTex_);
    specTex_ = floatTexture(rgb, w, h, false);

    std::ifstream in(dir + "/" + name + ".json");
    std::stringstream ss;
    ss << in.rdbuf();
    mjson::Value j = mjson::parse(ss.str());
    if (j.type != mjson::Value::Object) {
        if (err) *err = "cannot read " + dir + "/" + name + ".json";
        return false;
    }
    const auto& sh = j["sh9"];
    for (int i = 0; i < 9; ++i) sh_[i] = vec3Of(sh[i], {0, 0, 0});
    hasSun_ = j["sun_dir"].type == mjson::Value::Array;
    skySun_.dir = vec3Of(j["sun_dir"], {0.3f, 0.6f, -0.7f});
    // overcast: a weak, high diffuse "sun" keeps some modelling and soft shadows
    skySun_.irradiance = hasSun_ ? vec3Of(j["sun_irradiance"], {0, 0, 0}) : Vector3{0.6f, 0.6f, 0.62f};
    if (!hasSun_) skySun_.dir = Vector3Normalize({0.2f, 0.9f, -0.3f});
    return true;
}

Sun Renderer::sun() const {
    Sun s = skySun_;
    // the sky texture is looked up with skyRotate(d): the sun's world direction is the inverse
    float c = std::cos(skyYaw), sn = std::sin(skyYaw);
    Vector3 d = s.dir;
    s.dir = Vector3Normalize({c * d.x - sn * d.z, d.y, sn * d.x + c * d.z});
    s.irradiance = Vector3Scale(s.irradiance, sunIntensity);
    return s;
}

void Renderer::beginShadows(Vector3 focus, float radius) {
    rlDrawRenderBatchActive();
    Sun s = sun();
    Vector3 eye = Vector3Add(focus, Vector3Scale(s.dir, radius * 3));
    Matrix view = MatrixLookAt(eye, focus, {0, 1, 0});
    Matrix proj = MatrixOrtho(-radius, radius, -radius, radius, radius * 0.5, radius * 6);
    lightVP_ = MatrixMultiply(view, proj);
    rlEnableFramebuffer(shadowFbo_);
    rlViewport(0, 0, shadowRes_, shadowRes_);
    rlClearScreenBuffers();
    rlEnableDepthTest();
    rlEnableShader(depth_s_.id);
}

void Renderer::drawShadow(const Mesh& m, Matrix model) {
    Matrix mvp = MatrixMultiply(model, lightVP_);
    rlSetUniformMatrix(GetShaderLocation(depth_s_, "mvp"), mvp);
    rlEnableVertexArray(m.vaoId);
    if (m.indices) rlDrawVertexArrayElements(0, m.triangleCount * 3, 0);
    else rlDrawVertexArray(0, m.vertexCount);
    rlDisableVertexArray();
}

void Renderer::endShadows() {
    rlDisableShader();
    rlDisableFramebuffer();
}

void Renderer::beginScene(const Camera3D& cam) {
    cam_ = cam;
    view_ = MatrixLookAt(cam.position, cam.target, cam.up);
    proj_ = MatrixPerspective(cam.fovy * DEG2RAD, (double)w_ / h_, 0.5, 15000.0);
    rlEnableFramebuffer(fbo_);
    rlViewport(0, 0, w_, h_);
    rlClearColor(0, 0, 0, 255);
    rlClearScreenBuffers();
    rlEnableDepthTest();
    rlEnableBackfaceCulling();
}

void Renderer::draw(const Mesh& m, const Material& mat, Matrix model) {
    rlEnableShader(pbr_.id);
    Matrix mvp = MatrixMultiply(model, MatrixMultiply(view_, proj_));
    rlSetUniformMatrix(L_.mvp, mvp);
    rlSetUniformMatrix(L_.model, model);
    rlSetUniformMatrix(L_.normalMat, MatrixTranspose(MatrixInvert(model)));
    rlSetUniformMatrix(L_.lightVP, lightVP_);
    rlSetUniform(L_.viewPos, &cam_.position, RL_SHADER_UNIFORM_VEC3, 1);
    Sun s = sun();
    rlSetUniform(L_.sunDir, &s.dir, RL_SHADER_UNIFORM_VEC3, 1);
    rlSetUniform(L_.sunColor, &s.irradiance, RL_SHADER_UNIFORM_VEC3, 1);
    Vector3 sh[9];
    for (int i = 0; i < 9; ++i) sh[i] = Vector3Scale(sh_[i], skyIntensity);
    rlSetUniform(L_.sh, sh, RL_SHADER_UNIFORM_VEC3, 9);
    rlSetUniform(L_.skyYaw, &skyYaw, RL_SHADER_UNIFORM_FLOAT, 1);
    rlSetUniform(L_.fog, &fogDensity, RL_SHADER_UNIFORM_FLOAT, 1);
    rlSetUniform(L_.specMax, &skyIntensity, RL_SHADER_UNIFORM_FLOAT, 1);
    int layers = 0;
    while (layers < 3 && mat.layer[layers]) ++layers;
    rlSetUniform(L_.layers, &layers, RL_SHADER_UNIFORM_INT, 1);
    rlSetUniform(L_.layerScale, mat.scale, RL_SHADER_UNIFORM_VEC3, 1);
    rlSetUniform(L_.tint, &mat.tint, RL_SHADER_UNIFORM_VEC3, 1);
    rlSetUniform(L_.layerTint, mat.layerTint, RL_SHADER_UNIFORM_VEC3, 3);
    rlSetUniform(L_.roughMul, &mat.roughness, RL_SHADER_UNIFORM_FLOAT, 1);
    rlSetUniform(L_.metalMul, &mat.metalness, RL_SHADER_UNIFORM_FLOAT, 1);
    rlSetUniform(L_.normalStrength, &mat.normalStrength, RL_SHADER_UNIFORM_FLOAT, 1);
    rlSetUniform(L_.clearcoat, &mat.clearcoat, RL_SHADER_UNIFORM_FLOAT, 1);
    rlSetUniform(L_.ccRough, &mat.clearcoatRoughness, RL_SHADER_UNIFORM_FLOAT, 1);
    rlSetUniform(L_.depthBias, &mat.depthBias, RL_SHADER_UNIFORM_FLOAT, 1);
    int macro = mat.macroVariation ? 1 : 0;
    rlSetUniform(L_.macro, &macro, RL_SHADER_UNIFORM_INT, 1);

    int slot = 0;
    auto bind = [&](int loc, unsigned id) {
        rlActiveTextureSlot(slot);
        rlEnableTexture(id);
        rlSetUniform(loc, &slot, RL_SHADER_UNIFORM_INT, 1);
        ++slot;
    };
    for (int l = 0; l < 3; ++l) {
        const TextureSet* t = mat.layer[l] ? mat.layer[l] : mat.layer[0];
        bind(L_.tex[l * 3 + 0], t->albedo.id);
        bind(L_.tex[l * 3 + 1], t->normal.id);
        bind(L_.tex[l * 3 + 2], t->orm.id);
    }
    bind(L_.shadow, shadowTex_);
    bind(L_.spec, specTex_.id);

    if (mat.doubleSided) rlDisableBackfaceCulling();
    rlEnableVertexArray(m.vaoId);
    if (m.indices) rlDrawVertexArrayElements(0, m.triangleCount * 3, 0);
    else rlDrawVertexArray(0, m.vertexCount);
    rlDisableVertexArray();
    if (mat.doubleSided) rlEnableBackfaceCulling();
    for (int i = slot - 1; i >= 0; --i) {
        rlActiveTextureSlot(i);
        rlDisableTexture();
    }
    rlActiveTextureSlot(0);
    rlDisableShader();
}

void Renderer::drawSky() {
    rlEnableShader(sky_.id);
    Matrix rot = view_;
    rot.m12 = rot.m13 = rot.m14 = 0;
    Matrix mvp = MatrixMultiply(rot, proj_);
    rlSetUniformMatrix(GetShaderLocation(sky_, "mvp"), mvp);
    rlSetUniform(GetShaderLocation(sky_, "skyYaw"), &skyYaw, RL_SHADER_UNIFORM_FLOAT, 1);
    rlSetUniform(GetShaderLocation(sky_, "skyScale"), &skyIntensity, RL_SHADER_UNIFORM_FLOAT, 1);
    Sun s = sun();
    if (!hasSun_) s.irradiance = {0, 0, 0};
    rlSetUniform(GetShaderLocation(sky_, "sunDir"), &s.dir, RL_SHADER_UNIFORM_VEC3, 1);
    rlSetUniform(GetShaderLocation(sky_, "sunColor"), &s.irradiance, RL_SHADER_UNIFORM_VEC3, 1);
    int slot = 0;
    rlActiveTextureSlot(0);
    rlEnableTexture(skyTex_.id);
    rlSetUniform(GetShaderLocation(sky_, "skyTex"), &slot, RL_SHADER_UNIFORM_INT, 1);
    rlDisableBackfaceCulling();
    rlDisableDepthMask();
    rlEnableVertexArray(cube_.vaoId);
    if (cube_.indices) rlDrawVertexArrayElements(0, cube_.triangleCount * 3, 0);
    else rlDrawVertexArray(0, cube_.vertexCount);
    rlDisableVertexArray();
    rlEnableDepthMask();
    rlEnableBackfaceCulling();
    rlDisableTexture();
    rlDisableShader();
}

void Renderer::endScene() {
    rlDisableFramebuffer();
}

void Renderer::present(int x, int y, int width, int height) {
    Texture2D t{color_, w_, h_, 1, PIXELFORMAT_UNCOMPRESSED_R16G16B16A16};
    SetShaderValue(tonemap_, GetShaderLocation(tonemap_, "exposure"), &exposure, SHADER_UNIFORM_FLOAT);
    BeginShaderMode(tonemap_);
    // render targets are stored bottom-up
    DrawTexturePro(t, {0, 0, (float)w_, (float)-h_}, {(float)x, (float)y, (float)width, (float)height}, {0, 0}, 0, WHITE);
    EndShaderMode();
}

// ---------------------------------------------------------------- meshes

int MeshBuilder::add(const Vertex& v) {
    if (v_.size() >= 65535) flush();
    v_.push_back(v);
    return (int)v_.size() - 1;
}

void MeshBuilder::tri(int a, int b, int c) {
    i_.push_back((unsigned short)a);
    i_.push_back((unsigned short)b);
    i_.push_back((unsigned short)c);
}

bool MeshBuilder::reserve(int needed) {
    if ((int)v_.size() + needed <= 65535) return false;
    flush();
    return true;
}

void MeshBuilder::flush() {
    ++chunk_;
    if (!v_.empty() && !i_.empty()) done_.push_back({std::move(v_), std::move(i_)});
    v_.clear();
    i_.clear();
}

std::vector<Mesh> MeshBuilder::build() {
    flush();
    std::vector<Mesh> out;
    for (Chunk& c : done_) {
        computeTangents(c.v, c.i);
        Mesh m{};
        m.vertexCount = (int)c.v.size();
        m.triangleCount = (int)c.i.size() / 3;
        const unsigned nv = (unsigned)m.vertexCount;
        m.vertices = (float*)MemAlloc(nv * 3 * sizeof(float));
        m.normals = (float*)MemAlloc(nv * 3 * sizeof(float));
        m.tangents = (float*)MemAlloc(nv * 4 * sizeof(float));
        m.texcoords = (float*)MemAlloc(nv * 2 * sizeof(float));
        m.texcoords2 = (float*)MemAlloc(nv * 2 * sizeof(float));
        m.colors = (unsigned char*)MemAlloc(nv * 4);
        m.indices = (unsigned short*)MemAlloc((unsigned)(c.i.size() * sizeof(unsigned short)));
        for (int k = 0; k < m.vertexCount; ++k) {
            const Vertex& v = c.v[k];
            std::memcpy(m.vertices + k * 3, &v.p, 12);
            std::memcpy(m.normals + k * 3, &v.n, 12);
            std::memcpy(m.tangents + k * 4, &v.t, 16);
            std::memcpy(m.texcoords + k * 2, &v.uv, 8);
            std::memcpy(m.texcoords2 + k * 2, &v.uv2, 8);
            std::memcpy(m.colors + k * 4, v.c, 4);
        }
        std::memcpy(m.indices, c.i.data(), c.i.size() * sizeof(unsigned short));
        UploadMesh(&m, false);
        out.push_back(m);
    }
    done_.clear();
    return out;
}

void computeTangents(std::vector<Vertex>& v, const std::vector<unsigned short>& idx) {
    std::vector<Vector3> tan(v.size(), {0, 0, 0}), bit(v.size(), {0, 0, 0});
    for (size_t k = 0; k + 2 < idx.size(); k += 3) {
        int a = idx[k], b = idx[k + 1], c = idx[k + 2];
        Vector3 e1 = Vector3Subtract(v[b].p, v[a].p), e2 = Vector3Subtract(v[c].p, v[a].p);
        Vector2 d1 = Vector2Subtract(v[b].uv, v[a].uv), d2 = Vector2Subtract(v[c].uv, v[a].uv);
        float det = d1.x * d2.y - d2.x * d1.y;
        if (std::fabs(det) < 1e-12f) continue;
        float r = 1.0f / det;
        Vector3 t = Vector3Scale(Vector3Subtract(Vector3Scale(e1, d2.y), Vector3Scale(e2, d1.y)), r);
        Vector3 bt = Vector3Scale(Vector3Subtract(Vector3Scale(e2, d1.x), Vector3Scale(e1, d2.x)), r);
        for (int i : {a, b, c}) {
            tan[i] = Vector3Add(tan[i], t);
            bit[i] = Vector3Add(bit[i], bt);
        }
    }
    for (size_t i = 0; i < v.size(); ++i) {
        Vector3 n = v[i].n;
        Vector3 t = Vector3Subtract(tan[i], Vector3Scale(n, Vector3DotProduct(n, tan[i])));
        if (Vector3Length(t) < 1e-8f) {
            // no UV gradient: any vector across the normal will do
            t = std::fabs(n.x) < 0.9f ? Vector3CrossProduct(n, {1, 0, 0}) : Vector3CrossProduct(n, {0, 0, 1});
        }
        t = Vector3Normalize(t);
        float w = Vector3DotProduct(Vector3CrossProduct(n, t), bit[i]) < 0 ? -1.0f : 1.0f;
        v[i].t = {t.x, t.y, t.z, w};
    }
}

}  // namespace gfx
