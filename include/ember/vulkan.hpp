#pragma once
// Vulkan-specific creation and native-context adaptation.
//
// Like ember/opengl.hpp this header pulls in its native API (<vulkan/vulkan.h>),
// so it is only for translation units that select or customize the Vulkan
// backend. The backend-independent facade (ember/system.hpp, ember/backend.hpp)
// stays free of Vulkan types. Supplies a borrowed device environment and owns
// a self-contained device helper for tests.
#include "ember/backend.hpp"
#include <cstdint>
#include <utility>
#include <vulkan/vulkan.h>

namespace ember {

// Borrowed Vulkan environment. The host owns and must outlive the backend;
// ember never creates or destroys instance/device/queue. One queue family with
// graphics + compute in the same family is required (see makeVulkanDevice).
struct VulkanContext {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    std::uint32_t queueFamilyIndex = 0;
    VkQueue queue = VK_NULL_HANDLE; // graphics + compute (same family)
};

// Create the Vulkan backend. A device must be injected with setVulkanContext()
// before the backend does any GPU work (WO-01 only needs the context set).
std::unique_ptr<ParticleBackend> makeVulkanBackend();
// Reject non-Vulkan backends before changing any state (see requireOpenGL).
void setVulkanContext(ParticleBackend&, const VulkanContext&);

// Per-frame render target (borrowed, never owned). Layout contract: on entry
// colorView is COLOR_ATTACHMENT_OPTIMAL and depthView (when used by soft
// particles/refraction) is SHADER_READ_ONLY_OPTIMAL; the backend issues no
// color/depth layout barriers and leaves both layouts unchanged on return.
// Exactly one target is valid at a time; calling again overwrites it. The host
// owns synchronization and must keep the views alive while in use.
//
// Projection/depth convention: render() takes the SAME GL-style projection
// matrix the OpenGL backend accepts; the particle vertex shader remaps clip
// z into Vulkan's [0,w] volume (see EMBER_CLIP_VULKAN in shaders/). A host
// rendering its own scene with a Vulkan-style (zero-to-one) projection gets
// numerically identical depth values, so host depth textures can be sampled
// for soft particles/refraction without conversion. The viewport uses a
// negative height, keeping the GL bottom-left origin for gl_FragCoord/UV.
struct VulkanFrameTarget {
    VkImageView colorView = VK_NULL_HANDLE; // COLOR_ATTACHMENT_OPTIMAL
    VkImageView depthView = VK_NULL_HANDLE; // optional; DEPTH_STENCIL_ATTACHMENT_OPTIMAL
    VkFormat colorFormat = VK_FORMAT_UNDEFINED;
    VkFormat depthFormat = VK_FORMAT_UNDEFINED;
    std::uint32_t width = 0, height = 0;
    std::uint32_t frameIndex = 0;
};
void setVulkanFrameTarget(ParticleBackend&, const VulkanFrameTarget&);

// ---- host command-buffer recording (WO-10) --------------------------------
// beginVulkanFrame opens a window in which update()/render() record their GPU
// commands into the HOST command buffer instead of the backend's own;
// endVulkanFrame closes it. Contract:
//  * Inside the window the backend never resets/begins/ends/submits its own
//    command buffer, never signals a fence, and never rotates its frame ring.
//  * The host must keep the kFramesInFlight=2 resource discipline: while frame
//    N's command buffer is in flight, do not record frame N+2 into the same
//    slot (spawn staging and per-frame UBOs are reused).
//  * frameIndex selects the resource slot (frameIndex % 2); render()'s
//    VulkanFrameTarget::frameIndex must resolve to that same slot.
//  * The host also owns layout transitions exactly as in default mode; update()
//    and render() must be called without an active render pass in cmd.
//  * Exact statistics/readback require the host to submit and wait cmd before
//    calling synchronizeStatistics()/aliveCount()/readParticles() outside the
//    window; those entries throw std::logic_error while the window is open.
//  * Illegal sequencing throws std::logic_error: end without begin, nested
//    begin, null cmd, or the disallowed entries listed above.
void beginVulkanFrame(ParticleBackend&, VkCommandBuffer cmd, std::uint32_t frameIndex);
void endVulkanFrame(ParticleBackend&);

// Borrowed host depth for soft particles (fragment fade) and refraction mode 1
// (depth-aware sampling). The image view must be SHADER_READ_ONLY_OPTIMAL and
// the sampler valid for the whole draw; ember never destroys either.
// Limitation vs the GL backend: one shared depth binding serves both features,
// so when both are injected the views must reference the same depth image.
void setVulkanSoftDepth(ParticleBackend&, VkImageView depth, VkSampler sampler);
void setVulkanRefractionInputs(ParticleBackend&, VkImageView color, VkImageView depth,
                               VkSampler sampler);

// Self-contained instance + device for tests and the standalone helper.
// Unlike VulkanContext this structure owns its handles and destroys
// device/instance on destruction. Debug enables validation when the layer is
// installed; a missing layer or extension degrades silently.
struct VulkanDevice {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    std::uint32_t queueFamilyIndex = 0;
    VkQueue queue = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT debugMessenger = VK_NULL_HANDLE; // owned when debug
    VulkanDevice() = default;
    ~VulkanDevice();
    VulkanDevice(const VulkanDevice&) = delete;
    VulkanDevice& operator=(const VulkanDevice&) = delete;
    VulkanDevice(VulkanDevice&& other) noexcept { *this = std::move(other); }
    VulkanDevice& operator=(VulkanDevice&& other) noexcept {
        std::swap(instance, other.instance);
        std::swap(physicalDevice, other.physicalDevice);
        std::swap(device, other.device);
        std::swap(queueFamilyIndex, other.queueFamilyIndex);
        std::swap(queue, other.queue);
        std::swap(debugMessenger, other.debugMessenger);
        return *this;
    }
    VulkanContext context() const {
        return {instance, physicalDevice, device, queueFamilyIndex, queue};
    }
};
// Throws std::runtime_error when no usable device/driver is available. The
// Vulkan baseline is 1.1; 1.0-only devices are rejected.
VulkanDevice makeVulkanDevice(bool debug = false);
} // namespace ember
