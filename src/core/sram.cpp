#include "n64forge/core/sram.hpp"
#include <algorithm>

namespace n64forge {

bool Sram::load(std::span<const u8> data) noexcept {
    if (!attached() || data.size() != Size) return false;
    std::copy_n(data.begin(), Size, storage_.begin());
    dirty_ = false;
    return true;
}

void Sram::reset() noexcept {
    dirty_ = false;
    // Uninitialized SRAM reads as 0xFF on real hardware.
    storage_.fill(0xFF);
}

u8 Sram::read(u32 offset) const noexcept {
    return storage_[offset & Mask];
}

void Sram::write(u32 offset, u8 value) noexcept {
    const u32 index = offset & Mask;
    if (storage_[index] != value) {
        storage_[index] = value;
        dirty_ = true;
    }
}

} // namespace n64forge
