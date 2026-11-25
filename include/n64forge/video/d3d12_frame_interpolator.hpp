#pragma once

#include "n64forge/emulation/frame_ring_buffer.hpp"
#include "n64forge/video/frame_interpolation.hpp"

#include <chrono>
#include <cstdint>

namespace n64forge::video::d3d12 {

// Windows D3D12 sub-frame interpolation helper. It mirrors the macOS Metal
// pipeline: compute alpha from host time, pull the current/previous source
// frames from the shared FrameRingBuffer, match draws by identity key, run
// InterpolateMvp and make the resulting MVP available for the D3D12 renderer
// to bind as a per-draw constant.
class FrameInterpolator {
public:
    explicit FrameInterpolator(n64forge::emulation::FrameRingBuffer& frameRing);

    void setSourceFps(double sourceFps) { sourceFps_ = sourceFps; }

    // Called once per host presentation frame. Returns the interpolated MVP for
    // the next draw call; the renderer should bind this via a root constant/
    // constant buffer before issuing the draw.
    [[nodiscard]] Mat4 beginDraw();

    // Must be called before each subsequent draw in the same host frame.
    [[nodiscard]] Mat4 nextDraw();

    // Returns the alpha used for the current host frame, primarily for logging
    // and debugging.
    [[nodiscard]] float currentAlpha() const noexcept { return currentAlpha_; }

private:
    void advanceFrame();

    n64forge::emulation::FrameRingBuffer& frameRing_;
    double sourceFps_ = 0.0;
    std::chrono::steady_clock::time_point linkStartTime_;
    uint64_t currentOrdinal_ = 0;
    uint64_t lastConsumedOrdinal_ = 0;
    float currentAlpha_ = 1.0f;
    std::optional<n64forge::video::FrameSnapshot> currentFrame_;
    std::optional<n64forge::video::FrameSnapshot> previousFrame_;
    std::size_t drawIndex_ = 0;
};

} // namespace n64forge::video::d3d12
