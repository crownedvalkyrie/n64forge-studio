#include "n64forge/emulation/frame_ring_buffer.hpp"

#include <algorithm>
#include <chrono>
#include <utility>

namespace n64forge::emulation {

namespace {

// A display-clock consumer can ask for an ordinal the producer has not reached
// yet (or never will). Bound the wait so presentation cannot stall forever.
constexpr auto kWaitTimeout = std::chrono::milliseconds(100);

} // namespace

void FrameRingBuffer::publish(FrameSnapshot snapshot) {
    if (closed_.load(std::memory_order_acquire)) return;

    const uint64_t ordinal = snapshot.frameOrdinal;
    std::lock_guard<std::mutex> lock(mutex_);
    if (closed_.load(std::memory_order_acquire)) return;

    const uint64_t published = head_.load(std::memory_order_acquire);
    // A stale or duplicate ordinal cannot help a consumer that has moved on.
    if (published != 0 && ordinal < published) return;

    const uint64_t tail = tail_.load(std::memory_order_acquire);
    // Slot `ordinal % kSlotCount` is reusable only once the consumer has asked
    // past `ordinal - kSlotCount`. Drop this newest frame if it is still held;
    // never make the guest spin waiting for the display thread.
    if (published != 0 && ordinal >= tail && ordinal - tail >= kSlotCount) return;

    slots_[ordinal % kSlotCount] = std::move(snapshot);
    // Release-store publishes the slot write to the consumer.
    head_.store(ordinal + 1, std::memory_order_release);
    condition_.notify_all();
}

std::optional<FrameSnapshot> FrameRingBuffer::waitFrame(uint64_t frameOrdinal) {
    FrameSnapshot fallback;
    fallback.frameOrdinal = frameOrdinal;

    std::unique_lock<std::mutex> lock(mutex_);
    // A renderer can be activated before any HLE producer exists. Return an
    // empty snapshot immediately instead of waiting for a frame that may never
    // arrive.
    if (head_.load(std::memory_order_acquire) == 0) return fallback;

    condition_.wait_for(lock, kWaitTimeout, [this, frameOrdinal] {
        return closed_.load(std::memory_order_acquire) ||
               head_.load(std::memory_order_acquire) > frameOrdinal ||
               tail_.load(std::memory_order_acquire) > frameOrdinal;
    });

    const auto advanceTail = [this, frameOrdinal] {
        const uint64_t current = tail_.load(std::memory_order_relaxed);
        tail_.store(std::max(current, frameOrdinal + 1), std::memory_order_release);
    };

    if (closed_.load(std::memory_order_acquire) ||
        tail_.load(std::memory_order_acquire) > frameOrdinal) {
        advanceTail();
        return fallback;
    }

    const auto& slot = slots_[frameOrdinal % kSlotCount];
    if (slot.frameOrdinal != frameOrdinal) {
        // head_ passed the request without this ordinal being published (a
        // skipped frame). Never return another frame's draws by mistake.
        advanceTail();
        return fallback;
    }

    FrameSnapshot out = slot;
    advanceTail();
    return out;
}

void FrameRingBuffer::close() noexcept {
    closed_.store(true, std::memory_order_release);
    condition_.notify_all();
}

} // namespace n64forge::emulation
