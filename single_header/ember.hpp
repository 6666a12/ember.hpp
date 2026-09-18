/* ============================================================================
 * ember.hpp — ember GPU particle library · single-header edition
 * ============================================================================
 * GENERATED FILE — do not edit by hand. Regenerate with:
 *     python tools/amalgamate.py           (--verify checks for drift)
 * ============================================================================
 * CONTENTS
 *   Public CPU data, backend contract and ParticleSystem facade
 *   OpenGL resources, shaders, simulation, statistics and rendering
 *   Optional GLFW convenience module
 * ============================================================================
 * QUICK START — in exactly ONE translation unit:
 *
 *     #define EMBER_IMPLEMENTATION
 *     #define EMBER_USE_GLFW            // optional: GLFW window module
 *     #define EMBER_USE_STB             // optional: PNG sprites
 *     #include "ember.hpp"
 *
 *     int main() {
 *         ember::Window win(1280, 720, "demo");   // 4.3 core context + gl::init
 *         ember::ParticleSystem sys({100000, 30000});
 *         sys.setGravity({0.f, -9.81f, 0.f});
 *         // main loop: win.pollEvents(); sys.update(dt);
 *         //            sys.render(view, proj, fbW, fbH, fovYDeg);
 *     }
 * ============================================================================
 * FEATURE MACROS
 *     EMBER_IMPLEMENTATION  define in ONE TU to compile the implementation
 *     EMBER_USE_GLFW        include the GLFW window convenience module
 *     EMBER_USE_STB         PNG sprite support (define STB_IMAGE_IMPLEMENTATION
 *                           in some TU as well)
 * ============================================================================
 * DEPENDENCIES (documented in INTEGRATION docs, §2)
 *     glad        OpenGL 4.3 core loader — include <glad/gl.h> + compile glad.c
 *     glm (>=0.9.9) header-only math
 *     GLFW 3.4    only with EMBER_USE_GLFW
 *     stb_image.h only with EMBER_USE_STB
 * ============================================================================
 * LICENSE — MIT (see the LICENSE file in the repository root).
 * ==========================================================================*/

/* ============================================================================
 * SECTION A — DECLARATIONS
 * ==========================================================================*/

#ifndef EMBER_SINGLE_HEADER_HPP
#define EMBER_SINGLE_HEADER_HPP

/* ============================================================================
 * [ 1] core.hpp
 * core.hpp
 * ========================================================================== */

// Core data types shared between the CPU side and the simulation.

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <random>

namespace ember {

// One particle. Layout must match the GLSL `Particle` struct (std430):
//   pos.xyz = position, pos.w = world-space size
//   vel.xyz = velocity, vel.w = age
//   life.x  = lifetime;  life.x < 0 marks a "corpse" slot owned by the free stack
//   color    = rgba
struct Particle {
    glm::vec4 pos;
    glm::vec4 vel;
    glm::vec4 life;
    glm::vec4 color;
};
static_assert(sizeof(Particle) == 64, "Particle must stay std430-compatible (64 bytes)");

// A point force. Positive strength attracts, negative repels.
// strength acts as a gravitation-like pull with inverse-square falloff + softening.
struct Attractor {
    glm::vec3 position{0.f};
    float strength = 0.f;
};

enum class DragMode {
    Linear,    // a = -v * k            — default, gentle air damping
    Quadratic, // a = -v * |v| * k      — stronger at high speed, fluid-like
};

// What happens when a particle crosses the boundary plane (y < planeY).
enum class BoundaryMode {
    Kill,   // particle dies on contact
    Bounce, // reflect velocity.y with restitution
};

// Tangential swirl around `axis` through `center`.
// Strength sign flips the rotation direction.
struct Vortex {
    glm::vec4 center{0.f};                       // xyz position, w = radius (falloff scale)
    glm::vec4 axisStrength{0.f, 1.f, 0.f, 1.f};  // xyz axis, w = strength
};
static_assert(sizeof(Vortex) == 32, "Vortex must stay std430-compatible (32 bytes)");

// Spring-damper pulling particles toward `anchor`:
//   a = (anchor - p) * stiffness - v * damping
struct alignas(16) Spring {
    glm::vec3 anchor{0.f};
    float stiffness = 0.f;
    float damping = 0.f;
};
static_assert(sizeof(Spring) == 32, "Spring must match std430 array stride (32 bytes)");
static_assert(offsetof(Spring, anchor) == 0 && offsetof(Spring, stiffness) == 12 &&
              offsetof(Spring, damping) == 16, "Spring member offsets must match GLSL");

enum class BlendMode {
    Additive, // glBlendFunc(GL_SRC_ALPHA, GL_ONE)          — default, glows
    Normal,   // glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA)
};

// One-shot burst (used for fireworks etc.). Applied on the next update().
struct BurstParams {
    std::uint32_t count = 100;
    glm::vec3 position{0.f};
    float speedMin = 5.f, speedMax = 10.f;
    float spread = 1.f; // 0 = single direction, 1 = uniform sphere
    float lifeMin = 1.f, lifeMax = 2.f;
    float sizeMin = 0.05f, sizeMax = 0.12f;
    glm::vec4 colorMin{1.f}, colorMax{1.f};
    bool refractive = false; // refractive particles (glass shards)
};

// Small RNG wrapper around std::mt19937. Default-seeded from std::random_device.
class Rng {
public:
    explicit Rng(std::uint32_t seed = 0) {
        gen_.seed(seed != 0 ? seed : std::random_device{}());
    }

    float unit() { return std::uniform_real_distribution<float>(0.f, 1.f)(gen_); }

    float range(float a, float b) { return a + (b - a) * unit(); }

    glm::vec3 box(glm::vec3 lo, glm::vec3 hi) {
        return glm::vec3(range(lo.x, hi.x), range(lo.y, hi.y), range(lo.z, hi.z));
    }

    // Uniform direction on the unit sphere.
    glm::vec3 dirOnSphere() {
        const float z = range(-1.f, 1.f);
        const float a = range(0.f, 6.283185307179586f);
        const float r = std::sqrt(std::max(0.f, 1.f - z * z));
        return glm::vec3(r * std::cos(a), r * std::sin(a), z);
    }

    // Random color, per-component lerp between a and b.
    glm::vec4 color(glm::vec4 a, glm::vec4 b) {
        return glm::vec4(range(a.r, b.r), range(a.g, b.g), range(a.b, b.b), range(a.a, b.a));
    }

private:
    std::mt19937 gen_;
};

} // namespace ember
/* ============================================================================
 * [ 2] emitters.hpp
 * emitters.hpp
 * ========================================================================== */

// CPU-side emitters: they generate spawn data for the GPU simulation.

#include <cstring>
#include <vector>
#include <stdexcept>

namespace ember {

// Compact GPU spawn request: the CPU encodes emitters/bursts into a few of
// these per frame (~176 B each); the compute shader samples the actual
// particles (shape, cone, palette, fade, size<->speed link) with its own RNG.
// Layout must match the GLSL `SpawnRequest` in simulate.comp exactly (std430).
struct SpawnRequest {
    glm::vec4 colorMin;        // 0
    glm::vec4 colorMax;        // 16
    glm::vec3 position;        // 32  spawn origin
    float radius;              // 44  Sphere/Cone base radius
    glm::vec3 axis;            // 48  Cone axis (normalized; zero = no cone)
    float coneAngle;           // 60  degrees; >0 => cone emission
    glm::vec3 dir;             // 64  normalized base velocity (zero = uniform sphere)
    float spread;              // 76  0..1 mix toward a random sphere direction
    glm::vec3 extents;         // 80  Box half-extents
    float speedMin;            // 92  already includes speedScale
    float speedMax;            // 96
    float speedSizeLink;       // 100 per-particle size<->speed link
    float lifeMin;             // 104
    float lifeMax;             // 108
    float sizeMin;             // 112
    float sizeMax;             // 116
    std::int32_t shape;        // 120 Emitter::Shape
    std::int32_t paletteIdx;   // 124 base index into the GPU palette buffer (-1 = colorMin/Max)
    std::int32_t flags;        // 128 bit0 = fade-to-color, bit1 = refractive (sign-encoded in pos.w)
    std::uint32_t base;        // 132 prefix offset: spawns work ids [base, base+count)
    std::uint32_t count;       // 136 particles this request produces
    std::int32_t paletteCount; // 140 colors in this palette range
    glm::vec3 fadeMin;         // 144 fade target range (RGB)
    // Event tag: bits 0-15 = onDeath+1 (0 = none), bits 16-31 = onBounce+1.
    std::uint32_t pad2;        // 156
    glm::vec3 fadeMax;         // 160
    // inheritVelocity float bits (event-template requests only; 0 otherwise).
    std::uint32_t pad3;        // 172
};
static_assert(sizeof(SpawnRequest) == 176, "SpawnRequest must match GLSL std430 layout (176 B)");

struct Emitter {
    enum class Shape { Point, Box, Sphere, Cone };

    Shape shape = Shape::Point;
    glm::vec3 position{0.f};
    glm::vec3 baseVelocity{0.f};   // preferred emission direction / speed
    glm::vec3 extents{1.f};        // Box half-extents
    glm::vec3 axis{0.f, 1.f, 0.f}; // Cone axis
    float radius = 1.f;            // Sphere radius / Cone base radius

    float rate = 100.f;            // particles per second
    float speedMin = 0.f, speedMax = 1.f;
    float spread = 0.f;            // 0 = exactly baseVelocity, 1 = uniform sphere
    float lifeMin = 1.f, lifeMax = 2.f;
    float sizeMin = 0.05f, sizeMax = 0.12f;
    glm::vec4 colorMin{1.f}, colorMax{1.f};
    std::vector<glm::vec4> palette; // if non-empty, overrides colorMin/Max (random pick)

    // ---- size <-> speed linking & cone emission ----
    float speedScale = 1.f;        // spawn-speed multiplier (set by setSizeScale / [emitter] speed_scale)
    float coneAngle = 0.f;         // degrees; >0 emits inside a cone around `axis` (overrides baseVelocity/spread)
    float speedSizeLink = 0.f;     // per-particle: speed *= 1 + link*(size/sizeMid - 1)  (0 = off)
    glm::vec3 fadeColorMin{0.f};   // RGB color particles fade INTO at death (stored in life.yzw)
    glm::vec3 fadeColorMax{0.f};
    bool hasFadeColor = false;     // true once fade_color_min/max are configured

    // System-maintained (GPU spawn): base index/count of this emitter's palette
    // inside the GPU palette buffer (-1 = no palette: uses colorMin/Max).
    int paletteIdx = -1;
    int paletteCount = 0;

    // Refractive particles: render as screen-space refraction (glass shards).
    // Sign-encoded into pos.w at spawn (zero new particle fields).
    bool refractive = false;

    bool active = true;

    // ---- events (sub-emission) ----
    // Template index triggered by this emitter's particles on death/bounce
    // (-1 = none). Only meaningful when registered via addEventEmitter.
    int onDeath = -1;
    int onBounce = -1;
    std::uint32_t eventCount = 8;  // children spawned per event, [1, 64]
    float inheritVelocity = 0.f;   // child velocity += parent velocity * this

    float accumulator = 0.f;       // fractional spawn carry-over

    // How many particles this emitter should emit this frame: rate*dt
    // accumulated, capped by `max`. Shared by the CPU path (spawn) and the
    // GPU path (request encoding in ParticleSystem::update).
    std::uint32_t takeCount(float dt, std::uint32_t max) {
        if (!std::isfinite(dt) || dt < 0.f || !std::isfinite(rate) || rate < 0.f)
            throw std::invalid_argument("ember: emission rate and dt must be finite and nonnegative");
        if (!std::isfinite(accumulator) || accumulator < 0.f) accumulator = 0.f;
        if (!active) {
            accumulator = 0.f; // re-enabling should not dump accumulated time
            return 0;
        }
        if (max == 0) return 0; // no budget this frame (e.g. bursts ate it): keep the carry
        // Cap the accumulator high enough that a long paused frame converts to
        // a single (budget-capped) burst on resume, without throttling high-rate
        // emitters. The old 1000 cap silently capped spawns at 1000/frame.
        accumulator = std::min((double)accumulator + (double)rate * dt, 1000000.0);
        std::uint32_t n = (std::uint32_t)accumulator;
        accumulator -= (float)n;
        if (n > max) n = max;
        return n;
    }

    // Emit up to `max` particles into `out`; returns how many were emitted.
    std::uint32_t spawn(std::vector<Particle>& out, std::uint32_t max, float dt, Rng& rng) {
        const std::uint32_t n = takeCount(dt, max);
        const float sizeMid = 0.5f * (sizeMin + sizeMax);
        for (std::uint32_t i = 0; i < n; ++i) {
            Particle p{};
            const float size = rng.range(sizeMin, sizeMax);
            p.pos = glm::vec4(samplePosition(rng), refractive ? -size : size);
            const glm::vec3 dir = sampleDirection(rng);
            float speed = rng.range(speedMin, speedMax) * speedScale;
            if (speedSizeLink > 0.f && sizeMid > 0.f)
                speed *= 1.f + speedSizeLink * (size / sizeMid - 1.f);
            p.vel = glm::vec4(dir * speed, 0.f); // age starts at 0
            p.life.x = rng.range(lifeMin, lifeMax);
            p.color = palette.empty()
                          ? rng.color(colorMin, colorMax)
                          : palette[(std::size_t)(rng.unit() * (float)palette.size()) % palette.size()];
            // Fade target color (RGB) goes into life.yzw; unset => identity (no color shift).
            if (hasFadeColor) {
                p.life.y = rng.range(fadeColorMin.r, fadeColorMax.r);
                p.life.z = rng.range(fadeColorMin.g, fadeColorMax.g);
                p.life.w = rng.range(fadeColorMin.b, fadeColorMax.b);
            } else {
                p.life.y = p.color.r;
                p.life.z = p.color.g;
                p.life.w = p.color.b;
            }
            out.push_back(p);
        }
        return n;
    }

    // Encode this emitter as a GPU spawn request producing `count` particles
    // with work ids [base, base+count). The GPU performs the actual sampling.
    SpawnRequest makeRequest(std::uint32_t base, std::uint32_t count) const {
        SpawnRequest r{};
        r.colorMin = colorMin;
        r.colorMax = colorMax;
        r.position = position;
        r.radius = radius;
        r.axis = glm::length(axis) > 1e-4f ? glm::normalize(axis) : glm::vec3(0.f);
        r.coneAngle = coneAngle;
        r.dir = glm::length(baseVelocity) > 1e-4f ? glm::normalize(baseVelocity) : glm::vec3(0.f);
        r.spread = spread;
        r.extents = extents;
        r.speedMin = speedMin * speedScale; // bake speedScale into the range
        r.speedMax = speedMax * speedScale;
        r.speedSizeLink = speedSizeLink;
        r.lifeMin = lifeMin;
        r.lifeMax = lifeMax;
        r.sizeMin = sizeMin;
        r.sizeMax = sizeMax;
        r.shape = (int)shape;
        r.paletteIdx = paletteIdx;
        r.paletteCount = paletteCount;
        r.flags = (hasFadeColor ? 1 : 0) | (refractive ? 2 : 0);
        r.base = base;
        r.count = count;
        r.fadeMin = fadeColorMin;
        r.fadeMax = fadeColorMax;
        const std::uint32_t death = (std::uint32_t)(onDeath + 1) & 0xFFFFu;
        const std::uint32_t bounce = (std::uint32_t)(onBounce + 1) & 0xFFFFu;
        r.pad2 = death | (bounce << 16);
        std::memcpy(&r.pad3, &inheritVelocity, sizeof(r.pad3));
        return r;
    }

    // ---- editing conveniences (used by the interactive editor) ----
    void moveBy(const glm::vec3& d) { position += d; }
    void scaleShape(float f) { radius *= f; extents *= f; } // by shape: sphere/cone use radius, box uses extents
    void setSizeRange(float a, float b) { sizeMin = a; sizeMax = b; }

private:
    glm::vec3 samplePosition(Rng& rng) const {
        switch (shape) {
            case Shape::Point: return position;
            case Shape::Box: return position + rng.box(-extents, extents);
            case Shape::Sphere: return position + rng.dirOnSphere() * rng.range(0.f, radius);
            case Shape::Cone: {
                glm::vec3 ax = glm::length(axis) > 1e-4f ? glm::normalize(axis) : glm::vec3(0, 1, 0);
                glm::vec3 u = glm::normalize(
                    glm::cross(ax, std::abs(ax.y) > 0.9f ? glm::vec3(1.f, 0.f, 0.f)
                                                         : glm::vec3(0.f, 1.f, 0.f)));
                glm::vec3 v = glm::cross(ax, u);
                const float r = radius * std::sqrt(rng.unit());
                const float a = rng.range(0.f, 6.283185307179586f);
                return position + (u * std::cos(a) + v * std::sin(a)) * r;
            }
        }
        return position;
    }

    glm::vec3 sampleDirection(Rng& rng) const {
        // Cone emission: uniform direction inside the cone around `axis`.
        if (coneAngle > 0.f && glm::length(axis) > 1e-4f) {
            const glm::vec3 ax = glm::length(axis) > 1e-4f ? glm::normalize(axis) : glm::vec3(0, 1, 0);
            glm::vec3 u = glm::normalize(
                glm::cross(ax, std::abs(ax.y) > 0.9f ? glm::vec3(1.f, 0.f, 0.f)
                                                      : glm::vec3(0.f, 1.f, 0.f)));
            glm::vec3 v = glm::cross(ax, u);
            const float cosA = std::cos(glm::radians(coneAngle));
            const float cosT = glm::mix(cosA, 1.f, rng.unit()); // cos-weighted on the cap
            const float sinT = std::sqrt(std::max(0.f, 1.f - cosT * cosT));
            const float phi = rng.range(0.f, 6.283185307179586f);
            return glm::normalize(ax * cosT + (u * std::cos(phi) + v * std::sin(phi)) * sinT);
        }
        const float bl = glm::length(baseVelocity);
        if (bl < 1e-4f) return rng.dirOnSphere();
        if (spread <= 0.01f) return glm::normalize(baseVelocity);
        // Mix the base direction toward a random sphere direction.
        return glm::normalize(glm::mix(glm::normalize(baseVelocity), rng.dirOnSphere(), spread));
    }
};

} // namespace ember
/* ============================================================================
 * [ 3] particle_types.hpp
 * particle_types.hpp
 * ========================================================================== */

// Backend-independent particle settings and per-call inputs. No graphics API headers.

namespace ember {
// Force identifiers. `enableForce(Force::X)` / `disableForce(Force::X)` flip the
// corresponding bit of the force mask (passed to the simulation as uForceMask).
// bit i <-> enum value i:
//   0 gravity  1 drag  2 wind  3 turbulence  4 attractors
//   5 vortex   6 spring 7 noise_wind 8 wave  9 boundary
enum class Force : std::uint32_t {
    Gravity = 0,
    Drag = 1,
    Wind = 2,
    Turbulence = 3,
    Attractors = 4,
    Vortex = 5,
    Spring = 6,
    NoiseWind = 7,
    Wave = 8,
    Boundary = 9,
};

struct RefractionParameters {
    bool enabled = false;
    int mode = 0;               // 0 = simple offset, 1 = depth-aware, 2 = noise (heat)
    float strength = 0.02f;     // simple/noise: UV offset; depth: projected displacement scale
    float ior = 1.5f;           // refractive index (mode 1)
    glm::vec3 tint{1.f};        // glass color absorption
    float absorption = 0.35f;   // mix(original, refracted, absorption)
    float fresnel = 2.f;        // rim brightening exponent (0 = off)
    float chroma = 0.f;         // chromatic dispersion (0 = off)
    float specular = 0.f;       // specular glint (0 = off)
    glm::vec3 lightDir{0.5f, 1.f, 0.3f}; // glint light direction (world space, normalized on set)
    int shape = 0;              // 0 = sprite/disc mask, 1 = procedural polygon shard (3-5 sides)
    float dome = 0.f;           // dome normal strength: silhouette rim + glint spots (0 = flat facet)
};

struct ParticleSettings {
    std::uint32_t capacity = 100000;
    std::uint32_t maxSpawnPerFrame = 65536;
    std::uint32_t seed = 0;
};
struct ParticleStatistics {
    std::uint64_t frame = 0;
    std::uint32_t alive = 0, allocated = 0;
};
// Lifecycle curves (WO-09): the facade bakes user keys into these 64-entry
// LUTs; the vertex shader samples them and multiplies. mask is CPU-only
// (bit0 = color, bit1 = size) and is the first 1280 B that get uploaded.
inline constexpr std::uint32_t kLifeCurveLutSize = 64;
struct LifeCurvesLut {
    glm::vec4 color[kLifeCurveLutSize];
    float     size[kLifeCurveLutSize];
    std::uint32_t mask = 0;
};
struct SimulationParameters {
    glm::vec3 gravity{0.f, -9.81f, 0.f};
    float drag = 0.f;
    glm::vec3 wind{0.f};
    float turbulence = 0.f;
    std::vector<Attractor> attractors;
    std::uint32_t forceMask = 0x1Fu; // bits 0..4 (existing forces) on by default
    DragMode dragMode = DragMode::Linear;
    std::vector<Vortex> vortexes;
    std::vector<Spring> springs;
    glm::vec3 noiseWindDir{1.f, 0.f, 0.f};
    float noiseWindAmp = 0.f;
    float noiseWindScale = 0.4f;
    float noiseWindSpeed = 0.8f;
    glm::vec3 waveDir{1.f, 0.f, 0.f};
    glm::vec3 waveK{1.f, 0.f, 0.f};
    float waveAmp = 0.f;
    float waveOmega = 1.f;
    BoundaryMode boundaryMode = BoundaryMode::Kill;
    float boundaryY = 0.f;
    float restitution = 0.5f;
};
struct RenderParameters {
    float sizeScale = 1.f;         // render size multiplier (uSizeScale)
    bool useSprite = true;
    int sheetCols = 1, sheetRows = 1;
    float streak = 0.f;
    float spin = 0.f;                  // billboard spin speed (rad/s; 0 = off)
    RefractionParameters refraction;     // refractive particles (glass)
    bool softParticles = false;
    float softRadius = 0.5f;
    float bloomThreshold = 1.f;
    BlendMode blend = BlendMode::Additive;
    bool depthTest = false;
    bool depthWrite = false;
};
struct UpdateBatch {
    const std::vector<SpawnRequest>& requests;
    std::uint32_t total;
    float dt, time;
    std::uint32_t seed;
};
struct RenderView {
    glm::mat4 view{1.f}, projection{1.f};
    float width = 0, height = 0, fovYDegrees = 60;
};
inline constexpr std::uint32_t maxSpawnRequests = 4096;
// Event sub-emission limits (fixed by design; overflow is dropped silently).
inline constexpr std::uint32_t maxEventTemplates = 64;
inline constexpr std::uint32_t maxEventInstancesPerFrame = 1024;
inline constexpr std::uint32_t maxEventChildrenPerFrame = 4096;
} // namespace ember
/* ============================================================================
 * [ 4] backend.hpp
 * backend.hpp
 * ========================================================================== */

// Particle-level backend contract. Native devices, images, command buffers and
// synchronization objects belong to backend-specific adapters, never this API.
#include <memory>
#include <string>

namespace ember {
struct BackendCapabilities {
    // Implementation support, not a promise that every device/shader/config
    // accepts the feature. Enabling still validates backend-specific limits.
    bool gpuScheduling = false;
    bool sorting = false;
    bool bloom = false;
    bool refraction = false;
    bool softParticles = false;
    bool spriteTextures = false;
    bool events = false; // GPU death/bounce sub-emission
    bool lifeCurves = false; // color-over-life / size-over-life LUTs
};
class ParticleBackend {
public:
    virtual ~ParticleBackend() = default;
    virtual const char* name() const noexcept = 0;
    virtual BackendCapabilities capabilities() const noexcept = 0;
    // One owner, externally serialized calls. initialize is called exactly once
    // by ParticleSystem; destruction must retire the backend's outstanding work.
    virtual void initialize(std::uint32_t capacity, bool debug) = 0;
    // Validate limits before config application; no particle/option mutations.
    virtual void validateConfiguration(std::uint32_t capacity, bool sorting) = 0;
    // Both discard particles and invalidate pending statistics, preserving the
    // update sequence and options. resize also changes storage capacity.
    virtual void resize(std::uint32_t capacity) = 0;
    virtual void clear() = 0;
    virtual void setDebug(bool enabled) = 0;
    virtual void uploadAttractors(const std::vector<Attractor>& values) = 0;
    virtual void uploadVortexes(const std::vector<Vortex>& values) = 0;
    virtual void uploadSprings(const std::vector<Spring>& values) = 0;
    virtual void uploadPalette(const std::vector<glm::vec4>& values) = 0;
    // Borrowed inputs are valid only during the call. Copy/upload before return.
    // Successful update advances sequence once, including dt=0 / empty frames.
    // Calls observe earlier calls in order. Completion may be asynchronous;
    // resource retirement and visibility barriers are the backend's job.
    virtual void update(const SimulationParameters&, const UpdateBatch&) = 0;
    virtual void render(const RenderParameters&, const RenderView&) = 0;
    virtual ParticleStatistics pollStatistics() = 0; // never wait on unfinished GPU work
    virtual ParticleStatistics synchronizeStatistics() const = 0; // exact, may wait
    virtual std::vector<Particle> readParticles(std::uint32_t max) const = 0;
    virtual std::uint64_t updateSequence() const = 0;
    virtual std::uint64_t droppedStatistics() const = 0;
    virtual bool gpuDriven() const { return false; }
    virtual void setGpuDriven(bool on) { if(on) unsupported("GPU scheduling"); }
    virtual bool sortEnabled() const { return false; }
    virtual void setSortEnabled(bool on) { if(on) unsupported("sorting"); }
    virtual bool bloom() const { return false; }
    virtual void setBloom(bool on) { if(on) unsupported("bloom"); }
    virtual void setSpriteTexture(const char*) { unsupported("sprite loading"); }
    virtual void setShaderDirectory(const char*) { unsupported("shader directory"); }
    // Event templates encoded as SpawnRequests (base/count filled, position
    // ignored). Default: any non-empty list is rejected; an empty list only
    // asks the backend to release its resources.
    virtual void uploadEventTemplates(const std::vector<SpawnRequest>& templates) {
        if (!templates.empty()) unsupported("event emission");
    }
    // Baked 64-entry lifecycle-curve LUTs (mask != 0 enables a channel).
    virtual void uploadLifeCurves(const LifeCurvesLut& lut) {
        if (lut.mask != 0) unsupported("life curves");
    }
protected:
    [[noreturn]] static void unsupported(const char* feature) {
        throw std::invalid_argument(std::string("ember: backend does not support ")+feature);
    }
};
} // namespace ember
/* ============================================================================
 * [ 5] system.hpp
 * system.hpp
 * ========================================================================== */

// Backend-independent ParticleSystem facade. Legacy OpenGL users may continue
// including ember/particle_system.hpp for the old transitive GL types.

namespace ember {
struct Config;

class ParticleSystem {
public:
    using Settings = ParticleSettings;

    ParticleSystem() : ParticleSystem(Settings{}) {}
    explicit ParticleSystem(const Settings& s); // default OpenGL adapter
    ParticleSystem(const Settings& s, std::unique_ptr<ParticleBackend> backend);
    const char* backendName() const { return backend_->name(); }
    // Backend-specific adapter functions (ember/opengl.hpp, future backends)
    // operate on the owned backend through this reference.
    ParticleBackend& backend() { return *backend_; }
    BackendCapabilities backendCapabilities() const { return backend_->capabilities(); }
    ~ParticleSystem();

    ParticleSystem(ParticleSystem&&) noexcept;
    ParticleSystem& operator=(ParticleSystem&&) noexcept;
    // Moved-from systems may only be destroyed or assigned a new system.
    ParticleSystem(const ParticleSystem&) = delete;
    ParticleSystem& operator=(const ParticleSystem&) = delete;

    // ---- configuration (programmatic API; Config::apply goes through these) ----
    void setGravity(glm::vec3 g);
    void setDrag(float k);
    void setWind(glm::vec3 w);
    void setTurbulence(float k);
    void setAttractors(const std::vector<Attractor>& a);
    void setBlendMode(BlendMode m);
    void setDepthTest(bool on);
    void setDepthWrite(bool on);
    void setMaxSpawnPerFrame(std::uint32_t n);

    // ---- force enable/disable (bitmask; default: existing forces on, new ones off) ----
    void setForceEnabled(Force f, bool on);
    void enableForce(Force f) { setForceEnabled(f, true); }
    void disableForce(Force f) { setForceEnabled(f, false); }
    bool forceEnabled(Force f) const {
        const auto bit = static_cast<std::uint32_t>(f);
        return bit <= static_cast<std::uint32_t>(Force::Boundary) && (simulation_.forceMask & (1u << bit)) != 0;
    }
    void setForceMask(std::uint32_t mask);
    std::uint32_t forceMask() const { return simulation_.forceMask; }

    // ---- new force fields ----
    void setDragMode(DragMode m);                                  // linear | quadratic
    void setVortexes(const std::vector<Vortex>& v);                // auto-enables/disables Force::Vortex
    void setSprings(const std::vector<Spring>& s);                 // auto-enables/disables Force::Spring
    void setNoiseWind(glm::vec3 dir, float amplitude, float scale, float speed); // auto-toggle
    void setWave(glm::vec3 dir, glm::vec3 waveVector, float amplitude, float omega); // auto-toggle
    void setBoundary(BoundaryMode mode, float planeY, float restitution = 0.5f); // enables Force::Boundary

    // ---- particle size / sprite ----
    // linkSpeed=true (default) scales every emitter's spawn speed by the same
    // factor, keeping trajectories proportional to the new size.
    void setSizeScale(float s, bool linkSpeed = true);
    float sizeScale() const { return rendering_.sizeScale; }
    void setUseSprite(bool on);                    // texture vs procedural glow
    bool useSprite() const { return rendering_.useSprite; }
    void setSpriteTexture(const char* pngPath);    // "" or failure => built-in gradient
    void setSpriteSheet(int cols, int rows);       // (1,1) = single frame

    // ---- rendering extras ----
    void setStreak(float k);                       // >0: centered projected-motion stretch, not a history trail
    void setSpin(float speed = 1.f);               // signed radians/sec; zero disables normal-particle spin
    void setRefractionParameters(const RefractionParameters& s);
    void setSoftParticleParameters(bool on, float radius = 0.5f); // radius must be finite and > 0
    void setSortEnabled(bool on);                  // back-to-front depth sort
    bool sortEnabled() const { return backend_->sortEnabled(); }
    void setBloom(bool on);
    bool bloom() const { return backend_->bloom(); }
    void setBloomThreshold(float t);               // bright-pass cutoff (default 1.0)

    // ---- lifecycle curves (WO-09) ------------------------------------------
    // keys are uniformly spaced (t_i = i/(n-1), n >= 2; n == 1 is constant);
    // an empty vector disables the channel (renders exactly as before). The
    // facade bakes 64-entry LUTs and uploads them to the backend.
    void setColorOverLife(std::vector<glm::vec4> keys);
    void setSizeOverLife(std::vector<float> keys);
    const std::vector<glm::vec4>& colorOverLifeKeys() const { return colorOverLifeKeys_; }
    const std::vector<float>& sizeOverLifeKeys() const { return sizeOverLifeKeys_; }

    // ---- shader sourcing ----
    // Backend-specific, unsupported backends throw. OpenGL loads
    // particle.vert/.frag/simulate.comp from `dir` (default
    // "shaders"); fall back to the embedded copies when the files are missing
    // or fail to compile. Custom programs set via setOpenGLPrograms() always win.
    void setShaderDirectory(const char* dir);

    // ---- configuration from an INI-style file (see config/example.ini) --------
    // Applies [system] + [palette] + [emitter ...] + [attractor ...] sections.
    // Throws std::runtime_error on parse or apply errors.
    void loadConfig(const char* path);
    void apply(const Config& cfg);

    // ---- emitters ------------------------------------------------------------------
    Emitter& addEmitter(const Emitter& e = Emitter{});
    void removeEmitter(std::size_t index);
    void clearEmitters();
    std::size_t emitterCount() const { return emitters_.size(); }
    // Direct access to a configured emitter (e.g. to animate its position).
    // Returns nullptr if index is out of range.
    Emitter* emitter(std::size_t index) {
        return index < emitters_.size() ? &emitters_[index] : nullptr;
    }

    // ---- event templates (sub-emission) ------------------------------------
    // Templates are copied by value; later edits to the source do not affect
    // the registered copy. Returns the template index used by onDeath/onBounce.
    std::size_t addEventEmitter(const Emitter& e);
    std::size_t eventEmitterCount() const { return eventEmitters_.size(); }
    const Emitter* eventEmitter(std::size_t index) const {
        return index < eventEmitters_.size() ? &eventEmitters_[index] : nullptr;
    }
    // Clears all templates and detaches every emitter (onDeath/onBounce = -1).
    void clearEventEmitters();

    // ---- simulation ------------------------------------------------------------------
    void update(float dt);           // run the GPU sim (spawn + integrate + recycle)
    void burst(const BurstParams& p); // instant spawn, applied on next update()
    void clear();                    // remove all particles

    // Optional GPU scheduling. OpenGL defaults to synchronous scheduling and
    // rejects custom shaders that lack its GPU scheduling protocol.
    void setGpuDriven(bool enabled);
    bool gpuDriven() const { return backend_->gpuDriven(); }
    using Statistics = ParticleStatistics;
    // May return an older frame (initially zero). OpenGL uses four staging slots
    // and requires the owning context current; full slots skip a sample.
    Statistics pollStatistics();       // newest ready snapshot; never waits for an unfinished fence
    Statistics synchronizeStatistics() const; // explicit blocking, exact current counts
    std::uint64_t updateSequence() const { return backend_->updateSequence(); }
    std::uint64_t droppedStatistics() const { return backend_->droppedStatistics(); } // cumulative skipped samples
    // Exact in both modes. GPU-driven callers should use pollStatistics for UI.
    std::uint32_t aliveCount() const;
    std::uint32_t capacity() const { return capacity_; }
    std::uint32_t maxSpawnPerFrame() const { return maxSpawnPerFrame_; }

    // Debug/tooling: copy up to `max` live particles back to the CPU (slot
    // order, NOT spawn order). `max` = 0 copies all alive. This is a GPU->CPU
    // readback — for tests / snapshots / tooling, not the render path.
    std::vector<Particle> readParticles(std::uint32_t max = 0) const;

    // ---- rendering ----------------------------------------------------------------------
    // view/proj: camera matrices; viewport width/height in pixels; fovYDeg in degrees.
    void render(const glm::mat4& view, const glm::mat4& proj,
                float viewportWidth, float viewportHeight, float fovYDeg);

private:
    void registerEmitterPalette(Emitter&);
    void synchronizePalettes();
    void rebuildPaletteBuffer();
    void synchronizeEventTemplates();
    void uploadLifeCurves();
    std::unique_ptr<ParticleBackend> backend_;
    std::uint32_t capacity_ = 0, maxSpawnPerFrame_ = 0;
    float time_ = 0.f;
    std::uint32_t frameSeed_ = 1;
    bool debug_ = false;
    SimulationParameters simulation_;
    RenderParameters rendering_;
    float speedScaleBase_ = 1.f;
    std::vector<Emitter> emitters_;
    std::vector<Emitter> eventEmitters_;
    bool eventTemplatesDirty_ = false;
    std::vector<glm::vec4> colorOverLifeKeys_;
    std::vector<float> sizeOverLifeKeys_;
    std::vector<SpawnRequest> pendingRequests_;
    std::vector<glm::vec4> paletteData_;
};
} // namespace ember
/* ============================================================================
 * [ 6] config.hpp
 * config.hpp
 * ========================================================================== */

// INI-style configuration with a self-contained parser (no third-party JSON).
//
// Users can edit gravity, wind, turbulence, emitters, palettes and attractors
// without touching code; ParticleSystem::loadConfig() / apply() consume this.
//
// Format (see config/example.ini):
//   # comment
//   [system]                 gravity = 0, -7, 0        drag = 0.05   ...
//   [palette "fire"]         colors = 1,0.9,0.4,1 | 1,0.15,0.05,1
//   [emitter "fountain"]     shape = cone    position = 0, 0, 0   ...
//   [attractor]              position = 0, 0.6, 0    strength = 60
//
// Errors throw std::runtime_error with a line number.

#include <cctype>
#include <limits>
#include <fstream>
#include <sstream>

namespace ember {

struct Palette {
    std::string name;
    std::vector<glm::vec4> colors;
};

// Mirrors Emitter + configuration metadata (section name / referenced palette).
// Explicit-time lifecycle-curve key (WO-09): t in [0,1], value = rgba (color)
// or .x = size. The facade resamples these to uniform 64-entry LUTs.
struct CurveKey {
    float t = 0.f;
    glm::vec4 value{1.f};
};

struct EmitterConfig : public Emitter {
    std::string name;
    std::string paletteName;  // optional: [palette "..."] to take colors from
    std::string onDeathName;  // [emitter]/[event] on_death = <event name>
    std::string onBounceName; // [emitter]/[event] on_bounce = <event name>
};

struct Config {
    struct System {
        glm::vec3 gravity{0.f, -9.81f, 0.f};
        float drag = 0.f;
        glm::vec3 wind{0.f};
        float turbulence = 0.f;
        std::string blend = "additive";   // additive | normal
        std::uint32_t capacity = 0;       // 0 = keep current
        std::uint32_t maxSpawnPerFrame = 0;

        // ---- force switches & new force fields ----
        std::vector<std::string> forces;  // enabled forces; empty = auto (setters decide)
        std::string dragMode = "linear";  // linear | quadratic
        glm::vec3 noiseWindDir{1.f, 0.f, 0.f};
        float noiseWindAmp = 0.f;         // 0 = off
        float noiseWindScale = 0.4f;
        float noiseWindSpeed = 0.8f;
        glm::vec3 waveDir{1.f, 0.f, 0.f};
        glm::vec3 waveK{1.f, 0.f, 0.f};   // wave vector (radians/unit)
        float waveAmp = 0.f;              // 0 = off
        float waveOmega = 1.f;
        std::string boundaryMode = "none"; // none | kill | bounce
        float boundaryY = 0.f;
        float restitution = 0.5f;
        bool debug = false;               // per-frame diagnostics to stderr

        // ---- size / texture / editor ----
        float sizeScale = 1.f;            // global particle size multiplier
        bool scaleSpeedWithSize = true;   // keep spawn speed proportional to size_scale
        std::string texture;              // optional sprite PNG path ("" = built-in gradient)
        bool editor = true;               // examples: interactive emitter editor (runtime toggle)

        // ---- rendering extras ----
        float streak = 0.f;               // >0: centered stretch along projected particle motion
        bool softParticles = false;       // fade particles near host scene depth
        float softRadius = 0.5f;
        bool bloom = false;               // additive HDR bloom post-process
        float bloomThreshold = 1.f;
        bool sort = false;                // GPU depth sort (OIT: correct alpha blending)
        float spin = 0.f;                 // billboard spin speed in rad/s (0 = off)
        bool refraction = false;          // refractive particles (glass; host supplies scene texture)
        std::string refractionMode = "simple"; // simple | depth | noise
        float refractionStrength = 0.02f; // refraction offset strength
    };
    System system;
    std::vector<Palette> palettes;
    std::vector<EmitterConfig> emitters;
    std::vector<EmitterConfig> events;
    std::vector<CurveKey> colorKeys; // [curves] color: value = rgba
    std::vector<CurveKey> sizeKeys;  // [curves] size:  value.x = size
    std::vector<Attractor> attractors;
    std::vector<Vortex> vortexes;
    std::vector<Spring> springs;

    // Keys/sections that actually appeared in the parsed text (e.g.
    // "system.gravity", "vortex", "emitter"). Lets apply() keep programmatic
    // state for anything the config does not mention.
    std::vector<std::string> specified;

    bool has(const char* key) const {
        return std::find(specified.begin(), specified.end(), key) != specified.end();
    }

    const Palette* findPalette(const char* name) const {
        for (const auto& p : palettes)
            if (p.name == name) return &p;
        return nullptr;
    }

    // Event template index by [event] name, or -1 when absent.
    int findEvent(const char* name) const {
        for (std::size_t i = 0; i < events.size(); ++i)
            if (events[i].name == name) return (int)i;
        return -1;
    }

    static Config fromFile(const char* path) {
        std::ifstream f(path);
        if (!f) throw std::runtime_error(std::string("ember config: cannot open '") + path + "'");
        std::ostringstream ss;
        char buffer[4096];
        while (f.read(buffer, sizeof(buffer))) ss.write(buffer, f.gcount());
        ss.write(buffer, f.gcount());
        if (f.bad() || !f.eof() || !ss)
            throw std::runtime_error(std::string("ember config: read failed: ") + path);
        return fromString(ss.str());
    }

    static Config fromString(const std::string& text) {
        Config cfg;
        std::istringstream in(text);
        std::string line;
        int lineNo = 0;

        enum class Section { None, System, Palette, Emitter, Event, Curves, Attractor, Vortex, Spring };
        Section section = Section::None;
        Palette* pal = nullptr;
        EmitterConfig* em = nullptr;

        auto fail = [&](const std::string& msg) -> void {
            throw std::runtime_error("ember config: " + msg + " (line " + std::to_string(lineNo) + ")");
        };

        while (std::getline(in, line)) {
            ++lineNo;
            // Strip a UTF-8 BOM (common when files are saved by Windows Notepad).
            if (lineNo == 1 && line.size() >= 3 && (unsigned char)line[0] == 0xEF &&
                (unsigned char)line[1] == 0xBB && (unsigned char)line[2] == 0xBF)
                line.erase(0, 3);
            line = trim(line);
            if (line.empty() || line[0] == '#') continue;

            if (line[0] == '[') { // new section
                if (line.back() != ']') fail("malformed section header '" + line + "'");
                const std::string body = trim(line.substr(1, line.size() - 2));
                const std::string lower = toLower(body);
                const std::string sec = lower.substr(0, lower.find_first_of(" \t")); // section keyword
                if (sec == "system") {
                    section = Section::System;
                } else if (sec == "curves") {
                    section = Section::Curves;
                } else if (sec == "attractor") {
                    cfg.attractors.push_back(Attractor{});
                    cfg.specified.push_back("attractor");
                    section = Section::Attractor;
                } else if (sec == "vortex" || sec == "spring") {
                    if (sec == "vortex") {
                        cfg.vortexes.push_back(Vortex{});
                        cfg.specified.push_back("vortex");
                        section = Section::Vortex;
                    } else {
                        cfg.springs.push_back(Spring{});
                        cfg.specified.push_back("spring");
                        section = Section::Spring;
                    }
                } else if (sec == "palette" || sec == "emitter" || sec == "event") {
                    std::string arg;
                    try { arg = sectionArg(body); }
                    catch (const std::invalid_argument&) { fail("malformed named section: " + body); }
                    if (sec == "palette") {
                        for (auto& p : cfg.palettes)
                            if (p.name == arg) fail("duplicate palette '" + arg + "'");
                        cfg.palettes.push_back(Palette{arg, {}});
                        pal = &cfg.palettes.back();
                        section = Section::Palette;
                    } else if (sec == "event") {
                        cfg.events.push_back(EmitterConfig{});
                        cfg.specified.push_back("event");
                        em = &cfg.events.back();
                        em->name = arg;
                        section = Section::Event;
                    } else {
                        cfg.emitters.push_back(EmitterConfig{});
                        cfg.specified.push_back("emitter");
                        em = &cfg.emitters.back();
                        em->name = arg;
                        section = Section::Emitter;
                    }
                } else {
                    fail("unknown section '" + body + "'");
                }
                continue;
            }

            const std::size_t eq = line.find('=');
            if (eq == std::string::npos) fail("expected 'key = value', got '" + line + "'");
            const std::string key = toLower(trim(line.substr(0, eq)));
            const std::string value = trim(line.substr(eq + 1));
            // Explicitly empty values are allowed for texture (built-in sprite)
            // and the curves channels (disable the curve).
            const bool emptyAllowed = (section == Section::System && key == "texture") ||
                                      (section == Section::Curves && (key == "size" || key == "color"));
            if (key.empty() || (value.empty() && !emptyAllowed))
                fail("empty key or value in '" + line + "'");

            // Record key presence so apply() can keep untouched state.
            const char* secName = section == Section::System ? "system"
                : section == Section::Palette ? "palette"
                : section == Section::Emitter ? "emitter"
                : section == Section::Event ? "event"
                : section == Section::Curves ? "curves"
                : section == Section::Attractor ? "attractor"
                : section == Section::Vortex ? "vortex"
                : section == Section::Spring ? "spring"
                : "none";
            if (section != Section::None) cfg.specified.push_back(std::string(secName) + "." + key);

            switch (section) {
                case Section::System: {
                    if (key == "gravity") cfg.system.gravity = parseVec3(value, lineNo);
                    else if (key == "drag") cfg.system.drag = parseFloat(value, lineNo);
                    else if (key == "wind") cfg.system.wind = parseVec3(value, lineNo);
                    else if (key == "turbulence") cfg.system.turbulence = parseFloat(value, lineNo);
                    else if (key == "blend") cfg.system.blend = toLower(value);
                    else if (key == "capacity") cfg.system.capacity = parseUInt(value, lineNo);
                    else if (key == "max_spawn_per_frame") cfg.system.maxSpawnPerFrame = parseUInt(value, lineNo);
                    else if (key == "forces") {
                        for (auto& tok : split(value, ',')) {
                            const std::string f = toLower(trim(tok));
                            if (!f.empty()) cfg.system.forces.push_back(f);
                        }
                    }
                    else if (key == "drag_mode") cfg.system.dragMode = toLower(value);
                    else if (key == "noise_wind_direction") cfg.system.noiseWindDir = parseVec3(value, lineNo);
                    else if (key == "noise_wind_amplitude") cfg.system.noiseWindAmp = parseFloat(value, lineNo);
                    else if (key == "noise_wind_scale") cfg.system.noiseWindScale = parseFloat(value, lineNo);
                    else if (key == "noise_wind_speed") cfg.system.noiseWindSpeed = parseFloat(value, lineNo);
                    else if (key == "wave_direction") cfg.system.waveDir = parseVec3(value, lineNo);
                    else if (key == "wave_vector") cfg.system.waveK = parseVec3(value, lineNo);
                    else if (key == "wave_amplitude") cfg.system.waveAmp = parseFloat(value, lineNo);
                    else if (key == "wave_omega") cfg.system.waveOmega = parseFloat(value, lineNo);
                    else if (key == "boundary_mode") cfg.system.boundaryMode = toLower(value);
                    else if (key == "boundary_y") cfg.system.boundaryY = parseFloat(value, lineNo);
                    else if (key == "restitution") cfg.system.restitution = parseFloat(value, lineNo);
                    else if (key == "debug") cfg.system.debug = parseBool(value, lineNo);
                    else if (key == "size_scale") cfg.system.sizeScale = parseFloat(value, lineNo);
                    else if (key == "scale_speed_with_size") cfg.system.scaleSpeedWithSize = parseBool(value, lineNo);
                    else if (key == "texture") cfg.system.texture = value;
                    else if (key == "editor") cfg.system.editor = parseBool(value, lineNo);
                    else if (key == "streak") cfg.system.streak = parseFloat(value, lineNo);
                    else if (key == "soft_particles") cfg.system.softParticles = parseBool(value, lineNo);
                    else if (key == "soft_radius") cfg.system.softRadius = parseFloat(value, lineNo);
                    else if (key == "bloom") cfg.system.bloom = parseBool(value, lineNo);
                    else if (key == "bloom_threshold") cfg.system.bloomThreshold = parseFloat(value, lineNo);
                    else if (key == "sort") cfg.system.sort = parseBool(value, lineNo);
                    else if (key == "spin") cfg.system.spin = parseFloat(value, lineNo);
                    else if (key == "refraction") cfg.system.refraction = parseBool(value, lineNo);
                    else if (key == "refraction_mode") {
                        cfg.system.refractionMode = toLower(value);
                        if (cfg.system.refractionMode != "simple" && cfg.system.refractionMode != "depth" && cfg.system.refractionMode != "noise")
                            fail("unknown refraction_mode: " + value);
                    }
                    else if (key == "refraction_strength") cfg.system.refractionStrength = parseFloat(value, lineNo);
                    else fail("unknown key '" + key + "' in [system]");
                    break;
                }
                case Section::Palette: {
                    if (key == "colors") {
                        for (auto& tok : split(value, '|'))
                            pal->colors.push_back(parseVec4(trim(tok), lineNo));
                        if (pal->colors.empty()) fail("palette '" + pal->name + "' has no colors");
                    } else fail("unknown key '" + key + "' in [palette]");
                    break;
                }
                case Section::Emitter:
                case Section::Event: {
                    if (key == "shape") em->shape = parseShape(value, lineNo);
                    else if (key == "position") em->position = parseVec3(value, lineNo);
                    else if (key == "base_velocity") em->baseVelocity = parseVec3(value, lineNo);
                    else if (key == "extents") em->extents = parseVec3(value, lineNo);
                    else if (key == "axis") em->axis = parseVec3(value, lineNo);
                    else if (key == "radius") em->radius = parseFloat(value, lineNo);
                    else if (key == "rate") em->rate = parseFloat(value, lineNo);
                    else if (key == "speed_min") em->speedMin = parseFloat(value, lineNo);
                    else if (key == "speed_max") em->speedMax = parseFloat(value, lineNo);
                    else if (key == "spread") em->spread = parseFloat(value, lineNo);
                    else if (key == "life_min") em->lifeMin = parseFloat(value, lineNo);
                    else if (key == "life_max") em->lifeMax = parseFloat(value, lineNo);
                    else if (key == "size_min") em->sizeMin = parseFloat(value, lineNo);
                    else if (key == "size_max") em->sizeMax = parseFloat(value, lineNo);
                    else if (key == "color_min") em->colorMin = parseVec4(value, lineNo);
                    else if (key == "color_max") em->colorMax = parseVec4(value, lineNo);
                    else if (key == "palette") em->paletteName = value;
                    else if (key == "active") em->active = parseBool(value, lineNo);
                    else if (key == "refractive") em->refractive = parseBool(value, lineNo);
                    else if (key == "cone_angle") em->coneAngle = parseFloat(value, lineNo);
                    else if (key == "speed_size_link") em->speedSizeLink = parseFloat(value, lineNo);
                    else if (key == "speed_scale") em->speedScale = parseFloat(value, lineNo);
                    else if (key == "fade_color_min") {
                        em->fadeColorMin = parseVec3(value, lineNo);
                        em->hasFadeColor = true;
                    }
                    else if (key == "fade_color_max") {
                        em->fadeColorMax = parseVec3(value, lineNo);
                        em->hasFadeColor = true;
                    }
                    else if (key == "on_death") em->onDeathName = value;
                    else if (key == "on_bounce") em->onBounceName = value;
                    else if (key == "count") {
                        if (section != Section::Event) fail("unknown key '" + key + "' in [emitter]");
                        em->eventCount = parseUInt(value, lineNo);
                    }
                    else if (key == "inherit") {
                        if (section != Section::Event) fail("unknown key '" + key + "' in [emitter]");
                        em->inheritVelocity = parseFloat(value, lineNo);
                    }
                    else fail("unknown key '" + key + "' in [" +
                              std::string(section == Section::Event ? "event" : "emitter") + "]");
                    break;
                }
                case Section::Curves: {
                    if (value.empty()) { // explicit empty = close the channel
                        if (key == "color") cfg.colorKeys.clear();
                        else if (key == "size") cfg.sizeKeys.clear();
                        else fail("unknown key '" + key + "' in [curves]");
                    } else if (key == "color") {
                        cfg.colorKeys = parseCurveKeys(value, lineNo, 4);
                    } else if (key == "size") {
                        cfg.sizeKeys = parseCurveKeys(value, lineNo, 1);
                    } else fail("unknown key '" + key + "' in [curves]");
                    break;
                }
                case Section::Attractor: {
                    if (key == "position") cfg.attractors.back().position = parseVec3(value, lineNo);
                    else if (key == "strength") cfg.attractors.back().strength = parseFloat(value, lineNo);
                    else fail("unknown key '" + key + "' in [attractor]");
                    break;
                }
                case Section::Vortex: {
                    if (key == "position")
                        cfg.vortexes.back().center =
                            glm::vec4(parseVec3(value, lineNo), cfg.vortexes.back().center.w);
                    else if (key == "radius") cfg.vortexes.back().center.w = parseFloat(value, lineNo);
                    else if (key == "axis")
                        cfg.vortexes.back().axisStrength =
                            glm::vec4(parseVec3(value, lineNo), cfg.vortexes.back().axisStrength.w);
                    else if (key == "strength") cfg.vortexes.back().axisStrength.w = parseFloat(value, lineNo);
                    else fail("unknown key '" + key + "' in [vortex]");
                    break;
                }
                case Section::Spring: {
                    if (key == "position") cfg.springs.back().anchor = parseVec3(value, lineNo);
                    else if (key == "stiffness") cfg.springs.back().stiffness = parseFloat(value, lineNo);
                    else if (key == "damping") cfg.springs.back().damping = parseFloat(value, lineNo);
                    else fail("unknown key '" + key + "' in [spring]");
                    break;
                }
                case Section::None:
                    fail("key '" + key + "' outside any section");
            }
        }
        return cfg;
    }

private:
    static std::string trim(const std::string& s) {
        std::size_t b = 0, e = s.size();
        while (b < e && std::isspace((unsigned char)s[b])) ++b;
        while (e > b && std::isspace((unsigned char)s[e - 1])) --e;
        return s.substr(b, e - b);
    }
    static std::string toLower(std::string s) {
        for (auto& c : s) c = (char)std::tolower((unsigned char)c);
        return s;
    }
    static std::vector<std::string> split(const std::string& s, char sep) {
        std::vector<std::string> out;
        std::string cur;
        for (char c : s) {
            if (c == sep) { out.push_back(cur); cur.clear(); }
            else cur += c;
        }
        out.push_back(cur);
        return out;
    }
    // "[section \"arg\"]" -> "arg"; "[section]" -> section name
    static std::string sectionArg(const std::string& body) {
        const auto split = body.find_first_of(" \t");
        if (split == std::string::npos) return body; // unnamed emitter
        const std::string arg = trim(body.substr(split));
        if (arg.size() < 2 || arg.front() != '"' || arg.back() != '"' ||
            arg.find('"', 1) != arg.size() - 1)
            throw std::invalid_argument("malformed section");
        return arg.substr(1, arg.size() - 2);
    }
    static float parseFloat(const std::string& v, int line) {
        try {
            std::size_t pos = 0;
            const float f = std::stof(v, &pos);
            if (pos != v.size() || !std::isfinite(f)) throw std::invalid_argument("invalid number");
            return f;
        } catch (...) {
            throw std::runtime_error("ember config: bad number '" + v + "' (line " + std::to_string(line) + ")");
        }
    }
    static std::uint32_t parseUInt(const std::string& v, int line) {
        try {
            std::size_t pos = 0;
            if (v.empty() || v.front() == '-') throw std::invalid_argument("negative");
            const unsigned long long u = std::stoull(v, &pos);
            if (pos != v.size() || u > std::numeric_limits<std::uint32_t>::max()) throw std::invalid_argument("trailing");
            return (std::uint32_t)u;
        } catch (...) {
            throw std::runtime_error("ember config: bad integer '" + v + "' (line " + std::to_string(line) + ")");
        }
    }
    static bool parseBool(const std::string& v, int line) {
        const std::string l = toLower(v);
        if (l == "true" || l == "1" || l == "yes" || l == "on") return true;
        if (l == "false" || l == "0" || l == "no" || l == "off") return false;
        throw std::runtime_error("ember config: bad bool '" + v + "' (line " + std::to_string(line) + ")");
    }
    static glm::vec3 parseVec3(const std::string& v, int line) {
        const auto t = split(v, ',');
        if (t.size() != 3) throw std::runtime_error("ember config: expected 3 numbers (line " + std::to_string(line) + ")");
        return glm::vec3(parseFloat(trim(t[0]), line), parseFloat(trim(t[1]), line), parseFloat(trim(t[2]), line));
    }
    static glm::vec4 parseVec4(const std::string& v, int line) {
        const auto t = split(v, ',');
        if (t.size() != 4) throw std::runtime_error("ember config: expected 4 numbers (line " + std::to_string(line) + ")");
        return glm::vec4(parseFloat(trim(t[0]), line), parseFloat(trim(t[1]), line),
                         parseFloat(trim(t[2]), line), parseFloat(trim(t[3]), line));
    }
    // Comma-separated "t:c0,c1,..." keys. Each key carries exactly `components`
    // values; t must lie in [0,1] and strictly increase.
    static std::vector<CurveKey> parseCurveKeys(const std::string& v, int line, int components) {
        std::vector<CurveKey> out;
        const auto toks = split(v, ',');
        std::size_t i = 0;
        while (i < toks.size()) {
            const std::string tok = trim(toks[i]);
            const std::size_t colon = tok.find(':');
            if (colon == std::string::npos)
                throw std::runtime_error("ember config: curves key must be 't:value' (line " + std::to_string(line) + ")");
            CurveKey k;
            k.t = parseFloat(tok.substr(0, colon), line);
            if (k.t < 0.f || k.t > 1.f)
                throw std::runtime_error("ember config: curves t must be in [0,1] (line " + std::to_string(line) + ")");
            if (!out.empty() && k.t <= out.back().t)
                throw std::runtime_error("ember config: curves t must strictly increase (line " + std::to_string(line) + ")");
            std::vector<float> vals;
            vals.push_back(parseFloat(tok.substr(colon + 1), line));
            ++i;
            while (i < toks.size() && toks[i].find(':') == std::string::npos) {
                vals.push_back(parseFloat(trim(toks[i]), line));
                ++i;
            }
            if ((int)vals.size() != components)
                throw std::runtime_error("ember config: curves key needs " + std::to_string(components) +
                                         " component(s) (line " + std::to_string(line) + ")");
            for (int c = 0; c < components; ++c) k.value[c] = vals[c];
            out.push_back(k);
        }
        if (out.empty())
            throw std::runtime_error("ember config: empty curves list (line " + std::to_string(line) + ")");
        return out;
    }
    static Emitter::Shape parseShape(const std::string& v, int line) {
        const std::string l = toLower(v);
        if (l == "point") return Emitter::Shape::Point;
        if (l == "box") return Emitter::Shape::Box;
        if (l == "sphere") return Emitter::Shape::Sphere;
        if (l == "cone") return Emitter::Shape::Cone;
        throw std::runtime_error("ember config: unknown shape '" + v + "' (line " + std::to_string(line) + ")");
    }
};

} // namespace ember
/* ============================================================================
 * [ 7] backends/opengl/gl.hpp
 * backends/opengl/gl.hpp
 * ========================================================================== */

// GL function loading + error helpers.
// The core library never creates a GL context — the host application does
// (GLFW / SDL2 / Qt / Win32 / EGL / your engine), then hands its loader here:
//
//     ember::gl::init(reinterpret_cast<GLADloadfunc>(glfwGetProcAddress));
//
// Works with any loader that resolves GL function names.

#include <glad/gl.h>

#include <cstdio>

namespace ember {
namespace gl {

// Initialize GL function pointers. Must be called after a context is current.
inline int init(GLADloadfunc loader) {
    return gladLoadGL(loader);
}

inline const char* errorString(GLenum e) {
    switch (e) {
        case GL_NO_ERROR: return "GL_NO_ERROR";
        case GL_INVALID_ENUM: return "GL_INVALID_ENUM";
        case GL_INVALID_VALUE: return "GL_INVALID_VALUE";
        case GL_INVALID_OPERATION: return "GL_INVALID_OPERATION";
        case GL_INVALID_FRAMEBUFFER_OPERATION: return "GL_INVALID_FRAMEBUFFER_OPERATION";
        case GL_OUT_OF_MEMORY: return "GL_OUT_OF_MEMORY";
        case GL_STACK_UNDERFLOW: return "GL_STACK_UNDERFLOW";
        case GL_STACK_OVERFLOW: return "GL_STACK_OVERFLOW";
        default: return "unknown GL error";
    }
}

// Pop and print all pending GL errors; returns how many were found.
inline int popErrors(const char* where = nullptr) {
    int n = 0;
    while (GLenum e = glGetError()) {
        if (where) std::fprintf(stderr, "[gl] %s: %s (0x%04x)\n", where, errorString(e), e);
        else std::fprintf(stderr, "[gl] %s (0x%04x)\n", errorString(e), e);
        ++n;
    }
    return n;
}

} // namespace gl
} // namespace ember
/* ============================================================================
 * [ 8] backends/opengl/gpu.hpp
 * backends/opengl/gpu.hpp
 * ========================================================================== */

// RAII wrappers for GL buffer objects and vertex arrays.

namespace ember {

// RAII wrapper for a GL buffer object (VBO / SSBO / ...).
class Buffer {
public:
    explicit Buffer(GLenum target) : target_(target) { glGenBuffers(1, &id_); }
    ~Buffer() {
        if (id_) glDeleteBuffers(1, &id_);
    }

    Buffer(const Buffer&) = delete;
    Buffer& operator=(const Buffer&) = delete;

    Buffer(Buffer&& o) noexcept : target_(o.target_), id_(o.id_) { o.id_ = 0; }
    Buffer& operator=(Buffer&& o) noexcept {
        if (this != &o) {
            if (id_) glDeleteBuffers(1, &id_);
            target_ = o.target_;
            id_ = o.id_;
            o.id_ = 0;
        }
        return *this;
    }

    void bind() const { glBindBuffer(target_, id_); }
    void data(const void* data, GLsizeiptr bytes, GLenum usage) {
        bind();
        glBufferData(target_, bytes, data, usage);
    }
    void subData(GLintptr offset, GLsizeiptr bytes, const void* data) {
        bind();
        glBufferSubData(target_, offset, bytes, data);
    }
    void getSubData(GLintptr offset, GLsizeiptr bytes, void* out) {
        bind();
        glGetBufferSubData(target_, offset, bytes, out);
    }
    GLuint id() const { return id_; }
    GLenum target() const { return target_; }

private:
    GLenum target_ = GL_ARRAY_BUFFER;
    GLuint id_ = 0;
};

// RAII wrapper for a 2D texture (sprite atlases etc.).
class Texture {
public:
    Texture() { glGenTextures(1, &id_); }
    ~Texture() {
        if (id_) glDeleteTextures(1, &id_);
    }

    Texture(const Texture&) = delete;
    Texture& operator=(const Texture&) = delete;

    Texture(Texture&& o) noexcept : id_(o.id_) { o.id_ = 0; }
    Texture& operator=(Texture&& o) noexcept {
        if (this != &o) {
            if (id_) glDeleteTextures(1, &id_);
            id_ = o.id_;
            o.id_ = 0;
        }
        return *this;
    }

    void bind() const { glBindTexture(GL_TEXTURE_2D, id_); }
    GLuint id() const { return id_; }

    // Depth-only attachment (soft particles sample this).
    void uploadDepth(int w, int h) {
        bind(); // other upload*() bind first; without it the image lands in whatever
                // texture is bound on the active unit (e.g. the particle sprite).
        glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, w, h, 0,
                     GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }

    // Upload RGBA8 and set linear filtering + clamp-to-edge (point sprites
    // want no mipmaps).
    void uploadRGBA8(int w, int h, const void* pixels) {
        bind();
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }

    // Float RGBA16F texture (bloom passes).
    void uploadRGBA16F(int w, int h) {
        bind();
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, w, h, 0, GL_RGBA, GL_FLOAT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }

private:
    GLuint id_ = 0;
};

// RAII wrapper for a vertex array object.
class VertexArray {
public:
    VertexArray() { glGenVertexArrays(1, &id_); }
    ~VertexArray() {
        if (id_) glDeleteVertexArrays(1, &id_);
    }

    VertexArray(const VertexArray&) = delete;
    VertexArray& operator=(const VertexArray&) = delete;

    VertexArray(VertexArray&& o) noexcept : id_(o.id_) { o.id_ = 0; }
    VertexArray& operator=(VertexArray&& o) noexcept {
        if (this != &o) {
            if (id_) glDeleteVertexArrays(1, &id_);
            id_ = o.id_;
            o.id_ = 0;
        }
        return *this;
    }

    void bind() const { glBindVertexArray(id_); }
    void unbind() const { glBindVertexArray(0); }
    GLuint id() const { return id_; }

private:
    GLuint id_ = 0;
};

} // namespace ember
/* ============================================================================
 * [ 9] backends/opengl/shader.hpp
 * backends/opengl/shader.hpp
 * ========================================================================== */

// RAII GL program with cached uniform lookup.

#include <initializer_list>
#include <unordered_map>
#include <utility>

namespace ember {

class Shader {
public:
    Shader() = default;
    ~Shader();

    Shader(const Shader&) = delete;
    Shader& operator=(const Shader&) = delete;
    Shader(Shader&&) noexcept;
    Shader& operator=(Shader&&) noexcept;

    // Compile + link from source strings, e.g.
    //   Shader::fromSources({ {GL_VERTEX_SHADER, vsSrc}, {GL_FRAGMENT_SHADER, fsSrc} });
    static Shader fromSources(std::initializer_list<std::pair<GLenum, const char*>> stages);
    static Shader fromSources(const std::vector<std::pair<GLenum, const char*>>& stages);
    static Shader fromFiles(std::initializer_list<std::pair<GLenum, const char*>> paths);
    // Convenience wrappers: vertex+fragment program / compute program from files.
    static Shader fromVertFrag(const char* vertPath, const char* fragPath);
    static Shader fromCompute(const char* compPath);

    void use() const { glUseProgram(id_); }
    GLuint id() const { return id_; }
    explicit operator bool() const { return id_ != 0; }

    // Cached uniform location (returns -1 if the uniform does not exist).
    // Loose uniforms only — block members have no location.
    GLint location(const char* name) const;

    // True if the program declares the uniform, including as a std140 block
    // member (used for shader-protocol detection; cached).
    bool hasUniform(const char* name) const;

    void setInt(const char* n, int v);
    void setUint(const char* n, unsigned v);
    void setFloat(const char* n, float v);
    void setVec2(const char* n, const glm::vec2& v);
    void setVec3(const char* n, const glm::vec3& v);
    void setVec4(const char* n, const glm::vec4& v);
    void setMat4(const char* n, const glm::mat4& v);

private:
    explicit Shader(GLuint id) : id_(id) {}

    GLuint id_ = 0;
    mutable std::unordered_map<std::string, GLint> locs_;
    mutable std::unordered_map<std::string, bool> uniforms_;
};

} // namespace ember
/* ============================================================================
 * [10] opengl.hpp
 * opengl.hpp
 * ========================================================================== */

// OpenGL-specific creation and native-resource customization. Current GL 4.3
// context required; external textures are borrowed, never deleted by ember.

namespace ember {
struct RefractionSettings {
    bool enabled = false;
    GLuint sceneColorTex = 0;   // host scene color (RGBA, CLAMP_TO_EDGE)
    int mode = 0;               // 0 = simple offset, 1 = depth-aware, 2 = noise (heat)
    GLuint sceneDepthTex = 0;   // needed by mode 1; 0 falls back to mode 0
    float strength = 0.02f;     // simple/noise: UV offset; depth: projected displacement scale
    float ior = 1.5f;           // refractive index (mode 1)
    glm::vec3 tint{1.f};        // glass color absorption
    float absorption = 0.35f;   // mix(original, refracted, absorption)
    float fresnel = 2.f;        // rim brightening exponent (0 = off)
    float chroma = 0.f;         // chromatic dispersion (0 = off)
    float specular = 0.f;       // specular glint (0 = off)
    glm::vec3 lightDir{0.5f, 1.f, 0.3f}; // glint light direction (world space, normalized on set)
    int shape = 0;              // 0 = sprite/disc mask, 1 = procedural polygon shard (3-5 sides)
    float dome = 0.f;           // dome normal strength: silhouette rim + glint spots (0 = flat facet)
};

// Built-in presets — fine params live here (in code), not in the config file.
namespace Refraction {
    RefractionSettings glass(); // slightly blue tint, mild chroma + glints
    RefractionSettings heat();  // noise mode, invisible lens
    RefractionSettings water(); // strong fresnel, blue tint
    RefractionSettings prism(); // full chromatic dispersion
}

class ParticleSystem;

std::unique_ptr<ParticleBackend> makeOpenGLBackend();
// Reject non-OpenGL backends before changing any state.
void setOpenGLPrograms(ParticleBackend&, Shader render, Shader simulate);
void setOpenGLRefractionInputs(ParticleBackend&, GLuint color, GLuint depth);
void setOpenGLSoftDepth(ParticleBackend&, GLuint depth);
// Whole-feature adapters: backend-agnostic parameters + native GL textures.
// Replacement programs must follow the uniform/binding contract documented in
// shaders/simulate.comp and shaders/particle.vert.
void setOpenGLRefraction(ParticleSystem&, const RefractionSettings&);
// A supplied depth texture is retained even when initially disabled.
void setOpenGLSoftParticles(ParticleSystem&, bool on, std::uint32_t sceneDepthTex = 0, float radius = 0.5f);
} // namespace ember

#ifdef EMBER_USE_GLFW

/* ============================================================================
 * [11] glfw_window.hpp
 * GLFW convenience window
 * ========================================================================== */

// Minimal GLFW window wrapper (optional convenience module).
// Creates a 4.3 core-profile context, initializes glad, tracks input state
// and per-frame delta time. Host applications that bring their own windowing
// (SDL / Qt / Win32 / ...) do NOT need this — see INTEGRATION.md.

#ifndef GLFW_INCLUDE_NONE
#define GLFW_INCLUDE_NONE
#endif
#include <GLFW/glfw3.h>

#include <functional>

namespace ember {

class ContextUnavailable : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class Window {
public:
    Window(int width, int height, const char* title, int samples = 4, bool vsync = true, bool visible = true);
    ~Window();

    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    bool shouldClose() const { return glfwWindowShouldClose(w_) != 0; }
    void pollEvents();
    void swapBuffers() { glfwSwapBuffers(w_); checkCallbacks(); }
    void setTitle(const char* t) { glfwSetWindowTitle(w_, t); checkCallbacks(); }
    void setVsync(bool on);
    void setCursorPos(glm::vec2 p) { glfwSetCursorPos(w_, p.x, p.y); checkCallbacks(); }

    double time() const { return glfwGetTime(); }
    // Seconds since the last pollEvents(); updated every poll.
    float deltaTime() const { return lastDt_; }

    glm::ivec2 size() const;
    glm::ivec2 framebufferSize() const;

    bool key(int glfwKey) const { return glfwGetKey(w_, glfwKey) == GLFW_PRESS; }
    bool mouseButton(int button) const { return glfwGetMouseButton(w_, button) == GLFW_PRESS; }
    glm::vec2 cursor() const;

    // Accumulated scroll since the last call (returns and resets).
    glm::vec2 scrollDelta();

    GLFWwindow* handle() const { return w_; }

    // Exceptions are captured at the C boundary and rethrown by the next
    // pollEvents/setTitle/setCursorPos/swapBuffers/setVsync on this thread.
    // Optional callbacks (GLFW-style signatures).
    std::function<void(int key, int scancode, int action, int mods)> onKey;
    std::function<void(double x, double y)> onCursorPos;
    std::function<void(int button, int action, int mods)> onMouseButton;
    std::function<void(double xoff, double yoff)> onScroll;
    std::function<void(int width, int height)> onResize;

private:
    static void checkCallbacks();
    static Window* fromHandle(GLFWwindow* w) {
        return static_cast<Window*>(glfwGetWindowUserPointer(w));
    }
    static void keyCb(GLFWwindow*, int, int, int, int);
    static void cursorCb(GLFWwindow*, double, double);
    static void mouseCb(GLFWwindow*, int, int, int);
    static void scrollCb(GLFWwindow*, double, double);
    static void resizeCb(GLFWwindow*, int, int);

    GLFWwindow* w_ = nullptr;
    float lastDt_ = 0.f;
    double lastTime_ = 0.0;
    glm::vec2 scrollAccum_{0.f};
};

} // namespace ember

#endif // EMBER_USE_GLFW

#endif // EMBER_SINGLE_HEADER_HPP

/* ============================================================================
 * SECTION B — IMPLEMENTATION
 * Compile ONCE: define EMBER_IMPLEMENTATION before including this header
 * (one TU in the whole project). Without it this section is skipped.
 * ==========================================================================*/

#ifdef EMBER_IMPLEMENTATION
#ifndef EMBER_SINGLE_HEADER_IMPLEMENTATION
#define EMBER_SINGLE_HEADER_IMPLEMENTATION

/* ============================================================================
 * [12] backends/opengl/particle_backend.hpp
 * backends/opengl/particle_backend.hpp
 * ========================================================================== */

#include <array>
#include <string>

namespace ember::detail::opengl {
class OpenGLBackend final : public ParticleBackend {
public:
    OpenGLBackend();
    ~OpenGLBackend() override;
    OpenGLBackend(const OpenGLBackend&) = delete;
    OpenGLBackend& operator=(const OpenGLBackend&) = delete;
    const char* name() const noexcept override { return "opengl"; }
    BackendCapabilities capabilities() const noexcept override { return {true,true,true,true,true,true,true,true}; }
    void initialize(std::uint32_t capacity, bool debug) override;
    void validateConfiguration(std::uint32_t capacity, bool sorting) override;
    void resize(std::uint32_t capacity) override;
    void clear() override;
    void setDebug(bool enabled) override { debug_=enabled; }
    void uploadAttractors(const std::vector<Attractor>&) override;
    void uploadVortexes(const std::vector<Vortex>&) override;
    void uploadSprings(const std::vector<Spring>&) override;
    void uploadPalette(const std::vector<glm::vec4>&) override;
    void uploadEventTemplates(const std::vector<SpawnRequest>&) override;
    void uploadLifeCurves(const LifeCurvesLut&) override;
    void update(const SimulationParameters&, const UpdateBatch&) override;
    void render(const RenderParameters&, const RenderView&) override;
    ParticleStatistics pollStatistics() override;
    ParticleStatistics synchronizeStatistics() const override;
    std::vector<Particle> readParticles(std::uint32_t max) const override;
    std::uint64_t updateSequence() const override { return frameCount_; }
    std::uint64_t droppedStatistics() const override { return droppedStatistics_; }
    bool gpuDriven() const override { return gpuDriven_; }
    void setGpuDriven(bool) override;
    bool sortEnabled() const override { return sortEnabled_; }
    void setSortEnabled(bool) override;
    bool bloom() const override { return bloom_; }
    void setBloom(bool on) override { bloom_=on; }
    void setSpriteTexture(const char*) override;
    void setShaderDirectory(const char*) override;
    void setPrograms(Shader,Shader);
    void setRefractionInputs(GLuint color,GLuint depth) { sceneColor_=color;refractionDepth_=depth; }
    void setSoftDepth(GLuint depth) { sceneDepth_=depth; }
private:
    void refreshCounts() const;
    void resetStatistics();
    void enqueueStatistics();
    void scheduleGpu(int phase, std::uint32_t requests = 0);
    void validateGpuCapacity(std::uint32_t) const;
    void ensurePrograms();
    void uploadBuiltinSprite();
    void ensureBloom(int w,int h);
    void ensureSortProgram();
    void sortParticles(const glm::mat4& view);
    void drawParticles(const RenderParameters&,const glm::mat4&,const glm::mat4&,float,float,bool refractionPass=false);
    void drawFullscreen();
    using Statistics = ParticleStatistics;
    std::uint32_t capacity_ = 0;
    mutable std::uint32_t alive_ = 0;
    mutable std::uint32_t allocated_ = 0; // exact only when countsFresh_
    bool debug_ = false;      // enabled via EMBER_DEBUG env var
    std::uint64_t frameCount_ = 0;
    bool gpuDriven_ = false;
    mutable bool countsFresh_ = true;
    mutable Statistics statistics_{};
    std::uint64_t droppedStatistics_ = 0;
    std::array<GLuint, 4> statisticsBuffers_{};
    std::array<GLsync, 4> statisticsFences_{};
    std::array<std::uint64_t, 4> statisticsFrames_{};

    Buffer bufA_, bufB_;            // particle ping-pong SSBOs (cur/next)
    Buffer spawnBuf_;               // GPU spawn request staging (binding 2)
    Buffer paletteBuf_;             // palette colors for GPU spawn (binding 9)
    Buffer deadBuf_;                // free-slot stack (recycled dead slots)
    Buffer counterBuf_;             // five counters + three batch reservation words (32 B)
    Buffer attractorBuf_;           // vec4 per attractor
    Buffer vortexBuf_;              // Vortex per entry (binding 6)
    Buffer springBuf_;              // Spring per entry (binding 7)
    Buffer sortedBuf_;              // GPU-sorted particle indices (binding 8)
    Buffer liveBuf_;                // dense live-slot indices (binding 11)
    Buffer nextLiveBuf_;            // next live list during optimized sim (binding 12)
    Buffer sortKeyBuf_;             // cached depth keys during tiled sort (binding 12)
    Buffer indirectBuf_;            // GPU-written draw args (glDrawArraysIndirect, binding 10)
    Buffer dispatchBuf_;            // GPU scheduling metadata and dispatch commands (binding 13)
    Buffer eventTagBuf_;            // per-slot metadata uvec2: event tag + birth id (binding 20)
    Buffer eventTemplateBuf_;       // event templates, SpawnRequest[] (binding 21)
    Buffer eventQueueBuf_;          // GPU event queue, header + GpuEvent[] (binding 22)
    Buffer curvesBuf_;              // baked lifecycle-curve LUTs (binding 23)
    Buffer uboSim_, uboSort_, uboSchedule_, uboDraw_, uboFrag_, uboBloom_; // std140 parameter blocks
    VertexArray vao_;
    Texture spriteTex_;             // particle sprite (built-in gradient or PNG)
    Buffer* cur_ = nullptr;         // sim source / render source
    Buffer* nxt_ = nullptr;

    Shader renderProg_, simProg_, sortProg_;
    Shader scheduleProg_;
    std::uint32_t sortKeyCapacity_ = 0; // lazily allocated padded key count
    // ---- bloom ----
    bool bloom_ = false;
    int bloomW_ = 0, bloomH_ = 0;
    Texture bloomDepthTex_;         // resolved host depth for bloom occlusion
    Texture bloomTex_, bloomHalfTex_, bloomBlurTex_;
    GLuint bloomFbo_ = 0, bloomHalfFbo_ = 0, bloomBlurFbo_ = 0;
    Shader bloomPass_, bloomBlur_, bloomComposite_;
    std::string shaderDir_ = "shaders";

    bool sortEnabled_ = false;
    GLuint sceneColor_ = 0, refractionDepth_ = 0, sceneDepth_ = 0;
    std::uint32_t eventTemplateCount_ = 0; // active templates (0 = events off)
};
} // namespace ember::detail::opengl
/* ============================================================================
 * [13] backends/opengl/params.hpp
 * backends/opengl/params.hpp
 * ========================================================================== */

// CPU mirrors of the std140 uniform blocks used by the built-in shaders.
// Loose uniforms are gone from the built-in GLSL so the same sources compile
// to SPIR-V for Vulkan (no loose uniforms there). Layout must match the GLSL
// blocks exactly — the static_asserts below pin size and offsets. The backend
// still writes the legacy loose uniforms too, so custom shaders written
// against the pre-UBO contract keep working.

#include <glm/glm.hpp>

#include <cstdint>

namespace ember::detail::opengl {

// simulate.comp — binding 14
struct SimParams {
    glm::vec3 gravity;          float drag;
    glm::vec3 wind;             float turbulence;
    glm::vec3 noiseWindDir;     float noiseWindAmp;
    float noiseWindScale;       float noiseWindSpeed;
    float waveAmp;              float waveOmega;
    glm::vec3 waveDir;          float dt;
    glm::vec3 waveK;            float time;
    std::uint32_t forceMask;    std::int32_t dragMode;
    std::int32_t attractorCount; std::int32_t vortexCount;
    std::int32_t springCount;   std::int32_t boundaryMode;
    float boundaryY;            float restitution;
    std::uint32_t spawnTotal;   std::uint32_t frameSeed;
    std::uint32_t inputAlive;   std::int32_t gpuDriven;
    // eventTemplates: active event-template count (0 = events off). Occupies
    // the former pad0 slot; layout is unchanged.
    std::int32_t phase;         std::uint32_t eventTemplates; std::int32_t pad1, pad2;
};
static_assert(sizeof(SimParams) == 160, "SimParams must match the std140 block");
static_assert(offsetof(SimParams, waveDir) == 64 && offsetof(SimParams, forceMask) == 96 &&
              offsetof(SimParams, spawnTotal) == 128 && offsetof(SimParams, phase) == 144,
              "SimParams member offsets must match the GLSL block");

// sort.comp — binding 15
struct SortParams {
    glm::mat4 view;
    std::uint32_t capacity, alive, paddedN, k;
    std::uint32_t j, mode, tileMode; std::int32_t gpuDriven;
};
static_assert(sizeof(SortParams) == 96, "SortParams must match the std140 block");

// schedule.comp — binding 16
struct ScheduleParams {
    std::int32_t schedulePhase; std::uint32_t requestCount;
    std::uint32_t pad0, pad1;
};
static_assert(sizeof(ScheduleParams) == 16, "ScheduleParams must match the std140 block");

// particle.vert — binding 17
struct DrawParams {
    glm::mat4 view;
    glm::mat4 proj;
    float sizeScale, streak, spinSpeed; std::int32_t useSorted;
    std::int32_t refraction; std::int32_t pad0, pad1, pad2;
};
static_assert(sizeof(DrawParams) == 160, "DrawParams must match the std140 block");

// particle.frag — binding 18
struct FragParams { // GLSL renames: proj->uFragProj, refraction->uFragRefraction
    glm::mat4 invProj;
    glm::mat4 fragProj;
    float viewportWidth, viewportHeight, softRadius; std::int32_t useSprite;
    std::int32_t useSoft, sheetCols, sheetRows, fragRefraction;
    std::int32_t refrMode, refrShape; float refrDome, refrStrength;
    float refrIor, refrAbsorption, refrFresnel, refrChroma;
    glm::vec3 refrTint; float refrSpecular;
    glm::vec3 refrLightDir; float refrPad;
};
static_assert(sizeof(FragParams) == 224, "FragParams must match the std140 block");
static_assert(offsetof(FragParams, viewportWidth) == 128 && offsetof(FragParams, refrTint) == 192 &&
              offsetof(FragParams, refrLightDir) == 208,
              "FragParams member offsets must match the GLSL block");

// bloom_threshold.frag / bloom_blur.frag — binding 19
struct BloomParams {
    float texelX, texelY, threshold, pad0;
    float dirX, dirY, pad1, pad2;
};
static_assert(sizeof(BloomParams) == 32, "BloomParams must match the std140 block");

// particle.vert — BufCurves SSBO (std430, the first non-UBO mirror here).
// GL binding 23, Vulkan render set binding 8; uploaded without the CPU mask.
struct CurvesParams {
    glm::vec4 colorLut[64];
    float     sizeLut[64];
};
static_assert(sizeof(CurvesParams) == 1280, "CurvesParams must match the std430 BufCurves block");
static_assert(offsetof(CurvesParams, sizeLut) == 1024, "sizeLut must follow colorLut");

} // namespace ember::detail::opengl
/* ============================================================================
 * [14] backends/opengl/common.hpp
 * backends/opengl/common.hpp
 * ========================================================================== */

#ifdef EMBER_BENCHMARK_HOOKS
#include <ember_benchmark_hooks.hpp>
#define EMBER_BENCH_BEGIN(stage) ::ember::benchmark::beginStage(::ember::benchmark::Stage::stage)
#define EMBER_BENCH_END(stage) ::ember::benchmark::endStage(::ember::benchmark::Stage::stage)
#else
#define EMBER_BENCH_BEGIN(stage) ((void)0)
#define EMBER_BENCH_END(stage) ((void)0)
#endif

namespace ember::detail::opengl {
inline constexpr std::uint32_t kSimGroupSize = 64;
inline constexpr std::uint32_t kMaxSpawnRequests = maxSpawnRequests;
inline constexpr GLuint kBindingCur = 0;         // particle SSBO (read)
inline constexpr GLuint kBindingNext = 1;        // particle SSBO (write)
inline constexpr GLuint kBindingSpawn = 2;       // spawn request SSBO (read)
inline constexpr GLuint kBindingDead = 3;        // free-slot stack
inline constexpr GLuint kBindingCounters = 4;    // five public counters + three spawn reservation words
inline constexpr GLuint kBindingAttractors = 5;  // vec4 attractors
inline constexpr GLuint kBindingVortexes = 6;    // Vortex array
inline constexpr GLuint kBindingSprings = 7;     // Spring array
inline constexpr GLuint kBindingSorted = 8;      // sorted particle indices (render only)
inline constexpr GLuint kBindingPalette = 9;     // palette colors (GPU spawn)
inline constexpr GLuint kBindingLive = 11;       // dense live-slot index list
inline constexpr GLuint kBindingScratch = 12;   // next live list during sim, depth keys during sort
inline constexpr GLuint kBindingIndirect = 10;   // draw args for glDrawArraysIndirect
inline constexpr GLuint kBindingSchedule = 13;
// Event sub-emission (WO-08): per-slot metadata (uvec2: event tag + birth id),
// templates and the GPU event queue.
inline constexpr GLuint kBindingEventTags = 20;      // uvec2 per particle slot
inline constexpr GLuint kBindingEventTemplates = 21; // SpawnRequest[maxEventTemplates]
inline constexpr GLuint kBindingEventQueue = 22;     // header + GpuEvent[maxEventInstancesPerFrame]
// Lifecycle curves (WO-09): BufCurves SSBO in the render (vertex) path.
inline constexpr GLuint kBindingCurves = 23;
// Uniform-block bindings for the built-in shaders (14+ keeps the Vulkan
// descriptor namespace free of collisions with SSBO bindings 0-13). GL
// requires MAX_UNIFORM_BUFFER_BINDINGS >= 24 on 4.3 hardware.
inline constexpr GLuint kBindingUboSim = 14;
inline constexpr GLuint kBindingUboSort = 15;
inline constexpr GLuint kBindingUboSchedule = 16;
inline constexpr GLuint kBindingUboDraw = 17;
inline constexpr GLuint kBindingUboFrag = 18;
inline constexpr GLuint kBindingUboBloom = 19;
// 2^30 capacity needs 276 tiled sort commands. Reserve 512 plus metadata.
inline constexpr GLsizeiptr kScheduleBytes = (5 + 3 * 512) * sizeof(GLuint);
// Event sub-emission (WO-08): GpuEvent is 48 B in std430; the queue is a
// 16-byte header followed by the fixed instance table.
inline constexpr std::uint32_t kMaxEventInstances = maxEventInstancesPerFrame;
inline constexpr std::uint32_t kMaxEventChildren = maxEventChildrenPerFrame;
inline constexpr std::uint64_t kGpuEventBytes = 48;
inline constexpr std::uint64_t kEventQueueBytes = 4ull * sizeof(std::uint32_t) + (std::uint64_t)kMaxEventInstances * kGpuEventBytes;
inline std::uint32_t nextPow2(std::uint32_t v) {
    if (v <= 1) return 1;
    v--;
    v |= v >> 1; v |= v >> 2; v |= v >> 4; v |= v >> 8; v |= v >> 16;
    return v + 1;
}

extern const char* const kScheduleComp;
extern const char* const kSimulateComp;
extern const char* const kSortComp;
extern const char* const kParticleVert;
extern const char* const kParticleFrag;
} // namespace ember::detail::opengl
/* ============================================================================
 * [15] particle_system.cpp
 * particle_system.cpp
 * ========================================================================== */

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <stdexcept>
#include <utility>

namespace ember {
namespace {
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

// Uniformly spaced keys: t_i = i/(n-1); n == 1 is constant; endpoints clamp.
// Both backends bake with this exact function so pixel comparisons line up.
glm::vec4 evalUniformColor(const std::vector<glm::vec4>& keys, float t) {
    if (keys.empty()) return glm::vec4(1.f);
    if (keys.size() == 1) return keys[0];
    const float u = std::max(0.f, std::min(1.f, t)) * float(keys.size() - 1);
    const std::size_t i = std::min<std::size_t>((std::size_t)u, keys.size() - 2);
    return glm::mix(keys[i], keys[i + 1], u - float(i));
}
float evalUniformSize(const std::vector<float>& keys, float t) {
    if (keys.empty()) return 1.f;
    if (keys.size() == 1) return keys[0];
    const float u = std::max(0.f, std::min(1.f, t)) * float(keys.size() - 1);
    const std::size_t i = std::min<std::size_t>((std::size_t)u, keys.size() - 2);
    return keys[i] + (keys[i + 1] - keys[i]) * (u - float(i));
}

}
ParticleSystem::ParticleSystem(const Settings& s, std::unique_ptr<ParticleBackend> backend)
    : backend_(std::move(backend)), capacity_(s.capacity),
      maxSpawnPerFrame_(s.maxSpawnPerFrame),
      frameSeed_(s.seed ? s.seed : std::random_device{}()),
      debug_(std::getenv("EMBER_DEBUG") != nullptr) {
    if(!backend_) throw std::invalid_argument("ember: null particle backend");
    if(capacity_==0 || capacity_>(1u<<30))
        throw std::invalid_argument("ember: capacity must be in [1, 2^30]");
    backend_->initialize(capacity_,debug_);
}
ParticleSystem::~ParticleSystem() = default;
ParticleSystem::ParticleSystem(ParticleSystem&&) noexcept = default;
ParticleSystem& ParticleSystem::operator=(ParticleSystem&&) noexcept = default;

void ParticleSystem::setGravity(glm::vec3 g) { simulation_.gravity = g; }

void ParticleSystem::setDrag(float k) { simulation_.drag = k; }

void ParticleSystem::setWind(glm::vec3 w) { simulation_.wind = w; }

void ParticleSystem::setTurbulence(float k) { simulation_.turbulence = k; }

void ParticleSystem::setBlendMode(BlendMode m) { rendering_.blend = m; }

void ParticleSystem::setDepthTest(bool on) { rendering_.depthTest = on; }

void ParticleSystem::setDepthWrite(bool on) { rendering_.depthWrite = on; }

void ParticleSystem::setMaxSpawnPerFrame(std::uint32_t n) {
    maxSpawnPerFrame_ = n; // spawn volume is budgeted in update(); no staging buffer to size
}

void ParticleSystem::setForceEnabled(Force f, bool on) {
    const auto index = static_cast<std::uint32_t>(f);
    if (index > static_cast<std::uint32_t>(Force::Boundary))
        throw std::invalid_argument("ember: invalid force");
    const std::uint32_t bit = 1u << index;
    if (on) simulation_.forceMask |= bit;
    else simulation_.forceMask &= ~bit;
}

void ParticleSystem::setForceMask(std::uint32_t mask) { simulation_.forceMask = mask; }

void ParticleSystem::setDragMode(DragMode m) { simulation_.dragMode = m; }

void ParticleSystem::setNoiseWind(glm::vec3 dir, float amplitude, float scale, float speed) {
    simulation_.noiseWindDir = dir;
    simulation_.noiseWindAmp = amplitude;
    simulation_.noiseWindScale = scale;
    simulation_.noiseWindSpeed = speed;
    setForceEnabled(Force::NoiseWind, amplitude != 0.f);
}

void ParticleSystem::setWave(glm::vec3 dir, glm::vec3 waveVector, float amplitude, float omega) {
    simulation_.waveDir = dir;
    simulation_.waveK = waveVector;
    simulation_.waveAmp = amplitude;
    simulation_.waveOmega = omega;
    setForceEnabled(Force::Wave, amplitude != 0.f);
}

void ParticleSystem::setBoundary(BoundaryMode mode, float planeY, float restitution) {
    simulation_.boundaryMode = mode;
    simulation_.boundaryY = planeY;
    simulation_.restitution = restitution;
    setForceEnabled(Force::Boundary, true);
}

void ParticleSystem::setSizeScale(float s, bool linkSpeed) {
    const float factor = (rendering_.sizeScale > 0.f && s > 0.f) ? s / rendering_.sizeScale : 1.f; // guard div-by-zero
    rendering_.sizeScale = s;
    if (linkSpeed && factor != 1.f) {
        speedScaleBase_ *= factor;
        for (auto& e : emitters_) e.speedScale *= factor;
    }
}

void ParticleSystem::setUseSprite(bool on) { rendering_.useSprite = on; }

void ParticleSystem::setSpriteSheet(int cols, int rows) {
    rendering_.sheetCols = std::max(1, cols);
    rendering_.sheetRows = std::max(1, rows);
}

void ParticleSystem::loadConfig(const char* path) {
    apply(Config::fromFile(path));
}

void ParticleSystem::apply(const Config& cfg) {
    // ------------------------------------------------------------------
    // Phase 1 — validate everything first, so a bad config throws before
    // any state is mutated (loadConfig stays atomic).
    // ------------------------------------------------------------------
    if (cfg.system.capacity > (1u << 30)) throw std::invalid_argument("ember: capacity exceeds supported range");
    std::uint32_t mask = 0;
    if (cfg.has("system.forces"))
        for (const auto& name : cfg.system.forces) mask |= forceBit(name);

    const DragMode dragMode = cfg.has("system.drag_mode")
        ? (cfg.system.dragMode == "quadratic" ? DragMode::Quadratic
           : cfg.system.dragMode == "linear"   ? DragMode::Linear
           : throw std::runtime_error("ember: unknown drag_mode '" + cfg.system.dragMode + "'"))
        : simulation_.dragMode;

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
        : rendering_.blend;

    if (cfg.has("system.refraction_mode") && cfg.system.refractionMode != "simple" &&
        cfg.system.refractionMode != "depth" && cfg.system.refractionMode != "noise")
        throw std::invalid_argument("ember: invalid refraction_mode");
    for (const auto& ec : cfg.emitters)
        if (!ec.paletteName.empty() && !cfg.findPalette(ec.paletteName.c_str()))
            throw std::runtime_error("ember: unknown palette '" + ec.paletteName +
                                     "' referenced by emitter '" + ec.name + "'");
    for (const auto& ec : cfg.events)
        if (!ec.paletteName.empty() && !cfg.findPalette(ec.paletteName.c_str()))
            throw std::runtime_error("ember: unknown palette '" + ec.paletteName +
                                     "' referenced by event '" + ec.name + "'");
    if (cfg.has("event") && cfg.events.size() > maxEventTemplates)
        throw std::invalid_argument("ember: too many event templates");
    // Event references resolve in two phases: collect every [event] name first,
    // then bind on_death/on_bounce (templates may chain to other templates).
    for (const auto& ec : cfg.events) {
        if (!ec.onDeathName.empty() && cfg.findEvent(ec.onDeathName.c_str()) < 0)
            throw std::runtime_error("ember: unknown event '" + ec.onDeathName +
                                     "' referenced by event '" + ec.name + "'");
        if (!ec.onBounceName.empty() && cfg.findEvent(ec.onBounceName.c_str()) < 0)
            throw std::runtime_error("ember: unknown event '" + ec.onBounceName +
                                     "' referenced by event '" + ec.name + "'");
    }
    for (const auto& ec : cfg.emitters) {
        if (!ec.onDeathName.empty() && cfg.findEvent(ec.onDeathName.c_str()) < 0)
            throw std::runtime_error("ember: unknown event '" + ec.onDeathName +
                                     "' referenced by emitter '" + ec.name + "'");
        if (!ec.onBounceName.empty() && cfg.findEvent(ec.onBounceName.c_str()) < 0)
            throw std::runtime_error("ember: unknown event '" + ec.onBounceName +
                                     "' referenced by emitter '" + ec.name + "'");
    }
    if (cfg.has("curves.color") && !cfg.colorKeys.empty() && !backend_->capabilities().lifeCurves)
        throw std::invalid_argument("ember: backend does not support life curves");
    if (cfg.has("curves.size") && !cfg.sizeKeys.empty() && !backend_->capabilities().lifeCurves)
        throw std::invalid_argument("ember: backend does not support life curves");
    backend_->validateConfiguration(cfg.system.capacity ? cfg.system.capacity : capacity_,
                                    cfg.has("system.sort") ? cfg.system.sort : sortEnabled());
    if (cfg.has("system.bloom") && cfg.system.bloom && !backend_->capabilities().bloom)
        throw std::invalid_argument("ember: backend does not support bloom");
    if (cfg.has("system.refraction") && cfg.system.refraction && !backend_->capabilities().refraction)
        throw std::invalid_argument("ember: backend does not support refraction");
    if (cfg.has("system.soft_particles") && cfg.system.softParticles && !backend_->capabilities().softParticles)
        throw std::invalid_argument("ember: backend does not support soft particles");
    if (cfg.has("system.texture") && !backend_->capabilities().spriteTextures)
        throw std::invalid_argument("ember: backend does not support sprite textures");
    if (cfg.has("system.soft_radius") && (!std::isfinite(cfg.system.softRadius) || cfg.system.softRadius <= 0.f))
        throw std::invalid_argument("ember: soft particle radius must be finite and positive");

    // ------------------------------------------------------------------
    // Phase 2 — apply. Only keys/sections actually present in the config
    // are touched; anything the config does not mention keeps its current
    // (programmatic) value.
    // ------------------------------------------------------------------
    if (cfg.system.capacity != 0 && cfg.system.capacity != capacity_) {
        backend_->resize(cfg.system.capacity);
        capacity_ = cfg.system.capacity;
    }
    if (cfg.system.maxSpawnPerFrame != 0) setMaxSpawnPerFrame(cfg.system.maxSpawnPerFrame);

    if (cfg.has("system.gravity")) setGravity(cfg.system.gravity);
    if (cfg.has("system.drag")) setDrag(cfg.system.drag);
    if (cfg.has("system.wind")) setWind(cfg.system.wind);
    if (cfg.has("system.turbulence")) setTurbulence(cfg.system.turbulence);
    if (cfg.has("system.drag_mode")) setDragMode(dragMode);

    if (cfg.has("system.noise_wind_direction") || cfg.has("system.noise_wind_amplitude") ||
        cfg.has("system.noise_wind_scale") || cfg.has("system.noise_wind_speed"))
        setNoiseWind(cfg.has("system.noise_wind_direction") ? cfg.system.noiseWindDir : simulation_.noiseWindDir,
                     cfg.has("system.noise_wind_amplitude") ? cfg.system.noiseWindAmp : simulation_.noiseWindAmp,
                     cfg.has("system.noise_wind_scale") ? cfg.system.noiseWindScale : simulation_.noiseWindScale,
                     cfg.has("system.noise_wind_speed") ? cfg.system.noiseWindSpeed : simulation_.noiseWindSpeed);
    if (cfg.has("system.wave_direction") || cfg.has("system.wave_vector") ||
        cfg.has("system.wave_amplitude") || cfg.has("system.wave_omega"))
        setWave(cfg.has("system.wave_direction") ? cfg.system.waveDir : simulation_.waveDir,
                cfg.has("system.wave_vector") ? cfg.system.waveK : simulation_.waveK,
                cfg.has("system.wave_amplitude") ? cfg.system.waveAmp : simulation_.waveAmp,
                cfg.has("system.wave_omega") ? cfg.system.waveOmega : simulation_.waveOmega);

    const float boundaryY = cfg.has("system.boundary_y") ? cfg.system.boundaryY : simulation_.boundaryY;
    const float restitution = cfg.has("system.restitution") ? cfg.system.restitution : simulation_.restitution;
    if (cfg.has("system.boundary_mode")) {
        if (boundary == BC::Kill) setBoundary(BoundaryMode::Kill, boundaryY, restitution);
        else if (boundary == BC::Bounce) setBoundary(BoundaryMode::Bounce, boundaryY, restitution);
        else disableForce(Force::Boundary);
    } else if (cfg.has("system.boundary_y") || cfg.has("system.restitution")) {
        simulation_.boundaryY = boundaryY; // param-only update, mask untouched
        simulation_.restitution = restitution;
    }

    if (cfg.has("system.blend")) setBlendMode(blend);
    if (cfg.has("system.debug")) { debug_ = cfg.system.debug; backend_->setDebug(debug_); }
    if (cfg.has("system.size_scale")) setSizeScale(cfg.system.sizeScale, cfg.system.scaleSpeedWithSize);
    if (cfg.has("system.texture"))
        setSpriteTexture(cfg.system.texture.c_str());
    if (cfg.has("system.streak")) setStreak(cfg.system.streak);
    if (cfg.has("system.soft_particles")) {
        rendering_.softParticles = cfg.system.softParticles; // scene depth stays host-provided
    }
    if (cfg.has("system.soft_radius")) rendering_.softRadius = cfg.system.softRadius;
    if (cfg.has("system.bloom")) setBloom(cfg.system.bloom);
    if (cfg.has("system.bloom_threshold")) setBloomThreshold(cfg.system.bloomThreshold);
    if (cfg.has("system.sort")) setSortEnabled(cfg.system.sort);
    if (cfg.has("system.spin")) setSpin(cfg.system.spin);
    if (cfg.has("system.refraction")) rendering_.refraction.enabled = cfg.system.refraction;
    if (cfg.has("system.refraction_mode"))
        rendering_.refraction.mode = cfg.system.refractionMode == "depth" ? 1
                         : cfg.system.refractionMode == "noise" ? 2 : 0;
    if (cfg.has("system.refraction_strength")) rendering_.refraction.strength = cfg.system.refractionStrength;

    // Lifecycle curves: resample explicit-t INI keys into 64 uniform values,
    // then hand them to the same setter the programmatic API uses.
    if (cfg.has("curves.color")) {
        std::vector<glm::vec4> uniform;
        if (!cfg.colorKeys.empty()) {
            uniform.resize(kLifeCurveLutSize);
            const auto& keys = cfg.colorKeys;
            for (std::uint32_t j = 0; j < kLifeCurveLutSize; ++j) {
                const float t = float(j) / float(kLifeCurveLutSize - 1);
                if (t <= keys.front().t) uniform[j] = keys.front().value;
                else if (t >= keys.back().t) uniform[j] = keys.back().value;
                else {
                    std::size_t i = 0;
                    while (i + 1 < keys.size() && keys[i + 1].t < t) ++i;
                    const float f = (t - keys[i].t) / (keys[i + 1].t - keys[i].t);
                    uniform[j] = glm::mix(keys[i].value, keys[i + 1].value, f);
                }
            }
        }
        setColorOverLife(std::move(uniform));
    }
    if (cfg.has("curves.size")) {
        std::vector<float> uniform;
        if (!cfg.sizeKeys.empty()) {
            uniform.resize(kLifeCurveLutSize);
            const auto& keys = cfg.sizeKeys;
            for (std::uint32_t j = 0; j < kLifeCurveLutSize; ++j) {
                const float t = float(j) / float(kLifeCurveLutSize - 1);
                if (t <= keys.front().t) uniform[j] = keys.front().value.x;
                else if (t >= keys.back().t) uniform[j] = keys.back().value.x;
                else {
                    std::size_t i = 0;
                    while (i + 1 < keys.size() && keys[i + 1].t < t) ++i;
                    const float f = (t - keys[i].t) / (keys[i + 1].t - keys[i].t);
                    uniform[j] = keys[i].value.x + (keys[i + 1].value.x - keys[i].value.x) * f;
                }
            }
        }
        setSizeOverLife(std::move(uniform));
    }

    if (cfg.has("event")) {
        clearEventEmitters();
        for (const auto& ec : cfg.events) {
            Emitter e = ec;
            if (!ec.paletteName.empty()) // validated in phase 1, so non-null
                e.palette = cfg.findPalette(ec.paletteName.c_str())->colors;
            if (!ec.onDeathName.empty()) e.onDeath = cfg.findEvent(ec.onDeathName.c_str());
            if (!ec.onBounceName.empty()) e.onBounce = cfg.findEvent(ec.onBounceName.c_str());
            addEventEmitter(e);
        }
    }
    if (cfg.has("emitter")) {
        clearEmitters();
        for (const auto& ec : cfg.emitters) {
            Emitter e = ec; // slices EmitterConfig -> Emitter (drops name/paletteName)
            if (!ec.paletteName.empty()) // validated in phase 1, so non-null
                e.palette = cfg.findPalette(ec.paletteName.c_str())->colors;
            if (!ec.onDeathName.empty()) e.onDeath = cfg.findEvent(ec.onDeathName.c_str());
            if (!ec.onBounceName.empty()) e.onBounce = cfg.findEvent(ec.onBounceName.c_str());
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

Emitter& ParticleSystem::addEmitter(const Emitter& e) {
    if (e.onDeath < -1 || e.onDeath >= (int)eventEmitters_.size() ||
        e.onBounce < -1 || e.onBounce >= (int)eventEmitters_.size())
        throw std::invalid_argument("ember: event reference out of range");
    emitters_.push_back(e);
    // Inherit the accumulated size<->speed link factor (setSizeScale(.., true)).
    emitters_.back().speedScale *= speedScaleBase_;
    registerEmitterPalette(emitters_.back());
    return emitters_.back();
}

std::size_t ParticleSystem::addEventEmitter(const Emitter& e) {
    if (eventEmitters_.size() >= maxEventTemplates)
        throw std::invalid_argument("ember: too many event templates");
    if (!std::isfinite(e.inheritVelocity))
        throw std::invalid_argument("ember: event inheritVelocity must be finite");
    Emitter ev = e;
    ev.eventCount = std::max(1u, std::min(ev.eventCount, 64u));
    eventEmitters_.push_back(ev);
    registerEmitterPalette(eventEmitters_.back());
    eventTemplatesDirty_ = true;
    return eventEmitters_.size() - 1;
}

void ParticleSystem::clearEventEmitters() {
    eventEmitters_.clear();
    for (auto& e : emitters_) { e.onDeath = -1; e.onBounce = -1; }
    eventTemplatesDirty_ = false;
    rebuildPaletteBuffer();
    backend_->uploadEventTemplates({}); // release backend resources; empty never throws
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
        backend_->uploadPalette(paletteData_);
    }
}

void ParticleSystem::rebuildPaletteBuffer() {
    paletteData_.clear();
    auto append = [&](std::vector<Emitter>& list) {
        for (auto& e : list) {
            e.paletteIdx = e.palette.empty() ? -1 : (int)paletteData_.size();
            e.paletteCount = (int)e.palette.size();
            paletteData_.insert(paletteData_.end(), e.palette.begin(), e.palette.end());
        }
    };
    append(emitters_);
    append(eventEmitters_);
    backend_->uploadPalette(paletteData_);
    // Template requests bake paletteIdx; a rebuilt layout invalidates the
    // encoded copies the backend holds, so re-upload on the next update.
    eventTemplatesDirty_ = true;
}

void ParticleSystem::synchronizePalettes() {
    std::size_t offset = 0;
    auto matches = [&](const std::vector<Emitter>& list) {
        for (const auto& e : list) {
            if (e.paletteIdx != (e.palette.empty() ? -1 : (int)offset) ||
                e.paletteCount != (int)e.palette.size() || offset + e.palette.size() > paletteData_.size() ||
                !std::equal(e.palette.begin(), e.palette.end(), paletteData_.begin() + offset))
                return false;
            offset += e.palette.size();
        }
        return true;
    };
    if (!matches(emitters_) || !matches(eventEmitters_) || offset != paletteData_.size())
        rebuildPaletteBuffer();
}

void ParticleSystem::synchronizeEventTemplates() {
    if (!eventTemplatesDirty_) return;
    std::vector<SpawnRequest> templates;
    templates.reserve(eventEmitters_.size());
    for (const auto& e : eventEmitters_) templates.push_back(e.makeRequest(0, e.eventCount));
    backend_->uploadEventTemplates(templates);
    eventTemplatesDirty_ = false;
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
    r.flags = b.refractive ? 2 : 0; // bit1 = refractive (sign-encoded in pos.w)
    r.count = std::min(b.count, maxSpawnPerFrame_);
    pendingRequests_.push_back(r);
    // Keep the pending queue bounded; drop the oldest overflow.
    if (pendingRequests_.size() > maxSpawnRequests) {
        pendingRequests_.erase(pendingRequests_.begin(),
                               pendingRequests_.begin() +
                                   (std::ptrdiff_t)(pendingRequests_.size() - maxSpawnRequests));
    }
}

void ParticleSystem::setStreak(float k) { rendering_.streak = k; }

void ParticleSystem::setSpin(float speed) { rendering_.spin = speed; }

void ParticleSystem::setBloomThreshold(float t) { rendering_.bloomThreshold = t; }

void ParticleSystem::uploadLifeCurves() {
    LifeCurvesLut lut{};
    lut.mask = (colorOverLifeKeys_.empty() ? 0u : 1u) | (sizeOverLifeKeys_.empty() ? 0u : 2u);
    for (std::uint32_t i = 0; i < kLifeCurveLutSize; ++i) {
        const float t = float(i) / float(kLifeCurveLutSize - 1);
        lut.color[i] = evalUniformColor(colorOverLifeKeys_, t);
        lut.size[i] = evalUniformSize(sizeOverLifeKeys_, t);
    }
    backend_->uploadLifeCurves(lut);
}

void ParticleSystem::setColorOverLife(std::vector<glm::vec4> keys) {
    if (!keys.empty() && !backend_->capabilities().lifeCurves)
        throw std::invalid_argument("ember: backend does not support life curves");
    for (const auto& k : keys)
        for (int c = 0; c < 4; ++c)
            if (!std::isfinite(k[c])) throw std::invalid_argument("ember: life curve keys must be finite");
    colorOverLifeKeys_ = std::move(keys);
    uploadLifeCurves();
}

void ParticleSystem::setSizeOverLife(std::vector<float> keys) {
    if (!keys.empty() && !backend_->capabilities().lifeCurves)
        throw std::invalid_argument("ember: backend does not support life curves");
    for (float k : keys)
        if (!std::isfinite(k)) throw std::invalid_argument("ember: life curve keys must be finite");
    sizeOverLifeKeys_ = std::move(keys);
    uploadLifeCurves();
}

void ParticleSystem::setAttractors(const std::vector<Attractor>& a) {
    simulation_.attractors = a;
    backend_->uploadAttractors(a);
}

void ParticleSystem::setVortexes(const std::vector<Vortex>& v) {
    simulation_.vortexes = v;
    backend_->uploadVortexes(v);
    setForceEnabled(Force::Vortex, !v.empty());
}

void ParticleSystem::setSprings(const std::vector<Spring>& s) {
    simulation_.springs = s;
    backend_->uploadSprings(s);
    setForceEnabled(Force::Spring, !s.empty());
}

void ParticleSystem::update(float dt) {
    if (!std::isfinite(dt) || dt < 0.f) throw std::invalid_argument("ember: dt must be finite and nonnegative");
    synchronizePalettes();
    synchronizeEventTemplates();
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
        if (reqs.size() == maxSpawnRequests) break;
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
        if (reqs.size() == maxSpawnRequests) break;
        const std::uint32_t cnt = e.takeCount(dt, budget - total);
        if (cnt > 0) {
            reqs.push_back(e.makeRequest(total, cnt));
            total += cnt;
        }
    }

    backend_->update(simulation_, UpdateBatch{reqs,total,dt,time_,frameSeed_++});
}

void ParticleSystem::setRefractionParameters(const RefractionParameters& s) {
    if (!std::isfinite(s.ior) || s.ior <= 0.f || s.mode < 0 || s.mode > 2 ||
        !std::isfinite(s.strength))
        throw std::invalid_argument("ember: invalid refraction index, mode or strength");
    if(s.enabled && !backend_->capabilities().refraction)
        throw std::invalid_argument("ember: backend does not support refraction");
    rendering_.refraction = s;
    // Keep lightDir unit-length: pow(dot, 24) explodes for |L| > 1 and turns
    // whole shards into blown-out white blobs.
    const float len = glm::length(rendering_.refraction.lightDir);
    if (len > 1e-6f) rendering_.refraction.lightDir /= len;
}

void ParticleSystem::setSoftParticleParameters(bool on, float radius) {
    if (!std::isfinite(radius) || radius <= 0.f)
        throw std::invalid_argument("ember: soft particle radius must be finite and positive");
    if(on && !backend_->capabilities().softParticles)
        throw std::invalid_argument("ember: backend does not support soft particles");
    rendering_.softParticles=on;
    rendering_.softRadius=radius;
}
void ParticleSystem::clear() { backend_->clear(); pendingRequests_.clear(); }
void ParticleSystem::setGpuDriven(bool on) { backend_->setGpuDriven(on); }
void ParticleSystem::setSortEnabled(bool on) { backend_->setSortEnabled(on); }
void ParticleSystem::setBloom(bool on) { backend_->setBloom(on); }
void ParticleSystem::setSpriteTexture(const char* path) { backend_->setSpriteTexture(path); }
void ParticleSystem::setShaderDirectory(const char* path) { backend_->setShaderDirectory(path); }
std::uint32_t ParticleSystem::aliveCount() const { return backend_->synchronizeStatistics().alive; }
ParticleSystem::Statistics ParticleSystem::pollStatistics() { return backend_->pollStatistics(); }
ParticleSystem::Statistics ParticleSystem::synchronizeStatistics() const { return backend_->synchronizeStatistics(); }
std::vector<Particle> ParticleSystem::readParticles(std::uint32_t max) const { return backend_->readParticles(max); }
void ParticleSystem::render(const glm::mat4& view, const glm::mat4& proj, float w, float h, float fov) {
    backend_->render(rendering_, RenderView{view,proj,w,h,fov});
}
} // namespace ember
/* ============================================================================
 * [16] backends/opengl/shader.cpp
 * backends/opengl/shader.cpp
 * ========================================================================== */

#include <glm/gtc/type_ptr.hpp>

#include <fstream>
#include <sstream>

namespace ember {

namespace {

std::string readFile(const char* path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error(std::string("ember: cannot open shader file: ") + path);
    std::ostringstream ss;
    char buffer[4096];
    while (f.read(buffer, sizeof(buffer))) ss.write(buffer, f.gcount());
    ss.write(buffer, f.gcount());
    if (f.bad() || !f.eof() || !ss)
        throw std::runtime_error(std::string("ember: cannot read shader file: ") + path);
    return ss.str();
}

struct StageOwner {
    GLuint id;
    ~StageOwner() { if (id) glDeleteShader(id); }
    GLuint release() { GLuint result = id; id = 0; return result; }
};

GLuint compileStage(GLenum stage, const char* src) {
    StageOwner owner{glCreateShader(stage)};
    GLuint s = owner.id;
    if (!s) throw std::runtime_error("ember: cannot create shader");
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[8192];
        GLsizei len = 0;
        glGetShaderInfoLog(s, sizeof(log), &len, log);
        std::string msg(log, len);
        throw std::runtime_error("ember: shader compile error: " + msg);
    }
    return owner.release();
}

} // namespace

Shader::~Shader() {
    if (id_) glDeleteProgram(id_);
}

Shader::Shader(Shader&& o) noexcept : id_(o.id_), locs_(std::move(o.locs_)),
                                      uniforms_(std::move(o.uniforms_)) { o.id_ = 0; }

Shader& Shader::operator=(Shader&& o) noexcept {
    if (this != &o) {
        if (id_) glDeleteProgram(id_);
        id_ = o.id_;
        o.id_ = 0;
        locs_ = std::move(o.locs_);
        uniforms_ = std::move(o.uniforms_);
    }
    return *this;
}

Shader Shader::fromSources(const std::vector<std::pair<GLenum, const char*>>& stages) {
    GLuint prog = glCreateProgram();
    std::vector<GLuint> shaders;
    try {
        for (const auto& [stage, src] : stages) {
            StageOwner shader{compileStage(stage, src)};
            shaders.push_back(shader.id);
            glAttachShader(prog, shader.release());
        }
        glLinkProgram(prog);
        GLint ok = 0;
        glGetProgramiv(prog, GL_LINK_STATUS, &ok);
        if (!ok) {
            char log[8192];
            GLsizei len = 0;
            glGetProgramInfoLog(prog, sizeof(log), &len, log);
            throw std::runtime_error(std::string("ember: program link error: ") + std::string(log, len));
        }
    } catch (...) {
        for (GLuint s : shaders) glDeleteShader(s);
        glDeleteProgram(prog);
        throw;
    }
    for (GLuint s : shaders) glDeleteShader(s);
    return Shader(prog);
}

Shader Shader::fromSources(std::initializer_list<std::pair<GLenum, const char*>> stages) {
    return fromSources(std::vector<std::pair<GLenum, const char*>>(stages));
}

Shader Shader::fromFiles(std::initializer_list<std::pair<GLenum, const char*>> paths) {
    std::vector<std::pair<GLenum, std::string>> srcs;
    srcs.reserve(paths.size());
    for (const auto& [stage, path] : paths) srcs.emplace_back(stage, readFile(path));
    std::vector<std::pair<GLenum, const char*>> views;
    views.reserve(srcs.size());
    for (const auto& [stage, src] : srcs) views.emplace_back(stage, src.c_str());
    return fromSources(views);
}

Shader Shader::fromVertFrag(const char* vertPath, const char* fragPath) {
    return fromFiles({{GL_VERTEX_SHADER, vertPath}, {GL_FRAGMENT_SHADER, fragPath}});
}

Shader Shader::fromCompute(const char* compPath) {
    return fromFiles({{GL_COMPUTE_SHADER, compPath}});
}

GLint Shader::location(const char* name) const {
    std::string key(name);
    auto it = locs_.find(key);
    if (it != locs_.end()) return it->second;
    GLint l = glGetUniformLocation(id_, name);
    locs_.emplace(std::move(key), l);
    return l;
}

bool Shader::hasUniform(const char* name) const {
    std::string key(name);
    auto it = uniforms_.find(key);
    if (it != uniforms_.end()) return it->second;
    // Program-interface query sees std140 block members too (blocks declared
    // without an instance name expose plain member names).
    const bool found = glGetProgramResourceIndex(id_, GL_UNIFORM, name) != GL_INVALID_INDEX;
    uniforms_.emplace(std::move(key), found);
    return found;
}

void Shader::setInt(const char* n, int v)   { glUniform1i(location(n), v); }
void Shader::setUint(const char* n, unsigned v) { glUniform1ui(location(n), v); }
void Shader::setFloat(const char* n, float v)   { glUniform1f(location(n), v); }
void Shader::setVec2(const char* n, const glm::vec2& v) { glUniform2fv(location(n), 1, glm::value_ptr(v)); }
void Shader::setVec3(const char* n, const glm::vec3& v) { glUniform3fv(location(n), 1, glm::value_ptr(v)); }
void Shader::setVec4(const char* n, const glm::vec4& v) { glUniform4fv(location(n), 1, glm::value_ptr(v)); }
void Shader::setMat4(const char* n, const glm::mat4& v) { glUniformMatrix4fv(location(n), 1, GL_FALSE, glm::value_ptr(v)); }

} // namespace ember
/* ============================================================================
 * [17] backends/opengl/shaders.cpp
 * backends/opengl/shaders.cpp
 * ========================================================================== */

namespace ember::detail::opengl {
const char* const kScheduleComp = R"GLSL(#version 430 core
// Internal scheduler: never overridden via setShaderDirectory. All commands
// are bounded by capacity, which the host validates against GL device limits.
layout(local_size_x=1) in;
layout(binding=4,std430) buffer Counters {
    uint alive; uint deadHead; uint requests; uint capacity; uint allocated;
};
layout(binding=10,std430) buffer Draw {
    uint vertexCount; uint instanceCount; uint firstVertex; uint baseInstance;
};
// words[0]=inputAlive, [1]=padded sort size, [2..4]=integration command,
// [5..]=three-word tiled sort commands in host submission order.
layout(binding=13,std430) buffer Schedule { uint words[]; };
// Event queue header (WO-08): reset here alongside the other frame counters.
layout(binding=22,std430) buffer EventQueue { uint head; uint totalChildren; };
// CPU mirror: ScheduleParams in src/backends/opengl/params.hpp.
layout(binding = 16, std140) uniform ScheduleParams {
    int uSchedulePhase; uint uRequestCount;
};
uint padded(uint n) {
    uint p=1u; while(p<n) p<<=1u; return p;
}
void command(uint offset,uint x) {words[offset]=x;words[offset+1u]=1u;words[offset+2u]=1u;}
void main() {
    uint count=min(alive,capacity);
    if(uSchedulePhase==0) {
        words[0]=count;
        command(2u,(count+63u)/64u);
        requests=uRequestCount;
        vertexCount=4u;instanceCount=0u;firstVertex=0u;baseInstance=0u;
        head=0u;totalChildren=0u;
    } else {
        uint n=padded(count),maxN=padded(capacity);
        words[1]=n;
        uint groups=count==0u?0u:(n+255u)/256u;
        command(5u,groups);
        uint offset=8u;
        for(uint k=512u;k<=maxN;k<<=1u) {
            uint stageGroups=k<=n?groups:0u;
            for(uint j=k>>1u;j>=256u;j>>=1u) {command(offset,stageGroups);offset+=3u;}
            command(offset,stageGroups);offset+=3u;
        }
    }
}
)GLSL";

const char* const kSimulateComp = R"GLSL(#version 430 core
// Stable slots: phase 0 integrates live indices; phase 3 reserves spawn slots;
// phase 1 spawns; phase 2 publishes draw args. Live lists are ping-ponged.
// GPU scheduling: uGpuDriven=1 reads input count from binding 13 word 0;
// uGpuDriven=0 uses uInputAlive. schedule.comp resets draw/request counters
// before phase 0. Keep both uniforms active for host protocol detection.
layout(local_size_x = 64) in;

struct Particle {
    vec4 pos;   // xyz position, w = size
    vec4 vel;   // xyz velocity, w = age
    vec4 life;  // x = lifetime (negative => corpse slot owned by the free stack)
    vec4 color; // rgba
};

layout(binding = 0, std430) buffer BufCur   { Particle cur[]; };
layout(binding = 1, std430) buffer BufNext  { Particle nxt[]; };
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
    int   flags;         // bit0 = fade-to-color, bit1 = refractive (sign-encoded in pos.w)
    uint  base;          // prefix offset: spawns work ids [base, base+count)
    uint  count;
    int   paletteCount;  // colors in this palette range
    vec3  fadeMin;       // fade target range (RGB)
    uint  pad2;
    vec3  fadeMax;
    uint  pad3;
};
layout(binding = 2, std430) readonly  buffer BufSpawnReq { SpawnRequest req[]; };
// Event sub-emission (WO-08): per-slot metadata, templates and a bounded GPU
// queue. Slot metadata is one uvec2: x = event tag (pad2 of the request),
// y = birth id — a slot-independent genealogy hash. CPU spawns take
// uFrameSeed ^ (j*phi); event children take parentBirth ^ (k*phi). Event seeds
// derive from the PARENT'S BIRTH ID, never the slot — slot identity permutes
// under free-stack recycling, which would otherwise make grandchildren values
// nondeterministic across runs/backends. (Merged into one buffer: NVIDIA GL
// exposes only 16 compute SSBO blocks, and this shader is at the limit.)
layout(binding = 20, std430) buffer BufSlotMeta { uvec2 slotMeta[]; };
layout(binding = 21, std430) readonly buffer BufEventTemplates { SpawnRequest eventTemplates[]; };
struct GpuEvent {          // std430, 48 B
    vec3 pos;
    uint templateIdx;
    vec3 vel;
    uint parentBirth;      // parent's birth id (slot-independent, see BufBirthIds)
    uint base;             // filled by phase 4 (0xFFFFFFFF = budget exhausted)
    uint pad0;
    uint pad1;
    uint pad2;
};
layout(binding = 22, std430) coherent buffer BufEventQueue {
    uint eventHead;
    uint eventTotalChildren;
    uint eventPad0;
    uint eventPad1;
    GpuEvent events[];
};
const uint kMaxEventInstances = 1024u;
const uint kMaxEventChildren = 4096u;
layout(binding = 9, std430) readonly  buffer BufPalette { vec4 palette[]; };
layout(binding = 10, std430) buffer BufIndirectArgs { // uPhase=2 write (draw count)
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
    uint uAllocated; // stable occupied-slot extent, changed only during spawning
    uint uSpawnReuse; // phase 3 -> phase 1: recycled portion of this batch
    uint uSpawnAppendBase;
    uint uSpawnAccepted;
};
layout(binding = 11, std430) readonly buffer BufLive { uint liveIndices[]; };
layout(binding = 12, std430) writeonly buffer BufNextLive { uint nextLiveIndices[]; };
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

// All scalar state lives in one std140 block (no loose uniforms: the same
// source must compile to SPIR-V for Vulkan). CPU mirror: SimParams in
// src/backends/opengl/params.hpp. Force switches: bit i corresponds to
// ember::Force enum value i (0 gravity .. 9 boundary).
layout(binding = 14, std140) uniform SimParams {
    vec3  uGravity;          float uDrag;
    vec3  uWind;             float uTurbulence;
    vec3  uNoiseWindDir;     float uNoiseWindAmp;
    float uNoiseWindScale;   float uNoiseWindSpeed;
    float uWaveAmp;          float uWaveOmega;
    vec3  uWaveDir;          float uDt;
    vec3  uWaveK;            float uTime;
    uint  uForceMask;        int   uDragMode;      // 0 = linear, 1 = quadratic
    int   uAttractorCount;   int   uVortexCount;
    int   uSpringCount;      int   uBoundaryMode;  // 1 = kill, 2 = bounce (mask bit 9)
    float uBoundaryY;        float uRestitution;
    uint  uSpawnTotal;       uint  uFrameSeed;     // phase-1 request total / RNG seed
    uint  uInputAlive;       int   uGpuDriven;     // population at update start / mode
    int   uPhase;            uint  uEventTemplates; int uPad1; int uPad2;
};
layout(binding=13,std430) readonly buffer Schedule { uint scheduleWords[]; };

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

// ---- shared sampling (mirrors the CPU Emitter::spawn formulas) ----
Particle sampleParticle(SpawnRequest r, uint s) {
    Particle p;
    float size = rngRange(s, r.sizeMin, r.sizeMax);
    p.pos = vec4(samplePosition(s, r), (r.flags & 2) != 0 ? -size : size);
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
    if ((r.flags & 1) != 0) p.life.yzw = rngVec3(s, r.fadeMin, r.fadeMax);
    else p.life.yzw = p.color.rgb;
    return p;
}

// Phase 3 reserves a disjoint slot range once for the whole dispatch. No CAS
// retries or per-particle global allocator atomics in the spawn hot path.
uint spawnSlot(uint j) {
    return j < uSpawnReuse ? dead[uDeadHead + uSpawnReuse - 1u - j]
                           : uSpawnAppendBase + j - uSpawnReuse;
}
void writeSpawned(uint j, uint slot, Particle p, uint tag, uint birth) {
    slotMeta[slot] = uvec2(tag, birth); // persisted for onDeath/onBounce (chaining included)
    nxt[slot] = p;
    nextLiveIndices[indirectArgs.instanceCount + j] = slot;
}

void spawnFromRequest(uint j) {
    if (j >= uSpawnAccepted) return;
    // Requests are sorted by ascending base — binary search the owner.
    int lo = 0, hi = int(uSpawnRequestCount) - 1;
    while (lo < hi) {
        int mid = (lo + hi + 1) >> 1;
        if (req[mid].base <= j) lo = mid;
        else hi = mid - 1;
    }
    SpawnRequest r = req[lo];
    uint birth = uFrameSeed ^ (j * 0x9E3779B9u);
    writeSpawned(j, spawnSlot(j), sampleParticle(r, birth), r.pad2, birth);
}

// Events triggered during phase 0 are appended to a bounded queue; phase 4
// computes the child slot prefix and phase 1 samples the children.
void enqueueEvent(vec3 pos, vec3 vel, uint templateIdx, uint parentBirth) {
    uint q = atomicAdd(eventHead, 1u);
    if (q < kMaxEventInstances) {
        events[q].pos = pos;
        events[q].vel = vel;
        events[q].templateIdx = templateIdx;
        events[q].parentBirth = parentBirth;
    }
}

void spawnFromEvent(uint j) {
    if (j >= uSpawnAccepted || uEventTemplates == 0u) return;
    uint e = j - uSpawnTotal;
    uint head = min(eventHead, kMaxEventInstances);
    if (head == 0u) return;
    // Instance table: base is non-decreasing; exhausted entries are sentinels.
    int lo = 0, hi = int(head) - 1;
    while (lo < hi) {
        int mid = (lo + hi + 1) >> 1;
        if (events[mid].base <= e) lo = mid;
        else hi = mid - 1;
    }
    GpuEvent inst = events[lo];
    if (inst.base == 0xFFFFFFFFu) return;
    SpawnRequest r = eventTemplates[inst.templateIdx];
    uint k = e - inst.base;
    if (k >= r.count) return;
    r.position = inst.pos; // children are born at the parent's death point
    uint birth = inst.parentBirth ^ (k * 0x9E3779B9u);
    Particle p = sampleParticle(r, birth);
    p.vel.xyz += inst.vel * uintBitsToFloat(r.pad3);
    writeSpawned(j, spawnSlot(j), p, r.pad2, birth);
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

// Kill a particle: queue an onDeath event, recycle its slot, shrink the alive
// count, leave a corpse marker. `emitDeath` is false for the NaN guard (which
// must not produce events); lifetime and boundary kills pass true.
void retire(uint i, bool emitDeath) {
    if (emitDeath && uEventTemplates != 0u) {
        const uvec2 meta = slotMeta[i];
        if ((meta.x & 0xFFFFu) != 0u && (meta.x & 0xFFFFu) - 1u < uEventTemplates) {
            Particle p = cur[i];
            enqueueEvent(p.pos.xyz, p.vel.xyz, (meta.x & 0xFFFFu) - 1u, meta.y);
        }
    }
    uint h = atomicAdd(uDeadHead, 1u);
    dead[h % uCapacity] = i;
    atomicAdd(uAlive, -1u);
    nxt[i] = Particle(vec4(0.0), vec4(0.0), vec4(-1.0), vec4(0.0));
    // Keep both buffers' tombstones valid for slot-order readback and for
    // switching to a legacy custom shader that scans the allocated extent.
    cur[i] = nxt[i];
}

bool simulate(uint i) {
    Particle p = cur[i];
    if (p.life.x < 0.0) return false;
    // NaN guard: a NaN particle is retired instead of poisoning the buffer
    // (NaN positions can also stall some drivers during rasterization).
    if (isnan(p.pos.x) || isnan(p.pos.y) || isnan(p.pos.z) ||
        isnan(p.vel.x) || isnan(p.vel.y) || isnan(p.vel.z)) {
        retire(i, false);
        return false;
    }
    p.vel.w += uDt;                      // age
    if (p.vel.w >= p.life.x) {
        retire(i, true);
        return false;
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
    if ((uForceMask & (1u << 7u)) != 0u && uNoiseWindAmp != 0.0) {        // signed spatial noise wind
        float n = snoise3(p.pos.xyz * uNoiseWindScale + uTime * uNoiseWindSpeed);
        acc += uNoiseWindDir * (n * 2.0 - 1.0) * uNoiseWindAmp;
    }
    if ((uForceMask & (1u << 8u)) != 0u && uWaveAmp != 0.0) {             // signed traveling wave
        float phase = dot(p.pos.xyz, uWaveK) + uTime * uWaveOmega;
        acc += uWaveDir * (sin(phase) * uWaveAmp);
    }

    // Semi-implicit Euler integration.
    p.vel.xyz += acc * uDt;
    p.vel.xyz = clamp(p.vel.xyz, vec3(-500.0), vec3(500.0)); // guard BEFORE displacement
    p.pos.xyz += p.vel.xyz * uDt;

    // ---- boundary plane (y < uBoundaryY) -----------------------------------
    if ((uForceMask & (1u << 9u)) != 0u && uBoundaryMode != 0 && p.pos.y < uBoundaryY) {
        if (uBoundaryMode == 1) {          // kill
            retire(i, true);
            return false;
        }
        p.pos.y = uBoundaryY;              // bounce
        if (p.vel.y < 0.0) p.vel.y = -p.vel.y * uRestitution;
        if (abs(p.vel.y) < 0.1) p.vel.y = 0.0; // settle on the plane
        if (uEventTemplates != 0u) {       // onBounce: every contact triggers
            const uvec2 meta = slotMeta[i];
            const uint onBounce = meta.x >> 16;
            if (onBounce != 0u && onBounce - 1u < uEventTemplates)
                enqueueEvent(p.pos.xyz, p.vel.xyz, onBounce - 1u, meta.y);
        }
    }
    nxt[i] = p;
    return true;
}

void main() {
    if (uPhase == 0) {
        uint j = gl_GlobalInvocationID.x;
        if (j >= (uGpuDriven != 0 ? scheduleWords[0] : uInputAlive)) return;
        uint i = liveIndices[j];
        if (simulate(i)) {
            uint drawIndex = atomicAdd(indirectArgs.instanceCount, 1u);
            nextLiveIndices[drawIndex] = i;
        }
    } else if (uPhase == 4) {
        // Serial prefix sum over this frame's event instances (<= 1024 adds);
        // exhausting the child budget marks later instances as sentinels.
        if (gl_GlobalInvocationID.x == 0u) {
            uint head = min(eventHead, kMaxEventInstances);
            uint running = 0u;
            for (uint q = 0u; q < head; ++q) {
                if (running >= kMaxEventChildren) {
                    events[q].base = 0xFFFFFFFFu;
                    continue;
                }
                events[q].base = running;
                running = min(running + eventTemplates[events[q].templateIdx].count,
                              kMaxEventChildren);
            }
            eventTotalChildren = running;
        }
    } else if (uPhase == 3 && gl_GlobalInvocationID.x == 0u) {
        // Integration has finished and no producer/consumer overlaps this
        // reservation. Recycle first, then append; bounded without atomics.
        // Event children are part of the same batch and share the accounting.
        uint batchTotal = uSpawnTotal + eventTotalChildren;
        uSpawnReuse = min(batchTotal, uDeadHead);
        uSpawnAppendBase = uAllocated;
        uint appendCount = min(batchTotal - uSpawnReuse, uCapacity - uAllocated);
        uSpawnAccepted = uSpawnReuse + appendCount;
        uDeadHead -= uSpawnReuse;
        uAllocated += appendCount;
        uAlive += uSpawnAccepted;
    } else if (uPhase == 1) {
        uint j = gl_GlobalInvocationID.x;
        if (j < uSpawnTotal) spawnFromRequest(j);
        else spawnFromEvent(j);
    } else if (uPhase == 2) {
        if (gl_GlobalInvocationID.x == 0u) indirectArgs.instanceCount = uAlive;
    }
}
)GLSL";

const char* const kParticleVert = R"GLSL(#version 430 core
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
)GLSL";

const char* const kParticleFrag = R"GLSL(#version 430 core
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
)GLSL";

// GPU bitonic sort of alive particle indices (see shaders/sort.comp).
const char* const kSortComp = R"GLSL(#version 430 core
// Exact back-to-front bitonic sort with cached depth keys and 256-entry tiles.
// uMode=0 + uTileMode=1: fill and sort alternating tiles in shared memory.
// uMode=1 + uTileMode=0: one global compare-exchange step (uJ >= 256).
// uMode=1 + uTileMode=2: finish uJ=128..1 in shared memory for stage uK.
// All local invocations participate in barriers, including padding lanes.
// uGpuDriven=1 reads alive from binding 4 and padded N from binding 13 word 1.
// schedule.comp provides indirect commands; inactive merge stages have X=0.
layout(local_size_x = 256) in;

struct Particle { vec4 pos; vec4 vel; vec4 life; vec4 color; };
layout(binding = 0, std430) readonly buffer BufCur { Particle cur[]; };
layout(binding = 8, std430) buffer BufSorted { uint sorted[]; };
layout(binding = 11, std430) readonly buffer BufLive { uint liveIndices[]; };
// Binding 12 is scratch: next live indices during sim, depth keys during sort.
layout(binding = 12, std430) buffer BufKeys { float keys[]; };
// Scalar state in one std140 block (Vulkan-compilable; CPU mirror: SortParams
// in src/backends/opengl/params.hpp).
layout(binding = 15, std140) uniform SortParams {
    mat4 uView;
    uint uCapacity; uint uAlive; uint uPaddedN; uint uK;
    uint uJ; uint uMode; uint uTileMode; int uGpuDriven;
};
layout(binding=4,std430) readonly buffer Counters { uint gpuAlive; };
layout(binding=13,std430) readonly buffer Schedule { uint scheduleWords[]; };
shared uint tileIndices[256];
shared float tileKeys[256];

bool greaterKey(float a, uint ai, float b, uint bi) {
    return a > b || (a == b && ai > bi); // reproducible tie-break by stable slot
}
float depthKey(uint idx) {
    if (idx >= uCapacity) return uintBitsToFloat(0x7f800000u);
    float z = (uView * vec4(cur[idx].pos.xyz, 1.0)).z;
    return isnan(z) ? uintBitsToFloat(0x7f800000u) : z;
}
void tileStep(uint lane, uint globalIndex, uint k, uint j) {
    uint other = lane ^ j;
    if (lane < other) {
        float a = tileKeys[lane], b = tileKeys[other];
        uint ai = tileIndices[lane], bi = tileIndices[other];
        bool ascending = (globalIndex & k) == 0u;
        if (ascending ? greaterKey(a, ai, b, bi) : greaterKey(b, bi, a, ai)) {
            tileKeys[lane] = b; tileKeys[other] = a;
            tileIndices[lane] = bi; tileIndices[other] = ai;
        }
    }
    barrier();
}
void main() {
    uint count = uGpuDriven != 0 ? gpuAlive : uAlive;
    uint paddedN = uGpuDriven != 0 ? scheduleWords[1] : uPaddedN;
    uint i = gl_GlobalInvocationID.x;
    uint lane = gl_LocalInvocationID.x;
    if (uTileMode == 0u) {
        uint other = i ^ uJ;
        if (i >= paddedN || other <= i) return;
        float a = keys[i], b = keys[other];
        uint ai = sorted[i], bi = sorted[other];
        bool ascending = (i & uK) == 0u;
        if (ascending ? greaterKey(a, ai, b, bi) : greaterKey(b, bi, a, ai)) {
            keys[i] = b; keys[other] = a;
            sorted[i] = bi; sorted[other] = ai;
        }
        return;
    }
    uint idx = uCapacity;
    float key = uintBitsToFloat(0x7f800000u);
    if (uMode == 0u) {
        idx = i < count ? liveIndices[i] : uCapacity;
        key = depthKey(idx);
    } else if (i < paddedN) {
        idx = sorted[i]; key = keys[i];
    }
    tileIndices[lane] = idx;
    tileKeys[lane] = key;
    barrier();
    if (uMode == 0u) {
        for (uint k = 2u; k <= 256u; k <<= 1u)
            for (uint j = k >> 1u; j > 0u; j >>= 1u) tileStep(lane, i, k, j);
    } else {
        for (uint j = 128u; j > 0u; j >>= 1u) tileStep(lane, i, uK, j);
    }
    if (i < paddedN) {
        sorted[i] = tileIndices[lane];
        keys[i] = tileKeys[lane];
    }
}
)GLSL";

}
/* ============================================================================
 * [18] backends/opengl/resources.cpp
 * backends/opengl/resources.cpp
 * ========================================================================== */

#ifdef EMBER_USE_STB
#include "stb_image.h"
#endif // EMBER_USE_STB

#ifdef EMBER_USE_STB
#endif
#include <cstddef>
#include <cstdio>

namespace ember::detail::opengl {
OpenGLBackend::OpenGLBackend()
    : bufA_(GL_SHADER_STORAGE_BUFFER),
      bufB_(GL_SHADER_STORAGE_BUFFER),
      spawnBuf_(GL_SHADER_STORAGE_BUFFER),
      paletteBuf_(GL_SHADER_STORAGE_BUFFER),
      deadBuf_(GL_SHADER_STORAGE_BUFFER),
      counterBuf_(GL_SHADER_STORAGE_BUFFER),
      attractorBuf_(GL_SHADER_STORAGE_BUFFER),
      vortexBuf_(GL_SHADER_STORAGE_BUFFER),
      springBuf_(GL_SHADER_STORAGE_BUFFER),
      sortedBuf_(GL_SHADER_STORAGE_BUFFER),
      liveBuf_(GL_SHADER_STORAGE_BUFFER),
      nextLiveBuf_(GL_SHADER_STORAGE_BUFFER),
      sortKeyBuf_(GL_SHADER_STORAGE_BUFFER),
      indirectBuf_(GL_SHADER_STORAGE_BUFFER),
      dispatchBuf_(GL_SHADER_STORAGE_BUFFER),
      eventTagBuf_(GL_SHADER_STORAGE_BUFFER),
      eventTemplateBuf_(GL_SHADER_STORAGE_BUFFER),
      eventQueueBuf_(GL_SHADER_STORAGE_BUFFER),
      curvesBuf_(GL_SHADER_STORAGE_BUFFER),
      uboSim_(GL_UNIFORM_BUFFER),
      uboSort_(GL_UNIFORM_BUFFER),
      uboSchedule_(GL_UNIFORM_BUFFER),
      uboDraw_(GL_UNIFORM_BUFFER),
      uboFrag_(GL_UNIFORM_BUFFER),
      uboBloom_(GL_UNIFORM_BUFFER) {}

void OpenGLBackend::initialize(std::uint32_t capacity,bool debug) {
    capacity_=capacity;debug_=debug;
    if (capacity_ == 0 || capacity_ > (1u << 30))
        throw std::invalid_argument("ember: capacity must be in [1, 2^30]");
    cur_ = &bufA_;
    nxt_ = &bufB_;

    uploadBuiltinSprite();

    if (debug_) {
        std::fprintf(stderr, "[ember] GL_VERSION=%s GL_RENDERER=%s\n",
                     (const char*)glGetString(GL_VERSION), (const char*)glGetString(GL_RENDERER));
    }

    const GLsizeiptr pbytes = (GLsizeiptr)capacity_ * (GLsizeiptr)sizeof(Particle);
    std::vector<std::byte> zeros(pbytes);
    bufA_.data(zeros.data(), pbytes, GL_DYNAMIC_COPY);
    bufB_.data(zeros.data(), pbytes, GL_DYNAMIC_COPY);
    spawnBuf_.data(nullptr, (GLsizeiptr)kMaxSpawnRequests * (GLsizeiptr)sizeof(SpawnRequest), GL_DYNAMIC_DRAW);
    paletteBuf_.data(nullptr, 0, GL_DYNAMIC_DRAW);
    deadBuf_.data(nullptr, (GLsizeiptr)capacity_ * (GLsizeiptr)sizeof(GLuint), GL_DYNAMIC_DRAW);
    const GLuint ctr[8] = {0, 0, 0, capacity_, 0, 0, 0, 0};
    counterBuf_.data(ctr, sizeof(ctr), GL_DYNAMIC_DRAW);
    attractorBuf_.data(nullptr, 0, GL_DYNAMIC_DRAW);
    vortexBuf_.data(nullptr, 0, GL_DYNAMIC_DRAW);
    springBuf_.data(nullptr, 0, GL_DYNAMIC_DRAW);
    liveBuf_.data(nullptr, (GLsizeiptr)capacity_ * sizeof(GLuint), GL_DYNAMIC_DRAW);
    nextLiveBuf_.data(nullptr, (GLsizeiptr)capacity_ * sizeof(GLuint), GL_DYNAMIC_DRAW);
    sortedBuf_.data(nullptr, (GLsizeiptr)nextPow2(capacity_) * (GLsizeiptr)sizeof(GLuint), GL_DYNAMIC_DRAW);
    const GLuint indirectInit[4] = {4, 0, 0, 0}; // {vertexCount, instanceCount, first, baseInstance}
    indirectBuf_.data(indirectInit, sizeof(indirectInit), GL_DYNAMIC_DRAW);
    eventTagBuf_.data(nullptr, (GLsizeiptr)capacity_ * 2 * sizeof(GLuint), GL_DYNAMIC_DRAW); // uvec2: tag + birth id
    eventTemplateBuf_.data(nullptr, (GLsizeiptr)maxEventTemplates * (GLsizeiptr)sizeof(SpawnRequest), GL_DYNAMIC_DRAW);
    eventQueueBuf_.data(nullptr, (GLsizeiptr)kEventQueueBytes, GL_DYNAMIC_DRAW);
    const GLuint queueZero[4] = {0, 0, 0, 0};
    eventQueueBuf_.subData(0, sizeof(queueZero), queueZero);
    // Lifecycle curves default to all-ones (disabled) so the shader multiply is
    // bit-identical to the plain path until the facade uploads real keys.
    CurvesParams curves;
    for (auto& c : curves.colorLut) c = glm::vec4(1.f);
    for (auto& s : curves.sizeLut) s = 1.f;
    curvesBuf_.data(&curves, sizeof(curves), GL_DYNAMIC_DRAW);

    // std140 parameter blocks for the built-in shaders (bindings 14-19).
    uboSim_.data(nullptr, sizeof(SimParams), GL_DYNAMIC_DRAW);
    uboSort_.data(nullptr, sizeof(SortParams), GL_DYNAMIC_DRAW);
    uboSchedule_.data(nullptr, sizeof(ScheduleParams), GL_DYNAMIC_DRAW);
    uboDraw_.data(nullptr, sizeof(DrawParams), GL_DYNAMIC_DRAW);
    uboFrag_.data(nullptr, sizeof(FragParams), GL_DYNAMIC_DRAW);
    uboBloom_.data(nullptr, sizeof(BloomParams), GL_DYNAMIC_DRAW);

    // Instanced quads: no vertex attributes — the VS expands gl_VertexID.
    vao_.bind();
    vao_.unbind();

    ensurePrograms();
}

OpenGLBackend::~OpenGLBackend() {
    for (auto fence : statisticsFences_) if (fence) glDeleteSync(fence);
    glDeleteBuffers((GLsizei)statisticsBuffers_.size(), statisticsBuffers_.data());
    if (bloomFbo_) {
        glDeleteFramebuffers(1, &bloomFbo_);
        glDeleteFramebuffers(1, &bloomHalfFbo_);
        glDeleteFramebuffers(1, &bloomBlurFbo_);
    }
}

void OpenGLBackend::setSpriteTexture(const char* pngPath) {
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

void OpenGLBackend::uploadBuiltinSprite() {
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

void OpenGLBackend::setShaderDirectory(const char* dir) { shaderDir_ = dir ? dir : ""; }

void OpenGLBackend::ensurePrograms() {
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

void OpenGLBackend::setPrograms(Shader render, Shader simulate) {
    if (gpuDriven_ && (!simulate.hasUniform("uGpuDriven") || !simulate.hasUniform("uInputAlive")))
        throw std::invalid_argument("ember: incompatible simulation shader in GPU-driven mode");
    if (allocated_ && simulate.hasUniform("uInputAlive") &&
        !simProg_.hasUniform("uInputAlive")) {
        // A legacy integrator may leave the previous frame's live contents
        // in inactive slots of nxt_. Live-only traversal will never touch
        // those slots again, so initialize its tombstones once on transition.
        glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
        glBindBuffer(GL_COPY_READ_BUFFER, cur_->id());
        glBindBuffer(GL_COPY_WRITE_BUFFER, nxt_->id());
        glCopyBufferSubData(GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER, 0, 0,
                           (GLsizeiptr)allocated_ * sizeof(Particle));
    }
    renderProg_ = std::move(render);
    simProg_ = std::move(simulate);
}

void OpenGLBackend::uploadAttractors(const std::vector<Attractor>& a) {
    std::vector<glm::vec4> v;
    v.reserve(a.size());
    for (const auto& at : a) v.emplace_back(at.position, at.strength);
    attractorBuf_.data(v.empty() ? nullptr : v.data(),
                       (GLsizeiptr)(v.size() * sizeof(glm::vec4)), GL_DYNAMIC_DRAW);
}

void OpenGLBackend::uploadVortexes(const std::vector<Vortex>& v) {
    vortexBuf_.data(v.empty() ? nullptr : v.data(),
                    (GLsizeiptr)(v.size() * sizeof(Vortex)), GL_DYNAMIC_DRAW);
}

void OpenGLBackend::uploadSprings(const std::vector<Spring>& s) {
    springBuf_.data(s.empty() ? nullptr : s.data(),
                    (GLsizeiptr)(s.size() * sizeof(Spring)), GL_DYNAMIC_DRAW);
}

void OpenGLBackend::uploadPalette(const std::vector<glm::vec4>& values) {
    paletteBuf_.data(values.empty()?nullptr:values.data(),GLsizeiptr(values.size()*sizeof(glm::vec4)),GL_DYNAMIC_DRAW);
}

void OpenGLBackend::uploadEventTemplates(const std::vector<SpawnRequest>& templates) {
    if (templates.empty()) {
        eventTemplateCount_ = 0;
        eventTemplateBuf_.data(nullptr, 0, GL_DYNAMIC_DRAW); // release; empty never throws
        return;
    }
    // Legacy custom simulation shaders predate the event protocol; reject them
    // rather than feeding a buffer the shader cannot address.
    ensurePrograms();
    if (simProg_ && glGetProgramResourceIndex(simProg_.id(), GL_SHADER_STORAGE_BLOCK,
                                              "BufEventQueue") == GL_INVALID_INDEX)
        throw std::invalid_argument("ember: custom simulation shader lacks the event queue protocol");
    eventTemplateCount_ = (std::uint32_t)templates.size();
    eventTemplateBuf_.data(templates.data(),
                           (GLsizeiptr)templates.size() * (GLsizeiptr)sizeof(SpawnRequest),
                           GL_DYNAMIC_DRAW);
}

void OpenGLBackend::uploadLifeCurves(const LifeCurvesLut& lut) {
    CurvesParams curves;
    std::copy(lut.color, lut.color + kLifeCurveLutSize, curves.colorLut);
    std::copy(lut.size, lut.size + kLifeCurveLutSize, curves.sizeLut);
    curvesBuf_.subData(0, sizeof(curves), &curves);
}
void OpenGLBackend::validateConfiguration(std::uint32_t capacity,bool sorting) {
    if(capacity==0 || capacity>(1u<<30)) throw std::invalid_argument("ember: invalid capacity");
    if(gpuDriven_) {
        validateGpuCapacity(capacity);
        if(sorting) {
            ensureSortProgram();
            if(!sortProg_ || !sortProg_.hasUniform("uGpuDriven") || !sortProg_.hasUniform("uTileMode"))
                throw std::invalid_argument("ember: GPU-driven mode requires the GPU scheduling sort protocol");
        }
    }
}
void OpenGLBackend::resize(std::uint32_t capacity) {
    validateConfiguration(capacity,sortEnabled_);
    capacity_ = capacity;
    allocated_ = 0;
    alive_ = 0;
    const GLsizeiptr pbytes = (GLsizeiptr)capacity_ * (GLsizeiptr)sizeof(Particle);
    std::vector<std::byte> zeros(pbytes);
    bufA_.data(zeros.data(), pbytes, GL_DYNAMIC_COPY);
    bufB_.data(zeros.data(), pbytes, GL_DYNAMIC_COPY);
    deadBuf_.data(nullptr, (GLsizeiptr)capacity_ * (GLsizeiptr)sizeof(GLuint), GL_DYNAMIC_DRAW);
    liveBuf_.data(nullptr, (GLsizeiptr)capacity_ * sizeof(GLuint), GL_DYNAMIC_DRAW);
    nextLiveBuf_.data(nullptr, (GLsizeiptr)capacity_ * sizeof(GLuint), GL_DYNAMIC_DRAW);
    sortKeyBuf_.data(nullptr, 0, GL_DYNAMIC_DRAW); // lazy resize on next sort
    sortKeyCapacity_ = 0;
    sortedBuf_.data(nullptr, (GLsizeiptr)nextPow2(capacity_) * (GLsizeiptr)sizeof(GLuint), GL_DYNAMIC_DRAW);
    eventTagBuf_.data(nullptr, (GLsizeiptr)capacity_ * 2 * sizeof(GLuint), GL_DYNAMIC_DRAW); // uvec2: tag + birth id
    const GLuint ctr[8] = {0, 0, 0, capacity_, 0, 0, 0, 0};
    counterBuf_.data(ctr, sizeof(ctr), GL_DYNAMIC_DRAW);
    const GLuint args[4]={4,0,0,0};
    indirectBuf_.data(args,sizeof(args),GL_DYNAMIC_DRAW);
    const GLuint queueZero[4] = {0, 0, 0, 0};
    eventQueueBuf_.subData(0, sizeof(queueZero), queueZero);
    resetStatistics();
    if (gpuDriven_ && sortEnabled_) scheduleGpu(1);
}
void OpenGLBackend::clear() {
    alive_ = 0;
    allocated_ = 0;
    const GLsizeiptr pbytes = (GLsizeiptr)capacity_ * (GLsizeiptr)sizeof(Particle);
    std::vector<std::byte> zeros(pbytes);
    bufA_.data(zeros.data(), pbytes, GL_DYNAMIC_COPY);
    bufB_.data(zeros.data(), pbytes, GL_DYNAMIC_COPY);
    const GLuint ctr[8] = {0, 0, 0, capacity_, 0, 0, 0, 0};
    counterBuf_.data(ctr, sizeof(ctr), GL_DYNAMIC_DRAW);
    const GLuint args[4]={4,0,0,0};
    indirectBuf_.data(args,sizeof(args),GL_DYNAMIC_DRAW);
    eventTagBuf_.data(nullptr, (GLsizeiptr)capacity_ * 2 * sizeof(GLuint), GL_DYNAMIC_DRAW); // uvec2: tag + birth id
    const GLuint queueZero[4] = {0, 0, 0, 0};
    eventQueueBuf_.subData(0, sizeof(queueZero), queueZero);
    resetStatistics();
    if (gpuDriven_ && sortEnabled_) scheduleGpu(1);
}
} // namespace ember::detail::opengl
/* ============================================================================
 * [19] backends/opengl/statistics.cpp
 * backends/opengl/statistics.cpp
 * ========================================================================== */

namespace ember::detail::opengl {
void OpenGLBackend::validateGpuCapacity(std::uint32_t capacity) const {
    GLint limit=0;
    glGetIntegeri_v(GL_MAX_COMPUTE_WORK_GROUP_COUNT,0,&limit);
    if ((capacity+63ull)/64ull > (std::uint64_t)limit ||
        (nextPow2(capacity)+255ull)/256ull > (std::uint64_t)limit)
        throw std::invalid_argument("ember: capacity exceeds GPU indirect dispatch limits");
}

void OpenGLBackend::scheduleGpu(int phase, std::uint32_t requests) {
    glUseProgram(scheduleProg_.id());
    scheduleProg_.setInt("uSchedulePhase",phase);
    scheduleProg_.setUint("uRequestCount",requests);
    const ScheduleParams params{phase, requests, 0u, 0u};
    uboSchedule_.subData(0, sizeof(params), &params);
    glBindBufferBase(GL_UNIFORM_BUFFER, kBindingUboSchedule, uboSchedule_.id());
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER,kBindingCounters,counterBuf_.id());
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER,kBindingIndirect,indirectBuf_.id());
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER,kBindingSchedule,dispatchBuf_.id());
    // schedule.comp resets the event queue header (binding 22) in phase 0.
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER,kBindingEventQueue,eventQueueBuf_.id());
    glDispatchCompute(1,1,1);
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_COMMAND_BARRIER_BIT);
}

void OpenGLBackend::setGpuDriven(bool enabled) {
    if (enabled==gpuDriven_) return;
    if (enabled) {
        validateGpuCapacity(capacity_);
        if (!simProg_.hasUniform("uInputAlive") || !simProg_.hasUniform("uGpuDriven"))
            throw std::invalid_argument("ember: GPU-driven mode requires the GPU scheduling simulation protocol");
        if (sortEnabled_) {
            ensureSortProgram();
            if (!sortProg_ || !sortProg_.hasUniform("uGpuDriven") || !sortProg_.hasUniform("uTileMode"))
                throw std::invalid_argument("ember: GPU-driven mode requires the GPU scheduling sort protocol");
        }
        if (!scheduleProg_) scheduleProg_=Shader::fromSources({{GL_COMPUTE_SHADER,kScheduleComp}});
        dispatchBuf_.data(nullptr,kScheduleBytes,GL_DYNAMIC_COPY);
        if (!statisticsBuffers_[0]) {
            glGenBuffers((GLsizei)statisticsBuffers_.size(),statisticsBuffers_.data());
            for (auto buffer:statisticsBuffers_) {
                glBindBuffer(GL_COPY_WRITE_BUFFER,buffer);
                glBufferData(GL_COPY_WRITE_BUFFER,5*sizeof(GLuint),nullptr,GL_STREAM_READ);
            }
        }
        // Support render immediately after enabling (without another update).
        if (sortEnabled_) scheduleGpu(1);
        gpuDriven_=true;
    } else {
        refreshCounts(); // deliberate one-time synchronization at mode switch
        spawnBuf_.data(nullptr,(GLsizeiptr)kMaxSpawnRequests*sizeof(SpawnRequest),GL_DYNAMIC_DRAW);
        gpuDriven_=false;
        resetStatistics();
    }
}

void OpenGLBackend::refreshCounts() const {
    if (countsFresh_) return;
    GLuint counters[5]{};
    glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
    glBindBuffer(GL_COPY_READ_BUFFER,counterBuf_.id());
    glGetBufferSubData(GL_COPY_READ_BUFFER,0,sizeof(counters),counters);
    alive_=std::min(counters[0],capacity_);
    allocated_=std::min(counters[4],capacity_);
    countsFresh_=true;
    statistics_={frameCount_,alive_,allocated_};
}

OpenGLBackend::Statistics OpenGLBackend::synchronizeStatistics() const {
    refreshCounts();
    return statistics_;
}

void OpenGLBackend::resetStatistics() {
    for (std::size_t i=0;i<statisticsFences_.size();++i) {
        if (statisticsFences_[i]) glDeleteSync(statisticsFences_[i]);
        statisticsFences_[i]=nullptr;
        if (statisticsBuffers_[i]) {
            // Orphan pending storage rather than reuse an in-flight copy.
            glBindBuffer(GL_COPY_WRITE_BUFFER,statisticsBuffers_[i]);
            glBufferData(GL_COPY_WRITE_BUFFER,5*sizeof(GLuint),nullptr,GL_STREAM_READ);
        }
    }
    countsFresh_=true;
    statistics_={frameCount_,alive_,allocated_};
}

OpenGLBackend::Statistics OpenGLBackend::pollStatistics() {
    // One flush is sufficient to make this context's pending samples progress;
    // repeating it for every occupied slot adds driver submission overhead.
    GLbitfield flags=GL_SYNC_FLUSH_COMMANDS_BIT;
    for (std::size_t i=0;i<statisticsFences_.size();++i) {
        if (!statisticsFences_[i]) continue;
        const GLenum ready=glClientWaitSync(statisticsFences_[i],flags,0);
        flags=0;
        if (ready==GL_WAIT_FAILED) throw std::runtime_error("ember: statistics fence failed");
        if (ready!=GL_ALREADY_SIGNALED && ready!=GL_CONDITION_SATISFIED) continue;
        GLuint counters[5]{};
        glBindBuffer(GL_COPY_READ_BUFFER,statisticsBuffers_[i]);
        glGetBufferSubData(GL_COPY_READ_BUFFER,0,sizeof(counters),counters);
        glDeleteSync(statisticsFences_[i]);statisticsFences_[i]=nullptr;
        if (statisticsFrames_[i]>statistics_.frame) {
            statistics_={statisticsFrames_[i],std::min(counters[0],capacity_),std::min(counters[4],capacity_)};
            if (statistics_.frame==frameCount_) {
                alive_=statistics_.alive;allocated_=statistics_.allocated;countsFresh_=true;
            }
        }
    }
    return statistics_;
}

void OpenGLBackend::enqueueStatistics() {
    pollStatistics();
    for (std::size_t i=0;i<statisticsFences_.size();++i) {
        if (statisticsFences_[i]) continue;
        glBindBuffer(GL_COPY_READ_BUFFER,counterBuf_.id());
        glBindBuffer(GL_COPY_WRITE_BUFFER,statisticsBuffers_[i]);
        glCopyBufferSubData(GL_COPY_READ_BUFFER,GL_COPY_WRITE_BUFFER,0,0,5*sizeof(GLuint));
        statisticsFrames_[i]=frameCount_;
        statisticsFences_[i]=glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE,0);
        if (!statisticsFences_[i]) throw std::runtime_error("ember: cannot create statistics fence");
        return;
    }
    ++droppedStatistics_; // bounded queue: skip telemetry, never block simulation
}

std::vector<Particle> OpenGLBackend::readParticles(std::uint32_t max) const {
    refreshCounts();
    const std::uint32_t n = std::min(max == 0 ? alive_ : std::min(max, alive_), capacity_);
    std::vector<Particle> out;
    if (n == 0) return out;
    std::vector<Particle> slots(allocated_);
    glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
    cur_->getSubData(0, (GLsizeiptr)slots.size() * sizeof(Particle), slots.data());
    out.reserve(n);
    for (const auto& p : slots) {
        if (p.life.x >= 0.f) out.push_back(p);
        if (out.size() == n) break;
    }
    return out;
}
} // namespace ember::detail::opengl
/* ============================================================================
 * [20] backends/opengl/simulation.cpp
 * backends/opengl/simulation.cpp
 * ========================================================================== */

namespace ember::detail::opengl {
void OpenGLBackend::update(const SimulationParameters& p,const UpdateBatch& batch) {
    const auto& reqs=batch.requests;
    const auto total=batch.total;
    const auto dt=batch.dt;
    const std::uint32_t nr = (std::uint32_t)reqs.size();
    if (nr > 0) {
        const auto bytes=(GLsizeiptr)nr * (GLsizeiptr)sizeof(SpawnRequest);
        // Rename request storage to avoid overwriting a previous frame's input.
        if (gpuDriven_) spawnBuf_.data(reqs.data(),bytes,GL_STREAM_DRAW);
        else spawnBuf_.subData(0,bytes,reqs.data());
    }
    if (gpuDriven_) scheduleGpu(0,nr);
    else {
        counterBuf_.subData(2 * sizeof(GLuint), sizeof(GLuint), &nr);
        const GLuint drawArgs[4] = {4, 0, 0, 0};
        indirectBuf_.subData(0, sizeof(drawArgs), drawArgs);
        const GLuint queueZero[4] = {0, 0, 0, 0}; // event queue header reset (GPU mode: schedule.comp)
        eventQueueBuf_.subData(0, sizeof(queueZero), queueZero);
    }

    // 2) Run the simulation.
    if (!simProg_) ensurePrograms();
    const bool livePipeline = simProg_.hasUniform("uInputAlive");
    glUseProgram(simProg_.id());
    // (Shader::set* caches uniform locations; -1 locations are no-ops)
    simProg_.setFloat("uDt", dt);
    simProg_.setFloat("uTime", batch.time);
    simProg_.setUint("uForceMask", p.forceMask);
    simProg_.setVec3("uGravity", p.gravity);
    simProg_.setFloat("uDrag", p.drag);
    simProg_.setInt("uDragMode", (int)p.dragMode);
    simProg_.setVec3("uWind", p.wind);
    simProg_.setFloat("uTurbulence", p.turbulence);
    simProg_.setInt("uAttractorCount", (int)p.attractors.size());
    simProg_.setInt("uVortexCount", (int)p.vortexes.size());
    simProg_.setInt("uSpringCount", (int)p.springs.size());
    simProg_.setVec3("uNoiseWindDir", p.noiseWindDir);
    simProg_.setFloat("uNoiseWindAmp", p.noiseWindAmp);
    simProg_.setFloat("uNoiseWindScale", p.noiseWindScale);
    simProg_.setFloat("uNoiseWindSpeed", p.noiseWindSpeed);
    simProg_.setVec3("uWaveDir", p.waveDir);
    simProg_.setVec3("uWaveK", p.waveK);
    simProg_.setFloat("uWaveAmp", p.waveAmp);
    simProg_.setFloat("uWaveOmega", p.waveOmega);
    simProg_.setInt("uBoundaryMode", p.boundaryMode == BoundaryMode::Kill ? 1 : 2);
    simProg_.setFloat("uBoundaryY", p.boundaryY);
    simProg_.setFloat("uRestitution", p.restitution);
    simProg_.setUint("uSpawnTotal", total);
    simProg_.setUint("uFrameSeed", batch.seed);
    simProg_.setUint("uInputAlive", alive_);
    simProg_.setInt("uGpuDriven",gpuDriven_?1:0);

    // Built-in shaders read one std140 block instead of the loose uniforms
    // above; both are written so custom shaders keep their legacy contract.
    SimParams sp{};
    sp.gravity = p.gravity;         sp.drag = p.drag;
    sp.wind = p.wind;               sp.turbulence = p.turbulence;
    sp.noiseWindDir = p.noiseWindDir; sp.noiseWindAmp = p.noiseWindAmp;
    sp.noiseWindScale = p.noiseWindScale; sp.noiseWindSpeed = p.noiseWindSpeed;
    sp.waveAmp = p.waveAmp;         sp.waveOmega = p.waveOmega;
    sp.waveDir = p.waveDir;         sp.dt = dt;
    sp.waveK = p.waveK;             sp.time = batch.time;
    sp.forceMask = p.forceMask;     sp.dragMode = (int)p.dragMode;
    sp.attractorCount = (std::int32_t)p.attractors.size();
    sp.vortexCount = (std::int32_t)p.vortexes.size();
    sp.springCount = (std::int32_t)p.springs.size();
    sp.boundaryMode = p.boundaryMode == BoundaryMode::Kill ? 1 : 2;
    sp.boundaryY = p.boundaryY;     sp.restitution = p.restitution;
    sp.spawnTotal = total;          sp.frameSeed = batch.seed;
    sp.inputAlive = alive_;         sp.gpuDriven = gpuDriven_ ? 1 : 0;
    sp.eventTemplates = eventTemplateCount_;
    glBindBufferBase(GL_UNIFORM_BUFFER, kBindingUboSim, uboSim_.id());
    const auto uploadPhase = [&](int phase) {
        sp.phase = phase;
        uboSim_.subData(0, sizeof(sp), &sp);
    };

    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kBindingCur, cur_->id());
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kBindingNext, nxt_->id());
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kBindingSpawn, spawnBuf_.id());
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kBindingDead, deadBuf_.id());
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kBindingCounters, counterBuf_.id());
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kBindingAttractors, attractorBuf_.id());
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kBindingVortexes, vortexBuf_.id());
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kBindingSprings, springBuf_.id());
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kBindingPalette, paletteBuf_.id());
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kBindingLive, liveBuf_.id());
    if (livePipeline) glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kBindingScratch, nextLiveBuf_.id());
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kBindingIndirect, indirectBuf_.id()); // phase 2 writes draw args
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kBindingEventTags, eventTagBuf_.id());
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kBindingEventTemplates, eventTemplateBuf_.id());
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kBindingEventQueue, eventQueueBuf_.id());

    EMBER_BENCH_BEGIN(Integrate);
    simProg_.setInt("uPhase", 0); // simulate
    uploadPhase(0);
    const auto integrationCount = livePipeline ? alive_ : allocated_;
    if (gpuDriven_) {
        glBindBuffer(GL_DISPATCH_INDIRECT_BUFFER,dispatchBuf_.id());
        glDispatchComputeIndirect(2*sizeof(GLuint));
    } else if (integrationCount > 0) {
        glDispatchCompute((integrationCount + kSimGroupSize - 1u) / kSimGroupSize, 1, 1);
    }
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);

    EMBER_BENCH_END(Integrate);
    EMBER_BENCH_BEGIN(Spawn);
    if (eventTemplateCount_ > 0) {
        // phase 4: serial event compression + child slot prefix sum.
        simProg_.setInt("uPhase", 4);
        uploadPhase(4);
        glDispatchCompute(1, 1, 1);
        glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
    }
    if (livePipeline && (total > 0 || eventTemplateCount_ > 0)) {
        simProg_.setInt("uPhase", 3); // reserve this entire birth batch once
        uploadPhase(3);
        glDispatchCompute(1, 1, 1);
        glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
    }
    simProg_.setInt("uPhase", 1); // spawn (GPU samples particles from requests)
    uploadPhase(1);
    if (total > 0 || eventTemplateCount_ > 0) {
        // Only the accepted prefix can write a particle. Bound the launch by
        // capacity in GPU mode so indirect-limit validation also covers births.
        // Active event templates may add up to the per-frame child cap; the
        // shader drops the excess (j >= uSpawnAccepted) without a readback.
        const std::uint32_t eventExtra = eventTemplateCount_ ? kMaxEventChildren : 0u;
        const auto spawnCount = gpuDriven_ ? std::min(total + eventExtra, capacity_)
                                           : (total + eventExtra);
        glDispatchCompute((spawnCount + kSimGroupSize - 1u) / kSimGroupSize, 1, 1);
    }
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);

    EMBER_BENCH_END(Spawn);
    EMBER_BENCH_BEGIN(BuildLive);
    // Publish draw args; the optimized pipeline already built the next live
    // list during integration/spawning. Legacy shaders scan the extent here.
    simProg_.setInt("uPhase", 2);
    uploadPhase(2);
    const auto extent = (std::uint32_t)std::min<std::uint64_t>(capacity_, (std::uint64_t)allocated_ + total);
    if (livePipeline) glDispatchCompute(1, 1, 1);
    else if (extent) glDispatchCompute((extent + kSimGroupSize - 1u) / kSimGroupSize, 1, 1);
    glMemoryBarrier(GL_COMMAND_BARRIER_BIT | GL_SHADER_STORAGE_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);
    if (gpuDriven_ && sortEnabled_) scheduleGpu(1);
    EMBER_BENCH_END(BuildLive);

    // 3) Publish CPU statistics. GPU scheduling never depends on this snapshot.
    EMBER_BENCH_BEGIN(Readback);
    ++frameCount_;
    countsFresh_=false;
    if (gpuDriven_) enqueueStatistics();
    else refreshCounts();
    EMBER_BENCH_END(Readback);

    // 4) Debug diagnostics (EMBER_DEBUG=1 / config debug=true): ~10 lines/sec + GL error sweep.
    if (debug_ && !gpuDriven_ && (frameCount_ % 6 == 0)) {
        GLuint ctr[4] = {0, 0, 0, 0};
        counterBuf_.getSubData(0, sizeof(ctr), ctr);
        Particle p0{};
        cur_->getSubData(0, sizeof(Particle), &p0);
        std::fprintf(stderr,
                     "[ember] frame=%llu alive=%u head=%u reqs=%u total=%u cap=%u mask=0x%X dt=%.4f\n"
                     "        p0 pos=(%.3f,%.3f,%.3f) size=%.3f vel=(%.2f,%.2f,%.2f) age=%.3f life=%.3f col=(%.2f,%.2f,%.2f,%.2f)\n",
                     (unsigned long long)frameCount_, ctr[0], ctr[1], nr, total, ctr[3], p.forceMask, dt,
                     p0.pos.x, p0.pos.y, p0.pos.z, p0.pos.w,
                     p0.vel.x, p0.vel.y, p0.vel.z,
                     p0.vel.w, p0.life.x,
                     p0.color.r, p0.color.g, p0.color.b, p0.color.a);
        const int errs = gl::popErrors("update");
        if (errs > 0) std::fprintf(stderr, "[ember] %d GL error(s) after update\n", errs);
    }

    // 5) Swap: the freshly written buffer becomes the render/sim source.
    std::swap(cur_, nxt_);
    if (livePipeline) {
        std::swap(liveBuf_, nextLiveBuf_);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kBindingLive, liveBuf_.id());
    }
}
} // namespace ember::detail::opengl
/* ============================================================================
 * [21] backends/opengl/render.cpp
 * backends/opengl/render.cpp
 * ========================================================================== */

namespace ember::detail::opengl {
struct RenderState {
    GLint drawFbo, readFbo, viewport[4], depthFunc;
    GLboolean depthTest, depthWrite, cullFace, scissor;
    RenderState() {
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &drawFbo);
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &readFbo);
        glGetIntegerv(GL_VIEWPORT, viewport);
        glGetIntegerv(GL_DEPTH_FUNC, &depthFunc);
        depthTest = glIsEnabled(GL_DEPTH_TEST);
        cullFace = glIsEnabled(GL_CULL_FACE);
        scissor = glIsEnabled(GL_SCISSOR_TEST);
        glGetBooleanv(GL_DEPTH_WRITEMASK, &depthWrite);
    }
    ~RenderState() {
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, drawFbo);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, readFbo);
        glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
        if (depthTest) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
        if (cullFace) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
        restoreScissor();
        glDepthMask(depthWrite);
        glDepthFunc(depthFunc);
    }
    void restoreScissor() const {
        if (scissor) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
    }
};

void OpenGLBackend::render(const RenderParameters& p,const RenderView& camera) {
    const auto& view=camera.view;
    const auto& proj=camera.projection;
    const float viewportWidth=camera.width,viewportHeight=camera.height,fovYDeg=camera.fovYDegrees;

    if ((!gpuDriven_ && alive_ == 0) || viewportWidth <= 0 || viewportHeight <= 0 ||
        !std::isfinite(viewportWidth) || !std::isfinite(viewportHeight)) return;
    RenderState state;
    glDisable(GL_CULL_FACE); // billboards/fullscreen triangles are two-sided
    if (!renderProg_) ensurePrograms();
    if (sortEnabled_) sortParticles(view);
    (void)fovYDeg;
    const int w = (int)viewportWidth, h = (int)viewportHeight;
    glViewport(0, 0, w, h);
    if (bloom_) ensureBloom(w, h);
    const bool useBloom = bloom_ && bloomPass_ && bloomBlur_ && bloomComposite_;
    if (useBloom) {
        // Host scissor coordinates only apply to the final framebuffer. Half
        // resolution intermediates must be fully cleared and processed.
        glDisable(GL_SCISSOR_TEST);
        EMBER_BENCH_BEGIN(BloomPrepare);
        // Resolve the host depth into an identically formatted attachment.
        // This preserves scene occlusion for the offscreen particle pass,
        // including hosts whose depth buffer is multisampled.
        glBindFramebuffer(GL_READ_FRAMEBUFFER, state.drawFbo);
        GLint bits = 0, component = GL_UNSIGNED_NORMALIZED, stencil = 0;
        if (p.depthTest) {
            const GLenum attachment = state.drawFbo ? GL_DEPTH_ATTACHMENT : GL_DEPTH;
            GLint object = GL_FRAMEBUFFER_DEFAULT;
            if (state.drawFbo)
                glGetFramebufferAttachmentParameteriv(GL_READ_FRAMEBUFFER, attachment,
                    GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &object);
            if (object != GL_NONE) {
                glGetFramebufferAttachmentParameteriv(GL_READ_FRAMEBUFFER, attachment,
                    GL_FRAMEBUFFER_ATTACHMENT_DEPTH_SIZE, &bits);
                glGetFramebufferAttachmentParameteriv(GL_READ_FRAMEBUFFER, attachment,
                    GL_FRAMEBUFFER_ATTACHMENT_COMPONENT_TYPE, &component);
                glGetFramebufferAttachmentParameteriv(GL_READ_FRAMEBUFFER, attachment,
                    GL_FRAMEBUFFER_ATTACHMENT_STENCIL_SIZE, &stencil);
                // Default framebuffer's stencil attachment is queried separately.
                if (!state.drawFbo)
                    glGetFramebufferAttachmentParameteriv(GL_READ_FRAMEBUFFER, GL_STENCIL,
                        GL_FRAMEBUFFER_ATTACHMENT_STENCIL_SIZE, &stencil);
            }
        }
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, bloomFbo_);
        if (bits) {
            GLenum internal = stencil ? (component == GL_FLOAT ? GL_DEPTH32F_STENCIL8 : GL_DEPTH24_STENCIL8)
                : component == GL_FLOAT ? GL_DEPTH_COMPONENT32F
                : bits <= 16 ? GL_DEPTH_COMPONENT16 : bits <= 24 ? GL_DEPTH_COMPONENT24 : GL_DEPTH_COMPONENT32;
            glActiveTexture(GL_TEXTURE0);
            bloomDepthTex_.bind();
            glTexImage2D(GL_TEXTURE_2D, 0, internal, w, h, 0,
                stencil ? GL_DEPTH_STENCIL : GL_DEPTH_COMPONENT,
                stencil ? (component == GL_FLOAT ? GL_FLOAT_32_UNSIGNED_INT_24_8_REV : GL_UNSIGNED_INT_24_8) : GL_FLOAT, nullptr);
            glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, bloomDepthTex_.id(), 0);
            glBlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_DEPTH_BUFFER_BIT, GL_NEAREST);
        } else {
            glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, 0, 0);
        }
        glClearColor(0, 0, 0, 0);
        glClear(GL_COLOR_BUFFER_BIT);
        EMBER_BENCH_END(BloomPrepare);
        EMBER_BENCH_BEGIN(BloomDraw);
        drawParticles(p,view, proj, viewportWidth, viewportHeight);
        EMBER_BENCH_END(BloomDraw);
        // Draw the actual particles onto the host with their chosen blending
        // and depth policy. The HDR target contributes only the halo.
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, state.drawFbo);
        state.restoreScissor();
        EMBER_BENCH_BEGIN(Draw);
        drawParticles(p,view, proj, viewportWidth, viewportHeight);
        EMBER_BENCH_END(Draw);
        EMBER_BENCH_BEGIN(BloomPost);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_DEPTH_TEST);
        glDepthMask(GL_FALSE);
        glDisable(GL_BLEND);
        const int hw = std::max(1, w / 2), hh = std::max(1, h / 2);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, bloomHalfFbo_);
        glViewport(0, 0, hw, hh);
        glUseProgram(bloomPass_.id());
        bloomPass_.setInt("uTex", 0);
        bloomPass_.setFloat("uThreshold", p.bloomThreshold);
        bloomPass_.setVec2("uTexel", {1.f / w, 1.f / h});
        BloomParams bp{};
        bp.texelX = 1.f / w; bp.texelY = 1.f / h; bp.threshold = p.bloomThreshold;
        uboBloom_.subData(0, sizeof(bp), &bp);
        glBindBufferBase(GL_UNIFORM_BUFFER, kBindingUboBloom, uboBloom_.id());        glActiveTexture(GL_TEXTURE0);
        bloomTex_.bind();
        drawFullscreen();
        for (int i = 0; i < 2; ++i) {
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, i == 0 ? bloomBlurFbo_ : bloomHalfFbo_);
            glUseProgram(bloomBlur_.id());
            bloomBlur_.setInt("uTex", 0);
            bloomBlur_.setVec2("uTexel", {1.f / hw, 1.f / hh});
            bloomBlur_.setVec2("uDir", i == 0 ? glm::vec2(1, 0) : glm::vec2(0, 1));
            bp.texelX = 1.f / hw; bp.texelY = 1.f / hh;
            bp.dirX = i == 0 ? 1.f : 0.f; bp.dirY = i == 0 ? 0.f : 1.f;
            uboBloom_.subData(0, sizeof(bp), &bp);
            glBindBufferBase(GL_UNIFORM_BUFFER, kBindingUboBloom, uboBloom_.id());            (i == 0 ? bloomHalfTex_ : bloomBlurTex_).bind();
            drawFullscreen();
        }
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, state.drawFbo);
        glViewport(0, 0, w, h);
        state.restoreScissor();
        glEnable(GL_BLEND);
        glBlendEquation(GL_FUNC_ADD);
        glBlendFunc(GL_ONE, GL_ONE);
        glUseProgram(bloomComposite_.id());
        bloomComposite_.setInt("uTex", 0);
        bloomHalfTex_.bind();
        drawFullscreen();
        glDisable(GL_BLEND);
        EMBER_BENCH_END(BloomPost);
    } else {
        EMBER_BENCH_BEGIN(Draw);
        drawParticles(p,view, proj, viewportWidth, viewportHeight);
        EMBER_BENCH_END(Draw);
    }
    if (p.refraction.enabled && sceneColor_) {
        EMBER_BENCH_BEGIN(Refraction);
        drawParticles(p,view, proj, viewportWidth, viewportHeight, true);
        EMBER_BENCH_END(Refraction);
    }
}

void OpenGLBackend::drawParticles(const RenderParameters& p,const glm::mat4& view, const glm::mat4& proj,
                                   float viewportWidth, float viewportHeight, bool refractionPass) {
    glUseProgram(renderProg_.id());
    renderProg_.setMat4("uView", view);
    renderProg_.setMat4("uProj", proj);
    renderProg_.setFloat("uSizeScale", p.sizeScale);
    renderProg_.setFloat("uStreak", p.streak);
    renderProg_.setInt("uUseSorted", sortEnabled_ ? 1 : 0);
    renderProg_.setInt("uUseSprite", p.useSprite ? 1 : 0);
    renderProg_.setInt("uSheetCols", p.sheetCols);
    renderProg_.setInt("uSheetRows", p.sheetRows);
    renderProg_.setVec2("uViewportSize", {viewportWidth, viewportHeight});
    renderProg_.setFloat("uSpinSpeed", p.spin);
    renderProg_.setInt("uRefraction", refractionPass ? 1 : 0);

    // Built-in shaders read std140 blocks (bindings 17/18); the loose
    // uniforms above serve custom shaders on the legacy contract.
    DrawParams dp{};
    dp.view = view; dp.proj = proj;
    dp.sizeScale = p.sizeScale; dp.streak = p.streak; dp.spinSpeed = p.spin;
    dp.useSorted = sortEnabled_ ? 1 : 0;
    dp.refraction = refractionPass ? 1 : 0;
    FragParams fp{};
    fp.invProj = glm::inverse(proj); fp.fragProj = proj;
    fp.viewportWidth = viewportWidth; fp.viewportHeight = viewportHeight;
    fp.softRadius = p.softRadius;
    fp.useSprite = p.useSprite ? 1 : 0;
    fp.sheetCols = p.sheetCols; fp.sheetRows = p.sheetRows;
    fp.fragRefraction = refractionPass ? 1 : 0;
    if (refractionPass) {
        // Refractive particles: replace pixels (blend off) by sampling the
        // host's scene color texture (unit 2) with a facet offset.
        int mode = p.refraction.mode;
        const bool hasDepth = refractionDepth_ != 0 || sceneDepth_ != 0;
        if (mode == 1 && !hasDepth) mode = 0; // depth-aware without a depth texture
        renderProg_.setInt("uRefrMode", mode);
        renderProg_.setInt("uRefrShape", p.refraction.shape);
        renderProg_.setFloat("uRefrDome", p.refraction.dome);
        renderProg_.setFloat("uRefrStrength", p.refraction.strength);
        renderProg_.setFloat("uRefrIor", p.refraction.ior);
        renderProg_.setVec3("uRefrTint", p.refraction.tint);
        renderProg_.setFloat("uRefrAbsorption", p.refraction.absorption);
        renderProg_.setFloat("uRefrFresnel", p.refraction.fresnel);
        renderProg_.setFloat("uRefrChroma", p.refraction.chroma);
        renderProg_.setFloat("uRefrSpecular", p.refraction.specular);
        renderProg_.setVec3("uRefrLightDir", glm::mat3(view) * p.refraction.lightDir);
        fp.refrMode = mode; fp.refrShape = p.refraction.shape;
        fp.refrDome = p.refraction.dome; fp.refrStrength = p.refraction.strength;
        fp.refrIor = p.refraction.ior; fp.refrTint = p.refraction.tint;
        fp.refrAbsorption = p.refraction.absorption; fp.refrFresnel = p.refraction.fresnel;
        fp.refrChroma = p.refraction.chroma; fp.refrSpecular = p.refraction.specular;
        fp.refrLightDir = glm::mat3(view) * p.refraction.lightDir;        // Use the system's explicit policy, not the host's previous state.
        // Pixel replacement still needs occlusion when depth testing is on.
        if (p.depthTest) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
        glDepthMask(p.depthWrite ? GL_TRUE : GL_FALSE);
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, sceneColor_);
        renderProg_.setInt("uSceneColor", 2);
        if (mode == 1) {
            const GLuint depth = refractionDepth_ ? refractionDepth_ : sceneDepth_;
            renderProg_.setMat4("uInvProj", glm::inverse(proj)); // scene-point reconstruction
            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, depth);
            renderProg_.setInt("uSceneDepth", 1);
        }
        glDisable(GL_BLEND);
    } else {
        const bool soft = p.softParticles && sceneDepth_ != 0;
        renderProg_.setInt("uUseSoft", soft ? 1 : 0);
        fp.useSoft = soft ? 1 : 0;        if (soft) {
            renderProg_.setMat4("uInvProj", glm::inverse(proj));
            renderProg_.setFloat("uSoftRadius", p.softRadius);
            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, sceneDepth_);
            renderProg_.setInt("uSceneDepth", 1);
        }
        glEnable(GL_BLEND);
        glBlendEquation(GL_FUNC_ADD);
        if (p.blend == BlendMode::Additive) glBlendFunc(GL_SRC_ALPHA, GL_ONE);
        else glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        if (p.depthTest) glEnable(GL_DEPTH_TEST);
        else glDisable(GL_DEPTH_TEST);
        glDepthMask(p.depthWrite ? GL_TRUE : GL_FALSE);
    }

    uboDraw_.subData(0, sizeof(dp), &dp);
    uboFrag_.subData(0, sizeof(fp), &fp);
    glBindBufferBase(GL_UNIFORM_BUFFER, kBindingUboDraw, uboDraw_.id());
    glBindBufferBase(GL_UNIFORM_BUFFER, kBindingUboFrag, uboFrag_.id());

    glActiveTexture(GL_TEXTURE0);
    spriteTex_.bind();
    renderProg_.setInt("uSprite", 0);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kBindingCur, cur_->id());
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kBindingLive, liveBuf_.id());
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kBindingSorted, sortedBuf_.id());
    // Curves bind unconditionally: a custom render shader without BufCurves
    // simply ignores them (the render path stays tolerant).
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kBindingCurves, curvesBuf_.id());
    vao_.bind();
    // Draw count comes from the GPU-written indirect args (phase 2 of update());
    // the CPU alive_ readback is only for aliveCount()/UI — not the render path.
    glBindBuffer(GL_DRAW_INDIRECT_BUFFER, indirectBuf_.id());
    glDrawArraysIndirect(GL_TRIANGLE_STRIP, nullptr);
    glBindBuffer(GL_DRAW_INDIRECT_BUFFER, 0);
    vao_.unbind();

    glDepthMask(GL_TRUE); // restore
}

void OpenGLBackend::drawFullscreen() {
    vao_.bind();
    glDrawArrays(GL_TRIANGLES, 0, 3);
    vao_.unbind();
}

void OpenGLBackend::ensureBloom(int w, int h) {
    RenderState state;
    try {
        if (!bloomPass_ || !bloomBlur_ || !bloomComposite_) {
            const std::string dir = shaderDir_.empty() ? "" : shaderDir_ + "/";
            auto pass = Shader::fromVertFrag((dir + "bloom.vert").c_str(), (dir + "bloom_threshold.frag").c_str());
            auto blur = Shader::fromVertFrag((dir + "bloom.vert").c_str(), (dir + "bloom_blur.frag").c_str());
            auto composite = Shader::fromVertFrag((dir + "bloom.vert").c_str(), (dir + "bloom_composite.frag").c_str());
            bloomPass_ = std::move(pass);
            bloomBlur_ = std::move(blur);
            bloomComposite_ = std::move(composite);
        }
        if (bloomFbo_ && w == bloomW_ && h == bloomH_) return;
        glActiveTexture(GL_TEXTURE0);
        bloomTex_.uploadRGBA16F(w, h);
        bloomHalfTex_.uploadRGBA16F(std::max(1, w / 2), std::max(1, h / 2));
        bloomBlurTex_.uploadRGBA16F(std::max(1, w / 2), std::max(1, h / 2));
        if (!bloomFbo_) {
            GLuint fbos[3]{};
            glGenFramebuffers(3, fbos);
            bloomFbo_ = fbos[0]; bloomHalfFbo_ = fbos[1]; bloomBlurFbo_ = fbos[2];
        }
        const GLuint fbos[] = {bloomFbo_, bloomHalfFbo_, bloomBlurFbo_};
        const GLuint textures[] = {bloomTex_.id(), bloomHalfTex_.id(), bloomBlurTex_.id()};
        for (int i = 0; i < 3; ++i) {
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbos[i]);
            glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, 0, 0);
            glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, textures[i], 0);
            if (glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
                throw std::runtime_error("incomplete bloom framebuffer");
        }
        bloomW_ = w; bloomH_ = h;
    } catch (const std::exception& e) {
        bloomPass_ = Shader(); bloomBlur_ = Shader(); bloomComposite_ = Shader();
        bloomW_ = bloomH_ = 0;
        bloom_ = false;
        std::fprintf(stderr, "[ember] bloom disabled: %s\n", e.what());
    }
}

} // namespace ember::detail::opengl
/* ============================================================================
 * [22] backends/opengl/sort.cpp
 * backends/opengl/sort.cpp
 * ========================================================================== */

namespace ember::detail::opengl {
void OpenGLBackend::setSortEnabled(bool on) {
    if (on && gpuDriven_) {
        ensureSortProgram();
        if (!sortProg_ || !sortProg_.hasUniform("uGpuDriven") || !sortProg_.hasUniform("uTileMode"))
            throw std::invalid_argument("ember: GPU-driven mode requires the GPU scheduling sort protocol");
        if (!sortEnabled_) scheduleGpu(1); // allow render before another update
    }
    sortEnabled_ = on;
}

void OpenGLBackend::ensureSortProgram() {
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

void OpenGLBackend::sortParticles(const glm::mat4& view) {
    ensureSortProgram();
    if (!sortProg_) return;
    EMBER_BENCH_BEGIN(Sort);
    if (gpuDriven_ && (!sortProg_.hasUniform("uGpuDriven") || !sortProg_.hasUniform("uTileMode")))
        throw std::invalid_argument("ember: incompatible sort shader in GPU-driven mode");
    const std::uint32_t N = nextPow2(gpuDriven_?capacity_:alive_);
    const bool tiled = sortProg_.hasUniform("uTileMode");
    const auto groupSize = tiled ? 256u : kSimGroupSize;
    const auto groups = (N + groupSize - 1u) / groupSize;
    if (tiled) {
        if (sortKeyCapacity_ < N) {
            sortKeyBuf_.data(nullptr, (GLsizeiptr)N * sizeof(float), GL_DYNAMIC_COPY);
            sortKeyCapacity_ = N;
        }
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kBindingScratch, sortKeyBuf_.id());
    }
    glUseProgram(sortProg_.id());
    sortProg_.setMat4("uView", view);
    sortProg_.setUint("uAlive", alive_);
    sortProg_.setUint("uCapacity", capacity_);
    sortProg_.setUint("uPaddedN", N);
    sortProg_.setInt("uGpuDriven",gpuDriven_?1:0);
    // Built-in shader reads a std140 block; the loose uniforms above serve
    // custom shaders on the legacy sort contract.
    SortParams sp{};
    sp.view = view;
    sp.capacity = capacity_; sp.alive = alive_; sp.paddedN = N;
    sp.gpuDriven = gpuDriven_ ? 1 : 0;
    glBindBufferBase(GL_UNIFORM_BUFFER, kBindingUboSort, uboSort_.id());
    const auto uploadSort = [&] { uboSort_.subData(0, sizeof(sp), &sp); };
    if (gpuDriven_) {
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER,kBindingSchedule,dispatchBuf_.id());
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER,kBindingCounters,counterBuf_.id());
        glBindBuffer(GL_DISPATCH_INDIRECT_BUFFER,dispatchBuf_.id());
    }
    GLintptr dispatchOffset=5*sizeof(GLuint);
    auto dispatch=[&]() {
        if (gpuDriven_) {
            glDispatchComputeIndirect(dispatchOffset);
            dispatchOffset+=3*sizeof(GLuint);
        } else glDispatchCompute(groups,1,1);
    };
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kBindingCur, cur_->id());
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kBindingLive, liveBuf_.id());
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kBindingSorted, sortedBuf_.id());

    // Fill the live permutation and cache keys; optimized shaders also sort
    // alternating 256-entry tiles here. Legacy custom shaders keep mode 0/1.
    sortProg_.setUint("uMode", 0);
    sortProg_.setUint("uTileMode", 1);
    sp.mode = 0; sp.tileMode = 1; sp.k = 0; sp.j = 0;
    uploadSort();
    dispatch();
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);

    // Cross-tile merges use cached keys; all final intra-tile steps execute
    // in one dispatch. No approximation or reduced sorting frequency.
    for (std::uint32_t k = tiled ? 512u : 2u; k <= N; k <<= 1) {
        for (std::uint32_t j = k >> 1; j >= (tiled ? 256u : 1u); j >>= 1) {
            sortProg_.setUint("uK", k);
            sortProg_.setUint("uJ", j);
            sortProg_.setUint("uMode", 1);
            sortProg_.setUint("uTileMode", 0);
            sp.k = k; sp.j = j; sp.mode = 1; sp.tileMode = 0;
            uploadSort();
            dispatch();
            glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
        }
        if (tiled) {
            sortProg_.setUint("uTileMode", 2);
            sp.tileMode = 2;
            uploadSort();
            dispatch();
            glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
        }
    }
    glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
    EMBER_BENCH_END(Sort);
}

} // namespace ember::detail::opengl
/* ============================================================================
 * [23] backends/opengl/compat.cpp
 * backends/opengl/compat.cpp
 * ========================================================================== */

namespace ember {
namespace {
detail::opengl::OpenGLBackend& requireOpenGL(ParticleBackend& backend) {
    auto* gl=dynamic_cast<detail::opengl::OpenGLBackend*>(&backend);
    if(!gl) throw std::invalid_argument("ember: this operation requires the OpenGL backend");
    return *gl;
}
}
std::unique_ptr<ParticleBackend> makeOpenGLBackend() {
    return std::make_unique<detail::opengl::OpenGLBackend>();
}
ParticleSystem::ParticleSystem(const Settings& s) : ParticleSystem(s,makeOpenGLBackend()) {}
void setOpenGLPrograms(ParticleBackend& backend,Shader render,Shader simulate) {
    requireOpenGL(backend).setPrograms(std::move(render),std::move(simulate));
}
void setOpenGLRefractionInputs(ParticleBackend& backend,GLuint color,GLuint depth) {
    requireOpenGL(backend).setRefractionInputs(color,depth);
}
void setOpenGLSoftDepth(ParticleBackend& backend,GLuint depth) {
    requireOpenGL(backend).setSoftDepth(depth);
}
void setOpenGLRefraction(ParticleSystem& sys,const RefractionSettings& settings) {
    auto& gl=requireOpenGL(sys.backend());
    RefractionParameters p;
    p.enabled=settings.enabled;
    p.mode=settings.mode;
    p.strength=settings.strength;
    p.ior=settings.ior;
    p.tint=settings.tint;
    p.absorption=settings.absorption;
    p.fresnel=settings.fresnel;
    p.chroma=settings.chroma;
    p.specular=settings.specular;
    p.lightDir=settings.lightDir;
    p.shape=settings.shape;
    p.dome=settings.dome;
    sys.setRefractionParameters(p);
    gl.setRefractionInputs(settings.sceneColorTex,settings.sceneDepthTex);
}
void setOpenGLSoftParticles(ParticleSystem& sys,bool on,std::uint32_t sceneDepthTex,float radius) {
    if(on || sceneDepthTex!=0) {
        sys.setSoftParticleParameters(on,radius);
        requireOpenGL(sys.backend()).setSoftDepth(sceneDepthTex);
    } else sys.setSoftParticleParameters(false,radius);
}
namespace Refraction {
RefractionSettings glass() {
    RefractionSettings s;
    s.tint = {0.95f, 0.97f, 1.f};
    s.absorption = 0.35f;
    s.fresnel = 2.f;
    s.chroma = 0.15f;
    s.specular = 0.4f;
    s.shape = 1;   // polygon shards (3-5 sides, jittered)
    s.dome = 1.3f; // curved-facet lighting: visible rim, glint spots
    return s;
}
RefractionSettings heat() {
    RefractionSettings s;
    s.mode = 2; // noise offset: invisible lens
    s.strength = 0.015f;
    s.absorption = 1.f; // full displaced scene: mix(original, refr, 0) would be invisible
    s.fresnel = 0.f;
    return s;
}
RefractionSettings water() {
    RefractionSettings s;
    s.tint = {0.8f, 0.9f, 1.f};
    s.absorption = 0.5f;
    s.fresnel = 4.f;
    s.dome = 2.2f; // droplet-like: tight bright rim
    return s;
}
RefractionSettings prism() {
    RefractionSettings s;
    s.chroma = 1.0f;
    s.absorption = 0.5f;
    s.specular = 0.2f;
    s.shape = 1;   // polygon shards
    s.dome = 0.8f;
    return s;
}
} // namespace Refraction

} // namespace ember

#ifdef EMBER_USE_GLFW

/* ============================================================================
 * [24] glfw_window.cpp
 * GLFW window wrapper
 * ========================================================================== */

#include <exception>

namespace ember {

namespace {
thread_local std::exception_ptr g_callbackError;
template<class F> void callback(F&& f) noexcept {
    try { f(); } catch (...) { if (!g_callbackError) g_callbackError = std::current_exception(); }
}
int g_glfwRefs = 0; // refcount for glfwInit/glfwTerminate
[[noreturn]] void contextFailure(int code, const char* message) {
    if (code == GLFW_API_UNAVAILABLE || code == GLFW_VERSION_UNAVAILABLE ||
        code == GLFW_PLATFORM_UNAVAILABLE || code == GLFW_PLATFORM_ERROR ||
        code == GLFW_FORMAT_UNAVAILABLE)
        throw ContextUnavailable(message);
    throw std::runtime_error(message);
}
}

Window::Window(int width, int height, const char* title, int samples, bool vsync, bool visible) {
    if (g_glfwRefs++ == 0) {
        if (!glfwInit()) {
            const int error = glfwGetError(nullptr);
            --g_glfwRefs;
            contextFailure(error, "ember: glfwInit failed");
        }
    }
    glfwWindowHint(GLFW_VISIBLE, visible ? GLFW_TRUE : GLFW_FALSE);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_SAMPLES, samples);
#ifdef __APPLE__
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE); // (GL 4.1 on macOS — compute shaders need 4.3)
#endif
    w_ = glfwCreateWindow(width, height, title, nullptr, nullptr);
    if (!w_) {
        const int error = glfwGetError(nullptr);
        if (--g_glfwRefs == 0) glfwTerminate();
        contextFailure(error, "ember: failed to create GLFW window (needs OpenGL 4.3 core)");
    }
    glfwMakeContextCurrent(w_);
    if (!gl::init(glfwGetProcAddress)) {
        glfwDestroyWindow(w_);
        w_ = nullptr;
        if (--g_glfwRefs == 0) glfwTerminate();
        throw std::runtime_error("ember: glad failed to load OpenGL 4.3 entry points");
    }
    glfwSetWindowUserPointer(w_, this);
    glfwSwapInterval(vsync ? 1 : 0);
    glfwSetKeyCallback(w_, &Window::keyCb);
    glfwSetCursorPosCallback(w_, &Window::cursorCb);
    glfwSetMouseButtonCallback(w_, &Window::mouseCb);
    glfwSetScrollCallback(w_, &Window::scrollCb);
    glfwSetFramebufferSizeCallback(w_, &Window::resizeCb);
    lastTime_ = glfwGetTime();
}

Window::~Window() {
    if (w_) glfwDestroyWindow(w_);
    if (--g_glfwRefs <= 0) glfwTerminate();
}

void Window::checkCallbacks() {
    if (g_callbackError) {
        auto error = g_callbackError;
        g_callbackError = nullptr;
        std::rethrow_exception(error);
    }
}

void Window::setVsync(bool on) {
    GLFWwindow* previous = glfwGetCurrentContext();
    if (previous != w_) glfwMakeContextCurrent(w_);
    glfwSwapInterval(on ? 1 : 0);
    if (previous != w_) glfwMakeContextCurrent(previous);
    checkCallbacks();
}

void Window::pollEvents() {
    glfwPollEvents();
    checkCallbacks();
    const double t = glfwGetTime();
    lastDt_ = (float)(t - lastTime_);
    lastTime_ = t;
}

glm::ivec2 Window::size() const {
    int w = 0, h = 0;
    glfwGetWindowSize(w_, &w, &h);
    return {w, h};
}

glm::ivec2 Window::framebufferSize() const {
    int w = 0, h = 0;
    glfwGetFramebufferSize(w_, &w, &h);
    return {w, h};
}

glm::vec2 Window::cursor() const {
    double x = 0.0, y = 0.0;
    glfwGetCursorPos(w_, &x, &y);
    return {(float)x, (float)y};
}

glm::vec2 Window::scrollDelta() {
    const glm::vec2 d = scrollAccum_;
    scrollAccum_ = {0.f, 0.f};
    return d;
}

// ---- static callbacks -----------------------------------------------------

void Window::keyCb(GLFWwindow* w, int key, int sc, int act, int mods) {
    if (auto* s = fromHandle(w); s && s->onKey) callback([&] { s->onKey(key, sc, act, mods); });
}
void Window::cursorCb(GLFWwindow* w, double x, double y) {
    if (auto* s = fromHandle(w); s && s->onCursorPos) callback([&] { s->onCursorPos(x, y); });
}
void Window::mouseCb(GLFWwindow* w, int b, int act, int mods) {
    if (auto* s = fromHandle(w); s && s->onMouseButton) callback([&] { s->onMouseButton(b, act, mods); });
}
void Window::scrollCb(GLFWwindow* w, double x, double y) {
    if (auto* s = fromHandle(w)) {
        s->scrollAccum_ += glm::vec2((float)x, (float)y);
        if (s->onScroll) callback([&] { s->onScroll(x, y); });
    }
}
void Window::resizeCb(GLFWwindow* w, int width, int height) {
    if (auto* s = fromHandle(w); s && s->onResize) callback([&] { s->onResize(width, height); });
}

} // namespace ember

#endif // EMBER_USE_GLFW

#endif // EMBER_SINGLE_HEADER_IMPLEMENTATION
#endif // EMBER_IMPLEMENTATION

#undef EMBER_BENCH_BEGIN
#undef EMBER_BENCH_END