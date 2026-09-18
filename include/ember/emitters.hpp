#pragma once

// CPU-side emitters: they generate spawn data for the GPU simulation.

#include "ember/core.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
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
