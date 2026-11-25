#pragma once
#include "n64forge/core/types.hpp"
#include <array>
#include <deque>
#include <functional>
#include <string>
#include <string_view>

namespace n64forge {

// IS-Viewer 64 debug cartridge. The device is a 64 KiB PI RAM window at
// 0x13FF0000. Software writes the "IS64" magic at +0x00, fills the text buffer
// at +0x20, then writes the output end index at +0x16 to flush a message.
// +0x04 is a buffer-relative read pointer; the device clears it after a flush.
// The flush convention is from ares and supports both libultra (end pointer)
// and libdragon (byte count) clients without inspecting the guest ROM.
class IsViewer {
public:
    static constexpr u32 Base = 0x13FF0000;
    static constexpr u32 End = 0x14000000;
    static constexpr u32 Size = 0x10000;
    static constexpr u32 MagicOffset = 0x00;
    static constexpr u32 ReadPointerOffset = 0x04;
    static constexpr u32 FlushOffset = 0x16;
    static constexpr u32 BufferOffset = 0x20;
    static constexpr std::size_t MaxLogLines = 64;

    using LogCallback = std::function<void(std::string_view)>;

    void reset() noexcept;
    void setLogCallback(LogCallback callback) { callback_ = std::move(callback); }

    [[nodiscard]] const std::deque<std::string>& lines() const noexcept { return lines_; }
    [[nodiscard]] bool magicPresent() const noexcept { return magicPresent_; }
    [[nodiscard]] u32 flushCount() const noexcept { return flushCount_; }

    [[nodiscard]] u8 read8(u32 offset) const noexcept;
    [[nodiscard]] u16 read16(u32 offset) const noexcept;
    [[nodiscard]] u32 read32(u32 offset) const noexcept;
    void write8(u32 offset, u8 value) noexcept;
    void write16(u32 offset, u16 value) noexcept;
    void write32(u32 offset, u32 value) noexcept;

private:
    [[nodiscard]] u8& byte(u32 offset) noexcept { return ram_[offset & (Size - 1)]; }
    [[nodiscard]] u8 byte(u32 offset) const noexcept { return ram_[offset & (Size - 1)]; }
    void store16(u32 offset, u16 value) noexcept;
    void store32(u32 offset, u32 value) noexcept;
    void observeMagic() noexcept;
    void flush(u16 end) noexcept;
    void emitLine() noexcept;

    std::array<u8, Size> ram_{};
    std::deque<std::string> lines_;
    std::string pending_;
    LogCallback callback_;
    bool magicPresent_ = false;
    u32 flushCount_ = 0;
};

} // namespace n64forge
