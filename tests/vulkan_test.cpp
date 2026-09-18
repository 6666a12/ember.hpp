// Vulkan backend test. Requires a working Vulkan 1.1 device; when no
// device/driver is available it prints SKIP and exits with 77 (ctest
// SKIP_RETURN_CODE), mirroring the GL system test's headless behavior.
#include "ember/vulkan.hpp"
#include "ember/system.hpp"
#include "behavior.hpp"
#ifdef EMBER_VULKAN_TEST_HAS_GL
#include "ember/glfw_window.hpp"
#include "ember/particle_system.hpp"
#endif

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>
#include <glm/gtc/matrix_transform.hpp>

namespace {
void check(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
// Local error-checking shim: the backend's VK_CHECK is internal to its sources.
void checkVk(VkResult result, const char* what) {
    if (result != VK_SUCCESS) throw std::runtime_error(std::string("vulkan call failed: ") + what);
}
#define VK_CHECK(expr) checkVk((expr), #expr)

// Owned offscreen render target for the pixel tests: RGBA8 color + D32 depth,
// both transitioned to the layouts ember expects around render().
struct OffscreenTarget {
    VkDevice device = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    std::uint32_t width = 64, height = 64;

    VkImage color = VK_NULL_HANDLE, depth = VK_NULL_HANDLE;
    VkDeviceMemory colorMem = VK_NULL_HANDLE, depthMem = VK_NULL_HANDLE;
    VkImageView colorView = VK_NULL_HANDLE, depthView = VK_NULL_HANDLE;
    VkBuffer readback = VK_NULL_HANDLE;
    VkDeviceMemory readbackMem = VK_NULL_HANDLE;
    void* readbackMapped = nullptr;
    VkSampler sampler = VK_NULL_HANDLE; // clamps, linear; for host texture injection

    static std::uint32_t memoryType(VkPhysicalDevice physical, std::uint32_t bits, VkMemoryPropertyFlags props) {
        VkPhysicalDeviceMemoryProperties mem{};
        vkGetPhysicalDeviceMemoryProperties(physical, &mem);
        for (std::uint32_t i = 0; i < mem.memoryTypeCount; ++i)
            if ((bits & (1u << i)) && (mem.memoryTypes[i].propertyFlags & props) == props) return i;
        throw std::runtime_error("no memory type");
    }
    void makeImage(VkFormat format, VkImageUsageFlags usage, VkImage& image,
                   VkDeviceMemory& memory, VkImageView& view, VkImageAspectFlags aspect) {
        VkImageCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = format;
        info.extent = {width, height, 1};
        info.mipLevels = 1;
        info.arrayLayers = 1;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = usage;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VK_CHECK(vkCreateImage(device, &info, nullptr, &image));
        VkMemoryRequirements req{};
        vkGetImageMemoryRequirements(device, image, &req);
        VkMemoryAllocateInfo alloc{};
        alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        alloc.allocationSize = req.size;
        alloc.memoryTypeIndex = memoryType(physical, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        VK_CHECK(vkAllocateMemory(device, &alloc, nullptr, &memory));
        VK_CHECK(vkBindImageMemory(device, image, memory, 0));
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = format;
        viewInfo.subresourceRange = {aspect, 0, 1, 0, 1};
        VK_CHECK(vkCreateImageView(device, &viewInfo, nullptr, &view));
    }
    OffscreenTarget(const ember::VulkanDevice& dev, std::uint32_t w = 64, std::uint32_t h = 64)
        : device(dev.device), physical(dev.physicalDevice), queue(dev.queue), width(w), height(h) {
        VkCommandPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex = dev.queueFamilyIndex;
        VK_CHECK(vkCreateCommandPool(device, &poolInfo, nullptr, &pool));
        makeImage(VK_FORMAT_R8G8B8A8_UNORM,
                  VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                  VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                  color, colorMem, colorView, VK_IMAGE_ASPECT_COLOR_BIT);
        makeImage(VK_FORMAT_D32_SFLOAT,
                  VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                  VK_IMAGE_USAGE_SAMPLED_BIT,
                  depth, depthMem, depthView, VK_IMAGE_ASPECT_DEPTH_BIT);
        VkBufferCreateInfo bufferInfo{};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = (VkDeviceSize)width * height * 4;
        bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        VK_CHECK(vkCreateBuffer(device, &bufferInfo, nullptr, &readback));
        VkMemoryRequirements req{};
        vkGetBufferMemoryRequirements(device, readback, &req);
        VkMemoryAllocateInfo alloc{};
        alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        alloc.allocationSize = req.size;
        alloc.memoryTypeIndex = memoryType(physical, req.memoryTypeBits,
                                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        VK_CHECK(vkAllocateMemory(device, &alloc, nullptr, &readbackMem));
        VK_CHECK(vkBindBufferMemory(device, readback, readbackMem, 0));
        VK_CHECK(vkMapMemory(device, readbackMem, 0, VK_WHOLE_SIZE, 0, &readbackMapped));
        VkSamplerCreateInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        si.magFilter = VK_FILTER_LINEAR;
        si.minFilter = VK_FILTER_LINEAR;
        si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.maxLod = 0.f;
        VK_CHECK(vkCreateSampler(device, &si, nullptr, &sampler));
        clear(1.f);
    }
    ~OffscreenTarget() {
        vkDeviceWaitIdle(device);
        vkDestroySampler(device, sampler, nullptr);
        vkDestroyBuffer(device, readback, nullptr);
        vkFreeMemory(device, readbackMem, nullptr);
        vkDestroyImageView(device, colorView, nullptr);
        vkDestroyImageView(device, depthView, nullptr);
        vkDestroyImage(device, color, nullptr);
        vkDestroyImage(device, depth, nullptr);
        vkFreeMemory(device, colorMem, nullptr);
        vkFreeMemory(device, depthMem, nullptr);
        vkDestroyCommandPool(device, pool, nullptr);
    }
    VkCommandBuffer beginOneShot() {
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkCommandBufferAllocateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        info.commandPool = pool;
        info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        info.commandBufferCount = 1;
        VK_CHECK(vkAllocateCommandBuffers(device, &info, &cmd));
        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        VK_CHECK(vkBeginCommandBuffer(cmd, &begin));
        return cmd;
    }
    void submitOneShot(VkCommandBuffer cmd) {
        VK_CHECK(vkEndCommandBuffer(cmd));
        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &cmd;
        VK_CHECK(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE));
        VK_CHECK(vkQueueWaitIdle(queue));
        vkFreeCommandBuffers(device, pool, 1, &cmd);
    }
    // Clear color to a constant and depth to `d`, leaving the layouts ember expects.
    void clear(float d, float r = 0.f, float g = 0.f, float b = 0.f) {
        VkCommandBuffer cmd = beginOneShot();
        // Clear both images in TRANSFER_DST_OPTIMAL (the only layout that accepts
        // vkCmdClear*Image), then transition to the attachment layouts ember wants.
        VkImageMemoryBarrier toTransfer{};
        toTransfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        toTransfer.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toTransfer.image = color;
        toTransfer.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        VkImageMemoryBarrier depthTransfer = toTransfer;
        depthTransfer.image = depth;
        depthTransfer.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
        VkImageMemoryBarrier barriers[2] = {toTransfer, depthTransfer};
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 0, nullptr, 0, nullptr, 2, barriers);
        VkClearColorValue colorValue{};
        colorValue.float32[0] = r; colorValue.float32[1] = g;
        colorValue.float32[2] = b; colorValue.float32[3] = 0.f;
        VkClearDepthStencilValue depthValue{};
        depthValue.depth = d;
        VkImageSubresourceRange colorRange{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageSubresourceRange depthRange{VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cmd, color, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &colorValue, 1, &colorRange);
        vkCmdClearDepthStencilImage(cmd, depth, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                    &depthValue, 1, &depthRange);
        VkImageMemoryBarrier toAttachment = toTransfer;
        toAttachment.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toAttachment.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        toAttachment.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toAttachment.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        VkImageMemoryBarrier depthAttachment = toAttachment;
        depthAttachment.image = depth;
        depthAttachment.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
        depthAttachment.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        depthAttachment.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        VkImageMemoryBarrier back[2] = {toAttachment, depthAttachment};
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                             VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
                             0, 0, nullptr, 0, nullptr, 2, back);
        submitOneShot(cmd);
    }
    ember::VulkanFrameTarget target(std::uint32_t frameIndex = 0) const {
        ember::VulkanFrameTarget t{};
        t.colorView = colorView;
        t.depthView = depthView;
        t.colorFormat = VK_FORMAT_R8G8B8A8_UNORM;
        t.depthFormat = VK_FORMAT_D32_SFLOAT;
        t.width = width;
        t.height = height;
        t.frameIndex = frameIndex;
        return t;
    }
    // Copy the color image into host memory; empty on failure.
    std::vector<unsigned char> pixels() {
        VkCommandBuffer cmd = beginOneShot();
        VkImageMemoryBarrier toSrc{};
        toSrc.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        toSrc.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        toSrc.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        toSrc.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toSrc.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toSrc.image = color;
        toSrc.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        toSrc.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        toSrc.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toSrc);
        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {width, height, 1};
        vkCmdCopyImageToBuffer(cmd, color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback, 1, &copy);
        VkImageMemoryBarrier toColor = toSrc;
        toColor.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        toColor.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        toColor.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        toColor.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr, 1, &toColor);
        submitOneShot(cmd);
        const unsigned char* src = static_cast<const unsigned char*>(readbackMapped);
        return std::vector<unsigned char>(src, src + (std::size_t)width * height * 4);
    }
};

int sumChannel(const std::vector<unsigned char>& pixels, int channel = 0) {
    int total = 0;
    for (std::size_t i = (std::size_t)channel; i < pixels.size(); i += 4) total += pixels[i];
    return total;
}
int pixelAt(const std::vector<unsigned char>& pixels, int w, int x, int y, int channel) {
    return pixels[((std::size_t)y * w + x) * 4 + channel];
}

std::vector<ember::Particle> sortedCopy(std::vector<ember::Particle> v) {
    std::sort(v.begin(), v.end(), [](const ember::Particle& a, const ember::Particle& b) {
        const float* fa = &a.pos.x;
        const float* fb = &b.pos.x;
        for (int i = 0; i < 16; ++i) {
            if (fa[i] < fb[i]) return true;
            if (fa[i] > fb[i]) return false;
        }
        return false;
    });
    return v;
}
bool sameMultiset(const std::vector<ember::Particle>& a, const std::vector<ember::Particle>& b) {
    if (a.size() != b.size()) return false;
    if (a.empty()) return true;
    const float* fa = &a[0].pos.x;
    const float* fb = &b[0].pos.x;
    for (std::size_t p = 0; p < a.size(); ++p)
        for (int i = 0; i < 16; ++i)
            if (fa[p * 16 + i] != fb[p * 16 + i]) return false;
    return true;
}

ember::BurstParams centeredBurst(std::uint32_t count, float size, glm::vec4 color) {
    ember::BurstParams b;
    b.count = count;
    b.position = {0.f, 0.f, -2.f};
    b.speedMin = b.speedMax = 0.f;
    b.sizeMin = b.sizeMax = size;
    b.lifeMin = b.lifeMax = 100.f;
    b.colorMin = b.colorMax = color;
    return b;
}

// ---- WO-04 GPU scheduling cases ---------------------------------------------

void gpuDrivenLifecycle(const ember::VulkanContext& ctx) {
    // Same seed/config run synchronously and GPU-driven must agree frame by
    // frame (the GPU protocol is a scheduling change, not a numeric one).
    auto make = [&](bool gpu) {
        auto backend = ember::makeVulkanBackend();
        ember::setVulkanContext(*backend, ctx);
        auto sys = std::make_unique<ember::ParticleSystem>(ember::ParticleSettings{1031, 2000, 987},
                                                           std::move(backend));
        sys->setForceMask(0);
        sys->setGpuDriven(gpu);
        return sys;
    };
    auto sync = make(false);
    auto async = make(true);
    for (int f = 0; f < 64; ++f) {
        for (auto* s : {sync.get(), async.get()}) {
            if (f % 5) {
                ember::BurstParams b;
                b.count = unsigned(f % 4 + 1) * 150;
                b.position = {float(f), 0.f, -2.f};
                b.speedMin = b.speedMax = 0.f;
                b.sizeMin = b.sizeMax = 1.f;
                b.lifeMin = b.lifeMax = float(f % 3 + 1) * 0.25f;
                s->burst(b);
            }
            s->update(0.01f);
        }
        check(sync->aliveCount() == async->aliveCount(), "GPU scheduling changed alive count");
    }
    check(sync->updateSequence() == async->updateSequence(), "GPU scheduling changed sequence");
    check(async->gpuDriven(), "gpuDriven flag lost");
    std::printf("vulkan scheduling: gpu/sync alive parity PASSED\n");
}

void gpuDrivenStatistics(const ember::VulkanContext& ctx) {
    auto backend = ember::makeVulkanBackend();
    ember::setVulkanContext(*backend, ctx);
    ember::ParticleSystem sys({1031, 2000, 987}, std::move(backend));
    sys.setForceMask(0);
    sys.setGpuDriven(true);
    std::uint64_t reported = 0;
    for (unsigned f = 1; f <= 12; ++f) {
        ember::BurstParams b;
        b.count = 17; b.position = {0.f, 0.f, -2.f};
        b.speedMin = b.speedMax = 0.f; b.sizeMin = b.sizeMax = 1.f; b.lifeMin = b.lifeMax = 10.f;
        sys.burst(b);
        sys.update(0);
        const auto s = sys.pollStatistics();
        // Samples may lag but must never run ahead of the published sequence,
        // and must never exceed capacity.
        check(s.frame <= sys.updateSequence(), "pollStatistics ran ahead of updateSequence");
        check(s.alive <= sys.capacity(), "pollStatistics exceeded capacity");
        check(sys.droppedStatistics() >= reported, "droppedStatistics decreased");
        reported = sys.droppedStatistics();
    }
    // The exact path may wait and must return the newest sample (204 particles).
    const auto exact = sys.synchronizeStatistics();
    check(exact.frame == sys.updateSequence() && exact.alive == 204,
          "synchronizeStatistics did not return the newest sample");
    check(sys.aliveCount() == 204, "exact aliveCount mismatch");
    // Non-blocking: 1000 polls must complete well under a GPU frame's worth of
    // time; the guard is generous on purpose (no fragile perf assertion).
    const auto started = std::chrono::steady_clock::now();
    for (int i = 0; i < 1000; ++i) (void)sys.pollStatistics();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started).count();
    check(elapsed < 2000, "pollStatistics appears to block");
    std::printf("vulkan scheduling: async statistics PASSED\n");
}

void gpuDrivenModeSwitch(const ember::VulkanContext& ctx) {
    auto backend = ember::makeVulkanBackend();
    ember::setVulkanContext(*backend, ctx);
    ember::ParticleSystem sys({1031, 2000, 987}, std::move(backend));
    sys.setForceMask(0);
    sys.setGpuDriven(true);
    ember::BurstParams b; b.count = 900; b.position = {0.f, 0.f, -2.f};
    b.speedMin = b.speedMax = 0.f; b.sizeMin = b.sizeMax = 1.f; b.lifeMin = b.lifeMax = 100.f;
    sys.burst(b); sys.update(0);
    const auto before = sys.synchronizeStatistics().alive;
    check(before == 900, "GPU mode population");
    sys.setGpuDriven(false);
    check(sys.aliveCount() == 900, "disabling GPU scheduling lost counts");
    sys.update(0.01f);
    sys.setGpuDriven(true);
    sys.update(0.01f);
    check(sys.synchronizeStatistics().alive == 900, "re-enabling changed population");
    // clear/resize invalidate pending samples
    const auto sequence = sys.updateSequence();
    sys.clear();
    check(sys.pollStatistics().alive == 0 && sys.updateSequence() == sequence,
          "clear retained stale GPU statistics");
    b.count = 1031; sys.burst(b); sys.update(0);
    check(sys.synchronizeStatistics().alive == 1031, "clear left stale population");
    sys.apply(ember::Config::fromString("[system]\ncapacity=257\n"));
    b.count = 257; sys.burst(b); sys.update(0);
    check(sys.synchronizeStatistics().alive == 257, "resize population");
    std::printf("vulkan scheduling: mode switch/clear/resize PASSED\n");
}

void gpuDrivenSortAccepted(const ember::VulkanDevice& device, const ember::VulkanContext& ctx) {
    // WO-05 lifts the WO-04 rejection: GPU scheduling and sorting now combine.
    auto backend = ember::makeVulkanBackend();
    ember::setVulkanContext(*backend, ctx);
    ember::ParticleSystem sys({256, 256, 7}, std::move(backend));
    sys.setForceMask(0);
    sys.setGpuDriven(true);
    sys.setSortEnabled(true); // must not throw
    check(sys.gpuDriven() && sys.sortEnabled(), "gpuDriven+sort combination rejected");
    sys.burst(centeredBurst(4, 0.5f, {1.f, 1.f, 1.f, 1.f}));
    sys.update(0);
    // Rendering right after enabling sort (before another update) must still
    // work: the sort command list is regenerated lazily.
    OffscreenTarget target(device);
    ember::setVulkanFrameTarget(sys.backend(), target.target(0));
    target.clear(1.f);
    sys.render(glm::mat4(1.f), glm::perspective(glm::radians(50.f), 1.f, 0.1f, 100.f),
               64.f, 64.f, 50.f);
    check(sumChannel(target.pixels()) > 0, "sort without an intervening update drew nothing");
    std::printf("vulkan scheduling: gpuDriven+sort combination PASSED\n");
}

// ---- WO-06 effects cases -----------------------------------------------------

// A plain sampled image for host-texture injection (scene color/depth).
struct HostImage {
    VkDevice device = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    std::uint32_t width, height;

    HostImage(const ember::VulkanDevice& dev, VkFormat format, std::uint32_t w, std::uint32_t h,
              VkImageAspectFlags aspect, VkImageUsageFlags extra = 0)
        : device(dev.device), physical(dev.physicalDevice), queue(dev.queue),
          width(w), height(h) {
        VkCommandPoolCreateInfo pi{};
        pi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pi.queueFamilyIndex = dev.queueFamilyIndex;
        VK_CHECK(vkCreateCommandPool(device, &pi, nullptr, &pool));
        VkImageCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = format;
        info.extent = {w, h, 1};
        info.mipLevels = 1;
        info.arrayLayers = 1;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | extra;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VK_CHECK(vkCreateImage(device, &info, nullptr, &image));
        VkMemoryRequirements req{};
        vkGetImageMemoryRequirements(device, image, &req);
        VkMemoryAllocateInfo alloc{};
        alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        alloc.allocationSize = req.size;
        alloc.memoryTypeIndex = OffscreenTarget::memoryType(physical, req.memoryTypeBits,
                                                            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        VK_CHECK(vkAllocateMemory(device, &alloc, nullptr, &memory));
        VK_CHECK(vkBindImageMemory(device, image, memory, 0));
        VkImageViewCreateInfo vi{};
        vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vi.image = image;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = format;
        vi.subresourceRange = {aspect, 0, 1, 0, 1};
        VK_CHECK(vkCreateImageView(device, &vi, nullptr, &view));
    }
    ~HostImage() {
        vkDeviceWaitIdle(device);
        vkDestroyImageView(device, view, nullptr);
        vkDestroyImage(device, image, nullptr);
        vkFreeMemory(device, memory, nullptr);
        vkDestroyCommandPool(device, pool, nullptr);
    }
    void upload(const void* data, VkDeviceSize bytes, VkImageAspectFlags aspect) {
        VkBufferCreateInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bi.size = bytes;
        bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        VkBuffer staging = VK_NULL_HANDLE;
        VK_CHECK(vkCreateBuffer(device, &bi, nullptr, &staging));
        VkMemoryRequirements req{};
        vkGetBufferMemoryRequirements(device, staging, &req);
        VkMemoryAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = OffscreenTarget::memoryType(physical, req.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        VkDeviceMemory mem = VK_NULL_HANDLE;
        VK_CHECK(vkAllocateMemory(device, &ai, nullptr, &mem));
        VK_CHECK(vkBindBufferMemory(device, staging, mem, 0));
        void* mapped = nullptr;
        VK_CHECK(vkMapMemory(device, mem, 0, VK_WHOLE_SIZE, 0, &mapped));
        std::memcpy(mapped, data, (std::size_t)bytes);

        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkCommandBufferAllocateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        ci.commandPool = pool;
        ci.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ci.commandBufferCount = 1;
        VK_CHECK(vkAllocateCommandBuffers(device, &ci, &cmd));
        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        VK_CHECK(vkBeginCommandBuffer(cmd, &begin));
        VkImageMemoryBarrier toDst{};
        toDst.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        toDst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        toDst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toDst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toDst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toDst.image = image;
        toDst.subresourceRange = {aspect, 0, 1, 0, 1};
        toDst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &toDst);
        VkBufferImageCopy copy{};
        copy.imageSubresource = {aspect, 0, 0, 1};
        copy.imageExtent = {width, height, 1};
        vkCmdCopyBufferToImage(cmd, staging, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        toDst.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toDst.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        toDst.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toDst.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &toDst);
        VK_CHECK(vkEndCommandBuffer(cmd));
        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &cmd;
        VK_CHECK(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE));
        VK_CHECK(vkQueueWaitIdle(queue));
        vkFreeCommandBuffers(device, pool, 1, &cmd);
        vkUnmapMemory(device, mem);
        vkDestroyBuffer(device, staging, nullptr);
        vkFreeMemory(device, mem, nullptr);
    }
};

void spriteTextureModes(const ember::VulkanDevice& device, const ember::VulkanContext& ctx) {
    OffscreenTarget target(device);
    auto backend = ember::makeVulkanBackend();
    ember::setVulkanContext(*backend, ctx);
    ember::ParticleSystem sys({8, 8, 123}, std::move(backend));
    sys.setForceMask(0);
    auto b = centeredBurst(1, 1.2f, {1.f, 1.f, 1.f, 1.f});
    b.lifeMin = b.lifeMax = 1.f;
    sys.burst(b); sys.update(0);
    ember::setVulkanFrameTarget(sys.backend(), target.target(0));
    const glm::mat4 view(1.f);
    const glm::mat4 proj = glm::perspective(glm::radians(50.f), 1.f, 0.1f, 100.f);
    auto draw = [&] { target.clear(1.f); sys.render(view, proj, 64.f, 64.f, 50.f); return target.pixels(); };

    const auto builtin = draw();
    check(sumChannel(builtin) > 0, "built-in sprite rendered nothing");
    sys.setSpriteTexture("sprites/test.png");
    const auto png = draw();
    check(png != builtin, "PNG sprite did not replace the built-in gradient");
    // Empty path resets to the built-in gradient.
    sys.setSpriteTexture("");
    check(draw() == builtin, "empty path did not restore the built-in sprite");
    // Sprite sheet frame animation advances with age.
    sys.setSpriteTexture("sprites/test.png");
    sys.setSpriteSheet(2, 1);
    const auto first = draw();
    for (int i = 0; i < 6; ++i) sys.update(0.1f);
    const auto later = draw();
    check(first != later, "sprite sheet frame did not advance");
    std::printf("vulkan effects: sprite textures/sheet PASSED\n");
}

void softParticleFade(const ember::VulkanDevice& device, const ember::VulkanContext& ctx) {
    OffscreenTarget target(device);
    // Depth plane closer than the particle: it must fade out.
    HostImage depth(device, VK_FORMAT_D32_SFLOAT, 64, 64, VK_IMAGE_ASPECT_DEPTH_BIT);
    auto backend = ember::makeVulkanBackend();
    ember::setVulkanContext(*backend, ctx);
    ember::ParticleSystem sys({8, 8, 123}, std::move(backend));
    sys.setForceMask(0);
    sys.setUseSprite(false);
    sys.burst(centeredBurst(1, 0.8f, {1.f, 1.f, 1.f, 1.f}));
    sys.update(0);
    ember::setVulkanFrameTarget(sys.backend(), target.target(0));
    const glm::mat4 view(1.f);
    const glm::mat4 proj = glm::perspective(glm::radians(50.f), 1.f, 0.1f, 100.f);

    target.clear(1.f);
    sys.render(view, proj, 64.f, 64.f, 50.f);
    const int without = sumChannel(target.pixels());
    check(without > 0, "soft-particle fixture has no particle");

    // Particle sits at view z=-2. A depth surface at z=-1 (in front) occludes.
    auto depthPlane = [&](float z) {
        std::vector<float> ds(64 * 64);
        auto p = proj * glm::vec4(0, 0, z, 1);
        const float d = (p.z / p.w + 1.f) * 0.5f;
        std::fill(ds.begin(), ds.end(), d);
        depth.upload(ds.data(), ds.size() * sizeof(float), VK_IMAGE_ASPECT_DEPTH_BIT);
    };
    depthPlane(-1.f);
    ember::setVulkanSoftDepth(sys.backend(), depth.view, target.sampler);
    sys.setSoftParticleParameters(true, 0.5f);
    target.clear(1.f);
    sys.render(view, proj, 64.f, 64.f, 50.f);
    const int faded = sumChannel(target.pixels());
    check(faded < without, "soft particle did not fade in front of the scene depth");
    std::printf("vulkan effects: soft particles PASSED\n");
}

void refractionModes(const ember::VulkanDevice& device, const ember::VulkanContext& ctx) {
    OffscreenTarget target(device);
    HostImage scene(device, VK_FORMAT_R8G8B8A8_UNORM, 64, 64, VK_IMAGE_ASPECT_COLOR_BIT);
    std::vector<unsigned char> gradient(64 * 64 * 4);
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x) {
            const int i = (y * 64 + x) * 4;
            gradient[i] = x * 4; gradient[i + 1] = y * 4; gradient[i + 2] = 128; gradient[i + 3] = 255;
        }
    scene.upload(gradient.data(), gradient.size(), VK_IMAGE_ASPECT_COLOR_BIT);

    auto backend = ember::makeVulkanBackend();
    ember::setVulkanContext(*backend, ctx);
    ember::ParticleSystem sys({8, 8, 123}, std::move(backend));
    sys.setForceMask(0);
    ember::BurstParams b = centeredBurst(1, 0.8f, {1.f, 1.f, 1.f, 1.f});
    b.refractive = true;
    sys.burst(b); sys.update(0);
    ember::setVulkanFrameTarget(sys.backend(), target.target(0));
    const glm::mat4 view(1.f);
    const glm::mat4 proj = glm::perspective(glm::radians(50.f), 1.f, 0.1f, 100.f);

    ember::RefractionParameters r;
    r.enabled = true;
    r.mode = 0;
    r.absorption = 1.f;
    r.fresnel = 0.f;
    r.shape = 1;
    ember::setVulkanRefractionInputs(sys.backend(), scene.view, VK_NULL_HANDLE, target.sampler);
    sys.setRefractionParameters(r);
    target.clear(1.f);
    sys.render(view, proj, 64.f, 64.f, 50.f);
    const auto sampled = target.pixels();
    check(sumChannel(sampled) > 0, "refraction did not sample the scene color");
    check(sumChannel(sampled, 2) > 0, "refraction lost the scene blue channel");

    // Mode 1 without a depth texture falls back to mode 0.
    r.mode = 1;
    sys.setRefractionParameters(r);
    target.clear(1.f);
    sys.render(view, proj, 64.f, 64.f, 50.f);
    check(target.pixels() == sampled, "mode 1 without depth did not fall back to mode 0");
    std::printf("vulkan effects: refraction PASSED\n");
}

void bloomHalo(const ember::VulkanDevice& device, const ember::VulkanContext& ctx) {
    OffscreenTarget target(device);
    auto backend = ember::makeVulkanBackend();
    ember::setVulkanContext(*backend, ctx);
    ember::ParticleSystem sys({8, 8, 123}, std::move(backend));
    sys.setForceMask(0);
    sys.setUseSprite(false);
    ember::BurstParams b = centeredBurst(1, 0.3f, {8.f, 8.f, 8.f, 1.f});
    sys.burst(b); sys.update(0);
    ember::setVulkanFrameTarget(sys.backend(), target.target(0));
    const glm::mat4 view(1.f);
    const glm::mat4 proj = glm::perspective(glm::radians(50.f), 1.f, 0.1f, 100.f);

    // Occlusion first, with fresh bloom state.
    sys.setDepthTest(true);
    target.clear(0.1f);
    sys.setBloom(true);
    sys.render(view, proj, 64.f, 64.f, 50.f);
    check(sumChannel(target.pixels()) == 0, "bloom leaked through foreground depth");
    sys.setDepthTest(false);

    target.clear(1.f);
    sys.setBloom(true);
    sys.render(view, proj, 64.f, 64.f, 50.f);
    const auto bloomed = target.pixels();
    sys.setBloom(false);
    target.clear(1.f);
    sys.render(view, proj, 64.f, 64.f, 50.f);
    const auto plain = target.pixels();
    check(sumChannel(bloomed) > sumChannel(plain), "bloom did not brighten the image");
    // Halo: pixels that were dark in the plain image must gain energy.
    int halo = 0;
    for (std::size_t i = 0; i < plain.size(); i += 4)
        if (plain[i] == 0 && bloomed[i] > 4) ++halo;
    check(halo > 0, "bloom has no halo outside the particle footprint");
    // Host depth occlusion: a nearer foreground depth must suppress bloom too.
    target.clear(0.1f);
    sys.setDepthTest(true);
    sys.setBloom(true);
    sys.render(view, proj, 64.f, 64.f, 50.f);
    check(sumChannel(target.pixels()) == 0, "bloom leaked through foreground depth");
    std::printf("vulkan effects: bloom halo/occlusion PASSED\n");
}

void lifeCurveModes(const ember::VulkanDevice& device, const ember::VulkanContext& ctx) {
    OffscreenTarget target(device);
    auto backend = ember::makeVulkanBackend();
    ember::setVulkanContext(*backend, ctx);
    ember::ParticleSystem sys({8, 8, 123}, std::move(backend));
    sys.setForceMask(0);
    sys.setUseSprite(false);
    ember::BurstParams b = centeredBurst(1, 0.8f, {1.f, 1.f, 1.f, 1.f});
    b.lifeMin = b.lifeMax = 1.f;
    sys.burst(b); sys.update(0);
    ember::setVulkanFrameTarget(sys.backend(), target.target(0));
    const glm::mat4 view(1.f);
    const glm::mat4 proj = glm::perspective(glm::radians(50.f), 1.f, 0.1f, 100.f);
    auto draw = [&] { target.clear(1.f); sys.render(view, proj, 64.f, 64.f, 50.f); return target.pixels(); };

    const auto reference = draw();
    check(sumChannel(reference) > 0, "curve fixture has no particle");
    // size-over-life shrinks coverage monotonically with age.
    sys.setSizeOverLife({1.f, 0.f});
    const auto size0 = draw();
    for (int i = 0; i < 5; ++i) sys.update(0.1f);
    const auto size1 = draw();
    for (int i = 0; i < 4; ++i) sys.update(0.1f);
    const auto size2 = draw();
    check(sumChannel(size0) > sumChannel(size1) && sumChannel(size1) > sumChannel(size2),
          "size-over-life did not shrink coverage");
    sys.update(0.1f); // age crosses the lifetime: coverage drops to zero
    check(sumChannel(draw()) == 0, "size-over-life particle did not die out");
    // color-over-life fades the red channel toward the end of life.
    sys.setSizeOverLife({});
    sys.clear();
    sys.burst(b); sys.update(0);
    const auto colorBase = draw();
    sys.setColorOverLife({{1.f, 1.f, 1.f, 1.f}, {0.f, 1.f, 1.f, 1.f}});
    for (int i = 0; i < 9; ++i) sys.update(0.1f);
    const auto colorFaded = draw();
    check(sumChannel(colorFaded, 0) < sumChannel(colorBase, 0) / 2,
          "color-over-life did not fade the red channel");
    // Clearing both channels restores the exact baseline render.
    sys.setColorOverLife({});
    sys.setSizeOverLife({});
    sys.clear();
    sys.burst(b); sys.update(0);
    check(draw() == reference, "cleared curves did not restore the exact baseline");
    std::printf("vulkan effects: lifecycle curves PASSED\n");
}

void dualBackendSmoke() {
    // GL and Vulkan in one process: a GL context and a VkDevice stay alive
    // together and each runs a frame. The GL half is skipped (not failed) when
    // no GL context can be created, so a Vulkan-only host still runs this test.
    ember::VulkanDevice device = ember::makeVulkanDevice(false);
#ifdef EMBER_VULKAN_TEST_HAS_GL
    std::unique_ptr<ember::Window> window;
    try {
        window = std::make_unique<ember::Window>(64, 64, "ember dual backend", 0,
                                                 /*vsync=*/false, /*visible=*/false);
        ember::ParticleSystem glSys({16, 16, 7});
        glSys.setForceMask(0);
        glSys.setUseSprite(false);
        glSys.burst(centeredBurst(1, 0.4f, {1.f, 1.f, 1.f, 1.f}));
        glSys.update(0);
        glSys.render(glm::mat4(1.f), glm::perspective(glm::radians(50.f), 1.f, 0.1f, 100.f),
                     64.f, 64.f, 50.f);
    } catch (const ember::ContextUnavailable&) {
        window.reset(); // no GL here; the VkDevice still gets exercised below
    }
#endif
    OffscreenTarget target(device);
    auto backend = ember::makeVulkanBackend();
    ember::setVulkanContext(*backend, device.context());
    ember::ParticleSystem sys({16, 16, 7}, std::move(backend));
    sys.setForceMask(0);
    sys.setUseSprite(false);
    sys.burst(centeredBurst(1, 0.4f, {1.f, 1.f, 1.f, 1.f}));
    sys.update(0);
    ember::setVulkanFrameTarget(sys.backend(), target.target(0));
    target.clear(1.f);
    sys.render(glm::mat4(1.f), glm::perspective(glm::radians(50.f), 1.f, 0.1f, 100.f),
               64.f, 64.f, 50.f);
    check(sumChannel(target.pixels()) > 0, "vulkan frame produced nothing");
    std::printf("vulkan effects: dual-backend smoke PASSED\n");
}

// ---- WO-10 host command-buffer recording -------------------------------------

std::unique_ptr<ember::ParticleSystem> makeHostTestSystem(const ember::VulkanContext& ctx,
                                                          std::uint32_t capacity,
                                                          std::uint32_t budget,
                                                          std::uint32_t seed) {
    auto backend = ember::makeVulkanBackend();
    ember::setVulkanContext(*backend, ctx);
    auto sys = std::make_unique<ember::ParticleSystem>(ember::ParticleSettings{capacity, budget, seed},
                                                       std::move(backend));
    sys->setForceMask(0);
    return sys;
}

// Default and host-recorded updates share one instruction stream and must keep
// identical simulation state (compared as sorted snapshots: particles die).
void hostCommandUpdateParity(const ember::VulkanDevice& device, const ember::VulkanContext& ctx) {
    OffscreenTarget target(device);
    auto ref = makeHostTestSystem(ctx, 1200, 300, 0x40E57);
    auto host = makeHostTestSystem(ctx, 1200, 300, 0x40E57);
    for (int f = 0; f < 16; ++f) {
        ember::BurstParams b;
        b.count = 120;
        b.position = {float(f % 4) * 0.2f, 0.f, 0.f};
        b.speedMin = 0.f; b.speedMax = 1.f;
        b.spread = 1.f;
        b.lifeMin = 0.05f; b.lifeMax = 0.2f;
        b.sizeMin = 0.02f; b.sizeMax = 0.08f;
        ref->burst(b);
        host->burst(b);
        ref->update(1.f / 60.f);
        VkCommandBuffer cmd = target.beginOneShot();
        ember::beginVulkanFrame(host->backend(), cmd, static_cast<std::uint32_t>(f));
        host->update(1.f / 60.f);
        ember::endVulkanFrame(host->backend());
        target.submitOneShot(cmd);
        check(host->aliveCount() == ref->aliveCount(), "host update alive parity");
    }
    check(sameMultiset(sortedCopy(ref->readParticles()), sortedCopy(host->readParticles())),
          "host update particle multiset diverged");
    std::printf("vulkan host cmd: update parity PASSED\n");
}

// A host-recorded render must produce byte-identical pixels to the default path.
void hostCommandRenderParity(const ember::VulkanDevice& device, const ember::VulkanContext& ctx) {
    OffscreenTarget target(device);
    auto sys = makeHostTestSystem(ctx, 16, 16, 7);
    sys->setUseSprite(false);
    sys->burst(centeredBurst(1, 0.8f, {1.f, 1.f, 1.f, 1.f}));
    sys->update(0);
    ember::setVulkanFrameTarget(sys->backend(), target.target(0));
    const glm::mat4 view(1.f);
    const glm::mat4 proj = glm::perspective(glm::radians(50.f), 1.f, 0.1f, 100.f);

    target.clear(1.f);
    sys->render(view, proj, 64.f, 64.f, 50.f);
    const auto reference = target.pixels();
    check(sumChannel(reference) > 0, "host render fixture has no particle");

    target.clear(1.f);
    VkCommandBuffer cmd = target.beginOneShot();
    ember::beginVulkanFrame(sys->backend(), cmd, 0);
    sys->render(view, proj, 64.f, 64.f, 50.f);
    ember::endVulkanFrame(sys->backend());
    target.submitOneShot(cmd);
    check(target.pixels() == reference, "host-recorded render differs from the default path");
    std::printf("vulkan host cmd: render parity PASSED\n");
}

void hostCommandStateMachine(const ember::VulkanDevice& device, const ember::VulkanContext& ctx) {
    OffscreenTarget target(device);
    auto backend = ember::makeVulkanBackend();
    ember::setVulkanContext(*backend, ctx);
    ember::ParticleSystem sys({64, 64, 1}, std::move(backend));
    sys.setForceMask(0);
    auto expectLogic = [](const char* what, auto fn) {
        bool threw = false;
        try { fn(); } catch (const std::logic_error&) { threw = true; }
        check(threw, what);
    };
    expectLogic("end without begin", [&] { ember::endVulkanFrame(sys.backend()); });
    expectLogic("null command buffer", [&] { ember::beginVulkanFrame(sys.backend(), VK_NULL_HANDLE, 0); });

    VkCommandBuffer cmd = target.beginOneShot();
    ember::beginVulkanFrame(sys.backend(), cmd, 0);
    expectLogic("nested begin", [&] { ember::beginVulkanFrame(sys.backend(), cmd, 0); });
    expectLogic("setVulkanFrameTarget in window",
                [&] { ember::setVulkanFrameTarget(sys.backend(), target.target(0)); });
    expectLogic("synchronizeStatistics in window", [&] { (void)sys.aliveCount(); });
    expectLogic("readParticles in window", [&] { (void)sys.readParticles(); });
    expectLogic("setGpuDriven in window", [&] { sys.setGpuDriven(false); });
    expectLogic("setSortEnabled in window", [&] { sys.setSortEnabled(false); });
    ember::endVulkanFrame(sys.backend());
    target.submitOneShot(cmd);

    // Spawn a particle so render() does not early-out before the slot check.
    sys.burst(centeredBurst(1, 0.8f, {1.f, 1.f, 1.f, 1.f}));
    sys.update(0);
    check(sys.aliveCount() == 1, "state-machine fixture has no particle");

    // Target frameIndex must select the same slot as beginVulkanFrame.
    ember::setVulkanFrameTarget(sys.backend(), target.target(1));
    VkCommandBuffer cmd2 = target.beginOneShot();
    ember::beginVulkanFrame(sys.backend(), cmd2, 0);
    sys.update(0);
    expectLogic("target slot mismatch",
                [&] { sys.render(glm::mat4(1.f), glm::mat4(1.f), 64.f, 64.f, 50.f); });
    ember::endVulkanFrame(sys.backend());
    target.submitOneShot(cmd2);
    std::printf("vulkan host cmd: state machine PASSED\n");
}

// Alternating host/default frames on one system must track an all-default run.
void hostCommandMixedMode(const ember::VulkanDevice& device, const ember::VulkanContext& ctx) {
    OffscreenTarget target(device);
    auto ref = makeHostTestSystem(ctx, 800, 200, 0x3A17);
    auto mixed = makeHostTestSystem(ctx, 800, 200, 0x3A17);
    for (int f = 0; f < 8; ++f) {
        ember::BurstParams b;
        b.count = 80;
        b.position = {0.f, float(f % 3) * 0.1f, -2.f};
        b.speedMin = 0.f; b.speedMax = 0.5f;
        b.spread = 1.f;
        b.lifeMin = 0.05f; b.lifeMax = 0.15f;
        b.sizeMin = 0.02f; b.sizeMax = 0.06f;
        ref->burst(b);
        mixed->burst(b);
        ref->update(1.f / 60.f);
        if (f % 2 == 0) {
            VkCommandBuffer cmd = target.beginOneShot();
            ember::beginVulkanFrame(mixed->backend(), cmd, static_cast<std::uint32_t>(f));
            mixed->update(1.f / 60.f);
            ember::endVulkanFrame(mixed->backend());
            target.submitOneShot(cmd);
        } else {
            mixed->update(1.f / 60.f);
        }
        check(mixed->aliveCount() == ref->aliveCount(), "mixed-mode alive parity");
    }
    check(sameMultiset(sortedCopy(ref->readParticles()), sortedCopy(mixed->readParticles())),
          "mixed-mode particle multiset diverged");
    // Identical state must also rasterize identically through both mode paths.
    ember::setVulkanFrameTarget(ref->backend(), target.target(0));
    ember::setVulkanFrameTarget(mixed->backend(), target.target(0));
    const glm::mat4 view(1.f);
    const glm::mat4 proj = glm::perspective(glm::radians(50.f), 1.f, 0.1f, 100.f);
    target.clear(1.f);
    ref->render(view, proj, 64.f, 64.f, 50.f);
    const auto refPixels = target.pixels();
    check(sumChannel(refPixels) > 0, "mixed-mode render fixture has no particle");
    target.clear(1.f);
    mixed->render(view, proj, 64.f, 64.f, 50.f);
    check(target.pixels() == refPixels, "mixed-mode final render differs");
    std::printf("vulkan host cmd: mixed mode PASSED\n");
}

// Host gpuDriven statistics: poll never validates host samples; exact queries
// drain them after the host has waited its command buffer.
void hostCommandGpuStatistics(const ember::VulkanDevice& device, const ember::VulkanContext& ctx) {
    OffscreenTarget target(device);
    auto sys = makeHostTestSystem(ctx, 512, 128, 7);
    sys->setGpuDriven(true);
    ember::BurstParams b = centeredBurst(100, 0.1f, {1.f, 1.f, 1.f, 1.f});
    const std::uint64_t drops0 = sys->droppedStatistics();
    // H2: poll never validates host samples, so without an exact query in the
    // loop the polled frame must stay at the pre-window baseline forever.
    const std::uint64_t baseline = sys->pollStatistics().frame;
    for (int f = 0; f < 6; ++f) {
        sys->burst(b);
        VkCommandBuffer cmd = target.beginOneShot();
        ember::beginVulkanFrame(sys->backend(), cmd, static_cast<std::uint32_t>(f));
        sys->update(1.f / 60.f);
        const auto polled = sys->pollStatistics();
        check(polled.frame == baseline, "poll validated a host sample");
        ember::endVulkanFrame(sys->backend());
        target.submitOneShot(cmd);
    }
    const auto exact = sys->synchronizeStatistics();
    check(exact.alive == sys->aliveCount(), "host gpu exact statistics mismatch");
    check(exact.alive > 0, "host gpu statistics never advanced");
    check(sys->droppedStatistics() >= drops0, "dropped statistics decreased");
    std::printf("vulkan host cmd: gpu statistics PASSED\n");
}

// Regression: a host-recorded update must not leave render() thinking the
// system is empty — the CPU alive_ snapshot only refreshes on default submits.
void hostCommandStaleAliveRender(const ember::VulkanDevice& device, const ember::VulkanContext& ctx) {
    OffscreenTarget target(device);
    auto sys = makeHostTestSystem(ctx, 16, 16, 7);
    sys->setUseSprite(false);
    sys->burst(centeredBurst(1, 0.8f, {1.f, 1.f, 1.f, 1.f}));
    ember::setVulkanFrameTarget(sys->backend(), target.target(0));
    const glm::mat4 view(1.f);
    const glm::mat4 proj = glm::perspective(glm::radians(50.f), 1.f, 0.1f, 100.f);
    target.clear(1.f);
    VkCommandBuffer cmd = target.beginOneShot();
    ember::beginVulkanFrame(sys->backend(), cmd, 0);
    sys->update(0); // recorded, not submitted: alive_ stays 0 on the CPU
    sys->render(view, proj, 64.f, 64.f, 50.f);
    ember::endVulkanFrame(sys->backend());
    target.submitOneShot(cmd);
    check(sumChannel(target.pixels()) > 0,
          "host-recorded spawn rendered nothing (stale alive_ early-out)");
    std::printf("vulkan host cmd: stale-alive render PASSED\n");
}

// ---- WO-05 sorting cases -----------------------------------------------------

// Back-to-front coverage: a big far (blue) and a small near (red) particle.
// With exact depth sorting the near one is drawn last for normal blending, so
// the center pixel is red; without sorting the far one may paint over it.
void sortColorOrder(const ember::VulkanDevice& device, const ember::VulkanContext& ctx, bool gpu) {
    OffscreenTarget target(device);
    auto backend = ember::makeVulkanBackend();
    ember::setVulkanContext(*backend, ctx);
    ember::ParticleSystem sys({16, 16, 7}, std::move(backend));
    sys.setForceMask(0);
    sys.setUseSprite(false);
    sys.setBlendMode(ember::BlendMode::Normal);
    sys.setGpuDriven(gpu);

    ember::BurstParams nearB;
    nearB.count = 1; nearB.position = {0.f, 0.f, -2.f};
    nearB.speedMin = nearB.speedMax = 0.f;
    nearB.sizeMin = nearB.sizeMax = 0.3f;
    nearB.lifeMin = nearB.lifeMax = 10.f;
    nearB.colorMin = nearB.colorMax = {1.f, 0.f, 0.f, 1.f};
    sys.burst(nearB);
    sys.update(0);
    ember::BurstParams farB = nearB;
    farB.position = {0.f, 0.f, -3.f};
    farB.sizeMin = farB.sizeMax = 1.5f;
    farB.colorMin = farB.colorMax = {0.f, 0.f, 1.f, 1.f};
    sys.burst(farB);
    sys.update(0);

    ember::setVulkanFrameTarget(sys.backend(), target.target(0));
    const glm::mat4 view(1.f);
    const glm::mat4 proj = glm::perspective(glm::radians(50.f), 1.f, 0.1f, 100.f);

    target.clear(1.f);
    sys.render(view, proj, 64.f, 64.f, 50.f);
    const auto before = target.pixels();
    const int center0 = (32 * 64 + 32) * 4;
    const bool unsortedBlue = before[center0 + 2] > before[center0];

    sys.setSortEnabled(true);
    sys.update(0);
    target.clear(1.f);
    sys.render(view, proj, 64.f, 64.f, 50.f);
    const auto sorted = target.pixels();
    check(sorted[center0] > sorted[center0 + 2], "sort is not back to front at the center");
    // The fixture only proves anything if the unsorted order actually differed.
    check(unsortedBlue, "sorting fixture did not start in the wrong order");
    std::printf("vulkan sort (%s): color order PASSED\n", gpu ? "gpu" : "sync");
}

// Sorting must not duplicate or lose draws across death/recycle frames.
void sortWithRecycling(const ember::VulkanContext& ctx, bool gpu) {
    auto backend = ember::makeVulkanBackend();
    ember::setVulkanContext(*backend, ctx);
    ember::ParticleSystem sys({256, 256, 7}, std::move(backend));
    sys.setForceMask(0);
    sys.setGpuDriven(gpu);
    sys.setSortEnabled(true);
    for (int f = 0; f < 12; ++f) {
        ember::BurstParams b;
        b.count = 40; b.position = {float(f % 3), 0.f, -2.f - float(f % 5) * 0.1f};
        b.speedMin = b.speedMax = 0.f; b.sizeMin = b.sizeMax = 0.5f;
        b.lifeMin = b.lifeMax = 0.15f;
        sys.burst(b);
        sys.update(0.05f);
        const std::uint32_t alive = sys.aliveCount();
        for (const auto& p : sys.readParticles()) check(p.life.x >= 0.f, "sorted readback kept a corpse");
        check(alive <= sys.capacity(), "sorted population exceeded capacity");
    }
    std::printf("vulkan sort (%s): recycling PASSED\n", gpu ? "gpu" : "sync");
}

// ---- WO-03 pixel cases -------------------------------------------------------

void renderCenterAndCorners(const ember::VulkanDevice& device, const ember::VulkanContext& ctx) {
    OffscreenTarget target(device);
    auto backend = ember::makeVulkanBackend();
    ember::setVulkanContext(*backend, ctx);
    ember::ParticleSystem sys({64, 64, 7}, std::move(backend));
    sys.setForceMask(0);
    sys.setUseSprite(false); // procedural soft disc path
    sys.burst(centeredBurst(1, 0.4f, {1.f, 1.f, 1.f, 1.f}));
    sys.update(0);
    ember::setVulkanFrameTarget(sys.backend(), target.target(0));
    const glm::mat4 view(1.f);
    const glm::mat4 proj = glm::perspective(glm::radians(50.f), 1.f, 0.1f, 100.f);
    target.clear(1.f); // black background: corners must stay exactly zero
    sys.render(view, proj, (float)target.width, (float)target.height, 50.f);
    const auto pixels = target.pixels();
    check(sumChannel(pixels) > 0, "centered particle rendered nothing");
    check(pixelAt(pixels, 64, 0, 0, 0) == 0 && pixelAt(pixels, 64, 63, 0, 0) == 0 &&
          pixelAt(pixels, 64, 0, 63, 0) == 0 && pixelAt(pixels, 64, 63, 63, 0) == 0,
          "particle leaked into the corners");
    std::printf("vulkan render: center/corners PASSED\n");
}

void renderAdditiveBlend(const ember::VulkanDevice& device, const ember::VulkanContext& ctx) {
    OffscreenTarget target(device);
    auto backend = ember::makeVulkanBackend();
    ember::setVulkanContext(*backend, ctx);
    ember::ParticleSystem sys({64, 64, 7}, std::move(backend));
    sys.setForceMask(0);
    sys.setUseSprite(false);
    sys.burst(centeredBurst(1, 0.5f, {0.4f, 0.4f, 0.4f, 1.f}));
    sys.update(0);
    ember::setVulkanFrameTarget(sys.backend(), target.target(0));
    target.clear(1.f);
    const glm::mat4 view(1.f);
    const glm::mat4 proj = glm::perspective(glm::radians(50.f), 1.f, 0.1f, 100.f);
    sys.render(view, proj, 64.f, 64.f, 50.f);
    const int single = sumChannel(target.pixels(), 1);
    // Second coincident particle: additive blending must brighten the overlap.
    sys.burst(centeredBurst(1, 0.5f, {0.4f, 0.4f, 0.4f, 1.f}));
    sys.update(0);
    target.clear(1.f);
    sys.render(view, proj, 64.f, 64.f, 50.f);
    const int doubled = sumChannel(target.pixels(), 1);
    check(doubled > single, "additive blending did not brighten the overlap");
    std::printf("vulkan render: additive blend PASSED\n");
}

void renderDepthPolicy(const ember::VulkanDevice& device, const ember::VulkanContext& ctx) {
    OffscreenTarget target(device);
    auto backend = ember::makeVulkanBackend();
    ember::setVulkanContext(*backend, ctx);
    ember::ParticleSystem sys({64, 64, 7}, std::move(backend));
    sys.setForceMask(0);
    sys.setUseSprite(false);
    sys.setDepthTest(true);
    sys.setDepthWrite(true);
    // Particle sits at view z=-2: with the GL-style projection and the vertex
    // shader's clip remap its depth lands at ~0.95 in Vulkan's [0,1] range.
    sys.burst(centeredBurst(1, 0.5f, {1.f, 1.f, 1.f, 1.f}));
    sys.update(0);
    ember::setVulkanFrameTarget(sys.backend(), target.target(0));
    const glm::mat4 view(1.f);
    const glm::mat4 proj = glm::perspective(glm::radians(50.f), 1.f, 0.1f, 100.f);

    // Scene depth far (1.0): the particle passes the depth test.
    target.clear(1.f);
    sys.render(view, proj, 64.f, 64.f, 50.f);
    check(sumChannel(target.pixels()) > 0, "particle should be visible over far scene depth");

    // Scene depth near (0.1, closer than the particle): fully occluded.
    target.clear(0.1f);
    sys.render(view, proj, 64.f, 64.f, 50.f);
    check(sumChannel(target.pixels()) == 0, "particle should be occluded by nearer scene depth");
    std::printf("vulkan render: depth policy PASSED\n");
}

} // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);
    ember::VulkanDevice device;
    try {
        // EMBER_VK_DEBUG=1 enables validation layers when installed.
        device = ember::makeVulkanDevice(/*debug=*/std::getenv("EMBER_VK_DEBUG") != nullptr);
    } catch (const std::exception& e) {
        std::printf("SKIP: cannot create Vulkan device: %s\n", e.what());
        return 77;
    }

    try {
        const ember::VulkanContext ctx = device.context();

        // ---- skeleton-level checks ------------------------------------------
        {
            auto backend = ember::makeVulkanBackend();
            ember::setVulkanContext(*backend, ctx);
            ember::ParticleSystem sys({1024, 256, 7}, std::move(backend));
            check(std::string(sys.backendName()) == "vulkan", "backend name");
            const ember::BackendCapabilities caps = sys.backendCapabilities();
            check(caps.gpuScheduling && caps.sorting && caps.bloom && caps.refraction &&
                  caps.softParticles && caps.spriteTextures && caps.events && caps.lifeCurves,
                  "all capabilities must be on (WO-06/08/09)");
            sys.update(0.016f);
            check(sys.updateSequence() == 1, "update must advance the sequence once");
            check(sys.aliveCount() == 0, "empty update must report no particles");
            sys.render(glm::mat4(1.f), glm::mat4(1.f), 64.f, 64.f, 50.f);
        }

        // ---- backend-agnostic behavior suite --------------------------------
        behavior::run([ctx](const ember::ParticleSettings& s) {
            auto b = ember::makeVulkanBackend();
            ember::setVulkanContext(*b, ctx);
            return ember::ParticleSystem(s, std::move(b));
        });

        // ---- GPU scheduling cases -------------------------------------------
        gpuDrivenLifecycle(ctx);
        gpuDrivenStatistics(ctx);
        gpuDrivenModeSwitch(ctx);
        gpuDrivenSortAccepted(device, ctx);

        // ---- render pixel cases ---------------------------------------------
        renderCenterAndCorners(device, ctx);
        renderAdditiveBlend(device, ctx);
        renderDepthPolicy(device, ctx);

        // ---- sorting cases ---------------------------------------------------
        sortColorOrder(device, ctx, /*gpu=*/false);
        sortColorOrder(device, ctx, /*gpu=*/true);
        sortWithRecycling(ctx, /*gpu=*/false);
        sortWithRecycling(ctx, /*gpu=*/true);

        // ---- optional effects -------------------------------------------------
        spriteTextureModes(device, ctx);
        softParticleFade(device, ctx);
        refractionModes(device, ctx);
        bloomHalo(device, ctx);
        lifeCurveModes(device, ctx);
        dualBackendSmoke();

        // ---- host command-buffer recording (WO-10) ---------------------------
        hostCommandUpdateParity(device, ctx);
        hostCommandRenderParity(device, ctx);
        hostCommandStateMachine(device, ctx);
        hostCommandMixedMode(device, ctx);
        hostCommandGpuStatistics(device, ctx);
        hostCommandStaleAliveRender(device, ctx);

        std::printf("vulkan simulation: PASSED\n");
        return 0;
    } catch (const std::exception& e) {
        std::printf("FAIL: %s\n", e.what());
        return 1;
    }
}
