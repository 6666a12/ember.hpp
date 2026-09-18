#pragma once
#include "ember/opengl.hpp"
#include <array>
#include <string>

namespace ember::detail::opengl {
class OpenGLBackend final : public ParticleBackend {
public:
    OpenGLBackend();
    ~OpenGLBackend() override;
    OpenGLBackend(const OpenGLBackend&) = delete;
    OpenGLBackend& operator=(const OpenGLBackend&) = delete;
    const char* name() const noexcept override { return "opengl"; }
    BackendCapabilities capabilities() const noexcept override { return {true,true,true,true,true,true,true,true}; }
    void initialize(std::uint32_t capacity, bool debug) override;
    void validateConfiguration(std::uint32_t capacity, bool sorting) override;
    void resize(std::uint32_t capacity) override;
    void clear() override;
    void setDebug(bool enabled) override { debug_=enabled; }
    void uploadAttractors(const std::vector<Attractor>&) override;
    void uploadVortexes(const std::vector<Vortex>&) override;
    void uploadSprings(const std::vector<Spring>&) override;
    void uploadPalette(const std::vector<glm::vec4>&) override;
    void uploadEventTemplates(const std::vector<SpawnRequest>&) override;
    void uploadLifeCurves(const LifeCurvesLut&) override;
    void update(const SimulationParameters&, const UpdateBatch&) override;
    void render(const RenderParameters&, const RenderView&) override;
    ParticleStatistics pollStatistics() override;
    ParticleStatistics synchronizeStatistics() const override;
    std::vector<Particle> readParticles(std::uint32_t max) const override;
    std::uint64_t updateSequence() const override { return frameCount_; }
    std::uint64_t droppedStatistics() const override { return droppedStatistics_; }
    bool gpuDriven() const override { return gpuDriven_; }
    void setGpuDriven(bool) override;
    bool sortEnabled() const override { return sortEnabled_; }
    void setSortEnabled(bool) override;
    bool bloom() const override { return bloom_; }
    void setBloom(bool on) override { bloom_=on; }
    void setSpriteTexture(const char*) override;
    void setShaderDirectory(const char*) override;
    void setPrograms(Shader,Shader);
    void setRefractionInputs(GLuint color,GLuint depth) { sceneColor_=color;refractionDepth_=depth; }
    void setSoftDepth(GLuint depth) { sceneDepth_=depth; }
private:
    void refreshCounts() const;
    void resetStatistics();
    void enqueueStatistics();
    void scheduleGpu(int phase, std::uint32_t requests = 0);
    void validateGpuCapacity(std::uint32_t) const;
    void ensurePrograms();
    void uploadBuiltinSprite();
    void ensureBloom(int w,int h);
    void ensureSortProgram();
    void sortParticles(const glm::mat4& view);
    void drawParticles(const RenderParameters&,const glm::mat4&,const glm::mat4&,float,float,bool refractionPass=false);
    void drawFullscreen();
    using Statistics = ParticleStatistics;
    std::uint32_t capacity_ = 0;
    mutable std::uint32_t alive_ = 0;
    mutable std::uint32_t allocated_ = 0; // exact only when countsFresh_
    bool debug_ = false;      // enabled via EMBER_DEBUG env var
    std::uint64_t frameCount_ = 0;
    bool gpuDriven_ = false;
    mutable bool countsFresh_ = true;
    mutable Statistics statistics_{};
    std::uint64_t droppedStatistics_ = 0;
    std::array<GLuint, 4> statisticsBuffers_{};
    std::array<GLsync, 4> statisticsFences_{};
    std::array<std::uint64_t, 4> statisticsFrames_{};

    Buffer bufA_, bufB_;            // particle ping-pong SSBOs (cur/next)
    Buffer spawnBuf_;               // GPU spawn request staging (binding 2)
    Buffer paletteBuf_;             // palette colors for GPU spawn (binding 9)
    Buffer deadBuf_;                // free-slot stack (recycled dead slots)
    Buffer counterBuf_;             // five counters + three batch reservation words (32 B)
    Buffer attractorBuf_;           // vec4 per attractor
    Buffer vortexBuf_;              // Vortex per entry (binding 6)
    Buffer springBuf_;              // Spring per entry (binding 7)
    Buffer sortedBuf_;              // GPU-sorted particle indices (binding 8)
    Buffer liveBuf_;                // dense live-slot indices (binding 11)
    Buffer nextLiveBuf_;            // next live list during optimized sim (binding 12)
    Buffer sortKeyBuf_;             // cached depth keys during tiled sort (binding 12)
    Buffer indirectBuf_;            // GPU-written draw args (glDrawArraysIndirect, binding 10)
    Buffer dispatchBuf_;            // GPU scheduling metadata and dispatch commands (binding 13)
    Buffer eventTagBuf_;            // per-slot metadata uvec2: event tag + birth id (binding 20)
    Buffer eventTemplateBuf_;       // event templates, SpawnRequest[] (binding 21)
    Buffer eventQueueBuf_;          // GPU event queue, header + GpuEvent[] (binding 22)
    Buffer curvesBuf_;              // baked lifecycle-curve LUTs (binding 23)
    Buffer uboSim_, uboSort_, uboSchedule_, uboDraw_, uboFrag_, uboBloom_; // std140 parameter blocks
    VertexArray vao_;
    Texture spriteTex_;             // particle sprite (built-in gradient or PNG)
    Buffer* cur_ = nullptr;         // sim source / render source
    Buffer* nxt_ = nullptr;

    Shader renderProg_, simProg_, sortProg_;
    Shader scheduleProg_;
    std::uint32_t sortKeyCapacity_ = 0; // lazily allocated padded key count
    // ---- bloom ----
    bool bloom_ = false;
    int bloomW_ = 0, bloomH_ = 0;
    Texture bloomDepthTex_;         // resolved host depth for bloom occlusion
    Texture bloomTex_, bloomHalfTex_, bloomBlurTex_;
    GLuint bloomFbo_ = 0, bloomHalfFbo_ = 0, bloomBlurFbo_ = 0;
    Shader bloomPass_, bloomBlur_, bloomComposite_;
    std::string shaderDir_ = "shaders";

    bool sortEnabled_ = false;
    GLuint sceneColor_ = 0, refractionDepth_ = 0, sceneDepth_ = 0;
    std::uint32_t eventTemplateCount_ = 0; // active templates (0 = events off)
};
} // namespace ember::detail::opengl
