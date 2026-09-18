#include "vulkan_backend.hpp"
#include "../opengl/params.hpp"
#include <algorithm>
#include <cstddef>
#include <cstring>
#include <stdexcept>

namespace ember::detail::vulkan {

void VulkanBackend::validateGpuCapacity(std::uint32_t capacity) const {
    // Mirrors the GL validateGpuCapacity: both the integration dispatch
    // (64-wide) and the tiled sort dispatch (256-wide) must fit the device.
    const std::uint64_t count = limits_.maxComputeWorkGroupCountX;
    if ((capacity + 63ull) / 64ull > count ||
        (nextPow2(capacity) + 255ull) / 256ull > count)
        throw std::invalid_argument("ember: capacity exceeds GPU indirect dispatch limits");
}

void VulkanBackend::destroyStatisticsSlots() {
    if (!context_.device) return;
    for (auto& slot : statisticsSlots_) {
        if (slot.readback.mapped) { vkUnmapMemory(context_.device, slot.readback.memory); slot.readback.mapped = nullptr; }
        if (slot.readback.buffer) vkDestroyBuffer(context_.device, slot.readback.buffer, nullptr);
        if (slot.readback.memory) vkFreeMemory(context_.device, slot.readback.memory, nullptr);
        slot.readback = Buffer{};
        slot.pending = false;
        slot.frame = 0;
    }
}

void VulkanBackend::resetStatistics() {
    // Invalidate pending samples: orphan the readback storage instead of
    // reusing an in-flight copy (matches GL's resetStatistics). Buffers are
    // kept (owned by the GPU-driven mode) but marked unused.
    for (auto& slot : statisticsSlots_) {
        slot.pending = false;
        slot.frame = 0;
    }
    countsFresh_ = true;
    statistics_ = {frameCount_, alive_, allocated_};
}

void VulkanBackend::refreshCounts() const {
    // The synchronous update path already waited for this frame's counters;
    // the cached values are exact whenever countsFresh_ is set.
    if (countsFresh_ || !context_.device) return;
    const std::uint32_t* counters = static_cast<const std::uint32_t*>(readbackBuf_.mapped);
    alive_ = std::min(counters[0], capacity_);
    allocated_ = std::min(counters[4], capacity_);
    countsFresh_ = true;
    statistics_ = {frameCount_, alive_, allocated_};
}

ParticleStatistics VulkanBackend::synchronizeStatistics() const {
    if (!context_.device) return statistics_;
    if (hostMode())
        throw std::logic_error("ember/vulkan: statistics queries are not allowed inside beginVulkanFrame/endVulkanFrame");
    // Exact path: wait for each pending sample's frame fence, then read the
    // newest one (the contract explicitly allows synchronizeStatistics to wait).
    // Host samples (H3) were recorded into a host command buffer the caller has
    // already submitted and waited; drain them without touching a fence.
    auto* self = const_cast<VulkanBackend*>(this);
    if (gpuDriven_) self->drainStatistics(true);
    else self->refreshCounts(); // synchronous updates publish before returning
    if (statistics_.frame == frameCount_) {
        self->alive_ = statistics_.alive;
        self->allocated_ = statistics_.allocated;
        self->countsFresh_ = true;
    }
    return statistics_;
}

ParticleStatistics VulkanBackend::pollStatistics() {
    if (!context_.device) return statistics_;
    // Non-blocking: never waits on unfinished GPU work. A sample is valid when
    // its own frame fence signalled, or when the frame slot has since been
    // reused (the queue is in-order, so the earlier copy already completed).
    drainStatistics(false);
    return statistics_;
}

void VulkanBackend::drainStatistics(bool wait) {
    for (auto& slot : statisticsSlots_) {
        if (!slot.pending) continue;
        if (slot.hostSample) {
            // H2: poll must never validate a host sample (the backend cannot see
            // the host's submission). H3 (wait) reads it because the caller has
            // already submitted and waited its command buffer.
            if (!wait) continue;
        } else {
            FrameResources& f = frames_[slot.frameSlot];
            if (f.submittedFrame != 0 && f.submittedFrame <= slot.submission) {
                if (wait) {
                    VkResult r = vkWaitForFences(context_.device, 1, &f.fence, VK_TRUE, UINT64_MAX);
                    if (r != VK_SUCCESS && r != VK_TIMEOUT) continue;
                } else if (vkGetFenceStatus(context_.device, f.fence) != VK_SUCCESS) {
                    continue;
                }
            }
        }
        const std::uint32_t* c = static_cast<const std::uint32_t*>(slot.readback.mapped);
        if (slot.frame > statistics_.frame) {
            statistics_ = {slot.frame, std::min(c[0], capacity_), std::min(c[4], capacity_)};
            if (statistics_.frame == frameCount_) {
                alive_ = statistics_.alive;
                allocated_ = statistics_.allocated;
                countsFresh_ = true;
            }
        }
        slot.pending = false;
        slot.hostSample = false;
    }
}

void VulkanBackend::enqueueStatistics() {
    pollStatistics();
    for (std::uint32_t i = 0; i < kStatisticsSlots; ++i) {
        StatisticsSlot& slot = statisticsSlots_[i];
        if (slot.pending) continue;
        // The copy itself is recorded inside update()'s command buffer; here we
        // only reserve the slot and remember which frame fence gates it. The
        // sample belongs to the sequence number this update is about to publish
        // (mirrors the GL statisticsFrames_ convention).
        slot.frame = frameCount_ + 1;
        slot.frameSlot = activeSlot();
        // submittedFrame is set right after the submit that records this copy;
        // record the expected value so validation can tell "still the same
        // submission" from "the frame slot has already been reused" (in which
        // case the copy is complete because the queue is in-order).
        slot.submission = frameCount_ + 1;
        // H1: host-recorded samples are gated by the host's own submission, not
        // by a backend fence. Only exact queries (H3) may drain them.
        slot.hostSample = hostMode();
        slot.pending = true;
        return;
    }
    ++droppedStatistics_; // bounded queue: skip telemetry, never block simulation
}

std::vector<Particle> VulkanBackend::readParticles(std::uint32_t max) const {
    if (hostMode())
        throw std::logic_error("ember/vulkan: readParticles is not allowed inside beginVulkanFrame/endVulkanFrame");
    if (gpuDriven_) {
        // H3: the host submitted and waited its command buffer before calling.
        auto* self = const_cast<VulkanBackend*>(this);
        self->drainStatistics(true);
        if (statistics_.frame == frameCount_) {
            self->alive_ = statistics_.alive;
            self->allocated_ = statistics_.allocated;
            self->countsFresh_ = true;
        }
    } else {
        refreshCounts();
    }
    std::vector<Particle> out;
    if (!context_.device || allocated_ == 0 || alive_ == 0) return out;
    const std::uint32_t n = std::min(max == 0 ? alive_ : std::min(max, alive_), capacity_);
    if (n == 0) return out;

    // Copy the occupied extent back through a temporary host-visible buffer.
    const VkDeviceSize bytes = (VkDeviceSize)allocated_ * sizeof(Particle);
    Buffer staging = const_cast<VulkanBackend*>(this)->makeBuffer(
        bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    queueWaitIdle();
    hostDirty_ = false; // queue drained: earlier host work is complete
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkCommandBufferAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc.commandPool = commandPool_;
    alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc.commandBufferCount = 1;
    VK_CHECK(vkAllocateCommandBuffers(context_.device, &alloc, &cmd));
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(cmd, &begin));
    VkBufferCopy copy{};
    copy.size = bytes;
    vkCmdCopyBuffer(cmd, cur_->buffer, staging.buffer, 1, &copy);
    VK_CHECK(vkEndCommandBuffer(cmd));
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    VK_CHECK(vkQueueSubmit(context_.queue, 1, &submit, VK_NULL_HANDLE));
    VK_CHECK(vkQueueWaitIdle(context_.queue));
    vkFreeCommandBuffers(context_.device, commandPool_, 1, &cmd);

    const Particle* slots = static_cast<const Particle*>(staging.mapped);
    out.reserve(n);
    for (std::uint32_t i = 0; i < allocated_ && out.size() < n; ++i) {
        if (slots[i].life.x >= 0.f) out.push_back(slots[i]);
    }
    if (staging.mapped) vkUnmapMemory(context_.device, staging.memory);
    vkDestroyBuffer(context_.device, staging.buffer, nullptr);
    vkFreeMemory(context_.device, staging.memory, nullptr);
    return out;
}
} // namespace ember::detail::vulkan
