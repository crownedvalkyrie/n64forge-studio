#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>

#ifdef __APPLE__
#include <simd/simd.h>
#endif

namespace n64forge::video {

// Cross-platform SIMD-friendly math types used by the sub-frame interpolation
// pipeline. On Apple platforms the underlying storage is the native simd.h type;
// on Windows it is a plain C++20 implementation with the same column-major,
// column-vector layout.

#ifdef __APPLE__

using Mat4 = simd::float4x4;
using Vec4 = simd::float4;
using Vec3 = simd::float3;
using Quat = simd::quatf;

#else

struct Vec4 {
    float v[4];
    [[nodiscard]] float& operator[](std::size_t i) noexcept { return v[i]; }
    [[nodiscard]] const float& operator[](std::size_t i) const noexcept { return v[i]; }
};

struct Mat4 {
    Vec4 columns[4];
};

struct Vec3 {
    float v[3];
    [[nodiscard]] float& operator[](std::size_t i) noexcept { return v[i]; }
    [[nodiscard]] const float& operator[](std::size_t i) const noexcept { return v[i]; }
};

struct Quat {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 1.0f;
};

#endif

// Column-vector convention: translation lives in the homogeneous column.
[[nodiscard]] inline Vec3 Mat4Translation(const Mat4& m) noexcept {
    Vec3 t;
    t[0] = m.columns[3][0];
    t[1] = m.columns[3][1];
    t[2] = m.columns[3][2];
    return t;
}

// Per-axis scale from the column norms of the linear part. Negative scale
// (mirroring) is preserved as a negative scalar per axis.
[[nodiscard]] inline Vec3 Mat4Scale(const Mat4& m) noexcept {
    const auto colNorm = [](const Vec4& c) -> float {
        return std::sqrt(c[0] * c[0] + c[1] * c[1] + c[2] * c[2]);
    };
    Vec3 s;
    s[0] = colNorm(m.columns[0]);
    s[1] = colNorm(m.columns[1]);
    s[2] = colNorm(m.columns[2]);
    return s;
}

// Recompose M = T * R * S (column-vector convention).
[[nodiscard]] Mat4 Mat4FromTrs(const Vec3& translation, const Quat& rotation, const Vec3& scale) noexcept;

// Extract the rotation quaternion from a (possibly scaled) 4x4 matrix.
// Negative determinant / mirroring is handled by preserving the sign of the
// per-axis scale in callers; this routine returns the pure rotation part.
[[nodiscard]] Quat Mat4Rotation(const Mat4& m) noexcept;

// Quaternion dot product.
[[nodiscard]] float QuatDot(const Quat& a, const Quat& b) noexcept;

// Spherical linear interpolation between two unit quaternions.
[[nodiscard]] Quat QuatSlerp(const Quat& a, const Quat& b, float t) noexcept;

// Convert a quaternion to a 4x4 rotation matrix.
[[nodiscard]] Mat4 QuatToMat4(const Quat& q) noexcept;

// Linearly interpolate each component.
[[nodiscard]] inline Vec3 Vec3Lerp(const Vec3& a, const Vec3& b, float t) noexcept {
    Vec3 r;
    r[0] = a[0] + (b[0] - a[0]) * t;
    r[1] = a[1] + (b[1] - a[1]) * t;
    r[2] = a[2] + (b[2] - a[2]) * t;
    return r;
}

// L-infinity (Chebyshev) distance between two 3D points.
[[nodiscard]] inline float Vec3MaxAbsDiff(const Vec3& a, const Vec3& b) noexcept {
    return std::max(std::abs(a[0] - b[0]),
                    std::max(std::abs(a[1] - b[1]),
                             std::abs(a[2] - b[2])));
}

} // namespace n64forge::video
