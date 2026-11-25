#include "n64forge/video/renderer.hpp"
#include "n64forge/video/d3d12_frame_interpolator.hpp"
#include "n64forge/emulation/frame_ring_buffer.hpp"
#include <d3d12.h>
#include <dxgi1_6.h>
#include <memory>

namespace {

class D3D12Renderer final : public n64forge::Renderer {
public:
    D3D12Renderer() {
        frameRing_ = std::make_unique<n64forge::emulation::FrameRingBuffer>();
        interpolator_ = std::make_unique<n64forge::video::d3d12::FrameInterpolator>(*frameRing_);
    }

    bool initialize(void* nativeWindow, const n64forge::RendererOptions& options) override {
        (void)nativeWindow;
        (void)options;
        // TODO: create DXGI factory, adapter, device, command queue, swap chain and PSOs.
        // The interpolation pipeline is already wired; this scaffold returns false
        // because no real D3D12 device is created yet.
        return false;
    }

    void present() override {
        if (!interpolator_) return;

        // Advance the interpolation timeline and retrieve the interpolated MVP
        // for the first draw of this host frame. The renderer would bind this
        // matrix to a root constant / constant buffer before each draw call.
        const n64forge::video::Mat4 mvp = interpolator_->beginDraw();
        (void)mvp;

        // TODO: encode render pass, bind per-draw MVP, issue draws, present.
    }

    std::string backendName() const override { return "Direct3D 12 (sub-frame interpolation)"; }

    [[nodiscard]] n64forge::emulation::FrameRingBuffer* frameRing() const noexcept {
        return frameRing_.get();
    }

private:
    std::unique_ptr<n64forge::emulation::FrameRingBuffer> frameRing_;
    std::unique_ptr<n64forge::video::d3d12::FrameInterpolator> interpolator_;
};

} // namespace

std::unique_ptr<n64forge::Renderer> createD3D12Renderer() {
    return std::make_unique<D3D12Renderer>();
}
