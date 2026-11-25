#pragma once
#include "n64forge/core/hw_registers.hpp"
#include "n64forge/core/types.hpp"
#include <array>
#include <span>

namespace n64forge {

// Serial EEPROM on Joybus channel 4. 4 Kbit (64 x 8-byte blocks) and 16 Kbit
// (256 x 8-byte blocks) variants. Data is big-endian bus order.
class Eeprom {
public:
    static constexpr u32 Size4K = 512;
    static constexpr u32 Size16K = 2048;
    static constexpr u32 BlockSize = 8;
    // EEPROM writes complete in the background; games poll the info/status
    // byte until the busy bit clears. ~6 ms per N64brew documentation.
    static constexpr u32 WriteCycles = HwRegisters::CpuClockHz * 6 / 1000;

    enum class Type { None, Eeprom4K, Eeprom16K };

    Eeprom() { reset(); }
    void attach(Type type) noexcept;
    void enableAutoExpansion() noexcept { autoExpand_ = true; }
    void observeBlock(u8 block) noexcept;
    void markAccessed() const noexcept { accessed_ = true; }
    [[nodiscard]] Type type() const noexcept { return type_; }
    [[nodiscard]] bool attached() const noexcept { return type_ != Type::None; }
    [[nodiscard]] bool is16K() const noexcept { return type_ == Type::Eeprom16K; }
    [[nodiscard]] u32 size() const noexcept { return is16K() ? Size16K : Size4K; }
    [[nodiscard]] bool dirty() const noexcept { return dirty_; }
    [[nodiscard]] bool accessed() const noexcept { return accessed_; }
    void markSaved() noexcept { dirty_ = false; }
    [[nodiscard]] bool busy() const noexcept { return writeCycles_ > 0; }
    [[nodiscard]] bool load(std::span<const u8> data) noexcept;
    [[nodiscard]] const auto& image() const noexcept { return storage_; }
    void reset() noexcept;
    void advance(u32 cycles) noexcept;

    // Joybus channel-4 commands. readBlock returns 8 data bytes into `out`.
    // writeBlock copies 8 bytes from `data` and starts the write timer.
    // Returns false if the block index is invalid or a write is still busy.
    [[nodiscard]] bool readBlock(u8 block, std::span<u8> out) const noexcept;
    [[nodiscard]] bool writeBlock(u8 block, std::span<const u8> data) noexcept;
    // Info reply: 2-byte big-endian device ID, bit 0x80 in byte 2 when busy.
    [[nodiscard]] u16 infoId() const noexcept;
    [[nodiscard]] u8 infoStatus() const noexcept { return busy() ? 0x80 : 0; }

private:
    std::array<u8, Size16K> storage_{};
    Type type_ = Type::None;
    u32 writeCycles_ = 0;
    bool dirty_ = false;
    bool autoExpand_ = false;
    mutable bool accessed_ = false;
};

} // namespace n64forge
