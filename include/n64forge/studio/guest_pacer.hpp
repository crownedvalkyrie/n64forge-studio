#pragma once

#include "n64forge/core/hw_registers.hpp"
#include <chrono>

namespace n64forge::studio {

// Schedules guest work against monotonic wall time, not UI or audio ticks.
class GuestPacer {
public:
    using Clock = std::chrono::steady_clock;
    explicit GuestPacer(Clock::time_point now) : deadline_(now) {}

    [[nodiscard]] Clock::time_point advance(u32 cycles, Clock::time_point now) {
        const auto ns = u64{cycles} * 1'000'000'000 + remainder_;
        deadline_ += std::chrono::nanoseconds(ns / HwRegisters::CpuClockHz);
        remainder_ = ns % HwRegisters::CpuClockHz;
        // Keep short overruns as debt. Don't try to play a long suspend or
        // breakpoint as a burst of catch-up audio and video.
        if (now - deadline_ > std::chrono::milliseconds(250)) *this = GuestPacer(now);
        return deadline_;
    }

private:
    Clock::time_point deadline_;
    u64 remainder_ = 0;
};

} // namespace n64forge::studio
