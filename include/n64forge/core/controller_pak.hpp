#pragma once

#include "n64forge/core/types.hpp"
#include <array>
#include <span>

namespace n64forge {
enum class PakType : u8 { None, Controller, Rumble };

class ControllerPak {
public:
    static constexpr unsigned Size = 32 * 1024;
    using Block = std::span<u8, 32>;
    using ConstBlock = std::span<const u8, 32>;
    void attach(PakType type) noexcept;
    [[nodiscard]] PakType type() const noexcept { return type_; }
    [[nodiscard]] bool load(std::span<const u8> bytes) noexcept;
    [[nodiscard]] const auto& image() const noexcept { return data_; }
    [[nodiscard]] bool dirty() const noexcept { return dirty_; }
    void markSaved() noexcept { dirty_ = false; }
    [[nodiscard]] bool rumbling() const noexcept { return motor_; }
    [[nodiscard]] u8 info(bool reset) noexcept;
    [[nodiscard]] u8 read(u16 packedAddress, Block reply) noexcept;
    [[nodiscard]] u8 write(u16 packedAddress, ConstBlock bytes) noexcept;
    static u8 addressChecksum(u16 address) noexcept;
    static u8 dataCrc(ConstBlock bytes) noexcept;
private:
    void format() noexcept;
    [[nodiscard]] bool addressValid(u16 address) noexcept;
    std::array<u8, Size> data_{};
    PakType type_ = PakType::None;
    bool initialized_ = false, inserted_ = false, crcError_ = false;
    bool motor_ = false, unlocked_ = false, dirty_ = false;
};
} // namespace n64forge
