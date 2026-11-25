#pragma once
#include "n64forge/core/types.hpp"
#include <array>
#include <span>

namespace n64forge {

// 32 KiB battery-backed SRAM at PI domain 0x08000000. Plain byte-addressed
// storage with no command latch; immediately readable/writable.
class Sram {
public:
    static constexpr u32 Base = 0x08000000;
    static constexpr u32 Size = 32 * 1024;
    static constexpr u32 Mask = Size - 1;

    Sram() { reset(); }
    void attach(bool enabled) noexcept { attached_ = enabled; }
    [[nodiscard]] bool attached() const noexcept { return attached_; }
    [[nodiscard]] bool dirty() const noexcept { return dirty_; }
    void markSaved() noexcept { dirty_ = false; }
    [[nodiscard]] bool load(std::span<const u8> data) noexcept;
    [[nodiscard]] const auto& image() const noexcept { return storage_; }
    void reset() noexcept;
    [[nodiscard]] u8 read(u32 offset) const noexcept;
    void write(u32 offset, u8 value) noexcept;

private:
    std::array<u8, Size> storage_{};
    bool attached_ = false, dirty_ = false;
};

} // namespace n64forge
