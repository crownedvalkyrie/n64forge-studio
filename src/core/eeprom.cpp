#include "n64forge/core/eeprom.hpp"
#include <algorithm>
#include <cstring>

namespace n64forge {

void Eeprom::attach(Type type) noexcept {
    reset();
    type_ = type;
}

void Eeprom::observeBlock(u8 block) noexcept {
    markAccessed();
    if (autoExpand_ && type_ == Type::Eeprom4K && block >= Size4K / BlockSize) {
        type_ = Type::Eeprom16K;
        dirty_ = true;
    }
}

bool Eeprom::load(std::span<const u8> data) noexcept {
    if (!attached() || data.size() < size()) return false;
    std::copy_n(data.begin(), size(), storage_.begin());
    dirty_ = false;
    return true;
}

void Eeprom::reset() noexcept {
    writeCycles_ = 0;
    dirty_ = false;
    autoExpand_ = false;
    accessed_ = false;
    // Unformatted EEPROM reads as 0xFF on real hardware.
    storage_.fill(0xFF);
}

void Eeprom::advance(u32 cycles) noexcept {
    if (writeCycles_ == 0) return;
    writeCycles_ = writeCycles_ > cycles ? writeCycles_ - cycles : 0;
}

bool Eeprom::readBlock(u8 block, std::span<u8> out) const noexcept {
    if (!attached() || out.size() < BlockSize) return false;
    markAccessed();
    // 4 Kbit parts ignore the upper two block-index bits (64 blocks wrap).
    const u32 index = is16K() ? (block & 0xFFu) : (block & 0x3Fu);
    const u32 offset = index * BlockSize;
    if (offset + BlockSize > size()) return false;
    std::copy_n(storage_.begin() + offset, BlockSize, out.begin());
    return true;
}

bool Eeprom::writeBlock(u8 block, std::span<const u8> data) noexcept {
    if (!attached() || busy() || data.size() < BlockSize) return false;
    markAccessed();
    const u32 index = is16K() ? (block & 0xFFu) : (block & 0x3Fu);
    const u32 offset = index * BlockSize;
    if (offset + BlockSize > size()) return false;
    std::copy_n(data.begin(), BlockSize, storage_.begin() + offset);
    dirty_ = true;
    writeCycles_ = WriteCycles;
    return true;
}

u16 Eeprom::infoId() const noexcept {
    // 0x0080 = 4 Kbit, 0x00C0 = 16 Kbit.
    return is16K() ? 0x00C0 : 0x0080;
}

} // namespace n64forge
