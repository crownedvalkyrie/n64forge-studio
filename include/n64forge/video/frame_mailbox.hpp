#pragma once

#include "n64forge/core/types.hpp"
#include "n64forge/video/viewport_presenter.hpp"
#include <array>
#include <algorithm>
#include <atomic>
#include <memory>
#include <span>

namespace n64forge::video {

// One producer and one consumer. Each queued, displayed or producer-retained
// frame owns a slot reference. If all slots are held we drop a host snapshot,
// never stall the guest or overwrite pixels held by the presenter.
class FrameMailbox {
public:
    struct Slot {
        std::unique_ptr<u32[]> data;
        std::shared_ptr<FrameBacking> backing;
        u32 width = 0, height = 0;
        u32 bytesPerPixel = 4, pitch = 0;
        u64 timestamp = 0;
        std::atomic<unsigned> refs{0};
        u32* pixelsData() const { return backing ? backing->data() : data.get(); }
        std::span<const u32> pixels() const {
            return {pixelsData(), backing ? pitch * height / sizeof(u32) : width * height};
        }
        void release() noexcept { refs.fetch_sub(1, std::memory_order_release); }
    };
    explicit FrameMailbox(u32 capacity) : FrameMailbox(capacity, [](u32) {
        return std::shared_ptr<FrameBacking>{};
    }) {}
    template<class Allocate>
    FrameMailbox(u32 capacity, Allocate allocate) : capacity_(capacity) {
        for (auto& slot : slots_) slot.backing = allocate(capacity);
        packed_ = std::all_of(slots_.begin(), slots_.end(), [](const auto& s) { return bool(s.backing); });
        if (!packed_) for (auto& slot : slots_) {
            slot.backing.reset();
            slot.data = std::make_unique_for_overwrite<u32[]>(capacity);
        }
    }
    [[nodiscard]] bool packed() const { return packed_; }

    [[nodiscard]] Slot* acquire() noexcept {
        for (auto& slot : slots_) {
            unsigned free = 0;
            if (slot.refs.compare_exchange_strong(free, 1, std::memory_order_acquire,
                                                   std::memory_order_relaxed)) return &slot;
        }
        return nullptr;
    }
    [[nodiscard]] std::span<u32> storage(Slot& slot) const { return {slot.pixelsData(), capacity_}; }
    [[nodiscard]] std::span<const u32> lastPixels() const {
        return last_ ? last_->pixels() : std::span<const u32>{};
    }
    // Consumes the producer's acquisition; the producer retains it as last_.
    void publish(Slot& slot) noexcept {
        slot.refs.fetch_add(1, std::memory_order_relaxed);
        if (auto* old = pending_.exchange(&slot, std::memory_order_acq_rel)) old->release();
        if (last_) last_->release();
        last_ = &slot;
    }
    // The caller takes ownership of the queued reference.
    [[nodiscard]] Slot* take() noexcept { return pending_.exchange(nullptr, std::memory_order_acquire); }

    // Called while the worker is stopped.
    void reset() noexcept {
        if (auto* queued = take()) queued->release();
        if (last_) last_->release();
        last_ = nullptr;
    }
private:
    // Previous/current UI frames can both retain their slots. A queued frame
    // and a writable slot can then coexist without waiting on presentation.
    std::array<Slot, 4> slots_;
    const u32 capacity_;
    bool packed_ = false;
    std::atomic<Slot*> pending_{nullptr};
    Slot* last_ = nullptr;
};
}
