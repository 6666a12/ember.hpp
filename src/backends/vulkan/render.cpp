#include "vulkan_backend.hpp"
#include "../opengl/params.hpp"
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace ember::detail::vulkan {
namespace {
VkAttachmentDescription colorAttachment(VkFormat format) {
    VkAttachmentDescription a{};
    a.format = format;
    a.samples = VK_SAMPLE_COUNT_1_BIT;
    a.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    a.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    a.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    return a;
}
VkAttachmentDescription depthAttachment(VkFormat format) {
    VkAttachmentDescription a{};
    a.format = format;
    a.samples = VK_SAMPLE_COUNT_1_BIT;
    a.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    a.initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    a.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    return a;
}
} // namespace

VkRenderPass VulkanBackend::renderPass(VkFormat color, VkFormat depth) {
    RenderPassKey key{color, depth};
    auto it = renderPasses_.find(key);
    if (it != renderPasses_.end()) return it->second;
    const bool hasDepth = isDepthFormat(depth);
    VkAttachmentDescription attachments[2]{};
    attachments[0] = colorAttachment(color);
    VkAttachmentReference colorRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference depthRef{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorRef;
    if (hasDepth) {
        attachments[1] = depthAttachment(depth);
        subpass.pDepthStencilAttachment = &depthRef;
    }
    VkRenderPassCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    info.attachmentCount = hasDepth ? 2u : 1u;
    info.pAttachments = attachments;
    info.subpassCount = 1;
    info.pSubpasses = &subpass;
    VkRenderPass pass = VK_NULL_HANDLE;
    VK_CHECK(vkCreateRenderPass(context_.device, &info, nullptr, &pass));
    renderPasses_[key] = pass;
    return pass;
}

VkPipeline VulkanBackend::particlePipeline(PipelineKey key) {
    key.pass = key.pass ? key.pass : currentPass_;
    auto it = particlePipelines_.find(key);
    if (it != particlePipelines_.end()) return it->second;

    // Pipelines are tied to the render pass (host target or bloom HDR scene);
    // the key covers blend/depth state plus the pass pointer.
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertModule_;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragModule_;
    stages[1].pName = "main";

    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{};
    raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE; // two-sided billboards
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.f;
    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = key.depthTest ? VK_TRUE : VK_FALSE;
    depthStencil.depthWriteEnable = key.depthWrite ? VK_TRUE : VK_FALSE;
    depthStencil.depthCompareOp = VK_COMPARE_OP_LESS; // GL default depth func
    depthStencil.depthBoundsTestEnable = VK_FALSE;
    depthStencil.stencilTestEnable = VK_FALSE;

    VkPipelineColorBlendAttachmentState blend{};
    blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                           VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    if (key.refraction) {
        blend.blendEnable = VK_FALSE; // pixel replacement
    } else {
        blend.blendEnable = VK_TRUE;
        blend.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        blend.dstColorBlendFactor = key.blend == 0 ? VK_BLEND_FACTOR_ONE
                                                   : VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blend.colorBlendOp = VK_BLEND_OP_ADD;
        blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        blend.dstAlphaBlendFactor = key.blend == 0 ? VK_BLEND_FACTOR_ONE
                                                   : VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blend.alphaBlendOp = VK_BLEND_OP_ADD;
    }
    VkPipelineColorBlendStateCreateInfo colorBlend{};
    colorBlend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlend.attachmentCount = 1;
    colorBlend.pAttachments = &blend;

    const VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{};
    dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamicStates;

    VkGraphicsPipelineCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    info.stageCount = 2;
    info.pStages = stages;
    info.pVertexInputState = &vertexInput;
    info.pInputAssemblyState = &inputAssembly;
    info.pViewportState = &viewportState;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &multisample;
    info.pDepthStencilState = &depthStencil;
    info.pColorBlendState = &colorBlend;
    info.pDynamicState = &dynamic;
    info.layout = renderPipelineLayout_;
    info.renderPass = key.pass;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VK_CHECK(vkCreateGraphicsPipelines(context_.device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline));
    particlePipelines_[key] = pipeline;
    return pipeline;
}

void VulkanBackend::clearParticlePipelines() {
    for (auto& kv : particlePipelines_) vkDestroyPipeline(context_.device, kv.second, nullptr);
    particlePipelines_.clear();
}

void VulkanBackend::ensureRenderResources() {
    if (!vertModule_) createRenderPipeline();
    const VkFormat colorFormat = target_.colorFormat;
    const VkFormat depthFormat = isDepthFormat(target_.depthFormat) && target_.depthView
                                     ? target_.depthFormat : VK_FORMAT_UNDEFINED;
    const RenderPassKey passKey{colorFormat, depthFormat};
    if (!haveTargetFramebuffer_ || !(passKey == targetPassKey_)) {
        // A new pass render pass changes pipeline compatibility. Bloom's
        // composite pipeline is bound to the host pass too, so drop the whole
        // bloom chain and let ensureBloom() rebuild it lazily.
        clearParticlePipelines();
        destroyBloomResources();
        if (targetFramebuffer_) {
            vkDestroyFramebuffer(context_.device, targetFramebuffer_, nullptr);
            targetFramebuffer_ = VK_NULL_HANDLE;
        }
        currentPass_ = renderPass(colorFormat, depthFormat);
        targetPassKey_ = passKey;
    }
    const FramebufferKey framebufferKey{target_.colorView, target_.depthView,
                                        target_.width, target_.height};
    if (!haveTargetFramebuffer_ || !(framebufferKey == targetFramebufferKey_)) {
        if (targetFramebuffer_) vkDestroyFramebuffer(context_.device, targetFramebuffer_, nullptr);
        const bool hasDepth = depthFormat != VK_FORMAT_UNDEFINED;
        VkImageView attachments[2] = {target_.colorView, target_.depthView};
        VkFramebufferCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        info.renderPass = currentPass_;
        info.attachmentCount = hasDepth ? 2u : 1u;
        info.pAttachments = attachments;
        info.width = target_.width;
        info.height = target_.height;
        info.layers = 1;
        VK_CHECK(vkCreateFramebuffer(context_.device, &info, nullptr, &targetFramebuffer_));
        targetFramebufferKey_ = framebufferKey;
        haveTargetFramebuffer_ = true;
    }
}

void VulkanBackend::drawParticles(const RenderParameters& p, const RenderView& camera,
                                  bool refractionPass, VkRenderPass pass, VkFramebuffer framebuffer) {
    const std::uint32_t cmdSlot = target_.frameIndex % kFramesInFlight;
    FrameResources& frame = frames_[cmdSlot];
    VkCommandBuffer cmd = activeCmd(cmdSlot);

    opengl::DrawParams dp{};
    dp.view = camera.view; dp.proj = camera.projection;
    dp.sizeScale = p.sizeScale; dp.streak = p.streak; dp.spinSpeed = p.spin;
    dp.useSorted = sortEnabled_ ? 1 : 0;
    dp.refraction = refractionPass ? 1 : 0;

    opengl::FragParams fp{};
    fp.invProj = glm::inverse(camera.projection); fp.fragProj = camera.projection;
    fp.viewportWidth = camera.width; fp.viewportHeight = camera.height;
    fp.softRadius = p.softRadius;
    fp.useSprite = p.useSprite ? 1 : 0;
    fp.sheetCols = p.sheetCols; fp.sheetRows = p.sheetRows;
    fp.fragRefraction = refractionPass ? 1 : 0;
    if (refractionPass) {
        // Refraction mode 1 needs a host depth texture; fall back to mode 0
        // exactly like the GL backend when none was injected.
        int mode = p.refraction.mode;
        const bool hasDepth = refrDepthView_ != VK_NULL_HANDLE || sceneDepthView_ != VK_NULL_HANDLE;
        if (mode == 1 && !hasDepth) mode = 0;
        fp.refrMode = mode; fp.refrShape = p.refraction.shape;
        fp.refrDome = p.refraction.dome; fp.refrStrength = p.refraction.strength;
        fp.refrIor = p.refraction.ior; fp.refrTint = p.refraction.tint;
        fp.refrAbsorption = p.refraction.absorption; fp.refrFresnel = p.refraction.fresnel;
        fp.refrChroma = p.refraction.chroma; fp.refrSpecular = p.refraction.specular;
        fp.refrLightDir = glm::mat3(camera.view) * p.refraction.lightDir;
    } else {
        const bool soft = p.softParticles && sceneDepthView_ != VK_NULL_HANDLE;
        fp.useSoft = soft ? 1 : 0;
    }
    // Per-pass parameter blocks ride the command stream via vkCmdUpdateBuffer:
    // the bloom/refraction passes differ per call, and a host-visible overwrite
    // would make every recorded pass read the LAST written values. Descriptor
    // sets are refreshed before recording starts (see render()); updating them
    // here would invalidate the in-flight command buffer (no UPDATE_AFTER_BIND).
    vkCmdUpdateBuffer(cmd, frame.uboDraw.buffer, 0, sizeof(dp), &dp);
    vkCmdUpdateBuffer(cmd, frame.uboFrag.buffer, 0, sizeof(fp), &fp);
    {
        VkMemoryBarrier uboBarrier{};
        uboBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        uboBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        uboBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             0, 1, &uboBarrier, 0, nullptr, 0, nullptr);
    }

    PipelineKey key{};
    key.blend = p.blend == BlendMode::Additive ? 0 : 1;
    key.depthTest = p.depthTest ? 1 : 0;
    key.depthWrite = p.depthWrite ? 1 : 0;
    key.refraction = refractionPass ? 1 : 0;
    key.pass = pass;
    VkPipeline pipeline = particlePipeline(key);

    // Own the render pass so bloom can redirect the particle pass into its HDR
    // target; the host path passes currentPass_/targetFramebuffer_.
    VkRenderPassBeginInfo passInfo{};
    passInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    passInfo.renderPass = pass;
    passInfo.framebuffer = framebuffer;
    passInfo.renderArea.extent = {target_.width, target_.height};
    VkClearValue clear{};
    clear.color = {{0.f, 0.f, 0.f, 0.f}};
    // Only the bloom HDR color target is cleared; host color and all depth
    // attachments use LOAD semantics and need no clear values.
    if (pass == bloomScenePass_) {
        passInfo.clearValueCount = 1;
        passInfo.pClearValues = &clear;
    }
    vkCmdBeginRenderPass(cmd, &passInfo, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport viewport{};
    viewport.x = 0.f;
    viewport.y = static_cast<float>(target_.height);
    viewport.width = static_cast<float>(target_.width);
    viewport.height = -static_cast<float>(target_.height);
    viewport.minDepth = 0.f;
    viewport.maxDepth = 1.f;
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    VkRect2D scissor{{0, 0}, {target_.width, target_.height}};
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, renderPipelineLayout_,
                            0, 1, &frame.renderSet, 0, nullptr);
    // Indirect draw args are GPU-written by simulation phase 2 (offset 0); the
    // CPU alive_ readback is not part of the render path.
    vkCmdDrawIndirect(cmd, indirectBuf_.buffer, 0, 1, sizeof(std::uint32_t) * 4);
    vkCmdEndRenderPass(cmd);
}

void VulkanBackend::render(const RenderParameters& p, const RenderView& camera) {
    if (!context_.device || target_.colorView == VK_NULL_HANDLE ||
        target_.width == 0 || target_.height == 0) return;
    // GPU scheduling never depends on the CPU alive_ snapshot for visibility.
    // Host mode also cannot: the recorded spawns are not read back until the
    // host submits, so alive_ may be stale zero while particles exist.
    if (!gpuDriven_ && !hostMode() && alive_ == 0) return;
    if (!std::isfinite(camera.width) || !std::isfinite(camera.height)) return;

    const std::uint32_t slot = target_.frameIndex % kFramesInFlight;
    if (hostMode() && slot != hostFrameSlot_)
        throw std::logic_error("ember/vulkan: VulkanFrameTarget::frameIndex does not match beginVulkanFrame's slot");
    const bool host = hostMode();
    FrameResources& frame = frames_[slot];
    if (!host) {
        waitFrame(slot);
        VK_CHECK(vkResetFences(context_.device, 1, &frame.fence));
    }

    ensureRenderResources();

    // All descriptor updates happen before recording starts: this backend has
    // no UPDATE_AFTER_BIND, so touching a set mid-recording invalidates the
    // command buffer. sortPrepare() also (re)allocates the depth-key buffer.
    updateRenderSet(slot);
    if (sortEnabled_) sortPrepare(slot);
    if (bloom_) {
        ensureBloom(target_.width, target_.height);
        if (bloomFull_.framebuffer != VK_NULL_HANDLE) updateBloomSets(slot);
    }

    VkCommandBuffer cmd = activeCmd(slot);
    if (!host) {
        VK_CHECK(vkResetCommandBuffer(cmd, 0));
        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        VK_CHECK(vkBeginCommandBuffer(cmd, &begin));
    }

    // Depth sorting records compute dispatches before the render pass; the VS
    // consumes sorted[] with useSorted=1. GL runs this at the same point.
    if (sortEnabled_) sortParticles(camera.view, cmd, slot);

    // Bloom mirrors the GL flow: the particle pass renders into an HDR target
    // (halo source), then the particles are drawn normally onto the host and
    // the blurred halo is composited additively.
    const bool useBloom = bloom_ && bloomFull_.framebuffer != VK_NULL_HANDLE &&
                          bloomScenePass_ != VK_NULL_HANDLE;
    if (useBloom) {
        drawParticles(p, camera, false, bloomScenePass_, bloomFull_.framebuffer);
        drawParticles(p, camera, false, currentPass_, targetFramebuffer_);
        runBloom(cmd, p.bloomThreshold);
    } else {
        drawParticles(p, camera, false, currentPass_, targetFramebuffer_);
    }

    // Refraction replaces pixels on the host target after bloom (GL order).
    if (p.refraction.enabled && sceneColorView_ != VK_NULL_HANDLE) {
        drawParticles(p, camera, true, currentPass_, targetFramebuffer_);
    }
    if (!host) {
        VK_CHECK(vkEndCommandBuffer(cmd));
        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &cmd;
        VK_CHECK(vkQueueSubmit(context_.queue, 1, &submit, frame.fence));
        frame.submittedFrame = frameCount_ + 1;
        frame.recorded = true;
        VK_CHECK(vkWaitForFences(context_.device, 1, &frame.fence, VK_TRUE, UINT64_MAX));
    }
}

void VulkanBackend::destroyRenderResources() {
    if (context_.device == VK_NULL_HANDLE) return;
    for (auto& kv : particlePipelines_) vkDestroyPipeline(context_.device, kv.second, nullptr);
    particlePipelines_.clear();
    for (auto& kv : renderPasses_) vkDestroyRenderPass(context_.device, kv.second, nullptr);
    renderPasses_.clear();
    if (targetFramebuffer_) vkDestroyFramebuffer(context_.device, targetFramebuffer_, nullptr);
    targetFramebuffer_ = VK_NULL_HANDLE;
    currentPass_ = VK_NULL_HANDLE;
    haveTargetFramebuffer_ = false;
}
} // namespace ember::detail::vulkan
