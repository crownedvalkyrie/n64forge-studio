#include "n64forge/video/vi_interpolation.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace n64forge::video {
namespace {
int luma(u32 rgba) {
    return ((rgba & 255) * 77 + ((rgba >> 8) & 255) * 150 + ((rgba >> 16) & 255) * 29) >> 8;
}
}

void ViInterpolation::estimate(std::span<const u32> previous, std::span<const u32> current,
                               int width, int height) {
    width_ = height_ = columns_ = 0;
    if (width <= 0 || height <= 0 || std::size_t(width) * height != previous.size() ||
        current.size() != previous.size()) return;
    width_ = width;
    height_ = height;
    columns_ = (width - 1) / Block + 1;
    const int rows = (height - 1) / Block + 1;
    blocks_.resize(std::size_t(columns_) * rows);
    for (int by = 0; by < rows; ++by) {
        for (int bx = 0; bx < columns_; ++bx) {
            Motion best{};
            int bestError = std::numeric_limits<int>::max();
            const int x0 = bx * Block, y0 = by * Block;
            const int x1 = std::min(width, x0 + Block), y1 = std::min(height, y0 + Block);
            // Search centered on the midpoint frame, measuring luminance SAD.
            for (int dy = -3; dy <= 3; ++dy) {
                for (int dx = -3; dx <= 3; ++dx) {
                    int error = 4 * (std::abs(dx) + std::abs(dy));
                    for (int y = y0; y < y1; y += 3) {
                        const int aRow = std::clamp(y - dy, 0, height - 1) * width;
                        const int bRow = std::clamp(y + dy, 0, height - 1) * width;
                        for (int x = x0; x < x1; x += 3)
                            error += std::abs(luma(previous[aRow + std::clamp(x - dx, 0, width - 1)]) -
                                              luma(current[bRow + std::clamp(x + dx, 0, width - 1)]));
                    }
                    if (error < bestError) {
                        bestError = error;
                        best = {2 * dx, 2 * dy};
                    }
                }
            }
            blocks_[by * columns_ + bx] = best;
        }
    }
}

bool ViInterpolation::synthesize(std::span<const u32> previous, std::span<const u32> current,
                                std::span<u32> output, double fraction) const {
    if (!width_ || output.size() != std::size_t(width_) * height_ ||
        previous.size() != output.size() || current.size() != output.size() || !std::isfinite(fraction))
        return false;
    const int t = static_cast<int>(std::clamp(fraction, 0.0, 1.0) * 256 + 0.5);
    if (t == 0 || t == 256) {
        const auto source = t == 0 ? previous : current;
        std::copy(source.begin(), source.end(), output.begin());
        return true;
    }
    for (int y = 0; y < height_; ++y) {
        for (int x = 0; x < width_; x += Block) {
            const auto m = blocks_[y / Block * columns_ + x / Block];
            const int ax = static_cast<int>(std::round(m.x * t / 256.0));
            const int ay = static_cast<int>(std::round(m.y * t / 256.0));
            const int bx = m.x - ax, by = m.y - ay;
            const int aRow = std::clamp(y - ay, 0, height_ - 1) * width_;
            const int bRow = std::clamp(y + by, 0, height_ - 1) * width_;
            for (int i = x; i < std::min(x + Block, width_); ++i) {
                const u32 a = previous[aRow + std::clamp(i - ax, 0, width_ - 1)];
                const u32 b = current[bRow + std::clamp(i + bx, 0, width_ - 1)];
                u32 pixel = 0xFF000000;
                for (u32 channel = 0; channel < 24; channel += 8) {
                    const u32 value = (((a >> channel) & 255) * (256 - t) + ((b >> channel) & 255) * t + 128) >> 8;
                    pixel |= value << channel;
                }
                output[y * width_ + i] = pixel;
            }
        }
    }
    return true;
}

} // namespace n64forge::video
