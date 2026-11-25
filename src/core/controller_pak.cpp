#include "n64forge/core/controller_pak.hpp"
#include <algorithm>

namespace n64forge {
void ControllerPak::attach(PakType type) noexcept {
    if (type == type_) return;
    type_ = type;
    inserted_ = type != PakType::None;
    crcError_ = motor_ = unlocked_ = false;
    if (type == PakType::Controller && !initialized_) format();
}

bool ControllerPak::load(std::span<const u8> bytes) noexcept {
    if (bytes.size() != Size) return false;
    if (std::all_of(bytes.begin(), bytes.end(), [&](u8 b) { return b == bytes.front(); }) &&
        (bytes.front() == 0 || bytes.front() == 0xFF)) {
        format();
    } else {
        std::copy(bytes.begin(), bytes.end(), data_.begin());
        initialized_ = true;
        dirty_ = false;
    }
    return true;
}

void ControllerPak::format() noexcept {
    data_.fill(0);
    // An empty single-bank Pak: four ID records, two identical inode
    // tables, and an empty note directory. Generate checksums rather than
    // copying a hardware image.
    for (unsigned i = 0; i < 32; ++i) data_[i] = static_cast<u8>(i);
    data_[0] = 0x81;
    std::array<u8, 32> id{};
    id[0] = 0x4E; id[1] = 0x36; id[2] = 0x34; id[3] = 0x46;
    id[24] = 0; id[25] = 1; // device ID: Controller Pak
    id[26] = 1; // one bank
    u16 sum = 0;
    for (unsigned i = 0; i < 28; i += 2) sum += (u16{id[i]} << 8) | id[i + 1];
    const auto inverse = static_cast<u16>(0xFFF2 - sum);
    id[28] = sum >> 8; id[29] = sum;
    id[30] = inverse >> 8; id[31] = inverse;
    for (unsigned at : {0x20u, 0x60u, 0x80u, 0xC0u})
        std::copy(id.begin(), id.end(), data_.begin() + at);
    for (unsigned table : {0x100u, 0x200u}) {
        for (unsigned page = 5; page < 128; ++page) data_[table + 2 * page + 1] = 3;
        data_[table + 1] = (123 * 3) & 0xFF;
    }
    initialized_ = true;
    dirty_ = true;
}

u8 ControllerPak::addressChecksum(u16 address) noexcept {
    constexpr u8 keys[]{1, 0x1A, 0x0D, 0x1C, 0x0E, 7, 0x19, 0x16, 0x0B, 0x1F, 0x15};
    u8 crc = 0;
    for (unsigned i = 0; i < 11; ++i)
        if (address & (0x8000u >> i)) crc ^= keys[i];
    return crc;
}

u8 ControllerPak::dataCrc(ConstBlock bytes) noexcept {
    u8 crc = 0;
    for (const auto value : bytes) {
        crc ^= value;
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = static_cast<u8>((crc << 1) ^ (crc & 0x80 ? 0x85 : 0));
    }
    return crc;
}

bool ControllerPak::addressValid(u16 address) noexcept {
    crcError_ = (address & 31) != addressChecksum(address);
    return !crcError_ && !inserted_ && type_ != PakType::None;
}

u8 ControllerPak::info(bool reset) noexcept {
    if (reset) motor_ = unlocked_ = false;
    const u8 state = type_ == PakType::None ? 2 : inserted_ ? 3 : 1;
    const u8 result = state | (crcError_ ? 4 : 0);
    inserted_ = crcError_ = false;
    return result;
}

u8 ControllerPak::read(u16 packedAddress, Block reply) noexcept {
    const bool attached = addressValid(packedAddress);
    const unsigned address = packedAddress & 0xFFE0;
    std::fill(reply.begin(), reply.end(), 0);
    if (!attached) return 0xFF;
    if (type_ == PakType::Controller && address < Size)
        std::copy_n(data_.begin() + address, 32, reply.begin());
    if (type_ == PakType::Rumble && address >= 0x8000)
        std::fill(reply.begin(), reply.end(), address < 0xC000 ? (unlocked_ ? 0x80 : 0) : motor_);
    return dataCrc(reply);
}

u8 ControllerPak::write(u16 packedAddress, ConstBlock bytes) noexcept {
    const u8 crc = dataCrc(bytes);
    if (!addressValid(packedAddress)) return crc ^ 0xFF;
    const unsigned address = packedAddress & 0xFFE0;
    if (type_ == PakType::Controller && address < Size) {
        std::copy(bytes.begin(), bytes.end(), data_.begin() + address);
        dirty_ = true;
    } else if (type_ == PakType::Rumble) {
        if (address >= 0xC000) motor_ = (bytes.front() & 1) != 0;
        else if (address >= 0x8000) unlocked_ = bytes.front() == 0x80;
    }
    return crc;
}
} // namespace n64forge
