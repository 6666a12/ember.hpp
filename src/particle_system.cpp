// ember — GPU particle system.
//
// Frame pipeline (all GPU work, one 16-byte CPU readback per frame):
//   1. CPU encodes spawn requests (a few compact structs: emitters + bursts);
//      the GPU samples the actual particles (shape/cone/palette/fade/links).
//   2. Compute dispatch A (phase 0): integrate alive particles, retire the dead
//      into a free stack (atomic LIFO), shrink the alive counter.
//   3. Compute dispatch B (phase 1): sample requested particles into a dead slot
//      (CAS pop) or appended at the end (with capacity guard).
//   4. Read back the alive counter, swap cur/next, render instanced quads.

#include "ember/particle_system.hpp"

#include "ember/config.hpp"

#include "stb_image.h"

#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace ember {

namespace {

constexpr std::uint32_t kSimGroupSize = 64;
constexpr std::uint32_t kMaxSpawnRequests = 4096; // cap on requests per frame (emitters + bursts)
constexpr GLuint kBindingCur = 0;         // particle SSBO (read)
constexpr GLuint kBindingNext = 1;        // particle SSBO (write)
constexpr GLuint kBindingSpawn = 2;       // spawn request SSBO (read)
constexpr GLuint kBindingDead = 3;        // free-slot stack
constexpr GLuint kBindingCounters = 4;    // uAlive/uDeadHead/uSpawnRequestCount/uCapacity
constexpr GLuint kBindingAttractors = 5;  // vec4 attractors
constexpr GLuint kBindingVortexes = 6;    // Vortex array
constexpr GLuint kBindingSprings = 7;     // Spring array
constexpr GLuint kBindingSorted = 8;      // sorted particle indices (render only)
constexpr GLuint kBindingPalette = 9;     // palette colors (GPU spawn)
constexpr GLuint kBindingIndirect = 10;   // draw args for glDrawArraysIndirect

const char* const kSimulateComp = R"GLSL(#version 430 core
// ember simulation — dispatch with uPhase=0 (simulate), then uPhase=1 (spawn).
layout(local_size_x = 64) in;

struct Particle {
    vec4 pos;   // xyz position, w = size
    vec4 vel;   // xyz velocity, w = age
    vec4 life;  // x = lifetime (negative => corpse slot owned by the free stack)
    vec4 color; // rgba
};

layout(binding = 0, std430) readonly  buffer BufCur   { Particle cur[]; };
layout(binding = 1, std430) writeonly buffer BufNext  { Particle nxt[]; };
// Spawn requests: the CPU encodes emitters/bursts as compact requests; this
// shader samples the actual particles (shape, cone, palette, fade, size<->speed
// link) with its own hash RNG. Layout matches ember::SpawnRequest (std430).
struct SpawnRequest {
    vec4  colorMin;
    vec4  colorMax;
    vec3  position;
    float radius;
    vec3  axis;          // cone axis (normalized; zero = no cone)
    float coneAngle;     // degrees; >0 => cone emission
    vec3  dir;           // normalized base velocity (zero = uniform sphere)
    float spread;        // 0..1 mix toward a random sphere direction
    vec3  extents;       // box half-extents
    float speedMin;      // already includes speedScale
    float speedMax;
    float speedSizeLink; // per-particle: speed *= 1 + link*(size/sizeMid - 1)
    float lifeMin;
    float lifeMax;
    float sizeMin;
    float sizeMax;
    int   shape;         // 0 point, 1 box, 2 sphere, 3 cone
    int   paletteIdx;    // base index into the palette buffer (-1 = colorMin/Max)
    int   hasFade;       // write life.yzw = fade target
    uint  base;          // prefix offset: spawns work ids [base, base+count)
    uint  count;
    int   paletteCount;  // colors in this palette range
    vec3  fadeMin;       // fade target range (RGB)
    uint  pad2;
    vec3  fadeMax;
    uint  pad3;
};
layout(binding = 2, std430) readonly  buffer BufSpawnReq { SpawnRequest req[]; };
layout(binding = 9, std430) readonly  buffer BufPalette { vec4 palette[]; };
layout(binding = 10, std430) writeonly buffer BufIndirectArgs { // uPhase=2 write (draw count)
    uint vertexCount;    // always 4 (quad)
    uint instanceCount;  // alive
    uint firstVertex;    // 0
    uint baseInstance;   // 0
} indirectArgs;
layout(binding = 3, std430) coherent  buffer BufDead  { uint dead[]; };
layout(binding = 4, std430) coherent  buffer BufCtr   {
    uint uAlive;
    uint uDeadHead;
    uint uSpawnRequestCount;
    uint uCapacity;
};
layout(binding = 5, std430) readonly buffer BufAttr { vec4 attractors[]; };

struct Vortex {
    vec4 center;        // xyz position, w = radius (falloff scale)
    vec4 axisStrength;  // xyz axis, w = strength
};
layout(binding = 6, std430) readonly buffer BufVortex { Vortex vortexes[]; };

struct Spring {
    vec3  anchor;
    float stiffness;
    float damping;
};
layout(binding = 7, std430) readonly buffer BufSpring { Spring springs[]; };

// ---------------------------------------------------------------------------
// Force switches. bit i corresponds to ember::Force enum value i:
//   0 gravity   1 drag   2 wind   3 turbulence   4 attractors
//   5 vortex    6 spring 7 noise_wind          8 wave    9 boundary
// ---------------------------------------------------------------------------
uniform uint uForceMask;

uniform vec3  uGravity;
uniform float uDrag;
uniform int   uDragMode;      // 0 = linear, 1 = quadratic
uniform vec3  uWind;
uniform float uTurbulence;
uniform int   uAttractorCount;
uniform int   uVortexCount;
uniform int   uSpringCount;
uniform vec3  uNoiseWindDir;
uniform float uNoiseWindAmp;
uniform float uNoiseWindScale;
uniform float uNoiseWindSpeed;
uniform vec3  uWaveDir;
uniform vec3  uWaveK;
uniform float uWaveAmp;
uniform float uWaveOmega;
uniform int   uBoundaryMode;  // 1 = kill, 2 = bounce (mask bit 9 must be set)
uniform float uBoundaryY;
uniform float uRestitution;
uniform float uDt;
uniform float uTime;
uniform int   uPhase;
uniform uint  uSpawnTotal;     // total particles requested this frame (phase 1)
uniform uint  uFrameSeed;      // per-frame RNG seed (phase 1)

// Deterministic hash RNG (splitmix-ish) — per-particle state derived from the
// work id + frame seed, so concurrent spawn threads never share mutable state.
uint hashUint(uint x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}
float rngUnit(inout uint s) { s = hashUint(s + 0x9E3779B9u); return float(s) / 4294967296.0; }
float rngRange(inout uint s, float a, float b) { return a + (b - a) * rngUnit(s); }
vec3 rngSphere(inout uint s) {
    float z = rngRange(s, -1.0, 1.0);
    float a = rngRange(s, 0.0, 6.283185307179586);
    float r = sqrt(max(0.0, 1.0 - z * z));
    return vec3(r * cos(a), r * sin(a), z);
}
vec3 rngVec3(inout uint s, vec3 a, vec3 b) {
    return vec3(rngRange(s, a.x, b.x), rngRange(s, a.y, b.y), rngRange(s, a.z, b.z));
}
void basis(vec3 ax, out vec3 u, out vec3 v) {
    u = normalize(cross(ax, abs(ax.y) > 0.9 ? vec3(1, 0, 0) : vec3(0, 1, 0)));
    v = cross(ax, u);
}

vec3 samplePosition(inout uint s, SpawnRequest r) {
    if (r.shape == 1) { // box
        return r.position + rngVec3(s, -r.extents, r.extents);
    }
    if (r.shape == 2) { // sphere (uniform radius, matching the CPU sampler)
        return r.position + rngSphere(s) * rngRange(s, 0.0, r.radius);
    }
    if (r.shape == 3) { // cone base disc
        vec3 ax = length(r.axis) > 1e-4 ? normalize(r.axis) : vec3(0, 1, 0);
        vec3 u, v; basis(ax, u, v);
        float rad = r.radius * sqrt(rngUnit(s));
        float a = rngRange(s, 0.0, 6.283185307179586);
        return r.position + (u * cos(a) + v * sin(a)) * rad;
    }
    return r.position; // point
}

vec3 sampleDirection(inout uint s, SpawnRequest r) {
    if (r.coneAngle > 0.0 && length(r.axis) > 1e-4) {
        vec3 ax = normalize(r.axis);
        vec3 u, v; basis(ax, u, v);
        float cosA = cos(radians(r.coneAngle));
        float cosT = mix(cosA, 1.0, rngUnit(s)); // cos-weighted on the cap
        float sinT = sqrt(max(0.0, 1.0 - cosT * cosT));
        float phi = rngRange(s, 0.0, 6.283185307179586);
        return normalize(ax * cosT + (u * cos(phi) + v * sin(phi)) * sinT);
    }
    float bl = length(r.dir);
    if (bl < 1e-4) return rngSphere(s);
    if (r.spread <= 0.01) return normalize(r.dir);
    return normalize(mix(normalize(r.dir), rngSphere(s), r.spread));
}

void spawnFromRequest(uint j) {
    if (j >= uSpawnTotal) return;
    // Requests are sorted by ascending base — binary search the owner.
    int lo = 0, hi = int(uSpawnRequestCount) - 1;
    while (lo < hi) {
        int mid = (lo + hi + 1) >> 1;
        if (req[mid].base <= j) lo = mid;
        else hi = mid - 1;
    }
    SpawnRequest r = req[lo];
    uint s = uFrameSeed ^ (j * 0x9E3779B9u);

    // ---- sample the particle (mirrors the CPU Emitter::spawn formulas) ----
    Particle p;
    float size = rngRange(s, r.sizeMin, r.sizeMax);
    p.pos = vec4(samplePosition(s, r), size);
    vec3 dirv = sampleDirection(s, r);
    float speed = rngRange(s, r.speedMin, r.speedMax);
    if (r.speedSizeLink > 0.0) {
        float sizeMid = 0.5 * (r.sizeMin + r.sizeMax);
        if (sizeMid > 0.0) speed *= 1.0 + r.speedSizeLink * (size / sizeMid - 1.0);
    }
    p.vel = vec4(dirv * speed, 0.0); // age starts at 0
    p.life.x = rngRange(s, r.lifeMin, r.lifeMax);
    if (r.paletteIdx >= 0 && r.paletteCount > 0) {
        int ci = r.paletteIdx + int(rngUnit(s) * float(r.paletteCount)) % r.paletteCount;
        p.color = palette[ci];
    } else {
        p.color = vec4(rngRange(s, r.colorMin.r, r.colorMax.r),
                       rngRange(s, r.colorMin.g, r.colorMax.g),
                       rngRange(s, r.colorMin.b, r.colorMax.b),
                       rngRange(s, r.colorMin.a, r.colorMax.a));
    }
    // Fade target color (RGB) goes into life.yzw; unset => identity (no shift).
    if (r.hasFade != 0) p.life.yzw = rngVec3(s, r.fadeMin, r.fadeMax);
    else p.life.yzw = p.color.rgb;

    // ---- slot recycling / append (unchanged from the pre-request path) ----
    uint old = uDeadHead;
    while (old > 0u) {
        if (atomicCompSwap(uDeadHead, old, old - 1u) == old) {
            uint slot = dead[(old - 1u) % uCapacity];
            atomicAdd(uAlive, 1u); // the recycled particle is alive (its death already decremented)
            nxt[slot] = p;
            return;
        }
        old = uDeadHead;
    }
    // Stack empty — append instead (capacity guarded).
    uint slot = atomicAdd(uAlive, 1u);
    if (slot >= uCapacity) {
        atomicAdd(uAlive, -1u); // capacity reached: drop this particle
        return;
    }
    nxt[slot] = p;
}

void main() {
    if (uPhase == 0) {
        uint i = gl_GlobalInvocationID.x;
        if (i >= uAlive) return;
        simulate(i);
    } else if (uPhase == 1) {
        spawnFromRequest(gl_GlobalInvocationID.x);
    } else if (uPhase == 2) {
        // Publish the draw count for glDrawArraysIndirect (host no longer needs
        // the per-frame alive readback to issue the draw).
        if (gl_GlobalInvocationID.x == 0u) {
            indirectArgs.vertexCount = 4u;
            indirectArgs.instanceCount = uAlive;
            indirectArgs.firstVertex = 0u;
            indirectArgs.baseInstance = 0u;
        }
    }
}

float hash11(vec3 p) {
    p = fract(p * 0.1031);
    p += dot(p, p.yzx + 33.33);
    return fract((p.x + p.y) * p.z);
}

// Smooth 3D value noise (lattice interpolation) — drifting gust-like turbulence.
// (name avoids the built-in noise3() family from GLSL)
float snoise3(vec3 x) {
    vec3 i = floor(x);
    vec3 f = fract(x);
    f = f * f * (3.0 - 2.0 * f);
    float n000 = hash11(i);
    float n100 = hash11(i + vec3(1, 0, 0));
    float n010 = hash11(i + vec3(0, 1, 0));
    float n110 = hash11(i + vec3(1, 1, 0));
    float n001 = hash11(i + vec3(0, 0, 1));
    float n101 = hash11(i + vec3(1, 0, 1));
    float n011 = hash11(i + vec3(0, 1, 1));
    float n111 = hash11(i + vec3(1, 1, 1));
    return mix(
        mix(mix(n000, n100, f.x), mix(n010, n110, f.x), f.y),
        mix(mix(n001, n101, f.x), mix(n011, n111, f.x), f.y),
        f.z);
}

// Kill a particle: recycle its slot, shrink the alive count, leave a corpse marker.
void retire(uint i) {
    uint h = atomicAdd(uDeadHead, 1u);
    dead[h % uCapacity] = i;
    atomicAdd(uAlive, -1u);
    nxt[i] = Particle(vec4(0.0), vec4(0.0), vec4(-1.0), vec4(0.0));
}

void simulate(uint i) {
    Particle p = cur[i];
    if (p.life.x < 0.0) return;          // corpse: slot belongs to the free stack
    // NaN guard: a NaN particle is retired instead of poisoning the buffer
    // (NaN positions can also stall some drivers during rasterization).
    if (isnan(p.pos.x) || isnan(p.pos.y) || isnan(p.pos.z) ||
        isnan(p.vel.x) || isnan(p.vel.y) || isnan(p.vel.z)) {
        retire(i);
        return;
    }
    p.vel.w += uDt;                      // age
    if (p.vel.w >= p.life.x) {
        retire(i);
        return;
    }

    // ---- force accumulation (each term guarded by uForceMask) -------------
    vec3 acc = vec3(0.0);
    if ((uForceMask & (1u << 0u)) != 0u) acc += uGravity;                 // gravity
    if ((uForceMask & (1u << 1u)) != 0u) {                                // drag
        if (uDragMode == 1) acc -= p.vel.xyz * length(p.vel.xyz) * uDrag; // quadratic
        else acc -= p.vel.xyz * uDrag;                                    // linear
    }
    if ((uForceMask & (1u << 2u)) != 0u) acc += uWind;                    // uniform wind
    if ((uForceMask & (1u << 3u)) != 0u && uTurbulence > 0.0) {           // turbulence
        vec3 n = vec3(snoise3(p.pos.xyz * 0.3 + uTime),
                      snoise3(p.pos.xyz * 0.3 + uTime * 1.31 + 7.7),
                      snoise3(p.pos.xyz * 0.3 + uTime * 0.67 + 3.3));
        acc += (n * 2.0 - 1.0) * uTurbulence;
    }
    if ((uForceMask & (1u << 4u)) != 0u) {                                // point attractors
        for (int k = 0; k < uAttractorCount; ++k) {
            vec3 d = attractors[k].xyz - p.pos.xyz;
            float r2 = dot(d, d) + 1e-4;     // softening
            float inv = inversesqrt(r2);
            acc += d * (attractors[k].w * inv * inv * inv); // inverse-square, sign = attract/repel
        }
    }
    if ((uForceMask & (1u << 5u)) != 0u && uVortexCount > 0) {            // vortex swirls
        for (int k = 0; k < uVortexCount; ++k) {
            Vortex v = vortexes[k];
            vec3 d = v.center.xyz - p.pos.xyz;
            float r2 = dot(d, d) + 1e-4;
            float fall = v.axisStrength.w / (r2 + v.center.w * v.center.w); // smooth core, ~1/r² outside
            acc += cross(v.axisStrength.xyz, d) * fall;                     // tangential swirl
        }
    }
    if ((uForceMask & (1u << 6u)) != 0u && uSpringCount > 0) {            // spring-to-anchor
        for (int k = 0; k < uSpringCount; ++k) {
            Spring s = springs[k];
            vec3 d = s.anchor - p.pos.xyz;
            acc += d * s.stiffness - p.vel.xyz * s.damping;
        }
    }
    if ((uForceMask & (1u << 7u)) != 0u && uNoiseWindAmp > 0.0) {         // spatial noise wind
        float n = snoise3(p.pos.xyz * uNoiseWindScale + uTime * uNoiseWindSpeed);
        acc += uNoiseWindDir * (n * 2.0 - 1.0) * uNoiseWindAmp;
    }
    if ((uForceMask & (1u << 8u)) != 0u && uWaveAmp > 0.0) {              // traveling wave
        float phase = dot(p.pos.xyz, uWaveK) + uTime * uWaveOmega;
        acc += uWaveDir * (sin(phase) * uWaveAmp);
    }

    // Semi-implicit Euler integration.
    p.vel.xyz += acc * uDt;
    p.pos.xyz += p.vel.xyz * uDt;
    p.vel.xyz = clamp(p.vel.xyz, vec3(-500.0), vec3(500.0)); // explode guard

    // ---- boundary plane (y < uBoundaryY) -----------------------------------
    if ((uForceMask & (1u << 9u)) != 0u && uBoundaryMode != 0 && p.pos.y < uBoundaryY) {
        if (uBoundaryMode == 1) {          // kill
            retire(i);
            return;
        }
        p.pos.y = uBoundaryY;              // bounce
        p.vel.y = -p.vel.y * uRestitution;
        if (abs(p.vel.y) < 0.1) p.vel.y = 0.0; // settle on the plane
    }
    nxt[i] = p;
}
)GLSL";

const char* const kParticleVert = R"GLSL(#version 430 core
// Instanced billboard quad: 4 vertices per particle (GL_TRIANGLE_STRIP),
// the quad is expanded in view space from gl_VertexID — no vertex attributes.

struct Particle {
    vec4 pos;
    vec4 vel;
    vec4 life;
    vec4 color;
};
layout(binding = 0, std430) readonly buffer BufCur    { Particle particles[]; };
layout(binding = 8, std430) readonly buffer BufSorted { uint sorted[]; };

uniform mat4  uView;
uniform mat4  uProj;
uniform float uSizeScale;
uniform float uStreak;    // >0: stretch quads along lateral velocity (spark trails)
uniform int   uUseSorted; // 1 = fetch particle via sorted[gl_InstanceID] (GPU sort)

out vec2 vUV;
out vec4 vColor;
out float vFade;
out vec3 vFadeRGB;
out float vViewZ;         // view-space z, for soft particles in the FS

void main() {
    const uint idx = (uUseSorted != 0) ? sorted[gl_InstanceID] : gl_InstanceID;
    const Particle p = particles[idx];

    // Assign outputs unconditionally (strict drivers reject varyings that are
    // only written on some paths).
    vColor = p.color;
    vFade = 1.0 - clamp(p.vel.w / max(p.life.x, 1e-4), 0.0, 1.0);
    vFadeRGB = p.life.yzw;

    const vec3 viewPos = (uView * vec4(p.pos.xyz, 1.0)).xyz;
    if (viewPos.z > -0.01) {                 // behind the near plane: clip out
        gl_Position = vec4(0.0, 0.0, 3.0, 1.0);
        vUV = vec2(0.0);
        vViewZ = 0.0;
        return;
    }
    vViewZ = viewPos.z;

    const float hs = 0.5 * p.pos.w * uSizeScale;
    // Quad corner in [-1,1]^2, triangle-strip order.
    const vec2 c = vec2(float(gl_VertexID & 1) * 2.0 - 1.0,
                        float((gl_VertexID >> 1) & 1) * 2.0 - 1.0);
    vUV = c * 0.5 + 0.5;

    vec2 offset;
    if (uStreak > 0.0) {
        // Stretch along the particle's velocity projected on the view plane.
        const vec3 vv = (uView * vec4(p.vel.xyz, 0.0)).xyz;
        const vec2 dir2 = normalize(vv.xy + vec2(1e-6));
        const vec2 perp = vec2(-dir2.y, dir2.x);
        const float elong = 1.0 + uStreak * length(vv.xy);
        offset = dir2 * (c.x * hs * elong) + perp * (c.y * hs);
    } else {
        offset = c * hs;
    }
    gl_Position = uProj * vec4(viewPos + vec3(offset, 0.0), 1.0);
}
)GLSL";

const char* const kParticleFrag = R"GLSL(#version 430 core
in vec2 vUV;
in vec4 vColor;
in float vFade;
in vec3 vFadeRGB;
in float vViewZ;
out vec4 frag;

uniform sampler2D uSprite;
uniform sampler2D uSceneDepth;
uniform mat4  uInvViewProj;
uniform float uSoftRadius;
uniform vec2  uViewportSize; // screen size in pixels (soft-particle depth lookup)
uniform int   uUseSprite;  // 1 = texture path, 0 = procedural glow
uniform int   uUseSoft;    // 1 = soft particles (fade near scene depth)
uniform int   uSheetCols;  // sprite-sheet grid (1x1 = single frame)
uniform int   uSheetRows;

void main() {
    vec2 uv = vUV;
    vec4 texel = vec4(1.0);
    if (uUseSprite != 0) {
        if (uSheetCols > 1 || uSheetRows > 1) {
            // age-based frame animation (vFade = 1 - age/life)
            float t = 1.0 - vFade;
            int total = uSheetCols * uSheetRows;
            int frame = int(clamp(t, 0.0, 0.999) * float(total));
            uv = (vUV + vec2(float(frame % uSheetCols), float(frame / uSheetCols)))
                 / vec2(uSheetCols, uSheetRows);
        }
        texel = texture(uSprite, uv);
        if (texel.a < 0.02) discard;
    } else {
        // procedural soft disc + halo
        vec2 c = vUV * 2.0 - 1.0;
        float r = length(c);
        if (r > 1.0) discard;
        texel.a = smoothstep(1.0, 0.0, r) + smoothstep(1.0, 0.0, r * r) * 0.5;
    }
    // fade toward vFadeRGB as the particle dies
    vec3 rgb = mix(vFadeRGB, vColor.rgb, vFade);
    float a = vFade * texel.a * vColor.a;

    // Soft particles: fade out when the particle is close behind a scene
    // surface. Sample the scene depth at the FRAGMENT's screen position — vUV
    // is the quad-local UV, which only matches screen space for tiny centered
    // quads and breaks for off-center / large / streaked particles.
    if (uUseSoft != 0) {
        const vec2 screenUV = gl_FragCoord.xy / uViewportSize;
        const float sd = texture(uSceneDepth, screenUV).r;
        const vec4 clip = vec4(screenUV * 2.0 - 1.0, sd * 2.0 - 1.0, 1.0);
        const vec4 sv = uInvViewProj * clip;
        const float sceneZ = sv.z / sv.w;          // view-space z of the surface
        a *= smoothstep(0.0, uSoftRadius, sceneZ - vViewZ);
    }
    frag = vec4(rgb * texel.rgb, a);
}
)GLSL";

// GPU bitonic sort of alive particle indices (see shaders/sort.comp).
const char* const kSortComp = R"GLSL(#version 430 core
// GPU bitonic sort of alive particle indices by view-space depth.
//
// Host protocol (one dispatch per step, see ParticleSystem::sortParticles):
//   uMode = 0   identity fill: sorted[i] = i for i < uAlive
//   uMode = 1   one compare-exchange step for bitonic stage uK / step uJ
//               (classic network: for k = 2; k <= N; k <<= 1
//                                 for j = k/2; j > 0; j >>= 1  dispatch)
//
// In-place on a single buffer: within one step every element is read/written
// by exactly one thread (the lower index of each pair owns the swap), so there
// is no cross-workgroup race; the host issues a glMemoryBarrier between steps.
// uPaddedN is the smallest power of two >= uAlive; threads past N must not
// touch the buffer (their pairs would collide with valid threads' pairs).
//
// Result: sorted[0..uAlive) = alive particle indices, far-to-near (ascending
// view-space z = descending depth), which yields correct alpha blending when
// quads are drawn in that order. Padding entries (idx >= uAlive) carry the
// key +1e30 and sink to the end of the ascending sequence — never drawn.

layout(local_size_x = 64) in;

struct Particle {
    vec4 pos;   // xyz position, w = size
    vec4 vel;   // xyz velocity, w = age
    vec4 life;  // x = lifetime (negative => corpse slot)
    vec4 color; // rgba
};
layout(binding = 0, std430) readonly buffer BufCur  { Particle cur[]; };
layout(binding = 8, std430) buffer       BufSorted { uint sorted[]; };

uniform mat4  uView;
uniform uint  uAlive;
uniform uint  uPaddedN;  // power of two >= uAlive (buffer holds at least this)
uniform uint  uK;        // bitonic stage mask (2, 4, 8, ...)
uniform uint  uJ;        // step within the stage (uK/2 ... 1)
uniform uint  uMode;     // 0 = identity fill, 1 = compare-exchange step

// View-space z as the sort key: negative in front, so ascending order puts
// the farthest particle first (back-to-front = correct alpha compositing).
// Padding entries get +1e30 (the largest key) so they sink to the END of the
// ascending sequence — they are never drawn.
float depthKey(uint idx) {
    if (idx >= uAlive) return 1e30;
    return (uView * vec4(cur[idx].pos.xyz, 1.0)).z;
}

void main() {
    const uint i = gl_GlobalInvocationID.x;
    if (i >= uPaddedN) return;         // over-spawned threads: hands off

    if (uMode == 0u) {                 // identity fill
        // Padding slots (i >= uAlive) get the sentinel uAlive — never a valid
        // index, so stale values from previous frames can't become duplicates.
        sorted[i] = (i < uAlive) ? i : uAlive;
        return;
    }

    // Bitonic compare-exchange on the pair (i, i ^ j); lower index owns it.
    // The keys must come from the VALUES at those positions (sorted[i] /
    // sorted[ij] are particle indices), not from the positions themselves.
    const uint j  = uJ;
    const uint ij = i ^ j;
    if (ij <= i) return;

    const float a = depthKey(sorted[i]);
    const float b = depthKey(sorted[ij]);
    if (a == b) return;                // ties (incl. padding) stay put

    const bool ascending = (i & uK) == 0u;   // half of the bitonic sequence
    if (ascending ? (a > b) : (a < b)) {
        const uint t  = sorted[i];
        sorted[i]     = sorted[ij];
        sorted[ij]    = t;
    }
}
)GLSL";

// Smallest power of two >= v (the bitonic sort pads its work to a power of two).
std::uint32_t nextPow2(std::uint32_t v) {
    if (v <= 1) return 1;
    v--;
    v |= v >> 1; v |= v >> 2; v |= v >> 4; v |= v >> 8; v |= v >> 16;
    return v + 1;
}

std::uint32_t forceBit(const std::string& name) {
    struct Entry { const char* name; std::uint32_t bit; };
    static const Entry kForces[] = {
        {"gravity", 1u << 0}, {"drag", 1u << 1}, {"wind", 1u << 2},
        {"turbulence", 1u << 3}, {"attractors", 1u << 4}, {"vortex", 1u << 5},
        {"spring", 1u << 6}, {"noise_wind", 1u << 7}, {"wave", 1u << 8},
        {"boundary", 1u << 9},
    };
    for (const auto& e : kForces)
        if (name == e.name) return e.bit;
    throw std::runtime_error("ember: unknown force '" + name + "' in forces list");
}

} // namespace

ParticleSystem::ParticleSystem(const Settings& s)
    : capacity_(s.capacity),
      maxSpawnPerFrame_(s.maxSpawnPerFrame),
      frameSeed_(1),
      bufA_(GL_SHADER_STORAGE_BUFFER),
      bufB_(GL_SHADER_STORAGE_BUFFER),
      spawnBuf_(GL_SHADER_STORAGE_BUFFER),
      paletteBuf_(GL_SHADER_STORAGE_BUFFER),
      deadBuf_(GL_SHADER_STORAGE_BUFFER),
      counterBuf_(GL_SHADER_STORAGE_BUFFER),
      attractorBuf_(GL_SHADER_STORAGE_BUFFER),
      vortexBuf_(GL_SHADER_STORAGE_BUFFER),
      springBuf_(GL_SHADER_STORAGE_BUFFER),
      sortedBuf_(GL_SHADER_STORAGE_BUFFER),
      indirectBuf_(GL_SHADER_STORAGE_BUFFER),
      rng_(s.seed) {
    cur_ = &bufA_;
    nxt_ = &bufB_;
    debug_ = std::getenv("EMBER_DEBUG") != nullptr;

    uploadBuiltinSprite();

    if (debug_) {
        std::fprintf(stderr, "[ember] GL_VERSION=%s GL_RENDERER=%s\n",
                     (const char*)glGetString(GL_VERSION), (const char*)glGetString(GL_RENDERER));
        std::fprintf(stderr, "[ember] capacity=%u maxSpawnPerFrame=%u\n", capacity_, maxSpawnPerFrame_);
    }

    const GLsizeiptr pbytes = (GLsizeiptr)capacity_ * (GLsizeiptr)sizeof(Particle);
    std::vector<std::byte> zeros(pbytes);
    bufA_.data(zeros.data(), pbytes, GL_DYNAMIC_COPY);
    bufB_.data(zeros.data(), pbytes, GL_DYNAMIC_COPY);
    spawnBuf_.data(nullptr, (GLsizeiptr)kMaxSpawnRequests * (GLsizeiptr)sizeof(SpawnRequest), GL_DYNAMIC_DRAW);
    paletteBuf_.data(nullptr, 0, GL_DYNAMIC_DRAW);
    deadBuf_.data(nullptr, (GLsizeiptr)capacity_ * (GLsizeiptr)sizeof(GLuint), GL_DYNAMIC_DRAW);
    const GLuint ctr[4] = {0, 0, 0, capacity_};
    counterBuf_.data(ctr, sizeof(ctr), GL_DYNAMIC_DRAW);
    attractorBuf_.data(nullptr, 0, GL_DYNAMIC_DRAW);
    vortexBuf_.data(nullptr, 0, GL_DYNAMIC_DRAW);
    springBuf_.data(nullptr, 0, GL_DYNAMIC_DRAW);
    sortedBuf_.data(nullptr, (GLsizeiptr)nextPow2(capacity_) * (GLsizeiptr)sizeof(GLuint), GL_DYNAMIC_DRAW);
    const GLuint indirectInit[4] = {4, 0, 0, 0}; // {vertexCount, instanceCount, first, baseInstance}
    indirectBuf_.data(indirectInit, sizeof(indirectInit), GL_DYNAMIC_DRAW);

    // Instanced quads: no vertex attributes — the VS expands gl_VertexID.
    vao_.bind();
    vao_.unbind();

    GLfloat range[2] = {1.f, 255.f};
    // GL_ALIASED_POINT_SIZE_RANGE (0x846D) is not emitted by the glad 4.3
    // generator, so use the literal value.
    glGetFloatv((GLenum)0x846D, range);
    maxPointSize_ = range[1];

    ensurePrograms();
}

ParticleSystem::~ParticleSystem() {
    if (bloomFbo_) {
        glDeleteFramebuffers(1, &bloomFbo_);
        glDeleteFramebuffers(1, &bloomHalfFbo_);
        glDeleteFramebuffers(1, &bloomBlurFbo_);
    }
}

ParticleSystem::ParticleSystem(ParticleSystem&& o) noexcept
    : capacity_(o.capacity_),
      maxSpawnPerFrame_(o.maxSpawnPerFrame_),
      alive_(o.alive_),
      time_(o.time_),
      maxPointSize_(o.maxPointSize_),
      debug_(o.debug_),
      frameCount_(o.frameCount_),
      frameSeed_(o.frameSeed_),
      bufA_(std::move(o.bufA_)),
      bufB_(std::move(o.bufB_)),
      spawnBuf_(std::move(o.spawnBuf_)),
      paletteBuf_(std::move(o.paletteBuf_)),
      deadBuf_(std::move(o.deadBuf_)),
      counterBuf_(std::move(o.counterBuf_)),
      attractorBuf_(std::move(o.attractorBuf_)),
      vortexBuf_(std::move(o.vortexBuf_)),
      springBuf_(std::move(o.springBuf_)),
      sortedBuf_(std::move(o.sortedBuf_)),
      indirectBuf_(std::move(o.indirectBuf_)),
      vao_(std::move(o.vao_)),
      spriteTex_(std::move(o.spriteTex_)),
      renderProg_(std::move(o.renderProg_)),
      simProg_(std::move(o.simProg_)),
      sortProg_(std::move(o.sortProg_)),
      emitters_(std::move(o.emitters_)),
      pendingRequests_(std::move(o.pendingRequests_)),
      paletteData_(std::move(o.paletteData_)),
      rng_(std::move(o.rng_)),
      gravity_(o.gravity_),
      drag_(o.drag_),
      wind_(o.wind_),
      turbulence_(o.turbulence_),
      attractors_(std::move(o.attractors_)),
      forceMask_(o.forceMask_),
      dragMode_(o.dragMode_),
      vortexes_(std::move(o.vortexes_)),
      springs_(std::move(o.springs_)),
      noiseWindDir_(o.noiseWindDir_),
      noiseWindAmp_(o.noiseWindAmp_),
      noiseWindScale_(o.noiseWindScale_),
      noiseWindSpeed_(o.noiseWindSpeed_),
      waveDir_(o.waveDir_),
      waveK_(o.waveK_),
      waveAmp_(o.waveAmp_),
      waveOmega_(o.waveOmega_),
      boundaryMode_(o.boundaryMode_),
      boundaryY_(o.boundaryY_),
      restitution_(o.restitution_),
      sizeScale_(o.sizeScale_),
      speedScaleBase_(o.speedScaleBase_),
      useSprite_(o.useSprite_),
      sheetCols_(o.sheetCols_),
      sheetRows_(o.sheetRows_),
      streak_(o.streak_),
      softParticles_(o.softParticles_),
      sceneDepth_(o.sceneDepth_),
      softRadius_(o.softRadius_),
      sortEnabled_(o.sortEnabled_),
      bloom_(o.bloom_),
      bloomThreshold_(o.bloomThreshold_),
      bloomW_(o.bloomW_),
      bloomH_(o.bloomH_),
      bloomTex_(std::move(o.bloomTex_)),
      bloomHalfTex_(std::move(o.bloomHalfTex_)),
      bloomBlurTex_(std::move(o.bloomBlurTex_)),
      bloomFbo_(o.bloomFbo_),
      bloomHalfFbo_(o.bloomHalfFbo_),
      bloomBlurFbo_(o.bloomBlurFbo_),
      bloomPass_(std::move(o.bloomPass_)),
      bloomBlur_(std::move(o.bloomBlur_)),
      bloomComposite_(std::move(o.bloomComposite_)),
      shaderDir_(std::move(o.shaderDir_)),
      blend_(o.blend_),
      depthTest_(o.depthTest_),
      depthWrite_(o.depthWrite_) {
    cur_ = (o.cur_ == &o.bufA_) ? &bufA_ : &bufB_;
    nxt_ = (o.nxt_ == &o.bufA_) ? &bufA_ : &bufB_;
    o.alive_ = 0;
    o.bloomFbo_ = o.bloomHalfFbo_ = o.bloomBlurFbo_ = 0;
}

ParticleSystem& ParticleSystem::operator=(ParticleSystem&& o) noexcept {
    if (this != &o) {
        this->~ParticleSystem();
        new (this) ParticleSystem(std::move(o));
    }
    return *this;
}

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

void ParticleSystem::setGravity(glm::vec3 g) { gravity_ = g; }
void ParticleSystem::setDrag(float k) { drag_ = k; }
void ParticleSystem::setWind(glm::vec3 w) { wind_ = w; }
void ParticleSystem::setTurbulence(float k) { turbulence_ = k; }
void ParticleSystem::setBlendMode(BlendMode m) { blend_ = m; }
void ParticleSystem::setDepthTest(bool on) { depthTest_ = on; }
void ParticleSystem::setDepthWrite(bool on) { depthWrite_ = on; }

void ParticleSystem::setMaxSpawnPerFrame(std::uint32_t n) {
    maxSpawnPerFrame_ = n; // spawn volume is budgeted in update(); no staging buffer to size
}

void ParticleSystem::setAttractors(const std::vector<Attractor>& a) {
    attractors_ = a;
    std::vector<glm::vec4> v;
    v.reserve(a.size());
    for (const auto& at : a) v.emplace_back(at.position, at.strength);
    attractorBuf_.data(v.empty() ? nullptr : v.data(),
                       (GLsizeiptr)(v.size() * sizeof(glm::vec4)), GL_DYNAMIC_DRAW);
}

void ParticleSystem::setForceEnabled(Force f, bool on) {
    const std::uint32_t bit = 1u << (std::uint32_t)f;
    if (on) forceMask_ |= bit;
    else forceMask_ &= ~bit;
}

void ParticleSystem::setForceMask(std::uint32_t mask) { forceMask_ = mask; }

void ParticleSystem::setDragMode(DragMode m) { dragMode_ = m; }

void ParticleSystem::setVortexes(const std::vector<Vortex>& v) {
    vortexes_ = v;
    vortexBuf_.data(v.empty() ? nullptr : v.data(),
                    (GLsizeiptr)(v.size() * sizeof(Vortex)), GL_DYNAMIC_DRAW);
    setForceEnabled(Force::Vortex, !v.empty());
}

void ParticleSystem::setSprings(const std::vector<Spring>& s) {
    springs_ = s;
    springBuf_.data(s.empty() ? nullptr : s.data(),
                    (GLsizeiptr)(s.size() * sizeof(Spring)), GL_DYNAMIC_DRAW);
    setForceEnabled(Force::Spring, !s.empty());
}

void ParticleSystem::setNoiseWind(glm::vec3 dir, float amplitude, float scale, float speed) {
    noiseWindDir_ = dir;
    noiseWindAmp_ = amplitude;
    noiseWindScale_ = scale;
    noiseWindSpeed_ = speed;
    setForceEnabled(Force::NoiseWind, amplitude != 0.f);
}

void ParticleSystem::setWave(glm::vec3 dir, glm::vec3 waveVector, float amplitude, float omega) {
    waveDir_ = dir;
    waveK_ = waveVector;
    waveAmp_ = amplitude;
    waveOmega_ = omega;
    setForceEnabled(Force::Wave, amplitude != 0.f);
}

void ParticleSystem::setBoundary(BoundaryMode mode, float planeY, float restitution) {
    boundaryMode_ = mode;
    boundaryY_ = planeY;
    restitution_ = restitution;
    setForceEnabled(Force::Boundary, true);
}

// ---- size / sprite ---------------------------------------------------------

void ParticleSystem::setSizeScale(float s, bool linkSpeed) {
    const float factor = (sizeScale_ > 0.f && s > 0.f) ? s / sizeScale_ : 1.f; // guard div-by-zero
    sizeScale_ = s;
    if (linkSpeed && factor != 1.f) {
        speedScaleBase_ *= factor;
        for (auto& e : emitters_) e.speedScale *= factor;
    }
}

void ParticleSystem::setUseSprite(bool on) { useSprite_ = on; }

void ParticleSystem::setSpriteTexture(const char* pngPath) {
    if (!pngPath || pngPath[0] == '\0') {
        uploadBuiltinSprite();
        return;
    }
    int w = 0, h = 0, ch = 0;
    unsigned char* data = nullptr;
#ifdef EMBER_USE_STB
    data = stbi_load(pngPath, &w, &h, &ch, 4);
    if (!data) {
        std::fprintf(stderr, "[ember] warning: cannot load sprite '%s' (%s); using built-in gradient\n",
                     pngPath, stbi_failure_reason());
        uploadBuiltinSprite();
        return;
    }
#else
    (void)w; (void)h; (void)ch;
    std::fprintf(stderr,
                 "[ember] warning: cannot load sprite '%s' (EMBER_USE_STB not defined); using built-in gradient\n",
                 pngPath);
    uploadBuiltinSprite();
    return;
#endif
    spriteTex_.uploadRGBA8(w, h, data);
#ifdef EMBER_USE_STB
    stbi_image_free(data);
#endif
    if (debug_) std::fprintf(stderr, "[ember] sprite '%s' loaded (%dx%d)\n", pngPath, w, h);
}

void ParticleSystem::setSpriteSheet(int cols, int rows) {
    sheetCols_ = std::max(1, cols);
    sheetRows_ = std::max(1, rows);
}

void ParticleSystem::uploadBuiltinSprite() {
    constexpr int kSize = 64;
    std::vector<unsigned char> px((std::size_t)kSize * kSize * 4, 0);
    for (int y = 0; y < kSize; ++y) {
        for (int x = 0; x < kSize; ++x) {
            const float dx = (x + 0.5f) / kSize * 2.f - 1.f;
            const float dy = (y + 0.5f) / kSize * 2.f - 1.f;
            const float r = std::sqrt(dx * dx + dy * dy);
            float a = 1.f - std::min(r, 1.f);                 // soft disc
            a += (1.f - std::min(r * r, 1.f)) * 0.35f;        // faint halo
            const std::size_t i = ((std::size_t)y * kSize + (std::size_t)x) * 4;
            px[i + 0] = 255;
            px[i + 1] = 255;
            px[i + 2] = 255;
            px[i + 3] = (unsigned char)(glm::clamp(a, 0.f, 1.f) * 255.f);
        }
    }
    spriteTex_.uploadRGBA8(kSize, kSize, px.data());
}

void ParticleSystem::loadConfig(const char* path) {
    apply(Config::fromFile(path));
}

void ParticleSystem::apply(const Config& cfg) {
    // ------------------------------------------------------------------
    // Phase 1 — validate everything first, so a bad config throws before
    // any state is mutated (loadConfig stays atomic).
    // ------------------------------------------------------------------
    std::uint32_t mask = 0;
    if (cfg.has("system.forces"))
        for (const auto& name : cfg.system.forces) mask |= forceBit(name);

    const DragMode dragMode = cfg.has("system.drag_mode")
        ? (cfg.system.dragMode == "quadratic" ? DragMode::Quadratic
           : cfg.system.dragMode == "linear"   ? DragMode::Linear
           : throw std::runtime_error("ember: unknown drag_mode '" + cfg.system.dragMode + "'"))
        : dragMode_;

    enum class BC { None, Kill, Bounce };
    const BC boundary = cfg.has("system.boundary_mode")
        ? (cfg.system.boundaryMode == "kill"    ? BC::Kill
           : cfg.system.boundaryMode == "bounce" ? BC::Bounce
           : cfg.system.boundaryMode == "none"   ? BC::None
           : throw std::runtime_error("ember: unknown boundary_mode '" + cfg.system.boundaryMode + "'"))
        : BC::None;

    const BlendMode blend = cfg.has("system.blend")
        ? (cfg.system.blend == "additive" ? BlendMode::Additive
           : cfg.system.blend == "normal" ? BlendMode::Normal
           : throw std::runtime_error("ember: unknown blend mode '" + cfg.system.blend + "'"))
        : blend_;

    for (const auto& ec : cfg.emitters)
        if (!ec.paletteName.empty() && !cfg.findPalette(ec.paletteName.c_str()))
            throw std::runtime_error("ember: unknown palette '" + ec.paletteName +
                                     "' referenced by emitter '" + ec.name + "'");

    // ------------------------------------------------------------------
    // Phase 2 — apply. Only keys/sections actually present in the config
    // are touched; anything the config does not mention keeps its current
    // (programmatic) value.
    // ------------------------------------------------------------------
    if (cfg.system.capacity != 0 && cfg.system.capacity != capacity_) {
        capacity_ = cfg.system.capacity;
        alive_ = 0;
        const GLsizeiptr pbytes = (GLsizeiptr)capacity_ * (GLsizeiptr)sizeof(Particle);
        std::vector<std::byte> zeros(pbytes);
        bufA_.data(zeros.data(), pbytes, GL_DYNAMIC_COPY);
        bufB_.data(zeros.data(), pbytes, GL_DYNAMIC_COPY);
        deadBuf_.data(nullptr, (GLsizeiptr)capacity_ * (GLsizeiptr)sizeof(GLuint), GL_DYNAMIC_DRAW);
        sortedBuf_.data(nullptr, (GLsizeiptr)nextPow2(capacity_) * (GLsizeiptr)sizeof(GLuint), GL_DYNAMIC_DRAW);
        const GLuint ctr[4] = {0, 0, 0, capacity_};
        counterBuf_.data(ctr, sizeof(ctr), GL_DYNAMIC_DRAW);
    }
    if (cfg.system.maxSpawnPerFrame != 0) setMaxSpawnPerFrame(cfg.system.maxSpawnPerFrame);

    if (cfg.has("system.gravity")) setGravity(cfg.system.gravity);
    if (cfg.has("system.drag")) setDrag(cfg.system.drag);
    if (cfg.has("system.wind")) setWind(cfg.system.wind);
    if (cfg.has("system.turbulence")) setTurbulence(cfg.system.turbulence);
    if (cfg.has("system.drag_mode")) setDragMode(dragMode);

    if (cfg.has("system.noise_wind_direction") || cfg.has("system.noise_wind_amplitude") ||
        cfg.has("system.noise_wind_scale") || cfg.has("system.noise_wind_speed"))
        setNoiseWind(cfg.system.noiseWindDir, cfg.system.noiseWindAmp,
                     cfg.system.noiseWindScale, cfg.system.noiseWindSpeed);
    if (cfg.has("system.wave_direction") || cfg.has("system.wave_vector") ||
        cfg.has("system.wave_amplitude") || cfg.has("system.wave_omega"))
        setWave(cfg.system.waveDir, cfg.system.waveK, cfg.system.waveAmp, cfg.system.waveOmega);

    if (cfg.has("system.boundary_mode")) {
        if (boundary == BC::Kill) setBoundary(BoundaryMode::Kill, cfg.system.boundaryY, cfg.system.restitution);
        else if (boundary == BC::Bounce) setBoundary(BoundaryMode::Bounce, cfg.system.boundaryY, cfg.system.restitution);
        else disableForce(Force::Boundary);
    } else if (cfg.has("system.boundary_y") || cfg.has("system.restitution")) {
        boundaryY_ = cfg.system.boundaryY; // param-only update, mask untouched
        restitution_ = cfg.system.restitution;
    }

    if (cfg.has("system.blend")) setBlendMode(blend);
    if (cfg.has("system.debug")) debug_ = cfg.system.debug;
    if (cfg.has("system.size_scale")) setSizeScale(cfg.system.sizeScale, cfg.system.scaleSpeedWithSize);
    if (cfg.has("system.texture") && !cfg.system.texture.empty())
        setSpriteTexture(cfg.system.texture.c_str());
    if (cfg.has("system.streak")) setStreak(cfg.system.streak);
    if (cfg.has("system.soft_particles")) {
        softParticles_ = cfg.system.softParticles; // scene depth stays host-provided
        softRadius_ = cfg.system.softRadius;
    }
    if (cfg.has("system.bloom")) setBloom(cfg.system.bloom);
    if (cfg.has("system.bloom_threshold")) setBloomThreshold(cfg.system.bloomThreshold);
    if (cfg.has("system.sort")) setSortEnabled(cfg.system.sort);

    if (cfg.has("emitter")) {
        clearEmitters();
        for (const auto& ec : cfg.emitters) {
            Emitter e = ec; // slices EmitterConfig -> Emitter (drops name/paletteName)
            if (!ec.paletteName.empty()) // validated in phase 1, so non-null
                e.palette = cfg.findPalette(ec.paletteName.c_str())->colors;
            addEmitter(e);
        }
    }
    if (cfg.has("attractor")) setAttractors(cfg.attractors);
    if (cfg.has("vortex")) setVortexes(cfg.vortexes);
    if (cfg.has("spring")) setSprings(cfg.springs);

    // An explicit forces list is the final word on the mask (overrides the
    // auto-enables from the parameter setters above).
    if (cfg.has("system.forces")) setForceMask(mask);
}

// ---------------------------------------------------------------------------
// Emitters
// ---------------------------------------------------------------------------

Emitter& ParticleSystem::addEmitter(const Emitter& e) {
    emitters_.push_back(e);
    // Inherit the accumulated size<->speed link factor (setSizeScale(.., true)).
    emitters_.back().speedScale *= speedScaleBase_;
    registerEmitterPalette(emitters_.back());
    return emitters_.back();
}

void ParticleSystem::removeEmitter(std::size_t index) {
    if (index < emitters_.size()) emitters_.erase(emitters_.begin() + (std::ptrdiff_t)index);
    rebuildPaletteBuffer(); // palette ranges shift; re-upload from the live emitters
}

void ParticleSystem::clearEmitters() {
    emitters_.clear();
    rebuildPaletteBuffer();
}

void ParticleSystem::registerEmitterPalette(Emitter& e) {
    e.paletteIdx = -1;
    e.paletteCount = 0;
    if (!e.palette.empty()) {
        e.paletteIdx = (int)paletteData_.size();
        e.paletteCount = (int)e.palette.size();
        paletteData_.insert(paletteData_.end(), e.palette.begin(), e.palette.end());
        paletteBuf_.data(paletteData_.data(),
                         (GLsizeiptr)(paletteData_.size() * sizeof(glm::vec4)), GL_DYNAMIC_DRAW);
    }
}

void ParticleSystem::rebuildPaletteBuffer() {
    paletteData_.clear();
    for (auto& e : emitters_) registerEmitterPalette(e);
    paletteBuf_.data(paletteData_.empty() ? nullptr : paletteData_.data(),
                     (GLsizeiptr)(paletteData_.size() * sizeof(glm::vec4)), GL_DYNAMIC_DRAW);
}

// ---------------------------------------------------------------------------
// Simulation
// ---------------------------------------------------------------------------

void ParticleSystem::update(float dt) {
    dt = std::min(dt, 0.1f); // clamp for stability; variable dt is fine below this
    time_ += dt;

    // 1) Encode spawn requests: pending bursts first, then emitters (capped
    //    total). The GPU samples the particles; the CPU only accumulates the
    //    per-emitter counts (rate*dt) and assigns prefix offsets.
    std::vector<SpawnRequest> reqs;
    reqs.reserve(pendingRequests_.size() + emitters_.size());
    std::uint32_t total = 0;
    const std::uint32_t budget = maxSpawnPerFrame_;
    for (const auto& r : pendingRequests_) {
        const std::uint32_t cnt = std::min(r.count, budget - total);
        if (cnt > 0) {
            SpawnRequest rr = r;
            rr.base = total;
            rr.count = cnt;
            reqs.push_back(rr);
            total += cnt;
        }
    }
    pendingRequests_.clear();
    for (auto& e : emitters_) {
        const std::uint32_t cnt = e.takeCount(dt, budget - total);
        if (cnt > 0) {
            reqs.push_back(e.makeRequest(total, cnt));
            total += cnt;
        }
    }

    const std::uint32_t nr = (std::uint32_t)reqs.size();
    if (nr > 0) {
        spawnBuf_.subData(0, (GLsizeiptr)nr * (GLsizeiptr)sizeof(SpawnRequest), reqs.data());
    }
    counterBuf_.subData(2 * sizeof(GLuint), sizeof(GLuint), &nr); // uSpawnRequestCount

    // 2) Run the simulation.
    if (!simProg_) ensurePrograms();
    glUseProgram(simProg_.id());
    // (Shader::set* caches uniform locations; -1 locations are no-ops)
    simProg_.setFloat("uDt", dt);
    simProg_.setFloat("uTime", time_);
    simProg_.setUint("uForceMask", forceMask_);
    simProg_.setVec3("uGravity", gravity_);
    simProg_.setFloat("uDrag", drag_);
    simProg_.setInt("uDragMode", (int)dragMode_);
    simProg_.setVec3("uWind", wind_);
    simProg_.setFloat("uTurbulence", turbulence_);
    simProg_.setInt("uAttractorCount", (int)attractors_.size());
    simProg_.setInt("uVortexCount", (int)vortexes_.size());
    simProg_.setInt("uSpringCount", (int)springs_.size());
    simProg_.setVec3("uNoiseWindDir", noiseWindDir_);
    simProg_.setFloat("uNoiseWindAmp", noiseWindAmp_);
    simProg_.setFloat("uNoiseWindScale", noiseWindScale_);
    simProg_.setFloat("uNoiseWindSpeed", noiseWindSpeed_);
    simProg_.setVec3("uWaveDir", waveDir_);
    simProg_.setVec3("uWaveK", waveK_);
    simProg_.setFloat("uWaveAmp", waveAmp_);
    simProg_.setFloat("uWaveOmega", waveOmega_);
    simProg_.setInt("uBoundaryMode", (int)boundaryMode_);
    simProg_.setFloat("uBoundaryY", boundaryY_);
    simProg_.setFloat("uRestitution", restitution_);
    simProg_.setUint("uSpawnTotal", total);
    simProg_.setUint("uFrameSeed", frameSeed_++);

    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kBindingCur, cur_->id());
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kBindingNext, nxt_->id());
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kBindingSpawn, spawnBuf_.id());
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kBindingDead, deadBuf_.id());
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kBindingCounters, counterBuf_.id());
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kBindingAttractors, attractorBuf_.id());
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kBindingVortexes, vortexBuf_.id());
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kBindingSprings, springBuf_.id());
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kBindingPalette, paletteBuf_.id());
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kBindingIndirect, indirectBuf_.id()); // phase 2 writes draw args

    simProg_.setInt("uPhase", 0); // simulate
    if (alive_ > 0) {
        glDispatchCompute((alive_ + kSimGroupSize - 1u) / kSimGroupSize, 1, 1);
    }
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);

    simProg_.setInt("uPhase", 1); // spawn (GPU samples particles from requests)
    if (total > 0) {
        glDispatchCompute((total + kSimGroupSize - 1u) / kSimGroupSize, 1, 1);
    }
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);

    // Publish draw args for glDrawArraysIndirect (render no longer depends on
    // the CPU alive readback).
    simProg_.setInt("uPhase", 2);
    glDispatchCompute(1, 1, 1);
    glMemoryBarrier(GL_COMMAND_BARRIER_BIT);

    // 3) Read back the alive count (for aliveCount()/UI/next-frame dispatch).
    counterBuf_.getSubData(0, sizeof(GLuint), &alive_);
    alive_ = std::min(alive_, capacity_);

    // 4) Debug diagnostics (EMBER_DEBUG=1 / config debug=true): ~10 lines/sec + GL error sweep.
    if (debug_ && (frameCount_ % 6 == 0)) {
        GLuint ctr[4] = {0, 0, 0, 0};
        counterBuf_.getSubData(0, sizeof(ctr), ctr);
        Particle p0{};
        cur_->getSubData(0, sizeof(Particle), &p0);
        std::fprintf(stderr,
                     "[ember] frame=%llu alive=%u head=%u reqs=%u total=%u cap=%u mask=0x%X dt=%.4f\n"
                     "        p0 pos=(%.3f,%.3f,%.3f) size=%.3f vel=(%.2f,%.2f,%.2f) age=%.3f life=%.3f col=(%.2f,%.2f,%.2f,%.2f)\n",
                     (unsigned long long)frameCount_, ctr[0], ctr[1], nr, total, ctr[3], forceMask_, dt,
                     p0.pos.x, p0.pos.y, p0.pos.z, p0.pos.w,
                     p0.vel.x, p0.vel.y, p0.vel.z,
                     p0.vel.w, p0.life.x,
                     p0.color.r, p0.color.g, p0.color.b, p0.color.a);
        const int errs = gl::popErrors("update");
        if (errs > 0) std::fprintf(stderr, "[ember] %d GL error(s) after update\n", errs);
    }
    ++frameCount_;

    // 5) Swap: the freshly written buffer becomes the render/sim source.
    std::swap(cur_, nxt_);
}

void ParticleSystem::burst(const BurstParams& b) {
    // Encode as a single GPU spawn request (the shader samples the particles).
    SpawnRequest r{};
    r.colorMin = b.colorMin;
    r.colorMax = b.colorMax;
    r.position = b.position;
    r.dir = glm::vec3(0.f, 1.f, 0.f); // spread mixes away from +Y (matches the old CPU path)
    r.spread = b.spread;
    r.speedMin = b.speedMin;
    r.speedMax = b.speedMax;
    r.lifeMin = b.lifeMin;
    r.lifeMax = b.lifeMax;
    r.sizeMin = b.sizeMin;
    r.sizeMax = b.sizeMax;
    r.shape = (int)Emitter::Shape::Point;
    r.paletteIdx = -1;
    r.count = std::min(b.count, maxSpawnPerFrame_);
    pendingRequests_.push_back(r);
    // Keep the pending queue bounded; drop the oldest overflow.
    if (pendingRequests_.size() > kMaxSpawnRequests) {
        pendingRequests_.erase(pendingRequests_.begin(),
                               pendingRequests_.begin() +
                                   (std::ptrdiff_t)(pendingRequests_.size() - kMaxSpawnRequests));
    }
}

std::vector<Particle> ParticleSystem::readParticles(std::uint32_t max) const {
    const std::uint32_t n = std::min(max == 0 ? alive_ : std::min(max, alive_), capacity_);
    std::vector<Particle> out(n);
    if (n > 0) cur_->getSubData(0, (GLsizeiptr)n * (GLsizeiptr)sizeof(Particle), out.data());
    return out;
}

void ParticleSystem::clear() {
    alive_ = 0;
    pendingRequests_.clear();
    const GLsizeiptr pbytes = (GLsizeiptr)capacity_ * (GLsizeiptr)sizeof(Particle);
    std::vector<std::byte> zeros(pbytes);
    bufA_.data(zeros.data(), pbytes, GL_DYNAMIC_COPY);
    bufB_.data(zeros.data(), pbytes, GL_DYNAMIC_COPY);
    const GLuint ctr[4] = {0, 0, 0, capacity_};
    counterBuf_.data(ctr, sizeof(ctr), GL_DYNAMIC_DRAW);
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

void ParticleSystem::setStreak(float k) { streak_ = k; }

void ParticleSystem::setSoftParticles(bool on, GLuint sceneDepthTex, float radius) {
    softParticles_ = on;
    if (on) {
        sceneDepth_ = sceneDepthTex;
        softRadius_ = radius;
    }
}

void ParticleSystem::setSortEnabled(bool on) { sortEnabled_ = on; }

void ParticleSystem::setBloom(bool on) { bloom_ = on; }
void ParticleSystem::setBloomThreshold(float t) { bloomThreshold_ = t; }

void ParticleSystem::render(const glm::mat4& view, const glm::mat4& proj,
                            float viewportWidth, float viewportHeight, float fovYDeg) {
    if (alive_ == 0) return;
    if (!renderProg_) ensurePrograms();
    if (sortEnabled_) sortParticles(view); // back-to-front order for correct blending
    (void)fovYDeg; // reserved (camera fov not needed by view-space quads)
    if (!bloom_) {
        drawParticles(view, proj, viewportWidth, viewportHeight);
        return;
    }

    // ---- bloom chain: particles -> HDR fbo -> bright/downsample -> blur -> add ----
    ensureBloom((int)viewportWidth, (int)viewportHeight);
    if (!bloomPass_) { // shader files missing: degrade gracefully
        drawParticles(view, proj, viewportWidth, viewportHeight);
        return;
    }
    GLint prevFbo = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);

    glBindFramebuffer(GL_FRAMEBUFFER, bloomFbo_);
    glViewport(0, 0, (int)viewportWidth, (int)viewportHeight);
    glClearColor(0.f, 0.f, 0.f, 0.f);
    glClear(GL_COLOR_BUFFER_BIT);
    drawParticles(view, proj, viewportWidth, viewportHeight);

    // Base pass: composite the FULL (unblurred) particle image onto the host
    // framebuffer, so bloom is an additive glow ON TOP of the real particles
    // (without this the host framebuffer only ever sees the blurred bright pass).
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prevFbo);
    glViewport(0, 0, (int)viewportWidth, (int)viewportHeight);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE);
    glUseProgram(bloomComposite_.id());
    bloomComposite_.setInt("uTex", 0);
    bloomTex_.bind();
    drawFullscreen();
    glDisable(GL_BLEND);

    // bright pass + 2x2 downsample to half res
    glBindFramebuffer(GL_FRAMEBUFFER, bloomHalfFbo_);
    glViewport(0, 0, bloomW_ / 2, bloomH_ / 2);
    glDisable(GL_BLEND);
    glUseProgram(bloomPass_.id());
    bloomPass_.setInt("uTex", 0);
    bloomPass_.setFloat("uThreshold", bloomThreshold_);
    bloomPass_.setVec2("uTexel", {1.f / (float)bloomW_, 1.f / (float)bloomH_});
    glActiveTexture(GL_TEXTURE0);
    bloomTex_.bind();
    drawFullscreen();

    // two ping-pong blur passes
    for (int i = 0; i < 2; ++i) {
        glBindFramebuffer(GL_FRAMEBUFFER, i == 0 ? bloomBlurFbo_ : bloomHalfFbo_);
        glUseProgram(bloomBlur_.id());
        bloomBlur_.setInt("uTex", 0);
        bloomBlur_.setVec2("uTexel", {1.f / (float)(bloomW_ / 2), 1.f / (float)(bloomH_ / 2)});
        bloomBlur_.setVec2("uDir", i == 0 ? glm::vec2(1.f, 0.f) : glm::vec2(0.f, 1.f));
        (i == 0 ? bloomHalfTex_ : bloomBlurTex_).bind();
        drawFullscreen();
    }

    // additive composite onto the caller's framebuffer.
    // The final blur lives in bloomHalfTex_ (pass 0: H -> bloomBlurTex_,
    // pass 1: V -> bloomHalfTex_) — compositing bloomBlurTex_ would use the
    // horizontal-only intermediate and stretch the glow.
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prevFbo);
    glViewport(0, 0, (int)viewportWidth, (int)viewportHeight);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE);
    glUseProgram(bloomComposite_.id());
    bloomComposite_.setInt("uTex", 0);
    bloomHalfTex_.bind();
    drawFullscreen();
    glDisable(GL_BLEND);
}

void ParticleSystem::drawParticles(const glm::mat4& view, const glm::mat4& proj,
                                   float viewportWidth, float viewportHeight) {
    glUseProgram(renderProg_.id());
    renderProg_.setMat4("uView", view);
    renderProg_.setMat4("uProj", proj);
    renderProg_.setFloat("uSizeScale", sizeScale_);
    renderProg_.setFloat("uStreak", streak_);
    renderProg_.setInt("uUseSorted", sortEnabled_ ? 1 : 0);
    renderProg_.setInt("uUseSprite", useSprite_ ? 1 : 0);
    renderProg_.setInt("uSheetCols", sheetCols_);
    renderProg_.setInt("uSheetRows", sheetRows_);
    const bool soft = softParticles_ && sceneDepth_ != 0;
    renderProg_.setInt("uUseSoft", soft ? 1 : 0);
    if (soft) {
        renderProg_.setMat4("uInvViewProj", glm::inverse(proj * view));
        renderProg_.setFloat("uSoftRadius", softRadius_);
        renderProg_.setVec2("uViewportSize", {viewportWidth, viewportHeight});
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, sceneDepth_);
        renderProg_.setInt("uSceneDepth", 1);
    }
    glActiveTexture(GL_TEXTURE0);
    spriteTex_.bind();
    renderProg_.setInt("uSprite", 0);

    glEnable(GL_BLEND);
    if (blend_ == BlendMode::Additive) glBlendFunc(GL_SRC_ALPHA, GL_ONE);
    else glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    if (depthTest_) glEnable(GL_DEPTH_TEST);
    else glDisable(GL_DEPTH_TEST);
    glDepthMask(depthWrite_ ? GL_TRUE : GL_FALSE);

    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kBindingCur, cur_->id());
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kBindingSorted, sortedBuf_.id());
    vao_.bind();
    // Draw count comes from the GPU-written indirect args (phase 2 of update());
    // the CPU alive_ readback is only for aliveCount()/UI — not the render path.
    glBindBuffer(GL_DRAW_INDIRECT_BUFFER, indirectBuf_.id());
    glDrawArraysIndirect(GL_TRIANGLE_STRIP, nullptr);
    glBindBuffer(GL_DRAW_INDIRECT_BUFFER, 0);
    vao_.unbind();

    glDepthMask(GL_TRUE); // restore
}

void ParticleSystem::drawFullscreen() {
    vao_.bind();
    glDrawArrays(GL_TRIANGLES, 0, 3);
    vao_.unbind();
}

void ParticleSystem::ensureBloom(int w, int h) {
    if (bloomFbo_ != 0 && w == bloomW_ && h == bloomH_) return;
    bloomW_ = w;
    bloomH_ = h;
    const int hw = std::max(1, w / 2), hh = std::max(1, h / 2);
    bloomTex_.uploadRGBA16F(w, h);
    bloomHalfTex_.uploadRGBA16F(hw, hh);
    bloomBlurTex_.uploadRGBA16F(hw, hh);
    if (bloomFbo_ == 0) {
        GLuint fbos[3] = {0, 0, 0};
        glGenFramebuffers(3, fbos);
        bloomFbo_ = fbos[0];
        bloomHalfFbo_ = fbos[1];
        bloomBlurFbo_ = fbos[2];
        glBindFramebuffer(GL_FRAMEBUFFER, bloomFbo_);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, bloomTex_.id(), 0);
        glBindFramebuffer(GL_FRAMEBUFFER, bloomHalfFbo_);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, bloomHalfTex_.id(), 0);
        glBindFramebuffer(GL_FRAMEBUFFER, bloomBlurFbo_);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, bloomBlurTex_.id(), 0);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        // completeness probe (bloom FBOs are RGBA16F color-only)
        for (int i = 0; i < 3; ++i) {
            GLuint fbo = fbos[i];
            GLenum st = 0;
            glBindFramebuffer(GL_FRAMEBUFFER, fbo);
            st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
            if (st != GL_FRAMEBUFFER_COMPLETE)
                std::fprintf(stderr, "[ember] warning: bloom FBO[%d] incomplete (0x%X)\n", i, st);
        }
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    } else {
        glBindFramebuffer(GL_FRAMEBUFFER, bloomFbo_);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, bloomTex_.id(), 0);
        glBindFramebuffer(GL_FRAMEBUFFER, bloomHalfFbo_);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, bloomHalfTex_.id(), 0);
        glBindFramebuffer(GL_FRAMEBUFFER, bloomBlurFbo_);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, bloomBlurTex_.id(), 0);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }
    // bloom shaders (file-only, sourced from shaderDir_ like the main shaders;
    // missing files disable bloom gracefully)
    if (!bloomPass_) {
        const std::string dir = shaderDir_.empty() ? "" : shaderDir_ + "/";
        try {
            bloomPass_ = Shader::fromVertFrag((dir + "bloom.vert").c_str(), (dir + "bloom_threshold.frag").c_str());
            bloomBlur_ = Shader::fromVertFrag((dir + "bloom.vert").c_str(), (dir + "bloom_blur.frag").c_str());
            bloomComposite_ = Shader::fromVertFrag((dir + "bloom.vert").c_str(), (dir + "bloom_composite.frag").c_str());
        } catch (const std::exception& e) {
            std::fprintf(stderr, "[ember] warning: bloom shaders unavailable (%s); bloom disabled\n", e.what());
            bloom_ = false;
        }
    }
}

void ParticleSystem::ensureSortProgram() {
    if (sortProg_) return;
    // Same sourcing policy as the sim/render programs: editable file first,
    // embedded copy as fallback (so embedding stays zero-file).
    const std::string dir = shaderDir_.empty() ? "" : shaderDir_ + "/";
    try {
        sortProg_ = Shader::fromCompute((dir + "sort.comp").c_str());
        if (debug_) std::fprintf(stderr, "[ember] sort shader loaded from %s\n", dir.c_str());
    } catch (const std::exception& e) {
        if (debug_) std::fprintf(stderr, "[ember] sort.comp unavailable (%s); using embedded\n", e.what());
        try {
            sortProg_ = Shader::fromSources({{GL_COMPUTE_SHADER, kSortComp}});
        } catch (const std::exception& e2) {
            // Degrade gracefully like bloom: never throw from render().
            std::fprintf(stderr, "[ember] warning: sort shader unavailable (%s); depth sort disabled\n", e2.what());
            sortEnabled_ = false;
        }
    }
}

void ParticleSystem::sortParticles(const glm::mat4& view) {
    ensureSortProgram();
    const std::uint32_t N = nextPow2(alive_); // padded work size (>= 1)
    glUseProgram(sortProg_.id());
    sortProg_.setMat4("uView", view);
    sortProg_.setUint("uAlive", alive_);
    sortProg_.setUint("uPaddedN", N);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kBindingCur, cur_->id());
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kBindingSorted, sortedBuf_.id());

    // Identity fill: sorted[] must be a permutation of [0, alive) every frame
    // (recycled slots and freshly appended particles change the active set).
    sortProg_.setUint("uMode", 0);
    glDispatchCompute((N + kSimGroupSize - 1u) / kSimGroupSize, 1, 1);
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);

    // Bitonic stages: k = 2, 4, 8, ...; j = k/2 ... 1 (classic network).
    // One dispatch per step — each element is owned by a single thread per
    // step, so the barrier between dispatches is the only sync needed.
    for (std::uint32_t k = 2; k <= N; k <<= 1) {
        for (std::uint32_t j = k >> 1; j > 0; j >>= 1) {
            sortProg_.setUint("uK", k);
            sortProg_.setUint("uJ", j);
            sortProg_.setUint("uMode", 1);
            glDispatchCompute((N + kSimGroupSize - 1u) / kSimGroupSize, 1, 1);
            glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
        }
    }
}

void ParticleSystem::setPrograms(Shader render, Shader simulate) {
    renderProg_ = std::move(render);
    simProg_ = std::move(simulate);
}

void ParticleSystem::setShaderDirectory(const char* dir) { shaderDir_ = dir ? dir : ""; }

void ParticleSystem::ensurePrograms() {
    if (renderProg_ || simProg_) return;
    // Prefer editable files in the shader directory (shaders/particle.vert,
    // .frag, simulate.comp); fall back to the embedded copies when the files
    // are missing or fail to compile, so embedding stays zero-file.
    const std::string dir = shaderDir_.empty() ? "" : shaderDir_ + "/";
    bool fromFiles = true;
    try {
        renderProg_ = Shader::fromVertFrag((dir + "particle.vert").c_str(), (dir + "particle.frag").c_str());
        simProg_ = Shader::fromCompute((dir + "simulate.comp").c_str());
    } catch (const std::exception& e) {
        if (debug_) std::fprintf(stderr, "[ember] shader files unavailable (%s); using embedded\n", e.what());
        renderProg_ = Shader();
        simProg_ = Shader();
        fromFiles = false;
    }
    if (!fromFiles) {
        renderProg_ = Shader::fromSources({
            {GL_VERTEX_SHADER, kParticleVert},
            {GL_FRAGMENT_SHADER, kParticleFrag},
        });
        simProg_ = Shader::fromSources({{GL_COMPUTE_SHADER, kSimulateComp}});
    } else if (debug_) {
        std::fprintf(stderr, "[ember] shaders loaded from %s/\n", dir.c_str());
    }
}

} // namespace ember
