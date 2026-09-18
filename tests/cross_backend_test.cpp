// Cross-backend consistency test: the SAME command stream (identical settings,
// seed, emitters, bursts, force configuration, dt sequence) must drive the
// OpenGL and Vulkan backends to the same simulation state, frame by frame.
//
// Three legs are compared after every update():
//   gl     — OpenGL backend, synchronous scheduling
//   vk     — Vulkan backend, synchronous scheduling
//   vk-gpu — Vulkan backend, GPU-driven scheduling
// Counts and statistics must match exactly; particle fields must match within
// a float tolerance (the GL driver and glslang compile the same GLSL source
// but may contract fma differently). The final report prints how much of the
// state is bit-identical, so a silent precision regression stays visible.
//
// Slot order is part of the contract while no particle dies (append-only
// spawning is deterministic integer logic); once recycling kicks in, the free
// stack's atomic push order makes slots a per-run permutation even on one
// backend, so recycle phases compare particle multisets instead.
//
// Requires both a GL 4.3 context and a Vulkan 1.1 device; exits 77 (ctest
// SKIP_RETURN_CODE) when either is unavailable.

#include "ember/glfw_window.hpp"
#include "ember/particle_system.hpp"
#include "ember/system.hpp"
#include "ember/vulkan.hpp"

#include <glm/glm.hpp>

#include <algorithm>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

[[noreturn]] void fail(const std::string& message) { throw std::runtime_error(message); }
void check(bool ok, const std::string& message) { if (!ok) fail(message); }

// Tolerance: same GLSL source, but the GL driver compiler and glslang may
// reorder/contract float ops differently, and force feedback (turbulence
// noise) amplifies that noise over frames. Measured on RTX 4060 Laptop:
// spawn-phase max |diff| ~2e-6, all-forces phase ~5e-4 after 30 frames.
// The gate sits ~10x above the measurement; formula-level backend bugs
// diverge far more. Exact equality is tracked separately (see Tally).
constexpr float kAbsEps = 1e-4f;
constexpr float kRelEps = 5e-3f;

const char* kFields[16] = {
    "pos.x", "pos.y", "pos.z", "pos.w",
    "vel.x", "vel.y", "vel.z", "vel.w",
    "life.x", "life.y", "life.z", "life.w",
    "color.r", "color.g", "color.b", "color.a",
};

struct Tally {
    std::size_t compared = 0;
    std::size_t bitExact = 0;
    float maxAbsDiff = 0.f;
    std::string worst; // description of the largest |a-b|

    void add(float a, float b, const char* phase, std::size_t particle, const char* field) {
        ++compared;
        if (a == b) { ++bitExact; return; } // covers identical zeros too
        check(std::isfinite(a) && std::isfinite(b),
              std::string(phase) + ": nonfinite value in " + field);
        const float diff = std::fabs(a - b);
        const float magnitude = std::max(std::fabs(a), std::fabs(b));
        if (diff > maxAbsDiff) {
            maxAbsDiff = diff;
            char buf[256];
            std::snprintf(buf, sizeof(buf), "%s particle %zu %s: %g vs %g",
                          phase, particle, field, (double)a, (double)b);
            worst = buf;
        }
        if (diff > kAbsEps + kRelEps * magnitude) {
            char buf[256];
            std::snprintf(buf, sizeof(buf),
                          "%s particle %zu %s diverged: %.9g vs %.9g (|d|=%.3g)",
                          phase, particle, field, (double)a, (double)b, (double)diff);
            fail(buf);
        }
    }
};
Tally g_tally; // reset per phase; reported by reportPhase

void reportPhase(const char* name) {
    const double exactPct = g_tally.compared
        ? 100.0 * double(g_tally.bitExact) / double(g_tally.compared) : 100.0;
    std::printf("phase %-9s %8zu floats, %6.2f%% bit-identical, max |diff| = %.3g (%s)\n",
                name, g_tally.compared, exactPct, (double)g_tally.maxAbsDiff,
                g_tally.worst.empty() ? "all bit-identical" : g_tally.worst.c_str());
    g_tally = {};
}

// Dying particles push their slot onto the free stack with atomicAdd
// (simulate.comp); the push ORDER depends on GPU thread scheduling, so once
// death/recycling occurs the slot->particle mapping is a per-run permutation
// even between two instances of the SAME backend. What stays deterministic is
// the multiset of particles. Phases with deaths therefore compare sorted
// snapshots; death-free phases compare in slot order (the stricter check).
void sortSnapshot(std::vector<ember::Particle>& v) {
    std::sort(v.begin(), v.end(), [](const ember::Particle& a, const ember::Particle& b) {
        const float* fa = &a.pos.x;
        const float* fb = &b.pos.x;
        for (int i = 0; i < 16; ++i) {
            if (fa[i] < fb[i]) return true;
            if (fa[i] > fb[i]) return false;
        }
        return false;
    });
}

struct Leg {
    std::string name;
    std::unique_ptr<ember::ParticleSystem> sys;
};

// Fans one command stream out to every leg and compares full state after each
// update. Statistics are exact; particle payloads go through the tally.
struct Harness {
    std::vector<Leg> legs;
    const char* phase = "?";
    bool unordered = false; // compare sorted snapshots (death/recycle phases)
    std::uint64_t frames = 0;

    void all(const std::function<void(ember::ParticleSystem&)>& fn) {
        for (auto& leg : legs) fn(*leg.sys);
    }

    void update(float dt) {
        for (auto& leg : legs) leg.sys->update(dt);
        ++frames;
        const auto& ref = *legs.front().sys;
        for (std::size_t i = 1; i < legs.size(); ++i) {
            auto& other = *legs[i].sys;
            check(other.aliveCount() == ref.aliveCount(),
                  std::string(phase) + ": aliveCount diverged (" + legs[i].name + " " +
                  std::to_string(other.aliveCount()) + " vs " + legs.front().name + " " +
                  std::to_string(ref.aliveCount()) + ") at frame " + std::to_string(frames));
            check(other.updateSequence() == ref.updateSequence(),
                  std::string(phase) + ": updateSequence diverged");
            const auto sa = ref.synchronizeStatistics();
            const auto sb = other.synchronizeStatistics();
            check(sa.alive == sb.alive && sa.allocated == sb.allocated,
                  std::string(phase) + ": statistics diverged (" + legs[i].name + ")");
        }
        // Particle payloads. Death-free phases compare in slot order (the
        // stricter check); recycle phases compare sorted snapshots.
        std::vector<std::vector<ember::Particle>> snapshots;
        snapshots.reserve(legs.size());
        for (auto& leg : legs) snapshots.push_back(leg.sys->readParticles());
        if (unordered)
            for (auto& s : snapshots) sortSnapshot(s);
        for (std::size_t i = 1; i < legs.size(); ++i) {
            const auto& a = snapshots[0];
            const auto& b = snapshots[i];
            check(a.size() == b.size(), std::string(phase) + ": readback size diverged");
            for (std::size_t p = 0; p < a.size(); ++p) {
                const float* fa = &a[p].pos.x;
                const float* fb = &b[p].pos.x;
                for (int f = 0; f < 16; ++f)
                    g_tally.add(fa[f], fb[f], phase, p, kFields[f]);
            }
        }
    }
};

Harness makeHarness(const ember::VulkanContext& ctx, const ember::ParticleSettings& settings) {
    Harness h;
    h.legs.push_back({"gl", std::make_unique<ember::ParticleSystem>(settings)});
    auto vulkan = [&](bool gpuDriven) {
        auto backend = ember::makeVulkanBackend();
        ember::setVulkanContext(*backend, ctx);
        auto sys = std::make_unique<ember::ParticleSystem>(settings, std::move(backend));
        sys->setGpuDriven(gpuDriven);
        return sys;
    };
    h.legs.push_back({"vk", vulkan(false)});
    h.legs.push_back({"vk-gpu", vulkan(true)});
    return h;
}

// ---------------------------------------------------------------------------
// Script phases. Every phase rebuilds fresh systems so phases stay isolated.
// ---------------------------------------------------------------------------

// All four emitter shapes + palette + fade + size/speed link, under gravity.
void phaseEmitters(const ember::VulkanContext& ctx) {
    auto h = makeHarness(ctx, {2048, 512, 0xE1117});
    h.phase = "emitters";

    ember::Emitter point;
    point.shape = ember::Emitter::Shape::Point;
    point.rate = 150.f;
    point.speedMin = 0.5f; point.speedMax = 2.5f;
    point.lifeMin = 1.f; point.lifeMax = 3.f;
    point.sizeMin = 0.03f; point.sizeMax = 0.12f;
    point.palette = {{1, 0, 0, 1}, {0, 1, 0, 1}, {0, 0.2f, 1, 1}, {1, 1, 0, 1}};
    point.hasFadeColor = true;
    point.fadeColorMin = {0.1f, 0.f, 0.f};
    point.fadeColorMax = {0.3f, 0.1f, 0.f};
    h.all([&](ember::ParticleSystem& s) { s.addEmitter(point); });

    ember::Emitter box;
    box.shape = ember::Emitter::Shape::Box;
    box.position = {1.f, 0.f, 0.f};
    box.extents = {0.5f, 0.1f, 0.5f};
    box.rate = 120.f;
    box.speedMin = 0.f; box.speedMax = 1.f;
    box.lifeMin = 2.f; box.lifeMax = 2.5f;
    box.sizeMin = box.sizeMax = 0.08f;
    h.all([&](ember::ParticleSystem& s) { s.addEmitter(box); });

    ember::Emitter sphere;
    sphere.shape = ember::Emitter::Shape::Sphere;
    sphere.position = {-1.f, 0.5f, 0.f};
    sphere.radius = 0.7f;
    sphere.rate = 100.f;
    sphere.speedMin = 1.f; sphere.speedMax = 1.f;
    sphere.speedSizeLink = 0.8f; // per-particle speed follows size
    sphere.lifeMin = 1.5f; sphere.lifeMax = 1.5f;
    sphere.sizeMin = 0.02f; sphere.sizeMax = 0.2f;
    h.all([&](ember::ParticleSystem& s) { s.addEmitter(sphere); });

    ember::Emitter cone;
    cone.shape = ember::Emitter::Shape::Cone;
    cone.position = {0.f, -0.5f, 0.f};
    cone.axis = {0.3f, 1.f, -0.2f}; // deliberately unnormalized
    cone.coneAngle = 25.f;
    cone.radius = 0.4f;
    cone.rate = 130.f;
    cone.speedMin = 2.f; cone.speedMax = 3.f;
    cone.lifeMin = 1.f; cone.lifeMax = 2.f;
    cone.sizeMin = cone.sizeMax = 0.05f;
    h.all([&](ember::ParticleSystem& s) { s.addEmitter(cone); });

    for (int f = 0; f < 24; ++f) h.update(1.f / 60.f);
}

// Every force field at once, with a varying dt sequence.
void phaseForces(const ember::VulkanContext& ctx) {
    auto h = makeHarness(ctx, {1024, 512, 0xF02CE});
    h.phase = "forces";
    const std::uint32_t all =
        (1u << 0) | (1u << 1) | (1u << 2) | (1u << 3) | (1u << 4); // gravity..attractors
    h.all([&](ember::ParticleSystem& s) {
        s.setForceMask(all);
        s.setGravity({0.f, -9.81f, 0.f});
        s.setDrag(0.35f);
        s.setDragMode(ember::DragMode::Linear);
        s.setWind({1.2f, 0.4f, -0.6f});
        s.setTurbulence(0.7f);
        s.setNoiseWind(glm::vec3(0.f, 0.f, 1.f), 0.9f, 0.5f, 1.3f);
        s.setWave(glm::vec3(1.f, 0.f, 0.f), glm::vec3(2.f, 0.f, 1.f), 0.4f, 3.f);
        s.setAttractors({{{2.f, 1.f, 0.f}, 3.f}, {{-2.f, -1.f, 1.f}, -1.5f}});
        s.setVortexes({{{0.f, 0.f, 0.f, 2.f}, {0.f, 1.f, 0.f, 2.5f}}});
        ember::Spring spring;
        spring.anchor = {0.f, 1.f, 0.f};
        spring.stiffness = 1.2f;
        spring.damping = 0.4f;
        s.setSprings({spring});
        ember::BurstParams b;
        b.count = 400;
        b.position = {0.f, 1.f, 0.f};
        b.speedMin = 1.f; b.speedMax = 4.f;
        b.spread = 1.f;
        b.lifeMin = 2.f; b.lifeMax = 4.f;
        b.sizeMin = 0.05f; b.sizeMax = 0.1f;
        s.burst(b);
    });
    for (int f = 0; f < 30; ++f) h.update(0.016f + 0.004f * float(f % 3));
}

// Boundary bounce with restitution (kill mode would only exercise counts).
void phaseBoundary(const ember::VulkanContext& ctx) {
    auto h = makeHarness(ctx, {512, 512, 0xB0B17});
    h.phase = "boundary";
    h.all([&](ember::ParticleSystem& s) {
        s.setForceMask((1u << 0)); // gravity only
        s.setGravity({0.f, -9.81f, 0.f});
        s.setBoundary(ember::BoundaryMode::Bounce, -1.5f, 0.45f);
        ember::BurstParams b;
        b.count = 300;
        b.position = {0.f, 2.f, 0.f};
        b.speedMin = 0.5f; b.speedMax = 2.f;
        b.spread = 1.f;
        b.lifeMin = 10.f; b.lifeMax = 10.f;
        b.sizeMin = b.sizeMax = 0.05f;
        s.burst(b);
    });
    for (int f = 0; f < 40; ++f) h.update(1.f / 60.f);
}

// Spawn-budget truncation, death/recycle churn, clear(), respawn.
// Death makes slot order scheduling-dependent (see sortSnapshot), so this
// phase compares sorted snapshots. A second GL leg proves the permutation is
// a scheduling property, not a backend difference.
void phaseRecycle(const ember::VulkanContext& ctx) {
    auto h = makeHarness(ctx, {700, 200, 0xDEC1C});
    h.phase = "recycle";
    h.unordered = true;
    h.legs.push_back({"gl-2", std::make_unique<ember::ParticleSystem>(
                                  ember::ParticleSettings{700, 200, 0xDEC1C})});
    h.all([](ember::ParticleSystem& s) { s.setForceMask(0); });
    for (int f = 0; f < 20; ++f) {
        h.all([f](ember::ParticleSystem& s) {
            ember::BurstParams b;
            b.count = 350; // above the 200/frame budget
            b.position = {float(f % 5) * 0.2f, 0.f, 0.f};
            b.speedMin = 0.f; b.speedMax = 1.f;
            b.spread = 1.f;
            b.lifeMin = 0.05f; b.lifeMax = 0.25f; // dies within a few frames
            b.sizeMin = 0.02f; b.sizeMax = 0.1f;
            s.burst(b);
        });
        h.update(0.05f);
    }
    h.all([](ember::ParticleSystem& s) { s.clear(); });
    h.update(0.05f);
    h.all([](ember::ParticleSystem& s) {
        ember::BurstParams b;
        b.count = 700; // capacity truncation
        b.speedMin = b.speedMax = 0.f;
        b.lifeMin = b.lifeMax = 5.f;
        s.burst(b);
    });
    h.update(0.05f);
    for (int f = 0; f < 5; ++f) h.update(0.05f);
}

// Quadratic drag + sizeScale with speed link + refractive sign encoding.
void phaseOptions(const ember::VulkanContext& ctx) {
    auto h = makeHarness(ctx, {512, 256, 0x0B710});
    h.phase = "options";
    h.all([](ember::ParticleSystem& s) {
        s.setForceMask((1u << 1)); // drag only
        s.setDrag(0.8f);
        s.setDragMode(ember::DragMode::Quadratic);
        s.setSizeScale(1.5f);
        ember::BurstParams b;
        b.count = 200;
        b.speedMin = 2.f; b.speedMax = 6.f;
        b.spread = 1.f;
        b.lifeMin = 3.f; b.lifeMax = 3.f;
        b.sizeMin = 0.05f; b.sizeMax = 0.15f;
        b.refractive = true; // pos.w sign-encoded, must match exactly
        s.burst(b);
    });
    for (int f = 0; f < 20; ++f) h.update(1.f / 60.f);
}

// Event sub-emission with a chained template (mother -> child -> grandchild).
// Deaths recycle slots, so the slot->particle map is a per-run permutation and
// this phase compares sorted snapshots.
void phaseEvents(const ember::VulkanContext& ctx) {
    auto h = makeHarness(ctx, {8192, 2048, 0xE7E17});
    h.phase = "events";
    h.unordered = true;
    // A fourth leg covers GL GPU-driven scheduling with events (the queue
    // reset goes through schedule.comp on that path, not the CPU).
    {
        auto glGpu = std::make_unique<ember::ParticleSystem>(
            ember::ParticleSettings{8192, 2048, 0xE7E17});
        glGpu->setGpuDriven(true);
        h.legs.push_back({"gl-gpu", std::move(glGpu)});
    }
    // Free-RNG templates: child values are seeded by the parent's BIRTH ID
    // (a slot-independent genealogy hash, see BufSlotMeta in simulate.comp),
    // so the multiset is deterministic even though queue order and slot
    // assignment are not. This is the strong form of the contract — do not
    // weaken it to fixed ranges without a documented reason.
    h.all([](ember::ParticleSystem& s) {
        s.setForceMask(0);
        ember::Emitter grandchild;
        grandchild.eventCount = 4;
        grandchild.lifeMin = 0.05f; grandchild.lifeMax = 0.15f;
        grandchild.speedMin = 0.5f; grandchild.speedMax = 2.f;
        grandchild.spread = 1.f; // uniform sphere directions from the RNG
        grandchild.sizeMin = 0.03f; grandchild.sizeMax = 0.08f;
        // Red-shifted range: the color signature the chain assertion scans for.
        grandchild.colorMin = {1.f, 0.2f, 0.f, 1.f};
        grandchild.colorMax = {1.f, 0.8f, 0.1f, 1.f};
        s.addEventEmitter(grandchild);
        ember::Emitter child;
        child.eventCount = 6;
        child.lifeMin = 0.05f; child.lifeMax = 0.1f;
        child.speedMin = 0.5f; child.speedMax = 1.5f;
        child.spread = 0.7f;
        child.baseVelocity = {0.f, 1.f, 0.f};
        child.sizeMin = 0.04f; child.sizeMax = 0.09f;
        child.palette = {{0, 1, 0.5f, 1}, {0.3f, 0.4f, 1, 1}}; // green/blue only
        child.inheritVelocity = 0.5f;
        child.onDeath = 0; // chained: children explode into grandchildren
        s.addEventEmitter(child);
        ember::Emitter mother;
        mother.rate = 0.f;
        mother.accumulator = 40.f;
        mother.baseVelocity = {1.f, 0.3f, 0.f};
        mother.lifeMin = 0.05f; mother.lifeMax = 0.08f;
        mother.speedMin = 0.3f; mother.speedMax = 0.8f;
        mother.spread = 0.4f;
        mother.sizeMin = 0.06f; mother.sizeMax = 0.1f;
        mother.onDeath = 1; // mothers explode into children
        s.addEmitter(mother);
    });
    bool sawGrandchildren = false;
    for (int f = 0; f < 30; ++f) {
        if (f % 5 == 0)
            h.all([](ember::ParticleSystem& s) { s.emitter(0)->accumulator += 40.f; });
        h.update(1.f / 60.f);
        // Chain liveness independent of the cross-leg comparison: without it
        // all legs could share the same broken chain and still agree.
        if (!sawGrandchildren)
            for (const auto& p : h.legs.front().sys->readParticles())
                if (p.color.r > 0.9f && p.color.g < 0.85f && p.color.b < 0.2f) {
                    sawGrandchildren = true;
                    break;
                }
    }
    check(sawGrandchildren, "events: chained grandchildren never appeared");
}

} // namespace

int main() {
    std::unique_ptr<ember::Window> window;
    try {
        window = std::make_unique<ember::Window>(64, 64, "ember cross backend",
                                                 0, /*vsync=*/false, /*visible=*/false);
    } catch (const std::exception& e) {
        std::printf("SKIP: cannot create GL 4.3 context: %s\n", e.what());
        return 77;
    }
    ember::VulkanDevice device;
    try {
        device = ember::makeVulkanDevice(/*debug=*/std::getenv("EMBER_VK_DEBUG") != nullptr);
    } catch (const std::exception& e) {
        std::printf("SKIP: cannot create Vulkan device: %s\n", e.what());
        return 77;
    }

    const ember::VulkanContext ctx = device.context();
    try {
        g_tally = {};
        phaseEmitters(ctx);
        reportPhase("emitters");
        phaseForces(ctx);
        reportPhase("forces");
        phaseBoundary(ctx);
        reportPhase("boundary");
        phaseRecycle(ctx);
        reportPhase("recycle");
        phaseOptions(ctx);
        reportPhase("options");
        phaseEvents(ctx);
        reportPhase("events");
        std::printf("cross backend test: ALL PASSED\n");
        return 0;
    } catch (const std::exception& e) {
        std::printf("FAIL: %s\n", e.what());
        return 1;
    }
}
