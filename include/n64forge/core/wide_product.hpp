#pragma once
#include "n64forge/core/types.hpp"

namespace n64forge {

struct WideProduct {
    u64 lo;
    u64 hi;
};

// Four 32-bit partial products avoid a compiler-specific integer type.
[[nodiscard]] constexpr WideProduct multiplyWideUnsigned(u64 a, u64 b) noexcept {
    constexpr u64 mask = 0xFFFFFFFFULL;
    const u64 p00 = (a & mask) * (b & mask);
    const u64 p01 = (a & mask) * (b >> 32);
    const u64 p10 = (a >> 32) * (b & mask);
    const u64 p11 = (a >> 32) * (b >> 32);
    const u64 middle = (p00 >> 32) + (p01 & mask) + (p10 & mask);
    return {(p00 & mask) | (middle << 32), p11 + (p01 >> 32) + (p10 >> 32) + (middle >> 32)};
}

[[nodiscard]] constexpr WideProduct multiplyWideSigned(u64 a, u64 b) noexcept {
    auto product = multiplyWideUnsigned(a, b);
    // Sign extending either factor changes only the high half of the product.
    if (a >> 63) product.hi -= b;
    if (b >> 63) product.hi -= a;
    return product;
}

} // namespace n64forge
