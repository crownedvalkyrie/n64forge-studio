#include "n64forge/core/cartridge.hpp"
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string_view>

#if defined(N64FORGE_HAS_ZLIB)
#include <zlib.h>
#endif

namespace n64forge {

namespace {

constexpr size_t HeaderSize = 0x40;
constexpr std::array<u8, 4> BigEndianMagic = {0x80, 0x37, 0x12, 0x40};

[[nodiscard]] u32 readU32Be(const u8* p) {
    return (static_cast<u32>(p[0]) << 24) |
           (static_cast<u32>(p[1]) << 16) |
           (static_cast<u32>(p[2]) << 8)  |
           static_cast<u32>(p[3]);
}

[[nodiscard]] u16 readU16Be(const u8* p) {
    return static_cast<u16>((static_cast<u16>(p[0]) << 8) | p[1]);
}

[[nodiscard]] u32 readLe32(const u8* p) {
    return static_cast<u32>(p[0]) |
           (static_cast<u32>(p[1]) << 8) |
           (static_cast<u32>(p[2]) << 16) |
           (static_cast<u32>(p[3]) << 24);
}

[[nodiscard]] bool hasRomExtension(const std::string& name) {
    const auto dot = name.rfind('.');
    if (dot == std::string::npos) return false;
    std::string ext = name.substr(dot);
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return ext == ".z64" || ext == ".n64" || ext == ".v64" || ext == ".rom";
}

[[nodiscard]] RomFormat extensionHint(const std::filesystem::path& path) {
    const std::string ext = path.extension().string();
    if (ext == ".z64" || ext == ".Z64") return RomFormat::Z64;
    if (ext == ".n64" || ext == ".N64") return RomFormat::N64;
    if (ext == ".v64" || ext == ".V64") return RomFormat::V64;
    return RomFormat::Unknown;
}

} // namespace

bool Cartridge::load(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) return false;
    const auto end = file.tellg();
    if (end < std::streamoff(4)) return false;
    const auto size = static_cast<std::size_t>(end);
    file.seekg(0, std::ios::beg);
    std::array<u8, 4> lead{};
    if (!file.read(reinterpret_cast<char*>(lead.data()), lead.size())) return false;
    // ZIP magic is checked before the ROM size limit: a compressed archive may
    // contain a full-size ROM in a much smaller file.
    if (lead[0] == 'P' && lead[1] == 'K' && (lead[2] == 3 || lead[2] == 5) &&
        (lead[3] == 4 || lead[3] == 6)) {
        return loadZip(path);
    }
    if (size < HeaderSize || size > MaxMappedSize) return false;
    file.seekg(0, std::ios::beg);
    std::vector<u8> data(size);
    if (!file.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(size))) return false;
    return loadFromBuffer(std::move(data), extensionHint(path));
}

bool Cartridge::loadZip(const std::filesystem::path& path) {
#if !defined(N64FORGE_HAS_ZLIB)
    (void)path;
    return false;
#else
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) return false;
    const auto fileSize = static_cast<std::size_t>(file.tellg());
    constexpr std::size_t EocdSize = 22;
    constexpr std::size_t MaxComment = 0xFFFF;
    if (fileSize < EocdSize) return false;
    const std::size_t tail = std::min(fileSize, EocdSize + MaxComment);
    std::vector<u8> end(tail);
    file.seekg(static_cast<std::streamoff>(fileSize - tail), std::ios::beg);
    if (!file.read(reinterpret_cast<char*>(end.data()), static_cast<std::streamsize>(tail))) return false;

    std::size_t eocd = std::string_view::npos;
    for (std::size_t i = end.size() - EocdSize + 1; i-- > 0;) {
        const auto* p = end.data() + i;
        if (p[0] == 'P' && p[1] == 'K' && p[2] == 5 && p[3] == 6) { eocd = i; break; }
    }
    if (eocd == std::string_view::npos) return false;
    const auto* e = end.data() + eocd;
    // Reject ZIP64 markers rather than silently truncating large archives.
    const auto isZip64U16 = [](const u8* p) { return p[0] == 0xFF && p[1] == 0xFF; };
    if (isZip64U16(e + 4) || isZip64U16(e + 6) || isZip64U16(e + 8) ||
        isZip64U16(e + 10) || readLe32(e + 12) == 0xFFFFFFFFu ||
        readLe32(e + 16) == 0xFFFFFFFFu)
        return false;
    const auto entries = static_cast<u16>(e[10] | (e[11] << 8));
    if (entries == 0) return false;
    u32 directorySize = readLe32(e + 12);
    u32 directoryOffset = readLe32(e + 16);
    if (static_cast<u64>(directoryOffset) + directorySize > fileSize) return false;

    std::vector<u8> directory(directorySize);
    file.seekg(static_cast<std::streamoff>(directoryOffset), std::ios::beg);
    if (!file.read(reinterpret_cast<char*>(directory.data()), static_cast<std::streamsize>(directorySize)))
        return false;

    struct Member {
        u32 offset = 0;
        u32 compressedSize = 0;
        u32 uncompressedSize = 0;
        u32 crc = 0;
        u16 method = 0;
        std::string name;
    };
    std::vector<Member> romMembers;
    std::size_t pos = 0;
    for (u16 n = 0; n < entries; ++n) {
        if (pos + 46 > directory.size() || readLe32(directory.data() + pos) != 0x02014B50)
            return false;
        const auto* h = directory.data() + pos;
        const u16 flags = static_cast<u16>(h[8] | (h[9] << 8));
        const u16 nameLen = static_cast<u16>(h[28] | (h[29] << 8));
        const u16 extraLen = static_cast<u16>(h[30] | (h[31] << 8));
        const u16 commentLen = static_cast<u16>(h[32] | (h[33] << 8));
        if (pos + 46u + nameLen + extraLen + commentLen > directory.size()) return false;
        const std::string name(reinterpret_cast<const char*>(h + 46), nameLen);
        pos += 46u + nameLen + extraLen + commentLen;
        if (!name.empty() && name.back() == '/') continue;
        if (name.starts_with("__MACOSX/") || name.ends_with(".DS_Store")) continue;
        if ((flags & 1u) != 0) return false; // encrypted member
        Member member;
        member.method = static_cast<u16>(h[10] | (h[11] << 8));
        member.crc = readLe32(h + 16);
        member.compressedSize = readLe32(h + 20);
        member.uncompressedSize = readLe32(h + 24);
        member.offset = readLe32(h + 42);
        member.name = name;
        if (member.method != 0 && member.method != 8) continue;
        if (member.uncompressedSize < HeaderSize || member.uncompressedSize > MaxMappedSize) continue;
        if (hasRomExtension(name)) romMembers.push_back(std::move(member));
        else if (romMembers.empty()) romMembers.push_back(std::move(member));
    }
    // Prefer explicit ROM extensions; otherwise accept one unambiguous file.
    if (romMembers.size() > 1) {
        std::vector<Member> named;
        for (const auto& member : romMembers)
            if (hasRomExtension(member.name)) named.push_back(member);
        if (named.size() != 1) return false;
        romMembers = std::move(named);
    }
    if (romMembers.size() != 1) return false;

    const auto& member = romMembers.front();
    std::array<u8, 30> local{};
    file.seekg(static_cast<std::streamoff>(member.offset), std::ios::beg);
    if (!file.read(reinterpret_cast<char*>(local.data()), local.size()) ||
        readLe32(local.data()) != 0x04034B50) return false;
    const u16 localNameLen = static_cast<u16>(local[26] | (local[27] << 8));
    const u16 localExtraLen = static_cast<u16>(local[28] | (local[29] << 8));
    const u64 dataOffset = static_cast<u64>(member.offset) + local.size() + localNameLen + localExtraLen;
    if (dataOffset + member.compressedSize > fileSize) return false;
    std::vector<u8> compressed(member.compressedSize);
    file.seekg(static_cast<std::streamoff>(dataOffset), std::ios::beg);
    if (!compressed.empty() &&
        !file.read(reinterpret_cast<char*>(compressed.data()), static_cast<std::streamsize>(compressed.size())))
        return false;

    std::vector<u8> data(member.uncompressedSize);
    if (member.method == 0) {
        if (compressed.size() != data.size() ||
            std::memcpy(data.data(), compressed.data(), data.size()) == nullptr) return false;
    } else {
        uLongf outputSize = static_cast<uLongf>(data.size());
        if (uncompress(data.data(), &outputSize, compressed.data(),
                       static_cast<uLong>(compressed.size())) != Z_OK ||
            outputSize != data.size()) return false;
    }
    if (crc32(0, data.data(), static_cast<uInt>(data.size())) != member.crc) return false;
    return loadFromBuffer(std::move(data), extensionHint(member.name));
#endif
}

bool Cartridge::loadFromBuffer(std::vector<u8> data, RomFormat hint) {
    if (data.size() < HeaderSize || data.size() > MaxMappedSize) return false;
    const auto format = detectFormat(data, hint);
    if (format == RomFormat::Unknown ||
        (format == RomFormat::V64 && data.size() % 2 != 0) ||
        (format == RomFormat::N64 && data.size() % 4 != 0)) return false;
    convertToBigEndian(data, format);

    // Verify the conversion produced the standard big-endian header magic.
    if (data.size() < BigEndianMagic.size() ||
        !std::equal(BigEndianMagic.begin(), BigEndianMagic.end(), data.begin())) {
        return false;
    }

    rom_ = std::move(data);
    format_ = format;
    parseHeader();
    return true;
}

RomFormat Cartridge::detectFormat(const std::vector<u8>& data, RomFormat hint) const {
    if (data.size() < 4) return RomFormat::Unknown;

    if (data[0] == BigEndianMagic[0] && data[1] == BigEndianMagic[1] &&
        data[2] == BigEndianMagic[2] && data[3] == BigEndianMagic[3]) {
        return RomFormat::Z64;
    }
    if (data[0] == BigEndianMagic[1] && data[1] == BigEndianMagic[0] &&
        data[2] == BigEndianMagic[3] && data[3] == BigEndianMagic[2]) {
        return RomFormat::V64;
    }
    if (data[0] == BigEndianMagic[3] && data[1] == BigEndianMagic[2] &&
        data[2] == BigEndianMagic[1] && data[3] == BigEndianMagic[0]) {
        return RomFormat::N64;
    }
    return hint;
}

void Cartridge::convertToBigEndian(std::vector<u8>& data, RomFormat format) {
    switch (format) {
        case RomFormat::Z64:
            break;
        case RomFormat::V64:
            for (size_t i = 0; i + 1 < data.size(); i += 2) {
                std::swap(data[i], data[i + 1]);
            }
            break;
        case RomFormat::N64:
            for (size_t i = 0; i + 3 < data.size(); i += 4) {
                std::swap(data[i], data[i + 3]);
                std::swap(data[i + 1], data[i + 2]);
            }
            break;
        default:
            break;
    }
}

void Cartridge::parseHeader() {
    if (rom_.size() < 0x40) return;
    const u8* p = rom_.data();
    header_.initialConfig = readU32Be(p + 0x00);
    header_.clockRate     = readU32Be(p + 0x04);
    header_.bootPc        = readU32Be(p + 0x08);
    header_.release       = readU32Be(p + 0x0C);
    header_.crc1          = readU32Be(p + 0x10);
    header_.crc2          = readU32Be(p + 0x14);

    constexpr size_t NameLen = 20;
    header_.name.assign(reinterpret_cast<const char*>(p + 0x20),
                        NameLen);
    // Trim trailing spaces/nulls.
    while (!header_.name.empty() &&
           (header_.name.back() == ' ' || header_.name.back() == '\0')) {
        header_.name.pop_back();
    }

    header_.mediaFormat = p[0x3B];
    header_.cartridgeId = readU16Be(p + 0x3C);
    header_.countryCode = p[0x3E];
    header_.version     = p[0x3F];
}

std::string Cartridge::formatName() const {
    switch (format_) {
        case RomFormat::Z64: return "z64 (big-endian)";
        case RomFormat::N64: return "n64 (little-endian word-swapped)";
        case RomFormat::V64: return "v64 (byte-swapped)";
        default: return "unknown";
    }
}

} // namespace n64forge
