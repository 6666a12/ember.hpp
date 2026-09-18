#include "vulkan_backend.hpp"
#include "../opengl/params.hpp"
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace ember::detail::vulkan {
namespace {
std::uint32_t findMemoryType(VkPhysicalDevice physical, std::uint32_t typeBits,
                             VkMemoryPropertyFlags props) {
    VkPhysicalDeviceMemoryProperties mem{};
    vkGetPhysicalDeviceMemoryProperties(physical, &mem);
    for (std::uint32_t i = 0; i < mem.memoryTypeCount; ++i) {
        if ((typeBits & (1u << i)) && (mem.memoryTypes[i].propertyFlags & props) == props)
            return i;
    }
    throw std::runtime_error("ember/vulkan: no suitable memory type");
}
} // namespace

Buffer VulkanBackend::makeBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                                 VkMemoryPropertyFlags props) {
    Buffer out;
    if (size == 0) size = 1; // Vulkan forbids zero-sized buffers
    VkBufferCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size = size;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VK_CHECK(vkCreateBuffer(context_.device, &info, nullptr, &out.buffer));

    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(context_.device, out.buffer, &req);
    VkMemoryAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = findMemoryType(context_.physicalDevice, req.memoryTypeBits, props);
    VK_CHECK(vkAllocateMemory(context_.device, &alloc, nullptr, &out.memory));
    VK_CHECK(vkBindBufferMemory(context_.device, out.buffer, out.memory, 0));
    out.size = size;
    if (props & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) {
        VK_CHECK(vkMapMemory(context_.device, out.memory, 0, VK_WHOLE_SIZE, 0, &out.mapped));
    }
    return out;
}

void VulkanBackend::writeBuffer(const Buffer& b, VkDeviceSize offset, VkDeviceSize bytes,
                                const void* src) {
    if (!src || bytes == 0) return;
    if (b.mapped) {
        std::memcpy(static_cast<std::byte*>(b.mapped) + offset, src, static_cast<std::size_t>(bytes));
        VkMappedMemoryRange range{};
        range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
        range.memory = b.memory;
        range.offset = 0;
        range.size = VK_WHOLE_SIZE;
        vkFlushMappedMemoryRanges(context_.device, 1, &range);
        return;
    }
    // Device-local fallback: one-shot staging copy through the command pool.
    Buffer staging = makeBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    std::memcpy(staging.mapped, src, static_cast<std::size_t>(bytes));
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
    vkCmdCopyBuffer(cmd, staging.buffer, b.buffer, 1, &copy);
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
}

void VulkanBackend::queueWaitIdle() const {
    if (context_.queue) vkQueueWaitIdle(context_.queue);
}

VkShaderModule VulkanBackend::makeModule(const char* name) {
    const SpirvBlob blob = loadShader(name);
    if (!blob.valid()) throw std::runtime_error(std::string("ember/vulkan: SPIR-V unavailable: ") + name);
    VkShaderModuleCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = blob.size;
    info.pCode = reinterpret_cast<const std::uint32_t*>(blob.data);
    VkShaderModule module = VK_NULL_HANDLE;
    VK_CHECK(vkCreateShaderModule(context_.device, &info, nullptr, &module));
    return module;
}

void VulkanBackend::destroyBuffers() {
    // Persistent capacity-sized buffers only. Per-frame UBOs/staging outlive
    // resize() (they are not capacity-dependent) and are torn down separately
    // in destroyPerFrameBuffers() at backend destruction.
    Buffer* all[] = {&bufA_, &bufB_, &spawnBuf_, &paletteBuf_, &attractorBuf_, &vortexBuf_,
                     &springBuf_, &deadBuf_, &counterBuf_, &liveBuf_, &nextLiveBuf_,
                     &sortedBuf_, &indirectBuf_, &readbackBuf_, &sortKeyBuf_,
                     &eventTagBuf_, &eventTemplateBuf_, &eventQueueBuf_, &curvesBuf_};
    for (Buffer* b : all) {
        if (b->mapped) { vkUnmapMemory(context_.device, b->memory); b->mapped = nullptr; }
        if (b->buffer) vkDestroyBuffer(context_.device, b->buffer, nullptr);
        if (b->memory) vkFreeMemory(context_.device, b->memory, nullptr);
        *b = Buffer{};
    }
}

void VulkanBackend::destroyPerFrameBuffers() {
    for (auto& f : frames_) {
        Buffer* perFrame[] = {&f.uboSim, &f.uboSchedule, &f.uboDraw, &f.uboFrag, &f.uboBloom, &f.uboSort, &f.spawnStaging};
        for (Buffer* b : perFrame) {
            if (b->mapped) { vkUnmapMemory(context_.device, b->memory); b->mapped = nullptr; }
            if (b->buffer) vkDestroyBuffer(context_.device, b->buffer, nullptr);
            if (b->memory) vkFreeMemory(context_.device, b->memory, nullptr);
            *b = Buffer{};
        }
    }
}

void VulkanBackend::ensureHostBuffer(Buffer& b, VkDeviceSize bytes) {
    // Force arrays arrive in arbitrary sizes; grow the persistently mapped
    // buffer on demand. Callers do this right before updateSimSet() so the
    // descriptor set never references a destroyed buffer.
    if (b.valid() && b.size >= bytes) return;
    if (b.mapped) { vkUnmapMemory(context_.device, b.memory); b.mapped = nullptr; }
    if (b.buffer) vkDestroyBuffer(context_.device, b.buffer, nullptr);
    if (b.memory) vkFreeMemory(context_.device, b.memory, nullptr);
    b = makeBuffer(std::max<VkDeviceSize>(bytes, 64), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
}

VulkanBackend::VulkanBackend() {
    // Curves default to all-ones (disabled); x * 1.0 is bit-identical to the
    // plain render path until the facade uploads real keys.
    for (auto& c : curvesLut_.color) c = glm::vec4(1.f);
    for (auto& s : curvesLut_.size) s = 1.f;
}

void VulkanBackend::createDeviceObjects() {
    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = context_.queueFamilyIndex;
    VK_CHECK(vkCreateCommandPool(context_.device, &poolInfo, nullptr, &commandPool_));

    // Simulation set (set 0): bindings 0-13 and 20-22 storage buffers, 14-19
    // uniforms. Bindings 20-22 are the WO-08 event metadata/templates/queue.
    VkDescriptorSetLayoutBinding simBindings[23]{};
    const auto addSimBinding = [&](std::uint32_t i, VkDescriptorType type) {
        simBindings[i].binding = i;
        simBindings[i].descriptorType = type;
        simBindings[i].descriptorCount = 1;
        simBindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    };
    for (std::uint32_t i = 0; i <= kBindingSchedule; ++i)
        addSimBinding(i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    for (std::uint32_t i = kBindingEventTags; i <= kBindingEventQueue; ++i)
        addSimBinding(i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    for (std::uint32_t i = kBindingUboSim; i <= kBindingUboBloom; ++i)
        addSimBinding(i, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
    VkDescriptorSetLayoutCreateInfo simLayoutInfo{};
    simLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    simLayoutInfo.bindingCount = 23;
    simLayoutInfo.pBindings = simBindings;
    VK_CHECK(vkCreateDescriptorSetLayout(context_.device, &simLayoutInfo, nullptr, &simSetLayout_));

    // Render set (set 1): relocates the per-stage GL bindings into one set with
    // non-colliding types (see the EMBER_RENDER_SET guards in the shaders).
    VkDescriptorSetLayoutBinding renderBindings[9]{};
    const auto addRender = [&](std::uint32_t binding, VkDescriptorType type, VkShaderStageFlags stages) {
        renderBindings[binding].binding = binding;
        renderBindings[binding].descriptorType = type;
        renderBindings[binding].descriptorCount = 1;
        renderBindings[binding].stageFlags = stages;
    };
    addRender(0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_VERTEX_BIT); // particles
    addRender(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_VERTEX_BIT); // sorted
    addRender(2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_VERTEX_BIT); // live
    addRender(3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT); // sprite
    addRender(4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT); // scene depth
    addRender(5, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT); // scene color
    addRender(6, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_VERTEX_BIT);   // DrawParams
    addRender(7, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_FRAGMENT_BIT); // FragParams
    addRender(8, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_VERTEX_BIT);   // lifecycle-curve LUTs
    VkDescriptorSetLayoutCreateInfo renderLayoutInfo{};
    renderLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    renderLayoutInfo.bindingCount = 9;
    renderLayoutInfo.pBindings = renderBindings;
    VK_CHECK(vkCreateDescriptorSetLayout(context_.device, &renderLayoutInfo, nullptr, &renderSetLayout_));


    VkDescriptorPoolSize poolSizes[3]{};
    poolSizes[0].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    // Storage descriptors: sim set 14 (0-13) + 3 event (20-22) = 17, plus the
    // render set's 4 (particles/sorted/live + curves 8) = 21 per frame, +4 spare.
    poolSizes[0].descriptorCount = 21 * kFramesInFlight + 4;
    poolSizes[1].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    poolSizes[1].descriptorCount = 8 * kFramesInFlight + 4;
    poolSizes[2].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[2].descriptorCount = 8 * kFramesInFlight + 4;
    VkDescriptorPoolCreateInfo descriptorInfo{};
    descriptorInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    descriptorInfo.maxSets = 4 * kFramesInFlight + 4;
    descriptorInfo.poolSizeCount = 3;
    descriptorInfo.pPoolSizes = poolSizes;
    VK_CHECK(vkCreateDescriptorPool(context_.device, &descriptorInfo, nullptr, &descriptorPool_));

    VkPipelineLayoutCreateInfo simLayout{};
    simLayout.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    simLayout.setLayoutCount = 1;
    simLayout.pSetLayouts = &simSetLayout_;
    VK_CHECK(vkCreatePipelineLayout(context_.device, &simLayout, nullptr, &simPipelineLayout_));

    VkDescriptorSetLayout renderLayouts[1] = {renderSetLayout_};
    VkPipelineLayoutCreateInfo renderLayout{};
    renderLayout.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    renderLayout.setLayoutCount = 1;
    renderLayout.pSetLayouts = renderLayouts;
    VK_CHECK(vkCreatePipelineLayout(context_.device, &renderLayout, nullptr, &renderPipelineLayout_));

    createSimPipeline();
    createRenderPipeline();
    createPerFrame();
}

void VulkanBackend::createSimPipeline() {
    simModule_ = makeModule("simulate.comp");
    VkPipelineShaderStageCreateInfo stage{};
    stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stage.module = simModule_;
    stage.pName = "main";
    VkComputePipelineCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    info.stage = stage;
    info.layout = simPipelineLayout_;
    VK_CHECK(vkCreateComputePipelines(context_.device, VK_NULL_HANDLE, 1, &info, nullptr, &simPipeline_));

    // Scheduler: shares the simulation descriptor set and layout. It is only
    // created on demand (setGpuDriven(true)); a missing SPIR-V module must not
    // break the synchronous backend.
    if (!schedulePipeline_) {
        try {
            scheduleModule_ = makeModule("schedule.comp");
            VkPipelineShaderStageCreateInfo sstage = stage;
            sstage.module = scheduleModule_;
            VkComputePipelineCreateInfo sinfo = info;
            sinfo.stage = sstage;
            VK_CHECK(vkCreateComputePipelines(context_.device, VK_NULL_HANDLE, 1, &sinfo, nullptr, &schedulePipeline_));
        } catch (const std::exception& e) {
            std::fprintf(stderr, "[ember] warning: schedule shader unavailable (%s); GPU scheduling disabled\n", e.what());
            schedulePipeline_ = VK_NULL_HANDLE;
        }
    }
}

void VulkanBackend::createRenderPipeline() {
    vertModule_ = makeModule("particle.vert");
    fragModule_ = makeModule("particle.frag");
    // Pre-create pass/pipelines for the common no-depth host case lazily; the
    // first render() call builds the exact variants it needs.
}

void VulkanBackend::createPerFrame() {
    VkCommandBufferAllocateInfo cmdInfo{};
    cmdInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cmdInfo.commandPool = commandPool_;
    cmdInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmdInfo.commandBufferCount = kFramesInFlight;
    VkCommandBuffer buffers[kFramesInFlight]{};
    VK_CHECK(vkAllocateCommandBuffers(context_.device, &cmdInfo, buffers));

    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    VkDescriptorSetLayout simLayouts[kFramesInFlight];
    VkDescriptorSetLayout renderLayouts[kFramesInFlight];
    for (std::uint32_t i = 0; i < kFramesInFlight; ++i) {
        simLayouts[i] = simSetLayout_;
        renderLayouts[i] = renderSetLayout_;
    }
    VkDescriptorSetAllocateInfo simInfo{};
    simInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    simInfo.descriptorPool = descriptorPool_;
    simInfo.descriptorSetCount = kFramesInFlight;
    simInfo.pSetLayouts = simLayouts;
    VkDescriptorSet simSets[kFramesInFlight]{};
    VK_CHECK(vkAllocateDescriptorSets(context_.device, &simInfo, simSets));
    VkDescriptorSetAllocateInfo renderInfo{};
    renderInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    renderInfo.descriptorPool = descriptorPool_;
    renderInfo.descriptorSetCount = kFramesInFlight;
    renderInfo.pSetLayouts = renderLayouts;
    VkDescriptorSet renderSets[kFramesInFlight]{};
    VK_CHECK(vkAllocateDescriptorSets(context_.device, &renderInfo, renderSets));

    const VkMemoryPropertyFlags host = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for (std::uint32_t i = 0; i < kFramesInFlight; ++i) {
        FrameResources& f = frames_[i];
        f.command = buffers[i];
        f.simSet = simSets[i];
        f.renderSet = renderSets[i];
        VK_CHECK(vkCreateFence(context_.device, &fenceInfo, nullptr, &f.fence));
        f.uboSim = makeBuffer(sizeof(opengl::SimParams), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, host);
        f.uboSort = makeBuffer(sizeof(opengl::SortParams), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, host);
        f.uboSchedule = makeBuffer(sizeof(opengl::ScheduleParams), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, host);
        f.uboDraw = makeBuffer(sizeof(opengl::DrawParams), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, host);
        f.uboFrag = makeBuffer(sizeof(opengl::FragParams), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, host);
        f.uboBloom = makeBuffer(sizeof(opengl::BloomParams), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, host);
        f.spawnStaging = makeBuffer((VkDeviceSize)kMaxSpawnRequests * sizeof(SpawnRequest),
                                    VK_BUFFER_USAGE_TRANSFER_SRC_BIT, host);
    }
}

void VulkanBackend::allocateBuffers(std::uint32_t capacity) {
    const VkMemoryPropertyFlags deviceLocal = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    const VkMemoryPropertyFlags host = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    const VkDeviceSize pbytes = (VkDeviceSize)capacity * sizeof(Particle);

    bufA_ = makeBuffer(pbytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                  VK_BUFFER_USAGE_TRANSFER_SRC_BIT, deviceLocal);
    bufB_ = makeBuffer(pbytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                  VK_BUFFER_USAGE_TRANSFER_SRC_BIT, deviceLocal);
    std::vector<std::byte> zeros((std::size_t)pbytes);
    writeBuffer(bufA_, 0, pbytes, zeros.data());
    writeBuffer(bufB_, 0, pbytes, zeros.data());
    cur_ = &bufA_;
    nxt_ = &bufB_;

    spawnBuf_ = makeBuffer((VkDeviceSize)kMaxSpawnRequests * sizeof(SpawnRequest),
                           VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, deviceLocal);
    paletteBuf_ = makeBuffer(sizeof(glm::vec4), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, host);
    attractorBuf_ = makeBuffer(sizeof(glm::vec4), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, host);
    vortexBuf_ = makeBuffer(sizeof(Vortex), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, host);
    springBuf_ = makeBuffer(sizeof(Spring), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, host);

    deadBuf_ = makeBuffer((VkDeviceSize)capacity * sizeof(std::uint32_t),
                          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, deviceLocal);
    counterBuf_ = makeBuffer(kCounterWords * sizeof(std::uint32_t),
                             VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                             VK_BUFFER_USAGE_TRANSFER_SRC_BIT, deviceLocal);
    liveBuf_ = makeBuffer((VkDeviceSize)capacity * sizeof(std::uint32_t),
                          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, deviceLocal);
    nextLiveBuf_ = makeBuffer((VkDeviceSize)capacity * sizeof(std::uint32_t),
                              VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, deviceLocal);
    sortedBuf_ = makeBuffer((VkDeviceSize)nextPow2(capacity) * sizeof(std::uint32_t),
                            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, deviceLocal);
    indirectBuf_ = makeBuffer(4 * sizeof(std::uint32_t),
                              VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                              VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, deviceLocal);
    readbackBuf_ = makeBuffer(5 * sizeof(std::uint32_t), VK_BUFFER_USAGE_TRANSFER_DST_BIT, host);
    eventTagBuf_ = makeBuffer((VkDeviceSize)capacity * 2 * sizeof(std::uint32_t), // uvec2: tag + birth id
                              VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, deviceLocal);
    eventTemplateBuf_ = makeBuffer((VkDeviceSize)maxEventTemplates * sizeof(SpawnRequest),
                                   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, host);
    eventQueueBuf_ = makeBuffer((VkDeviceSize)kEventQueueBytes,
                                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                deviceLocal);

    const std::uint32_t counters[kCounterWords] = {0, 0, 0, capacity, 0, 0, 0, 0};
    writeBuffer(counterBuf_, 0, sizeof(counters), counters);
    const std::uint32_t args[4] = {4, 0, 0, 0};
    writeBuffer(indirectBuf_, 0, sizeof(args), args);
    const std::uint32_t queueZero[4] = {0, 0, 0, 0};
    writeBuffer(eventQueueBuf_, 0, sizeof(queueZero), queueZero);
    curvesBuf_ = makeBuffer(sizeof(opengl::CurvesParams),
                            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, host);
    writeBuffer(curvesBuf_, 0, sizeof(opengl::CurvesParams), &curvesLut_);
    // resize() reallocates the template buffer; restore the live templates so
    // existing particles keep chaining correctly.
    if (!eventTemplates_.empty()) {
        const VkDeviceSize bytes = (VkDeviceSize)eventTemplates_.size() * sizeof(SpawnRequest);
        writeBuffer(eventTemplateBuf_, 0, bytes, eventTemplates_.data());
    }
    eventTemplatesDirty_ = false;

    alive_ = 0;
    allocated_ = 0;
}

void VulkanBackend::updateSimSet(std::uint32_t frameIndex) {
    FrameResources& f = frames_[frameIndex];
    VkDescriptorBufferInfo infos[23]{};
    auto ssbo = [&](std::uint32_t binding, const Buffer& b) {
        infos[binding].buffer = b.buffer;
        infos[binding].offset = 0;
        infos[binding].range = VK_WHOLE_SIZE;
    };
    ssbo(kBindingCur, *cur_);
    ssbo(kBindingNext, *nxt_);
    ssbo(kBindingSpawn, spawnBuf_);
    ssbo(kBindingDead, deadBuf_);
    ssbo(kBindingCounters, counterBuf_);
    ssbo(kBindingAttractors, attractorBuf_);
    ssbo(kBindingVortexes, vortexBuf_);
    ssbo(kBindingSprings, springBuf_);
    ssbo(kBindingSorted, sortedBuf_);
    ssbo(kBindingPalette, paletteBuf_);
    ssbo(kBindingIndirect, indirectBuf_);
    ssbo(kBindingLive, liveBuf_);
    ssbo(kBindingScratch, nextLiveBuf_);
    // Binding 13 is the scheduler command buffer; when GPU scheduling is off
    // the shader never reads it, so a valid dummy keeps the layout uniform.
    ssbo(kBindingSchedule, scheduleBuf_.valid() ? scheduleBuf_ : counterBuf_);
    ssbo(kBindingEventTags, eventTagBuf_);
    ssbo(kBindingEventTemplates, eventTemplateBuf_);
    ssbo(kBindingEventQueue, eventQueueBuf_);
    auto ubo = [&](std::uint32_t binding, const Buffer& b) {
        infos[binding].buffer = b.buffer;
        infos[binding].offset = 0;
        infos[binding].range = b.size;
    };
    ubo(kBindingUboSim, f.uboSim);
    ubo(kBindingUboSort, f.uboSort);
    ubo(kBindingUboSchedule, f.uboSchedule);
    ubo(kBindingUboDraw, f.uboDraw);
    ubo(kBindingUboFrag, f.uboFrag);
    ubo(kBindingUboBloom, f.uboBloom);

    VkWriteDescriptorSet writes[23]{};
    for (std::uint32_t i = 0; i < 23; ++i) {
        const bool uniform = i >= kBindingUboSim && i <= kBindingUboBloom;
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = f.simSet;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = uniform ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
                                           : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[i].pBufferInfo = &infos[i];
    }
    vkUpdateDescriptorSets(context_.device, 23, writes, 0, nullptr);
}

void VulkanBackend::updateRenderSet(std::uint32_t frameIndex) {
    FrameResources& f = frames_[frameIndex];
    VkDescriptorBufferInfo buffers[5]{};
    buffers[0].buffer = cur_->buffer; buffers[0].range = VK_WHOLE_SIZE;
    buffers[1].buffer = sortedBuf_.buffer; buffers[1].range = VK_WHOLE_SIZE;
    buffers[2].buffer = liveBuf_.buffer; buffers[2].range = VK_WHOLE_SIZE;
    buffers[3].buffer = f.uboDraw.buffer; buffers[3].range = f.uboDraw.size;
    buffers[4].buffer = curvesBuf_.buffer; buffers[4].range = VK_WHOLE_SIZE;
    VkDescriptorBufferInfo fragUbo{};
    fragUbo.buffer = f.uboFrag.buffer;
    fragUbo.range = f.uboFrag.size;

    // Binding 3 is always the sprite. Bindings 4/5 carry the borrowed host
    // depth/color views for soft particles and refraction; until the host
    // injects them they alias the sprite so the descriptors stay valid.
    VkDescriptorImageInfo spriteInfo{};
    spriteInfo.sampler = spriteSampler_;
    spriteInfo.imageView = spriteView_;
    spriteInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    const bool refrDepth = refrDepthView_ != VK_NULL_HANDLE;
    VkImageView depthView = refrDepth ? refrDepthView_ : sceneDepthView_;
    VkSampler depthSampler = refrDepth ? refrSampler_
                                       : (sceneDepthSampler_ ? sceneDepthSampler_ : spriteSampler_);
    VkDescriptorImageInfo depthInfo{};
    depthInfo.sampler = depthSampler;
    depthInfo.imageView = depthView ? depthView : spriteView_;
    depthInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkDescriptorImageInfo colorInfo{};
    colorInfo.sampler = refrSampler_ ? refrSampler_ : spriteSampler_;
    // Refraction depth takes priority when supplied; otherwise the soft-depth
    // view; otherwise the sprite placeholder.
    colorInfo.imageView = sceneColorView_ ? sceneColorView_ : spriteView_;
    colorInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkWriteDescriptorSet writes[9]{};
    auto bufferWrite = [&](std::uint32_t i, VkDescriptorType type, const VkDescriptorBufferInfo* info) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = f.renderSet;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = type;
        writes[i].pBufferInfo = info;
    };
    bufferWrite(0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &buffers[0]);
    bufferWrite(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &buffers[1]);
    bufferWrite(2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &buffers[2]);
    bufferWrite(6, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, &buffers[3]);
    bufferWrite(7, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, &fragUbo);
    bufferWrite(8, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &buffers[4]);
    for (std::uint32_t i : {3u, 4u, 5u}) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = f.renderSet;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    }
    writes[3].pImageInfo = &spriteInfo;
    writes[4].pImageInfo = &depthInfo;
    writes[5].pImageInfo = &colorInfo;
    vkUpdateDescriptorSets(context_.device, 9, writes, 0, nullptr);
}

void VulkanBackend::waitFrame(std::uint32_t index) {
    FrameResources& f = frames_[index];
    if (f.submittedFrame == 0) return; // never submitted: fence is pre-signaled
    VK_CHECK(vkWaitForFences(context_.device, 1, &f.fence, VK_TRUE, UINT64_MAX));
}

VkCommandBuffer VulkanBackend::activeCmd(std::uint32_t slot) const {
    return hostCmd_ != VK_NULL_HANDLE ? hostCmd_ : frames_[slot].command;
}

void VulkanBackend::setFrameTarget(const VulkanFrameTarget& target) {
    if (hostMode())
        throw std::logic_error("ember/vulkan: setVulkanFrameTarget is not allowed inside beginVulkanFrame/endVulkanFrame");
    target_ = target;
}

void VulkanBackend::beginFrame(VkCommandBuffer cmd, std::uint32_t frameIndex) {
    if (!context_.device)
        throw std::logic_error("ember/vulkan: beginVulkanFrame requires a device context");
    if (hostCmd_ != VK_NULL_HANDLE)
        throw std::logic_error("ember/vulkan: a host frame is already open");
    if (cmd == VK_NULL_HANDLE)
        throw std::logic_error("ember/vulkan: beginVulkanFrame needs a command buffer");
    const std::uint32_t slot = frameIndex % kFramesInFlight;
    // Serialize with the backend's last self-submission on this slot, but leave
    // the fence signaled: default mode resumes with an immediate waitFrame hit.
    waitFrame(slot);
    hostCmd_ = cmd;
    hostFrameSlot_ = slot;
}

void VulkanBackend::endFrame() {
    if (hostCmd_ == VK_NULL_HANDLE)
        throw std::logic_error("ember/vulkan: endVulkanFrame without beginVulkanFrame");
    hostCmd_ = VK_NULL_HANDLE;
    hostDirty_ = true; // completion is now the host's to observe
}

void VulkanBackend::initializeDevice() {
    if (commandPool_) return;
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(context_.physicalDevice, &props);
    limits_.maxStorageBufferRange = props.limits.maxStorageBufferRange;
    limits_.maxComputeWorkGroupCountX = props.limits.maxComputeWorkGroupCount[0];
    limits_.minStorageBufferOffsetAlignment = props.limits.minStorageBufferOffsetAlignment;
    limits_.valid = true;
    createDeviceObjects();
    allocateBuffers(capacity_);
    ensureSpriteSampler();
    uploadBuiltinSprite();
    countsFresh_ = true;
    statistics_ = {frameCount_, 0, 0};
    if (debug_) std::fprintf(stderr, "[ember] vulkan initialized (capacity=%u)\n", capacity_);
}

void VulkanBackend::initialize(std::uint32_t capacity, bool debug) {
    if (capacity == 0) throw std::invalid_argument("ember: capacity must be non-zero");
    capacity_ = capacity;
    debug_ = debug;
    frameCount_ = 0;
    if (!context_.device) return; // deferred until setVulkanContext()
    initializeDevice();
}

VulkanBackend::~VulkanBackend() {
    if (!context_.device) return;
    if (context_.queue) vkQueueWaitIdle(context_.queue);
    destroyBloomResources();
    destroyRenderResources();
    destroyBuffers();
    destroyPerFrameBuffers();
    destroyStatisticsSlots();
    for (auto& slot : statisticsSlots_) slot.readback = Buffer{};
    for (Buffer* b : {&scheduleBuf_, &sortKeyBuf_}) {
        if (b->mapped) { vkUnmapMemory(context_.device, b->memory); b->mapped = nullptr; }
        if (b->buffer) vkDestroyBuffer(context_.device, b->buffer, nullptr);
        if (b->memory) vkFreeMemory(context_.device, b->memory, nullptr);
        *b = Buffer{};
    }
    for (auto& f : frames_) {
        if (f.fence) vkDestroyFence(context_.device, f.fence, nullptr);
        f.fence = VK_NULL_HANDLE;
    }
    if (spriteView_) vkDestroyImageView(context_.device, spriteView_, nullptr);
    if (spriteImage_) vkDestroyImage(context_.device, spriteImage_, nullptr);
    if (spriteMemory_) vkFreeMemory(context_.device, spriteMemory_, nullptr);
    if (spriteSampler_) vkDestroySampler(context_.device, spriteSampler_, nullptr);
    if (simPipeline_) vkDestroyPipeline(context_.device, simPipeline_, nullptr);
    if (sortPipeline_) vkDestroyPipeline(context_.device, sortPipeline_, nullptr);
    if (schedulePipeline_) vkDestroyPipeline(context_.device, schedulePipeline_, nullptr);
    if (simModule_) vkDestroyShaderModule(context_.device, simModule_, nullptr);
    if (sortModule_) vkDestroyShaderModule(context_.device, sortModule_, nullptr);
    if (scheduleModule_) vkDestroyShaderModule(context_.device, scheduleModule_, nullptr);
    if (vertModule_) vkDestroyShaderModule(context_.device, vertModule_, nullptr);
    if (fragModule_) vkDestroyShaderModule(context_.device, fragModule_, nullptr);
    if (simPipelineLayout_) vkDestroyPipelineLayout(context_.device, simPipelineLayout_, nullptr);
    if (renderPipelineLayout_) vkDestroyPipelineLayout(context_.device, renderPipelineLayout_, nullptr);
    if (descriptorPool_) vkDestroyDescriptorPool(context_.device, descriptorPool_, nullptr);
    if (simSetLayout_) vkDestroyDescriptorSetLayout(context_.device, simSetLayout_, nullptr);
    if (renderSetLayout_) vkDestroyDescriptorSetLayout(context_.device, renderSetLayout_, nullptr);
    if (commandPool_) vkDestroyCommandPool(context_.device, commandPool_, nullptr);
}

void VulkanBackend::setContext(const VulkanContext& context) {
    if (hostMode())
        throw std::logic_error("ember/vulkan: setVulkanContext is not allowed inside beginVulkanFrame/endVulkanFrame");
    if (commandPool_) throw std::invalid_argument("ember: Vulkan context already set");
    context_ = context;
    // ParticleSystem calls initialize() at construction; when the context
    // arrives afterwards, finish the deferred device-side setup here.
    if (capacity_ != 0 && context_.device) initializeDevice();
}

void VulkanBackend::resize(std::uint32_t capacity) {
    if (hostMode())
        throw std::logic_error("ember/vulkan: resize is not allowed inside beginVulkanFrame/endVulkanFrame");
    validateConfiguration(capacity, false);
    if (!context_.device) { capacity_ = capacity; return; }
    queueWaitIdle();
    hostDirty_ = false; // queue drained: earlier host work is complete
    destroyBuffers();
    capacity_ = capacity;
    allocateBuffers(capacity_);
    const std::uint32_t queueZero[4] = {0, 0, 0, 0};
    writeBuffer(eventQueueBuf_, 0, sizeof(queueZero), queueZero);
    sortKeyCapacity_ = 0; // tiled keys are lazily rebuilt at the next sort
    sortScheduleValid_ = false;
    resetStatistics();
    for (auto& f : frames_) f.submittedFrame = 0;
}

void VulkanBackend::clear() {
    if (hostMode())
        throw std::logic_error("ember/vulkan: clear is not allowed inside beginVulkanFrame/endVulkanFrame");
    if (!context_.device) return;
    queueWaitIdle();
    hostDirty_ = false; // queue drained: earlier host work is complete
    alive_ = 0;
    allocated_ = 0;
    const VkDeviceSize pbytes = (VkDeviceSize)capacity_ * sizeof(Particle);
    std::vector<std::byte> zeros((std::size_t)pbytes);
    writeBuffer(bufA_, 0, pbytes, zeros.data());
    writeBuffer(bufB_, 0, pbytes, zeros.data());
    const std::uint32_t counters[kCounterWords] = {0, 0, 0, capacity_, 0, 0, 0, 0};
    writeBuffer(counterBuf_, 0, sizeof(counters), counters);
    const std::uint32_t args[4] = {4, 0, 0, 0};
    writeBuffer(indirectBuf_, 0, sizeof(args), args);
    const std::uint32_t queueZero[4] = {0, 0, 0, 0};
    writeBuffer(eventQueueBuf_, 0, sizeof(queueZero), queueZero);
    sortScheduleValid_ = false;
    resetStatistics();
    for (auto& f : frames_) f.submittedFrame = 0;
}

void VulkanBackend::validateConfiguration(std::uint32_t capacity, bool sorting) {
    if (capacity == 0 || capacity > (1u << 30))
        throw std::invalid_argument("ember: invalid capacity");
    (void)sorting; // sorting limits are validated in setSortEnabled
    if (!context_.device) return;
    if (!limits_.valid) return;
    const VkDeviceSize pbytes = (VkDeviceSize)capacity * sizeof(Particle);
    if ((std::uint64_t)pbytes > limits_.maxStorageBufferRange)
        throw std::invalid_argument("ember: capacity exceeds maxStorageBufferRange");
    const std::uint64_t groups = ((std::uint64_t)capacity + kSimGroupSize - 1u) / kSimGroupSize;
    if (groups > (std::uint64_t)limits_.maxComputeWorkGroupCountX)
        throw std::invalid_argument("ember: capacity exceeds maxComputeWorkGroupCount");
}

void VulkanBackend::uploadAttractors(const std::vector<Attractor>& values) {
    attractors_ = values;
    attractorsDirty_ = true;
}
void VulkanBackend::uploadVortexes(const std::vector<Vortex>& values) {
    vortexes_ = values;
    vortexesDirty_ = true;
}
void VulkanBackend::uploadSprings(const std::vector<Spring>& values) {
    springs_ = values;
    springsDirty_ = true;
}
void VulkanBackend::uploadPalette(const std::vector<glm::vec4>& values) {
    palette_ = values;
    paletteDirty_ = true;
}
void VulkanBackend::uploadEventTemplates(const std::vector<SpawnRequest>& templates) {
    eventTemplates_ = templates;
    eventTemplateCount_ = (std::uint32_t)std::min<std::size_t>(templates.size(), maxEventTemplates);
    if (!eventTemplateBuf_.valid()) { eventTemplatesDirty_ = true; return; } // deferred until setContext()
    if (!templates.empty()) {
        const VkDeviceSize bytes = (VkDeviceSize)std::min<std::size_t>(templates.size(), maxEventTemplates) * sizeof(SpawnRequest);
        writeBuffer(eventTemplateBuf_, 0, bytes, templates.data());
    }
    eventTemplatesDirty_ = false;
}
void VulkanBackend::uploadLifeCurves(const LifeCurvesLut& lut) {
    curvesLut_ = lut;
    // The first sizeof(CurvesParams) bytes are colorLut + sizeLut; the CPU mask
    // lives past that and is deliberately not uploaded.
    if (curvesBuf_.valid()) writeBuffer(curvesBuf_, 0, sizeof(opengl::CurvesParams), &lut);
}
} // namespace ember::detail::vulkan
