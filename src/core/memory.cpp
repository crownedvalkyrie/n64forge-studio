#include "n64forge/core/memory.hpp"
#include <cstddef>
#include <stdexcept>

namespace n64forge {
Memory::Memory() : rdram_(RdramSize, 0) {}
u8 Memory::read8(u32 a) const {
    if (static_cast<std::size_t>(a) >= rdram_.size()) throw std::out_of_range("RDRAM read8 out of range");
    return rdram_[a];
}
u16 Memory::read16(u32 a) const {
    if (static_cast<std::size_t>(a) > rdram_.size() - sizeof(u16)) throw std::out_of_range("RDRAM read16 out of range");
    return loadRdram<u16>(a);
}
u32 Memory::read32(u32 a) const {
    if (static_cast<std::size_t>(a) > rdram_.size() - sizeof(u32)) throw std::out_of_range("RDRAM read32 out of range");
    return loadRdram<u32>(a);
}
u64 Memory::read64(u32 a) const {
    if (static_cast<std::size_t>(a) > rdram_.size() - sizeof(u64)) throw std::out_of_range("RDRAM read64 out of range");
    return loadRdram<u64>(a);
}
void Memory::write8(u32 a, u8 v) {
    if (static_cast<std::size_t>(a) >= rdram_.size()) throw std::out_of_range("RDRAM write8 out of range");
    rdram_[a] = v;
    notifyWrite(a, 1);
}
void Memory::write16(u32 a, u16 v) {
    if (static_cast<std::size_t>(a) > rdram_.size() - sizeof(u16)) throw std::out_of_range("RDRAM write16 out of range");
    rdram_[a] = static_cast<u8>(v >> 8);
    rdram_[a + 1] = static_cast<u8>(v);
    notifyWrite(a, 2);
}
void Memory::write32(u32 a, u32 v) {
    if (static_cast<std::size_t>(a) > rdram_.size() - sizeof(u32)) throw std::out_of_range("RDRAM write32 out of range");
    rdram_[a] = static_cast<u8>(v >> 24);
    rdram_[a + 1] = static_cast<u8>(v >> 16);
    rdram_[a + 2] = static_cast<u8>(v >> 8);
    rdram_[a + 3] = static_cast<u8>(v);
    notifyWrite(a, 4);
}
void Memory::write64(u32 a, u64 v) {
    if (static_cast<std::size_t>(a) > rdram_.size() - sizeof(u64)) throw std::out_of_range("RDRAM write64 out of range");
    for (unsigned i = 0; i < 8; ++i) {
        rdram_[a + i] = static_cast<u8>(v >> (56 - 8 * i));
    }
    notifyWrite(a, 8);
}
u32 Memory::size() const noexcept { return static_cast<u32>(rdram_.size()); }
} // namespace n64forge
