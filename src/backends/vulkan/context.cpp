#include "vulkan_backend.hpp"
#include <cstring>
#include <vector>

namespace ember::detail::vulkan {
namespace {
VulkanBackend& requireVulkan(ParticleBackend& backend) {
    auto* vk = dynamic_cast<VulkanBackend*>(&backend);
    if (!vk) throw std::invalid_argument("ember: this operation requires the Vulkan backend");
    return *vk;
}

VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT,
    const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
        std::fprintf(stderr, "ember/vulkan validation: %s\n", data->pMessage);
    return VK_FALSE;
}

PFN_vkCreateDebugUtilsMessengerEXT createMessengerFn(VkInstance instance) {
    return reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
        vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT"));
}
PFN_vkDestroyDebugUtilsMessengerEXT destroyMessengerFn(VkInstance instance) {
    return reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
        vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT"));
}
bool hasLayer(const char* name) {
    std::uint32_t count = 0;
    if (vkEnumerateInstanceLayerProperties(&count, nullptr) != VK_SUCCESS) return false;
    std::vector<VkLayerProperties> layers(count);
    if (vkEnumerateInstanceLayerProperties(&count, layers.data()) != VK_SUCCESS) return false;
    for (const auto& layer : layers)
        if (std::strcmp(layer.layerName, name) == 0) return true;
    return false;
}
bool hasInstanceExtension(const char* name) {
    std::uint32_t count = 0;
    if (vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr) != VK_SUCCESS) return false;
    std::vector<VkExtensionProperties> exts(count);
    if (vkEnumerateInstanceExtensionProperties(nullptr, &count, exts.data()) != VK_SUCCESS) return false;
    for (const auto& ext : exts)
        if (std::strcmp(ext.extensionName, name) == 0) return true;
    return false;
}
// A queue family that supports both graphics and compute, as required by the
// single-queue Vulkan baseline.
bool findQueueFamily(VkPhysicalDevice device, std::uint32_t& index) {
    std::uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, families.data());
    const VkQueueFlags need = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
    for (std::uint32_t i = 0; i < count; ++i)
        if ((families[i].queueFlags & need) == need) { index = i; return true; }
    return false;
}
} // namespace
} // namespace ember::detail::vulkan

namespace ember {
std::unique_ptr<ParticleBackend> makeVulkanBackend() {
    return std::make_unique<detail::vulkan::VulkanBackend>();
}
void setVulkanContext(ParticleBackend& backend, const VulkanContext& context) {
    auto& vk = detail::vulkan::requireVulkan(backend);
    if (!context.instance || !context.physicalDevice || !context.device || !context.queue)
        throw std::invalid_argument("ember: Vulkan context is incomplete");
    vk.setContext(context);
}
void setVulkanFrameTarget(ParticleBackend& backend, const VulkanFrameTarget& target) {
    auto& vk = detail::vulkan::requireVulkan(backend);
    if (!target.colorView || target.width == 0 || target.height == 0 ||
        target.colorFormat == VK_FORMAT_UNDEFINED)
        throw std::invalid_argument("ember: Vulkan frame target is incomplete");
    vk.setFrameTarget(target);
}
void beginVulkanFrame(ParticleBackend& backend, VkCommandBuffer cmd, std::uint32_t frameIndex) {
    detail::vulkan::requireVulkan(backend).beginFrame(cmd, frameIndex);
}
void endVulkanFrame(ParticleBackend& backend) {
    detail::vulkan::requireVulkan(backend).endFrame();
}
void setVulkanSoftDepth(ParticleBackend& backend, VkImageView depth, VkSampler sampler) {
    detail::vulkan::requireVulkan(backend).setSoftDepth(depth, sampler);
}
void setVulkanRefractionInputs(ParticleBackend& backend, VkImageView color, VkImageView depth,
                               VkSampler sampler) {
    detail::vulkan::requireVulkan(backend).setRefractionInputs(color, depth, sampler);
}

VulkanDevice::~VulkanDevice() {
    if (device) vkDeviceWaitIdle(device);
    if (device) vkDestroyDevice(device, nullptr);
    if (debugMessenger && instance) {
        if (auto destroy = detail::vulkan::destroyMessengerFn(instance))
            destroy(instance, debugMessenger, nullptr);
    }
    if (instance) vkDestroyInstance(instance, nullptr);
}

VulkanDevice makeVulkanDevice(bool debug) {
    VulkanDevice out;

    // Validation is best-effort: a missing layer or instance extension must
    // not break device creation.
    const bool wantValidation =
        debug && detail::vulkan::hasLayer("VK_LAYER_KHRONOS_validation") &&
        detail::vulkan::hasInstanceExtension(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);

    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "ember";
    app.apiVersion = VK_API_VERSION_1_1;

    std::vector<const char*> layers;
    std::vector<const char*> extensions;
    if (wantValidation) {
        layers.push_back("VK_LAYER_KHRONOS_validation");
        extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    }
    VkInstanceCreateInfo instanceInfo{};
    instanceInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instanceInfo.pApplicationInfo = &app;
    instanceInfo.enabledLayerCount = static_cast<std::uint32_t>(layers.size());
    instanceInfo.ppEnabledLayerNames = layers.empty() ? nullptr : layers.data();
    instanceInfo.enabledExtensionCount = static_cast<std::uint32_t>(extensions.size());
    instanceInfo.ppEnabledExtensionNames = extensions.empty() ? nullptr : extensions.data();
    VK_CHECK(vkCreateInstance(&instanceInfo, nullptr, &out.instance));

    if (wantValidation) {
        VkDebugUtilsMessengerCreateInfoEXT messengerInfo{};
        messengerInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
        messengerInfo.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                        VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        messengerInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                                    VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                                    VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        messengerInfo.pfnUserCallback = detail::vulkan::debugCallback;
        if (auto create = detail::vulkan::createMessengerFn(out.instance))
            create(out.instance, &messengerInfo, nullptr, &out.debugMessenger);
    }

    std::uint32_t deviceCount = 0;
    if (vkEnumeratePhysicalDevices(out.instance, &deviceCount, nullptr) != VK_SUCCESS || deviceCount == 0)
        throw std::runtime_error("ember: no Vulkan physical device available");
    std::vector<VkPhysicalDevice> devices(deviceCount);
    VK_CHECK(vkEnumeratePhysicalDevices(out.instance, &deviceCount, devices.data()));

    std::uint32_t family = 0;
    for (VkPhysicalDevice candidate : devices) {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(candidate, &props);
        // The baseline is 1.1 (negative viewport height); reject 1.0-only.
        if (props.apiVersion < VK_API_VERSION_1_1) continue;
        if (!detail::vulkan::findQueueFamily(candidate, family)) continue;
        out.physicalDevice = candidate;
        break;
    }
    if (!out.physicalDevice)
        throw std::runtime_error("ember: no Vulkan 1.1 device with a graphics+compute queue family");

    const float priority = 1.f;
    VkDeviceQueueCreateInfo queueInfo{};
    queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueInfo.queueFamilyIndex = family;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &priority;
    VkDeviceCreateInfo deviceInfo{};
    deviceInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    deviceInfo.queueCreateInfoCount = 1;
    deviceInfo.pQueueCreateInfos = &queueInfo;
    VK_CHECK(vkCreateDevice(out.physicalDevice, &deviceInfo, nullptr, &out.device));

    out.queueFamilyIndex = family;
    vkGetDeviceQueue(out.device, family, 0, &out.queue);
    return out;
}
} // namespace ember
