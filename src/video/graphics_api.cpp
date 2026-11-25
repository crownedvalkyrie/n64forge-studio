#include "n64forge/video/graphics_api.hpp"

namespace n64forge::video {

GraphicsApi resolveGraphicsApi(GraphicsApi requested,
                               const GraphicsBackendAvailability& available) noexcept {
    switch (requested) {
        case GraphicsApi::Software:
            return GraphicsApi::Software;

        case GraphicsApi::Auto:
#if defined(_WIN32)
            if (available.d3d12) return GraphicsApi::D3D12;
            if (available.win32Vulkan) return GraphicsApi::Vulkan;
#else
            if (available.metal) return GraphicsApi::Metal;
            if (available.moltenVulkan) return GraphicsApi::MoltenVK;
            if (available.kosmicVulkan) return GraphicsApi::KosmicKrisp;
#endif
            return GraphicsApi::Software;

        case GraphicsApi::Metal:
            return available.metal ? GraphicsApi::Metal : GraphicsApi::Software;
        case GraphicsApi::MoltenVK:
            return available.moltenVulkan ? GraphicsApi::MoltenVK : GraphicsApi::Software;
        case GraphicsApi::KosmicKrisp:
            return available.kosmicVulkan ? GraphicsApi::KosmicKrisp : GraphicsApi::Software;
        case GraphicsApi::D3D12:
            return available.d3d12 ? GraphicsApi::D3D12 : GraphicsApi::Software;
        case GraphicsApi::Vulkan:
            return available.win32Vulkan ? GraphicsApi::Vulkan : GraphicsApi::Software;
    }
    return GraphicsApi::Software;
}

} // namespace n64forge::video
