#pragma once
#include "n64forge/core/types.hpp"
#include <array>
#include <span>

namespace n64forge {
// Byte-indexed Macronix MX29L1101 variant. Save data is big-endian bus order.
class FlashRam {
public:
    static constexpr u32 Base = 0x08000000, Command = Base + 0x10000;
    static constexpr u32 Size = 128 * 1024, Page = 128, Sector = 16 * 1024;
    FlashRam() { storage_.fill(0xFF); reset(); }
    void attach(bool enabled) noexcept { attached_ = enabled; reset(); }
    [[nodiscard]] bool attached() const noexcept { return attached_; }
    [[nodiscard]] bool dirty() const noexcept { return dirty_; }
    void markSaved() noexcept { dirty_ = false; }
    [[nodiscard]] bool load(std::span<const u8> data) noexcept;
    [[nodiscard]] const auto& image() const noexcept { return storage_; }
    void reset() noexcept;
    void command(u32 value) noexcept;
    // Cartridges decode the command latch as two 16-bit writes.
    void commandHalf(u32 offset, u16 value) noexcept;
    void advance(u32 cycles) noexcept;
    [[nodiscard]] u8 read(u32 offset) const noexcept;
    void write(u32 offset, u8 value) noexcept;

private:
    enum class Mode { Array, Status, SiliconId, LoadPage };
    enum class Operation { Idle, SectorErase, ChipErase, Program };
    std::array<u8, Size> storage_;
    std::array<u8, Page> page_;
    Mode mode_ = Mode::Array;
    Operation setup_ = Operation::Idle, busy_ = Operation::Idle;
    u32 offset_ = 0, cycles_ = 0, status_ = 0;
    u16 commandHigh_ = 0;
    bool commandHighValid_ = false;
    bool attached_ = false, dirty_ = false;
};
} // namespace n64forge
