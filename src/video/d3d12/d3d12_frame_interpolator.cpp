#include "n64forge/video/d3d12_frame_interpolator.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace n64forge::video::d3d12 {

namespace {

using n64forge::video::DrawCallSnapshot;

const DrawCallSnapshot& MatchPreviousDraw(
    const DrawCallSnapshot& current,
    const std::unordered_map<uint64_t, DrawCallSnapshot>& previousByKey,
    const DrawCallSnapshot* previousSameIndex) {
    const auto it = previousByKey.find(current.matchKey);
    if (it != previousByKey.end()) {
        return it->second;
    }
    if (previousSameIndex) {
        return *previousSameIndex;
    }
    return current;
}

} // namespace

FrameInterpolator::FrameInterpolator(n64forge::emulation::FrameRingBuffer& frameRing)
    : frameRing_(frameRing)
    , linkStartTime_(std::chrono::steady_clock::now()) {
}

void FrameInterpolator::advanceFrame() {
    const auto now = std::chrono::steady_clock::now();
    const double hostTimeSeconds = std::chrono::duration<double>(now - linkStartTime_).count();
    const double sourcePeriod = (sourceFps_ > 0.0) ? (1.0 / sourceFps_) : (1.0 / 30.0);
    const double sourceTime = hostTimeSeconds / sourcePeriod;
    const uint64_t currentOrdinal = static_cast<uint64_t>(std::floor(sourceTime)) + 1;

    currentAlpha_ = n64forge::video::ComputeInterpolationAlpha(sourceFps_, hostTimeSeconds, 0.0);

    if (currentOrdinal == lastConsumedOrdinal_) {
        return;
    }
    currentOrdinal_ = currentOrdinal;
    lastConsumedOrdinal_ = currentOrdinal;

    previousFrame_ = currentFrame_;
    currentFrame_ = frameRing_.waitFrame(currentOrdinal_);
    drawIndex_ = 0;
}

Mat4 FrameInterpolator::beginDraw() {
    advanceFrame();
    drawIndex_ = 0;
    return nextDraw();
}

Mat4 FrameInterpolator::nextDraw() {
    if (!currentFrame_ || drawIndex_ >= currentFrame_->draws.size()) {
        // No source frame available yet; return identity so rendering can
        // continue without crashing.
        Mat4 identity{};
        identity.columns[0][0] = 1.0f;
        identity.columns[1][1] = 1.0f;
        identity.columns[2][2] = 1.0f;
        identity.columns[3][3] = 1.0f;
        return identity;
    }

    const DrawCallSnapshot& current = currentFrame_->draws[drawIndex_];
    const DrawCallSnapshot* previousSameIndex = nullptr;
    if (previousFrame_ && drawIndex_ < previousFrame_->draws.size()) {
        previousSameIndex = &previousFrame_->draws[drawIndex_];
    }

    std::unordered_map<uint64_t, DrawCallSnapshot> previousByKey;
    if (previousFrame_) {
        previousByKey.reserve(previousFrame_->draws.size());
        for (const auto& draw : previousFrame_->draws) {
            previousByKey.emplace(draw.matchKey, draw);
        }
    }

    const DrawCallSnapshot& previous = MatchPreviousDraw(current, previousByKey, previousSameIndex);
    bool wasCut = false;
    const Mat4 mvp = n64forge::video::InterpolateMvp(previous, current, currentAlpha_, wasCut);
    ++drawIndex_;
    return mvp;
}

} // namespace n64forge::video::d3d12
