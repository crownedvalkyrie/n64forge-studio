#include "n64forge/video/interpolation_math.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace n64forge::video {

#ifdef __APPLE__

Mat4 Mat4FromTrs(const Vec3& translation, const Quat& rotation, const Vec3& scale) noexcept {
    const Mat4 rot = QuatToMat4(rotation);
    Mat4 result = rot;
    for (std::size_t col = 0; col < 4; ++col) {
        const Vec4& src = rot.columns[col];
        const float s = (col < 3) ? static_cast<float>(scale[col]) : 1.0f;
        Vec4 dst;
        dst[0] = src[0] * s;
        dst[1] = src[1] * s;
        dst[2] = src[2] * s;
        dst[3] = src[3] * s;
        if (col == 3) {
            dst[0] = src[0] + translation[0];
            dst[1] = src[1] + translation[1];
            dst[2] = src[2] + translation[2];
            dst[3] = src[3] + translation[3];
        }
        result.columns[col] = dst;
    }
    return result;
}

float QuatDot(const Quat& a, const Quat& b) noexcept {
    return simd_dot(a, b);
}

Quat QuatSlerp(const Quat& a, const Quat& b, float t) noexcept {
    return simd_slerp(a, b, t);
}

Mat4 QuatToMat4(const Quat& q) noexcept {
    return Mat4(q);
}

Quat Mat4Rotation(const Mat4& m) noexcept {
    // Remove per-column scale, then construct a quaternion from the normalized
    // rotation basis. This handles negative scale safely because the scale is
    // extracted separately by Mat4Scale(); here we only need the orientation.
    const Vec3 scale = Mat4Scale(m);
    Mat4 normalized{};
    for (std::size_t col = 0; col < 3; ++col) {
        const float s = scale[col];
        const float inv = (s != 0.0f) ? (1.0f / std::abs(s)) : 0.0f;
        normalized.columns[col][0] = m.columns[col][0] * inv;
        normalized.columns[col][1] = m.columns[col][1] * inv;
        normalized.columns[col][2] = m.columns[col][2] * inv;
        normalized.columns[col][3] = 0.0f;
    }
    normalized.columns[3][0] = 0.0f;
    normalized.columns[3][1] = 0.0f;
    normalized.columns[3][2] = 0.0f;
    normalized.columns[3][3] = 1.0f;
    return Quat(normalized);
}

#else

namespace {

[[nodiscard]] inline Quat quatNormalize(const Quat& q) noexcept {
    const float len = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (len <= 0.0f) return {0.0f, 0.0f, 0.0f, 1.0f};
    const float inv = 1.0f / len;
    return {q.x * inv, q.y * inv, q.z * inv, q.w * inv};
}

} // namespace

Mat4 Mat4FromTrs(const Vec3& translation, const Quat& rotation, const Vec3& scale) noexcept {
    const Mat4 rot = QuatToMat4(rotation);
    Mat4 result = rot;
    for (std::size_t col = 0; col < 4; ++col) {
        const Vec4& src = rot.columns[col];
        const float s = (col < 3) ? static_cast<float>(scale[col]) : 1.0f;
        Vec4 dst;
        dst[0] = src[0] * s;
        dst[1] = src[1] * s;
        dst[2] = src[2] * s;
        dst[3] = src[3] * s;
        if (col == 3) {
            dst[0] = src[0] + translation[0];
            dst[1] = src[1] + translation[1];
            dst[2] = src[2] + translation[2];
            dst[3] = src[3] + translation[3];
        }
        result.columns[col] = dst;
    }
    return result;
}

float QuatDot(const Quat& a, const Quat& b) noexcept {
    return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
}

Quat QuatSlerp(const Quat& a, const Quat& b, float t) noexcept {
    Quat b0 = b;
    float dot = QuatDot(a, b0);
    if (dot < 0.0f) {
        b0 = {-b0.x, -b0.y, -b0.z, -b0.w};
        dot = -dot;
    }
    if (dot > 0.9995f) {
        // Very close angles: fall back to normalized lerp to avoid division by
        // a tiny sine.
        Quat r;
        r.x = a.x + (b0.x - a.x) * t;
        r.y = a.y + (b0.y - a.y) * t;
        r.z = a.z + (b0.z - a.z) * t;
        r.w = a.w + (b0.w - a.w) * t;
        return quatNormalize(r);
    }
    const float theta0 = std::acos(dot);
    const float theta = theta0 * t;
    const float sinTheta = std::sin(theta);
    const float sinTheta0 = std::sin(theta0);
    const float s0 = std::cos(theta) - dot * sinTheta / sinTheta0;
    const float s1 = sinTheta / sinTheta0;
    return quatNormalize({a.x * s0 + b0.x * s1,
                          a.y * s0 + b0.y * s1,
                          a.z * s0 + b0.z * s1,
                          a.w * s0 + b0.w * s1});
}

Mat4 QuatToMat4(const Quat& q) noexcept {
    const Quat n = quatNormalize(q);
    const float xx = n.x * n.x;
    const float yy = n.y * n.y;
    const float zz = n.z * n.z;
    const float xy = n.x * n.y;
    const float xz = n.x * n.z;
    const float yz = n.y * n.z;
    const float wx = n.w * n.x;
    const float wy = n.w * n.y;
    const float wz = n.w * n.z;

    Mat4 m{};
    m.columns[0][0] = 1.0f - 2.0f * (yy + zz);
    m.columns[0][1] = 2.0f * (xy + wz);
    m.columns[0][2] = 2.0f * (xz - wy);
    m.columns[0][3] = 0.0f;

    m.columns[1][0] = 2.0f * (xy - wz);
    m.columns[1][1] = 1.0f - 2.0f * (xx + zz);
    m.columns[1][2] = 2.0f * (yz + wx);
    m.columns[1][3] = 0.0f;

    m.columns[2][0] = 2.0f * (xz + wy);
    m.columns[2][1] = 2.0f * (yz - wx);
    m.columns[2][2] = 1.0f - 2.0f * (xx + yy);
    m.columns[2][3] = 0.0f;

    m.columns[3][0] = 0.0f;
    m.columns[3][1] = 0.0f;
    m.columns[3][2] = 0.0f;
    m.columns[3][3] = 1.0f;
    return m;
}

Quat Mat4Rotation(const Mat4& m) noexcept {
    // Normalize columns to remove scale, then extract the rotation quaternion.
    const Vec3 scale = Mat4Scale(m);
    Mat4 r{};
    for (std::size_t col = 0; col < 3; ++col) {
        const float s = scale[col];
        const float inv = (s != 0.0f) ? (1.0f / std::abs(s)) : 0.0f;
        r.columns[col][0] = m.columns[col][0] * inv;
        r.columns[col][1] = m.columns[col][1] * inv;
        r.columns[col][2] = m.columns[col][2] * inv;
        r.columns[col][3] = 0.0f;
    }
    r.columns[3][3] = 1.0f;

    const float m00 = r.columns[0][0];
    const float m11 = r.columns[1][1];
    const float m22 = r.columns[2][2];
    const float trace = m00 + m11 + m22;

    Quat q;
    if (trace > 0.0f) {
        const float s = 0.5f / std::sqrt(trace + 1.0f);
        q.w = 0.25f / s;
        q.x = (r.columns[1][2] - r.columns[2][1]) * s;
        q.y = (r.columns[2][0] - r.columns[0][2]) * s;
        q.z = (r.columns[0][1] - r.columns[1][0]) * s;
    } else if (m00 > m11 && m00 > m22) {
        const float s = 2.0f * std::sqrt(1.0f + m00 - m11 - m22);
        q.w = (r.columns[1][2] - r.columns[2][1]) / s;
        q.x = 0.25f * s;
        q.y = (r.columns[1][0] + r.columns[0][1]) / s;
        q.z = (r.columns[2][0] + r.columns[0][2]) / s;
    } else if (m11 > m22) {
        const float s = 2.0f * std::sqrt(1.0f + m11 - m00 - m22);
        q.w = (r.columns[2][0] - r.columns[0][2]) / s;
        q.x = (r.columns[1][0] + r.columns[0][1]) / s;
        q.y = 0.25f * s;
        q.z = (r.columns[2][1] + r.columns[1][2]) / s;
    } else {
        const float s = 2.0f * std::sqrt(1.0f + m22 - m00 - m11);
        q.w = (r.columns[0][1] - r.columns[1][0]) / s;
        q.x = (r.columns[2][0] + r.columns[0][2]) / s;
        q.y = (r.columns[2][1] + r.columns[1][2]) / s;
        q.z = 0.25f * s;
    }
    return quatNormalize(q);
}

#endif

} // namespace n64forge::video
