#pragma once

#include "n64forge/core/types.hpp"
#include "n64forge/video/graphics_api.hpp"
#include <memory>
#include <span>
#include <string>

namespace n64forge::video {
// Drawable and fitted image rectangle in physical pixels.
struct PresentationGeometry {
    u32 width = 0, height = 0;
    u32 x = 0, y = 0, imageWidth = 0, imageHeight = 0;
    double devicePixelRatio = 1;
    bool operator==(const PresentationGeometry&) const = default;
};

struct FrameBacking {
    virtual ~FrameBacking() = default;
    virtual u32* data() const noexcept = 0;
};
// Shared linear-texture row alignment on the negotiated Metal path.
inline constexpr u32 PackedFrameAlignment = 256;
std::shared_ptr<FrameBacking> createPackedFrameBacking(u32 wordCapacity);

struct PackedPresentationFrame {
    std::shared_ptr<const FrameBacking> backing;
    std::shared_ptr<const void> lease; // Prevents recycling pixels while sampled by the GPU.
    u32 pitch = 0, width = 0, height = 0, bytesPerPixel = 0;
    u64 revision = 0;
};

// Host-only presentation. No native graphics types cross this boundary.
class ViewportPresenter {
public:
    virtual ~ViewportPresenter() = default;
    [[nodiscard]] virtual GraphicsApi api() const noexcept = 0;
    virtual bool attach(void* nativeView) = 0;
    virtual bool present(std::span<const u32> pixels, u32 width, u32 height,
                         u64 revision, float blend, bool smooth,
                         const PresentationGeometry& geometry) = 0;
    virtual bool presentPacked(const PackedPresentationFrame& frame, float blend, bool smooth,
                               const PresentationGeometry& geometry) = 0;
    virtual void discardFrames() = 0;
    // A drawable is still in flight; the previous image remains on screen.
    [[nodiscard]] virtual bool busy() const = 0;
    // Request another attempt even if the source revision did not change.
    [[nodiscard]] virtual bool needsRetry() const { return false; }
    // Asynchronous backends report completion, not just acceptance of a frame.
    [[nodiscard]] virtual bool hasPresentedFrame() const { return true; }
    [[nodiscard]] virtual std::string deviceDescription() const { return {}; }
    [[nodiscard]] virtual double gpuMilliseconds() const = 0;
};
std::unique_ptr<ViewportPresenter> createViewportPresenter(GraphicsApi api = GraphicsApi::Metal);
} // namespace n64forge::video
