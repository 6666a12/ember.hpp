#include "vulkan_backend.hpp"
#include "../opengl/params.hpp"
#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <stdexcept>

namespace ember::detail::vulkan {
namespace {
void computeBarrier(VkCommandBuffer cmd, VkAccessFlags src, VkAccessFlags dst) {
    VkMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    b.srcAccessMask = src;
    b.dstAccessMask = dst;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                         VK_PIPELINE_STAGE_TRANSFER_BIT |
                         VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT,
                         0, 1, &b, 0, nullptr, 0, nullptr);
}
} // namespace

void VulkanBackend::createSortPipeline() {
    if (sortPipeline_) return;
    sortModule_ = makeModule("sort.comp");
    VkPipelineShaderStageCreateInfo stage{};
    stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stage.module = sortModule_;
    stage.pName = "main";
    VkComputePipelineCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    info.stage = stage;
    info.layout = simPipelineLayout_;
    VK_CHECK(vkCreateComputePipelines(context_.device, VK_NULL_HANDLE, 1, &info, nullptr, &sortPipeline_));
}

void VulkanBackend::setSortEnabled(bool on) {
    if (hostMode())
        throw std::logic_error("ember/vulkan: setSortEnabled is not allowed inside beginVulkanFrame/endVulkanFrame");
    if (on == sortEnabled_) return;
    if (on) {
        if (!context_.device) { sortEnabled_ = true; return; }
        createSortPipeline();
        if (!sortPipeline_)
            throw std::invalid_argument("ember: sort shader unavailable");
        sortEnabled_ = true;
    } else {
        sortEnabled_ = false;
    }
    sortScheduleValid_ = false;
}

void VulkanBackend::sortPrepare(std::uint32_t frameSlot) {
    createSortPipeline();
    if (!sortPipeline_) return;

    const std::uint32_t N = nextPow2(gpuDriven_ ? capacity_ : std::max(alive_, 1u));
    // Tiled keys need N floats; the buffer is lazily allocated and invalidated
    // by resize() (capacity change) just like GL's sortKeyCapacity_.
    if (sortKeyCapacity_ < N || !sortKeyBuf_.valid()) {
        if (sortKeyBuf_.mapped) { vkUnmapMemory(context_.device, sortKeyBuf_.memory); sortKeyBuf_.mapped = nullptr; }
        if (sortKeyBuf_.buffer) vkDestroyBuffer(context_.device, sortKeyBuf_.buffer, nullptr);
        if (sortKeyBuf_.memory) vkFreeMemory(context_.device, sortKeyBuf_.memory, nullptr);
        sortKeyBuf_ = makeBuffer((VkDeviceSize)N * sizeof(float),
                                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        sortKeyCapacity_ = N;
    }

    FrameResources& frame = frames_[frameSlot];
    // Scratch (binding 12) is the depth-key cache during sorting; rebind for
    // this frame's sim set. updateSimSet() restores nextLive at the next update.
    VkDescriptorBufferInfo keyInfo{};
    keyInfo.buffer = sortKeyBuf_.buffer;
    keyInfo.offset = 0;
    keyInfo.range = VK_WHOLE_SIZE;
    VkWriteDescriptorSet keyWrite{};
    keyWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    keyWrite.dstSet = frame.simSet;
    keyWrite.dstBinding = kBindingScratch;
    keyWrite.descriptorCount = 1;
    keyWrite.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    keyWrite.pBufferInfo = &keyInfo;
    vkUpdateDescriptorSets(context_.device, 1, &keyWrite, 0, nullptr);
}

void VulkanBackend::sortParticles(const glm::mat4& view, VkCommandBuffer cmd, std::uint32_t frameSlot) {
    createSortPipeline();
    if (!sortPipeline_) return;

    // GPU mode consumes the command list emitted by schedule.comp; regenerate
    // it lazily if sort was enabled (or the buffers rebuilt) since the update.
    if (gpuDriven_ && !sortScheduleValid_) {
        scheduleGpu(1, 0, frameSlot);
        sortScheduleValid_ = true;
    }

    const std::uint32_t N = nextPow2(gpuDriven_ ? capacity_ : std::max(alive_, 1u));
    const std::uint32_t groups = (N + 255u) / 256u;

    FrameResources& frame = frames_[frameSlot];

    opengl::SortParams sp{};
    sp.view = view;
    sp.capacity = capacity_;
    sp.alive = alive_;
    sp.paddedN = N;
    sp.gpuDriven = gpuDriven_ ? 1 : 0;
    auto uploadSort = [&]() {
        vkCmdUpdateBuffer(cmd, frame.uboSort.buffer, 0, sizeof(sp), &sp);
        VkMemoryBarrier b{};
        b.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &b, 0, nullptr, 0, nullptr);
    };

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, sortPipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, simPipelineLayout_,
                            0, 1, &frame.simSet, 0, nullptr);

    const bool tiled = true; // the built-in sort.comp always uses 256-entry tiles
    std::uint32_t dispatchOffset = 5 * sizeof(std::uint32_t);
    auto dispatch = [&]() {
        if (gpuDriven_) {
            vkCmdDispatchIndirect(cmd, scheduleBuf_.buffer, dispatchOffset);
            dispatchOffset += 3 * sizeof(std::uint32_t);
        } else {
            vkCmdDispatch(cmd, groups, 1, 1);
        }
    };

    // mode 0 + tileMode 1: build the live permutation and cache depth keys;
    // the shader also sorts each 256-entry tile in shared memory.
    sp.mode = 0; sp.tileMode = 1; sp.k = 0; sp.j = 0;
    uploadSort();
    dispatch();
    computeBarrier(cmd, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);

    // Exact back-to-front merges. Cross-tile steps run in one dispatch, the
    // final uJ=128..1 window per stage runs in shared memory (tileMode 2).
    for (std::uint32_t k = tiled ? 512u : 2u; k <= N; k <<= 1) {
        for (std::uint32_t j = k >> 1; j >= (tiled ? 256u : 1u); j >>= 1) {
            sp.k = k; sp.j = j; sp.mode = 1; sp.tileMode = 0;
            uploadSort();
            dispatch();
            computeBarrier(cmd, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
        }
        if (tiled) {
            sp.tileMode = 2;
            uploadSort();
            dispatch();
            computeBarrier(cmd, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
        }
    }
    // The particle VS reads sorted[] and live[] afterwards.
    computeBarrier(cmd, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
}
} // namespace ember::detail::vulkan
