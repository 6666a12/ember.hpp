#pragma once
// Backend-independent ParticleSystem facade. Legacy OpenGL users may continue
// including ember/particle_system.hpp for the old transitive GL types.
#include "ember/backend.hpp"
#include <memory>
#include <vector>

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
