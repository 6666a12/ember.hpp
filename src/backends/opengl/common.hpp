#pragma once
#include "particle_backend.hpp"
#include "params.hpp"
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
