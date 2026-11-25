#pragma once
#include "n64forge/core/types.hpp"
#include <span>

namespace n64forge {

// CIC variants that can be identified from the IPL2 checksum stored by the PIF.
// The checksum function and the published secrets are documented in
// docs/REFERENCES.md; no ROM title or CRC is consulted.
enum class CicVariant {
    Unknown,
    Cic5101,
    Cic6101,
    Cic6102,
    Cic7102,
    Cic6103,
    Cic6105,
    Cic6106,
};

struct CicProfile {
    CicVariant variant = CicVariant::Unknown;
    u8 seed = 0x3F;
    u32 ntscId = 6102;
    u32 palId = 7101;
    u32 entryBias = 0;
    bool challenge = false;
    bool fixedEntry = false;
    u32 fixedEntryAddress = 0;

    [[nodiscard]] u32 id(bool pal) const noexcept { return pal ? palId : ntscId; }
};

// Computes the 48-bit IPL2 checksum over the cartridge boot code. `rom` must
// have at least 0x1004 bytes; IPL3 occupies 0x40..0xFFF and the algorithm reads
// the following word as its lookahead value.
[[nodiscard]] u64 ipl2Checksum(std::span<const u8> rom, u8 seed) noexcept;

// Looks up the CIC/IPL3 pair from an IPL2 checksum and from a complete ROM.
[[nodiscard]] CicProfile cicProfileForChecksum(u64 checksum) noexcept;
[[nodiscard]] CicProfile detectCic(std::span<const u8> rom) noexcept;
[[nodiscard]] const char* cicName(CicVariant variant) noexcept;

} // namespace n64forge
