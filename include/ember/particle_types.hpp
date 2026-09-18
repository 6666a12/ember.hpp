#pragma once
// Backend-independent particle settings and per-call inputs. No graphics API headers.
#include "ember/emitters.hpp"
#include <cstdint>
#include <vector>

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
