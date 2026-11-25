// IPL2 checksum derived from jago85/PifChecksum (MIT), cross-checked against
// the ares IPL2 implementation and the N64brew PIF-NUS published secrets.
// Copyright (c) 2019 jago85. Permission notice in docs/REFERENCES.md.
#include "n64forge/core/ipl.hpp"

#include <array>
#include <cstdint>

namespace n64forge {
namespace {

constexpr u32 BootOffset = 0x40;
constexpr u32 ChecksumWords = 1008;
constexpr u32 ChecksumMagic = 0x6C078965;

[[nodiscard]] u32 readWord(std::span<const u8> rom, u32 offset) noexcept {
    const u32 at = BootOffset + offset;
    if (at + 3 >= rom.size()) return 0;
    return (static_cast<u32>(rom[at]) << 24) | (static_cast<u32>(rom[at + 1]) << 16) |
           (static_cast<u32>(rom[at + 2]) << 8) | static_cast<u32>(rom[at + 3]);
}

[[nodiscard]] u32 rotateLeft(u32 value, u32 shift) noexcept {
    shift &= 31;
    return shift == 0 ? value : (value << shift) | (value >> (32 - shift));
}

[[nodiscard]] u32 rotateRight(u32 value, u32 shift) noexcept {
    shift &= 31;
    return shift == 0 ? value : (value >> shift) | (value << (32 - shift));
}

[[nodiscard]] u32 checksumFunction(u32 a0, u32 a1, u32 a2) noexcept {
    if (a1 == 0) a1 = a2;
    const u64 product = static_cast<u64>(a0) * a1;
    const u32 hi = static_cast<u32>(product >> 32);
    const u32 lo = static_cast<u32>(product);
    const u32 difference = hi - lo;
    return difference != 0 ? difference : a0;
}

} // namespace

u64 ipl2Checksum(std::span<const u8> rom, u8 seed) noexcept {
    if (rom.size() < BootOffset + ChecksumWords * 4 + 4) return 0;

    std::array<u32, 16> state{};
    const u32 initial = (ChecksumMagic * static_cast<u32>(seed & 0xFF) + 1) ^ readWord(rom, 0);
    state.fill(initial);

    u32 dataLast = 0;
    u32 data = readWord(rom, 0);
    u32 dataNext = data;
    for (u32 loop = 1;; ++loop) {
        dataLast = data;
        data = dataNext;

        state[0] += checksumFunction(1007 - loop, data, loop);
        state[1] = checksumFunction(state[1], data, loop);
        state[2] ^= data;
        state[3] += checksumFunction(data + 5, ChecksumMagic, loop);
        state[9] = dataLast < data ? checksumFunction(state[9], data, loop) : state[9] + data;
        state[4] += rotateRight(data, dataLast & 0x1F);
        state[7] = checksumFunction(state[7], rotateLeft(data, dataLast & 0x1F), loop);
        state[6] = data < state[6] ? (state[3] + state[6]) ^ (data + loop)
                                   : (state[4] + data) ^ state[6];
        state[5] += rotateLeft(data, dataLast >> 27);
        state[8] = checksumFunction(state[8], rotateRight(data, dataLast >> 27), loop);

        if (loop == ChecksumWords) break;

        dataNext = readWord(rom, loop * 4);
        state[15] = checksumFunction(
            checksumFunction(state[15], rotateLeft(data, dataLast >> 27), loop),
            rotateLeft(dataNext, data >> 27), loop);
        state[14] = checksumFunction(
            checksumFunction(state[14], rotateRight(data, dataLast & 0x1F), loop),
            rotateRight(dataNext, data & 0x1F), loop);
        state[13] += rotateRight(data, data & 0x1F) + rotateRight(dataNext, dataNext & 0x1F);
        state[10] = checksumFunction(state[10] + data, dataNext, loop);
        state[11] = checksumFunction(state[11] ^ data, dataNext, loop);
        state[12] += state[8] ^ data;
    }

    std::array<u32, 4> output{};
    output.fill(state[0]);
    for (u32 loop = 0; loop < state.size(); ++loop) {
        const u32 value = state[loop];
        const u32 rotated = rotateRight(value, value & 0x1F);
        const u32 sum = output[0] + rotated;
        output[0] = sum;
        output[1] = value < sum ? output[1] + value : checksumFunction(output[1], value, loop);

        const u32 bit = value & 1;
        output[2] = ((value >> 1) & 1) == bit ? output[2] + value
                                              : checksumFunction(output[2], value, loop);
        output[3] = bit != 0 ? output[3] ^ value : checksumFunction(output[3], value, loop);
    }

    return ((static_cast<u64>(checksumFunction(output[0], output[1], 16)) << 32) |
            static_cast<u64>(output[3] ^ output[2])) &
           0xFFFFFFFFFFFFULL;
}

CicProfile cicProfileForChecksum(u64 checksum) noexcept {
    switch (checksum) {
        // 6101 and 7102 have distinct IPL3 variants, so their reported CIC id
        // is fixed by the authenticated boot code rather than by the TV type.
        case 0x45CC73EE317AULL: return {CicVariant::Cic6101, 0x3F, 6101, 6101};
        case 0xA536C0F1D859ULL: return {CicVariant::Cic6102, 0x3F, 6102, 7101};
        case 0x44160EC5D9AFULL:
            return {CicVariant::Cic7102, 0x3F, 7102, 7102, 0, false, true, 0x80000480};
        case 0x586FD4709867ULL: return {CicVariant::Cic6103, 0x78, 6103, 7103, 0x100000};
        case 0x8618A45BC2D3ULL: return {CicVariant::Cic6105, 0x91, 6105, 7105, 0, true};
        case 0x2BBAD4E6EB74ULL: return {CicVariant::Cic6106, 0x85, 6106, 7106, 0x200000};
        case 0x93E983A8F152ULL: return {CicVariant::Cic5101, 0xAC, 5101, 5101, 0x100000};
        default: return {};
    }
}

const char* cicName(CicVariant variant) noexcept {
    switch (variant) {
        case CicVariant::Cic5101: return "CIC-NUS-5101";
        case CicVariant::Cic6101: return "CIC-NUS-6101";
        case CicVariant::Cic6102: return "CIC-NUS-6102/7101";
        case CicVariant::Cic7102: return "CIC-NUS-7102";
        case CicVariant::Cic6103: return "CIC-NUS-6103/7103";
        case CicVariant::Cic6105: return "CIC-NUS-6105/7105";
        case CicVariant::Cic6106: return "CIC-NUS-6106/7106";
        case CicVariant::Unknown: return "unknown";
    }
    return "unknown";
}

CicProfile detectCic(std::span<const u8> rom) noexcept {
    if (rom.size() < BootOffset + ChecksumWords * 4 + 4) return {};
    for (const u8 seed : {u8{0x3F}, u8{0x78}, u8{0x91}, u8{0x85}, u8{0xAC}}) {
        auto profile = cicProfileForChecksum(ipl2Checksum(rom, seed));
        if (profile.variant != CicVariant::Unknown) return profile;
    }
    return {};
}

} // namespace n64forge
