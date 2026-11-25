#include "n64forge/video/frame_interpolation.hpp"

#include <algorithm>
#include <cmath>

namespace n64forge::video {

namespace {

// FNV-1a 64-bit constants.
constexpr uint64_t kFnvOffsetBasis = 0xCBF29CE484222325ULL;
constexpr uint64_t kFnvPrime = 0x100000001B3ULL;

[[nodiscard]] inline uint64_t fnv1aMix(uint64_t hash, uint64_t value) noexcept {
    for (std::size_t i = 0; i < sizeof(value); ++i) {
        hash ^= (value >> (i * 8)) & 0xFF;
        hash *= kFnvPrime;
    }
    return hash;
}

} // namespace

uint64_t ComputeDrawIdentity(uint32_t vertexRdramAddr,
                             uint32_t vertexCount,
                             uint32_t texture0Crc,
                             uint32_t texture1Crc,
                             uint16_t matrixStackDepth,
                             uint64_t drawSequenceIndex) noexcept {
    uint64_t hash = kFnvOffsetBasis;
    hash = fnv1aMix(hash, static_cast<uint64_t>(vertexRdramAddr));
    hash = fnv1aMix(hash, static_cast<uint64_t>(vertexCount));
    hash = fnv1aMix(hash, static_cast<uint64_t>(texture0Crc));
    hash = fnv1aMix(hash, static_cast<uint64_t>(texture1Crc));
    hash = fnv1aMix(hash, static_cast<uint64_t>(matrixStackDepth));
    hash = fnv1aMix(hash, drawSequenceIndex);
    return hash;
}

Mat4 InterpolateMvp(const DrawCallSnapshot& prev,
                    const DrawCallSnapshot& curr,
                    float alpha,
                    bool& wasCut) noexcept {
    wasCut = false;
    if (alpha <= 0.0f) {
        return curr.mvp;
    }
    if (alpha >= 1.0f) {
        return curr.mvp;
    }
    if (prev.is2DUI || curr.is2DUI) {
        // Screen-space UI (G_TEXRECT / orthographic passes) never interpolates.
        return curr.mvp;
    }

    // Camera-cut / teleport detection: L-infinity translation delta plus
    // quaternion angular delta, compared against tunable thresholds.
    const Vec3 prevT = Mat4Translation(prev.mvp);
    const Vec3 currT = Mat4Translation(curr.mvp);
    const float translationDelta = Vec3MaxAbsDiff(currT, prevT);

    // Rotation delta from quaternion dot: angle = 2 * acos(|dot|).
    // We derive the rotation quaternion directly from each MVP (scale is removed
    // implicitly by the quaternion-from-matrix routine in the math layer).
    const Quat prevQuat = Mat4Rotation(prev.mvp);
    const Quat currQuat = Mat4Rotation(curr.mvp);
    const float dot = std::abs(QuatDot(prevQuat, currQuat));
    const float clampedDot = std::min(1.0f, dot);
    const float angleDelta = 2.0f * std::acos(clampedDot);

    if (translationDelta > kMaxTranslationDelta || angleDelta > kMaxRotationRadians) {
        wasCut = true;
        return curr.mvp;
    }

    // Decompose scale, slerp rotation, lerp translation/scale, recompose.
    const Quat midQuat = QuatSlerp(prevQuat, currQuat, alpha);
    const Vec3 prevS = Mat4Scale(prev.mvp);
    const Vec3 currS = Mat4Scale(curr.mvp);
    const Vec3 scale = Vec3Lerp(prevS, currS, alpha);
    const Vec3 translation = Vec3Lerp(prevT, currT, alpha);

    return Mat4FromTrs(translation, midQuat, scale);
}

float ComputeInterpolationAlpha(double sourceFps,
                                double hostTimeSeconds,
                                double frameOriginSeconds) noexcept {
    const double sourcePeriod = sourceFps > 0.0 ? (1.0 / sourceFps) : 0.0;
    if (!(sourcePeriod > 0.0)) {
        return 1.0f;
    }
    double t = hostTimeSeconds - frameOriginSeconds;
    double frac = std::fmod(t, sourcePeriod);
    if (frac < 0.0) {
        frac += sourcePeriod;
    }
    return static_cast<float>(std::clamp(frac / sourcePeriod, 0.0, 1.0));
}

} // namespace n64forge::video
