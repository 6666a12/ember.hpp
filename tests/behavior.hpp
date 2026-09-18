#pragma once

// Backend-agnostic behavior suite. Drives ParticleSystem exclusively through
// the public, backend-independent API (update/burst/clear/statistics/
// readParticles) — no graphics-API headers, no GL buffer snooping. Any
// backend can run these checks by supplying a factory; the OpenGL tests call
// run() with the default GL adapter, a future Vulkan backend with its own.
//
// Covered contract points (docs/backends.md "后端必须保持的行为"):
//   - birth budgets and capacity truncation are honored by the backend
//   - dead particles retire and stay retired; readback is finite and alive-only
//   - clear() wipes particles but preserves the update sequence
//   - updateSequence advances once per successful update, including empty frames
//   - pollStatistics never blocks and stays within capacity; synchronize is exact
//   - dt validation rejects negative / non-finite values
//   - emitter rate accumulation across frames

// Modular builds include the facade directly; the single-header test already
// has every declaration from ember.hpp (ember/system.hpp is not on its path).
#if __has_include("ember/system.hpp")
#include "ember/system.hpp"
#include "ember/config.hpp"
#endif

#include <cmath>
#include <cstdio>
#include <functional>
#include <limits>
#include <stdexcept>
#include <vector>

namespace behavior {

inline void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}

using Factory = std::function<ember::ParticleSystem(const ember::ParticleSettings&)>;

inline std::vector<ember::Particle> particles(const ember::ParticleSystem& sys) {
    auto ps = sys.readParticles();
    require(ps.size() == sys.aliveCount(), "readback count differs from population");
    for (const auto& p : ps) {
        for (const auto& v : {p.pos, p.vel, p.life, p.color})
            for (int i = 0; i < 4; ++i) require(std::isfinite(v[i]), "nonfinite particle");
        require(p.life.x >= 0 && p.vel.w < p.life.x, "readback contains retired particle");
    }
    return ps;
}

inline ember::BurstParams burst(std::uint32_t count, float life = 10.f) {
    ember::BurstParams b;
    b.count = count;
    b.speedMin = b.speedMax = 0;
    b.sizeMin = b.sizeMax = 1;
    b.lifeMin = b.lifeMax = life;
    return b;
}

inline void lifecycle(const Factory& make) {
    auto sys = make({512, 512, 42});
    sys.setForceMask(0);
    sys.burst(burst(100, 0.5f));
    sys.update(0);
    require(sys.aliveCount() == 100, "burst spawn count");
    particles(sys);
    for (int i = 0; i < 10; ++i) sys.update(0.06f); // 0.6s > 0.5s lifetime
    require(sys.aliveCount() == 0, "particles did not retire");
    sys.burst(burst(512, 5));
    sys.update(0);
    require(sys.aliveCount() == 512, "respawn after full death");
    require(particles(sys).size() == 512, "respawn readback");
}

inline void capacityAndBudget(const Factory& make) {
    auto sys = make({300, 1000, 7});
    sys.setForceMask(0);
    sys.burst(burst(1000));
    sys.update(0);
    require(sys.aliveCount() == 300, "capacity truncation");
    require(particles(sys).size() == 300, "capacity readback");

    auto sys2 = make({1000, 64, 7});
    sys2.setForceMask(0);
    sys2.burst(burst(500)); // per-frame budget is 64
    sys2.update(0);
    require(sys2.aliveCount() == 64, "spawn budget truncation");
}

inline void clearAndStatistics(const Factory& make) {
    auto sys = make({256, 128, 9});
    sys.setForceMask(0);
    sys.burst(burst(100));
    sys.update(0);
    const std::uint64_t seq0 = sys.updateSequence();
    sys.clear();
    require(sys.aliveCount() == 0, "clear population");
    sys.update(0.01f);
    require(sys.updateSequence() == seq0 + 1, "update sequence advance after clear");
    require(sys.synchronizeStatistics().alive == 0, "synchronized statistics after clear");
    const auto polled = sys.pollStatistics(); // may lag, must stay sane
    require(polled.alive <= sys.capacity() && polled.allocated <= sys.capacity(),
            "polled statistics out of range");
    sys.burst(burst(20));
    sys.update(0);
    require(particles(sys).size() == 20, "spawn after clear");
}

inline void dtValidation(const Factory& make) {
    auto sys = make({64, 64, 1});
    bool thrown = false;
    try { sys.update(-1.f); } catch (const std::invalid_argument&) { thrown = true; }
    require(thrown, "negative dt accepted");
    thrown = false;
    try { sys.update(std::numeric_limits<float>::quiet_NaN()); }
    catch (const std::invalid_argument&) { thrown = true; }
    require(thrown, "NaN dt accepted");
    sys.update(0); // dt = 0 is legal and still advances the sequence
}

inline void emitterAccumulation(const Factory& make) {
    auto sys = make({1000, 1000, 3});
    sys.setForceMask(0);
    auto& e = sys.addEmitter();
    e.rate = 100.f;
    e.lifeMin = e.lifeMax = 50;
    e.speedMin = e.speedMax = 0;
    e.sizeMin = e.sizeMax = 1;
    sys.update(0.05f);
    require(sys.aliveCount() == 5, "emitter rate accumulation");
    sys.update(0.05f);
    require(sys.aliveCount() == 10, "emitter cumulative spawn");
    sys.removeEmitter(0);
    sys.update(0.05f);
    require(sys.aliveCount() == 10, "removed emitter kept spawning");
}

inline void resizeAndConfig(const Factory& make) {
    auto sys = make({128, 128, 5});
    sys.setForceMask(0);
    sys.burst(burst(100));
    sys.update(0);
    require(sys.aliveCount() == 100, "pre-resize population");
    // Capacity change through Config triggers backend resize(): particles are
    // wiped, options survive, and the new capacity bounds the next spawn.
    sys.apply(ember::Config::fromString("[system]\ncapacity=64\n"));
    require(sys.capacity() == 64, "resize capacity");
    require(sys.aliveCount() == 0, "resize must wipe particles");
    sys.burst(burst(100));
    sys.update(0);
    require(sys.aliveCount() == 64, "post-resize capacity truncation");
    require(particles(sys).size() == 64, "post-resize readback");
}

inline void events(const Factory& make) {
    auto sys = make({4096, 4096, 77});
    if (!sys.backendCapabilities().events) return; // no GPU sub-emission
    sys.setForceMask(0);

    // onDeath: 10 mothers die -> 12 children each, in the same frame.
    ember::Emitter spark;
    spark.eventCount = 12;
    spark.lifeMin = spark.lifeMax = 10.f;
    spark.speedMin = spark.speedMax = 0.f;
    spark.sizeMin = spark.sizeMax = 0.5f;
    sys.addEventEmitter(spark);

    auto& mother = sys.addEmitter();
    mother.rate = 0.f;
    mother.accumulator = 10.f; // emits 10 on the first update
    mother.lifeMin = mother.lifeMax = 0.05f;
    mother.speedMin = mother.speedMax = 0.f;
    mother.sizeMin = mother.sizeMax = 0.5f;
    mother.onDeath = 0;
    sys.update(0);
    require(sys.aliveCount() == 10, "event mothers spawned");
    sys.update(0.1f); // update() clamps dt to 0.1; age past the 0.05 lifetime
    require(sys.aliveCount() == 120, "onDeath sub-emission count");
    require(particles(sys).size() == 120, "event children readback");

    // Clearing templates detaches links: the next deaths spawn nothing.
    sys.clearEventEmitters();
    auto& lone = sys.addEmitter();
    lone.rate = 0.f;
    lone.accumulator = 5.f;
    lone.lifeMin = lone.lifeMax = 0.05f;
    lone.speedMin = lone.speedMax = 0.f;
    sys.update(0);
    sys.update(0.1f);
    require(sys.aliveCount() == 120, "cleared event templates still fired");

    // onBounce: every boundary contact triggers children.
    auto sys2 = make({1024, 1024, 91});
    if (!sys2.backendCapabilities().events) return;
    ember::Emitter puff;
    puff.eventCount = 2;
    puff.lifeMin = puff.lifeMax = 10.f;
    puff.speedMin = puff.speedMax = 0.f;
    puff.sizeMin = puff.sizeMax = 0.2f;
    sys2.addEventEmitter(puff);
    auto& ball = sys2.addEmitter();
    ball.rate = 0.f;
    ball.accumulator = 3.f;
    ball.lifeMin = ball.lifeMax = 5.f;
    ball.speedMin = ball.speedMax = -2.f;
    ball.sizeMin = ball.sizeMax = 0.1f;
    ball.onBounce = 0;
    sys2.setBoundary(ember::BoundaryMode::Bounce, -0.5f, 0.f);
    sys2.update(0);
    require(sys2.aliveCount() == 3, "event balls spawned");
    for (int i = 0; i < 20; ++i) sys2.update(0.02f);
    require(sys2.aliveCount() > 3, "onBounce sub-emission");
}

inline void run(const Factory& make) {
    lifecycle(make);
    capacityAndBudget(make);
    clearAndStatistics(make);
    resizeAndConfig(make);
    dtValidation(make);
    emitterAccumulation(make);
    events(make);
    std::printf("behavior suite (backend-agnostic): ALL PASSED\n");
}

} // namespace behavior
