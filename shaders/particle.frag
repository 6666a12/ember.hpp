#version 430 core
layout(location = 0) in vec2 vUV;
layout(location = 1) in vec4 vColor;
layout(location = 2) in float vFade;
layout(location = 3) in vec3 vFadeRGB;
layout(location = 4) in float vViewZ;
layout(location = 5) in vec3 vFacetNormal;
layout(location = 6) in float vAngle;         // billboard rotation (rad), matches the quad orientation
layout(location = 7) flat in uint vShardSeed; // per-particle hash: polygon shard silhouette
layout(location = 0) out vec4 frag;

// Depth convention: GL clip space maps NDC z in [-1,1]; Vulkan uses [0,1].
// The host depth texture and gl_FragCoord.z follow the active convention, so
// both reconstruction paths stay correct by picking one conversion helper.
#if defined(EMBER_CLIP_VULKAN)
#define EMBER_DEPTH_TO_NDC(store) (store)
#else
#define EMBER_DEPTH_TO_NDC(store) ((store) * 2.0 - 1.0)
#endif

// Samplers carry explicit bindings (required for SPIR-V; GL 4.2+ honors
// them, so the host no longer needs glUniform1i for texture units).
// Vulkan moves render resources to a dedicated set (see particle.vert); GL
// keeps the legacy per-stage bindings.
#if defined(EMBER_CLIP_VULKAN)
#define EMBER_RENDER_SET set = 0,
#define EMBER_BIND_SPRITE 3
#define EMBER_BIND_DEPTH 4
#define EMBER_BIND_COLOR 5
#define EMBER_BIND_FRAG_PARAMS 7
#else
#define EMBER_RENDER_SET
#define EMBER_BIND_SPRITE 0
#define EMBER_BIND_DEPTH 1
#define EMBER_BIND_COLOR 2
#define EMBER_BIND_FRAG_PARAMS 18
#endif
layout(EMBER_RENDER_SET binding = EMBER_BIND_SPRITE) uniform sampler2D uSprite;
layout(EMBER_RENDER_SET binding = EMBER_BIND_DEPTH) uniform sampler2D uSceneDepth;
layout(EMBER_RENDER_SET binding = EMBER_BIND_COLOR) uniform sampler2D uSceneColor;
// Scalar state in one std140 block (CPU mirror: FragParams in
// src/backends/opengl/params.hpp).
layout(EMBER_RENDER_SET binding = EMBER_BIND_FRAG_PARAMS, std140) uniform FragParams {
    mat4 uInvProj; mat4 uFragProj; // renamed: uProj belongs to DrawParams (VS)
    vec2 uViewportSize; float uSoftRadius; int uUseSprite; // viewport px / soft fade / texture path
    int uUseSoft;      int uSheetCols;   int uSheetRows;   int uFragRefraction;
    int uRefrMode;     int uRefrShape;   float uRefrDome;  float uRefrStrength;
    float uRefrIor;    float uRefrAbsorption; float uRefrFresnel; float uRefrChroma;
    vec3 uRefrTint;    float uRefrSpecular;
    vec3 uRefrLightDir; float uRefrPad;   // glint light direction (view space)
};

float refrHash(vec2 p) {
    p = fract(p * 0.1031);
    return fract((p.x + p.y) * (p.x + p.y + 33.33) + p.y * p.x);
}
float refrNoise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(refrHash(i), refrHash(i + vec2(1, 0)), f.x),
               mix(refrHash(i + vec2(0, 1)), refrHash(i + vec2(1, 1)), f.x), f.y);
}

// Irregular polygon shard silhouette: 3-5 sides from the particle seed, with
// per-edge radial jitter (fracture nicks at the vertices). Returns the
// boundary radius in quad-local units (circumradius 1) at angle theta.
float shardRadius(float theta, uint seed) {
    const float PI = 3.14159265359;
    const float TAU = 6.28318530718;
    const float sides = 3.0 + float(seed % 3u);          // triangle / quad / pentagon
    const float sector = TAU / sides;
    const float a = mod(theta, TAU);
    const float k = floor(a / sector);
    const float local = mod(a, sector) - sector * 0.5;   // [-sector/2, sector/2]
    const float edge = cos(PI / sides) / cos(local);     // regular N-gon boundary
    const float j = 0.62 + 0.38 * refrHash(vec2(float(seed % 8191u) * 0.013,
                                                k * 3.71 + float((seed >> 13) % 8191u) * 0.007));
    return edge * j;
}

void main() {
    vec2 uv = vUV;
    vec4 texel = vec4(1.0);
    const vec2 dc = vUV * 2.0 - 1.0;   // quad-local coords [-1,1]
    const float rq = length(dc);
    float shardR = 1.0;                // silhouette radius at this angle (polygon shards)
    if (uFragRefraction != 0 && uRefrShape == 1) {
        // Polygon shard silhouette replaces the sprite mask entirely.
        shardR = shardRadius(atan(dc.y, dc.x), vShardSeed);
        if (rq > shardR) discard;
    } else if (uUseSprite != 0) {
        if (uSheetCols > 1 || uSheetRows > 1) {
            // age-based frame animation (vFade = 1 - age/life)
            float t = 1.0 - vFade;
            int total = uSheetCols * uSheetRows;
            int frame = int(clamp(t, 0.0, 0.999) * float(total));
            const vec2 grid = vec2(uSheetCols, uSheetRows);
            const vec2 cell = vec2(float(frame % uSheetCols), float(frame / uSheetCols)) / grid;
            // Linear filtering must stay inside this cell, rather than blend
            // in neighboring animation frames along the billboard edge.
            const vec2 inset = min(0.5 / vec2(textureSize(uSprite, 0)), 0.5 / grid);
            uv = clamp(cell + vUV / grid, cell + inset, cell + 1.0 / grid - inset);
        }
        texel = texture(uSprite, uv);
        if (texel.a < 0.02) discard;
    } else {
        // procedural soft disc + halo
        if (rq > 1.0) discard;
        texel.a = (1.0 - smoothstep(0.0, 1.0, rq)) + (1.0 - smoothstep(0.0, 1.0, rq * rq)) * 0.5;
    }

    // ---- refractive particles (glass shards) --------------------------------
    // Sample the host's scene color texture with a per-particle facet offset,
    // composite in-shader and replace the pixel (blending is disabled by the
    // host for this pass). Shape comes from the sprite mask above.
    if (uFragRefraction != 0) {
        const float coverage = clamp(vFade * vColor.a * texel.a, 0.0, 1.0);
        if (coverage <= 0.0) discard;
        const vec2 screenUV = gl_FragCoord.xy / uViewportSize;
        const vec3 n = normalize(vFacetNormal);
        // Dome-perturbed normal (per-PIXEL): the shard is lit like a tiny curved
        // facet — fresnel becomes a silhouette rim and the glint shrinks to a
        // spot. domeR = 1 exactly at the silhouette (shape-aware for polygons).
        vec3 nF = n;
        if (uRefrDome > 0.0) {
            const float domeR = clamp(rq / shardR, 0.0, 1.0);
            const vec2 dir = dc / max(rq, 1e-4);
            const float ca = cos(vAngle), sa = sin(vAngle);
            const vec2 dirV = vec2(dir.x * ca - dir.y * sa, dir.x * sa + dir.y * ca);
            nF = normalize(vec3(n.xy + dirV * (domeR * uRefrDome), n.z));
        }
        vec2 off;
        if (uRefrMode == 2) {
            // heat shimmer: noise-driven offset animated over the particle age
            const vec2 t = vec2(vFade * 6.2831, vFade * 3.7);
            off = vec2(refrNoise(screenUV * 8.0 + t) - 0.5) * (2.0 * uRefrStrength);
        } else if (uRefrMode == 1) {
            // Bend the view ray by the supplied refractive index, intersect
            // the scene's depth plane, then project the displaced sample.
            // IOR=1 is the identity. Missing depth is handled by the host's
            // simple-mode fallback; a cleared depth texel uses a unit plane.
            const float sd = texture(uSceneDepth, screenUV).r;
            const vec4 clip = vec4(screenUV * 2.0 - 1.0, EMBER_DEPTH_TO_NDC(sd), 1.0);
            const vec4 sv = uInvProj * clip;
            const float sceneZ = sd < 1.0 && abs(sv.w) > 1e-6 ? sv.z / sv.w : vViewZ - 1.0;
            const vec4 pv = uInvProj * vec4(screenUV * 2.0 - 1.0, EMBER_DEPTH_TO_NDC(gl_FragCoord.z), 1.0);
            const vec3 origin = pv.xyz / pv.w;
            const vec3 incident = uFragProj[3][3] == 0.0 ? normalize(origin) : vec3(0, 0, -1);
            const vec3 ray = refract(incident, n, 1.0 / uRefrIor);
            off = vec2(0.0);
            if (ray.z < -1e-6 && sceneZ < vViewZ && uRefrIor != 1.0) {
                const vec3 hit = origin + ray * ((sceneZ - vViewZ) / ray.z);
                const vec4 projected = uFragProj * vec4(hit, 1.0);
                off = (projected.xy / projected.w * 0.5 + 0.5 - screenUV) * uRefrStrength;
            }
        } else {
            off = n.xy * uRefrStrength;
        }
        vec3 col;
        if (uRefrChroma > 0.0) {
            col.r = texture(uSceneColor, screenUV + off * (1.0 + uRefrChroma)).r;
            col.g = texture(uSceneColor, screenUV + off).g;
            col.b = texture(uSceneColor, screenUV + off * (1.0 - uRefrChroma)).b;
        } else {
            col = texture(uSceneColor, screenUV + off).rgb;
        }
        vec3 refr = col * uRefrTint;
        const vec3 original = texture(uSceneColor, screenUV).rgb;
        vec3 outc = mix(original, refr, uRefrAbsorption);
        // Surface reflections (fresnel rim + glint spot) are ADDED by the glass,
        // not transmitted — they must not be attenuated by uRefrAbsorption.
        if (uRefrFresnel > 0.0)
            outc += pow(1.0 - abs(dot(nF, vec3(0.0, 0.0, 1.0))), uRefrFresnel) * vec3(1.0);
        if (uRefrSpecular > 0.0) {
            const vec3 rv = reflect(vec3(0.0, 0.0, -1.0), nF);
            outc += pow(max(dot(rv, uRefrLightDir), 0.0), 24.0) * uRefrSpecular;
        }
        // Keep pixel replacement, but fade the distortion/reflections into
        // the original scene using lifetime, sprite coverage and emitter alpha.
        frag = vec4(mix(original, outc, coverage), 1.0);
        return;
    }

    // ---- normal (glow) path --------------------------------------------------
    // fade toward vFadeRGB as the particle dies
    vec3 rgb = mix(vFadeRGB, vColor.rgb, vFade);
    float a = vFade * texel.a * vColor.a;

    // Soft particles: fade out when the particle is close in front of a scene
    // surface. Sample the scene depth at the FRAGMENT's screen position — vUV
    // is the quad-local UV, which only matches screen space for tiny centered
    // quads and breaks for off-center / large / streaked particles.
    if (uUseSoft != 0) {
        const vec2 screenUV = gl_FragCoord.xy / uViewportSize;
        const float sd = texture(uSceneDepth, screenUV).r;
        if (sd < 1.0) {
            const vec4 clip = vec4(screenUV * 2.0 - 1.0, EMBER_DEPTH_TO_NDC(sd), 1.0);
            const vec4 sv = uInvProj * clip;
            const float sceneZ = sv.z / sv.w;      // view-space z of the surface
            a *= smoothstep(0.0, uSoftRadius, vViewZ - sceneZ);
        }
    }
    if (a <= 0.0) discard; // invisible fragments must not occlude later draws
    frag = vec4(rgb * texel.rgb, a);
}
