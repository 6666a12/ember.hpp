#version 430 core
// gl_VertexID/gl_InstanceID are gl_VertexIndex/gl_InstanceIndex in Vulkan GLSL
// (same base-offset semantics here: firstVertex/baseInstance are always 0).
#if defined(VULKAN)
#define EMBER_VERTEX_INDEX gl_VertexIndex
#define EMBER_INSTANCE_INDEX gl_InstanceIndex
#else
#define EMBER_VERTEX_INDEX gl_VertexID
#define EMBER_INSTANCE_INDEX gl_InstanceID
#endif

// Instanced billboard quad: 4 vertices per particle (GL_TRIANGLE_STRIP),
// the quad is expanded in view space from gl_VertexID — no vertex attributes.

// Vulkan descriptor sets cannot alias one binding to different descriptor
// types per stage (GL's per-stage namespaces allow that), so the render
// resources move to a dedicated set with non-colliding binding numbers.
#if defined(EMBER_CLIP_VULKAN)
#define EMBER_RENDER_SET set = 0,
#define EMBER_BIND_CUR 0
#define EMBER_BIND_SORTED 1
#define EMBER_BIND_LIVE 2
#define EMBER_BIND_DRAW_PARAMS 6
#define EMBER_BIND_CURVES 8
#else
#define EMBER_RENDER_SET
#define EMBER_BIND_CUR 0
#define EMBER_BIND_SORTED 8
#define EMBER_BIND_LIVE 11
#define EMBER_BIND_DRAW_PARAMS 17
#define EMBER_BIND_CURVES 23
#endif

struct Particle {
    vec4 pos;
    vec4 vel;
    vec4 life;
    vec4 color;
};
layout(EMBER_RENDER_SET binding = EMBER_BIND_CUR, std430) readonly buffer BufCur    { Particle particles[]; };
layout(EMBER_RENDER_SET binding = EMBER_BIND_SORTED, std430) readonly buffer BufSorted { uint sorted[]; };

layout(EMBER_RENDER_SET binding = EMBER_BIND_LIVE, std430) readonly buffer BufLive { uint liveIndices[]; };

// Baked lifecycle curves (WO-09): color-over-life (rgba) and size-over-life.
// CPU mirror: CurvesParams in src/backends/opengl/params.hpp.
layout(EMBER_RENDER_SET binding = EMBER_BIND_CURVES, std430) readonly buffer BufCurves {
    vec4  colorLut[64];
    float sizeLut[64];
};

// Scalar state in one std140 block (Vulkan-compilable; CPU mirror: DrawParams
// in src/backends/opengl/params.hpp).
layout(EMBER_RENDER_SET binding = EMBER_BIND_DRAW_PARAMS, std140) uniform DrawParams {
    mat4 uView; mat4 uProj;
    float uSizeScale;
    float uStreak;    // >0: centered stretch along projected particle motion
    float uSpinSpeed; // signed angular velocity; zero disables normal-particle spin
    int   uUseSorted; // 1 = fetch particle via sorted[gl_InstanceID] (GPU sort)
    int   uRefraction;// 0 = normal pass, 1 = refraction pass
};

layout(location = 0) out vec2 vUV;
layout(location = 1) out vec4 vColor;
layout(location = 2) out float vFade;
layout(location = 3) out vec3 vFadeRGB;
layout(location = 4) out float vViewZ;         // view-space z, for soft particles in the FS
layout(location = 5) out vec3 vFacetNormal;    // view-space pseudo-normal (refraction offset direction)
layout(location = 6) out float vAngle;         // billboard rotation (rad): dome normals rotate with the shard
layout(location = 7) flat out uint vShardSeed; // per-particle hash: polygon shard silhouette in the FS

uint hashUint(uint x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}
float hashAngle(uint id) { return float(hashUint(id)) / 4294967296.0 * 6.28318530718; }
vec2 hashTilt(uint id) {
    uint a = hashUint(id);
    uint b = hashUint(id ^ 0x9E3779B9u);
    return vec2(float(a) / 4294967296.0 - 0.5, float(b) / 4294967296.0 - 0.5);
}

void main() {
#if defined(EMBER_DEBUG_FULLSCREEN)
    const vec2 dc = vec2(float(EMBER_VERTEX_INDEX & 1) * 2.0 - 1.0,
                         float((EMBER_VERTEX_INDEX >> 1) & 1) * 2.0 - 1.0);
    gl_Position = vec4(dc, 0.0, 1.0);
    vUV = dc * 0.5 + 0.5;
    vColor = vec4(1.0); vFade = 1.0; vFadeRGB = vec3(1.0);
    vViewZ = 0.0; vFacetNormal = vec3(0.0, 0.0, 1.0); vAngle = 0.0; vShardSeed = 0u;
    return;
#endif
    const uint idx = (uUseSorted != 0) ? sorted[EMBER_INSTANCE_INDEX] : liveIndices[EMBER_INSTANCE_INDEX];
    const Particle p = particles[idx];

    // Assign outputs unconditionally (strict drivers reject varyings that are
    // only written on some paths).
    // Lifecycle curves: multiply color and size by the baked LUT. A disabled
    // curve is all-ones, so x * 1.0 is bit-identical to the plain path.
    const float ageFrac = clamp(p.vel.w / max(p.life.x, 1e-4), 0.0, 1.0);
    const uint  lutIdx = min(uint(ageFrac * 63.0 + 0.5), 63u);
    const vec4  colorMul = colorLut[lutIdx];
    const float sizeMul = sizeLut[lutIdx];
    vColor = p.color * colorMul;
    vFade = 1.0 - ageFrac;
    vFadeRGB = p.life.yzw * colorMul.rgb;
    vShardSeed = hashUint(idx);
    vAngle = 0.0;

    // Refractive particles are sign-encoded in pos.w (size < 0). Cull instances
    // that do not belong to the current pass (uRefraction): a degenerate quad.
    const bool isRefractive = p.pos.w < 0.0;
    if (isRefractive != (uRefraction != 0)) {
        gl_Position = vec4(0.0, 0.0, 3.0, 1.0);
        vUV = vec2(0.0);
        vViewZ = 0.0;
        vFacetNormal = vec3(0.0, 0.0, 1.0);
        return;
    }
    const float size = abs(p.pos.w) * sizeMul;

    // Default orientation. A visible streak overrides it below; a vanishing
    // streak returns continuously to this angle instead of snapping at rest.
    float ang = 0.0;
    if (uSpinSpeed != 0.0 || uRefraction != 0)
        ang = hashAngle(idx) + p.vel.w * uSpinSpeed;

    const vec3 viewPos = (uView * vec4(p.pos.xyz, 1.0)).xyz;
    // Clipping belongs to uProj; a fixed view-Z cutoff breaks close near
    // planes and valid orthographic ranges that straddle the view origin.
    vViewZ = viewPos.z;

    const float hs = 0.5 * size * uSizeScale;
    // Quad corner in [-1,1]^2, triangle-strip order.
    const vec2 c = vec2(float(EMBER_VERTEX_INDEX & 1) * 2.0 - 1.0,
                        float((EMBER_VERTEX_INDEX >> 1) & 1) * 2.0 - 1.0);
    vUV = c * 0.5 + 0.5;

    float elong = 1.0;
    if (uStreak > 0.0) {
        // Differentiate the perspective projection at the particle center,
        // then express that motion in view-plane units at its current depth.
        // Dropping velocity.z is only correct for orthographic projection:
        // off-axis depth motion moves on screen; camera-ray motion does not.
        const vec3 vv = (uView * vec4(p.vel.xyz, 0.0)).xyz;
        vec2 motion = vv.xy;
        if (uProj[3][3] == 0.0 && abs(viewPos.z) > 1e-6)
            motion -= viewPos.xy * (vv.z / viewPos.z);
        const float speed = length(motion);
        const float extra = uStreak * speed;
        elong += extra;
        if (speed > 1e-6) {
            const float target = atan(motion.y, motion.x);
            // Up to 10% elongation, blend the shortest angular path from the
            // ordinary billboard orientation. Above it, velocity fully wins.
            const float weight = smoothstep(0.0, 0.1, extra);
            if (weight >= 1.0) ang = target;
            else ang += atan(sin(target - ang), cos(target - ang)) * weight;
        }
    }
    vAngle = ang;
    const float ca = cos(ang), sa = sin(ang);
    const vec2 offset = vec2(c.x * elong * ca - c.y * sa,
                             c.x * elong * sa + c.y * ca) * hs;

    // Geometry, dome coordinates and facet normal share one final orientation.
    if (uRefraction != 0) {
        const vec2 tilt = hashTilt(idx) * 1.2;
        const vec3 n = normalize(vec3(tilt, 1.0));
        vFacetNormal = vec3(n.x * ca - n.y * sa, n.x * sa + n.y * ca, n.z);
    } else {
        vFacetNormal = vec3(0.0, 0.0, 1.0);
    }
    gl_Position = uProj * vec4(viewPos + vec3(offset, 0.0), 1.0);
#if defined(EMBER_CLIP_VULKAN)
    // The host keeps a GL-style projection (NDC z in [-1,1]); Vulkan's clip
    // volume is z in [0,w]. Remap here and read depth back accordingly in the
    // fragment shader (see EMBER_DEPTH_TO_NDC).
    gl_Position.z = (gl_Position.z + gl_Position.w) * 0.5;
#endif
}
