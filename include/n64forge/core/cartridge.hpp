#pragma once
#include "n64forge/core/types.hpp"
#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace n64forge {

enum class RomFormat { Unknown, Z64, N64, V64 };

struct CartridgeHeader {
    u32 initialConfig = 0;
    u32 clockRate = 0;
    u32 bootPc = 0;
    u32 release = 0;
    u32 crc1 = 0;
    u32 crc2 = 0;
    std::string name;
    u8 mediaFormat = 0;
    u16 cartridgeId = 0;
    u8 countryCode = 0;
    u8 version = 0;
};

class Cartridge {
public:
    static constexpr u32 BusBase = 0x10000000;
    static constexpr u32 BusEnd = 0x1FC00000;
    static constexpr std::size_t MaxMappedSize = BusEnd - BusBase;
    Cartridge() = default;

    bool load(const std::filesystem::path& path);
    bool loadFromBuffer(std::vector<u8> data, RomFormat hint = RomFormat::Unknown);

    [[nodiscard]] bool loaded() const noexcept { return !rom_.empty(); }
    [[nodiscard]] RomFormat format() const noexcept { return format_; }
    [[nodiscard]] const std::vector<u8>& rom() const noexcept { return rom_; }
    [[nodiscard]] size_t size() const noexcept { return rom_.size(); }
    [[nodiscard]] const CartridgeHeader& header() const noexcept { return header_; }
    [[nodiscard]] std::string formatName() const;

private:
    bool loadZip(const std::filesystem::path& path);
    [[nodiscard]] RomFormat detectFormat(const std::vector<u8>& data, RomFormat hint) const;
    void convertToBigEndian(std::vector<u8>& data, RomFormat format);
    void parseHeader();

    std::vector<u8> rom_;
    RomFormat format_ = RomFormat::Unknown;
    CartridgeHeader header_;
};

} // namespace n64forge
