#pragma once

#include <algorithm>
#include <cstdint>

namespace n64forge::studio {

// Host presentation only; callers supply a monotonic elapsed time.
class BootSequence {
public:
    static constexpr std::int64_t SweepMs = 1200;
    static constexpr std::int64_t FadeMs = 320;

    void start(bool enabled) noexcept { *this = {}; active_ = enabled; }
    void observeVideo() noexcept { if (active_) video_ = true; }
    [[nodiscard]] bool waitingForVideo() const noexcept { return active_ && !video_; }
    [[nodiscard]] bool active() const noexcept { return active_; }
    double advance(std::int64_t elapsedMs) noexcept {
        if (!active_) return 0;
        if (fadeStart_ < 0 && video_ && elapsedMs >= SweepMs) fadeStart_ = elapsedMs;
        if (fadeStart_ < 0) return 1;
        const double progress = std::clamp(double(elapsedMs - fadeStart_) / FadeMs, 0.0, 1.0);
        if (progress == 1) active_ = false;
        return 1 - progress * progress * (3 - 2 * progress);
    }

private:
    bool active_ = false, video_ = false;
    std::int64_t fadeStart_ = -1;
};

} // namespace n64forge::studio
