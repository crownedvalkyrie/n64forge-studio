#include "n64forge/core/is_viewer.hpp"
#include <utility>

namespace n64forge {

void IsViewer::reset() noexcept {
    ram_.fill(0);
    lines_.clear();
    pending_.clear();
    magicPresent_ = false;
    flushCount_ = 0;
}

u8 IsViewer::read8(u32 offset) const noexcept {
    return byte(offset);
}

u16 IsViewer::read16(u32 offset) const noexcept {
    return static_cast<u16>((static_cast<u16>(byte(offset)) << 8) | byte(offset + 1));
}

u32 IsViewer::read32(u32 offset) const noexcept {
    return (static_cast<u32>(byte(offset)) << 24) |
           (static_cast<u32>(byte(offset + 1)) << 16) |
           (static_cast<u32>(byte(offset + 2)) << 8) |
           static_cast<u32>(byte(offset + 3));
}

void IsViewer::store16(u32 offset, u16 value) noexcept {
    byte(offset) = static_cast<u8>(value >> 8);
    byte(offset + 1) = static_cast<u8>(value);
}

void IsViewer::store32(u32 offset, u32 value) noexcept {
    byte(offset) = static_cast<u8>(value >> 24);
    byte(offset + 1) = static_cast<u8>(value >> 16);
    byte(offset + 2) = static_cast<u8>(value >> 8);
    byte(offset + 3) = static_cast<u8>(value);
}

void IsViewer::observeMagic() noexcept {
    if (!magicPresent_ && read32(MagicOffset) == 0x49533634u) // "IS64"
        magicPresent_ = true;
}

void IsViewer::write8(u32 offset, u8 value) noexcept {
    byte(offset) = value;
    observeMagic();
}

void IsViewer::write16(u32 offset, u16 value) noexcept {
    offset &= Size - 1;
    if (offset == FlushOffset) {
        flush(value);
        return;
    }
    store16(offset, value);
    observeMagic();
}

void IsViewer::write32(u32 offset, u32 value) noexcept {
    offset &= Size - 1;
    // PI writes are halfwords; a 32-bit guest store flushes through the low
    // halfword at +0x16.
    write16(offset, static_cast<u16>(value >> 16));
    write16(offset + 2, static_cast<u16>(value));
}

void IsViewer::emitLine() noexcept {
    while (!pending_.empty() && (pending_.back() == '\r' || pending_.back() == '\0'))
        pending_.pop_back();
    if (!pending_.empty()) {
        lines_.push_back(pending_);
        if (lines_.size() > MaxLogLines) lines_.pop_front();
        if (callback_) callback_(pending_);
    }
    pending_.clear();
}

void IsViewer::flush(u16 end) noexcept {
    ++flushCount_;
    const u16 start = read16(ReadPointerOffset);
    for (u32 address = start; address < end; ++address) {
        const u8 ch = byte(BufferOffset + address);
        if (ch == '\n' || ch == '\0') {
            emitLine();
        } else if (ch != '\r') {
            pending_.push_back(static_cast<char>(ch));
        }
    }
    emitLine();
    store32(ReadPointerOffset, 0);
}

} // namespace n64forge
