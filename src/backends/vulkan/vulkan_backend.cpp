#include "vulkan_backend.hpp"
#include <cstdio>

namespace ember::detail::vulkan {
SpirvBlob VulkanBackend::loadShader(const char* name) const {
    // Prefer the editable build product next to the executable; fall back to
    // the blob embedded at build time when no file is present.
    if (name && *name) {
        // Only *.spv files are valid here: the directory also holds the GLSL
        // sources, which must never be fed to vkCreateShaderModule.
        std::FILE* f = std::fopen((shaderDir_ + "/" + name + ".spv").c_str(), "rb");
        if (f) {
            std::fseek(f, 0, SEEK_END);
            const long n = std::ftell(f);
            std::fseek(f, 0, SEEK_SET);
            if (n > 0) {
                // Stable storage: the returned view must survive later loads.
                shaderFiles_.emplace_back(static_cast<std::size_t>(n));
                std::vector<unsigned char>& buffer = shaderFiles_.back();
                const std::size_t read = std::fread(buffer.data(), 1, buffer.size(), f);
                std::fclose(f);
                if (read == buffer.size()) return {buffer.data(), buffer.size()};
                return {};
            }
            std::fclose(f);
        }
    }
    return embeddedSpirv(name);
}
} // namespace ember::detail::vulkan
