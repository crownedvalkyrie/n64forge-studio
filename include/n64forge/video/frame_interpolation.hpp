#pragma once

#include "n64forge/video/interpolation_math.hpp"

#include <cstdint>
#include <optional>
#include <vector>

namespace n64forge::video {

// Tunable camera-cut thresholds. These are intentionally left as constexpr so
// they can be adjusted per game in a future configuration system.
constexpr float kMaxTranslationDelta = 64.0f;
constexpr float kMaxRotationRadians = 0.25f;

// Per-draw geometry identity + the combined ModelViewProjection (MVP) matrix
// that the RSP HLE cached for this draw call. The MVP is stored column-major
// in column-vector convention (homogeneous w == 1) as 16 floats.
struct DrawCallSnapshot {
    uint64_t matchKey = 0;
    uint32_t vertexBufferAddress = 0;
    uint32_t vertexCount = 0;
    uint32_t texture0Crc = 0;
    uint32_t texture1Crc = 0;
    uint16_t matrixStackDepth = 0;
    uint64_t drawSequenceIndex = 0;
    bool is2DUI = false;
    Mat4 mvp{};
};

struct FrameSnapshot {
    uint64_t frameOrdinal = 0;
    std::vector<DrawCallSnapshot> draws;
};

// 64-bit FNV-1a hash used to match draws between consecutive source frames.
// The key intentionally includes matrix-stack depth and a monotonic sequence
// index so multiple instances sharing the same mesh/texture remain distinct.
[[nodiscard]] uint64_t ComputeDrawIdentity(uint32_t vertexRdramAddr,
                                           uint32_t vertexCount,
                                           uint32_t texture0Crc,
                                           uint32_t texture1Crc,
                                           uint16_t matrixStackDepth,
                                           uint64_t drawSequenceIndex) noexcept;

// Decompose prev/curr MVP matrices into translation, rotation quaternion and
// scale; detect camera cuts; and interpolate. Returns `curr.mvp` unmodified
// when alpha is outside (0, 1), when either draw is 2D UI, or when a camera
// cut is detected (setting `wasCut = true`).
[[nodiscard]] Mat4 InterpolateMvp(const DrawCallSnapshot& prev,
                                  const DrawCallSnapshot& curr,
                                  float alpha,
                                  bool& wasCut) noexcept;

// Compute interpolation alpha in [0, 1] for the current host frame along the
// source frame timeline: alpha = (hostTime - frameOrigin) / sourcePeriod.
[[nodiscard]] float ComputeInterpolationAlpha(double sourceFps,
                                              double hostTimeSeconds,
                                              double frameOriginSeconds) noexcept;

} // namespace n64forge::video
