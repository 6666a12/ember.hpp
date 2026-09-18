#include "vulkan_backend.hpp"
#include "../opengl/params.hpp"
#include <algorithm>
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
    for (std::uint32_t i = 0; i < mem.memoryTypeCount; ++i)
        if ((typeBits & (1u << i)) && (mem.memoryTypes[i].propertyFlags & props) == props) return i;
    throw std::runtime_error("ember/vulkan: no suitable memory type");
}
VkAttachmentDescription bloomColorAttachment() {
    VkAttachmentDescription a{};
    a.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    a.samples = VK_SAMPLE_COUNT_1_BIT;
    a.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    a.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    a.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    return a;
}
VkSubpassDependency bloomDependency() {
    // These passes both write their own color target and sample a previous
    // pass's output, so the dependency must cover color-attachment writes and
    // fragment-shader sampled reads in both directions.
    VkSubpassDependency dep{};
    dep.srcSubpass = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass = 0;
    dep.srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                       VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                       VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                       VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dep.dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                       VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                       VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                       VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dep.srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dep.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    return dep;
}
} // namespace

void VulkanBackend::createBloomPipeline() {
    if (bloomCompositePipeline_) return;
    if (!bloomVertModule_) bloomVertModule_ = makeModule("bloom.vert");
    if (!bloomThresholdModule_) bloomThresholdModule_ = makeModule("bloom_threshold.frag");
    if (!bloomBlurModule_) bloomBlurModule_ = makeModule("bloom_blur.frag");
    if (!bloomCompositeModule_) bloomCompositeModule_ = makeModule("bloom_composite.frag");

    if (!bloomSetLayout_) {
        VkDescriptorSetLayoutBinding bindings[2]{};
        bindings[0].binding = 0;
        bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[0].descriptorCount = 1;
        bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        bindings[1].binding = kBindingUboBloom;
        bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        bindings[1].descriptorCount = 1;
        bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo layoutInfo{};
        layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layoutInfo.bindingCount = 2;
        layoutInfo.pBindings = bindings;
        VK_CHECK(vkCreateDescriptorSetLayout(context_.device, &layoutInfo, nullptr, &bloomSetLayout_));

        VkPipelineLayoutCreateInfo pl{};
        pl.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pl.setLayoutCount = 1;
        pl.pSetLayouts = &bloomSetLayout_;
        VK_CHECK(vkCreatePipelineLayout(context_.device, &pl, nullptr, &bloomPipelineLayout_));

        VkDescriptorPoolSize sizes[2]{};
        sizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        sizes[0].descriptorCount = 4;
        sizes[1].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        sizes[1].descriptorCount = 4;
        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.maxSets = 4;
        poolInfo.poolSizeCount = 2;
        poolInfo.pPoolSizes = sizes;
        VK_CHECK(vkCreateDescriptorPool(context_.device, &poolInfo, nullptr, &bloomPool_));
        VkDescriptorSetLayout layouts[4] = {bloomSetLayout_, bloomSetLayout_,
                                            bloomSetLayout_, bloomSetLayout_};
        VkDescriptorSetAllocateInfo alloc{};
        alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        alloc.descriptorPool = bloomPool_;
        alloc.descriptorSetCount = 4;
        alloc.pSetLayouts = layouts;
        VK_CHECK(vkAllocateDescriptorSets(context_.device, &alloc, bloomSets_));
    }

    auto makePipeline = [&](VkShaderModule frag, VkRenderPass pass, bool additive) {
        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = bloomVertModule_;
        stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = frag;
        stages[1].pName = "main";
        VkPipelineVertexInputStateCreateInfo vi{};
        vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        VkPipelineInputAssemblyStateCreateInfo ia{};
        ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo vp{};
        vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        vp.viewportCount = 1;
        vp.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo rs{};
        rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.cullMode = VK_CULL_MODE_NONE;
        rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rs.lineWidth = 1.f;
        VkPipelineMultisampleStateCreateInfo ms{};
        ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        // The composite pass may target a render pass with a depth attachment;
        // rasterization there still requires an explicit (disabled) state.
        VkPipelineDepthStencilStateCreateInfo depthStencil{};
        depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        depthStencil.depthTestEnable = VK_FALSE;
        depthStencil.depthWriteEnable = VK_FALSE;
        VkPipelineColorBlendAttachmentState blend{};
        blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                               VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        blend.blendEnable = additive ? VK_TRUE : VK_FALSE;
        blend.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        blend.dstColorBlendFactor = additive ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_ZERO;
        blend.colorBlendOp = VK_BLEND_OP_ADD;
        blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blend.dstAlphaBlendFactor = additive ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_ZERO;
        blend.alphaBlendOp = VK_BLEND_OP_ADD;
        VkPipelineColorBlendStateCreateInfo cb{};
        cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        cb.attachmentCount = 1;
        cb.pAttachments = &blend;
        const VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo ds{};
        ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        ds.dynamicStateCount = 2;
        ds.pDynamicStates = dyn;
        VkGraphicsPipelineCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        info.stageCount = 2;
        info.pStages = stages;
        info.pVertexInputState = &vi;
        info.pInputAssemblyState = &ia;
        info.pViewportState = &vp;
        info.pRasterizationState = &rs;
        info.pMultisampleState = &ms;
        info.pDepthStencilState = &depthStencil;
        info.pColorBlendState = &cb;
        info.pDynamicState = &ds;
        info.layout = bloomPipelineLayout_;
        info.renderPass = pass;
        VkPipeline pipeline = VK_NULL_HANDLE;
        VK_CHECK(vkCreateGraphicsPipelines(context_.device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline));
        return pipeline;
    };
    if (bloomThresholdPipeline_) vkDestroyPipeline(context_.device, bloomThresholdPipeline_, nullptr);
    if (bloomBlurPipeline_) vkDestroyPipeline(context_.device, bloomBlurPipeline_, nullptr);
    if (bloomCompositePipeline_) vkDestroyPipeline(context_.device, bloomCompositePipeline_, nullptr);
    bloomThresholdPipeline_ = makePipeline(bloomThresholdModule_, bloomHalf_.pass, false);
    bloomBlurPipeline_ = makePipeline(bloomBlurModule_, bloomBlur_.pass, false);
    bloomCompositePipeline_ = makePipeline(bloomCompositeModule_, currentPass_, true);
}

void VulkanBackend::destroyBloomResources() {
    if (context_.device == VK_NULL_HANDLE) return;
    if (bloomThresholdPipeline_) vkDestroyPipeline(context_.device, bloomThresholdPipeline_, nullptr);
    if (bloomBlurPipeline_) vkDestroyPipeline(context_.device, bloomBlurPipeline_, nullptr);
    if (bloomCompositePipeline_) vkDestroyPipeline(context_.device, bloomCompositePipeline_, nullptr);
    bloomThresholdPipeline_ = bloomBlurPipeline_ = bloomCompositePipeline_ = VK_NULL_HANDLE;
    if (bloomVertModule_) vkDestroyShaderModule(context_.device, bloomVertModule_, nullptr);
    if (bloomThresholdModule_) vkDestroyShaderModule(context_.device, bloomThresholdModule_, nullptr);
    if (bloomBlurModule_) vkDestroyShaderModule(context_.device, bloomBlurModule_, nullptr);
    if (bloomCompositeModule_) vkDestroyShaderModule(context_.device, bloomCompositeModule_, nullptr);
    bloomVertModule_ = bloomThresholdModule_ = bloomBlurModule_ = bloomCompositeModule_ = VK_NULL_HANDLE;
    auto destroyImage = [&](BloomImage& img) {
        if (img.framebuffer) vkDestroyFramebuffer(context_.device, img.framebuffer, nullptr);
        if (img.pass) vkDestroyRenderPass(context_.device, img.pass, nullptr);
        if (img.view) vkDestroyImageView(context_.device, img.view, nullptr);
        if (img.image) vkDestroyImage(context_.device, img.image, nullptr);
        if (img.memory) vkFreeMemory(context_.device, img.memory, nullptr);
        if (img.sampler) vkDestroySampler(context_.device, img.sampler, nullptr);
        img = BloomImage{};
    };
    // bloomFull_.pass aliases bloomScenePass_; drop the alias first so the
    // shared render pass is destroyed exactly once.
    bloomFull_.pass = VK_NULL_HANDLE;
    destroyImage(bloomFull_);
    destroyImage(bloomHalf_);
    destroyImage(bloomBlur_);
    if (bloomScenePass_) vkDestroyRenderPass(context_.device, bloomScenePass_, nullptr);
    bloomScenePass_ = VK_NULL_HANDLE;
    if (bloomPipelineLayout_) vkDestroyPipelineLayout(context_.device, bloomPipelineLayout_, nullptr);
    if (bloomSetLayout_) vkDestroyDescriptorSetLayout(context_.device, bloomSetLayout_, nullptr);
    if (bloomPool_) vkDestroyDescriptorPool(context_.device, bloomPool_, nullptr);
    bloomPipelineLayout_ = VK_NULL_HANDLE;
    bloomSetLayout_ = VK_NULL_HANDLE;
    bloomPool_ = VK_NULL_HANDLE;
    for (auto& set : bloomSets_) set = VK_NULL_HANDLE;
    bloomW_ = bloomH_ = 0;
    bloomHasDepth_ = false;
    bloomDepthFormat_ = VK_FORMAT_UNDEFINED;
}

void VulkanBackend::ensureBloom(std::uint32_t w, std::uint32_t h) {
    // Transactional like GL's ensureBloom: any failure disables bloom with a
    // warning instead of throwing out of render().
    try {
        bloomHasDepth_ = isDepthFormat(target_.depthFormat) && target_.depthView != VK_NULL_HANDLE;
        const VkFormat depthFormat = bloomHasDepth_ ? target_.depthFormat : VK_FORMAT_UNDEFINED;
        if (bloomThresholdPipeline_ && w == bloomW_ && h == bloomH_ &&
            depthFormat == bloomDepthFormat_) return;
        destroyBloomResources();
        // destroyBloomResources() clears the member; depthFormat is the source
        // of truth for this rebuild.
        bloomHasDepth_ = depthFormat != VK_FORMAT_UNDEFINED;
        bloomDepthFormat_ = depthFormat;

        const std::uint32_t hw = std::max(1u, w / 2), hh = std::max(1u, h / 2);
        auto makeImage = [&](BloomImage& img, std::uint32_t iw, std::uint32_t ih, VkFormat format,
                             VkImageUsageFlags usage) {
            VkImageCreateInfo info{};
            info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
            info.imageType = VK_IMAGE_TYPE_2D;
            info.format = format;
            info.extent = {iw, ih, 1};
            info.mipLevels = 1;
            info.arrayLayers = 1;
            info.samples = VK_SAMPLE_COUNT_1_BIT;
            info.tiling = VK_IMAGE_TILING_OPTIMAL;
            info.usage = usage;
            info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            VK_CHECK(vkCreateImage(context_.device, &info, nullptr, &img.image));
            VkMemoryRequirements req{};
            vkGetImageMemoryRequirements(context_.device, img.image, &req);
            VkMemoryAllocateInfo alloc{};
            alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            alloc.allocationSize = req.size;
            alloc.memoryTypeIndex = findMemoryType(context_.physicalDevice, req.memoryTypeBits,
                                                   VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            VK_CHECK(vkAllocateMemory(context_.device, &alloc, nullptr, &img.memory));
            VK_CHECK(vkBindImageMemory(context_.device, img.image, img.memory, 0));
            VkImageViewCreateInfo vi{};
            vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            vi.image = img.image;
            vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
            vi.format = format;
            vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            VK_CHECK(vkCreateImageView(context_.device, &vi, nullptr, &img.view));
            VkSamplerCreateInfo si{};
            si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
            si.magFilter = VK_FILTER_LINEAR;
            si.minFilter = VK_FILTER_LINEAR;
            si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
            si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            si.maxLod = 0.f;
            VK_CHECK(vkCreateSampler(context_.device, &si, nullptr, &img.sampler));
        };
        const VkImageUsageFlags bloomUsage =
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        makeImage(bloomFull_, w, h, VK_FORMAT_R16G16B16A16_SFLOAT, bloomUsage);
        makeImage(bloomHalf_, hw, hh, VK_FORMAT_R16G16B16A16_SFLOAT, bloomUsage);
        makeImage(bloomBlur_, hw, hh, VK_FORMAT_R16G16B16A16_SFLOAT, bloomUsage);

        // Half/blur targets: single RGBA16F color attachment.
        auto makeSimplePass = [&](BloomImage& img, std::uint32_t iw, std::uint32_t ih) {
            VkAttachmentDescription color = bloomColorAttachment();
            VkAttachmentReference colorRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
            VkSubpassDescription subpass{};
            subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
            subpass.colorAttachmentCount = 1;
            subpass.pColorAttachments = &colorRef;
            VkSubpassDependency dep = bloomDependency();
            VkRenderPassCreateInfo info{};
            info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
            info.attachmentCount = 1;
            info.pAttachments = &color;
            info.subpassCount = 1;
            info.pSubpasses = &subpass;
            info.dependencyCount = 1;
            info.pDependencies = &dep;
            VK_CHECK(vkCreateRenderPass(context_.device, &info, nullptr, &img.pass));
            VkFramebufferCreateInfo fi{};
            fi.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            fi.renderPass = img.pass;
            fi.attachmentCount = 1;
            fi.pAttachments = &img.view;
            fi.width = iw;
            fi.height = ih;
            fi.layers = 1;
            VK_CHECK(vkCreateFramebuffer(context_.device, &fi, nullptr, &img.framebuffer));
        };
        makeSimplePass(bloomHalf_, hw, hh);
        makeSimplePass(bloomBlur_, hw, hh);

        // Full-res particle pass (HDR color + optional host depth attachment).
        // The depth attachment LOADs the injected host depth so the HDR pass
        // occludes exactly like the host pass.
        VkAttachmentDescription attachments[2]{};
        attachments[0] = bloomColorAttachment();
        attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        attachments[1].format = depthFormat;
        attachments[1].samples = VK_SAMPLE_COUNT_1_BIT;
        attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachments[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachments[1].initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        attachments[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        VkAttachmentReference colorRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkAttachmentReference depthRef{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &colorRef;
        if (bloomHasDepth_) subpass.pDepthStencilAttachment = &depthRef;
        VkSubpassDependency dep = bloomDependency();
        VkRenderPassCreateInfo pi{};
        pi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        pi.attachmentCount = bloomHasDepth_ ? 2u : 1u;
        pi.pAttachments = attachments;
        pi.subpassCount = 1;
        pi.pSubpasses = &subpass;
        pi.dependencyCount = 1;
        pi.pDependencies = &dep;
        VK_CHECK(vkCreateRenderPass(context_.device, &pi, nullptr, &bloomScenePass_));
        bloomFull_.pass = bloomScenePass_;
        VkImageView views[2] = {bloomFull_.view, bloomHasDepth_ ? target_.depthView : VK_NULL_HANDLE};
        VkFramebufferCreateInfo fi{};
        fi.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fi.renderPass = bloomScenePass_;
        fi.attachmentCount = bloomHasDepth_ ? 2u : 1u;
        fi.pAttachments = views;
        fi.width = w;
        fi.height = h;
        fi.layers = 1;
        VK_CHECK(vkCreateFramebuffer(context_.device, &fi, nullptr, &bloomFull_.framebuffer));

        // Pipelines depend on the freshly created passes (and the host pass).
        createBloomPipeline();
        bloomW_ = w;
        bloomH_ = h;
    } catch (const std::exception& e) {
        destroyBloomResources();
        bloom_ = false;
        std::fprintf(stderr, "[ember] bloom disabled: %s\n", e.what());
    }
}

// Writes the four bloom pass descriptor sets (sources: full -> half -> blur ->
// half). Called from render() BEFORE command recording begins: descriptor
// updates during recording invalidate the command buffer, and reusing one set
// across passes would make every pass sample the last-written image.
void VulkanBackend::updateBloomSets(std::uint32_t slot) {
    FrameResources& frame = frames_[slot];
    const BloomImage* sources[4] = {&bloomFull_, &bloomHalf_, &bloomBlur_, &bloomHalf_};
    for (int i = 0; i < 4; ++i) {
        VkDescriptorImageInfo info{};
        info.sampler = sources[i]->sampler;
        info.imageView = sources[i]->view;
        info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkDescriptorBufferInfo ubo{};
        ubo.buffer = frame.uboBloom.buffer;
        ubo.offset = 0;
        ubo.range = frame.uboBloom.size;
        VkWriteDescriptorSet writes[2]{};
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = bloomSets_[i];
        writes[0].dstBinding = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[0].pImageInfo = &info;
        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = bloomSets_[i];
        writes[1].dstBinding = kBindingUboBloom;
        writes[1].descriptorCount = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[1].pBufferInfo = &ubo;
        vkUpdateDescriptorSets(context_.device, 2, writes, 0, nullptr);
    }
}

// Records the three bloom passes: threshold/downsample into bloomHalf_, two
// separable blur passes, then an additive composite onto the host target.
void VulkanBackend::runBloom(VkCommandBuffer cmd, float threshold) {
    const std::uint32_t slot = target_.frameIndex % kFramesInFlight;
    FrameResources& frame = frames_[slot];
    const std::uint32_t w = bloomW_, h = bloomH_;
    const std::uint32_t hw = std::max(1u, w / 2), hh = std::max(1u, h / 2);

    auto transition = [&](VkImage image, VkAccessFlags src, VkAccessFlags dst,
                          VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage) {
        VkImageMemoryBarrier b{};
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = image;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        b.srcAccessMask = src;
        b.dstAccessMask = dst;
        vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &b);
    };
    auto beginPass = [&](const BloomImage& target, std::uint32_t pw, std::uint32_t ph,
                         std::uint32_t frameSlot) {
        VkRenderPassBeginInfo passInfo{};
        passInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        passInfo.renderPass = target.pass;
        passInfo.framebuffer = target.framebuffer;
        passInfo.renderArea.extent = {pw, ph};
        VkClearValue clear{};
        clear.color = {{0.f, 0.f, 0.f, 0.f}};
        passInfo.clearValueCount = 1;
        passInfo.pClearValues = &clear;
        vkCmdBeginRenderPass(cmd, &passInfo, VK_SUBPASS_CONTENTS_INLINE);
        VkViewport viewport{};
        viewport.x = 0.f;
        viewport.y = static_cast<float>(ph);
        viewport.width = static_cast<float>(pw);
        viewport.height = -static_cast<float>(ph);
        viewport.minDepth = 0.f;
        viewport.maxDepth = 1.f;
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        VkRect2D scissor{{0, 0}, {pw, ph}};
        vkCmdSetScissor(cmd, 0, 1, &scissor);
        (void)frameSlot;
    };
    auto draw = [&](VkPipeline pipeline, std::uint32_t setIndex) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, bloomPipelineLayout_,
                                0, 1, &bloomSets_[setIndex], 0, nullptr);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRenderPass(cmd);
    };
    auto uploadParams = [&](float texelX, float texelY, float thr, float dx, float dy) {
        opengl::BloomParams bp{};
        bp.texelX = texelX; bp.texelY = texelY; bp.threshold = thr;
        bp.dirX = dx; bp.dirY = dy;
        vkCmdUpdateBuffer(cmd, frame.uboBloom.buffer, 0, sizeof(bp), &bp);
        VkMemoryBarrier b{};
        b.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 1, &b, 0, nullptr, 0, nullptr);
    };

    // The particle pass left bloomFull_ shader-readable; make the color writes
    // visible to the threshold pass's sampled read.
    transition(bloomFull_.image, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
               VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

    // 1) threshold + 2x2 downsample: bloomFull_ -> bloomHalf_.
    uploadParams(1.f / w, 1.f / h, threshold, 0.f, 0.f);
    beginPass(bloomHalf_, hw, hh, slot);
    draw(bloomThresholdPipeline_, 0);

    // 2) separable blur: bloomHalf_ -> bloomBlur_ (H), bloomBlur_ -> bloomHalf_ (V).
    transition(bloomHalf_.image, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
               VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    uploadParams(1.f / hw, 1.f / hh, threshold, 1.f, 0.f);
    beginPass(bloomBlur_, hw, hh, slot);
    draw(bloomBlurPipeline_, 1);

    transition(bloomBlur_.image, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
               VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    uploadParams(1.f / hw, 1.f / hh, threshold, 0.f, 1.f);
    beginPass(bloomHalf_, hw, hh, slot);
    draw(bloomBlurPipeline_, 2);

    // 3) additive composite onto the host target (LOAD, blend ONE,ONE).
    transition(bloomHalf_.image, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
               VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    VkRenderPassBeginInfo passInfo{};
    passInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    passInfo.renderPass = currentPass_;
    passInfo.framebuffer = targetFramebuffer_;
    passInfo.renderArea.extent = {w, h};
    vkCmdBeginRenderPass(cmd, &passInfo, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport viewport{};
    viewport.x = 0.f;
    viewport.y = static_cast<float>(h);
    viewport.width = static_cast<float>(w);
    viewport.height = -static_cast<float>(h);
    viewport.minDepth = 0.f;
    viewport.maxDepth = 1.f;
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    VkRect2D scissor{{0, 0}, {w, h}};
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    draw(bloomCompositePipeline_, 3);
}

} // namespace ember::detail::vulkan
