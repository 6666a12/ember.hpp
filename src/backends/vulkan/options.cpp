#include "vulkan_backend.hpp"

namespace ember::detail::vulkan {

// The Vulkan backend implements every optional capability (WO-06): simulation,
// rendering, GPU scheduling, sorting, sprite PNGs, soft particles, refraction
// and bloom. Device/shader limits can still make an option fail at enable time.
BackendCapabilities VulkanBackend::capabilities() const noexcept {
    BackendCapabilities caps{};
    caps.spriteTextures = true;
    caps.gpuScheduling = true;
    caps.sorting = true;
    caps.bloom = true;
    caps.refraction = true;
    caps.softParticles = true;
    caps.events = true;
    caps.lifeCurves = true;
    return caps;
}

} // namespace ember::detail::vulkan
