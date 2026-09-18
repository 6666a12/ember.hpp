#pragma once

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
