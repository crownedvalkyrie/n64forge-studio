#pragma once

#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>

namespace n64forge::video {

// Persisted numbers: append new values, do not renumber existing ones.
// Metal/MoltenVK/KosmicKrisp are macOS presenters; D3D12/Vulkan Windows
// presenters use the same single Graphics API setting.
enum class GraphicsApi {
    Metal = 0,
    MoltenVK = 1,
    KosmicKrisp = 2,
    Software = 3,
    Auto = 4,
    D3D12 = 5,
    Vulkan = 6,
};

[[nodiscard]] constexpr std::string_view graphicsApiName(GraphicsApi api) noexcept {
    switch (api) {
        case GraphicsApi::Metal: return "metal";
        case GraphicsApi::MoltenVK: return "moltenvk";
        case GraphicsApi::KosmicKrisp: return "kosmickrisp";
        case GraphicsApi::Software: return "software";
        case GraphicsApi::Auto: return "auto";
        case GraphicsApi::D3D12: return "d3d12";
        case GraphicsApi::Vulkan: return "vulkan";
    }
    return "software";
}

[[nodiscard]] constexpr std::optional<GraphicsApi> parseGraphicsApi(std::string_view name) noexcept {
    for (auto api : {GraphicsApi::Metal, GraphicsApi::MoltenVK, GraphicsApi::KosmicKrisp,
                     GraphicsApi::Software, GraphicsApi::Auto, GraphicsApi::D3D12,
                     GraphicsApi::Vulkan})
        if (name == graphicsApiName(api)) return api;
    return std::nullopt;
}

// One persisted setting per platform. macOS keeps its historical default;
// Windows defaults to Auto and probes D3D12, then Vulkan, then Software.
[[nodiscard]] constexpr GraphicsApi defaultGraphicsApi() noexcept {
#if defined(_WIN32)
    return GraphicsApi::Auto;
#else
    return GraphicsApi::Metal;
#endif
}

// Deterministic inputs to the fallback resolver. Runtime probes fill these in;
// unit tests supply exact values so no host GPU affects the expected result.
struct GraphicsBackendAvailability {
    bool metal = false;
    bool moltenVulkan = false;
    bool kosmicVulkan = false;
    bool d3d12 = false;
    bool win32Vulkan = false;
};

// The single source of truth for fallback selection. Explicit selections fall
// back only to Software; Auto walks the platform's hardware preference order.
[[nodiscard]] GraphicsApi resolveGraphicsApi(
    GraphicsApi requested, const GraphicsBackendAvailability& available) noexcept;

struct GraphicsProbe {
    GraphicsApi fallback;
    bool driverDetected;
    std::string detail;
    // Effective backend after fallback resolution; presenters may report a
    // different one later if native swapchain attachment fails.
    GraphicsApi active = fallback;
};

// Reports runtime driver discovery separately from whether Studio has a
// presenter for that driver. A native backend still has to attach to a view.
[[nodiscard]] GraphicsProbe probeGraphicsApi(GraphicsApi requested);

} // namespace n64forge::video
