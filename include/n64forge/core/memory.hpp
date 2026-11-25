#pragma once
#include "n64forge/core/types.hpp"
#include <algorithm>
#include <bit>
#include <cstring>
#include <span>
#include <vector>

namespace n64forge {
class Memory {
public:
    static constexpr u32 RdramBase = 0x00000000;
    static constexpr u32 RdramSize = 8 * 1024 * 1024;

    Memory();
    u8 read8(u32 address) const;
    u16 read16(u32 address) const;
    u32 read32(u32 address) const;
    u64 read64(u32 address) const;
    void write8(u32 address, u8 value);
    void write16(u32 address, u16 value);
    void write32(u32 address, u32 value);
    void write64(u32 address, u64 value);
    [[nodiscard]] u32 size() const noexcept;
    [[nodiscard]] u8* data() noexcept { return rdram_.data(); }
    [[nodiscard]] const u8* data() const noexcept { return rdram_.data(); }

    // Callers must prove that the complete access is inside RDRAM. A memcpy
    // permits native loads/stores without aliasing the byte array as a word.
    template<class T> [[nodiscard]] T loadRdram(u32 offset) const noexcept {
        T value;
        std::memcpy(&value, rdram_.data() + offset, sizeof(value));
        return guestOrder(value);
    }

    template<class T> void storeRdram(u32 offset, T value) noexcept {
        value = guestOrder(value);
        std::memcpy(rdram_.data() + offset, &value, sizeof(value));
        notifyWrite(offset, sizeof(T));
    }

    // A run of rasterizer stores can defer the coherency callback until the
    // guest can fetch code again. The data write itself is never deferred, so
    // guest reads always see it. Only touched 64-byte invalidation lines are
    // reported: one covering min/max range would invalidate all of RDRAM
    // between distant color and depth surfaces. The bitmap makes duplicate
    // writes to the same line constant-time instead of growing a range list.
    struct DeferredWriteSet {
        DeferredWriteSet() : touched(RdramSize / 64, 0) { touchedLines.reserve(1024); }

        void add(u32 address, u32 size) {
            if (!size || address >= RdramSize) return;
            const u32 last = address + std::min(size, RdramSize - address) - 1;
            for (u32 line = address >> 6; line <= (last >> 6); ++line) {
                if (!touched[line]) {
                    touched[line] = 1;
                    touchedLines.push_back(line);
                }
            }
        }
        [[nodiscard]] bool empty() const noexcept { return touchedLines.empty(); }
        void reset() noexcept {
            for (u32 line : touchedLines) touched[line] = 0;
            touchedLines.clear();
        }

        std::vector<u8> touched;
        std::vector<u32> touchedLines;
    };

    template<class T> void storeRdramDeferred(u32 offset, T value, DeferredWriteSet& deferred) {
        value = guestOrder(value);
        std::memcpy(rdram_.data() + offset, &value, sizeof(value));
        deferred.add(offset, sizeof(T));
    }

    void flushDeferred(DeferredWriteSet& deferred) noexcept {
        if (writeCallback_)
            for (u32 line : deferred.touchedLines) writeCallback_(writeContext_, line << 6, 64);
        deferred.reset();
    }

    // Same bounds contract as loadRdram. Copies guest bytes without byte swapping.
    void copyRdram(u32 offset, std::span<u8> out) const noexcept {
        std::memcpy(out.data(), rdram_.data() + offset, out.size());
    }

    // The callback observes RDRAM changes; rasterizer runs may defer many
    // stores' coherency notifications until the command completes. It is a
    // plain function pointer so a guest store does not pay for type-erased
    // call dispatch; the CPU installs it to keep its block cache coherent with
    // self-modifying code and DMA.
    using WriteCallback = void (*)(void* context, u32 address, u32 size);
    void setWriteCallback(WriteCallback cb, void* context = nullptr) {
        writeCallback_ = cb;
        writeContext_ = context;
    }

private:
    void notifyWrite(u32 address, u32 size) noexcept {
        if (writeCallback_ != nullptr) writeCallback_(writeContext_, address, size);
    }

    template<class T> static T guestOrder(T value) noexcept {
        if constexpr (std::endian::native == std::endian::little && sizeof(T) > 1) {
            T swapped = 0;
            for (unsigned i = 0; i < sizeof(T); ++i) {
                swapped = static_cast<T>((swapped << 8) | (value & 0xFF));
                value >>= 8;
            }
            return swapped;
        }
        return value;
    }

    std::vector<u8> rdram_;
    WriteCallback writeCallback_ = nullptr;
    void* writeContext_ = nullptr;
};
}
