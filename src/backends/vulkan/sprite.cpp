#include "vulkan_backend.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>
// The stb implementation lives in this TU (static linkage) so linking both the
// GL and Vulkan backends into one process cannot collide on stbi_* symbols.
#ifdef EMBER_USE_STB
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include "stb_image.h"
#pragma GCC diagnostic pop
#endif

namespace ember::detail::vulkan {
namespace {
std::uint32_t findMemoryType(VkPhysicalDevice physical, std::uint32_t typeBits,
                             VkMemoryPropertyFlags props) {
    VkPhysicalDeviceMemoryProperties mem{};
    vkGetPhysicalDeviceMemoryProperties(physical, &mem);
    for (std::uint32_t i = 0; i < mem.memoryTypeCount; ++i)
        if ((typeBits & (1u << i)) && (mem.memoryTypes[i].propertyFlags & props) == props) return i;
    throw std::runtime_error("ember/vulkan: no suitable memory type");
}
} // namespace

void VulkanBackend::ensureSpriteSampler() {
    if (spriteSampler_) return;
    VkSamplerCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    info.magFilter = VK_FILTER_LINEAR;
    info.minFilter = VK_FILTER_LINEAR;
    info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.maxLod = 0.f;
    VK_CHECK(vkCreateSampler(context_.device, &info, nullptr, &spriteSampler_));
}

void VulkanBackend::uploadBuiltinSprite() {
    // Same procedural soft disc + halo as the GL built-in sprite.
    constexpr int kSize = 64;
    std::vector<unsigned char> px((std::size_t)kSize * kSize * 4, 0);
    for (int y = 0; y < kSize; ++y) {
        for (int x = 0; x < kSize; ++x) {
            const float dx = (x + 0.5f) / kSize * 2.f - 1.f;
            const float dy = (y + 0.5f) / kSize * 2.f - 1.f;
            const float r = std::sqrt(dx * dx + dy * dy);
            float a = 1.f - std::min(r, 1.f);
            a += (1.f - std::min(r * r, 1.f)) * 0.35f;
            const std::size_t i = ((std::size_t)y * kSize + (std::size_t)x) * 4;
            px[i + 0] = 255; px[i + 1] = 255; px[i + 2] = 255;
            px[i + 3] = (unsigned char)(glm::clamp(a, 0.f, 1.f) * 255.f);
        }
    }
    uploadSprite((std::uint32_t)kSize, (std::uint32_t)kSize, px.data());
}

void VulkanBackend::uploadSprite(std::uint32_t width, std::uint32_t height,
                                 const unsigned char* rgba) {
    if (width == 0 || height == 0 || !rgba) return;
    const VkDeviceSize bytes = (VkDeviceSize)width * height * 4;
    Buffer staging = makeBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    std::memcpy(staging.mapped, rgba, (std::size_t)bytes);

    if (spriteView_) vkDestroyImageView(context_.device, spriteView_, nullptr);
    if (spriteImage_) vkDestroyImage(context_.device, spriteImage_, nullptr);
    if (spriteMemory_) vkFreeMemory(context_.device, spriteMemory_, nullptr);
    spriteView_ = VK_NULL_HANDLE;
    spriteImage_ = VK_NULL_HANDLE;
    spriteMemory_ = VK_NULL_HANDLE;

    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    imageInfo.extent = {width, height, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VK_CHECK(vkCreateImage(context_.device, &imageInfo, nullptr, &spriteImage_));
    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(context_.device, spriteImage_, &req);
    VkMemoryAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = findMemoryType(context_.physicalDevice, req.memoryTypeBits,
                                           VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VK_CHECK(vkAllocateMemory(context_.device, &alloc, nullptr, &spriteMemory_));
    VK_CHECK(vkBindImageMemory(context_.device, spriteImage_, spriteMemory_, 0));

    // Upload through a one-shot command buffer with layout transitions.
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkCommandBufferAllocateInfo cmdInfo{};
    cmdInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cmdInfo.commandPool = commandPool_;
    cmdInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmdInfo.commandBufferCount = 1;
    VK_CHECK(vkAllocateCommandBuffers(context_.device, &cmdInfo, &cmd));
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(cmd, &begin));

    VkImageMemoryBarrier toTransfer{};
    toTransfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toTransfer.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransfer.image = spriteImage_;
    toTransfer.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    toTransfer.srcAccessMask = 0;
    toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &toTransfer);
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {width, height, 1};
    vkCmdCopyBufferToImage(cmd, staging.buffer, spriteImage_,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    VkImageMemoryBarrier toShader = toTransfer;
    toShader.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toShader.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    toShader.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toShader.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &toShader);
    VK_CHECK(vkEndCommandBuffer(cmd));
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    VK_CHECK(vkQueueSubmit(context_.queue, 1, &submit, VK_NULL_HANDLE));
    VK_CHECK(vkQueueWaitIdle(context_.queue));
    vkFreeCommandBuffers(context_.device, commandPool_, 1, &cmd);
    vkDestroyBuffer(context_.device, staging.buffer, nullptr);
    vkFreeMemory(context_.device, staging.memory, nullptr);

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = spriteImage_;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VK_CHECK(vkCreateImageView(context_.device, &viewInfo, nullptr, &spriteView_));
    // render() calls updateRenderSet() before recording each frame, so the new
    // view is picked up without touching descriptor sets that may be in flight.
}

void VulkanBackend::setSpriteTexture(const char* path) {
    if (hostMode())
        throw std::logic_error("ember/vulkan: setSpriteTexture is not allowed inside beginVulkanFrame/endVulkanFrame");
    if (!path || path[0] == '\0') {
        uploadBuiltinSprite();
        return;
    }
    int w = 0, h = 0, ch = 0;
    unsigned char* data = nullptr;
#ifdef EMBER_USE_STB
    data = stbi_load(path, &w, &h, &ch, 4);
    if (!data) {
        std::fprintf(stderr, "[ember] warning: cannot load sprite '%s' (%s); using built-in gradient\n",
                     path, stbi_failure_reason());
        uploadBuiltinSprite();
        return;
    }
#else
    (void)w; (void)h; (void)ch;
    std::fprintf(stderr,
                 "[ember] warning: cannot load sprite '%s' (EMBER_USE_STB not defined); using built-in gradient\n",
                 path);
    uploadBuiltinSprite();
    return;
#endif
    if (w > 0 && h > 0) uploadSprite((std::uint32_t)w, (std::uint32_t)h, data);
    else uploadBuiltinSprite();
#ifdef EMBER_USE_STB
    stbi_image_free(data);
#endif
    if (debug_) std::fprintf(stderr, "[ember] sprite '%s' loaded (%dx%d)\n", path, w, h);
}
} // namespace ember::detail::vulkan
