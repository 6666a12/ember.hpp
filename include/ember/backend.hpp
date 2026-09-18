#pragma once
// Particle-level backend contract. Native devices, images, command buffers and
// synchronization objects belong to backend-specific adapters, never this API.
#include "ember/particle_types.hpp"
#include <memory>
#include <stdexcept>
#include <string>

namespace ember {
struct BackendCapabilities {
    // Implementation support, not a promise that every device/shader/config
    // accepts the feature. Enabling still validates backend-specific limits.
    bool gpuScheduling = false;
    bool sorting = false;
    bool bloom = false;
    bool refraction = false;
    bool softParticles = false;
    bool spriteTextures = false;
    bool events = false; // GPU death/bounce sub-emission
    bool lifeCurves = false; // color-over-life / size-over-life LUTs
};
class ParticleBackend {
public:
    virtual ~ParticleBackend() = default;
    virtual const char* name() const noexcept = 0;
    virtual BackendCapabilities capabilities() const noexcept = 0;
    // One owner, externally serialized calls. initialize is called exactly once
    // by ParticleSystem; destruction must retire the backend's outstanding work.
    virtual void initialize(std::uint32_t capacity, bool debug) = 0;
    // Validate limits before config application; no particle/option mutations.
    virtual void validateConfiguration(std::uint32_t capacity, bool sorting) = 0;
    // Both discard particles and invalidate pending statistics, preserving the
    // update sequence and options. resize also changes storage capacity.
    virtual void resize(std::uint32_t capacity) = 0;
    virtual void clear() = 0;
    virtual void setDebug(bool enabled) = 0;
    virtual void uploadAttractors(const std::vector<Attractor>& values) = 0;
    virtual void uploadVortexes(const std::vector<Vortex>& values) = 0;
    virtual void uploadSprings(const std::vector<Spring>& values) = 0;
    virtual void uploadPalette(const std::vector<glm::vec4>& values) = 0;
    // Borrowed inputs are valid only during the call. Copy/upload before return.
    // Successful update advances sequence once, including dt=0 / empty frames.
    // Calls observe earlier calls in order. Completion may be asynchronous;
    // resource retirement and visibility barriers are the backend's job.
    virtual void update(const SimulationParameters&, const UpdateBatch&) = 0;
    virtual void render(const RenderParameters&, const RenderView&) = 0;
    virtual ParticleStatistics pollStatistics() = 0; // never wait on unfinished GPU work
    virtual ParticleStatistics synchronizeStatistics() const = 0; // exact, may wait
    virtual std::vector<Particle> readParticles(std::uint32_t max) const = 0;
    virtual std::uint64_t updateSequence() const = 0;
    virtual std::uint64_t droppedStatistics() const = 0;
    virtual bool gpuDriven() const { return false; }
    virtual void setGpuDriven(bool on) { if(on) unsupported("GPU scheduling"); }
    virtual bool sortEnabled() const { return false; }
    virtual void setSortEnabled(bool on) { if(on) unsupported("sorting"); }
    virtual bool bloom() const { return false; }
    virtual void setBloom(bool on) { if(on) unsupported("bloom"); }
    virtual void setSpriteTexture(const char*) { unsupported("sprite loading"); }
    virtual void setShaderDirectory(const char*) { unsupported("shader directory"); }
    // Event templates encoded as SpawnRequests (base/count filled, position
    // ignored). Default: any non-empty list is rejected; an empty list only
    // asks the backend to release its resources.
    virtual void uploadEventTemplates(const std::vector<SpawnRequest>& templates) {
        if (!templates.empty()) unsupported("event emission");
    }
    // Baked 64-entry lifecycle-curve LUTs (mask != 0 enables a channel).
    virtual void uploadLifeCurves(const LifeCurvesLut& lut) {
        if (lut.mask != 0) unsupported("life curves");
    }
protected:
    [[noreturn]] static void unsupported(const char* feature) {
        throw std::invalid_argument(std::string("ember: backend does not support ")+feature);
    }
};
} // namespace ember
