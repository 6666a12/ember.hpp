#pragma once

// The particle system: GPU simulation (compute shaders) + instanced rendering.

#include "ember/core.hpp"
#include "ember/emitters.hpp"
#include "ember/gpu.hpp"
#include "ember/shader.hpp"

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
