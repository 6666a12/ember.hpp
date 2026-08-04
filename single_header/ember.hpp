// ember.hpp — single-header edition of the ember GPU particle library.
//
// GENERATED FILE — do not edit by hand. Regenerate with:
//     python tools/amalgamate.py
// (after changing include/ember/*.hpp or src/*.cpp)
//
// Usage (one translation unit):
//     #define EMBER_IMPLEMENTATION
//     #include "ember.hpp"
// Optional feature macros:
//     EMBER_USE_GLFW — include the GLFW window convenience module
//     EMBER_USE_STB  — PNG sprite support; define STB_IMAGE_IMPLEMENTATION
//                      somewhere (e.g. in the same TU, before this include)
//
// External dependencies (documented in INTEGRATION docs):
//   * glad  — OpenGL 4.3 core loader; the generated gl.h must be includeable
//             as <glad/gl.h> and its implementation TU (glad.c) linked.
//   * glm   — header-only math library (>= 0.9.9).
//   * GLFW  — only with EMBER_USE_GLFW (window convenience module).
//   * stb_image.h — only with EMBER_USE_STB (PNG sprites); without it
//             setSpriteTexture falls back to the built-in gradient.
//
// License: MIT (see LICENSE).

#ifndef EMBER_SINGLE_HEADER_HPP
#define EMBER_SINGLE_HEADER_HPP
// ================= [ember/gl.hpp] =================

// GL function loading + error helpers.
// The core library never creates a GL context — the host application does
// (GLFW / SDL2 / Qt / Win32 / EGL / your engine), then hands its loader here:
//
//     ember::gl::init(reinterpret_cast<void* (*)(const char*)>(glfwGetProcAddress));
//
// Works with any loader that resolves GL function names.

#include <glad/gl.h>

#include <cstdio>

namespace ember {
namespace gl {

// Initialize GL function pointers. Must be called after a context is current.
inline int init(void* (*loader)(const char*)) {
    return gladLoadGL(reinterpret_cast<GLADloadfunc>(loader));
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

// ================= [ember/core.hpp] =================

// Core data types shared between the CPU side and the simulation.

#include <glm/glm.hpp>

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
struct Spring {
    glm::vec3 anchor{0.f};
    float stiffness = 0.f;
    float damping = 0.f;
};
static_assert(sizeof(Spring) == 20, "Spring must stay std430-compatible (20 bytes)");

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

// ================= [ember/emitters.hpp] =================

// CPU-side emitters: they generate spawn data for the GPU simulation.


#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

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
    std::int32_t hasFade;      // 128 write life.yzw = fade target
    std::uint32_t base;        // 132 prefix offset: spawns work ids [base, base+count)
    std::uint32_t count;       // 136 particles this request produces
    std::int32_t paletteCount; // 140 colors in this palette range
    glm::vec3 fadeMin;         // 144 fade target range (RGB)
    std::uint32_t pad2;        // 156
    glm::vec3 fadeMax;         // 160
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

    bool active = true;

    float accumulator = 0.f;       // fractional spawn carry-over

    // How many particles this emitter should emit this frame: rate*dt
    // accumulated, capped by `max`. Shared by the CPU path (spawn) and the
    // GPU path (request encoding in ParticleSystem::update).
    std::uint32_t takeCount(float dt, std::uint32_t max) {
        if (!active) {
            accumulator = 0.f; // re-enabling should not dump accumulated time
            return 0;
        }
        if (max == 0) return 0; // no budget this frame (e.g. bursts ate it): keep the carry
        // Cap the accumulator high enough that a long paused frame converts to
        // a single (budget-capped) burst on resume, without throttling high-rate
        // emitters. The old 1000 cap silently capped spawns at 1000/frame.
        accumulator = std::min(accumulator + rate * dt, 1000000.f);
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
            p.pos = glm::vec4(samplePosition(rng), size);
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
        r.hasFade = hasFadeColor ? 1 : 0;
        r.base = base;
        r.count = count;
        r.fadeMin = fadeColorMin;
        r.fadeMax = fadeColorMax;
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
                glm::vec3 ax = glm::normalize(axis);
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
            const glm::vec3 ax = glm::normalize(axis);
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

// ================= [ember/gpu.hpp] =================

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

// ================= [ember/shader.hpp] =================

// RAII GL program with cached uniform lookup.


#include <glm/glm.hpp>

#include <initializer_list>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

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
    GLint location(const char* name) const;

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
};

} // namespace ember

// ================= [ember/config.hpp] =================

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


#include <algorithm>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace ember {

struct Palette {
    std::string name;
    std::vector<glm::vec4> colors;
};

// Mirrors Emitter + configuration metadata (section name / referenced palette).
struct EmitterConfig : public Emitter {
    std::string name;
    std::string paletteName; // optional: [palette "..."] to take colors from
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
        float streak = 0.f;               // >0: stretch quads along velocity (trails)
        bool softParticles = false;       // fade particles near host scene depth
        float softRadius = 0.5f;
        bool bloom = false;               // additive HDR bloom post-process
        float bloomThreshold = 1.f;
        bool sort = false;                // GPU depth sort (OIT: correct alpha blending)
    };
    System system;
    std::vector<Palette> palettes;
    std::vector<EmitterConfig> emitters;
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

    static Config fromFile(const char* path) {
        std::ifstream f(path);
        if (!f) throw std::runtime_error(std::string("ember config: cannot open '") + path + "'");
        std::ostringstream ss;
        ss << f.rdbuf();
        return fromString(ss.str());
    }

    static Config fromString(const std::string& text) {
        Config cfg;
        std::istringstream in(text);
        std::string line;
        int lineNo = 0;

        enum class Section { None, System, Palette, Emitter, Attractor, Vortex, Spring };
        Section section = Section::None;
        std::string sectionName;
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
                const std::string sec = lower.substr(0, lower.find(' ')); // section keyword
                if (sec == "system") {
                    section = Section::System;
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
                } else if (sec == "palette" || sec == "emitter") {
                    const std::string arg = sectionArg(body);
                    if (sec == "palette") {
                        for (auto& p : cfg.palettes)
                            if (p.name == arg) fail("duplicate palette '" + arg + "'");
                        cfg.palettes.push_back(Palette{arg, {}});
                        pal = &cfg.palettes.back();
                        section = Section::Palette;
                    } else {
                        cfg.emitters.push_back(EmitterConfig{});
                        cfg.specified.push_back("emitter");
                        em = &cfg.emitters.back();
                        em->name = arg;
                        section = Section::Emitter;
                    }
                    sectionName = arg;
                } else {
                    fail("unknown section '" + body + "'");
                }
                continue;
            }

            const std::size_t eq = line.find('=');
            if (eq == std::string::npos) fail("expected 'key = value', got '" + line + "'");
            const std::string key = toLower(trim(line.substr(0, eq)));
            const std::string value = trim(line.substr(eq + 1));
            if (key.empty() || value.empty()) fail("empty key or value in '" + line + "'");

            // Record key presence so apply() can keep untouched state.
            const char* secName = section == Section::System ? "system"
                : section == Section::Palette ? "palette"
                : section == Section::Emitter ? "emitter"
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
                case Section::Emitter: {
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
                    else fail("unknown key '" + key + "' in [emitter]");
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
        const std::size_t q = body.find('"');
        if (q == std::string::npos) return body;
        const std::size_t q2 = body.find('"', q + 1);
        if (q2 == std::string::npos) return body;
        return body.substr(q + 1, q2 - q - 1);
    }
    static float parseFloat(const std::string& v, int line) {
        try {
            std::size_t pos = 0;
            const float f = std::stof(v, &pos);
            if (pos != v.size()) throw std::invalid_argument("trailing");
            return f;
        } catch (...) {
            throw std::runtime_error("ember config: bad number '" + v + "' (line " + std::to_string(line) + ")");
        }
    }
    static std::uint32_t parseUInt(const std::string& v, int line) {
        try {
            std::size_t pos = 0;
            const unsigned long u = std::stoul(v, &pos);
            if (pos != v.size()) throw std::invalid_argument("trailing");
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

// ================= [ember/particle_system.hpp] =================

// The particle system: GPU simulation (compute shaders) + instanced rendering.


#include <cstdint>
#include <vector>

namespace ember {

struct Config; // defined in ember/config.hpp

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

class ParticleSystem {
public:
    struct Settings {
        std::uint32_t capacity = 100000;        // max live particles
        std::uint32_t maxSpawnPerFrame = 65536; // cap on new particles per update
        std::uint32_t seed = 0;                 // 0 = random
    };

    ParticleSystem() : ParticleSystem(Settings{}) {}
    explicit ParticleSystem(const Settings& s);
    ~ParticleSystem();

    ParticleSystem(ParticleSystem&&) noexcept;
    ParticleSystem& operator=(ParticleSystem&&) noexcept;
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
    bool forceEnabled(Force f) const { return (forceMask_ & (1u << (std::uint32_t)f)) != 0; }
    void setForceMask(std::uint32_t mask);
    std::uint32_t forceMask() const { return forceMask_; }

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
    float sizeScale() const { return sizeScale_; }
    void setUseSprite(bool on);                    // texture vs procedural glow
    bool useSprite() const { return useSprite_; }
    void setSpriteTexture(const char* pngPath);    // "" or failure => built-in gradient
    void setSpriteSheet(int cols, int rows);       // (1,1) = single frame

    // ---- rendering extras ----
    void setStreak(float k);                       // >0: stretch quads along velocity (trails)
    void setSoftParticles(bool on, GLuint sceneDepthTex = 0, float radius = 0.5f);
    void setSortEnabled(bool on);                  // GPU bitonic depth sort (correct alpha blending)
    bool sortEnabled() const { return sortEnabled_; }
    void setBloom(bool on);
    bool bloom() const { return bloom_; }
    void setBloomThreshold(float t);               // bright-pass cutoff (default 1.0)

    // ---- shader sourcing ----
    // Default: load particle.vert/.frag/simulate.comp from `dir` (default
    // "shaders"); fall back to the embedded copies when the files are missing
    // or fail to compile. Custom programs set via setPrograms() always win.
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

    // ---- simulation ------------------------------------------------------------------
    void update(float dt);           // run the GPU sim (spawn + integrate + recycle)
    void burst(const BurstParams& p); // instant spawn, applied on next update()
    void clear();                    // remove all particles

    std::uint32_t aliveCount() const { return alive_; }
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

    // ---- advanced: replace the default shaders ------------------------------------------
    // Replacement programs must follow the uniform/binding contract documented in
    // shaders/simulate.comp and shaders/particle.vert.
    void setPrograms(Shader render, Shader simulate);

private:
    void ensurePrograms();
    void uploadBuiltinSprite(); // 64x64 radial gradient fallback texture
    void ensureBloom(int w, int h);
    void ensureSortProgram();
    void sortParticles(const glm::mat4& view);
    void drawParticles(const glm::mat4& view, const glm::mat4& proj,
                       float viewportWidth, float viewportHeight);
    void drawFullscreen();
    void registerEmitterPalette(Emitter& e); // upload e.palette -> GPU palette buffer
    void rebuildPaletteBuffer();             // rebuild from all live emitters

    std::uint32_t capacity_ = 0;
    std::uint32_t maxSpawnPerFrame_ = 0;
    std::uint32_t alive_ = 0;
    float time_ = 0.f;
    float maxPointSize_ = 255.f;
    bool debug_ = false;      // enabled via EMBER_DEBUG env var
    std::uint64_t frameCount_ = 0;
    std::uint32_t frameSeed_ = 1; // per-frame seed for the GPU spawn RNG

    Buffer bufA_, bufB_;            // particle ping-pong SSBOs (cur/next)
    Buffer spawnBuf_;               // GPU spawn request staging (binding 2)
    Buffer paletteBuf_;             // palette colors for GPU spawn (binding 9)
    Buffer deadBuf_;                // free-slot stack (recycled dead slots)
    Buffer counterBuf_;             // uAlive | uDeadHead | uSpawnRequestCount | uCapacity
    Buffer attractorBuf_;           // vec4 per attractor
    Buffer vortexBuf_;              // Vortex per entry (binding 6)
    Buffer springBuf_;              // Spring per entry (binding 7)
    Buffer sortedBuf_;              // GPU-sorted particle indices (binding 8)
    Buffer indirectBuf_;            // GPU-written draw args (glDrawArraysIndirect, binding 10)
    VertexArray vao_;
    Texture spriteTex_;             // particle sprite (built-in gradient or PNG)
    Buffer* cur_ = nullptr;         // sim source / render source
    Buffer* nxt_ = nullptr;

    Shader renderProg_, simProg_, sortProg_;
    std::vector<Emitter> emitters_;
    std::vector<SpawnRequest> pendingRequests_; // bursts etc., applied on next update()
    std::vector<glm::vec4> paletteData_;        // CPU mirror of the GPU palette buffer
    Rng rng_;

    glm::vec3 gravity_{0.f, -9.81f, 0.f};
    float drag_ = 0.f;
    glm::vec3 wind_{0.f};
    float turbulence_ = 0.f;
    std::vector<Attractor> attractors_;
    std::uint32_t forceMask_ = 0x1Fu; // bits 0..4 (existing forces) on by default
    DragMode dragMode_ = DragMode::Linear;
    std::vector<Vortex> vortexes_;
    std::vector<Spring> springs_;
    glm::vec3 noiseWindDir_{1.f, 0.f, 0.f};
    float noiseWindAmp_ = 0.f;
    float noiseWindScale_ = 0.4f;
    float noiseWindSpeed_ = 0.8f;
    glm::vec3 waveDir_{1.f, 0.f, 0.f};
    glm::vec3 waveK_{1.f, 0.f, 0.f};
    float waveAmp_ = 0.f;
    float waveOmega_ = 1.f;
    BoundaryMode boundaryMode_ = BoundaryMode::Kill;
    float boundaryY_ = 0.f;
    float restitution_ = 0.5f;
    float sizeScale_ = 1.f;         // render size multiplier (uSizeScale)
    float speedScaleBase_ = 1.f;    // accumulated size->speed link factor for new emitters
    bool useSprite_ = true;
    int sheetCols_ = 1, sheetRows_ = 1;
    float streak_ = 0.f;
    bool softParticles_ = false;
    GLuint sceneDepth_ = 0;
    float softRadius_ = 0.5f;
    bool sortEnabled_ = false;
    // ---- bloom ----
    bool bloom_ = false;
    float bloomThreshold_ = 1.f;
    int bloomW_ = 0, bloomH_ = 0;
    Texture bloomTex_, bloomHalfTex_, bloomBlurTex_;
    GLuint bloomFbo_ = 0, bloomHalfFbo_ = 0, bloomBlurFbo_ = 0;
    Shader bloomPass_, bloomBlur_, bloomComposite_;
    std::string shaderDir_ = "shaders";
    BlendMode blend_ = BlendMode::Additive;
    bool depthTest_ = false;
    bool depthWrite_ = false;
};

} // namespace ember


#ifdef EMBER_USE_GLFW
// ================= [ember/glfw_window.hpp] =================

// Minimal GLFW window wrapper (optional convenience module).
// Creates a 4.3 core-profile context, initializes glad, tracks input state
// and per-frame delta time. Host applications that bring their own windowing
// (SDL / Qt / Win32 / ...) do NOT need this — see INTEGRATION.md.

#ifndef GLFW_INCLUDE_NONE
#define GLFW_INCLUDE_NONE
#endif
#include <GLFW/glfw3.h>

#include <glm/glm.hpp>

#include <functional>

namespace ember {

class Window {
public:
    Window(int width, int height, const char* title, int samples = 4, bool vsync = true, bool visible = true);
    ~Window();

    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    bool shouldClose() const { return glfwWindowShouldClose(w_) != 0; }
    void pollEvents();
    void swapBuffers() { glfwSwapBuffers(w_); }
    void setTitle(const char* t) { glfwSetWindowTitle(w_, t); }
    void setVsync(bool on) { glfwSwapInterval(on ? 1 : 0); }
    void setCursorPos(glm::vec2 p) { glfwSetCursorPos(w_, p.x, p.y); }

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

    // Optional callbacks (GLFW-style signatures).
    std::function<void(int key, int scancode, int action, int mods)> onKey;
    std::function<void(double x, double y)> onCursorPos;
    std::function<void(int button, int action, int mods)> onMouseButton;
    std::function<void(double xoff, double yoff)> onScroll;
    std::function<void(int width, int height)> onResize;

private:
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

// =====================================================================
// Implementation — compile ONCE by defining EMBER_IMPLEMENTATION in a
// single translation unit (e.g. right before this include).
// =====================================================================
#ifdef EMBER_IMPLEMENTATION
#ifndef EMBER_SINGLE_HEADER_IMPLEMENTATION
#define EMBER_SINGLE_HEADER_IMPLEMENTATION
// ================= [src/shader.cpp] =================

#include <glm/gtc/type_ptr.hpp>

#include <fstream>
#include <sstream>
#include <stdexcept>

namespace ember {

namespace {

std::string readFile(const char* path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error(std::string("ember: cannot open shader file: ") + path);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

GLuint compileStage(GLenum stage, const char* src) {
    GLuint s = glCreateShader(stage);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[8192];
        GLsizei len = 0;
        glGetShaderInfoLog(s, sizeof(log), &len, log);
        std::string msg(log, len);
        glDeleteShader(s);
        throw std::runtime_error("ember: shader compile error: " + msg);
    }
    return s;
}

} // namespace

Shader::~Shader() {
    if (id_) glDeleteProgram(id_);
}

Shader::Shader(Shader&& o) noexcept : id_(o.id_), locs_(std::move(o.locs_)) { o.id_ = 0; }

Shader& Shader::operator=(Shader&& o) noexcept {
    if (this != &o) {
        if (id_) glDeleteProgram(id_);
        id_ = o.id_;
        o.id_ = 0;
        locs_ = std::move(o.locs_);
    }
    return *this;
}

Shader Shader::fromSources(const std::vector<std::pair<GLenum, const char*>>& stages) {
    GLuint prog = glCreateProgram();
    std::vector<GLuint> shaders;
    try {
        for (const auto& [stage, src] : stages) {
            GLuint s = compileStage(stage, src);
            shaders.push_back(s);
            glAttachShader(prog, s);
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

void Shader::setInt(const char* n, int v)   { glUniform1i(location(n), v); }
void Shader::setUint(const char* n, unsigned v) { glUniform1ui(location(n), v); }
void Shader::setFloat(const char* n, float v)   { glUniform1f(location(n), v); }
void Shader::setVec2(const char* n, const glm::vec2& v) { glUniform2fv(location(n), 1, glm::value_ptr(v)); }
void Shader::setVec3(const char* n, const glm::vec3& v) { glUniform3fv(location(n), 1, glm::value_ptr(v)); }
void Shader::setVec4(const char* n, const glm::vec4& v) { glUniform4fv(location(n), 1, glm::value_ptr(v)); }
void Shader::setMat4(const char* n, const glm::mat4& v) { glUniformMatrix4fv(location(n), 1, GL_FALSE, glm::value_ptr(v)); }

} // namespace ember

// ================= [src/particle_system.cpp] =================
#ifdef EMBER_USE_STB
#include "stb_image.h"
#endif // EMBER_USE_STB

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


#ifdef EMBER_USE_GLFW
// ================= [src/glfw_window.cpp] =================


#include <stdexcept>

namespace ember {

namespace {
int g_glfwRefs = 0; // refcount for glfwInit/glfwTerminate
}

Window::Window(int width, int height, const char* title, int samples, bool vsync, bool visible) {
    if (g_glfwRefs++ == 0) {
        if (!glfwInit()) {
            --g_glfwRefs;
            throw std::runtime_error("ember: glfwInit failed");
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
        if (--g_glfwRefs == 0) glfwTerminate();
        throw std::runtime_error("ember: failed to create GLFW window (needs OpenGL 4.3 core)");
    }
    glfwMakeContextCurrent(w_);
    if (!gl::init(reinterpret_cast<void* (*)(const char*)>(glfwGetProcAddress))) {
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

void Window::pollEvents() {
    glfwPollEvents();
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
    if (auto* s = fromHandle(w); s && s->onKey) s->onKey(key, sc, act, mods);
}
void Window::cursorCb(GLFWwindow* w, double x, double y) {
    if (auto* s = fromHandle(w); s && s->onCursorPos) s->onCursorPos(x, y);
}
void Window::mouseCb(GLFWwindow* w, int b, int act, int mods) {
    if (auto* s = fromHandle(w); s && s->onMouseButton) s->onMouseButton(b, act, mods);
}
void Window::scrollCb(GLFWwindow* w, double x, double y) {
    if (auto* s = fromHandle(w)) {
        s->scrollAccum_ += glm::vec2((float)x, (float)y);
        if (s->onScroll) s->onScroll(x, y);
    }
}
void Window::resizeCb(GLFWwindow* w, int width, int height) {
    if (auto* s = fromHandle(w); s && s->onResize) s->onResize(width, height);
}

} // namespace ember


#endif // EMBER_USE_GLFW

#endif // EMBER_SINGLE_HEADER_IMPLEMENTATION
#endif // EMBER_IMPLEMENTATION
