#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace n64forge {

// Host-only 48 kHz stereo FIFO. The emulation producer never waits for the
// device. A single consumer owns the resampling cursor and underrun envelope.
template<std::size_t Capacity>
class AudioRing {
public:
    using Frame = std::array<std::int16_t, 2>;
    // Rendered output carries its provenance so callers can ignore synthetic
    // fade/tail frames when counting real guest audio.
    struct OutputFrame {
        Frame frame;
        bool fromGuest;
        [[nodiscard]] const std::int16_t& operator[](std::size_t i) const noexcept { return frame[i]; }
    };
    struct Stats {
        std::size_t fillFrames = 0;
        std::size_t capacityFrames = Capacity;
        std::uint64_t underruns = 0, overruns = 0;
    };
    static_assert(Capacity >= 8 && (Capacity & (Capacity - 1)) == 0);
    static_assert(std::atomic<std::uint32_t>::is_always_lock_free);

    std::size_t push(const std::int16_t* samples, std::size_t frames) noexcept {
        const auto write = write_.load(std::memory_order_relaxed);
        const auto read = read_.load(std::memory_order_acquire);
        const auto count = std::min(frames, Capacity - (write - read));
        for (std::size_t i = 0; i < count; ++i)
            data_[(write + i) & (Capacity - 1)] = {samples[i * 2], samples[i * 2 + 1]};
        write_.store(write + static_cast<std::uint32_t>(count), std::memory_order_release);
        if (count < frames) overruns_.fetch_add(1, std::memory_order_relaxed);
        return count;
    }

    // Each request uses a bounded correction based on FIFO fill. This corrects
    // small device clock drift; silence/short fades handle genuine starvation.
    template<class Emit>
    void render(std::size_t frames, Emit emit) noexcept {
        auto read = read_.load(std::memory_order_relaxed);
        auto write = write_.load(std::memory_order_acquire);
        constexpr auto target = Capacity / 4;
        const double error = (static_cast<double>(write - read) - target) / target;
        const double rate = 1.0 + std::clamp(error * 0.005, -0.005, 0.005);
        const auto step = static_cast<std::uint32_t>(rate * 65536.0 + 0.5);
        for (std::size_t i = 0; i < frames; ++i) {
            // The producer may push while this callback is running. If the
            // snapshot has run dry, pick up the current writer position so
            // those frames are played instead of being skipped by the
            // underrun reset below.
            if (write - read < 2) {
                // Atomic read coherence preserves progress even when the
                // 32-bit sequence wraps; a numeric comparison would not.
                write = write_.load(std::memory_order_acquire);
            }
            if (!primed_ && write - read >= target) {
                primed_ = true;
                fade_ = 0;
            }
            if (primed_ && write - read >= 2) {
                const auto& a = data_[read & (Capacity - 1)];
                const auto& b = data_[(read + 1) & (Capacity - 1)];
                fade_ = std::min(32u, fade_ + 1);
                for (std::size_t ch = 0; ch < 2; ++ch) {
                    const std::int32_t sample = (std::int32_t{a[ch]} * (65536 - phase_) +
                                                std::int32_t{b[ch]} * phase_) >> 16;
                    last_[ch] = scaleSample(sample, fade_);
                }
                const auto next = static_cast<std::uint32_t>(phase_) + step;
                read += next >> 16;
                phase_ = static_cast<std::uint16_t>(next);
                tail_ = 32;
                static_cast<void>(emit(OutputFrame{last_, true}));
            } else if (primed_ && write - read >= 1) {
                // With one guest frame left there is no second endpoint for
                // interpolation, but the frame is still real guest audio.
                const auto& a = data_[read & (Capacity - 1)];
                fade_ = std::min(32u, fade_ + 1);
                for (std::size_t ch = 0; ch < 2; ++ch)
                    last_[ch] = scaleSample(a[ch], fade_);
                ++read;
                phase_ = 0;
                tail_ = 32;
                static_cast<void>(emit(OutputFrame{last_, true}));
            } else {
                if (primed_) {
                    underruns_.fetch_add(1, std::memory_order_relaxed);
                    primed_ = false;
                    phase_ = 0;
                    read = write;
                }
                if (tail_) --tail_;
                static_cast<void>(emit(OutputFrame{
                    Frame{scaleSample(last_[0], tail_), scaleSample(last_[1], tail_)}, false}));
            }
        }
        read_.store(read, std::memory_order_release);
    }

    [[nodiscard]] Stats stats() const noexcept {
        const auto read = read_.load(std::memory_order_acquire);
        const auto write = write_.load(std::memory_order_acquire);
        return {std::min<std::size_t>(write - read, Capacity), Capacity,
                underruns_.load(std::memory_order_relaxed), overruns_.load(std::memory_order_relaxed)};
    }

    // Only while both ends are stopped.
    void reset() noexcept {
        read_.store(0, std::memory_order_relaxed);
        write_.store(0, std::memory_order_relaxed);
        underruns_.store(0, std::memory_order_relaxed);
        overruns_.store(0, std::memory_order_relaxed);
        last_ = {};
        phase_ = fade_ = tail_ = 0;
        primed_ = false;
    }

private:
    // Signed scaling keeps negative samples negative; clamp instead of
    // relying on the implementation-defined out-of-range int16 cast.
    [[nodiscard]] static std::int16_t scaleSample(std::int32_t value, unsigned factor) noexcept {
        const std::int32_t scaled = value * static_cast<std::int32_t>(factor) / 32;
        return static_cast<std::int16_t>(std::clamp(scaled, std::int32_t{-32768}, std::int32_t{32767}));
    }

    std::array<Frame, Capacity> data_{};
    alignas(64) std::atomic<std::uint32_t> read_{0};
    alignas(64) std::atomic<std::uint32_t> write_{0};
    std::atomic<std::uint64_t> underruns_{0}, overruns_{0};
    Frame last_{};
    std::uint16_t phase_ = 0;
    unsigned fade_ = 0, tail_ = 0;
    bool primed_ = false;
};
}
