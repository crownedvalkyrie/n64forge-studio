#pragma once

#include "n64forge/core/types.hpp"
#include <span>
#include <vector>

namespace n64forge::video {

// Host-only motion estimation on completed VI snapshots (RGBA byte order).
// No access to RDP, VI registers, or guest RAM.
class ViInterpolation {
public:
    void estimate(std::span<const u32> previous, std::span<const u32> current, int width, int height);
    bool synthesize(std::span<const u32> previous, std::span<const u32> current,
                    std::span<u32> output, double fraction) const;

private:
    struct Motion { int x, y; };
    static constexpr int Block = 16;
    int width_ = 0, height_ = 0, columns_ = 0;
    std::vector<Motion> blocks_;
};

} // namespace n64forge::video
