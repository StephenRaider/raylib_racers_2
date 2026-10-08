#include "gfx.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

#include "mini_json.hpp"
#include "raymath.h"
#include "rlgl.h"

// A few OpenGL calls rlgl does not wrap (multisampled targets), from the GLFW
// that raylib links.
extern "C" void* glfwGetProcAddress(const char* name);

namespace gfx {

namespace {

namespace gl {
typedef void (*GenFn)(int, unsigned*);
typedef void (*BindFn)(unsigned, unsigned);
typedef void (*StorageMsFn)(unsigned, int, unsigned, int, int);
typedef void (*FbRbFn)(unsigned, unsigned, unsigned, unsigned);
typedef void (*BlitFn)(int, int, int, int, int, int, int, int, unsigned, unsigned);
typedef void (*CapFn)(unsigned);
typedef void (*DelFn)(int, const unsigned*);
GenFn genRenderbuffers;
BindFn bindRenderbuffer, bindFramebuffer;
StorageMsFn renderbufferStorageMultisample;
FbRbFn framebufferRenderbuffer;
BlitFn blitFramebuffer;
CapFn enable, disable;
typedef void (*TexParamFn)(unsigned, unsigned, int);
TexParamFn texParameteri;
DelFn deleteRenderbuffers;
const unsigned RENDERBUFFER = 0x8D41, FRAMEBUFFER = 0x8D40, READ_FRAMEBUFFER = 0x8CA8, DRAW_FRAMEBUFFER = 0x8CA9;
const unsigned COLOR_ATTACHMENT0 = 0x8CE0, DEPTH_ATTACHMENT = 0x8D00, RGBA16F = 0x881A, DEPTH_COMPONENT24 = 0x81A6;
const unsigned COLOR_BUFFER_BIT = 0x4000, NEAREST = 0x2600, SAMPLE_ALPHA_TO_COVERAGE = 0x809E;
bool load() {
    genRenderbuffers = (GenFn)glfwGetProcAddress("glGenRenderbuffers");
    deleteRenderbuffers = (DelFn)glfwGetProcAddress("glDeleteRenderbuffers");
    bindRenderbuffer = (BindFn)glfwGetProcAddress("glBindRenderbuffer");
    bindFramebuffer = (BindFn)glfwGetProcAddress("glBindFramebuffer");
    renderbufferStorageMultisample = (StorageMsFn)glfwGetProcAddress("glRenderbufferStorageMultisample");
    framebufferRenderbuffer = (FbRbFn)glfwGetProcAddress("glFramebufferRenderbuffer");
    blitFramebuffer = (BlitFn)glfwGetProcAddress("glBlitFramebuffer");
    enable = (CapFn)glfwGetProcAddress("glEnable");
    disable = (CapFn)glfwGetProcAddress("glDisable");
    texParameteri = (TexParamFn)glfwGetProcAddress("glTexParameteri");
    return genRenderbuffers && deleteRenderbuffers && bindRenderbuffer && bindFramebuffer && renderbufferStorageMultisample &&
           framebufferRenderbuffer && blitFramebuffer && enable && disable && texParameteri;
}
}  // namespace gl

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

const char* kPbrInstVS = R"(#version 330
in vec3 vertexPosition;
in vec2 vertexTexCoord;
in vec3 vertexNormal;
in vec4 vertexColor;
in vec4 vertexTangent;
layout(location = 10) in mat4 instanceTransform;  // clear of the mesh attributes (0-7)
uniform mat4 mvp;  // view * projection: the model matrix comes per instance
uniform float depthBias;
out vec3 fragPos;
out vec3 fragNormal;
out vec4 fragTangent;
out vec2 fragUV;
out vec4 fragColor;
void main() {
    mat4 M = instanceTransform;
    vec3 tint = vec3(M[0][3], M[1][3], M[2][3]);
    M[0][3] = 0.0; M[1][3] = 0.0; M[2][3] = 0.0;
    vec4 wp = M * vec4(vertexPosition, 1.0);
    fragPos = wp.xyz;
    fragNormal = normalize(mat3(M) * vertexNormal);
    fragTangent = vec4(normalize(mat3(M) * vertexTangent.xyz), vertexTangent.w);
    fragUV = vertexTexCoord;
    fragColor = vertexColor * vec4(tint, 1.0);
    gl_Position = mvp * wp;
    gl_Position.z -= depthBias * 1e-6 * gl_Position.w;
}
)";

const char* kDepthInstVS = R"(#version 330
in vec3 vertexPosition;
in vec2 vertexTexCoord;
layout(location = 10) in mat4 instanceTransform;
uniform mat4 mvp;
out vec2 uv;
void main() {
    mat4 M = instanceTransform;
    M[0][3] = 0.0; M[1][3] = 0.0; M[2][3] = 0.0;
    uv = vertexTexCoord;
    gl_Position = mvp * (M * vec4(vertexPosition, 1.0));
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
uniform sampler2DShadow shadowMap0, shadowMap1;
uniform sampler2D specAtlas;
uniform sampler2D skyMap;  // the full-resolution sky, for mirror-like reflections (car paint)
uniform int layers;
uniform vec3 layerScale;
uniform vec3 tint;
uniform vec3 layerTint[3];
uniform float roughMul, metalMul, normalStrength, clearcoat, ccRough;
uniform int macroVariation;
uniform float alphaCut;
uniform float translucency;
uniform int vertexTint;
uniform int alphaToCoverage;
uniform int detile;
uniform vec3 viewPos;
uniform mat4 lightVP[2];
uniform vec3 sunDir;      // towards the sun
uniform vec3 sunColor;    // irradiance
uniform vec3 sh[9];       // sky irradiance, spherical harmonics (already scaled)
uniform float specScale;  // sky intensity for reflections
uniform float fogDensity;
uniform vec3 fogColor;    // the sky's mean radiance at the horizon
uniform sampler2D colorMap;
uniform vec4 colorMapRect;
uniform vec2 colorMapRange;
uniform int useColorMap;
uniform vec4 blob[8];   // car footprints: x, z, yaw, ground height
uniform int blobCount;

// Soft occlusion of the ground under and around the cars (an F1 car sits 3 cm off it).
float carOcclusion() {
    float occ = 1.0;
    for (int i = 0; i < blobCount; ++i) {
        vec4 b = blob[i];
        if (fragPos.y > b.w + 0.25) continue;  // the car itself, not the ground
        vec2 d = fragPos.xz - b.xy;
        float c = cos(b.z), s = sin(b.z);
        // into the car's frame: x along the car, y across (world x/z -> sim x/-y)
        vec2 l = vec2(c * d.x - s * d.y, -s * d.x - c * d.y);
        vec2 q = abs(l) - vec2(2.3, 0.75);
        float dist = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0);
        occ *= 1.0 - 0.75 * (1.0 - smoothstep(-0.5, 0.9, dist));
    }
    return occ;
}
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
    if (rough < 0.06) {
        // clear coat and chrome: the sky itself, sharp, blending into the first blurred level
        vec3 sharp = textureLod(skyMap, uv, 0.0).rgb;
        return mix(sharp, specLevel(uv, 0), rough / 0.06) * specScale;
    }
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

// Each tap is a hardware 2x2 comparison (linear-filtered depth compare), so a 3x3
// pattern of them is a soft 4x4 filter.
float shadowIn(sampler2DShadow map, mat4 vp, vec3 n, float offset, float bias, int r, out bool inside) {
    vec4 lp = vp * vec4(fragPos + n * offset, 1.0);
    vec3 p = lp.xyz / lp.w * 0.5 + 0.5;
    inside = p.x > 0.02 && p.x < 0.98 && p.y > 0.02 && p.y < 0.98 && p.z < 1.0;
    if (!inside) return 1.0;
    vec2 texel = 1.0 / vec2(textureSize(map, 0));
    float lit = 0.0, count = 0.0;
    for (int x = -r; x <= r; ++x)
        for (int y = -r; y <= r; ++y) {
            lit += texture(map, vec3(p.xy + vec2(x, y) * texel * 1.5, p.z - bias));
            count += 1.0;
        }
    return lit / count;
}

// Two cascades: the sharp one near the camera, then the coarse one out to ~1 km.
float shadow(vec3 n) {
    bool inside;
    float s0 = shadowIn(shadowMap0, lightVP[0], n, 0.08, 0.00008, 1, inside);
    if (inside) return s0;
    return shadowIn(shadowMap1, lightVP[1], n, 0.5, 0.00006, 0, inside);
}

// a cheap arithmetic hash (no sin)
float hash(vec2 p) {
    vec3 q = fract(vec3(p.xyx) * 0.1031);
    q += dot(q, q.yzx + 33.33);
    return fract((q.x + q.y) * q.z);
}
float vnoise(vec2 p) {
    vec2 i = floor(p), f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(hash(i), hash(i + vec2(1, 0)), f.x), mix(hash(i + vec2(0, 1)), hash(i + vec2(1, 1)), f.x), f.y);
}

struct Surface { vec3 albedo; vec3 n; vec3 orm; };
bool farAway = false;

// near: albedo, normal and ORM (twice with macro variation); far (detail false) the
// normal map is too small to see and is skipped, and only the albedo gets the second sample.
void sampleLayer(sampler2D a, sampler2D nm, sampler2D o, float scale, vec3 lt, float w, bool detail, inout Surface s) {
    if (w <= 0.001) return;
    vec2 uv = fragUV / scale;
    if (farAway) {
        // hundreds of metres off: one blurred albedo fetch, roughness from its mip average
        s.albedo += pow(textureLod(a, uv * 0.31 + 0.37, 6.0).rgb, vec3(2.2)) * lt * w;
        s.n += vec3(0.0, 0.0, 1.0) * w;
        s.orm += textureLod(o, uv, 8.0).rgb * w;
        return;
    }
    vec3 al, or, nn;
    if (detile != 0) {
        // No visible tiling (after Inigo Quilez's "texture repetition"): a noise picks one
        // of eight random offsets per patch, blended across the patch borders.
        float k = vnoise(uv * 0.45) * 8.0;
        float ia = floor(k), f = fract(k);
        vec2 oa = sin(vec2(3.0, 7.0) * ia), ob = sin(vec2(3.0, 7.0) * (ia + 1.0));
        vec2 dx = dFdx(uv), dy = dFdy(uv);
        vec3 a0 = textureGrad(a, uv + oa, dx, dy).rgb, a1 = textureGrad(a, uv + ob, dx, dy).rgb;
        float b = smoothstep(0.2, 0.8, f - 0.1 * dot(a0 - a1, vec3(1.0)));
        al = mix(a0, a1, b);
        or = mix(textureGrad(o, uv + oa, dx, dy).rgb, textureGrad(o, uv + ob, dx, dy).rgb, b);
        nn = detail ? mix(textureGrad(nm, uv + oa, dx, dy).rgb, textureGrad(nm, uv + ob, dx, dy).rgb, b) : vec3(0.5, 0.5, 1.0);
        // and broad patches of older, paler and fresher, darker tarmac
        al *= 0.86 + 0.24 * vnoise(fragPos.xz * 0.035) + 0.08 * vnoise(fragPos.xz * 0.3);
    } else {
        al = texture(a, uv).rgb;
        or = texture(o, uv).rgb;
        nn = detail ? texture(nm, uv).rgb : vec3(0.5, 0.5, 1.0);
    }
    if (macroVariation != 0) {
        // a second, rotated sample at a larger scale hides the tiling
        vec2 uv2 = mat2(0.8, -0.6, 0.6, 0.8) * uv * 0.31 + 0.37;
        al = mix(al, texture(a, uv2).rgb, 0.45);
        if (detail) {
            nn = mix(nn, texture(nm, uv2).rgb, 0.45);
            or = mix(or, texture(o, uv2).rgb, 0.45);
        }
    }
    s.albedo += pow(al, vec3(2.2)) * lt * w;
    s.n += (nn * 2.0 - 1.0) * w;
    s.orm += or * w;
}

void main() {
    float outAlpha = 1.0;
    if (alphaCut > 0.0) {
        // mipmaps average thin leaves and wires away: boost alpha with the mip level
        vec2 uv0 = fragUV / layerScale.x;
        vec2 dx = dFdx(uv0 * vec2(textureSize(albedo0, 0))), dy = dFdy(uv0 * vec2(textureSize(albedo0, 0)));
        float lod = 0.5 * log2(max(max(dot(dx, dx), dot(dy, dy)), 1e-8));
        float a = texture(albedo0, uv0).a * (1.0 + 0.12 * max(lod, 0.0));
        if (alphaToCoverage != 0) {
            // a sharp edge one pixel wide, which the multisampling turns into coverage
            outAlpha = clamp((a - alphaCut) / max(fwidth(a), 1e-4) + 0.5, 0.0, 1.0);
            if (outAlpha <= 0.0) discard;
        } else if (a < alphaCut) discard;
    }
    vec3 wts = layers > 1 ? fragColor.rgb : vec3(1.0, 0.0, 0.0);
    wts /= max(wts.r + wts.g + wts.b, 1e-4);
    Surface s = Surface(vec3(0.0), vec3(0.0), vec3(0.0));
    float camDist = length(viewPos - fragPos);
    bool detail = camDist < 90.0;
    // far off, a baked colour map stands in for the texture layers (and their aliasing)
    float cmix = useColorMap != 0 ? smoothstep(colorMapRange.x, colorMapRange.y, camDist) : 0.0;
    if (cmix < 1.0) {
        sampleLayer(albedo0, normal0, orm0, layerScale.x, layerTint[0], wts.r, detail, s);
        if (layers > 1) sampleLayer(albedo1, normal1, orm1, layerScale.y, layerTint[1], wts.g, detail, s);
        if (layers > 2) sampleLayer(albedo2, normal2, orm2, layerScale.z, layerTint[2], wts.b, detail, s);
    }
    if (cmix > 0.0) {
        vec3 c = pow(texture(colorMap, (fragPos.xz - colorMapRect.xy) * colorMapRect.zw).rgb, vec3(2.2));
        s.albedo = mix(s.albedo, c, cmix);
        s.n = mix(s.n, vec3(0.0, 0.0, 1.0), cmix);
        s.orm = mix(s.orm, vec3(1.0, 0.95, 0.0), cmix);
    }
    vec3 albedo = s.albedo * tint;
    if (vertexTint != 0) albedo *= fragColor.rgb;
    if (macroVariation != 0) {
        float m = vnoise(fragPos.xz * 0.025) * 0.6 + vnoise(fragPos.xz * 0.11) * 0.4;
        albedo *= 0.82 + 0.36 * m;
    }
    float carOcc = blobCount > 0 ? carOcclusion() : 1.0;
    float ao = s.orm.r * fragColor.a * carOcc;
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
    float shRaw = (NoL > 0.0 || translucency > 0.0) ? shadow(normalize(fragNormal)) : 0.0;
    float sh = (NoL > 0.0 ? shRaw : 0.0) * mix(1.0, carOcc, 0.6);
    // the shading normal can face the sun when the surface does not: no light there
    sh *= smoothstep(-0.05, 0.05, dot(normalize(fragNormal), L));
    vec3 F = F_Schlick(f0, VoH);
    vec3 spec = D_GGX(NoH, a) * V_Smith(NoV, NoL, a) * F;
    vec3 kd = (1.0 - F) * diffuseColor / PI;
    float Fc = 0.04 + 0.96 * pow(1.0 - VoH, 5.0);
    float ac = ccRough * ccRough;
    float cc = clearcoat * D_GGX(NoH, ac) * V_Smith(NoV, NoL, ac) * Fc;
    col += (kd + spec) * (1.0 - clearcoat * Fc) * sunColor * NoL * sh + cc * sunColor * NoL * sh;
    if (translucency > 0.0) {
        // light through the leaves: the shady side of a tree glows a little towards the sun
        float back = max(dot(-n, L), 0.0) * (0.5 + 0.5 * pow(max(dot(-V, L), 0.0), 4.0));
        col += diffuseColor / PI * sunColor * back * translucency * shRaw * fragColor.a;
    }

    // sky: diffuse from the SH, reflections from the prefiltered atlas
    vec3 E = irradianceSH(n);
    vec2 ab = envBRDF(rough, NoV);
    vec3 R = reflect(-V, n);
    // reflections of the ground below the horizon are darker than the sky there
    float horizon = clamp(1.0 + dot(R, normalize(fragNormal)) * 1.5, 0.0, 1.0);
    // rough surfaces reflect a blur of the whole sky: the SH irradiance is close enough
    // and saves the reflection lookup
    vec3 envSpec = (rough > 0.8 ? irradianceSH(R) / PI * specScale : prefiltered(R, rough)) * (f0 * ab.x + ab.y) * horizon * horizon;
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
    vec3 fogCol = fogColor + sunColor * 0.05 * pow(max(dot(-V, L), 0.0), 8.0);
    col = mix(col, fogCol, fog);
    finalColor = vec4(col, outAlpha);
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

const char* kDepthCutVS = R"(#version 330
in vec3 vertexPosition;
in vec2 vertexTexCoord;
uniform mat4 mvp;
out vec2 uv;
void main() { uv = vertexTexCoord; gl_Position = mvp * vec4(vertexPosition, 1.0); }
)";
const char* kDepthCutFS = R"(#version 330
in vec2 uv;
uniform sampler2D tex;
uniform float alphaCut;
uniform float uvScale;
out vec4 finalColor;
void main() {
    vec2 dx = dFdx(uv / uvScale * vec2(textureSize(tex, 0))), dy = dFdy(uv / uvScale * vec2(textureSize(tex, 0)));
    float lod = 0.5 * log2(max(max(dot(dx, dx), dot(dy, dy)), 1e-8));
    if (texture(tex, uv / uvScale).a * (1.0 + 0.12 * max(lod, 0.0)) < alphaCut) discard;
    finalColor = vec4(1.0);
}
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

TextureSet flatTextureSet(Texture2D albedo, float roughness, float metalness) {
    TextureSet t;
    t.albedo = albedo;
    Image n = GenImageColor(1, 1, Color{128, 128, 255, 255});
    Image o = GenImageColor(1, 1, Color{255, (unsigned char)(roughness * 255), (unsigned char)(metalness * 255), 255});
    t.normal = LoadTextureFromImage(n);
    t.orm = LoadTextureFromImage(o);
    UnloadImage(n);
    UnloadImage(o);
    return t;
}

void unloadTextureSet(TextureSet& t) {
    for (Texture2D* x : {&t.albedo, &t.normal, &t.orm})
        if (x->id) UnloadTexture(*x), x->id = 0;
}

// ---------------------------------------------------------------- renderer

void Renderer::lookUp(Shader& s, Locs& l) {
    auto U = [&](const char* n) { return GetShaderLocation(s, n); };
    l.mvp = U("mvp"); l.model = U("matModel"); l.normalMat = U("matNormal"); l.viewPos = U("viewPos");
    l.lightVP = U("lightVP[0]"); l.sunDir = U("sunDir"); l.sunColor = U("sunColor"); l.sh = U("sh");
    l.skyYaw = U("skyYaw"); l.fog = U("fogDensity"); l.specMax = U("specScale");
    l.layerScale = U("layerScale"); l.tint = U("tint"); l.layerTint = U("layerTint"); l.roughMul = U("roughMul"); l.metalMul = U("metalMul");
    l.normalStrength = U("normalStrength"); l.clearcoat = U("clearcoat"); l.ccRough = U("ccRough");
    l.macro = U("macroVariation"); l.layers = U("layers"); l.depthBias = U("depthBias");
    l.alphaCut = U("alphaCut"); l.translucency = U("translucency"); l.vertexTint = U("vertexTint");
    l.a2c = U("alphaToCoverage");
    l.fogColor = U("fogColor");
    l.detile = U("detile");
    l.colorMap = U("colorMap"); l.colorMapRect = U("colorMapRect"); l.colorMapRange = U("colorMapRange");
    l.useColorMap = U("useColorMap");
    const char* texNames[9] = {"albedo0", "normal0", "orm0", "albedo1", "normal1", "orm1", "albedo2", "normal2", "orm2"};
    for (int i = 0; i < 9; ++i) l.tex[i] = U(texNames[i]);
    l.shadow[0] = U("shadowMap0");
    l.shadow[1] = U("shadowMap1");
    l.spec = U("specAtlas");
    l.skyMap = U("skyMap");
    l.blob = U("blob[0]");
    l.blobCount = U("blobCount");
}

bool Renderer::init(int width, int height, std::string* err, int msaa) {
    msaa_ = gl::load() ? std::max(1, msaa) : 1;
    pbr_ = LoadShaderFromMemory(kPbrVS, withSkyCommon(kPbrFS).c_str());
    pbrInst_ = LoadShaderFromMemory(kPbrInstVS, withSkyCommon(kPbrFS).c_str());
    depthInst_ = LoadShaderFromMemory(kDepthInstVS, kDepthCutFS);
    depth_s_ = LoadShaderFromMemory(kDepthVS, kDepthFS);
    depthCut_ = LoadShaderFromMemory(kDepthCutVS, kDepthCutFS);
    sky_ = LoadShaderFromMemory(kSkyVS, withSkyCommon(kSkyFS).c_str());
    tonemap_ = LoadShaderFromMemory(nullptr, kTonemapFS);
    for (Shader* s : {&pbr_, &pbrInst_, &depth_s_, &depthCut_, &depthInst_, &sky_, &tonemap_})
        if (s->id == 0 || s->id == rlGetShaderIdDefault()) {
            if (err) *err = "shader compilation failed (OpenGL 3.3 required); see the log above";
            return false;
        }
    lookUp(pbr_, L_);
    lookUp(pbrInst_, Li_);

    // Sun shadow map: a depth texture.
    for (int c = 0; c < kCascades; ++c) {
        shadowFbo_[c] = rlLoadFramebuffer();
        shadowTex_[c] = rlLoadTextureDepth(shadowRes_[c], shadowRes_[c], false);
        rlFramebufferAttach(shadowFbo_[c], shadowTex_[c], RL_ATTACHMENT_DEPTH, RL_ATTACHMENT_TEXTURE2D, 0);
        if (!rlFramebufferComplete(shadowFbo_[c])) {
            if (err) *err = "could not create the shadow-map framebuffers";
            return false;
        }
        // linear-filtered depth comparisons (sampler2DShadow)
        rlTextureParameters(shadowTex_[c], RL_TEXTURE_MIN_FILTER, RL_TEXTURE_FILTER_LINEAR);
        rlTextureParameters(shadowTex_[c], RL_TEXTURE_MAG_FILTER, RL_TEXTURE_FILTER_LINEAR);
        rlEnableTexture(shadowTex_[c]);
        gl::texParameteri(0x0DE1 /* TEXTURE_2D */, 0x884C /* COMPARE_MODE */, 0x884E /* COMPARE_REF_TO_TEXTURE */);
        gl::texParameteri(0x0DE1, 0x884D /* COMPARE_FUNC */, 0x0203 /* LEQUAL */);
        rlDisableTexture();
    }

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
    if (msaa_ > 1) {
        // the scene renders here, multisampled, then resolves into fbo_'s colour texture
        msFbo_ = rlLoadFramebuffer();
        gl::genRenderbuffers(1, &msColor_);
        gl::genRenderbuffers(1, &msDepth_);
        gl::bindRenderbuffer(gl::RENDERBUFFER, msColor_);
        gl::renderbufferStorageMultisample(gl::RENDERBUFFER, msaa_, gl::RGBA16F, w_, h_);
        gl::bindRenderbuffer(gl::RENDERBUFFER, msDepth_);
        gl::renderbufferStorageMultisample(gl::RENDERBUFFER, msaa_, gl::DEPTH_COMPONENT24, w_, h_);
        gl::bindRenderbuffer(gl::RENDERBUFFER, 0);
        gl::bindFramebuffer(gl::FRAMEBUFFER, msFbo_);
        gl::framebufferRenderbuffer(gl::FRAMEBUFFER, gl::COLOR_ATTACHMENT0, gl::RENDERBUFFER, msColor_);
        gl::framebufferRenderbuffer(gl::FRAMEBUFFER, gl::DEPTH_ATTACHMENT, gl::RENDERBUFFER, msDepth_);
        bool ok = rlFramebufferComplete(msFbo_);
        gl::bindFramebuffer(gl::FRAMEBUFFER, 0);
        if (!ok) {  // no multisampling then
            gl::deleteRenderbuffers(1, &msColor_);
            gl::deleteRenderbuffers(1, &msDepth_);
            rlUnloadFramebuffer(msFbo_);
            msFbo_ = msColor_ = msDepth_ = 0;
            msaa_ = 1;
        }
    }
}

void Renderer::freeTargets() {
    if (fbo_) rlUnloadFramebuffer(fbo_);  // also frees the attachments
    fbo_ = color_ = depth_ = 0;
    if (msFbo_) {
        gl::deleteRenderbuffers(1, &msColor_);
        gl::deleteRenderbuffers(1, &msDepth_);
        rlUnloadFramebuffer(msFbo_);
        msFbo_ = msColor_ = msDepth_ = 0;
    }
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
    for (int c = 0; c < kCascades; ++c) {
        if (shadowFbo_[c]) rlUnloadFramebuffer(shadowFbo_[c]);
        shadowFbo_[c] = 0;
    }
    for (Texture2D* t : {&skyTex_, &specTex_})
        if (t->id) UnloadTexture(*t), t->id = 0;
    for (Shader* s : {&pbr_, &pbrInst_, &depth_s_, &depthCut_, &depthInst_, &sky_, &tonemap_})
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
    {
        // fog colour: the mean of the blurriest level's horizon row (16 x 8 texels at y 496)
        Vector3 sum = {0, 0, 0};
        const int y = 496 + 4;
        for (int x = 0; x < 16; ++x) sum = Vector3Add(sum, {rgb[(y * 512 + x) * 3], rgb[(y * 512 + x) * 3 + 1], rgb[(y * 512 + x) * 3 + 2]});
        fogColor_ = Vector3Scale(sum, 0.9f / 16);
    }

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

void Renderer::beginShadows(Vector3 focus, float radius, int cascade) {
    cascade_ = cascade;
    rlDrawRenderBatchActive();
    Sun s = sun();
    // Snap the focus to whole shadow-map texels across the sun's view, so the shadows
    // do not crawl and shimmer as the camera moves.
    {
        Vector3 f = Vector3Negate(s.dir);
        Vector3 right = Vector3Normalize(Vector3CrossProduct(f, {0, 1, 0}));
        Vector3 up = Vector3CrossProduct(right, f);
        float texel = 2 * radius / shadowRes_[cascade];
        float fx = Vector3DotProduct(focus, right), fy = Vector3DotProduct(focus, up);
        focus = Vector3Add(focus, Vector3Add(Vector3Scale(right, std::round(fx / texel) * texel - fx),
                                             Vector3Scale(up, std::round(fy / texel) * texel - fy)));
    }
    Vector3 eye = Vector3Add(focus, Vector3Scale(s.dir, radius * 3));
    Matrix view = MatrixLookAt(eye, focus, {0, 1, 0});
    Matrix proj = MatrixOrtho(-radius, radius, -radius, radius, radius * 0.5, radius * 6);
    lightVP_[cascade] = MatrixMultiply(view, proj);
    rlEnableFramebuffer(shadowFbo_[cascade]);
    rlViewport(0, 0, shadowRes_[cascade], shadowRes_[cascade]);
    rlClearScreenBuffers();
    rlEnableDepthTest();
}

void Renderer::drawShadow(const Mesh& m, Matrix model, unsigned alphaTex, float alphaCut, float uvScale) {
    Matrix mvp = MatrixMultiply(model, lightVP_[cascade_]);
    if (alphaTex) {
        rlEnableShader(depthCut_.id);
        rlSetUniformMatrix(GetShaderLocation(depthCut_, "mvp"), mvp);
        rlSetUniform(GetShaderLocation(depthCut_, "alphaCut"), &alphaCut, RL_SHADER_UNIFORM_FLOAT, 1);
        rlSetUniform(GetShaderLocation(depthCut_, "uvScale"), &uvScale, RL_SHADER_UNIFORM_FLOAT, 1);
        int slot = 0;
        rlActiveTextureSlot(0);
        rlEnableTexture(alphaTex);
        rlSetUniform(GetShaderLocation(depthCut_, "tex"), &slot, RL_SHADER_UNIFORM_INT, 1);
        rlDisableBackfaceCulling();
    } else {
        rlEnableShader(depth_s_.id);
        rlSetUniformMatrix(GetShaderLocation(depth_s_, "mvp"), mvp);
    }
    rlEnableVertexArray(m.vaoId);
    if (m.indices) rlDrawVertexArrayElements(0, m.triangleCount * 3, 0);
    else rlDrawVertexArray(0, m.vertexCount);
    rlDisableVertexArray();
    if (alphaTex) {
        rlDisableTexture();
        rlEnableBackfaceCulling();
    }
}

bool Renderer::boxInClip(const BoundingBox& b, const Matrix& m) {
    // outside when all eight corners are beyond the same clip plane
    int out[6] = {0, 0, 0, 0, 0, 0};
    for (int k = 0; k < 8; ++k) {
        float x = (k & 1) ? b.max.x : b.min.x, y = (k & 2) ? b.max.y : b.min.y, z = (k & 4) ? b.max.z : b.min.z;
        float cx = m.m0 * x + m.m4 * y + m.m8 * z + m.m12;
        float cy = m.m1 * x + m.m5 * y + m.m9 * z + m.m13;
        float cz = m.m2 * x + m.m6 * y + m.m10 * z + m.m14;
        float cw = m.m3 * x + m.m7 * y + m.m11 * z + m.m15;
        out[0] += cx < -cw; out[1] += cx > cw;
        out[2] += cy < -cw; out[3] += cy > cw;
        out[4] += cz < -cw; out[5] += cz > cw;
    }
    for (int i = 0; i < 6; ++i)
        if (out[i] == 8) return false;
    return true;
}

void Renderer::endShadows() {
    rlDisableShader();
    rlDisableFramebuffer();
}

void Renderer::beginScene(const Camera3D& cam) {
    cam_ = cam;
    view_ = MatrixLookAt(cam.position, cam.target, cam.up);
    proj_ = MatrixPerspective(cam.fovy * DEG2RAD, (double)w_ / h_, 0.5, 15000.0);
    viewProj_ = MatrixMultiply(view_, proj_);
    rlEnableFramebuffer(msFbo_ ? msFbo_ : fbo_);
    rlViewport(0, 0, w_, h_);
    rlClearColor(0, 0, 0, 255);
    rlClearScreenBuffers();
    rlEnableDepthTest();
    rlEnableBackfaceCulling();
}

int Renderer::bindMaterial(const Material& mat, const Locs& L, Matrix model, Matrix mvp) {
    rlSetUniformMatrix(L.mvp, mvp);
    if (L.model >= 0) rlSetUniformMatrix(L.model, model);
    if (L.normalMat >= 0) rlSetUniformMatrix(L.normalMat, MatrixTranspose(MatrixInvert(model)));
    rlSetUniformMatrix(L.lightVP, lightVP_[0]);
    rlSetUniformMatrix(L.lightVP + 1, lightVP_[1]);
    rlSetUniform(L.viewPos, &cam_.position, RL_SHADER_UNIFORM_VEC3, 1);
    Sun s = sun();
    rlSetUniform(L.sunDir, &s.dir, RL_SHADER_UNIFORM_VEC3, 1);
    rlSetUniform(L.sunColor, &s.irradiance, RL_SHADER_UNIFORM_VEC3, 1);
    Vector3 sh[9];
    for (int i = 0; i < 9; ++i) sh[i] = Vector3Scale(sh_[i], skyIntensity);
    rlSetUniform(L.sh, sh, RL_SHADER_UNIFORM_VEC3, 9);
    rlSetUniform(L.skyYaw, &skyYaw, RL_SHADER_UNIFORM_FLOAT, 1);
    rlSetUniform(L.fog, &fogDensity, RL_SHADER_UNIFORM_FLOAT, 1);
    Vector3 fc = Vector3Scale(fogColor_, skyIntensity);
    rlSetUniform(L.fogColor, &fc, RL_SHADER_UNIFORM_VEC3, 1);
    rlSetUniform(L.specMax, &skyIntensity, RL_SHADER_UNIFORM_FLOAT, 1);
    int layers = 0;
    while (layers < 3 && mat.layer[layers]) ++layers;
    rlSetUniform(L.layers, &layers, RL_SHADER_UNIFORM_INT, 1);
    rlSetUniform(L.layerScale, mat.scale, RL_SHADER_UNIFORM_VEC3, 1);
    rlSetUniform(L.tint, &mat.tint, RL_SHADER_UNIFORM_VEC3, 1);
    rlSetUniform(L.layerTint, mat.layerTint, RL_SHADER_UNIFORM_VEC3, 3);
    rlSetUniform(L.roughMul, &mat.roughness, RL_SHADER_UNIFORM_FLOAT, 1);
    rlSetUniform(L.metalMul, &mat.metalness, RL_SHADER_UNIFORM_FLOAT, 1);
    rlSetUniform(L.normalStrength, &mat.normalStrength, RL_SHADER_UNIFORM_FLOAT, 1);
    rlSetUniform(L.clearcoat, &mat.clearcoat, RL_SHADER_UNIFORM_FLOAT, 1);
    rlSetUniform(L.ccRough, &mat.clearcoatRoughness, RL_SHADER_UNIFORM_FLOAT, 1);
    rlSetUniform(L.depthBias, &mat.depthBias, RL_SHADER_UNIFORM_FLOAT, 1);
    rlSetUniform(L.alphaCut, &mat.alphaCut, RL_SHADER_UNIFORM_FLOAT, 1);
    rlSetUniform(L.translucency, &mat.translucency, RL_SHADER_UNIFORM_FLOAT, 1);
    int vtint = mat.vertexTint ? 1 : 0;
    rlSetUniform(L.vertexTint, &vtint, RL_SHADER_UNIFORM_INT, 1);
    int macro = mat.macroVariation ? 1 : 0;
    rlSetUniform(L.macro, &macro, RL_SHADER_UNIFORM_INT, 1);
    {
        int n = (int)std::min(blobs.size(), (size_t)kBlobs);
        rlSetUniform(L.blobCount, &n, RL_SHADER_UNIFORM_INT, 1);
        if (n) rlSetUniform(L.blob, blobs.data(), RL_SHADER_UNIFORM_VEC4, n);
    }
    int det = mat.detile ? 1 : 0;
    rlSetUniform(L.detile, &det, RL_SHADER_UNIFORM_INT, 1);
    int useCm = mat.colorMap ? 1 : 0;
    rlSetUniform(L.useColorMap, &useCm, RL_SHADER_UNIFORM_INT, 1);
    rlSetUniform(L.colorMapRect, &mat.colorMapRect, RL_SHADER_UNIFORM_VEC4, 1);
    rlSetUniform(L.colorMapRange, &mat.colorMapRange, RL_SHADER_UNIFORM_VEC2, 1);
    int a2c = (mat.alphaCut > 0 && msFbo_) ? 1 : 0;
    rlSetUniform(L.a2c, &a2c, RL_SHADER_UNIFORM_INT, 1);
    if (a2c) gl::enable(gl::SAMPLE_ALPHA_TO_COVERAGE);

    int slot = 0;
    auto bind = [&](int loc, unsigned id) {
        rlActiveTextureSlot(slot);
        rlEnableTexture(id);
        rlSetUniform(loc, &slot, RL_SHADER_UNIFORM_INT, 1);
        ++slot;
    };
    for (int l = 0; l < 3; ++l) {
        const TextureSet* t = mat.layer[l] ? mat.layer[l] : mat.layer[0];
        bind(L.tex[l * 3 + 0], t->albedo.id);
        bind(L.tex[l * 3 + 1], t->normal.id);
        bind(L.tex[l * 3 + 2], t->orm.id);
    }
    bind(L.shadow[0], shadowTex_[0]);
    bind(L.shadow[1], shadowTex_[1]);
    bind(L.spec, specTex_.id);
    bind(L.skyMap, skyTex_.id);
    bind(L.colorMap, mat.colorMap ? mat.colorMap : specTex_.id);
    if (mat.doubleSided) rlDisableBackfaceCulling();
    return slot;
}

void Renderer::unbindMaterial(int slots, const Material& mat) {
    if (mat.doubleSided) rlEnableBackfaceCulling();
    if (mat.alphaCut > 0 && msFbo_) gl::disable(gl::SAMPLE_ALPHA_TO_COVERAGE);
    for (int i = slots - 1; i >= 0; --i) {
        rlActiveTextureSlot(i);
        rlDisableTexture();
    }
    rlActiveTextureSlot(0);
    rlDisableShader();
}

void Renderer::draw(const Mesh& m, const Material& mat, Matrix model) {
    rlEnableShader(pbr_.id);
    int slots = bindMaterial(mat, L_, model, MatrixMultiply(model, viewProj_));
    rlEnableVertexArray(m.vaoId);
    if (m.indices) rlDrawVertexArrayElements(0, m.triangleCount * 3, 0);
    else rlDrawVertexArray(0, m.vertexCount);
    rlDisableVertexArray();
    unbindMaterial(slots, mat);
}

namespace {
// Points the instance attribute (a mat4 in locations 10-13) at a vertex buffer.
void bindInstances(unsigned vbo) {
    rlEnableVertexBuffer(vbo);
    for (int i = 0; i < 4; ++i) {
        rlEnableVertexAttribute(10 + i);
        rlSetVertexAttribute(10 + i, 4, RL_FLOAT, false, 64, i * 16);
        rlSetVertexAttributeDivisor(10 + i, 1);
    }
}
void unbindInstances() {
    for (int i = 0; i < 4; ++i) rlDisableVertexAttribute(10 + i);
    rlDisableVertexBuffer();
}
}  // namespace

void Renderer::drawInstanced(const Mesh& m, const Material& mat, unsigned instances, int count) {
    if (count <= 0) return;
    rlEnableShader(pbrInst_.id);
    int slots = bindMaterial(mat, Li_, MatrixIdentity(), viewProj_);
    rlEnableVertexArray(m.vaoId);
    bindInstances(instances);
    rlDrawVertexArrayElementsInstanced(0, m.triangleCount * 3, 0, count);
    unbindInstances();
    rlDisableVertexArray();
    unbindMaterial(slots, mat);
}

void Renderer::drawShadowInstanced(const Mesh& m, unsigned instances, int count, unsigned alphaTex, float alphaCut) {
    if (count <= 0) return;
    rlEnableShader(depthInst_.id);
    rlSetUniformMatrix(GetShaderLocation(depthInst_, "mvp"), lightVP_[cascade_]);
    rlSetUniform(GetShaderLocation(depthInst_, "alphaCut"), &alphaCut, RL_SHADER_UNIFORM_FLOAT, 1);
    float one = 1.0f;
    rlSetUniform(GetShaderLocation(depthInst_, "uvScale"), &one, RL_SHADER_UNIFORM_FLOAT, 1);
    int slot = 0;
    rlActiveTextureSlot(0);
    rlEnableTexture(alphaTex);
    rlSetUniform(GetShaderLocation(depthInst_, "tex"), &slot, RL_SHADER_UNIFORM_INT, 1);
    rlDisableBackfaceCulling();
    rlEnableVertexArray(m.vaoId);
    bindInstances(instances);
    rlDrawVertexArrayElementsInstanced(0, m.triangleCount * 3, 0, count);
    unbindInstances();
    rlDisableVertexArray();
    rlDisableTexture();
    rlEnableBackfaceCulling();
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
    if (msFbo_) {
        gl::bindFramebuffer(gl::READ_FRAMEBUFFER, msFbo_);
        gl::bindFramebuffer(gl::DRAW_FRAMEBUFFER, fbo_);
        gl::blitFramebuffer(0, 0, w_, h_, 0, 0, w_, h_, gl::COLOR_BUFFER_BIT, gl::NEAREST);
    }
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
