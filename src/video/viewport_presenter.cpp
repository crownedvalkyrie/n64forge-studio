#include "n64forge/video/viewport_presenter.hpp"
#include "n64forge/video/graphics_api.hpp"

#include <algorithm>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#ifdef __APPLE__
#include <cstdlib>
#include <filesystem>
#endif

namespace n64forge::video {
#ifdef N64FORGE_HAS_VULKAN_VIEWPORT
std::unique_ptr<ViewportPresenter> createVulkanViewport(GraphicsApi api);
bool vulkanDriverDetected(GraphicsApi api);
#endif
#ifdef N64FORGE_HAS_WIN32_VULKAN
std::unique_ptr<ViewportPresenter> createWin32VulkanViewport();
bool win32VulkanDriverDetected(std::string* detail);
#endif
#ifdef N64FORGE_HAS_D3D12
std::unique_ptr<ViewportPresenter> createD3D12Viewport();
bool d3d12DeviceAvailable(std::string* description);
#endif
#ifdef N64FORGE_HAS_METAL
std::unique_ptr<ViewportPresenter> createMetalViewport();
std::shared_ptr<FrameBacking> createMetalPackedFrameBacking(u32 wordCapacity);
std::string metalDeviceName();
#endif

namespace {

#ifdef __APPLE__
bool appleDriverPresent(GraphicsApi requested) {
    namespace fs = std::filesystem;
    const auto exists = [](const fs::path& file) {
        std::error_code error;
        return fs::is_regular_file(file, error);
    };
    if (requested == GraphicsApi::MoltenVK) {
        for (const auto* dir : {"/opt/homebrew/lib", "/usr/local/lib", "/opt/local/lib"})
            if (exists(fs::path(dir) / "libMoltenVK.dylib")) return true;
        if (const auto* sdk = std::getenv("VULKAN_SDK"))
            if (exists(fs::path(sdk) / "lib" / "libMoltenVK.dylib")) return true;
    }
    const auto matches = [&](const fs::path& file) {
        auto name = file.filename().string();
        for (auto& c : name) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
        const auto key = requested == GraphicsApi::MoltenVK ? "moltenvk" : "kosmickrisp";
        return name.ends_with(".json") && name.find(key) != name.npos && exists(file);
    };
    for (const auto* variable : {"VK_DRIVER_FILES", "VK_ICD_FILENAMES", "VK_ADD_DRIVER_FILES"}) {
        if (const auto* value = std::getenv(variable)) {
            std::string_view list(value);
            while (!list.empty()) {
                const auto end = list.find(':');
                if (matches(fs::path(list.substr(0, end)))) return true;
                if (end == list.npos) break;
                list.remove_prefix(end + 1);
            }
        }
    }
    for (const auto* dir : {"/opt/homebrew/share/vulkan/icd.d", "/usr/local/share/vulkan/icd.d",
                            "/opt/local/share/vulkan/icd.d", "/etc/vulkan/icd.d"}) {
        std::error_code error;
        for (fs::directory_iterator it(dir, error), end; !error && it != end; it.increment(error))
            if (matches(it->path())) return true;
    }
    return false;
}
#endif

GraphicsBackendAvailability detectGraphicsBackends() {
    GraphicsBackendAvailability available;
#ifdef N64FORGE_HAS_METAL
    available.metal = !metalDeviceName().empty();
#endif
#ifdef N64FORGE_HAS_VULKAN_VIEWPORT
    available.moltenVulkan = vulkanDriverDetected(GraphicsApi::MoltenVK);
    available.kosmicVulkan = vulkanDriverDetected(GraphicsApi::KosmicKrisp);
#endif
#ifdef N64FORGE_HAS_D3D12
    available.d3d12 = d3d12DeviceAvailable(nullptr);
#endif
#ifdef N64FORGE_HAS_WIN32_VULKAN
    available.win32Vulkan = win32VulkanDriverDetected(nullptr);
#endif
#if defined(__APPLE__) && !defined(N64FORGE_HAS_VULKAN_VIEWPORT)
    available.moltenVulkan = appleDriverPresent(GraphicsApi::MoltenVK);
    available.kosmicVulkan = appleDriverPresent(GraphicsApi::KosmicKrisp);
#endif
    return available;
}

// Tries each native candidate in preference order and exposes whichever one
// actually attached. If none attach, the caller keeps the QPainter software
// path and therefore still reports SOFTWARE in Studio.
class FallbackViewportPresenter final : public ViewportPresenter {
public:
    explicit FallbackViewportPresenter(std::vector<std::unique_ptr<ViewportPresenter>> candidates)
        : candidates_(std::move(candidates)) {}

    GraphicsApi api() const noexcept override { return active_ ? active_->api() : GraphicsApi::Software; }

    bool attach(void* nativeView) override {
        for (auto& candidate : candidates_) {
            if (candidate && candidate->attach(nativeView)) {
                active_ = candidate.get();
                return true;
            }
        }
        return false;
    }

    bool present(std::span<const u32> pixels, u32 width, u32 height, u64 revision,
                 float blend, bool smooth, const PresentationGeometry& geometry) override {
        return active_ && active_->present(pixels, width, height, revision, blend, smooth, geometry);
    }

    bool presentPacked(const PackedPresentationFrame& frame, float blend, bool smooth,
                       const PresentationGeometry& geometry) override {
        return active_ && active_->presentPacked(frame, blend, smooth, geometry);
    }

    void discardFrames() override { if (active_) active_->discardFrames(); }
    bool busy() const override { return active_ && active_->busy(); }
    bool needsRetry() const override { return active_ && active_->needsRetry(); }
    bool hasPresentedFrame() const override { return active_ && active_->hasPresentedFrame(); }
    std::string deviceDescription() const override {
        return active_ ? active_->deviceDescription() : std::string{};
    }
    double gpuMilliseconds() const override { return active_ ? active_->gpuMilliseconds() : 0.0; }

private:
    std::vector<std::unique_ptr<ViewportPresenter>> candidates_;
    ViewportPresenter* active_ = nullptr;
};

#ifndef N64FORGE_HAS_METAL
class HostPackedFrameBacking final : public FrameBacking {
public:
    explicit HostPackedFrameBacking(u32 wordCapacity) : words_(wordCapacity) {}
    u32* data() const noexcept override {
        return const_cast<u32*>(words_.data()); // FrameBacking owns mutable host pixels.
    }
private:
    std::vector<u32> words_;
};
#endif

} // namespace

GraphicsProbe probeGraphicsApi(GraphicsApi requested) {
    using enum GraphicsApi;
    if (requested == Software) return {Software, true, "CPU presentation", Software};
    const auto available = detectGraphicsBackends();
    const auto active = resolveGraphicsApi(requested, available);
    std::string detail;
    if (active == D3D12) {
#ifdef N64FORGE_HAS_D3D12
        std::string device;
        if (d3d12DeviceAvailable(&device)) detail = "D3D12 hardware device: " + device;
#endif
    } else if (active == Vulkan) {
#ifdef N64FORGE_HAS_WIN32_VULKAN
        std::string driver;
        if (win32VulkanDriverDetected(&driver)) detail = std::move(driver);
#endif
        if (detail.empty()) detail = "Vulkan driver loaded from vulkan-1.dll";
    } else if (active == Metal) {
#ifdef N64FORGE_HAS_METAL
        detail = metalDeviceName();
#endif
        if (detail.empty()) detail = "no supported Metal device";
    } else if (active == MoltenVK || active == KosmicKrisp) {
        detail = "driver located; requires a compatible native surface";
    } else {
        detail = "no supported graphics device; CPU presentation";
    }
    if (requested == Auto && active != Software)
        detail = "Auto: " + std::string(graphicsApiName(active)) + "; " + detail;
    return {active, active != Software, std::move(detail), active};
}

std::unique_ptr<ViewportPresenter> createViewportPresenter([[maybe_unused]] GraphicsApi api) {
    std::vector<std::unique_ptr<ViewportPresenter>> candidates;
    const auto add = [&](std::unique_ptr<ViewportPresenter> presenter) {
        if (presenter) candidates.push_back(std::move(presenter));
    };
    const auto available = detectGraphicsBackends();
    const auto selected = resolveGraphicsApi(api, available);

#if defined(_WIN32)
    // Auto prefers D3D12, then Vulkan. Explicit selections never silently
    // switch to another hardware API; they fall back to QPainter Software.
    if (api == GraphicsApi::Auto) {
#ifdef N64FORGE_HAS_D3D12
        if (available.d3d12) add(createD3D12Viewport());
#endif
#ifdef N64FORGE_HAS_WIN32_VULKAN
        if (available.win32Vulkan) add(createWin32VulkanViewport());
#endif
    } else if (selected == GraphicsApi::D3D12) {
#ifdef N64FORGE_HAS_D3D12
        add(createD3D12Viewport());
#endif
    } else if (selected == GraphicsApi::Vulkan) {
#ifdef N64FORGE_HAS_WIN32_VULKAN
        add(createWin32VulkanViewport());
#endif
    }
#else
    if (api == GraphicsApi::Auto) {
#ifdef N64FORGE_HAS_METAL
        if (available.metal) add(createMetalViewport());
#endif
#ifdef N64FORGE_HAS_VULKAN_VIEWPORT
        if (available.moltenVulkan) add(createVulkanViewport(GraphicsApi::MoltenVK));
        if (available.kosmicVulkan) add(createVulkanViewport(GraphicsApi::KosmicKrisp));
#endif
    } else if (selected == GraphicsApi::Metal) {
#ifdef N64FORGE_HAS_METAL
        add(createMetalViewport());
#endif
    } else if (selected == GraphicsApi::MoltenVK || selected == GraphicsApi::KosmicKrisp) {
#ifdef N64FORGE_HAS_VULKAN_VIEWPORT
        add(createVulkanViewport(selected));
#endif
    }
#endif
    if (candidates.empty()) return {};
    if (candidates.size() == 1) return std::move(candidates.front());
    return std::make_unique<FallbackViewportPresenter>(std::move(candidates));
}

std::shared_ptr<FrameBacking> createPackedFrameBacking([[maybe_unused]] u32 wordCapacity) {
#ifdef N64FORGE_HAS_METAL
    return createMetalPackedFrameBacking(wordCapacity);
#else
    return std::make_shared<HostPackedFrameBacking>(wordCapacity);
#endif
}
} // namespace n64forge::video
