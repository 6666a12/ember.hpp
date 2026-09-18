#pragma once
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
