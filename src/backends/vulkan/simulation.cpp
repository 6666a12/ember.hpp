#include "vulkan_backend.hpp"
#include "../opengl/params.hpp"
#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <utility>

namespace ember::detail::vulkan {
namespace {
void memoryBarrierStages(VkCommandBuffer cmd, VkPipelineStageFlags srcStage,
                         VkAccessFlags src, VkAccessFlags dst) {
    VkMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    b.srcAccessMask = src;
    b.dstAccessMask = dst;
    vkCmdPipelineBarrier(cmd, srcStage,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                         VK_PIPELINE_STAGE_TRANSFER_BIT |
                         VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT,
                         0, 1, &b, 0, nullptr, 0, nullptr);
}
void memoryBarrier(VkCommandBuffer cmd, VkAccessFlags src, VkAccessFlags dst) {
    memoryBarrierStages(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, src, dst);
}
void memoryBarrierWithIndirect(VkCommandBuffer cmd, VkAccessFlags src, VkAccessFlags dst) {
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
void transferToShader(VkCommandBuffer cmd, VkAccessFlags dst) {
    memoryBarrierStages(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, dst);
}
} // namespace

void VulkanBackend::scheduleGpu(std::int32_t phase, std::uint32_t requests, std::uint32_t frameSlot) {
    // schedule.comp runs 1x1x1. Phase 0 resets the draw/request counters, writes
    // the request count and emits the integration command; phase 1 emits the
    // tiled sort command list. Both are followed by a barrier that publishes the
    // SSBO writes to later compute and indirect-command reads.
    const std::uint32_t slot = frameSlot == kFramesInFlight ? activeSlot() : frameSlot;
    FrameResources& frame = frames_[slot];
    VkCommandBuffer cmd = activeCmd(slot);

    opengl::ScheduleParams sp{};
    sp.schedulePhase = phase;
    sp.requestCount = requests;
    memoryBarrier(cmd, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    vkCmdUpdateBuffer(cmd, frame.uboSchedule.buffer, 0, sizeof(sp), &sp);
    transferToShader(cmd, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, schedulePipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, simPipelineLayout_,
                            0, 1, &frame.simSet, 0, nullptr);
    vkCmdDispatch(cmd, 1, 1, 1);
    memoryBarrierWithIndirect(cmd, VK_ACCESS_SHADER_WRITE_BIT,
                              VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT |
                              VK_ACCESS_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT);
}

void VulkanBackend::setGpuDriven(bool enabled) {
    if (hostMode())
        throw std::logic_error("ember/vulkan: setGpuDriven is not allowed inside beginVulkanFrame/endVulkanFrame");
    if (enabled == gpuDriven_) return;
    if (enabled) {
        validateGpuCapacity(capacity_);
        if (sortEnabled_ && !sortPipeline_) createSortPipeline();
        if (sortEnabled_ && !sortPipeline_)
            throw std::invalid_argument("ember: GPU-driven mode requires the GPU scheduling sort protocol");
        if (!schedulePipeline_) createSimPipeline();
        if (!schedulePipeline_)
            throw std::invalid_argument("ember: Vulkan backend lacks the GPU scheduling protocol");
        // Schedule storage + the 4-slot statistics ring (mirrors GL's
        // dispatchBuf_ and statisticsBuffers_ allocation). The readback buffers
        // are host visible; the copy is recorded per frame in update().
        if (!scheduleBuf_.valid())
            scheduleBuf_ = makeBuffer(kScheduleBytes,
                                      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                      VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
                                      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        const VkMemoryPropertyFlags host =
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        for (auto& slot : statisticsSlots_) {
            if (!slot.readback.valid())
                slot.readback = makeBuffer(5 * sizeof(std::uint32_t),
                                           VK_BUFFER_USAGE_TRANSFER_DST_BIT, host);
        }
        gpuDriven_ = true;
        sortScheduleValid_ = false;
    } else {
        // Deliberate one-time synchronization at mode switch — skipped while
        // unobserved host work may still be in flight (host must submit+wait
        // first; reading readbackBuf_ then would publish garbage silently).
        if (!hostDirty_) refreshCounts();
        for (auto& slot : statisticsSlots_) {
            if (slot.readback.mapped) { vkUnmapMemory(context_.device, slot.readback.memory); slot.readback.mapped = nullptr; }
            if (slot.readback.buffer) vkDestroyBuffer(context_.device, slot.readback.buffer, nullptr);
            if (slot.readback.memory) vkFreeMemory(context_.device, slot.readback.memory, nullptr);
            slot.readback = Buffer{};
            slot.pending = false;
        }
        if (scheduleBuf_.buffer) {
            if (scheduleBuf_.mapped) { vkUnmapMemory(context_.device, scheduleBuf_.memory); scheduleBuf_.mapped = nullptr; }
            vkDestroyBuffer(context_.device, scheduleBuf_.buffer, nullptr);
            vkFreeMemory(context_.device, scheduleBuf_.memory, nullptr);
            scheduleBuf_ = Buffer{};
        }
        gpuDriven_ = false;
        resetStatistics();
    }
}

void VulkanBackend::update(const SimulationParameters& p, const UpdateBatch& batch) {
    if (!context_.device) throw std::runtime_error("ember/vulkan: no device context set");

    const auto& reqs = batch.requests;
    const auto total = batch.total;
    const auto dt = batch.dt;
    const std::uint32_t nr = (std::uint32_t)reqs.size();

    // Serialize against the frame that used this slot, then recycle it. In GPU
    // mode this fence also gates the newest statistics sample (poll is
    // non-blocking; the wait here is only for the returned slot). In host mode
    // beginVulkanFrame already waited this slot and the backend owns no fence.
    const bool host = hostMode();
    const std::uint32_t frameSlot = activeSlot();
    FrameResources& frame = frames_[frameSlot];
    if (!host) {
        waitFrame(frameSlot);
        VK_CHECK(vkResetFences(context_.device, 1, &frame.fence));
    }

    // Grow force/palette storage before the descriptor set is refreshed, so
    // updateSimSet never points at a buffer that is about to be destroyed.
    if (paletteDirty_) ensureHostBuffer(paletteBuf_, palette_.size() * sizeof(glm::vec4));
    if (attractorsDirty_) ensureHostBuffer(attractorBuf_, attractors_.size() * sizeof(glm::vec4));
    if (vortexesDirty_) ensureHostBuffer(vortexBuf_, vortexes_.size() * sizeof(Vortex));
    if (springsDirty_) ensureHostBuffer(springBuf_, springs_.size() * sizeof(Spring));

    // Bind the current ping-pong buffers into this slot's descriptor set.
    updateSimSet(frameSlot);

    // ---- host-side uploads -------------------------------------------------
    // Force arrays: setters only stage CPU copies, so flush dirty ones now.
    if (paletteDirty_) {
        if (!palette_.empty()) writeBuffer(paletteBuf_, 0, palette_.size() * sizeof(glm::vec4), palette_.data());
        paletteDirty_ = false;
    }
    if (attractorsDirty_) {
        std::vector<glm::vec4> v;
        v.reserve(attractors_.size());
        for (const auto& a : attractors_) v.emplace_back(a.position, a.strength);
        if (!v.empty()) writeBuffer(attractorBuf_, 0, v.size() * sizeof(glm::vec4), v.data());
        attractorsDirty_ = false;
    }
    if (vortexesDirty_) {
        if (!vortexes_.empty()) writeBuffer(vortexBuf_, 0, vortexes_.size() * sizeof(Vortex), vortexes_.data());
        vortexesDirty_ = false;
    }
    if (springsDirty_) {
        if (!springs_.empty()) writeBuffer(springBuf_, 0, springs_.size() * sizeof(Spring), springs_.data());
        springsDirty_ = false;
    }

    opengl::SimParams sp{};
    sp.gravity = p.gravity;             sp.drag = p.drag;
    sp.wind = p.wind;                   sp.turbulence = p.turbulence;
    sp.noiseWindDir = p.noiseWindDir;   sp.noiseWindAmp = p.noiseWindAmp;
    sp.noiseWindScale = p.noiseWindScale; sp.noiseWindSpeed = p.noiseWindSpeed;
    sp.waveAmp = p.waveAmp;             sp.waveOmega = p.waveOmega;
    sp.waveDir = p.waveDir;             sp.dt = dt;
    sp.waveK = p.waveK;                 sp.time = batch.time;
    sp.forceMask = p.forceMask;         sp.dragMode = (int)p.dragMode;
    sp.attractorCount = (std::int32_t)p.attractors.size();
    sp.vortexCount = (std::int32_t)p.vortexes.size();
    sp.springCount = (std::int32_t)p.springs.size();
    sp.boundaryMode = p.boundaryMode == BoundaryMode::Kill ? 1 : 2;
    sp.boundaryY = p.boundaryY;         sp.restitution = p.restitution;
    sp.spawnTotal = total;              sp.frameSeed = batch.seed;
    sp.inputAlive = alive_;             sp.gpuDriven = gpuDriven_ ? 1 : 0;
    sp.eventTemplates = eventTemplateCount_;
    sp.phase = 0;

    if (nr > 0) std::memcpy(frame.spawnStaging.mapped, reqs.data(), nr * sizeof(SpawnRequest));

    // ---- record ------------------------------------------------------------
    // Host mode records into the host-supplied command buffer and owns its
    // begin/end/submit; the backend only appends commands.
    VkCommandBuffer cmd = activeCmd(frameSlot);
    if (!host) {
        VK_CHECK(vkResetCommandBuffer(cmd, 0));
        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        VK_CHECK(vkBeginCommandBuffer(cmd, &begin));
    }

    // Stage spawn requests into device-local storage. The host writes to the
    // mapped staging buffer must be visible to the transfer read.
    if (nr > 0) {
        VkMemoryBarrier hb{};
        hb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        hb.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
        hb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 1, &hb, 0, nullptr, 0, nullptr);
        VkBufferCopy copy{};
        copy.size = (VkDeviceSize)nr * sizeof(SpawnRequest);
        vkCmdCopyBuffer(cmd, frame.spawnStaging.buffer, spawnBuf_.buffer, 1, &copy);
        transferToShader(cmd, VK_ACCESS_SHADER_READ_BIT);
    }

    if (gpuDriven_) {
        // GPU scheduling: schedule.comp resets indirect={4,0,0,0}, writes the
        // request count and emits the phase-0 integration command.
        scheduleGpu(0, nr);
    } else {
        // Draw args + request count for the CPU-scheduled path.
        const std::uint32_t args[4] = {4, 0, 0, 0};
        vkCmdUpdateBuffer(cmd, indirectBuf_.buffer, 0, sizeof(args), args);
        if (nr > 0) vkCmdUpdateBuffer(cmd, counterBuf_.buffer, 2 * sizeof(std::uint32_t), sizeof(nr), &nr);
        const std::uint32_t queueZero[4] = {0, 0, 0, 0}; // event queue header reset (GPU mode: schedule.comp)
        vkCmdUpdateBuffer(cmd, eventQueueBuf_.buffer, 0, sizeof(queueZero), queueZero);
        transferToShader(cmd, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    }

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, simPipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, simPipelineLayout_, 0, 1, &frame.simSet, 0, nullptr);

    // The SimParams block is patched per phase through vkCmdUpdateBuffer: the
    // host writes one value and the transfer is ordered in the command stream.
    auto setPhase = [&](std::int32_t phase) {
        const std::size_t off = offsetof(opengl::SimParams, phase);
        memoryBarrier(cmd, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
        vkCmdUpdateBuffer(cmd, frame.uboSim.buffer, off, sizeof(phase), &phase);
        transferToShader(cmd, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    };

    // Initial full block (phase 0 body already present).
    vkCmdUpdateBuffer(cmd, frame.uboSim.buffer, 0, sizeof(sp), &sp);
    transferToShader(cmd, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);

    // phase 0: integrate live slots. GPU mode consumes the schedule command.
    const std::uint32_t integrationCount = alive_;
    if (gpuDriven_) {
        vkCmdDispatchIndirect(cmd, scheduleBuf_.buffer, 2 * sizeof(std::uint32_t));
    } else if (integrationCount > 0) {
        const std::uint32_t groups = (integrationCount + kSimGroupSize - 1u) / kSimGroupSize;
        vkCmdDispatch(cmd, groups, 1, 1);
    }
    memoryBarrier(cmd, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);

    // phase 4: serial event compression + child slot prefix sum.
    if (eventTemplateCount_ > 0) {
        setPhase(4);
        vkCmdDispatch(cmd, 1, 1, 1);
        memoryBarrier(cmd, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    }

    // phase 3: reserve this batch's slots once.
    if (total > 0 || eventTemplateCount_ > 0) {
        setPhase(3);
        vkCmdDispatch(cmd, 1, 1, 1);
        memoryBarrier(cmd, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    }

    // phase 1: spawn, bounded by capacity for the accepted prefix. Active
    // event templates may add up to the per-frame child cap; the shader drops
    // the excess (j >= uSpawnAccepted) without a frame-internal readback.
    setPhase(1);
    const std::uint32_t eventExtra = eventTemplateCount_ ? kMaxEventChildren : 0u;
    const std::uint32_t spawnTotal = total + eventExtra;
    if (spawnTotal > 0) {
        const std::uint32_t spawnCount = std::min(spawnTotal, capacity_);
        const std::uint32_t groups = (spawnCount + kSimGroupSize - 1u) / kSimGroupSize;
        vkCmdDispatch(cmd, groups, 1, 1);
    }
    memoryBarrier(cmd, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);

    // phase 2: publish draw args.
    setPhase(2);
    vkCmdDispatch(cmd, 1, 1, 1);
    memoryBarrier(cmd, VK_ACCESS_SHADER_WRITE_BIT,
                  VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT |
                  VK_ACCESS_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT);

    // GPU mode with sorting: schedule the tiled sort commands now (the sort
    // itself runs inside render(), consuming this command list).
    if (gpuDriven_ && sortEnabled_) {
        scheduleGpu(1);
        sortScheduleValid_ = true;
    }

    // Copy the counters to host-visible storage. In GPU mode the destination is
    // this frame's statistics slot; the frame fence gates sample validity.
    Buffer* statsTarget = &readbackBuf_;
    if (gpuDriven_) {
        enqueueStatistics();
        for (auto& slot : statisticsSlots_) {
            if (slot.pending && slot.frame == frameCount_ + 1) { statsTarget = &slot.readback; break; }
        }
    }
    VkBufferCopy countersCopy{};
    countersCopy.size = 5 * sizeof(std::uint32_t);
    vkCmdCopyBuffer(cmd, counterBuf_.buffer, statsTarget->buffer, 1, &countersCopy);
    VkMemoryBarrier toHost{};
    toHost.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    toHost.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toHost.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                         0, 1, &toHost, 0, nullptr, 0, nullptr);

    if (!host) {
        VK_CHECK(vkEndCommandBuffer(cmd));
        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &cmd;
        VK_CHECK(vkQueueSubmit(context_.queue, 1, &submit, frame.fence));
        frame.submittedFrame = frameCount_ + 1;
    }

    if (host) {
        // The host owns submission and synchronization. The recorded counters
        // become readable only after the host submits and waits its command
        // buffer; synchronize/readParticles drain the host sample afterwards
        // (statistics.cpp H3).
        countsFresh_ = false;
    } else if (!gpuDriven_) {
        // Synchronous default: read this frame's counters before publishing.
        // Queue order makes this fence wait cover earlier host submissions too.
        VK_CHECK(vkWaitForFences(context_.device, 1, &frame.fence, VK_TRUE, UINT64_MAX));
        hostDirty_ = false;
        const std::uint32_t* counters = static_cast<const std::uint32_t*>(readbackBuf_.mapped);
        alive_ = std::min(counters[0], capacity_);
        allocated_ = std::min(counters[4], capacity_);
        countsFresh_ = true;
        statistics_ = {frameCount_ + 1, alive_, allocated_};
    } else {
        // GPU scheduling never blocks on this frame: the sample stays pending
        // until its fence signals and pollStatistics() (or synchronize) reads it.
        countsFresh_ = false;
    }

    // Swap ping-pong buffers; the fence above (or the ring protocol) guarantees
    // the old bindings are no longer in use. The next update rebinds them.
    std::swap(cur_, nxt_);
    std::swap(liveBuf_, nextLiveBuf_);
    if (!host) {
        frame.recorded = true;
        frameIndex_ = (frameIndex_ + 1u) % kFramesInFlight;
    }
    ++frameCount_;

    if (!host && debug_ && (frameCount_ % 6 == 0)) {
        const std::uint32_t* counters = gpuDriven_
            ? static_cast<const std::uint32_t*>(readbackBuf_.mapped)
            : static_cast<const std::uint32_t*>(readbackBuf_.mapped);
        std::fprintf(stderr, "[ember] vulkan frame=%llu alive=%u allocated=%u head=%u reqs=%u total=%u cap=%u dt=%.4f\n",
                     (unsigned long long)frameCount_, alive_, allocated_, counters[1], nr, total, capacity_, dt);
    }
}
} // namespace ember::detail::vulkan
