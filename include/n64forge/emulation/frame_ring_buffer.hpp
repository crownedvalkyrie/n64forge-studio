#pragma once

#include "n64forge/video/frame_interpolation.hpp"

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>

namespace n64forge::emulation {

// Aliases so the ring buffer API reads naturally; the canonical types live in
// n64forge::video so both macOS and Windows share the same layout.
using DrawCallSnapshot = n64forge::video::DrawCallSnapshot;
using FrameSnapshot = n64forge::video::FrameSnapshot;

// Triple-buffered frame ring between the N64 emulation thread (producer) and
// the host render thread (consumer) on both macOS and Windows.
//
// - Producer (emulation thread / HLE graphics plugin): never blocks. If the
//   target slot has not been reclaimed, the newest frame is dropped instead of
//   stalling guest execution.
// - Consumer (host display thread): waits at most a short bounded interval for
//   an ordinal. If no producer has ever published, the ordinal was skipped, or
//   close() was called, it receives an empty snapshot with that ordinal so
//   wall-clock-driven interpolators can keep rendering without hanging.
class FrameRingBuffer {
public:
    static constexpr uint32_t kSlotCount = 3;

    void publish(FrameSnapshot snapshot);
    [[nodiscard]] std::optional<FrameSnapshot> waitFrame(uint64_t frameOrdinal);
    [[nodiscard]] uint64_t head() const { return head_.load(std::memory_order_acquire); }

    // Stop accepting frames and wake every waiter. Idempotent and callable
    // from any thread; waitFrame() returns a fallback snapshot afterwards.
    void close() noexcept;
    [[nodiscard]] bool closed() const noexcept { return closed_.load(std::memory_order_acquire); }

private:
    std::array<FrameSnapshot, kSlotCount> slots_{};
    std::atomic<uint64_t> head_{0};
    std::atomic<uint64_t> tail_{0};
    std::atomic<bool> closed_{false};
    mutable std::mutex mutex_;
    std::condition_variable condition_;
};

} // namespace n64forge::emulation
