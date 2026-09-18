#pragma once
#include "common.hpp"
#include <array>
#include <unordered_map>

namespace ember::detail::vulkan {

struct FrameResources {
    VkCommandBuffer command = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    Buffer uboSim, uboSort, uboSchedule, uboDraw, uboFrag, uboBloom;
    Buffer spawnStaging;
    VkDescriptorSet simSet = VK_NULL_HANDLE;    // set 0: simulation SSBO/UBO
    VkDescriptorSet renderSet = VK_NULL_HANDLE; // set 1: render buffers/samplers/UBOs
    bool recorded = false;
    std::uint64_t submittedFrame = 0; // frameCount_ at submit time (0 = never)
};

struct PipelineKey {
    std::uint8_t blend = 0;       // 0 additive, 1 normal
    std::uint8_t depthTest = 0;
    std::uint8_t depthWrite = 0;
    std::uint8_t refraction = 0;  // refraction pass: pixel replacement, blend off
    VkRenderPass pass = VK_NULL_HANDLE; // host target pass or the bloom scene pass
    bool operator==(const PipelineKey& o) const {
        return blend == o.blend && depthTest == o.depthTest &&
               depthWrite == o.depthWrite && refraction == o.refraction && pass == o.pass;
    }
};
struct PipelineKeyHash {
    std::size_t operator()(const PipelineKey& k) const {
        const std::size_t state = (std::size_t)k.blend | ((std::size_t)k.depthTest << 2) |
                                  ((std::size_t)k.depthWrite << 3) | ((std::size_t)k.refraction << 4);
        return state ^ (std::size_t)k.pass;
    }
};
struct RenderPassKey {
    VkFormat color = VK_FORMAT_UNDEFINED;
    VkFormat depth = VK_FORMAT_UNDEFINED;
    bool operator==(const RenderPassKey& o) const { return color == o.color && depth == o.depth; }
};
struct RenderPassKeyHash {
    std::size_t operator()(const RenderPassKey& k) const {
        return (std::size_t)k.color * 31u + (std::size_t)k.depth;
    }
};
// Framebuffers bind concrete views, so they cannot be cached by format alone.
struct FramebufferKey {
    VkImageView color = VK_NULL_HANDLE;
    VkImageView depth = VK_NULL_HANDLE;
    std::uint32_t width = 0, height = 0;
    bool operator==(const FramebufferKey& o) const {
        return color == o.color && depth == o.depth && width == o.width && height == o.height;
    }
};
struct FramebufferKeyHash {
    std::size_t operator()(const FramebufferKey& k) const {
        return (std::size_t)k.color ^ ((std::size_t)k.depth << 1) ^
               ((std::size_t)k.width << 32) ^ (std::size_t)k.height;
    }
};

// Vulkan particle backend. Stages: simulation and synchronous statistics
// (WO-02), host-target billboard rendering with the built-in sprite (WO-03),
// GPU scheduling + asynchronous fence-ring statistics (WO-04), depth sorting
// (WO-05) and the optional effects (WO-06).
class VulkanBackend final : public ParticleBackend {
public:
    VulkanBackend();
    ~VulkanBackend() override;
    VulkanBackend(const VulkanBackend&) = delete;
    VulkanBackend& operator=(const VulkanBackend&) = delete;

    const char* name() const noexcept override { return "vulkan"; }
    BackendCapabilities capabilities() const noexcept override;
    void initialize(std::uint32_t capacity, bool debug) override;
    void validateConfiguration(std::uint32_t capacity, bool sorting) override;
    void resize(std::uint32_t capacity) override;
    void clear() override;
    void setDebug(bool enabled) override { debug_ = enabled; }
    bool gpuDriven() const override { return gpuDriven_; }
    void setGpuDriven(bool on) override;
    bool sortEnabled() const override { return sortEnabled_; }
    void setSortEnabled(bool on) override;
    void uploadAttractors(const std::vector<Attractor>& values) override;
    void uploadVortexes(const std::vector<Vortex>& values) override;
    void uploadSprings(const std::vector<Spring>& values) override;
    void uploadPalette(const std::vector<glm::vec4>& values) override;
    void uploadEventTemplates(const std::vector<SpawnRequest>& templates) override;
    void uploadLifeCurves(const LifeCurvesLut& lut) override;
    void update(const SimulationParameters&, const UpdateBatch&) override;
    void render(const RenderParameters&, const RenderView&) override;
    ParticleStatistics pollStatistics() override;
    ParticleStatistics synchronizeStatistics() const override;
    std::vector<Particle> readParticles(std::uint32_t max) const override;
    std::uint64_t updateSequence() const override { return frameCount_; }
    std::uint64_t droppedStatistics() const override { return droppedStatistics_; }
    bool bloom() const override { return bloom_; }
    void setBloom(bool on) override { bloom_ = on; }
    void setSpriteTexture(const char* path) override;
    void setShaderDirectory(const char* dir) override { shaderDir_ = dir ? dir : "shaders"; }
    SpirvBlob loadShader(const char* name) const;

    // Injected once by setVulkanContext(); borrowed, never destroyed here.
    void setContext(const VulkanContext& context);
    const VulkanContext& context() const { return context_; }
    void setFrameTarget(const VulkanFrameTarget& target);
    // Host command-buffer recording window (WO-10). See beginVulkanFrame.
    void beginFrame(VkCommandBuffer cmd, std::uint32_t frameIndex);
    void endFrame();
    bool hostMode() const { return hostCmd_ != VK_NULL_HANDLE; }
    // Borrowed host textures for the effects stage (WO-06 wires them into the
    // render descriptor set); stored now so the public adapters link.
    void setSoftDepth(VkImageView depth, VkSampler sampler) {
        sceneDepthView_ = depth; sceneDepthSampler_ = sampler;
    }
    void setRefractionInputs(VkImageView color, VkImageView depth, VkSampler sampler) {
        sceneColorView_ = color; refrDepthView_ = depth; refrSampler_ = sampler;
    }

private:
    // ---- setup / teardown (resources.cpp) ----
    void createDeviceObjects();
    void createPerFrame();
    void destroyBuffers();
    void destroyPerFrameBuffers();
    void ensureHostBuffer(Buffer&, VkDeviceSize bytes);
    void initializeDevice();
    void allocateBuffers(std::uint32_t capacity);
    void createSimPipeline();
    void createRenderPipeline();
    void updateSimSet(std::uint32_t frameIndex);
    void updateRenderSet(std::uint32_t frameIndex);
    void writeBuffer(const Buffer&, VkDeviceSize offset, VkDeviceSize bytes, const void* src);
    Buffer makeBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags props);
    void queueWaitIdle() const;
    void waitFrame(std::uint32_t index);
    // The command buffer a recording pass should target: the host's inside a
    // begin/endVulkanFrame window, otherwise this frame slot's own buffer.
    VkCommandBuffer activeCmd(std::uint32_t slot) const;
    std::uint32_t activeSlot() const { return hostMode() ? hostFrameSlot_ : frameIndex_; }
    VkShaderModule makeModule(const char* name);
    VkRenderPass renderPass(VkFormat color, VkFormat depth);
    VkPipeline particlePipeline(PipelineKey key);
    void clearParticlePipelines();

    // ---- render (render.cpp) ----
    void drawParticles(const RenderParameters&, const RenderView&, bool refractionPass,
                       VkRenderPass pass, VkFramebuffer framebuffer);
    void ensureRenderResources();
    void destroyRenderResources();

    // ---- sprite (sprite.cpp) ----
    void uploadBuiltinSprite();
    void uploadSprite(std::uint32_t width, std::uint32_t height, const unsigned char* rgba);
    void ensureSpriteSampler();

    // ---- counters / readback (statistics.cpp) ----
    void refreshCounts() const;
    void resetStatistics();
    void enqueueStatistics();
    void drainStatistics(bool wait);
    void destroyStatisticsSlots();
    void scheduleGpu(std::int32_t phase, std::uint32_t requests = 0,
                     std::uint32_t frameSlot = kFramesInFlight);
    void validateGpuCapacity(std::uint32_t capacity) const;

    // ---- sorting (sort.cpp) ----
    void createSortPipeline();
    // Pre-record phase: key-buffer (re)allocation + scratch descriptor rebind.
    // vkUpdateDescriptorSets is illegal while the frame's command buffer is
    // recording, so render() calls this before vkBeginCommandBuffer.
    void sortPrepare(std::uint32_t frameSlot);
    void sortParticles(const glm::mat4& view, VkCommandBuffer cmd, std::uint32_t frameSlot);

    // ---- bloom (bloom.cpp) ----
    void ensureBloom(std::uint32_t w, std::uint32_t h);
    void destroyBloomResources();
    // Writes the four per-pass bloom sets; pre-record only (render()).
    void updateBloomSets(std::uint32_t frameSlot);
    void createBloomPipeline();
    void runBloom(VkCommandBuffer cmd, float threshold);

    VulkanContext context_{};
    std::uint32_t capacity_ = 0;
    bool debug_ = false;
    std::uint64_t frameCount_ = 0;
    std::string shaderDir_ = "shaders";
    DeviceLimits limits_{};

    // CPU-side copies of uploaded arrays (setters must not retain borrows).
    std::vector<Attractor> attractors_;
    std::vector<Vortex> vortexes_;
    std::vector<Spring> springs_;
    std::vector<glm::vec4> palette_;
    std::vector<SpawnRequest> eventTemplates_; // CPU copy of the uploaded templates

    // Device objects.
    VkCommandPool commandPool_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout simSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout renderSetLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout simPipelineLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout renderPipelineLayout_ = VK_NULL_HANDLE;

    VkPipeline simPipeline_ = VK_NULL_HANDLE;
    VkPipeline sortPipeline_ = VK_NULL_HANDLE;
    VkPipeline schedulePipeline_ = VK_NULL_HANDLE;
    VkShaderModule simModule_ = VK_NULL_HANDLE;
    VkShaderModule sortModule_ = VK_NULL_HANDLE;
    VkShaderModule scheduleModule_ = VK_NULL_HANDLE;
    VkShaderModule vertModule_ = VK_NULL_HANDLE;
    VkShaderModule fragModule_ = VK_NULL_HANDLE;

    // Persistent SSBOs (simulation set 0, bindings 0-13).
    Buffer bufA_, bufB_;          // particle ping-pong
    Buffer spawnBuf_;             // GPU spawn requests (device local)
    Buffer paletteBuf_;           // palette colors (host visible)
    Buffer attractorBuf_, vortexBuf_, springBuf_;
    Buffer deadBuf_;              // free-slot stack
    Buffer counterBuf_;           // 8 x uint
    Buffer liveBuf_, nextLiveBuf_;
    Buffer sortedBuf_;            // sorted particle indices (render set 1)
    Buffer sortKeyBuf_;           // cached depth keys during tiled sort (binding 12)
    Buffer indirectBuf_;          // draw args {4,0,0,0}
    Buffer scheduleBuf_;          // GPU scheduling metadata + indirect commands (binding 13)
    Buffer eventTagBuf_;          // per-slot metadata uvec2: event tag + birth id (binding 20)
    Buffer eventTemplateBuf_;     // event templates, SpawnRequest[] (binding 21)
    Buffer eventQueueBuf_;        // GPU event queue, header + GpuEvent[] (binding 22)
    Buffer readbackBuf_;          // host-visible statistics copy
    Buffer* cur_ = nullptr;
    Buffer* nxt_ = nullptr;
    std::uint32_t sortKeyCapacity_ = 0; // lazily allocated padded key count

    // GPU-driven state (WO-04): schedule.comp generates integration commands;
    // statistics are sampled asynchronously through a 4-slot fence ring.
    bool gpuDriven_ = false;
    bool sortEnabled_ = false;
    // GPU-mode sort commands are produced by schedule.comp; this tracks whether
    // the schedule buffer matches the current state (invalidated by enabling
    // sort / resize / clear so render() can regenerate it lazily).
    bool sortScheduleValid_ = false;
    struct StatisticsSlot {
        Buffer readback;              // HOST_VISIBLE 5 x uint
        std::uint64_t frame = 0;      // updateSequence() this sample belongs to
        std::uint32_t frameSlot = 0;  // frame ring slot whose fence gates it
        std::uint64_t submission = 0; // frames_[frameSlot].submittedFrame at enqueue
        bool pending = false;
        bool hostSample = false; // recorded into a host command buffer (H1)
    };
    std::array<StatisticsSlot, 4> statisticsSlots_{};

    // Render set 1 resources.
    Buffer curvesBuf_;            // baked lifecycle-curve LUTs (render binding 8)
    LifeCurvesLut curvesLut_{};   // CPU copy; re-uploaded after resize()
    VkSampler spriteSampler_ = VK_NULL_HANDLE;
    VkImageView spriteView_ = VK_NULL_HANDLE;
    VkImage spriteImage_ = VK_NULL_HANDLE;
    VkDeviceMemory spriteMemory_ = VK_NULL_HANDLE;
    // Borrowed host textures (WO-06 effects); never destroyed here.
    VkImageView sceneDepthView_ = VK_NULL_HANDLE, refrDepthView_ = VK_NULL_HANDLE, sceneColorView_ = VK_NULL_HANDLE;
    VkSampler sceneDepthSampler_ = VK_NULL_HANDLE, refrSampler_ = VK_NULL_HANDLE;

    // Render target / passes.
    VulkanFrameTarget target_{};
    VkRenderPass currentPass_ = VK_NULL_HANDLE;
    VkFramebuffer targetFramebuffer_ = VK_NULL_HANDLE;
    FramebufferKey targetFramebufferKey_{};
    bool haveTargetFramebuffer_ = false;
    RenderPassKey targetPassKey_{VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED};

    std::unordered_map<PipelineKey, VkPipeline, PipelineKeyHash> particlePipelines_;
    std::unordered_map<RenderPassKey, VkRenderPass, RenderPassKeyHash> renderPasses_;

    // ---- bloom (bloom.cpp) ----
    bool bloom_ = false;
    std::uint32_t bloomW_ = 0, bloomH_ = 0;
    struct BloomImage {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkFramebuffer framebuffer = VK_NULL_HANDLE;
        VkRenderPass pass = VK_NULL_HANDLE;
        VkSampler sampler = VK_NULL_HANDLE;
    };
    BloomImage bloomFull_, bloomHalf_, bloomBlur_;      // RGBA16F intermediates
    bool bloomHasDepth_ = false;                        // host depth used directly as attachment
    VkShaderModule bloomVertModule_ = VK_NULL_HANDLE;
    VkShaderModule bloomThresholdModule_ = VK_NULL_HANDLE;
    VkShaderModule bloomBlurModule_ = VK_NULL_HANDLE;
    VkShaderModule bloomCompositeModule_ = VK_NULL_HANDLE;
    VkPipeline bloomThresholdPipeline_ = VK_NULL_HANDLE;
    VkPipeline bloomBlurPipeline_ = VK_NULL_HANDLE;
    VkPipeline bloomCompositePipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout bloomPipelineLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout bloomSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool bloomPool_ = VK_NULL_HANDLE;
    // One descriptor set per bloom pass source (full -> half -> blur -> half):
    // sets are written once at (re)creation, never during command recording.
    VkDescriptorSet bloomSets_[4] = {};
    // Offscreen bloom target: the particle pass renders here (HDR) when bloom
    // is active, mirroring the GL bloomFbo_ chain.
    VkRenderPass bloomScenePass_ = VK_NULL_HANDLE;
    VkFormat bloomDepthFormat_ = VK_FORMAT_UNDEFINED;

    std::array<FrameResources, kFramesInFlight> frames_{};
    std::uint32_t frameIndex_ = 0;
    // Host-recording window (WO-10): non-null between beginVulkanFrame and
    // endVulkanFrame; the backend does not submit while it is set.
    VkCommandBuffer hostCmd_ = VK_NULL_HANDLE;
    // Host work was recorded whose completion the backend never observed.
    // Guards internal exact reads that bypass the host-wait contract
    // (setGpuDriven(false)'s refreshCounts). mutable: cleared by const
    // completion-observing reads (readParticles' queue drain).
    mutable bool hostDirty_ = false;
    std::uint32_t hostFrameSlot_ = 0;

    // Dirty flags coalesce uploads until the next update().
    bool paletteDirty_ = false, attractorsDirty_ = false;
    bool vortexesDirty_ = false, springsDirty_ = false;
    bool eventTemplatesDirty_ = false;
    std::uint32_t eventTemplateCount_ = 0; // active templates (0 = events off)
    // File-backed SPIR-V modules; loadShader() returns a view into these.
    mutable std::vector<std::vector<unsigned char>> shaderFiles_;
    mutable std::uint32_t alive_ = 0;
    mutable std::uint32_t allocated_ = 0;
    mutable bool countsFresh_ = true;
    mutable ParticleStatistics statistics_{};
    mutable std::uint64_t droppedStatistics_ = 0;
};
} // namespace ember::detail::vulkan
