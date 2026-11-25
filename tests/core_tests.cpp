#include "n64forge/core/bus.hpp"
#include "n64forge/core/cartridge.hpp"
#include "n64forge/core/cpu.hpp"
#include "n64forge/core/emulator.hpp"
#include "n64forge/core/hle_libultra.hpp"
#include "n64forge/core/hw_registers.hpp"
#include "n64forge/core/ipl.hpp"
#include "n64forge/core/memory.hpp"
#include "n64forge/core/rdp.hpp"
#include "n64forge/core/rsp.hpp"
#include "n64forge/core/rsp_hle.hpp"
#include "n64forge/emulation/audio_ring.hpp"
#include "n64forge/video/vi_interpolation.hpp"
#include "n64forge/video/frame_mailbox.hpp"
#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#if defined(N64FORGE_HAS_ZLIB)
#include <zlib.h>
#endif

int run_recomp_tests();
int runGraphicsApiTests();
int run_core_safety_tests();
int run_rdp_safety_tests();
int run_discord_tests();
int run_frame_ring_tests();

using namespace n64forge;
constexpr u64 Kseg0 = 0xFFFFFFFF80000000ULL;

// The platforms where CI promises a native CPU backend. Keeping this explicit
// means a broken x64 JIT fails Windows x64 CI instead of silently skipping the
// parity assertions.
constexpr bool nativeJitExpected() noexcept {
#if (defined(_WIN32) && defined(_M_X64)) || \
    (defined(__APPLE__) && defined(__aarch64__) && !defined(__arm64e__)) || \
    (defined(__APPLE__) && defined(__x86_64__) && !defined(__arm64e__))
    return true;
#else
    return false;
#endif
}

// The boot handoff is applied from an already-authenticated CIC profile in
// production. This friend hook lets the deterministic tests exercise every
// documented variant without embedding copyrighted IPL3 firmware.
namespace n64forge {
class EmulatorTestAccess {
public:
    static void applyBootProfile(Emulator& emulator, const CicProfile& profile) {
        emulator.applyBootProfile(profile);
    }
};
} // namespace n64forge

// ---------------------------------------------------------------------------
// MIPS instruction encoding helpers
// ---------------------------------------------------------------------------

static constexpr u32 opSpecial = 0x00;
static constexpr u32 opRegimm  = 0x01;
static constexpr u32 opJ       = 0x02;
static constexpr u32 opJal     = 0x03;
static constexpr u32 opBeq     = 0x04;
static constexpr u32 opBne     = 0x05;
static constexpr u32 opBlez    = 0x06;
static constexpr u32 opBgtz    = 0x07;
static constexpr u32 opAddi    = 0x08;
static constexpr u32 opAddiu   = 0x09;
static constexpr u32 opSlti    = 0x0A;
static constexpr u32 opSltiu   = 0x0B;
static constexpr u32 opAndi    = 0x0C;
static constexpr u32 opOri     = 0x0D;
static constexpr u32 opXori    = 0x0E;
static constexpr u32 opLui     = 0x0F;
static constexpr u32 opCop0    = 0x10;
static constexpr u32 opLb      = 0x20;
static constexpr u32 opLh      = 0x21;
static constexpr u32 opLw      = 0x23;
static constexpr u32 opLbu     = 0x24;
static constexpr u32 opLhu     = 0x25;
static constexpr u32 opSb      = 0x28;
static constexpr u32 opSh      = 0x29;
static constexpr u32 opSw      = 0x2B;

[[nodiscard]] u32 encodeR(u32 funct, u32 rs, u32 rt, u32 rd, u32 shamt = 0) {
    return opSpecial | (rs << 21) | (rt << 16) | (rd << 11) | (shamt << 6) | funct;
}
[[nodiscard]] u32 encodeI(u32 opcode, u32 rs, u32 rt, s16 imm) {
    return (opcode << 26) | (rs << 21) | (rt << 16) | (static_cast<u16>(imm));
}
[[nodiscard]] u32 encodeJ(u32 opcode, u32 target) {
    return (opcode << 26) | (target & 0x03FFFFFF);
}

[[nodiscard]] u32 addiu(u32 rt, u32 rs, s16 imm) { return encodeI(opAddiu, rs, rt, imm); }
[[nodiscard]] u32 ori(u32 rt, u32 rs, u16 imm)   { return encodeI(opOri, rs, rt, static_cast<s16>(imm)); }
[[nodiscard]] u32 lui(u32 rt, u16 imm)           { return encodeI(opLui, 0, rt, static_cast<s16>(imm)); }
[[nodiscard]] u32 sw(u32 rt, u32 rs, s16 imm)    { return encodeI(opSw, rs, rt, imm); }
[[nodiscard]] u32 lw(u32 rt, u32 rs, s16 imm)    { return encodeI(opLw, rs, rt, imm); }
[[nodiscard]] u32 lbu(u32 rt, u32 rs, s16 imm)   { return encodeI(opLbu, rs, rt, imm); }
[[nodiscard]] u32 lb(u32 rt, u32 rs, s16 imm)    { return encodeI(opLb, rs, rt, imm); }
[[nodiscard]] u32 beq(u32 rs, u32 rt, s16 off)   { return encodeI(opBeq, rs, rt, off); }
[[nodiscard]] u32 bne(u32 rs, u32 rt, s16 off)   { return encodeI(opBne, rs, rt, off); }
[[nodiscard]] u32 j(u32 target)                  { return encodeJ(opJ, target); }
[[nodiscard]] u32 addu(u32 rd, u32 rs, u32 rt)   { return encodeR(0x21, rs, rt, rd); }
[[nodiscard]] u32 subu(u32 rd, u32 rs, u32 rt)   { return encodeR(0x23, rs, rt, rd); }
[[nodiscard]] u32 and_(u32 rd, u32 rs, u32 rt)   { return encodeR(0x24, rs, rt, rd); }
[[nodiscard]] u32 or_(u32 rd, u32 rs, u32 rt)    { return encodeR(0x25, rs, rt, rd); }
[[nodiscard]] u32 xor_(u32 rd, u32 rs, u32 rt)   { return encodeR(0x26, rs, rt, rd); }
[[nodiscard]] u32 nor_(u32 rd, u32 rs, u32 rt)   { return encodeR(0x27, rs, rt, rd); }
[[nodiscard]] u32 slt(u32 rd, u32 rs, u32 rt)    { return encodeR(0x2A, rs, rt, rd); }
[[nodiscard]] u32 sltu(u32 rd, u32 rs, u32 rt)   { return encodeR(0x2B, rs, rt, rd); }
[[nodiscard]] u32 sll(u32 rd, u32 rt, u32 sh)    { return encodeR(0x00, 0, rt, rd, sh); }
[[nodiscard]] u32 srl(u32 rd, u32 rt, u32 sh)    { return encodeR(0x02, 0, rt, rd, sh); }

// 64-bit / COP0 / COP1 helpers
[[nodiscard]] u32 daddiu(u32 rt, u32 rs, s16 imm) { return encodeI(0x19, rs, rt, imm); }
[[nodiscard]] u32 daddu(u32 rd, u32 rs, u32 rt)   { return encodeR(0x2D, rs, rt, rd); }
[[nodiscard]] u32 dsubu(u32 rd, u32 rs, u32 rt)   { return encodeR(0x2F, rs, rt, rd); }
[[nodiscard]] u32 dsll(u32 rd, u32 rt, u32 sh)    { return encodeR(0x38, 0, rt, rd, sh); }
[[nodiscard]] u32 dsrl(u32 rd, u32 rt, u32 sh)    { return encodeR(0x3A, 0, rt, rd, sh); }
[[nodiscard]] u32 dsra(u32 rd, u32 rt, u32 sh)    { return encodeR(0x3B, 0, rt, rd, sh); }
[[nodiscard]] u32 dsll32(u32 rd, u32 rt, u32 sh)  { return encodeR(0x3C, 0, rt, rd, sh); }
[[nodiscard]] u32 dsrl32(u32 rd, u32 rt, u32 sh)  { return encodeR(0x3E, 0, rt, rd, sh); }
[[nodiscard]] u32 dsra32(u32 rd, u32 rt, u32 sh)  { return encodeR(0x3F, 0, rt, rd, sh); }
[[nodiscard]] u32 ld(u32 rt, u32 rs, s16 imm)     { return encodeI(0x37, rs, rt, imm); }
[[nodiscard]] u32 sd(u32 rt, u32 rs, s16 imm)     { return encodeI(0x3F, rs, rt, imm); }

[[nodiscard]] u32 encodeCop0(u32 rs, u32 rt, u32 rd, u32 funct = 0) {
    return (0x10u << 26) | (rs << 21) | (rt << 16) | (rd << 11) | funct;
}
[[nodiscard]] u32 mtc0(u32 rt, u32 rd)  { return encodeCop0(0x04, rt, rd); }
[[nodiscard]] u32 dmtc0(u32 rt, u32 rd) { return encodeCop0(0x05, rt, rd); }
[[nodiscard]] u32 mfc0(u32 rt, u32 rd)  { return encodeCop0(0x00, rt, rd); }
[[nodiscard]] u32 tlbwi()               { return (0x10u << 26) | (1u << 25) | 0x02; }

[[nodiscard]] u32 encodeCop1(u32 rs, u32 rt, u32 rd, u32 funct = 0) {
    return (0x11u << 26) | (rs << 21) | (rt << 16) | (rd << 11) | funct;
}
[[nodiscard]] u32 mtc1(u32 rt, u32 fs)          { return encodeCop1(0x04, rt, fs); }
[[nodiscard]] u32 dmtc1(u32 rt, u32 fs)         { return encodeCop1(0x05, rt, fs); }
[[nodiscard]] u32 mfc1(u32 rt, u32 fs)          { return encodeCop1(0x00, rt, fs); }
[[nodiscard]] u32 dmfc1(u32 rt, u32 fs)         { return encodeCop1(0x01, rt, fs); }
[[nodiscard]] u32 add_s(u32 fd, u32 fs, u32 ft) { return encodeCop1(0x10, ft, fs, (fd << 6) | 0x00); }
[[nodiscard]] u32 add_d(u32 fd, u32 fs, u32 ft) { return encodeCop1(0x11, ft, fs, (fd << 6) | 0x00); }
[[nodiscard]] u32 bc1t(s16 off)                 { return encodeCop1(0x08, 0x01, 0, static_cast<u16>(off)); }
[[nodiscard]] u32 ctc1(u32 rt, u32 rd)          { return encodeCop1(0x06, rt, rd); }
[[nodiscard]] u32 bc1(u32 sub, s16 off)         { return encodeCop1(0x08, sub, 0, static_cast<u16>(off)); }
[[nodiscard]] u32 cop1Compare(u32 fmt, u32 ft, u32 fs, u32 fn) {
    return (0x11u << 26) | (fmt << 21) | (ft << 16) | (fs << 11) | fn;
}

[[nodiscard]] u32 eret()                        { return (0x10u << 26) | (1u << 25) | 0x18; }

[[nodiscard]] u32 floatRaw(float f) {
    u32 r = 0;
    std::memcpy(&r, &f, sizeof(r));
    return r;
}

// ---------------------------------------------------------------------------
// Helpers to write a program into RDRAM and run it
// ---------------------------------------------------------------------------

void writeProgram(Bus& bus, u32 physAddr, const std::vector<u32>& instructions) {
    for (size_t i = 0; i < instructions.size(); ++i) {
        bus.write32(static_cast<u32>(physAddr + i * 4), instructions[i]);
    }
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

void test_memory() {
    Memory memory;
    memory.write32(0x100, 0x12345678);
    assert(memory.read32(0x100) == 0x12345678);
    memory.write16(0x200, 0xABCD);
    assert(memory.read16(0x200) == 0xABCD);
    memory.write8(0x300, 0x42);
    assert(memory.read8(0x300) == 0x42);
}

void test_cartridge_conversion() {
    constexpr size_t RomSize = 1024 * 1024;
    std::vector<u8> z64(RomSize, 0);
    // Standard big-endian header
    z64[0] = 0x80; z64[1] = 0x37; z64[2] = 0x12; z64[3] = 0x40;
    z64[0x20] = 'T'; z64[0x21] = 'E'; z64[0x22] = 'S'; z64[0x23] = 'T';

    // .v64 = byte-swapped pairs
    std::vector<u8> v64 = z64;
    for (size_t i = 0; i + 1 < v64.size(); i += 2) {
        std::swap(v64[i], v64[i + 1]);
    }

    // .n64 = little-endian word-swapped
    std::vector<u8> n64 = z64;
    for (size_t i = 0; i + 3 < n64.size(); i += 4) {
        std::swap(n64[i], n64[i + 3]);
        std::swap(n64[i + 1], n64[i + 2]);
    }

    Cartridge cartZ, cartV, cartN;
    assert(cartZ.loadFromBuffer(z64, RomFormat::Z64));
    assert(cartV.loadFromBuffer(v64, RomFormat::V64));
    assert(cartN.loadFromBuffer(n64, RomFormat::N64));

    assert(cartZ.format() == RomFormat::Z64);
    assert(cartV.format() == RomFormat::V64);
    assert(cartN.format() == RomFormat::N64);

    assert(cartZ.rom()[0] == 0x80 && cartZ.rom()[1] == 0x37);
    assert(cartV.rom()[0] == 0x80 && cartV.rom()[1] == 0x37);
    assert(cartN.rom()[0] == 0x80 && cartN.rom()[1] == 0x37);
    assert(cartZ.header().name == "TEST");
    assert(cartV.header().name == "TEST");
    assert(cartN.header().name == "TEST");
}

namespace {

std::vector<u8> makeTestRom(std::size_t size, u8 fill = 0) {
    std::vector<u8> rom(size, fill);
    constexpr std::array<u8, 4> Magic{0x80, 0x37, 0x12, 0x40};
    if (rom.size() >= Magic.size()) std::copy(Magic.begin(), Magic.end(), rom.begin());
    constexpr std::string_view Name = "ZIPTEST";
    for (std::size_t i = 0; i < Name.size() && 0x20 + i < rom.size(); ++i)
        rom[0x20 + i] = static_cast<u8>(Name[i]);
    return rom;
}

#if defined(N64FORGE_HAS_ZLIB)
void appendLe16(std::vector<u8>& out, u16 value) {
    out.push_back(static_cast<u8>(value));
    out.push_back(static_cast<u8>(value >> 8));
}

void appendLe32(std::vector<u8>& out, u32 value) {
    for (unsigned shift = 0; shift < 32; shift += 8)
        out.push_back(static_cast<u8>(value >> shift));
}

void setLe16(std::vector<u8>& out, std::size_t pos, u16 value) {
    out[pos] = static_cast<u8>(value);
    out[pos + 1] = static_cast<u8>(value >> 8);
}

struct ZipMember {
    std::string name;
    std::vector<u8> data;
};

std::vector<u8> makeZip(const std::vector<ZipMember>& members, u16 method,
                        bool encrypted = false, bool zip64 = false) {
    struct Entry {
        u32 crc = 0;
        u32 compressedSize = 0;
        u32 offset = 0;
    };
    std::vector<u8> out;
    std::vector<Entry> entries;
    entries.reserve(members.size());

    for (const auto& member : members) {
        const auto crc = crc32(0, member.data.data(),
                               static_cast<uInt>(member.data.size()));
        std::vector<u8> payload;
        if (method == 8) {
            uLongf bound = compressBound(static_cast<uLong>(member.data.size()));
            payload.resize(bound);
            assert(compress2(payload.data(), &bound, member.data.data(),
                             static_cast<uLong>(member.data.size()), Z_DEFAULT_COMPRESSION) == Z_OK);
            payload.resize(bound);
        } else {
            payload = member.data;
        }

        Entry entry;
        entry.crc = crc;
        entry.compressedSize = static_cast<u32>(payload.size());
        entry.offset = static_cast<u32>(out.size());
        entries.push_back(entry);

        appendLe32(out, 0x04034B50); // local file header
        appendLe16(out, 20);
        appendLe16(out, encrypted ? 1 : 0);
        appendLe16(out, method);
        appendLe16(out, 0); // time
        appendLe16(out, 0); // date
        appendLe32(out, entry.crc);
        appendLe32(out, entry.compressedSize);
        appendLe32(out, static_cast<u32>(member.data.size()));
        appendLe16(out, static_cast<u16>(member.name.size()));
        appendLe16(out, 0); // extra length
        out.insert(out.end(), member.name.begin(), member.name.end());
        out.insert(out.end(), payload.begin(), payload.end());
    }

    const auto directoryOffset = static_cast<u32>(out.size());
    for (std::size_t i = 0; i < members.size(); ++i) {
        const auto& member = members[i];
        const auto& entry = entries[i];
        appendLe32(out, 0x02014B50); // central directory header
        appendLe16(out, 20);
        appendLe16(out, 20);
        appendLe16(out, encrypted ? 1 : 0);
        appendLe16(out, method);
        appendLe16(out, 0);
        appendLe16(out, 0);
        appendLe32(out, entry.crc);
        appendLe32(out, entry.compressedSize);
        appendLe32(out, static_cast<u32>(member.data.size()));
        appendLe16(out, static_cast<u16>(member.name.size()));
        appendLe16(out, 0);
        appendLe16(out, 0);
        appendLe16(out, 0);
        appendLe16(out, 0);
        appendLe32(out, 0);
        appendLe32(out, entry.offset);
        out.insert(out.end(), member.name.begin(), member.name.end());
    }
    const auto directorySize = static_cast<u32>(out.size() - directoryOffset);

    const auto eocdOffset = out.size();
    appendLe32(out, 0x06054B50);
    appendLe16(out, 0);
    appendLe16(out, 0);
    appendLe16(out, static_cast<u16>(members.size()));
    appendLe16(out, static_cast<u16>(members.size()));
    appendLe32(out, directorySize);
    appendLe32(out, directoryOffset);
    appendLe16(out, 0); // comment length
    if (zip64) {
        setLe16(out, eocdOffset + 8, 0xFFFF);
        setLe16(out, eocdOffset + 10, 0xFFFF);
    }
    return out;
}

std::filesystem::path writeTempFile(const std::string& suffix,
                                    const std::vector<u8>& bytes) {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto path = std::filesystem::temp_directory_path() /
        ("n64forge-" + std::to_string(stamp) + suffix);
    std::ofstream file(path, std::ios::binary);
    assert(file.is_open());
    file.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
    return path;
}
#endif // N64FORGE_HAS_ZLIB

std::vector<u8> makeByteSwapped(const std::vector<u8>& rom, RomFormat format) {
    auto swapped = rom;
    if (format == RomFormat::V64) {
        for (std::size_t i = 0; i + 1 < swapped.size(); i += 2)
            std::swap(swapped[i], swapped[i + 1]);
    } else if (format == RomFormat::N64) {
        for (std::size_t i = 0; i + 3 < swapped.size(); i += 4) {
            std::swap(swapped[i], swapped[i + 3]);
            std::swap(swapped[i + 1], swapped[i + 2]);
        }
    }
    return swapped;
}

} // namespace

void test_cartridge_files_and_zip() {
#if defined(N64FORGE_HAS_ZLIB)
    // ZIP archives are accepted only when they contain exactly one usable ROM.
    const auto rom = makeTestRom(64 * 1024 + 20); // deliberately not a power of two
    const auto loadZip = [](const std::vector<u8>& bytes, Cartridge& cart) {
        const auto path = writeTempFile(".zip", bytes);
        const bool loaded = cart.load(path);
        std::filesystem::remove(path);
        return loaded;
    };

    Cartridge stored;
    assert(loadZip(makeZip({{"game.z64", rom}}, 0), stored));
    assert(stored.format() == RomFormat::Z64 && stored.rom() == rom);
    assert(stored.header().name == "ZIPTEST");

    Cartridge deflated;
    assert(loadZip(makeZip({{"game.z64", rom}}, 8), deflated));
    assert(deflated.rom() == rom && deflated.size() == rom.size());

    Cartridge byteSwapped;
    assert(loadZip(makeZip({{"game.v64", makeByteSwapped(rom, RomFormat::V64)}}, 8), byteSwapped));
    assert(byteSwapped.format() == RomFormat::V64 && byteSwapped.rom() == rom);

    Cartridge unnamed;
    assert(loadZip(makeZip({{"payload.bin", rom}}, 0), unnamed));
    assert(unnamed.rom() == rom);

    Cartridge rejected;
    assert(!loadZip(makeZip({{"a.z64", rom}, {"b.z64", rom}}, 0), rejected));
    assert(!loadZip(makeZip({{"game.z64", rom}}, 0, true), rejected));
    assert(!loadZip(makeZip({{"game.z64", rom}}, 12), rejected));
    assert(!loadZip(makeZip({{"game.z64", rom}}, 0, false, true), rejected));

    // Direct files use the same normalization path as archived ROMs.
    for (const auto format : {RomFormat::Z64, RomFormat::V64, RomFormat::N64}) {
        const char* extension = format == RomFormat::Z64 ? ".z64"
            : format == RomFormat::V64 ? ".v64" : ".n64";
        const auto path = writeTempFile(extension, makeByteSwapped(rom, format));
        Cartridge direct;
        assert(direct.load(path));
        assert(direct.format() == format && direct.rom() == rom);
        std::filesystem::remove(path);
    }
#endif // N64FORGE_HAS_ZLIB

    // Header-only images and arbitrary, non-power-of-two lengths are valid.
    std::vector<u8> tiny = makeTestRom(0x40);
    Cartridge tinyCart;
    assert(tinyCart.loadFromBuffer(std::move(tiny)));
    assert(tinyCart.size() == 0x40 && tinyCart.header().name == "ZIPTEST");
    assert(!Cartridge().loadFromBuffer(makeByteSwapped(makeTestRom(65), RomFormat::V64),
                                       RomFormat::V64));
    assert(!Cartridge().loadFromBuffer(makeByteSwapped(makeTestRom(67), RomFormat::N64),
                                       RomFormat::N64));
    assert(!Cartridge().loadFromBuffer(makeTestRom(Cartridge::MaxMappedSize + 1)));

    // Reads past the end of a mounted ROM return PI open bus, not RAM data.
    Cartridge small;
    assert(small.loadFromBuffer(makeTestRom(0x100)));
    Bus bus;
    bus.mountCartridge(&small);
    assert(bus.read8(Cartridge::BusBase) == 0x80);
    assert(bus.read32(Cartridge::BusBase + 0x100) == 0x01000100);
}

void test_bus_mapping() {
    Bus bus;
    bus.write32(0x03F80008, 0x18082838);
    assert(bus.read32(0x03F00008) == 0x18082838 && bus.read32(0x03F01808) == 0x18082838);
    bus.write32(0x03F00808, 0x12345678);
    assert(bus.read32(0x03F00808) == 0x12345678 && bus.read32(0x03F00008) == 0x18082838);
    assert(bus.read32(0x03F02008) == 0);
    Rsp rsp;
    bus.attachRsp(&rsp);
    rsp.setBus(&bus);

    bus.write32(0x100, 0xAABBCCDD);
    assert(bus.read32(0x100) == 0xAABBCCDD);

    // SP DMEM and IMEM
    bus.write32(0x04000000, 0x11111111);
    assert(bus.read32(0x04000000) == 0x11111111);
    bus.write32(0x04001000, 0x22222222);
    assert(bus.read32(0x04001000) == 0x22222222);

    // SP status register (not RSP memory)
    bus.write32(0x04040010, 0x00000001); // clear halt
    assert((bus.read32(0x04040010) & 0x0001) == 0);

    // PIF RAM
    bus.write32(0x1FC007FC, 0x33333333);
    assert(bus.read32(0x1FC007FC) == 0x33333333);
}

void test_is_viewer() {
    Bus bus;
    std::vector<std::string> logged;
    bus.setIsViewerLogCallback([&](std::string_view line) { logged.emplace_back(line); });

    constexpr u32 Base = IsViewer::Base;
    bus.write32(Base + IsViewer::MagicOffset, 0x49533634u); // "IS64"
    assert(bus.read32(Base + IsViewer::MagicOffset) == 0x49533634u);
    assert(bus.isViewer().magicPresent());

    const auto writeText = [&](u32 offset, const char* text) {
        for (const char* p = text; *p != '\0'; ++p)
            bus.write8(Base + IsViewer::BufferOffset + offset++, static_cast<u8>(*p));
    };
    const auto expectLine = [&](const char* text) {
        assert(!logged.empty());
        assert(logged.back() == text);
    };

    writeText(0, "FIRST");
    bus.write16(Base + IsViewer::FlushOffset, 5);
    expectLine("FIRST");

    writeText(0, "SECOND");
    bus.write16(Base + IsViewer::FlushOffset, 6);
    expectLine("SECOND");

    // A 32-bit store is split into halfwords; the count occupies the halfword
    // that lands at +0x16.
    writeText(0, "THIRD");
    bus.write32(Base + IsViewer::FlushOffset - 2, 5);
    expectLine("THIRD");

    // libultra treats +0x16 as the end pointer and +0x04 as the current
    // buffer-relative pointer. A nonzero read pointer starts there.
    writeText(16, "OFFSET");
    bus.write16(Base + IsViewer::ReadPointerOffset, 16);
    bus.write16(Base + IsViewer::FlushOffset, 22);
    expectLine("OFFSET");
    assert(bus.isViewer().flushCount() == 4);
    assert(bus.read16(Base + IsViewer::ReadPointerOffset) == 0);

    // KSEG0 and KSEG1 aliases share the same PI physical window.
    Cpu cpu;
    cpu.setPc(Kseg0 + 0x200);
    writeProgram(bus, 0x200, {
        lui(1, 0xB3FF), // KSEG1 alias of 0x13FF0000
        lui(2, 0x4953),
        ori(2, 2, 0x3634),
        sw(2, 1, 0),
        lw(3, 1, 0),
    });
    for (int i = 0; i < 5; ++i) cpu.step(bus);
    assert(cpu.gpr()[3] == 0x49533634u);

    // Register offsets are masked to the 64 KiB PI window. A 16-bit access at
    // the final byte wraps into the first byte of the same window.
    IsViewer wrapped;
    wrapped.write32(0xFFFF, 0x11223344);
    assert(wrapped.read8(0xFFFF) == 0x11);
    assert(wrapped.read8(0x0000) == 0x22);
    assert(wrapped.read16(0xFFFF) == 0x1122);

    bus.reset();
    assert(bus.isViewer().lines().empty());
    assert(!bus.isViewer().magicPresent());
    assert(bus.isViewer().flushCount() == 0);
}

void test_cpu_alu() {
    Bus bus;
    Cpu cpu;
    cpu.setPc(Kseg0 + 0x100);

    // r1 = 10; r2 = 20; r3 = r1 + r2; r4 = r2 - r1; r5 = r1 | r2
    writeProgram(bus, 0x100, {
        addiu(1, 0, 10),
        addiu(2, 0, 20),
        addu(3, 1, 2),
        subu(4, 2, 1),
        ori(5, 1, 0xF0),
        and_(6, 1, 2),
    });

    for (int i = 0; i < 6; ++i) cpu.step(bus);

    assert(cpu.gpr()[1] == 10);
    assert(cpu.gpr()[2] == 20);
    assert(cpu.gpr()[3] == 30);
    assert(cpu.gpr()[4] == 10);
    assert(cpu.gpr()[5] == (10 | 0xF0));
    assert(cpu.gpr()[6] == (10 & 20));
}

void test_cpu_shift_divide_edges() {
    Bus bus;
    Cpu cpu;
    constexpr u64 Base = 0xFFFFFFFF80000000ULL;
    const auto run = [&](bool cached, u32 instr, u64 rs, u64 rt) {
        cpu.reset();
        cpu.setPc(Base + 0x400);
        bus.write64(0x200, rs);
        bus.write64(0x208, rt);
        writeProgram(bus, 0x400, {lui(4, 0x8000), ld(1, 4, 0x200), ld(2, 4, 0x208), instr});
        for (int i = 0; i < 4; ++i) {
            if (cached) cpu.stepCached(bus);
            else cpu.step(bus);
        }
        assert(!cpu.exceptionTrace().observed);
    };
    constexpr u64 MinusOne = ~u64{0};
    constexpr u64 MinWord = 0xFFFFFFFF80000000ULL, MinDword = 0x8000000000000000ULL;
    struct DivCase { u32 op; u64 rs, rt, lo, hi; };
    const DivCase cases[]{
        {0x1A, 7, 0, MinusOne, 7}, {0x1A, MinWord, 0, 1, MinWord},
        {0x1A, MinWord, MinusOne, MinWord, 0},
        {0x1B, MinWord, 0, MinusOne, MinWord}, {0x1B, MinWord, 1, MinWord, 0},
        {0x1B, MinWord, MinusOne, 0, MinWord},
        {0x1E, 7, 0, MinusOne, 7}, {0x1E, MinDword, 0, 1, MinDword},
        {0x1E, MinDword, MinusOne, MinDword, 0},
        {0x1F, MinDword, 0, MinusOne, MinDword}, {0x1F, MinDword, 1, MinDword, 0},
    };
    for (bool cached : {false, true}) {
        run(cached, encodeR(3, 0, 2, 3), 0, 0x0000000180000000ULL);
        assert(cpu.gpr()[3] == MinWord);
        run(cached, encodeR(3, 0, 2, 3, 1), 0, 0x0000000080000000ULL);
        assert(cpu.gpr()[3] == 0x40000000);
        run(cached, encodeR(7, 1, 2, 3), 33, 0x0000000100000000ULL);
        assert(cpu.gpr()[3] == MinWord);
        run(cached, encodeR(7, 1, 2, 3), 32, 0x0000000080000000ULL);
        assert(cpu.gpr()[3] == MinWord);
        for (const auto& c : cases) {
            run(cached, encodeR(c.op, 1, 2, 0), c.rs, c.rt);
            assert(cpu.lo() == c.lo && cpu.hi() == c.hi);
        }
    }
}

void test_cpu_load_store() {
    Bus bus;
    Cpu cpu;
    cpu.setPc(Kseg0 + 0x200);

    // Store 0x12345678 at 0x400, load back into r2, then load byte
    writeProgram(bus, 0x200, {
        lui(1, 0x1234),
        ori(1, 1, 0x5678),
        lui(5, 0x8000),
        sw(1, 5, 0x400),
        lw(2, 5, 0x400),
        lbu(3, 5, 0x400),
        lb(4, 5, 0x400),
    });

    for (int i = 0; i < 7; ++i) cpu.step(bus);

    assert(cpu.gpr()[1] == 0x12345678);
    assert(cpu.gpr()[2] == 0x12345678);
    assert(cpu.gpr()[3] == 0x12); // big-endian: MSB at base address
    assert(cpu.gpr()[4] == 0x12);
}

void test_cpu_rdram_aliases() {
    Bus bus;
    Cpu cpu;
    writeProgram(bus, 0, {
        lui(1, 0x807F), ori(1, 1, 0xFFF8), // last aligned 64-bit RDRAM word
        lui(2, 0xA07F), ori(2, 2, 0xFFF8),
        lui(3, 0x1234), ori(3, 3, 0x5678),
        sd(3, 1, 0), ld(4, 2, 0),
        lui(5, 0xA440), sw(3, 5, 4), lw(6, 5, 4), // VI_ORIGIN MMIO fallback
    });
    cpu.setPc(0xFFFFFFFFA0000000ULL);
    for (int i = 0; i < 11; ++i) cpu.step(bus);
    assert(cpu.gpr()[4] == 0x12345678);
    assert(bus.read64(Memory::RdramSize - 8) == 0x12345678);
    assert(cpu.gpr()[6] == 0x345678 && bus.hw().vi().origin == 0x345678);
    // A write from the physical bus must be visible to either fetch alias.
    bus.write32(0, addiu(4, 0, 19));
    cpu.setPc(0xFFFFFFFF80000000ULL);
    cpu.step(bus);
    assert(cpu.gpr()[4] == 19);
    bus.reset();
    bus.write32(0, addiu(4, 0, 31));
    cpu.setPc(0xFFFFFFFFA0000000ULL);
    cpu.step(bus);
    assert(cpu.gpr()[4] == 31);
}

void test_cpu_big_endian_merge_accesses() {
    Bus bus;
    Cpu cpu;
    const auto ins = [](u32 op, u32 rt, u32 offset) {
        return (op << 26) | (1u << 21) | (rt << 16) | offset;
    };
    constexpr u64 initial = 0x12345678A1B2C3D4ULL;
    constexpr u64 source = 0x81FEA56789ABCDEFULL;
    bus.write64(0x2000, source);
    bus.write64(0x2008, initial);
    for (u32 size : {4u, 8u}) {
        for (bool left : {true, false}) {
            const u32 load = size == 4 ? (left ? 0x22 : 0x26) : (left ? 0x1A : 0x1B);
            const u32 store = size == 4 ? (left ? 0x2A : 0x2E) : (left ? 0x2C : 0x2D);
            for (u32 offset = 0; offset < size; ++offset) {
                bus.write64(0x2010, source);
                writeProgram(bus, 0, {
                    0x3C018000,                 // lui r1,0x8000
                    ins(0x37, 2, 0x2008),      // ld r2,initial
                    ins(load, 2, 0x2000 + offset),
                    ins(0x37, 3, 0x2008),
                    ins(store, 3, 0x2010 + offset),
                });
                cpu.setPc(0xFFFFFFFF80000000ULL);
                for (int i = 0; i < 5; ++i) cpu.step(bus);
                u64 expectedReg = initial;
                u64 expectedMemory = source;
                const u32 first = left ? offset : 0;
                const u32 last = left ? size - 1 : offset;
                for (u32 lane = first; lane <= last; ++lane) {
                    const u32 regLane = left ? lane - first : size - 1 - (last - lane);
                    const u32 regShift = (size - 1 - regLane) * 8;
                    const u32 memShift = (7 - lane) * 8;
                    expectedReg = (expectedReg & ~(0xFFULL << regShift)) |
                                  (((source >> memShift) & 255) << regShift);
                    expectedMemory = (expectedMemory & ~(0xFFULL << memShift)) |
                                     (((initial >> regShift) & 255) << memShift);
                }
                if (size == 4 && (left || offset == 3))
                    expectedReg = static_cast<s64>(static_cast<s32>(expectedReg));
                assert(cpu.gpr()[2] == expectedReg);
                assert(bus.read64(0x2010) == expectedMemory);
            }
        }
    }
    // Copy a packed four-byte record at an unaligned address. Both surrounding
    // bytes must survive the store pair, just as in a guest DMA packet builder.
    bus.write64(0x2020, 0xDEADBEEFCAFEBABEULL);
    writeProgram(bus, 0, {0x3C018000, ins(0x22, 2, 0x2001), ins(0x26, 2, 0x2004),
                         ins(0x2A, 2, 0x2022), ins(0x2E, 2, 0x2025)});
    cpu.setPc(0xFFFFFFFF80000000ULL);
    for (int i = 0; i < 5; ++i) cpu.step(bus);
    assert(cpu.gpr()[2] == 0xFFFFFFFFFEA56789ULL);
    assert(bus.read64(0x2020) == 0xDEADFEA56789BABEULL);
}

void test_cpu_branch_delay_slot() {
    Bus bus;
    Cpu cpu;
    cpu.setPc(Kseg0 + 0x300);

    // r1 = 5
    // loop:
    //   addiu r1, r1, -1
    //   bne r1, r0, loop
    //   nop
    // sw r1, 0x500(r2)
    writeProgram(bus, 0x300, {
        addiu(1, 0, 5),
        addiu(1, 1, -1),          // loop target
        bne(1, 0, -2),            // branch back to addiu
        lui(2, 0x8000),           // delay slot sets data base
        sw(1, 2, 0x500),
    });

    // 1 (addiu init) + 5 iterations * 3 instructions (addiu, bne, nop)
    for (int i = 0; i < 1 + 5 * 3; ++i) cpu.step(bus);
    assert(cpu.gpr()[1] == 0);

    // Execute the store
    cpu.step(bus);
    assert(bus.read32(0x500) == 0);
}

void test_cpu_jump_and_link() {
    Bus bus;
    Cpu cpu;
    cpu.setPc(Kseg0 + 0x400);

    // jal target
    // nop
    // ... target: addiu r1, r0, 7
    // jr r31
    // nop
    const u32 target = (0x500 >> 2);
    writeProgram(bus, 0x400, {
        encodeJ(opJal, target),
        0x00000000,
        addiu(1, 0, 99),
        0x00000000,
    });
    writeProgram(bus, 0x500, {
        addiu(1, 0, 7),
        encodeR(0x08, 31, 0, 0), // jr r31
        0x00000000,
    });

    cpu.step(bus); // jal
    cpu.step(bus); // delay nop
    cpu.step(bus); // addiu at target
    cpu.step(bus); // jr r31
    cpu.step(bus); // delay nop
    cpu.step(bus); // instruction after jal (addiu r1, 0, 99)

    assert(cpu.gpr()[1] == 99);
    assert(cpu.gpr()[31] == Kseg0 + 0x408);
}

void test_cpu_64_cop_tlb() {
    Bus bus;
    Cpu cpu;
    cpu.setPc(Kseg0 + 0x400);

    // Program tests 64-bit ops, COP0, COP1, TLB and ERET.
    writeProgram(bus, 0x400, {
        // --- 64-bit integer ops ---
        daddiu(1, 0, -1),          // r1 = 0xFFFFFFFFFFFFFFFF
        daddiu(2, 0, 1),           // r2 = 1
        daddu(3, 1, 2),            // r3 = 0
        dsubu(4, 2, 1),            // r4 = 2
        dsll32(5, 2, 0),           // r5 = 0x1_0000_0000
        dsrl(6, 5, 16),            // r6 = 0x1_0000
        dsra32(7, 1, 0),           // r7 = 0xFFFFFFFFFFFFFFFF (arith shift of -1)

        // --- Enable COP1 (Status.CU1 = 1) ---
        mfc0(20, 12),              // r20 = Status
        lui(21, 0x2000),           // r21 = 1 << 29
        or_(20, 20, 21),           // r20 |= CU1
        mtc0(20, 12),              // Status = r20

        // --- COP1 single-precision add ---
        lui(8, 0x3F80),            // r8 = 0x3F800000 (1.0f bits)
        mtc1(8, 0),                // f0 = 1.0f
        add_s(2, 0, 0),            // f2 = f0 + f0 = 2.0f
        mfc1(9, 2),                // r9 = raw float bits of f2

        // --- COP1 double-precision add ---
        lui(10, 0x4000),           // r10 = 0x40000000
        dsll32(10, 10, 0),         // r10 = 0x4000000000000000 (2.0 double)
        dmtc1(10, 4),              // f4 (double) = 2.0
        add_d(6, 4, 4),            // f6 (double) = 4.0
        dmfc1(11, 6),              // r11 = raw double bits of f6

        // --- TLB setup and mapped access ---
        // Map VPN2=0x60000 (KSSEG base 0xC0000000): side0 -> pfn 0x1 (phys 0x1000),
        // side1 -> pfn 0x2 (phys 0x2000).
        // EntryLo: (pfn << 6) | dirty | valid | global
        daddiu(12, 0, 0x0047),     // EntryLo0 = pfn1, dirty, valid, global = 0x47
        mtc0(12, 2),               // C0_EntryLo0
        daddiu(13, 0, 0x0087),     // EntryLo1 = pfn2, dirty, valid, global = 0x87
        mtc0(13, 3),               // C0_EntryLo1
        mtc0(0, 0),                // C0_Index = 0
        lui(22, 0xC000),           // r22 = 0xFFFFFFFFC0000000 (KSSEG base, VPN2=0x7FE000)
        dmtc0(22, 10),             // C0_EntryHi (full 64 bits required for VPN2)
        mtc0(0, 5),                // C0_PageMask = 0 (4 KiB pages)
        tlbwi(),                   // write TLB entry 0

        lui(14, 0xDEAD),           // r14 = 0x00000000DEAD0000
        ori(14, 14, 0xBEEF),       // r14 = 0xDEADBEEF
        sw(14, 22, 0),             // store at vaddr 0xFFFFFFFFC0000000 -> phys 0x1000
        lw(15, 22, 0x1000),        // load from vaddr 0xFFFFFFFFC0001000 -> phys 0x2000 (should be 0)
        lui(16, 0xCAFE),           // r16 = 0x00000000CAFE0000
        ori(16, 16, 0xBABE),       // r16 = 0xCAFEBABE
        sw(16, 22, 0x1000),        // store at vaddr 0xFFFFFFFFC0001000 -> phys 0x2000
        lw(17, 22, 0),             // load vaddr 0xFFFFFFFFC0000000 -> phys 0x1000 = 0xDEADBEEF

        // --- ERET ---
        lui(18, 0x8000), ori(18, 18, 0x500),
        dmtc0(18, 14),             // EPC = KSEG0 + 0x500
        eret(),
        0x00000000,
    });

    // Place a marker instruction at the EPC target.
    writeProgram(bus, 0x500, {
        daddiu(19, 0, 0x1234),
        0x00000000,
    });

    // Run enough steps to execute the whole sequence.
    for (int i = 0; i < 60; ++i) cpu.step(bus);

    assert(cpu.gpr()[3] == 0);
    assert(cpu.gpr()[4] == 2);
    assert(cpu.gpr()[5] == 0x100000000ULL);
    assert(cpu.gpr()[6] == 0x10000);
    assert(cpu.gpr()[7] == 0xFFFFFFFFFFFFFFFFULL);

    assert(cpu.gpr()[9] == 0x40000000);          // 2.0f raw
    assert(cpu.gpr()[11] == 0x4010000000000000ULL); // 4.0 double raw

    assert(cpu.gpr()[15] == 0);
    assert(cpu.gpr()[17] == 0xFFFFFFFFDEADBEEFULL);
    assert(bus.read32(0x2000) == 0xCAFEBABE);

    assert(cpu.gpr()[19] == 0x1234);
    assert((cpu.cop0()[12] & 0x2) == 0); // EXL cleared
}

void test_vr4300_cop2_transfer_latch() {
    const auto cop2 = [](u32 rs, u32 rt, u32 rd) {
        return (0x12u << 26) | (rs << 21) | (rt << 16) | (rd << 11);
    };
    for (const bool cached : {false, true}) {
        Bus bus;
        Cpu cpu;
        const auto step = [&] { if (cached) cpu.stepCached(bus); else cpu.step(bus); };
        const auto run = [&](int count) {
            cpu.setPc(Kseg0 + 0x400);
            while (count--) step();
        };
        writeProgram(bus, 0x400, {beq(0, 0, 1), cop2(0, 3, 0)});
        run(2);
        assert((cpu.cop0()[13] & 0xB000007C) == 0xA000002C);
        assert(cpu.cop0()[14] == Kseg0 + 0x400 && cpu.gpr()[3] == 0);

        cpu.reset();
        writeProgram(bus, 0x400, {
            lui(1, 0x6400), mtc0(1, 12), lui(2, 0xDEAD), ori(2, 2, 0xBEEF),
            dsll(2, 2, 2), cop2(5, 2, 7), cop2(1, 3, 9), cop2(0, 4, 1),
            encodeI(0x32, 0, 5, 1), // enabled LWC2 must raise RI before address lookup
        });
        run(9);
        assert(cpu.gpr()[3] == 0xFFFFFFFF7AB6FBBCULL && cpu.gpr()[4] == 0x7AB6FBBCULL);
        assert((cpu.cop0()[13] & 0x3000007C) == 0x20000028);
        assert(cpu.cop0()[14] == Kseg0 + 0x420 && cpu.cop0()[8] == 0);
    }
}

void test_vr4300_unaligned_access() {
    constexpr u64 Address = Kseg0 + 0x2003;
    for (const bool cached : {false, true}) {
        for (const u32 op : {0x21u, 0x23u, 0x29u, 0x2Bu, 0x31u, 0x37u, 0x3Fu}) {
            Bus bus;
            Cpu cpu;
            cpu.setPc(Kseg0 + 0x400);
            writeProgram(bus, 0x400, {
                lui(1, 0x8000), ori(1, 1, 0x2003),
                addiu(2, 0, -1), dmtc0(2, 4), dmtc0(2, 20),
                beq(0, 0, 1), encodeI(op, 1, 2, 0),
            });
            bus.write64(0x2000, 0x123456789ABCDEF0ULL);
            for (int n = 0; n < 7; ++n) {
                if (cached) cpu.stepCached(bus);
                else cpu.step(bus);
            }
            const bool store = op == 0x29 || op == 0x2B || op == 0x3F;
            assert(cpu.pc() == Kseg0 + 0x180 && cpu.cop0()[14] == Kseg0 + 0x414);
            assert(cpu.cop0()[13] == (0x80000000 | (store ? 5 : 4) << 2));
            assert(cpu.cop0()[8] == Address);
            assert(cpu.cop0()[4] == (0xFFFFFFFFFF800000ULL | ((Address >> 9) & 0x7FFFF0)));
            assert(cpu.cop0()[20] == (0xFFFFFFFE00000000ULL | ((Address >> 9) & 0x7FFFFFF0) |
                                      ((Address >> 62) << 31)));
            assert(cpu.gpr()[2] == ~0ULL && cpu.fpr()[2] == 0);
            assert(bus.read64(0x2000) == 0x123456789ABCDEF0ULL);
        }
    }
}

void test_cpu_quiet_self_branch() {
    Bus batchedBus, scalarBus;
    Cpu batched, scalar;
    const auto setup = [](Bus& bus, Cpu& cpu) {
        writeProgram(bus, 0x400, {beq(0, 0, -1), 0});
        cpu.setPc(Kseg0 + 0x400);
    };
    setup(batchedBus, batched);
    setup(scalarBus, scalar);
    const auto elapsed = batched.runQuietBlock(batchedBus, 4095);
    assert(elapsed == 4094 && batched.cacheStats().branchLinksTaken == 0);
    for (u32 i = 0; i < elapsed; ++i) scalar.step(scalarBus);
    assert(batched.pc() == scalar.pc() && batched.gpr() == scalar.gpr() &&
           batched.cop0() == scalar.cop0() &&
           batched.instructionsDispatched() == scalar.instructionsDispatched());

    // Replacing the delay slot must invalidate the cached block. This loop
    // changes state every iteration and cannot take the same shortcut.
    batchedBus.write32(0x404, addiu(3, 3, 1));
    scalarBus.write32(0x404, addiu(3, 3, 1));
    const auto changed = batched.runQuietBlock(batchedBus, 32);
    assert(changed == 32 && batched.gpr()[3] == 16);
    for (u32 i = 0; i < changed; ++i) scalar.step(scalarBus);
    assert(batched.pc() == scalar.pc() && batched.gpr() == scalar.gpr() && batched.cop0() == scalar.cop0());
}

void test_cpu_kseg2_overlay_remap() {
    constexpr u64 Overlay = 0xFFFFFFFFE0000000ULL;
    constexpr u32 Tlbwr = 0x42000006, Tlbr = 0x42000001, Tlbp = 0x42000008;
    constexpr u32 JalrOverlay = (1u << 21) | (31u << 11) | 9;
    for (const auto mode : {CpuExecutionMode::Interpreter, CpuExecutionMode::Cached, CpuExecutionMode::Jit}) {
        Bus bus;
        Cpu cpu;
        const bool cached = mode != CpuExecutionMode::Interpreter;
        if (mode == CpuExecutionMode::Jit && !cpu.setJitEnabled(true)) continue;
        const auto step = [&] { if (cached) cpu.stepCached(bus); else cpu.step(bus); };
        const auto run = [&](std::initializer_list<u32> code) {
            writeProgram(bus, 0x400, code);
            cpu.setPc(Kseg0 + 0x400);
            for (u32 i = 0; i < code.size();) {
                const auto n = mode == CpuExecutionMode::Jit ?
                    cpu.runQuietBlock(bus, static_cast<u32>(code.size()) - i) : 0;
                if (n) i += n;
                else { step(); ++i; }
            }
        };
        for (u32 page : {2u, 3u, 4u}) {
            writeProgram(bus, page * 4096, {daddiu(6, 0, page), encodeR(8, 31, 0, 0), sw(6, 1, 0x24)});
            bus.write32(page * 4096 + 0x20, 0x1000 + page);
        }
        const auto executeOverlay = [&](u32 page) {
            assert(cpu.pc() == Overlay);
            assert(cpu.runQuietBlock(bus, 3) == 0); // Mapped execution retains the TLB path.
            step(); step(); step(); // Includes the mapped store in JR's delay slot.
            assert(cpu.gpr()[6] == page && cpu.pc() == cpu.gpr()[31]);
            assert(bus.read32(page * 4096 + 0x24) == page);
        };
        run({lui(1, 0xE000), mtc0(1, 10), ori(9, 0, 31), mtc0(9, 0),
             ori(3, 0, 0x87), mtc0(3, 2), ori(3, 0, 1), mtc0(3, 3), tlbwi(),
             lw(4, 1, 0x20), lw(5, 1, 0x20), JalrOverlay, 0});
        assert(cpu.gpr()[4] == 0x1002 && cpu.gpr()[5] == 0x1002);
        executeOverlay(2);
        assert(cpu.tlb()[31].region == 3);

        run({ori(3, 0, 0xC7), mtc0(3, 2), lw(4, 1, 0x20), tlbwi(),
             lw(5, 1, 0x20), JalrOverlay, 0});
        assert(cpu.gpr()[4] == 0x1002 && cpu.gpr()[5] == 0x1003);
        executeOverlay(3);
        run({ori(3, 0, 0x107), mtc0(3, 2), Tlbwr, lw(5, 1, 0x20), JalrOverlay, 0});
        assert(cpu.gpr()[5] == 0x1004);
        executeOverlay(4);
        run({Tlbr, Tlbp});
        assert(cpu.cop0()[10] == 0xC00000FFE0000000ULL && cpu.cop0()[0] == 31);

        // Same VPN2 and ASID, distinct extended region: neither probe nor data
        // translation may alias the region-3 global entry.
        run({ori(3, 0, 0x80), mtc0(3, 12), dsll(2, 1, 24), dsrl(2, 2, 24),
             dmtc0(2, 10), Tlbp, lw(5, 2, 0x20)});
        assert(cpu.cop0()[0] == 0x80000000 && cpu.cop0()[8] == 0x000000FFE0000020ULL);
        assert(cpu.gpr()[5] == 0x1004 && cpu.exceptionTrace().counts[2] == 1);

        // A word-form EPC write must not retain the high bits of the
        // direct-segment fault PC; ERET must resume the mapped overlay.
        run({mtc0(1, 14), eret()});
        assert(cpu.cop0()[14] == Overlay && cpu.pc() == Overlay);
    }
}

void test_cpu_native_alu_and_smc() {
    Bus compiledBus, scalarBus;
    Cpu compiled, scalar;
    const bool native = compiled.setJitEnabled(true);
    assert(native == nativeJitExpected());
    const auto binary = [](u32 fn, u32 dest, u32 a, u32 b) { return encodeR(fn, a, b, dest); };
    constexpr u32 Start = 0x400;
    const u32 code[] = {
        lui(1, 0x8001), ori(2, 1, 0x7801), daddiu(3, 0, -1),
        encodeI(9, 3, 4, -32000), daddiu(5, 3, 16000), encodeI(9, 1, 6, 0x7fff),
        binary(0x21, 7, 1, 2), binary(0x23, 8, 4, 5), binary(0x24, 9, 3, 6),
        binary(0x25, 10, 7, 8), binary(0x26, 11, 9, 10), binary(0x27, 12, 11, 0),
        encodeR(0, 0, 2, 13, 3), encodeR(2, 0, 2, 14, 8), encodeR(3, 0, 2, 15, 15),
        dsll32(16, 2, 1), dsra32(17, 16, 5),
        binary(0x2a, 18, 3, 0), binary(0x2b, 19, 3, 0),
        binary(0x18, 0, 2, 3), encodeR(0x10, 0, 0, 20), encodeR(0x12, 0, 0, 21),
        binary(0x19, 0, 2, 3), encodeR(0x10, 0, 0, 22), encodeR(0x12, 0, 0, 23),
        binary(0x2d, 24, 16, 5), binary(0x2f, 25, 17, 3),
        encodeI(0x0c, 2, 26, 0xff01), encodeI(0x0e, 3, 27, 0x8001),
        daddiu(0, 3, -1), 0,
        (2u << 26) | (Start >> 2), daddiu(28, 28, 1),
    };
    for (u32 i = 0; i < std::size(code); ++i) {
        compiledBus.write32(Start + i * 4, code[i]);
        scalarBus.write32(Start + i * 4, code[i]);
    }
    compiled.setPc(Kseg0 + Start);
    scalar.setPc(Kseg0 + Start);
    for (u32 repeat = 0; repeat < 4; ++repeat) {
        for (u32 i = 0; i < std::size(code);) {
            const auto budget = std::min<u32>(std::size(code) - i, repeat == 2 ? 3 : 16);
            auto n = compiled.runQuietBlock(compiledBus, budget);
            if (!n) { compiled.stepCached(compiledBus); n = 1; }
            assert(n <= budget);
            for (u32 j = 0; j < n; ++j) scalar.step(scalarBus);
            i += n;
            assert(compiled.pc() == scalar.pc() && compiled.gpr() == scalar.gpr() &&
                   compiled.hi() == scalar.hi() && compiled.lo() == scalar.lo() &&
                   compiled.cop0() == scalar.cop0() &&
                   compiled.instructionsDispatched() == scalar.instructionsDispatched());
        }
        assert(compiled.pc() == Kseg0 + Start);
        if (repeat == 1) {
            // Modify the already-executed code line without resetting the PC,
            // CPU, or cache epoch. The next execution must use the new word.
            const auto epoch = compiled.rdramGenerationEpoch();
            compiledBus.write32(Start, lui(1, 0x7ffe));
            scalarBus.write32(Start, lui(1, 0x7ffe));
            assert(compiled.rdramGenerationEpoch() == epoch);
        }
        if (repeat >= 2) assert(compiled.gpr()[1] == 0x7ffe0000);
    }
    assert(native ? compiled.jitSteps() >= 32 : compiled.jitSteps() == 0);
}

void test_cpu_native_memory_fastpath() {
    Bus compiledBus, scalarBus;
    Cpu compiled, scalar;
    const bool native = compiled.setJitEnabled(true);
    constexpr u32 Start = 0x400;
    const std::vector<u32> code{
        lui(1, 0x8001), ori(2, 1, 0x0080), lui(3, 0x1122), ori(3, 3, 0x3344),
        encodeI(opSw, 2, 3, 0), encodeI(opLw, 2, 4, 0),
        encodeI(opLbu, 2, 5, 0), encodeI(opLb, 2, 6, 0),
        encodeI(opLhu, 2, 7, 2), encodeI(opLh, 2, 8, 2),
        daddiu(9, 0, 0xAA), encodeI(opSb, 2, 9, 1),
        daddiu(10, 0, 0xBBCC), encodeI(opSh, 2, 10, 4),
        encodeI(opLw, 2, 11, 0),
        lui(13, 0xa001), ori(13, 13, 0x0080), daddiu(14, 0, -1),
        encodeI(0x3f, 2, 14, 8), encodeI(0x37, 13, 15, 8),
        encodeI(0x27, 13, 16, 8), beq(0, 0, -22), daddiu(12, 12, 1),
    };
    for (auto* bus : {&compiledBus, &scalarBus}) writeProgram(*bus, Start, code);
    compiled.setPc(Kseg0 + Start);
    scalar.setPc(Kseg0 + Start);
    for (u32 done = 0; done < 100;) {
        u32 steps = compiled.runQuietBlock(compiledBus, 100 - done);
        if (!steps) { compiled.stepCached(compiledBus); steps = 1; }
        assert(steps <= 100 - done);
        for (u32 i = 0; i < steps; ++i) scalar.step(scalarBus);
        done += steps;
        assert(compiled.gpr() == scalar.gpr() && compiled.pc() == scalar.pc());
    }
    assert(native ? compiled.jitSteps() > 0 : compiled.jitSteps() == 0);
    assert(compiled.gpr()[15] == ~u64{0} && compiled.gpr()[16] == 0xffffffffu);
    for (u32 offset = 0; offset < 16; ++offset) {
        const u32 address = 0x10080 + offset;
        assert(compiledBus.rdram().read8(address) == scalarBus.rdram().read8(address));
    }
}

void test_cpu_native_delay_slot_fallback() {
    Bus compiledBus, scalarBus;
    Cpu compiled, scalar;
    const bool native = compiled.setJitEnabled(true);
    constexpr u32 Start = 0x400;
    for (auto* bus : {&compiledBus, &scalarBus}) {
        writeProgram(*bus, Start, {
            lui(1, 0x8000), daddiu(1, 1, 1), beq(0, 0, 1),
            encodeI(opLw, 1, 2, 0), daddiu(3, 0, 1),
        });
    }
    compiled.setPc(Kseg0 + Start);
    scalar.setPc(Kseg0 + Start);
    for (u32 done = 0; done < 4;) {
        auto n = compiled.runQuietBlock(compiledBus, 4 - done);
        if (!n) { compiled.stepCached(compiledBus); n = 1; }
        assert(n <= 4 - done);
        for (u32 i = 0; i < n; ++i) scalar.step(scalarBus);
        done += n;
        assert(compiled.pc() == scalar.pc() && compiled.gpr() == scalar.gpr());
    }
    assert(!native || compiled.jitSteps() == 2);
    assert(compiled.cop0() == scalar.cop0());
    assert(compiled.cop0()[14] == Kseg0 + Start + 8);
    assert(compiled.cop0()[13] & (1u << 31)); // BD
}

void test_cpu_native_store_to_current_code() {
    Bus compiledBus, scalarBus;
    Cpu compiled, scalar;
    const bool native = compiled.setJitEnabled(true);
    constexpr u32 Start = 0x400;
    const u32 replacement = daddiu(3, 0, 9);
    for (auto* bus : {&compiledBus, &scalarBus}) {
        writeProgram(*bus, Start, {
            lui(1, 0x8000), lui(2, replacement >> 16), ori(2, 2, replacement & 0xffff),
            encodeI(opSw, 1, 2, Start + 16), daddiu(3, 0, 1), 0, 0x0000000D,
        });
    }
    compiled.setPc(Kseg0 + Start);
    scalar.setPc(Kseg0 + Start);
    for (u32 done = 0; done < 6;) {
        auto n = compiled.runQuietBlock(compiledBus, 6 - done);
        if (!n) { compiled.stepCached(compiledBus); n = 1; }
        assert(n <= 6 - done);
        for (u32 i = 0; i < n; ++i) scalar.step(scalarBus);
        done += n;
        assert(compiled.pc() == scalar.pc() && compiled.gpr() == scalar.gpr());
    }
    assert(!native || compiled.jitSteps() >= 3);
    assert(compiled.gpr()[3] == 9);
}

void test_cpu_native_cop1_prefix() {
    Bus compiledBus, scalarBus;
    Cpu compiled, scalar;
    const bool native = compiled.setJitEnabled(true);
    const auto fp = [](u32 fmt, u32 ft, u32 fs, u32 fd, u32 fn) {
        return (0x11u << 26) | (fmt << 21) | (ft << 16) | (fs << 11) | (fd << 6) | fn;
    };
    constexpr u32 Setup = 0x400, Start = 0x438;
    const std::array code{
        fp(16, 1, 0, 1, 0), fp(16, 0, 1, 3, 1),
        fp(16, 0, 3, 4, 2), fp(16, 0, 4, 5, 3),
        fp(16, 0, 0, 8, 0x21), fp(16, 0, 1, 10, 0x21),
        fp(17, 10, 8, 12, 0), fp(17, 8, 12, 14, 1),
        fp(17, 8, 14, 16, 2), fp(17, 8, 16, 18, 3), fp(17, 0, 18, 7, 0x20),
    };
    for (auto* bus : {&compiledBus, &scalarBus}) {
        for (u32 i = 0; i < code.size(); ++i) bus->write32(Start + i * 4, code[i]);
        bus->write32(Start + static_cast<u32>(code.size()) * 4, 0x0000000D); // BREAK bounds the prefix.
    }
    for (const u32 status : {0x2400u, 0x2000u, 0x2400u}) {
        const std::array setup{lui(1, status), mtc0(1, 12), lui(2, 0x3fc0), lui(3, 0x4010),
                               fp(4, 2, 0, 0, 0), fp(4, 3, 1, 0, 0)};
        for (auto* bus : {&compiledBus, &scalarBus})
            for (u32 i = 0; i < setup.size(); ++i) bus->write32(Setup + i * 4, setup[i]);
        compiled.setPc(Kseg0 + Setup);
        scalar.setPc(Kseg0 + Setup);
        for (u32 i = 0; i < setup.size(); ++i) {
            compiled.step(compiledBus);
            scalar.step(scalarBus);
        }
        compiled.setPc(Kseg0 + Start);
        scalar.setPc(Kseg0 + Start);
        const auto before = compiled.jitSteps();
        for (u32 i = 0; i < code.size();) {
            u32 steps = compiled.runQuietBlock(compiledBus, static_cast<u32>(code.size()) - i);
            if (!steps) { compiled.stepCached(compiledBus); steps = 1; }
            // The native path may span both source cache lines, but must not
            // retire instructions past the budget.
            if (native && i == 0) assert(steps == code.size());
            for (u32 j = 0; j < steps; ++j) scalar.step(scalarBus);
            i += steps;
            assert(compiled.fpr() == scalar.fpr() && compiled.pc() == scalar.pc());
            assert(compiled.cop0() == scalar.cop0());
        }
        if (native) assert(compiled.jitSteps() - before == code.size());
    }
}

void test_cpu_native_chain_loop_and_budget() {
    Bus compiledBus, scalarBus;
    Cpu compiled, scalar;
    const bool native = compiled.setJitEnabled(true);
    constexpr u32 Start = 0x400;
    const std::vector<u32> code{
        daddiu(1, 0, 0), daddiu(1, 1, 1), daddiu(2, 0, 0), daddiu(2, 2, 1),
        beq(0, 0, -5), daddiu(3, 3, 1), // six-step self-chaining loop
    };
    for (auto* bus : {&compiledBus, &scalarBus}) writeProgram(*bus, Start, code);
    compiled.setPc(Kseg0 + Start);
    scalar.setPc(Kseg0 + Start);

    // Sixty steps cross ten chained blocks but must return to C++ only once.
    const auto invocations = compiled.jitInvocations();
    const auto first = compiled.runQuietBlock(compiledBus, 60);
    assert(first == 60);
    if (native) assert(compiled.jitInvocations() == invocations + 1);
    for (u32 i = 0; i < first; ++i) scalar.step(scalarBus);
    assert(compiled.pc() == scalar.pc() && compiled.gpr() == scalar.gpr() &&
           compiled.cop0() == scalar.cop0() &&
           compiled.instructionsDispatched() == scalar.instructionsDispatched());

    // A short budget exits mid-loop: the native chain retires twelve steps,
    // then the interpreter finishes the remaining three at the exact PC.
    const auto beforeSteps = compiled.jitSteps();
    const auto beforeInvocations = compiled.jitInvocations();
    const auto second = compiled.runQuietBlock(compiledBus, 15);
    assert(second == 12);
    if (native) {
        assert(compiled.jitSteps() - beforeSteps == 12);
        assert(compiled.jitInvocations() == beforeInvocations + 2);
    }
    for (u32 i = 0; i < second; ++i) scalar.step(scalarBus);
    assert(compiled.pc() == scalar.pc() && compiled.gpr() == scalar.gpr() &&
           compiled.cop0() == scalar.cop0() &&
           compiled.instructionsDispatched() == scalar.instructionsDispatched());

    // The remaining three steps are below the native translation window and
    // are interpreted from the exact PC the chain exited at.
    const auto remainder = compiled.runQuietBlock(compiledBus, 3);
    assert(remainder == 3);
    for (u32 i = 0; i < remainder; ++i) scalar.step(scalarBus);
    assert(compiled.pc() == scalar.pc() && compiled.gpr() == scalar.gpr() &&
           compiled.cop0() == scalar.cop0() &&
           compiled.instructionsDispatched() == scalar.instructionsDispatched());
    if (native) assert(compiled.jitSteps() >= 72);
}

void test_cpu_native_chain_invalidation() {
    Bus compiledBus, scalarBus;
    Cpu compiled, scalar;
    const bool native = compiled.setJitEnabled(true);
    constexpr u32 A = 0x400, B = 0x410, TlbCode = 0x500;
    const std::vector<u32> program{
        daddiu(1, 1, 1), (2u << 26) | (B >> 2), daddiu(3, 3, 1), 0,
        daddiu(2, 2, 1), (2u << 26) | (A >> 2), daddiu(4, 4, 1),
    };
    for (auto* bus : {&compiledBus, &scalarBus}) {
        writeProgram(*bus, A, program);
        bus->write32(TlbCode, tlbwi());
    }
    compiled.setPc(Kseg0 + A);
    scalar.setPc(Kseg0 + A);

    const auto invocations = compiled.jitInvocations();
    // Cold start compiles A, exits once so B can be compiled and back-patched,
    // then chains both directions. A warm traversal needs a single entry.
    const auto full = compiled.runQuietBlock(compiledBus, 60);
    assert(full == 60);
    if (native) assert(compiled.jitInvocations() == invocations + 2);
    for (u32 i = 0; i < full; ++i) scalar.step(scalarBus);
    assert(compiled.pc() == scalar.pc() && compiled.gpr() == scalar.gpr() &&
           compiled.cop0() == scalar.cop0());

    const auto warmInvocations = compiled.jitInvocations();
    const auto warm = compiled.runQuietBlock(compiledBus, 60);
    assert(warm == 60);
    if (native) assert(compiled.jitInvocations() == warmInvocations + 1);
    for (u32 i = 0; i < warm; ++i) scalar.step(scalarBus);
    assert(compiled.pc() == scalar.pc() && compiled.gpr() == scalar.gpr() &&
           compiled.cop0() == scalar.cop0());

    // Store into the chained target's first line. The incoming A -> B edge must
    // be unlinked before the new word is observed.
    compiledBus.write32(B, daddiu(2, 2, 10));
    scalarBus.write32(B, daddiu(2, 2, 10));
    const auto beforeSmc = compiled.jitInvocations();
    const auto firstHalf = compiled.runQuietBlock(compiledBus, 3);
    assert(firstHalf == 3);
    assert(compiled.pc() == Kseg0 + B && compiled.gpr()[2] == scalar.gpr()[2]);
    if (native) assert(compiled.jitInvocations() == beforeSmc + 1);
    const auto secondHalf = compiled.runQuietBlock(compiledBus, 3);
    assert(secondHalf == 3);
    for (u32 i = 0; i < firstHalf + secondHalf; ++i) scalar.step(scalarBus);
    assert(compiled.gpr()[2] == scalar.gpr()[2] && compiled.gpr()[4] == scalar.gpr()[4]);
    assert(compiled.pc() == scalar.pc() && compiled.cop0() == scalar.cop0());

    // A COP0 TLB write changes mapped access but leaves both direct blocks
    // and their incoming links intact.
    compiled.setPc(Kseg0 + TlbCode);
    scalar.setPc(Kseg0 + TlbCode);
    compiled.stepCached(compiledBus);
    scalar.stepCached(scalarBus);
    compiled.setPc(Kseg0 + A);
    scalar.setPc(Kseg0 + A);
    const auto beforeTlb = compiled.jitInvocations();
    const auto afterTlb = compiled.runQuietBlock(compiledBus, 6);
    assert(afterTlb == 6);
    if (native) assert(compiled.jitInvocations() == beforeTlb + 1);
    for (u32 i = 0; i < afterTlb; ++i) scalar.step(scalarBus);
    assert(compiled.pc() == scalar.pc() && compiled.gpr() == scalar.gpr() &&
           compiled.cop0() == scalar.cop0());
}

void test_cpu_native_control_flow_and_calls() {
    constexpr u32 Setup = 0x300, Clear = 0x320, BneCode = 0x400, Bc1tCode = 0x410;
    constexpr u32 Bc1fCode = 0x420, Main = 0x500, FunctionA = 0x600, FunctionB = 0x700;
    const auto jal = [](u32 target) { return (0x03u << 26) | (target >> 2); };
    const auto jr = [](u32 rs) { return encodeR(0x08, rs, 0, 0); };

    Bus compiledBus, scalarBus;
    Cpu compiled, scalar;
    const bool native = compiled.setJitEnabled(true);
    const auto reset = [&] {
        compiled.reset();
        scalar.reset();
    };
    const auto run = [&](u32 steps) {
        for (u32 done = 0; done < steps;) {
            auto count = compiled.runQuietBlock(compiledBus, steps - done);
            if (!count) { compiled.stepCached(compiledBus); count = 1; }
            assert(count <= steps - done);
            for (u32 i = 0; i < count; ++i) scalar.step(scalarBus);
            done += count;
            assert(compiled.pc() == scalar.pc() && compiled.gpr() == scalar.gpr() &&
                   compiled.fpr() == scalar.fpr() && compiled.fcr31() == scalar.fcr31() &&
                   compiled.cop0() == scalar.cop0() &&
                   compiled.instructionsDispatched() == scalar.instructionsDispatched());
        }
    };
    const auto start = [&](u32 address) {
        compiled.setPc(Kseg0 + address);
        scalar.setPc(Kseg0 + address);
    };
    const auto stepScalarSetup = [&](u32 count) {
        for (u32 i = 0; i < count; ++i) {
            compiled.stepCached(compiledBus);
            scalar.step(scalarBus);
        }
    };

    // BNE self back-edge with a pinned counter in the delay slot.
    reset();
    for (auto* bus : {&compiledBus, &scalarBus}) {
        writeProgram(*bus, BneCode, {
            daddiu(1, 0, 3), daddiu(2, 0, 0),
            daddiu(1, 1, -1), daddiu(2, 2, 1), bne(1, 0, -3), 0,
        });
    }
    start(BneCode);
    const auto bneInvocations = compiled.jitInvocations();
    run(14);
    assert(compiled.gpr()[1] == 0 && compiled.gpr()[2] == 3);
    if (native) {
        assert(compiled.jitSteps() >= 12);
        assert(compiled.jitInvocations() <= bneInvocations + 2);
    }

    // BC1T and BC1F self-loops sample FCSR in the branch and mutate a pinned
    // GPR in the delay slot. Both must remain byte-identical to the interpreter.
    reset();
    for (auto* bus : {&compiledBus, &scalarBus}) {
        writeProgram(*bus, Setup, {
            lui(8, 0x2400), mtc0(8, 12), // CU1 | FR
            daddiu(1, 0, 0), ctc1(1, 31),
            cop1Compare(16, 0, 0, 0x32), // set the condition before the loop
        });
        writeProgram(*bus, Clear, {daddiu(1, 0, 0), ctc1(1, 31)});
        writeProgram(*bus, Bc1tCode, {
            bc1(1, -1), daddiu(2, 2, 1),
        });
        writeProgram(*bus, Bc1fCode, {
            bc1(0, -1), daddiu(3, 3, 1),
        });
    }
    start(Setup);
    stepScalarSetup(5);
    start(Bc1tCode);
    run(20);
    assert(compiled.gpr()[2] == 10 && compiled.pc() == Kseg0 + Bc1tCode);
    start(Clear);
    stepScalarSetup(2);
    start(Bc1fCode);
    run(20);
    assert(compiled.gpr()[3] == 10 && compiled.pc() == Kseg0 + Bc1fCode);
    if (native) assert(compiled.jitSteps() >= 40);

    // Nested JAL/JR crosses three separate blocks; the inner JR restores the
    // saved $ra before the outer JR $ra can take the native return chain.
    reset();
    for (auto* bus : {&compiledBus, &scalarBus}) {
        writeProgram(*bus, Main, {
            jal(FunctionA), daddiu(3, 0, 0),
            daddiu(4, 4, 1), jr(31), 0,
        });
        writeProgram(*bus, FunctionA, {
            or_(7, 31, 0), jal(FunctionB), daddiu(6, 6, 1),
            jr(7), 0,
        });
        writeProgram(*bus, FunctionB, {
            daddiu(8, 8, 1), jr(31), 0,
        });
    }
    start(Main);
    run(13);
    // All three return paths have retired: the inner JR returned to FunctionA,
    // the saved $ra returned to Main, and Main's JR $ra is next. Parity at each
    // chunk boundary already verifies the exact PC after every transfer.
    assert(compiled.pc() == Kseg0 + FunctionA + 12);
    assert(compiled.gpr()[3] == 0 && compiled.gpr()[4] == 1);
    assert(compiled.gpr()[6] == 1 && compiled.gpr()[7] == Kseg0 + Main + 8);
    assert(compiled.gpr()[8] == 1);
    if (native) assert(compiled.jitSteps() >= 8);
}

void test_cpu_native_cop1_compare_predicates() {
    constexpr u32 Setup = 0x400, Code = 0x500;
    const auto doubleRaw = [](double value) {
        u64 bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        return bits;
    };
    const auto rawFloat = [](u32 raw) { return static_cast<u64>(raw); };
    struct Values {
        u64 a, b;
    };
    const std::array<Values, 4> singles{{
        {rawFloat(floatRaw(1.0f)), rawFloat(floatRaw(2.0f))},
        {rawFloat(floatRaw(2.0f)), rawFloat(floatRaw(1.0f))},
        {rawFloat(floatRaw(1.0f)), rawFloat(floatRaw(1.0f))},
        {0x7FC00000ULL, rawFloat(floatRaw(1.0f))}, // unordered
    }};
    const std::array<Values, 4> doubles{{
        {doubleRaw(1.0), doubleRaw(2.0)},
        {doubleRaw(2.0), doubleRaw(1.0)},
        {doubleRaw(1.0), doubleRaw(1.0)},
        {0x7FF8000000000000ULL, doubleRaw(1.0)},
    }};

    // Reuse RAM and the native arena; all touched code is replaced below.
    // Resetting the CPUs isolates the cases without rebuilding 128 bus pairs.
    Bus compiledBus, scalarBus;
    Cpu compiled, scalar;
    const bool native = compiled.setJitEnabled(true);
    for (const u32 fmt : {16u, 17u}) {
        const auto& cases = fmt == 16 ? singles : doubles;
        for (u32 predicate = 0; predicate < 16; ++predicate) {
            for (const auto& values : cases) {
                compiled.reset();
                scalar.reset();
                std::vector<u32> setup{
                    lui(8, 0x2400), mtc0(8, 12), // CU1 | FR
                    lui(9, 0), ctc1(9, 31),      // clear FCSR condition
                };
                const auto load = [&](u32 gpr, u32 scratch, u32 fpr, u64 bits) {
                    const u32 hi = static_cast<u32>(bits >> 32);
                    const u32 lo = static_cast<u32>(bits);
                    if (fmt == 16) {
                        setup.push_back(lui(gpr, lo >> 16));
                        setup.push_back(ori(gpr, gpr, static_cast<u16>(lo)));
                        setup.push_back(mtc1(gpr, fpr));
                    } else {
                        setup.push_back(lui(gpr, hi >> 16));
                        setup.push_back(ori(gpr, gpr, static_cast<u16>(hi)));
                        setup.push_back(dsll32(gpr, gpr, 0));
                        setup.push_back(lui(scratch, lo >> 16));
                        setup.push_back(ori(scratch, scratch, static_cast<u16>(lo)));
                        setup.push_back(or_(gpr, gpr, scratch));
                        setup.push_back(dmtc1(gpr, fpr));
                    }
                };
                load(1, 2, 0, values.a);
                load(3, 4, 1, values.b);
                const std::vector<u32> code{
                    cop1Compare(fmt, 1, 0, 0x30u + predicate), 0, 0, 0,
                };
                for (auto* bus : {&compiledBus, &scalarBus}) {
                    writeProgram(*bus, Setup, setup);
                    writeProgram(*bus, Code, code);
                    bus->write32(Code + static_cast<u32>(code.size()) * 4, 0x0000000D);
                }
                compiled.setPc(Kseg0 + Setup);
                scalar.setPc(Kseg0 + Setup);
                for (std::size_t i = 0; i < setup.size(); ++i) {
                    compiled.stepCached(compiledBus);
                    scalar.step(scalarBus);
                }
                compiled.setPc(Kseg0 + Code);
                scalar.setPc(Kseg0 + Code);
                const auto beforeSteps = compiled.jitSteps();
                u32 executed = compiled.runQuietBlock(compiledBus, 4);
                if (!executed) { compiled.stepCached(compiledBus); executed = 1; }
                for (u32 i = 0; i < executed; ++i) scalar.step(scalarBus);

                assert(compiled.fcr31() == scalar.fcr31());
                assert(compiled.fpr() == scalar.fpr());
                assert(compiled.pc() == scalar.pc() && compiled.cop0() == scalar.cop0());
                assert(compiled.instructionsDispatched() == scalar.instructionsDispatched());
                // Non-signaling predicates always execute in native code. A
                // signaling NaN instead bails at the faulting PC so the
                // interpreter raises Invalid Operation with exact FCSR state.
                const bool signalingNan = predicate >= 8 && values.a == 0x7FC00000ULL;
                const bool doubleNan = predicate >= 8 && values.a == 0x7FF8000000000000ULL;
                if (native && !signalingNan && !doubleNan)
                    assert(compiled.jitSteps() - beforeSteps == 4);
            }
        }
    }
}

void test_cpu_native_bc1_delay_slot() {
    constexpr u32 Setup = 0x400, Code = 0x500;
    struct Case {
        u32 sub;
        bool condition;
    };
    constexpr std::array<Case, 8> cases{{
        {0, false}, {0, true},  // BC1F
        {1, false}, {1, true},  // BC1T
        {2, false}, {2, true},  // BC1FL
        {3, false}, {3, true},  // BC1TL
    }};
    for (const auto& test : cases) {
        Bus compiledBus, scalarBus;
        Cpu compiled, scalar;
        const bool native = compiled.setJitEnabled(true);
        const std::vector<u32> setup{
            lui(8, 0x2400), mtc0(8, 12),
            lui(1, 0x3F80), mtc1(1, 0), // f0 = 1.0
            lui(1, 0), mtc1(1, 1),      // f1 = 0.0
            lui(1, test.condition ? 0x0080 : 0), ctc1(1, 31),
        };
        // The branch samples FCSR before the delay slot; the delay slot then
        // flips the condition. Likely branches skip that write when not taken.
        const std::vector<u32> code{
            cop1Compare(16, 1, 0, 0x32), // false
            bc1(test.sub, 3),            // target is code[5]
            cop1Compare(16, 0, 0, 0x32), // true, in the delay slot
            0, 0, daddiu(5, 5, 1), 0,
        };
        for (auto* bus : {&compiledBus, &scalarBus}) {
            writeProgram(*bus, Setup, setup);
            writeProgram(*bus, Code, code);
        }
        compiled.setPc(Kseg0 + Setup);
        scalar.setPc(Kseg0 + Setup);
        for (std::size_t i = 0; i < setup.size(); ++i) {
            compiled.stepCached(compiledBus);
            scalar.step(scalarBus);
        }
        compiled.setPc(Kseg0 + Code);
        scalar.setPc(Kseg0 + Code);
        const auto beforeSteps = compiled.jitSteps();
        u32 executed = compiled.runQuietBlock(compiledBus, 3);
        if (!executed) { compiled.stepCached(compiledBus); executed = 1; }
        for (u32 i = 0; i < executed; ++i) scalar.step(scalarBus);
        assert(compiled.pc() == scalar.pc() && compiled.fcr31() == scalar.fcr31());
        assert(compiled.gpr() == scalar.gpr() && compiled.fpr() == scalar.fpr());
        assert(compiled.cop0() == scalar.cop0());
        assert(compiled.instructionsDispatched() == scalar.instructionsDispatched());
        // Likely-not-taken paths retire only the compare and branch; all other
        // cases retire the delay slot as well. Interpreter parity is exact.
        if (native) assert(compiled.jitSteps() - beforeSteps >= 2);
    }
}

void test_cpu_native_cop1_cu1_disabled() {
    Bus compiledBus, scalarBus;
    Cpu compiled, scalar;
    const bool native = compiled.setJitEnabled(true);
    constexpr u32 Code = 0x400;
    const std::vector<u32> setup{lui(1, 0), mtc0(1, 12)}; // CU1 clear
    const std::vector<u32> code{cop1Compare(16, 0, 0, 0x32), 0, 0, 0};
    for (auto* bus : {&compiledBus, &scalarBus}) {
        writeProgram(*bus, 0x300, setup);
        writeProgram(*bus, Code, code);
        bus->write32(Code + static_cast<u32>(code.size()) * 4, 0x0000000D);
    }
    compiled.setPc(Kseg0 + 0x300);
    scalar.setPc(Kseg0 + 0x300);
    for (std::size_t i = 0; i < setup.size(); ++i) {
        compiled.stepCached(compiledBus);
        scalar.step(scalarBus);
    }
    compiled.setPc(Kseg0 + Code);
    scalar.setPc(Kseg0 + Code);

    const auto invocations = compiled.jitInvocations();
    u32 executed = compiled.runQuietBlock(compiledBus, 4);
    if (!executed) { compiled.stepCached(compiledBus); executed = 1; }
    for (u32 i = 0; i < executed; ++i) scalar.step(scalarBus);
    assert(compiled.pc() == scalar.pc() && compiled.cop0() == scalar.cop0());
    assert(compiled.fcr31() == scalar.fcr31());
    assert(compiled.exceptionTrace().counts[11] == 1);
    assert(compiled.exceptionTrace().lastPc == Kseg0 + Code);
    if (native) {
        assert(compiled.jitInvocations() > invocations);
        assert(compiled.jitSteps() == 0);
    }
}

void test_cpu_tlb_exception_routing() {
    Bus bus;
    Cpu cpu;
    cpu.setPc(Kseg0 + 0x400);

    // Empty TLB: a store to KUSEG raises a store miss (EXCP cause 3).
    // through the exception state machine instead of propagating out of step().
    writeProgram(bus, 0x400, {
        lui(22, 0x0040), // r22 = 0x00400000 (KUSEG)
        sw(1, 22, 0),    // store to unmapped KUSEG address -> TLB miss
        0x00000000,
    });

    bool propagated = false;
    try {
        cpu.step(bus); // lui r22, 0x0040
        cpu.step(bus); // sw r1, r22, 0 -> TLB miss
    } catch (...) {
        propagated = true;
    }
    assert(!propagated);

    assert((cpu.cop0()[12] & (1ULL << 1)) != 0); // EXL set
    assert((cpu.cop0()[13] >> 2) == 3);        // cause = TLB store miss (bits 6:2)
    assert(cpu.cop0()[14] != 0);                 // EPC captured
    assert(cpu.gpr()[1] == 0);                   // store never landed
    assert(cpu.pc() == Kseg0);                  // TLB refill vector
    assert(cpu.cop0()[8] == 0x00400000);
    const auto& trace = cpu.exceptionTrace();
    assert(trace.observed && trace.counts[3] == 1);
    assert(trace.firstCode == 3 && trace.lastCode == 3);
    assert(trace.firstPc == Kseg0 + 0x404 && trace.lastPc == Kseg0 + 0x404);
    assert(trace.lastPcByCode[3] == Kseg0 + 0x404);
    cpu.reset();
    assert(!trace.observed && trace.counts[3] == 0);
}

void test_cpu_kuseg_mapped_cache() {
    Bus bus;
    Cpu cpu;
    const auto run = [&](std::initializer_list<u32> code) {
        cpu.setPc(Kseg0 + 0x400);
        writeProgram(bus, 0x400, code);
        for (size_t i = 0; i < code.size(); ++i) cpu.stepCached(bus);
    };
    bus.write32(0x2000, 0x11112222);
    bus.write32(0x3000, 0x33334444);
    bus.write32(0x4000, 0x55556666);
    run({lui(1, 0x0040), ori(2, 1, 7), mtc0(2, 10),
         ori(3, 0, 0x86), mtc0(3, 2), tlbwi(), lw(4, 1, 0), lw(5, 1, 0)});
    assert(cpu.gpr()[4] == 0x11112222 && cpu.gpr()[5] == cpu.gpr()[4]);

    // EntryLo is staging: it only changes the translation after TLBWI.
    run({ori(3, 0, 0xC6), mtc0(3, 2), lw(4, 1, 0), tlbwi(), lw(5, 1, 0)});
    assert(cpu.gpr()[4] == 0x11112222 && cpu.gpr()[5] == 0x33334444);
    const auto gen = cpu.rdramLineGeneration(0x3000 / 64);
    run({sw(4, 1, 0)});
    assert(bus.read32(0x3000) == 0x11112222 && cpu.rdramLineGeneration(0x3000 / 64) == gen + 1);

    // Reuse the VA with a different ASID in a random slot, read-only.
    constexpr u32 Tlbwr = 0x42000006;
    run({ori(2, 1, 8), mtc0(2, 10), ori(3, 0, 0x102), mtc0(3, 2),
         Tlbwr, lw(6, 1, 0), lw(7, 1, 0)});
    assert(cpu.gpr()[6] == 0x55556666 && cpu.gpr()[7] == cpu.gpr()[6]);
    run({sw(4, 1, 0)});
    assert(cpu.pc() == Kseg0 + 0x180 && ((cpu.cop0()[13] >> 2) & 31) == 1);
    assert(bus.read32(0x4000) == 0x55556666);

    // Invalid even entries are distinct from misses. EXL suppresses refill
    // vector selection and preserves the first exception's EPC.
    const auto epc = cpu.cop0()[14];
    run({ori(2, 1, 7), mtc0(2, 10), mtc0(0, 2), tlbwi(), lw(5, 1, 0x1000)});
    assert(cpu.pc() == Kseg0 + 0x180 && ((cpu.cop0()[13] >> 2) & 31) == 2);
    assert(cpu.cop0()[14] == epc);

    // ERL temporarily exposes the physical low segment, not the TLB entry.
    bus.write32(0x400000, 0x77778888);
    run({ori(3, 0, 4), mtc0(3, 12), lw(4, 1, 0)});
    assert(cpu.gpr()[4] == 0x77778888);
}

void test_cpu_load_linked_store_conditional() {
    Bus bus;
    Cpu cpu;
    cpu.setPc(Kseg0 + 0x400);
    bus.write64(0x1000, 0x89ABCDEF01234567ULL);
    writeProgram(bus, 0x400, {
        lui(1, 0x8000), encodeI(0x30, 1, 2, 0x1000), mfc0(3, 17),
        encodeI(0x34, 1, 4, 0x1000), encodeI(0x3C, 1, 4, 0x1040),
        lui(5, 0x8000), ori(5, 5, 0x430), dmtc0(5, 14), eret(),
    });
    writeProgram(bus, 0x430, {encodeI(0x38, 0, 6, 0x1000)}); // no reservation, unmapped
    for (int i = 0; i < 10; ++i) cpu.stepCached(bus);
    assert(cpu.gpr()[2] == 0xFFFFFFFF89ABCDEFULL && cpu.gpr()[3] == 0x100);
    assert(cpu.gpr()[4] == 1 && bus.read64(0x1040) == 0x89ABCDEF01234567ULL);
    assert(cpu.gpr()[6] == 0 && !cpu.exceptionTrace().observed);
}

void test_rsp_task_dp_submission() {
    Emulator emulator;

    auto& bus = emulator.bus();
    constexpr u32 bufAddr = 0x40, bufSize = 40, image = 0x200;
    // The genuine DPC XBUS path fetches these commands from SP DMEM.
    bus.write64(0x04000040, (0xFFULL << 56) | (2ULL << 51) | (3ULL << 32) | image);
    bus.write64(0x04000048, (0xEFULL << 56) | (3ULL << 52));
    bus.write64(0x04000050, (0xF7ULL << 56) | 0xF80107C1); // red, green
    bus.write64(0x04000058, (0xF6ULL << 56) | (12ULL << 44) | (4ULL << 32));
    bus.write64(0x04000060, 0xE9ULL << 56); // SYNC_FULL
    writeProgram(bus, 0x04001000, {
        addiu(1, 0, 10), 0x40815800, // set XBUS and FREEZE
        addiu(1, 0, bufAddr), (0x40814000), // MTC0 r1, DPC_START
        addiu(1, 0, bufAddr + bufSize), (0x40814800), // MTC0 r1, DPC_END
        addiu(1, 0, 4), 0x40815800, // clear FREEZE
        0x0000000D, // BREAK
    });
    bus.write32(0x04040010, 0x101);
    for (int i = 0; i < 7; ++i) emulator.runOneStep();
    assert(bus.read32(image) == 0 && !(bus.hw().mi().interrupt & 0x20));
    for (int i = 0; i < 2; ++i) emulator.runOneStep();
    assert(bus.read32(image) == 0xF80107C1 && bus.read32(image + 12) == 0xF80107C1);
    assert(bus.read32(0x04100008) == bufAddr + bufSize);
    assert((bus.hw().mi().interrupt & 0x21) == 0x21);
    assert((bus.hw().sp().status & 0x203) == 3);
    bus.write32(0x04300000, 0x800);
    emulator.runOneStep();
    assert(!(bus.hw().mi().interrupt & 0x20)); // consumed SYNC_FULL is not replayed
}

void test_vi_scanout() {
    Emulator emu;
    auto& bus = emu.bus();
    bus.write32(0x04400004, 0x1000);
    bus.write32(0x04400008, 4);
    bus.write32(0x04400024, (100 << 16) | 104);
    bus.write32(0x04400028, (30 << 16) | 34);
    bus.write32(0x04400030, 512);
    bus.write32(0x04400034, 1024);
    bus.write32(0x04400000, 2);
    bus.write32(0x1000, 0xF80107C1);
    bus.write32(0x1008, 0x003F0000);
    // The worker advances exactly to the field boundary and gets the guest
    // cycles consumed for host pacing. Two lines, two clocks per line.
    bus.hw().setVideoClock(HwRegisters::CpuClockHz);
    bus.write32(0x04400018, 3);
    bus.write32(0x0440001C, 1);
    emu.cpu().setPc(0xFFFFFFFF80000000ULL);
    assert(emu.runVideoFrame() == 4 && bus.hw().videoFields() == 1);
    assert(emu.framebufferWidth() == 2 && emu.framebufferHeight() == 2);
    assert((emu.framebuffer() == std::vector<u32>{0xFF0000FF, 0xFF00FF00, 0xFFFF0000, 0xFF000000}));
    bus.write32(0x04400000, 3);
    bus.write32(0x04400030, (1024 << 16) | 512); // shift the crop one pixel
    bus.write32(0x1004, 0x12345600);
    emu.scanout();
    assert(emu.framebuffer()[0] == 0xFF563412);
    bus.write32(0x04400000, 0);
    emu.scanout();
    assert(emu.framebuffer().empty());
}

void test_ai_dma() {
    Bus bus;
    auto& hw = bus.hw();
    hw.setVideoClock(HwRegisters::CpuClockHz); // deterministic integer divider
    bus.write32(0x1FF8, 0x80017FFF);
    bus.write32(0x1FFC, 0x1234FFFF);
    bus.write32(0x5000, 0x45670042); // delayed carry shifts buffer 2 by 8 KiB
    bus.write32(0x04500010, 2); // one stereo sample every three steps
    bus.write32(0x04500000, 0x1FF8);
    bus.write32(0x04500004, 8);
    assert(hw.mi().interrupt & 4);
    bus.write32(0x0450000C, 0);
    bus.write32(0x04500000, 0x3000);
    bus.write32(0x04500004, 8);
    assert(!(hw.mi().interrupt & 4) && (bus.read32(0x0450000C) & 0xC0000000) == 0xC0000000);
    assert(!hw.tickAudio(bus) && bus.read32(0x04500004) == 8);
    bus.write32(0x04500008, 1);
    auto next = [&] {
        assert(!hw.tickAudio(bus) && !hw.tickAudio(bus));
        return hw.tickAudio(bus);
    };
    assert((next() == HwRegisters::StereoSample{-32767, 32767}));
    assert(!(hw.mi().interrupt & 4) && bus.read32(0x04500004) == 4);
    assert((next() == HwRegisters::StereoSample{0x1234, -1}));
    assert(hw.mi().interrupt & 4);
    bus.write32(0x0450000C, 0xFFFFFFFF);
    assert((next() == HwRegisters::StereoSample{0x4567, 0x42}));
    assert(next() && !(hw.mi().interrupt & 4));
    assert(!(bus.read32(0x0450000C) & 0xC0000000));

    bus.write32(0x04500000, 0x1FF8);
    bus.write32(0x04500004, 8);
    assert(!hw.tickAudio(bus)); // retain elapsed phase when the divider changes
    bus.write32(0x04500010, 1);
    assert(hw.tickAudio(bus));
}

void test_audio_resample_to_host_fifo() {
    for (u32 rate : {22050u, 32000u, 44100u}) {
        Emulator emu;
        auto& bus = emu.bus();
        const auto divider = (bus.hw().videoClockHz() + rate / 2) / rate;
        for (u32 a = 0x2000; a < 0x3000; a += 4) bus.write32(a, 0x1234EDCC);
        bus.write32(0x04500010, divider - 1);
        bus.write32(0x04500000, 0x2000);
        bus.write32(0x04500004, 0x1000);
        bus.write32(0x04500008, 1);
        AudioRing<256> ring;
        for (int batch = 0; batch < 2; ++batch) {
            emu.runFrame();
            size_t frames;
            const auto* samples = emu.audioSamples(frames);
            assert(samples && frames >= 48 && frames <= 53);
            assert(ring.push(samples, frames) == frames);
        }
        u32 audible = 0;
        ring.render(90, [&](auto f) {
            assert(f[0] == -f[1]);
            audible += f[0] != 0;
        });
        assert(audible == 90);
        assert((bus.read32(0x0450000C) & 0xC0000000) == 0x40000000);
    }
}

void test_vi_timing_register_update() {
    Bus bus;
    auto& hw = bus.hw();
    hw.setVideoClock(HwRegisters::CpuClockHz);
    bus.write32(0x04400018, 3);
    bus.write32(0x0440001C, 7);
    hw.tickVideo(3);
    assert(hw.vi().current == 0);
    bus.write32(0x0440001C, 3);
    hw.tickVideo(1);
    assert(hw.vi().current == 2);
    hw.tickVideo(4);
    assert(hw.videoFields() == 1);
}

void test_rdp_copy_texture_and_split_dma() {
    for (bool block : {false, true}) {
        Bus bus;
        Rdp rdp;
        rdp.setBus(&bus);
        rdp.setHw(&bus.hw());
        constexpr u32 Dpc = 0x04100000;
        const std::array<u16, 8> texture{0xF801, 0x07C1, 0x003F, 0xFFFF, 0x0001, 0xFFC1, 0x07FF, 0xF83F};
        for (u32 i = 0; i < texture.size(); ++i) bus.write16(0x300 + i * 2, texture[i]);
        const u64 commands[] = {
            (0xFDULL << 56) | (2ULL << 51) | (3ULL << 32) | 0x300, // SETTIMG
            (0xF5ULL << 56) | (2ULL << 51) | (block ? 0 : 1ULL << 41),
            block ? (0xF3ULL << 56) | (7ULL << 12) | 2048     // LOADBLOCK, one word per row
                  : (0xF4ULL << 56) | (12ULL << 12) | 4,      // LOADTILE
            (0xF5ULL << 56) | (2ULL << 51) | (1ULL << 41),    // render tile, stride 8
            (0xF2ULL << 56) | (12ULL << 12) | 4,              // SETTILESIZE
            (0xFFULL << 56) | (2ULL << 51) | (3ULL << 32) | 0x400,
            (0xEFULL << 56) | (2ULL << 52), // copy cycle
            (0xE4ULL << 56) | (12ULL << 44) | (4ULL << 32),
            (4096ULL << 16) | 1024, // S and T advance one texel per output pixel
            0xE9ULL << 56,
        };
        for (u32 i = 0; i < std::size(commands); ++i) bus.write64(0x100 + i * 8, commands[i]);
        bus.write32(Dpc, 0x100);
        bus.write32(Dpc + 4, 0x140); // only first word of TEXRECT
        rdp.process();
        assert(bus.read32(Dpc + 8) == 0x140 && bus.read32(0x400) == 0);
        bus.write64(0x138, 0); // RDP already DMA'd the first half
        bus.write32(Dpc + 4, 0x150);
        rdp.process();
        for (u32 i = 0; i < texture.size(); ++i) assert(bus.read16(0x400 + i * 2) == texture[i]);
        assert(rdp.stats().fullSyncs == 1 && (bus.hw().mi().interrupt & MI_INTR_DP));
    }
}
void test_rsp_scalar() {
    Bus bus;
    Rsp rsp;
    bus.attachRsp(&rsp);
    rsp.setBus(&bus);
    rsp.setPc(0);

    // Write program into RSP IMEM via the CPU-visible bus address.
    writeProgram(bus, 0x04001000, {
        addiu(1, 0, 7),
        addiu(2, 0, 3),
        addu(3, 1, 2),
        sw(3, 0, 0xFFF), // unaligned scalar store wraps within DMEM
        lw(4, 0, 0x1FFF),
    });

    bus.write32(0x04040010, 1);
    for (int i = 0; i < 5; ++i) rsp.step();

    assert(rsp.regs()[3] == 10);
    assert(rsp.regs()[4] == 10 && bus.read8(0x04000002) == 10);
    assert(bus.read32(0x04001000) == addiu(1, 0, 7));
}

void test_rsp_imem_decode_invalidation() {
    Bus bus;
    Rsp rsp;
    bus.attachRsp(&rsp);
    rsp.setBus(&bus);
    constexpr u32 Sp = 0x04040000, Imem = 0x04001000;
    bus.write32(Sp + 0x10, 1); // clear HALT

    const auto runPair = [&] {
        rsp.setPc(0xFFC);
        rsp.step();
        assert(rsp.pc() == 0);
        rsp.step();
    };
    bus.write32(Imem + 0xFFC, addiu(1, 0, 7));
    bus.write32(Imem, addiu(2, 0, 8));
    runPair();
    assert(rsp.regs()[1] == 7 && rsp.regs()[2] == 8);
    bus.write8(Imem + 0xFFF, 9);
    runPair();
    assert(rsp.regs()[1] == 9 && rsp.regs()[2] == 8);

    // DMA wraps within the selected IMEM bank, invalidating both cached words.
    bus.write32(0x1084, addiu(1, 0, -7));
    bus.write32(0x1088, (0x12u << 26) | (16u << 21) | (1u << 16) |
                        (1u << 11) | (2u << 6) | 0x2A); // VOR v2,v1,v1
    Rsp::Vreg lanes{1, 2, 3, 4, 5, 6, 7, 8};
    rsp.setVreg(1, lanes);
    bus.write32(Sp, 0x1FF8);
    bus.write32(Sp + 4, 0x1080);
    bus.write32(Sp + 8, 15);
    runPair();
    assert(rsp.regs()[1] == static_cast<u32>(-7) && rsp.regs()[2] == 8);
    assert(rsp.vreg(2) == lanes);

    rsp.reset();
    runPair();
    assert(rsp.regs()[1] == 0 && rsp.vreg(2) == Rsp::Vreg{});
}

void test_rsp_vector_registers() {
    Rsp rsp;
    // Initialize a vector register.
    Rsp::Vreg v{};
    for (u32 i = 0; i < Rsp::Vlanes; ++i) v[i] = static_cast<u16>(0x0100 * (i + 1));
    rsp.setVreg(1, v);

    // Store to DMEM and load back into a different register.
    rsp.vstore(1, 0, 0);
    rsp.vload(2, 0, 0);

    const auto& loaded = rsp.vreg(2);
    for (u32 i = 0; i < Rsp::Vlanes; ++i) {
        assert(loaded[i] == static_cast<u16>(0x0100 * (i + 1)));
    }
}

void test_rsp_vector_elements() {
    Rsp rsp;
    Rsp::Vreg v{};
    for (u32 i = 0; i < Rsp::Vlanes; ++i) v[i] = static_cast<u16>(0x0200 + (i * 0x11));
    rsp.setVreg(1, v);
    rsp.vstore(1, 0, 0x100); // element offset 2

    // vload with element=2 shifts lanes: lane (2+i) % 8 receives lane i.
    rsp.vload(2, 2, 0x100);
    const auto& loaded = rsp.vreg(2);
    for (u32 i = 0; i < Rsp::Vlanes; ++i) {
        const u32 dst = (2 + i) & 0x7;
        assert(loaded[dst] == v[i]);
    }
}

void test_rsp_cop2() {
    Rsp rsp;
    const auto alu = [](u32 op, u32 dst, u32 s, u32 t, u32 e = 0) {
        return 0x4A000000 | (e << 21) | (t << 16) | (s << 11) | (dst << 6) | op;
    };
    const auto run = [&](u32 instr) {
        rsp.writeMem32(0x1000 + rsp.pc(), instr);
        rsp.step();
    };
    rsp.setVreg(1, {0x7FFF, 0x8000, 0xFFFF, 0, 1, 2, 3, 4});
    rsp.setVreg(2, {1, 0xFFFF, 1, 0, 2, 3, 4, 5});
    run(alu(0x10, 3, 1, 2)); // VADD
    assert((rsp.vreg(3) == Rsp::Vreg{0x7FFF, 0x8000, 0, 0, 3, 5, 7, 9}));
    run(alu(0x1D, 4, 0, 0, 10)); // VSAR ACC_LO (unclamped)
    assert(rsp.vreg(4)[0] == 0x8000 && rsp.vreg(4)[1] == 0x7FFF);
    run(alu(0x14, 3, 1, 2)); // VADDC
    run(0x48450000); // CFC2 r5,VCO
    assert(rsp.regs()[5] == 6);
    run(alu(0x10, 3, 1, 2, 8)); // VADD with lane 0 broadcast, carry-in
    assert(rsp.vreg(3)[2] == 1 && rsp.vreg(3)[7] == 5);
    run(alu(0x00, 3, 1, 1, 9)); // VMULF: lane 1 multiplies -32768 by -32768
    run(alu(0x1D, 4, 0, 0, 9));
    assert(rsp.vreg(3)[1] == 0x7FFF && rsp.vreg(4)[1] == 0x8000);
    run(alu(0x30, 3, 0, 2, 8)); // VRCP(1) = 0x7FFFC000
    run(alu(0x32, 3, 1, 2, 11)); // VRCPH reads upper half, latches zero
    assert(rsp.vreg(3)[0] == 0xC000 && rsp.vreg(3)[1] == 0x7FFF);
    run(alu(0x35, 3, 2, 2, 11)); // VRSQL(0) = 0x7FFFFFFF
    assert(rsp.vreg(3)[2] == 0xFFFF);

    // Exercise both SIMD and portable paths with aliasing and overflow.
    rsp.setVreg(6, {0, 1, 2, 3, 0x7FFF, 0x8000, 0xFFFF, 0x8000});
    run(alu(0x07, 7, 6, 6, 15)); // VMUDH, multiply every lane by signed -32768
    run(alu(0x0F, 7, 6, 6, 15)); // VMADH wraps +2^47 to -2^47 in lanes 5,7
    run(alu(0x1D, 8, 0, 0, 8)); // ACC_HI
    assert((rsp.vreg(7) == Rsp::Vreg{0, 0x8000, 0x8000, 0x8000, 0x8000, 0x8000, 0x7FFF, 0x8000}));
    assert(rsp.vreg(8)[5] == 0x8000 && rsp.vreg(8)[7] == 0x8000);
    run(alu(0x2D, 6, 6, 6, 15)); // VNXOR, VD == VS == VT
    run(alu(0x1D, 8, 0, 0, 10));
    assert((rsp.vreg(6) == Rsp::Vreg{0x7FFF, 0x7FFE, 0x7FFD, 0x7FFC, 0, 0xFFFF, 0x8000, 0xFFFF}));
    assert(rsp.vreg(8) == rsp.vreg(6));
    run(alu(0x1D, 8, 0, 0, 8)); // VNXOR preserves high accumulator
    assert(rsp.vreg(8)[5] == 0x8000);
}

void test_host_audio_ring() {
    AudioRing<256> ring;
    std::array<s16, 512> frames{};
    for (int i = 0; i < 256; ++i) {
        frames[i * 2] = static_cast<s16>(i * 100);
        frames[i * 2 + 1] = static_cast<s16>(i * 50);
    }
    assert(ring.push(frames.data(), 63) == 63);
    ring.render(4, [](auto f) { assert(f[0] == 0 && f[1] == 0); }); // not primed
    assert(ring.stats().fillFrames == 63 && ring.stats().underruns == 0);
    assert(ring.push(frames.data() + 126, 193) == 193);
    assert(ring.push(frames.data(), 1) == 0); // do not overwrite unread samples
    int sample = 0;
    ring.render(100, [&](auto f) {
        if (sample == 99) {
            assert(f[0] > 9900 && f[0] < 10'000); // interpolation, +0.5% FIFO correction
            assert(f[1] == f[0] / 2);
        }
        ++sample;
    });
    assert(ring.push(frames.data(), 96) == 96); // wrap physical ring
    ring.render(400, [&](auto f) {
        if (sample >= 390) assert(f[0] == 0 && f[1] == 0);
        ++sample;
    });
    assert(ring.push(frames.data(), 64) == 64); // recovered from underrun
    const auto stats = ring.stats();
    assert(stats.fillFrames == 64 && stats.capacityFrames == 256);
    assert(stats.underruns == 1 && stats.overruns == 1);
    ring.reset();
    assert(ring.stats().fillFrames == 0 && ring.stats().underruns == 0 && ring.stats().overruns == 0);
}

void test_audio_ring_producer_frames_during_render() {
    AudioRing<256> ring;
    std::array<s16, 256> prime{};
    std::array<s16, 256> fresh{};
    prime.fill(1000);
    fresh.fill(20000);
    assert(ring.push(prime.data(), 64) == 64);

    std::array<std::array<s16, 2>, 128> out{};
    bool pushed = false;
    std::size_t emitted = 0;
    ring.render(out.size(), [&](const auto& frame) {
        out[emitted] = {frame[0], frame[1]};
        assert(frame.fromGuest); // refreshed writer must avoid an underrun reset
        // Last frame readable from the snapshot. Queue more data here; the
        // next iteration used to see the stale write cursor and emit tail.
        if (!pushed && emitted == 62) {
            assert(ring.push(fresh.data(), 64) == 64);
            pushed = true;
        }
        ++emitted;
    });
    assert(pushed && emitted == out.size());
    assert(out[62][0] == 1000 && out[64][0] == 20000);
    assert(std::all_of(out.begin(), out.end(), [](const auto& f) {
        return f[0] == f[1] && f[0] != 0;
    }));
    const auto stats = ring.stats();
    assert(stats.underruns == 0 && stats.overruns == 0 && stats.fillFrames == 0);
}

void test_audio_ring_signed_fade_and_tail() {
    AudioRing<256> ring;
    std::array<s16, 128> input{};
    input.fill(-32768);
    assert(ring.push(input.data(), 64) == 64);

    std::array<int, 64> fade{};
    std::size_t index = 0;
    ring.render(fade.size(), [&](const auto& frame) {
        assert(frame.fromGuest);
        fade[index++] = frame[0];
        assert(frame[0] == frame[1] && frame[0] < 0);
    });
    assert(index == fade.size());
    assert(fade[0] == -1024 && fade[31] == -32768 && fade.back() == -32768);

    std::array<int, 4> tail{};
    index = 0;
    ring.render(tail.size(), [&](const auto& frame) {
        assert(!frame.fromGuest); // tail is synthetic, not guest data
        tail[index++] = frame[0];
        assert(frame[0] == frame[1] && frame[0] < 0);
    });
    constexpr std::array<int, 4> expected{-31744, -30720, -29696, -28672};
    assert(tail == expected);
    const auto stats = ring.stats();
    assert(stats.underruns == 1 && stats.fillFrames == 0);
}

void test_audio_ring_overrun_accounting() {
    AudioRing<256> ring;
    std::array<s16, 512> input{};
    for (std::size_t i = 0; i < input.size(); ++i) input[i] = static_cast<s16>(i + 1);
    assert(ring.push(input.data(), 200) == 200);
    assert(ring.push(input.data() + 400, 100) == 56); // only 56 frames fit
    auto stats = ring.stats();
    assert(stats.fillFrames == 256 && stats.overruns == 1 && stats.underruns == 0);

    std::size_t emitted = 0, guest = 0;
    ring.render(255, [&](const auto& frame) {
        assert(frame.fromGuest);
        ++emitted;
        guest += frame.fromGuest;
    });
    assert(emitted == 255 && guest == 255); // rejected frames never enter the FIFO

    std::size_t synthetic = 0;
    ring.render(2, [&](const auto& frame) { synthetic += !frame.fromGuest; });
    assert(synthetic == 2);
    stats = ring.stats();
    assert(stats.fillFrames == 0 && stats.overruns == 1 && stats.underruns == 1);
}

void test_core_stats_sampler() {
    CoreStatsSampler sampler;
    CoreCounters c;
    sampler.reset(0, c);
    c.cycles = NominalCpuHz;
    c.instructions = 90'000'000;
    c.viFields = 60;
    c.paints = 120;
    c.cacheLookups = 50;
    c.cacheHits = 40;
    c.sampledSteps = c.cycles / 100;
    c.cpuNs = 2'500'000;
    c.rspNs = 1'000'000;
    c.rdpNs = 150'000'000;
    c.sleepNs = 250'000'000;
    c.gfx = 7;
    c.audio = 30;
    c.other = 4;
    c.triangles = 723;
    assert(!sampler.update(999'999'999, c));
    assert(sampler.update(1'000'000'000, c));
    auto s = sampler.snapshot();
    assert(s.ready && s.speed == 100 && s.mips == 90 && s.mhz == 93.75);
    assert(s.viFps == 60 && s.displayFps == 120);
    assert(s.hasCacheRate && s.cacheHit == 80);
    assert(s.gfxPerSec == 7 && s.audioPerSec == 30 && s.otherPerSec == 4 && s.trianglesPerSec == 723);
    const auto near = [](double a, double b) { return std::abs(a - b) < 1e-9; };
    assert(near(s.cpuMs, 250.0 / 60) && near(s.rspMs, 100.0 / 60) && near(s.rdpMs, 150.0 / 60));
    assert(near(s.frameMs, s.cpuMs + s.rspMs + s.rdpMs + s.sleepMs + s.otherMs));
    c.cycles += NominalCpuHz / 2;
    c.viFields += 30;
    c.paints += 60;
    assert(sampler.update(2'000'000'000, c));
    s = sampler.snapshot();
    assert(near(s.speed, 50 + 50 / std::exp(1.0)) && !s.hasCacheRate);
    assert(near(s.viFps, 30 + 30 / std::exp(1.0)));
    sampler.reset(2'000'000'000, {});
    assert(!sampler.snapshot().ready);
    CoreCounters mixed;
    mixed.cycles = 1000;
    mixed.batchedCycles = 800;
    mixed.sampledSteps = 110;
    mixed.sampledBatchCycles = 100;
    mixed.batchCpuNs = 10'000'000;
    mixed.cpuNs = 2'000'000;
    mixed.rspNs = 1'000'000;
    mixed.viFields = 50;
    assert(sampler.update(3'000'000'000, mixed));
    assert(near(sampler.snapshot().cpuMs, 2.4) && near(sampler.snapshot().rspMs, 0.4));
}

void test_vi_motion_interpolation() {
    constexpr int size = 32;
    std::array<u32, size * size> previous{}, current{}, out{};
    previous.fill(0xFF000000);
    current = previous;
    for (int y = 4; y < 12; ++y) {
        for (int x = 3; x < 8; ++x) {
            previous[y * size + x] = 0xFFFFFFFF;
            current[y * size + x + 4] = 0xFFFFFFFF;
        }
    }
    video::ViInterpolation motion;
    motion.estimate(previous, current, size, size);
    assert(motion.synthesize(previous, current, out, 0.0) && out == previous);
    assert(motion.synthesize(previous, current, out, 1.0) && out == current);
    assert(motion.synthesize(previous, current, out, 0.5));
    assert(out[7 * size + 4] == 0xFF000000 && out[7 * size + 5] == 0xFFFFFFFF);
    assert(out[7 * size + 9] == 0xFFFFFFFF && out[7 * size + 10] == 0xFF000000);
    previous.fill(0xFF0000FF);
    current.fill(0xFFFF0000);
    motion.estimate(previous, current, size, size);
    assert(motion.synthesize(previous, current, out, 0.5) && out[0] == 0xFF800080);
    assert(!motion.synthesize(previous, current, std::span{out}.first(3), 0.5));
}

void test_rsp_transpose() {
    Rsp rsp;
    const auto run = [&](u32 instr) {
        rsp.writeMem32(0x1000 + rsp.pc(), instr);
        rsp.step();
    };
    const auto mem = [](u32 op, u32 base, u32 vt, u32 e, u32 offset) {
        return (op << 26) | (base << 21) | (vt << 16) | (11 << 11) | (e << 7) | offset;
    };
    // LTV's 16-byte window starts at 0xFF8, straddling the DMEM boundary.
    for (u32 i = 0; i < 16; ++i) rsp.writeMem8((0xFF8 + i) & 0xFFF, 0x40 + i);
    run(addiu(1, 0, 0xFF8));
    run(mem(0x32, 1, 13, 2, 0));
    constexpr u16 expected[] = {0x4A4B, 0x4C4D, 0x4E4F, 0x4041, 0x4243, 0x4445, 0x4647, 0x4849};
    for (u32 i = 0; i < 8; ++i) {
        Rsp::Vreg vector{};
        vector[i] = expected[i];
        assert(rsp.vreg(8 + ((i + 1) & 7)) == vector);
    }
    run(addiu(1, 0, 0x108));
    run(mem(0x3A, 1, 15, 2, 0));
    for (u32 i = 0; i < 16; ++i)
        assert(rsp.readMem8(0x108 + i) == 0x40 + ((i + 10) & 15));
}

void test_hw_registers() {
    Bus bus;

    // VI origin masks to 24 bits.
    bus.write32(0x04400004, 0x12345678);
    assert(bus.read32(0x04400004) == 0x00345678);

    // MI mask and pending interrupt aggregation.
    bus.write32(0x0430000C, 0x80);
    bus.hw().mi().setPending(0x08);
    assert(bus.read32(0x04300008) == 0x08);
    assert(bus.hw().mi().irq());

    bus.write32(0x04400010, 0);
    assert(!bus.hw().mi().irq());

    // SP status write clears halt.
    bus.write32(0x04040010, 0x00000101);
    assert((bus.read32(0x04040010) & 0x0041) == 0x0040);
}

void test_pif_ram_si_interrupt() {
    Bus bus;
    Cpu cpu;
    cpu.setPc(Kseg0 + 0x400);
    writeProgram(bus, 0x400, {0x34010401, 0x40816000, 0}); // ORI r1,0,0x401; MTC0 r1,Status
    cpu.step(bus);
    cpu.step(bus);
    constexpr u32 Pif = 0x1FC007C0;
    constexpr u32 Si = 0x04800000;
    constexpr u32 MiMask = 0x0430000C;
    bus.write32(MiMask, 0x8); // set SI mask

    bus.write32(Pif, 0x12345678);
    assert(bus.read32(Pif + 0x800) == 0x12345678);
    assert(bus.read32(0x1FC00000) == 0); // locked PIF ROM is not RAM
    assert((bus.read32(Si + 0x18) & 3) == 3);
    assert(!bus.hw().mi().irq());
    assert(bus.hw().tickDma(bus));
    assert((bus.read32(Si + 0x18) & 0x1003) == 0x1000);
    assert(bus.hw().mi().irq());
    cpu.step(bus);
    assert(cpu.pc() == 0xFFFFFFFF80000180ULL);
    assert(cpu.cop0()[14] == Kseg0 + 0x408);
    assert((cpu.cop0()[13] & 0x47C) == 0x400); // external interrupt, IP2 set
    bus.write32(Si + 0x18, 0);
    assert(!bus.hw().mi().irq());

    for (u32 i = 0; i < 63; ++i) bus.write8(0x100 + i, static_cast<u8>(i + 1));
    bus.write32(Si, 0x100);
    bus.write32(Si + 0x10, Pif); // RDRAM -> PIF
    assert((bus.read32(Si + 0x18) & 3) == 1);
    assert(!bus.hw().mi().irq());
    assert(bus.hw().tickDma(bus));
    assert(bus.read32(Pif) == 0x01020304);
    assert(bus.hw().mi().irq());
    bus.write32(Si + 0x18, 0);

    bus.write32(Si, 0x200);
    bus.write32(Si + 4, Pif); // PIF -> RDRAM
    assert(bus.hw().tickDma(bus));
    for (u32 i = 0; i < 64; ++i) assert(bus.read8(0x200 + i) == bus.read8(Pif + i));
    assert(bus.hw().mi().irq());
    bus.write32(Si + 0x18, 0);
    assert((bus.read32(Si + 0x18) & 0x1000) == 0);
    assert(!bus.hw().mi().irq());
}

void test_vi_scanline_interrupt() {
    Bus bus;
    Cpu cpu;
    cpu.setPc(Kseg0 + 0x400);
    writeProgram(bus, 0x400, {0x34010401, 0x40816000, 0});
    cpu.step(bus);
    cpu.step(bus);
    constexpr u32 Vi = 0x04400000;
    bus.write32(0x0430000C, 0x80); // enable MI VI
    bus.write32(Vi + 0x18, 5); // three progressive lines per field
    bus.write32(Vi + 0x1C, 3093);
    bus.write32(Vi + 0x0C, 3); // low bit ignored in progressive mode
    // Blanking the DAC doesn't stop retrace or VI interrupts.
    bus.hw().tickVideo(5958);
    assert(bus.read32(Vi + 0x10) == 0 && !bus.hw().mi().irq());
    bus.hw().tickVideo(1);
    assert(bus.read32(Vi + 0x10) == 2 && bus.hw().mi().irq());
    cpu.step(bus);
    assert(cpu.pc() == 0xFFFFFFFF80000180ULL && (cpu.cop0()[13] & 0x400));
    bus.write32(Vi + 0x10, 0xFFFFFFFF);
    assert(bus.read32(Vi + 0x10) == 2 && !bus.hw().mi().irq());
    bus.hw().tickVideo(5958);
    assert(bus.read32(Vi + 0x10) == 4 && !bus.hw().mi().irq());
    bus.hw().tickVideo(5959);
    assert(bus.read32(Vi + 0x10) == 0);
}

void test_controller_pak_validation_edges() {
    ControllerPak pak;
    std::array<u8, ControllerPak::Size> blank{};
    assert(!pak.load({blank.data(), blank.size() - 1}));
    assert(pak.type() == PakType::None);
    assert(!pak.dirty() && !pak.rumbling());
    assert(pak.info(false) == 2);

    const auto good = static_cast<u16>(0x0020 | ControllerPak::addressChecksum(0x0020));
    const auto bad = static_cast<u16>(good ^ 1);
    std::array<u8, 32> reply{};
    reply.fill(0xAA);
    assert(pak.read(good, reply) == 0xFF);
    assert(std::all_of(reply.begin(), reply.end(), [](u8 value) { return value == 0; }));
    const auto noPakCrc = ControllerPak::dataCrc(reply);
    assert(pak.write(good, reply) == (noPakCrc ^ 0xFF));
    assert(!pak.dirty());

    pak.attach(PakType::Controller);
    assert(pak.info(false) == 3); // insertion reported once
    assert(pak.info(false) == 1);
    reply.fill(0xAA);
    assert(pak.read(bad, reply) == 0xFF);
    assert(std::all_of(reply.begin(), reply.end(), [](u8 value) { return value == 0; }));
    assert((pak.info(false) & 4) != 0); // bad-address flag is sticky until read

    reply.fill(0x5A);
    const auto crc = ControllerPak::dataCrc(reply);
    assert(pak.write(good, reply) == crc && pak.dirty());
    reply.fill(0);
    assert(pak.read(good, reply) == crc && reply.front() == 0x5A);

    pak.attach(PakType::Rumble);
    assert(pak.info(false) == 3);
    const auto unlock = static_cast<u16>(0x8000 | ControllerPak::addressChecksum(0x8000));
    const auto motor = static_cast<u16>(0xC000 | ControllerPak::addressChecksum(0xC000));
    std::array<u8, 32> command{};
    command.front() = 0x40;
    assert(pak.write(unlock, command) == ControllerPak::dataCrc(command));
    reply.fill(0);
    assert(pak.read(unlock, reply) == ControllerPak::dataCrc(reply) && reply.front() == 0);
    command.front() = 0x80;
    assert(pak.write(unlock, command) == ControllerPak::dataCrc(command));
    reply.fill(0);
    assert(pak.read(unlock, reply) == ControllerPak::dataCrc(reply) && reply.front() == 0x80);
    command.front() = 0;
    assert(pak.write(motor, command) == ControllerPak::dataCrc(command) && !pak.rumbling());
    command.front() = 1;
    assert(pak.write(motor, command) == ControllerPak::dataCrc(command) && pak.rumbling());
    reply.fill(0);
    assert(pak.read(motor, reply) == ControllerPak::dataCrc(reply) && reply.front() == 1);

    pak.attach(PakType::None);
    assert(pak.info(false) == 2 && !pak.rumbling());
}

void test_pif_joybus_and_cic() {
    Bus bus;
    constexpr u32 Pif = 0x1FC007C0;
    constexpr u32 Si = 0x04800000;
    const auto exchange = [&](const std::array<u8, 64>& request) {
        for (u32 i = 0; i < request.size(); ++i) bus.write8(0x200 + i, request[i]);
        bus.write32(Si, 0x200);
        bus.write32(Si + 0x10, Pif);
        assert(bus.hw().tickDma(bus));
        bus.write32(Si, 0x300);
        bus.write32(Si + 4, Pif);
        assert(bus.hw().tickDma(bus));
    };
    std::array<u8, 64> packet{0xFF, 1, 3, 0, 0, 0, 0, 1, 3, 0xFF, 0, 0, 0, 0xFE};
    packet.back() = 0x09; // terminate boot, configure frame
    exchange(packet);
    assert(bus.read8(0x305) == 0 && bus.read8(0x304) == 5 && bus.read8(0x306) == 2);
    assert(bus.read8(0x308) == 0x83 && bus.read8(0x33F) == 0);
    assert(bus.hw().si().bootTerminated);

    packet = {1, 4, 1, 0xFF, 0xFF, 0xFF, 0xFF, 0xFE};
    packet.back() = 1;
    exchange(packet);
    assert(bus.read8(0x301) == 4);
    for (u32 i = 3; i < 7; ++i) assert(bus.read8(0x300 + i) == 0);

    packet = {3, 33, 2, 0, 0};
    packet[38] = 0xFE;
    packet.back() = 1;
    exchange(packet);
    assert(bus.read8(0x301) == 33 && bus.read8(0x325) == 0xFF);
    for (u32 i = 5; i < 37; ++i) assert(bus.read8(0x300 + i) == 0);

    packet = {35, 1, 3, 0, 0};
    packet[38] = 0xFE;
    packet.back() = 1;
    exchange(packet);
    assert(bus.read8(0x301) == 1 && bus.read8(0x325) == 0xFF);

    packet = {0, 3, 33, 2, 0, 0}; // same read on disconnected port 1
    packet[39] = 0xFE;
    packet.back() = 1;
    exchange(packet);
    assert(bus.read8(0x302) == (0x80 | 33));

    auto& pak = bus.hw().si().paks[0];
    pak.attach(PakType::Controller);
    packet = {1, 3, 0};
    packet.back() = 1;
    exchange(packet);
    assert(bus.read8(0x305) == 3); // insertion reported once
    exchange(packet);
    assert(bus.read8(0x305) == 1);
    const auto address = static_cast<u16>(0x7FE0 | ControllerPak::addressChecksum(0x7FE0));
    assert(ControllerPak::addressChecksum(0x8000) == 1);
    assert(ControllerPak::addressChecksum(0x0020) == 0x15);
    packet = {35, 1, 3, static_cast<u8>(address >> 8), static_cast<u8>(address)};
    for (unsigned i = 0; i < 32; ++i) packet[5 + i] = static_cast<u8>(i);
    packet[38] = 0xFE;
    packet.back() = 1;
    exchange(packet);
    assert(bus.read8(0x325) == 0x33);
    const auto saved = pak.image();
    ControllerPak restored;
    assert(restored.load(saved));
    restored.attach(PakType::Controller);
    assert(restored.info(false) == 3);
    std::array<u8, 32> reply{};
    assert(restored.read(address ^ 1, reply) == 0xFF && reply[31] == 0);
    assert(restored.info(false) == 5); // inserted + address error
    assert(restored.read(address, reply) == 0x33 && reply[31] == 31);
    packet = {3, 33, 2, static_cast<u8>(address >> 8), static_cast<u8>(address)};
    packet[38] = 0xFE;
    packet.back() = 1;
    exchange(packet);
    assert(bus.read8(0x325) == 0x33 && bus.read8(0x324) == 31);
    pak.attach(PakType::Rumble);
    assert(pak.info(false) == 3);
    const auto motor = static_cast<u16>(0xC000 | ControllerPak::addressChecksum(0xC000));
    reply.fill(1);
    assert(pak.write(motor, reply) == ControllerPak::dataCrc(reply));
    assert(pak.rumbling());
    reply.fill(0);
    assert(pak.write(motor, reply) == 0 && !pak.rumbling());

    bus.hw().si().cic6105 = true;
    packet = {};
    for (u8 i = 0; i < 15; ++i) packet[0x30 + i] = i;
    packet.back() = 2;
    exchange(packet);
    constexpr std::array<u8, 15> response{
        0xBF, 0x94, 0xE0, 0x4D, 0x3B, 0x5E, 0xC4, 0xE9, 0x91, 0x1E, 0xC8, 0xCD, 0x33, 0x72, 0xAE};
    for (u32 i = 0; i < response.size(); ++i) assert(bus.read8(0x330 + i) == response[i]);
    assert(bus.read8(0x33F) == 0);
}

void test_pi_ri_register_conformance() {
    Bus bus;
    constexpr u32 Pi = 0x04600000;
    constexpr u32 Ri = 0x04700000;

    // PI addresses use a 24-bit DRAM field and ignore bit 0 of both addresses.
    bus.write32(Pi + 0x00, 0xFFFFFF03);
    assert(bus.read32(Pi + 0x00) == 0x00FFFF02);
    bus.write32(Pi + 0x04, 0x10000103);
    assert(bus.read32(Pi + 0x04) == 0x10000102);

    // Length writes latch count-1, set DMA busy, and reject further non-status
    // writes with the PI error bit until the transfer is reset.
    bus.write32(Pi + 0x0C, 0xFFFFFF03);
    assert(bus.read32(Pi + 0x0C) == 0x00FFFF04);
    assert((bus.read32(Pi + 0x10) & 1) != 0);
    bus.write32(Pi + 0x00, 0x00000800);
    assert(bus.read32(Pi + 0x00) == 0x00FFFF02);
    assert((bus.read32(Pi + 0x10) & 0x04) != 0);
    bus.write32(Pi + 0x10, 0x01);
    assert((bus.read32(Pi + 0x10) & 0x05) == 0);

    const std::array<std::pair<u32, u32>, 8> bsdFields{{
        {0x14, 0xFF}, {0x18, 0xFF}, {0x1C, 0x0F}, {0x20, 0x03},
        {0x24, 0xFF}, {0x28, 0xFF}, {0x2C, 0xFF}, {0x30, 0xFF},
    }};
    for (const auto& [offset, expected] : bsdFields) {
        bus.write32(Pi + offset, 0xFFFFFFFF);
        assert(bus.read32(Pi + offset) == expected);
    }

    // PI and RI register blocks repeat at their documented 0x40/0x20 strides.
    bus.write32(Pi + 0x40, 0x00000400);
    assert(bus.read32(Pi + 0x00) == 0x00000400);

    const std::array<u32, 8> riValues{
        0x00000011, 0x22222222, 0x33333333, 0x44444444,
        0x55555555, 0x66666666, 0x77777777, 0x88888888,
    };
    for (u32 i = 0; i < riValues.size(); ++i) {
        bus.write32(Ri + 4 * i, riValues[i]);
        assert(bus.read32(Ri + 4 * i) == riValues[i]);
        assert(bus.read32(Ri + 0x100 + 4 * i) == riValues[i]);
    }
}

void test_pi_dma() {
    // Create a tiny cartridge and verify PI DMA copies bytes to RDRAM.
    constexpr size_t RomSize = 1024 * 1024;
    std::vector<u8> rom(RomSize, 0);
    rom[0] = 0x80; rom[1] = 0x37; rom[2] = 0x12; rom[3] = 0x40;
    rom[0x20] = 'T'; rom[0x21] = 'E'; rom[0x22] = 'S'; rom[0x23] = 'T';
    rom[0x100] = 0xDE; rom[0x101] = 0xAD; rom[0x102] = 0xBE; rom[0x103] = 0xEF;

    Cartridge cart;
    assert(cart.loadFromBuffer(rom, RomFormat::Z64));

    Bus bus;
    bus.mountCartridge(&cart);

    bus.write32(0x04600000, 0x00000200); // PI_DRAM_ADDR
    bus.write32(0x04600004, 0x10000100); // PI_CART_ADDR
    bus.write32(0x0460000C, 3);          // PI_WR_LEN (cart -> RDRAM)
    assert((bus.read32(0x04600010) & 1) != 0);

    // Drain the DMA in chunks.
    for (int i = 0; i < 10 && bus.hw().pi().dmaWriteActive(); ++i) {
        bus.hw().tickDma(bus);
    }

    assert(bus.read32(0x00000200) == 0xDEADBEEF);
    assert((bus.read32(0x04600010) & 9) == 8);
    assert((bus.hw().mi().interrupt & 0x10) != 0);
    bus.write32(0x04600010, 2);
    assert((bus.hw().mi().interrupt & 0x10) == 0);
}

void test_flash_ram_pi() {
    Emulator emu;
    auto& bus = emu.bus();
    auto& flash = bus.flash();
    flash.attach(true);
    bus.setPiSaveMedia(Bus::PiSaveMedia::FlashRam);
    constexpr u32 Pi = 0x04600000;
    const auto dma = [&](u32 dram, u32 cart, u32 count, bool toFlash) {
        bus.write32(Pi, dram);
        bus.write32(Pi + 4, cart);
        bus.write32(Pi + (toFlash ? 8 : 12), count - 1);
        assert(bus.hw().tickDma(bus));
        assert(bus.hw().mi().interrupt & 0x10);
        bus.write32(Pi + 0x10, 2);
    };
    const auto command = [&](u32 v) { bus.write32(FlashRam::Command, v); };
    // Cartridges also accept the command latch as two 16-bit stores.
    bus.write16(FlashRam::Command, 0xE100);
    bus.write16(FlashRam::Command + 2, 0x0000);
    dma(0x1000, FlashRam::Base, 8, false);
    assert(bus.read64(0x1000) == 0x1111800100C2001DULL);
    constexpr u32 PageOffset = 0x237 * FlashRam::Page;
    for (u32 i = 0; i < FlashRam::Page; ++i) bus.write8(0x2000 + i, static_cast<u8>(i ^ 0xA5));
    command(0xB4000000);
    dma(0x2000, FlashRam::Base, FlashRam::Page, true);
    command(0xA5000237);
    assert((bus.read32(FlashRam::Base) & 0x00800080) == 0);
    flash.advance(HwRegisters::CpuClockHz / 100);
    assert((bus.read32(FlashRam::Base) & 0x00800080) == 0x00800080);
    command(0xF0000000);
    dma(0x3000, FlashRam::Base + PageOffset, FlashRam::Page, false);
    for (u32 i = 0; i < FlashRam::Page; ++i)
        assert(bus.read8(0x3000 + i) == bus.read8(0x2000 + i));
    assert(flash.dirty());
    FlashRam loaded;
    assert(loaded.load(flash.image()) && loaded.read(PageOffset) == 0xA5);
    command(0x4B000237);
    command(0x78000000);
    assert((bus.read32(FlashRam::Base) & 0x00020002) == 0x00020002);
    flash.advance(HwRegisters::CpuClockHz / 10);
    command(0xF0000000);
    dma(0x3000, FlashRam::Base + PageOffset, FlashRam::Page, false);
    for (u32 i = 0; i < FlashRam::Page; ++i) assert(bus.read8(0x3000 + i) == 0xFF);
}

void test_eeprom_joybus() {
    Emulator emu;
    auto& bus = emu.bus();
    auto& si = bus.hw().si();
    auto& eeprom = bus.eeprom();

    // Both sizes share the same Joybus command encoding.
    for (auto type : {Eeprom::Type::Eeprom4K, Eeprom::Type::Eeprom16K}) {
        eeprom.attach(type);
        const u32 capacity = eeprom.size();
        assert(capacity == (type == Eeprom::Type::Eeprom16K ? Eeprom::Size16K : Eeprom::Size4K));

        // Info query: 0x0080 / 0x00C0, busy bit clear when idle.
        si.joybusOffsets.fill(0xFF);
        si.pifRam.fill(0);
        si.pifRam[0] = 1;   // tx
        si.pifRam[1] = 3;   // rx
        si.pifRam[2] = 0;   // info command
        si.joybusOffsets[4] = 0;
        si.preparePifRead({}, &eeprom);
        assert(si.pifRam[3] == static_cast<u8>(eeprom.infoId() >> 8));
        assert(si.pifRam[4] == static_cast<u8>(eeprom.infoId()));
        assert(si.pifRam[5] == 0);

        // Write block 7 with 8 deterministic bytes.
        std::array<u8, 8> payload{};
        for (u32 i = 0; i < 8; ++i) payload[i] = static_cast<u8>(0xA0 + i);
        si.pifRam.fill(0);
        si.pifRam[0] = 10;  // tx: command + block + 8 data
        si.pifRam[1] = 1;   // rx: status
        si.pifRam[2] = 5;   // write command
        si.pifRam[3] = 7;   // block
        std::copy_n(payload.begin(), 8, si.pifRam.begin() + 4);
        si.joybusOffsets[4] = 0;
        si.preparePifRead({}, &eeprom);
        assert(si.pifRam[12] == 0);  // accepted
        assert(eeprom.busy() && eeprom.dirty());

        // A second write while busy must return 0x80.
        si.pifRam.fill(0);
        si.pifRam[0] = 10; si.pifRam[1] = 1; si.pifRam[2] = 5; si.pifRam[3] = 8;
        si.joybusOffsets[4] = 0;
        si.preparePifRead({}, &eeprom);
        assert(si.pifRam[12] == 0x80);

        // Info while busy reports the status bit.
        si.pifRam.fill(0);
        si.pifRam[0] = 1; si.pifRam[1] = 3; si.pifRam[2] = 0;
        si.joybusOffsets[4] = 0;
        si.preparePifRead({}, &eeprom);
        assert((si.pifRam[5] & 0x80) != 0);

        eeprom.advance(Eeprom::WriteCycles + 1);
        assert(!eeprom.busy());

        // Read block 7 back.
        si.pifRam.fill(0);
        si.pifRam[0] = 2; si.pifRam[1] = 8; si.pifRam[2] = 4; si.pifRam[3] = 7;
        si.joybusOffsets[4] = 0;
        si.preparePifRead({}, &eeprom);
        for (u32 i = 0; i < 8; ++i) assert(si.pifRam[4 + i] == payload[i]);

        // Persistence image round-trip.
        Eeprom reloaded;
        reloaded.attach(type);
        assert(reloaded.load(eeprom.image()));
        assert(reloaded.readBlock(7, std::span<u8>(si.pifRam.begin(), 8)));
        for (u32 i = 0; i < 8; ++i) assert(si.pifRam[i] == payload[i]);
    }

    // 4 Kbit block indices wrap modulo 64.
    eeprom.attach(Eeprom::Type::Eeprom4K);
    std::array<u8, 8> wrapData{};
    assert(eeprom.writeBlock(1, std::array<u8, 8>{1, 2, 3, 4, 5, 6, 7, 8}));
    eeprom.advance(Eeprom::WriteCycles + 1);
    assert(eeprom.readBlock(65, wrapData) && wrapData[0] == 1);
}

void test_auto_save_detection() {
    // Auto cartridge save selection must come from guest bus activity. No ROM
    // header, filename or address is consulted beyond the device's real range.
    {
        Bus bus;
        bus.configureAutoPiSave(false);
        assert(bus.piSaveMedia() == Bus::PiSaveMedia::None);
        assert(bus.sram().attached() && bus.flash().attached());
        bus.write8(Sram::Base + 7, 0x5A);
        assert(bus.piSaveMedia() == Bus::PiSaveMedia::Sram);
        assert(bus.read8(Sram::Base + 7) == 0x5A && bus.sram().dirty());
        Sram loaded;
        loaded.attach(true);
        assert(loaded.load(bus.sram().image()) && loaded.read(7) == 0x5A);
    }
    {
        Bus bus;
        bus.configureAutoPiSave(false);
        bus.write32(FlashRam::Command, 0xB4000000);
        assert(bus.piSaveMedia() == Bus::PiSaveMedia::FlashRam);
        bus.write8(FlashRam::Base, 0x5A);
        bus.write32(FlashRam::Command, 0xA5000000);
        bus.flash().advance(HwRegisters::CpuClockHz);
        bus.write32(FlashRam::Command, 0xF0000000);
        assert(bus.flash().dirty() && bus.flash().read(0) == 0x5A);
        assert(bus.flash().image().size() == FlashRam::Size);
        FlashRam loaded;
        assert(loaded.load(bus.flash().image()) && loaded.read(0) == 0x5A);
    }
    {
        Bus bus;
        bus.configureAutoPiSave(false);
        auto& eeprom = bus.eeprom();
        eeprom.attach(Eeprom::Type::Eeprom4K);
        eeprom.enableAutoExpansion();
        assert(eeprom.attached() && eeprom.type() == Eeprom::Type::Eeprom4K);
        auto& si = bus.hw().si();
        si.pifRam.fill(0);
        si.pifRam[0] = 10; si.pifRam[1] = 1; si.pifRam[2] = 5; si.pifRam[3] = 70;
        std::fill_n(si.pifRam.begin() + 4, 8, 0x5B);
        si.joybusOffsets[4] = 0;
        si.preparePifRead({}, &eeprom);
        assert(eeprom.type() == Eeprom::Type::Eeprom16K);
        assert(eeprom.accessed() && eeprom.dirty());
        eeprom.advance(Eeprom::WriteCycles + 1);
        std::array<u8, Eeprom::BlockSize> block{};
        assert(eeprom.readBlock(70, block) && block[0] == 0x5B);
        Eeprom loaded;
        loaded.attach(Eeprom::Type::Eeprom16K);
        assert(loaded.load(eeprom.image()) && loaded.readBlock(70, block) && block[0] == 0x5B);

        // An explicitly attached 4 Kbit part has no auto-expansion and wraps.
        Eeprom fixed;
        fixed.attach(Eeprom::Type::Eeprom4K);
        const std::array<u8, Eeprom::BlockSize> data{1, 2, 3, 4, 5, 6, 7, 8};
        assert(fixed.writeBlock(70, data));
        fixed.advance(Eeprom::WriteCycles + 1);
        assert(fixed.readBlock(6, block) && block == data);
    }
}

void test_sram_pi() {
    Emulator emu;
    auto& bus = emu.bus();
    auto& sram = bus.sram();
    sram.attach(true);
    bus.setPiSaveMedia(Bus::PiSaveMedia::Sram);
    constexpr u32 Pi = 0x04600000;

    // Direct MMIO byte/word access through SRAM's full 32 KiB mirror.
    bus.write8(Sram::Base + 0x1234, 0x5A);
    assert(bus.read8(Sram::Base + 0x1234) == 0x5A);
    assert(bus.read8(Sram::Base + 0x9234) == 0x5A); // hardware mirror

    // PI DMA write RDRAM -> SRAM and read back.
    for (u32 i = 0; i < 64; ++i) bus.write8(0x2000 + i, static_cast<u8>(i ^ 0xC3));
    bus.write32(Pi, 0x2000);
    bus.write32(Pi + 4, Sram::Base + 0x100);
    bus.write32(Pi + 8, 63);
    assert(bus.hw().tickDma(bus));
    bus.write32(Pi + 0x10, 2);

    bus.write32(Pi, 0x3000);
    bus.write32(Pi + 4, Sram::Base + 0x100);
    bus.write32(Pi + 12, 63);
    assert(bus.hw().tickDma(bus));
    bus.write32(Pi + 0x10, 2);
    for (u32 i = 0; i < 64; ++i)
        assert(bus.read8(0x3000 + i) == static_cast<u8>(i ^ 0xC3));
    assert(sram.dirty());

    Sram reloaded;
    reloaded.attach(true);
    assert(reloaded.load(sram.image()));
    assert(reloaded.read(0x100) == 0xC3);
}

void test_video_frame_interleaves_cpu_and_dma() {
    std::vector<u8> rom(0x2000);
    rom[0] = 0x80; rom[1] = 0x37; rom[2] = 0x12; rom[3] = 0x40;
    rom[8] = 0x80; rom[10] = 0x04;
    rom[0x100] = 0x12; rom[0x101] = 0x34; rom[0x102] = 0x56; rom[0x103] = 0x78;
    Cartridge cart;
    assert(cart.loadFromBuffer(std::move(rom)));
    Emulator emu;
    assert(emu.loadCartridge(std::move(cart)));
    auto& bus = emu.bus();
    bus.write32(0x04600000, 0x2000);
    bus.write32(0x04600004, 0x10000100);
    bus.hw().setVideoClock(HwRegisters::CpuClockHz);
    bus.write32(0x04400018, 3);
    bus.write32(0x0440001C, 3); // one field in eight nominal cycles
    writeProgram(bus, 0x400, {
        lui(1, 0xA460), lui(4, 0xA000), addiu(2, 0, 3),
        sw(2, 1, 12), // launch cartridge-to-RDRAM transfer
        lw(3, 4, 0x2000), // the very next CPU step must see the DMA result
    });
    assert(emu.runVideoFrame() == 8);
    assert(emu.cpu().gpr()[3] == 0x12345678);
    const auto counters = emu.counters();
    assert(counters.cycles == 8 && counters.instructions == 8 && counters.viFields == 1);
    assert(counters.sampledSteps == 0 && counters.cacheHits > 0);
    emu.setTimingEnabled(true);
    assert(emu.runVideoFrame() == 8);
    assert(emu.counters().cycles == 16 && emu.counters().sampledSteps > 0);
}

void test_event_deadline_interrupts() {
    for (bool compare : {false, true}) {
        for (bool batched : {false, true}) {
            Emulator emu;
            emu.setEventBatchingEnabled(batched);
            auto& bus = emu.bus();
            writeProgram(bus, 0x200, {
                ori(1, 0, compare ? 0x8001 : 0x0401), mtc0(1, 12), ori(2, 0, 4),
                0, mtc0(0, 9), mtc0(2, 11),
            });
            emu.cpu().setPc(0xFFFFFFFF80000200ULL);
            for (int i = 0; i < 6; ++i) emu.runOneStep();
            bus.hw().setVideoClock(HwRegisters::CpuClockHz);
            bus.write32(0x04400018, 3);
            bus.write32(0x0440001C, 3); // VI line at cycle 4, field at cycle 8
            bus.write32(0x0440000C, 2);
            bus.write32(0x0430000C, 0x80); // enable MI VI
            assert(bus.hw().cyclesToDeviceEvent(8) == 4);
            for (u32 a = 0x100; a < 0x120; a += 4) bus.write32(a, addiu(3, 3, 1));
            emu.cpu().setPc(0xFFFFFFFF80000100ULL);
            assert(emu.runVideoFrame() == 8);
            assert(emu.cpu().gpr()[3] == (compare ? 5 : 4));
            assert(emu.cpu().cop0()[14] == (compare ? 0xFFFFFFFF80000114ULL : 0xFFFFFFFF80000110ULL));
            assert(emu.cpu().cop0()[9] == 5);
            assert((emu.cpu().cop0()[13] & 0x8400) == 0x8400);
            assert(emu.counters().cycles == 14);
            if (batched) assert(emu.counters().deviceTicks < emu.counters().cycles);
        }
    }

    Bus bus;
    auto& hw = bus.hw();
    hw.setVideoClock(HwRegisters::CpuClockHz);
    bus.write32(0x04500004, 8);
    bus.write32(0x04500008, 1);
    bus.write32(0x04500010, 2);
    assert(hw.cyclesToDeviceEvent(20) == 3);
    assert(!hw.tickAudio(bus, 2) && hw.cyclesToDeviceEvent(20) == 1);
    assert(hw.tickAudio(bus) && hw.cyclesToDeviceEvent(20) == 3);

    for (bool batched : {false, true}) {
        Emulator emu;
        emu.setEventBatchingEnabled(batched);
        auto& bus = emu.bus();
        writeProgram(bus, 0x100, {lui(1, 0xA440), 0, 0, 0, lw(3, 1, 16), lw(4, 1, 16), 0, 0, 0});
        emu.cpu().setPc(0xFFFFFFFF80000100ULL);
        emu.runOneStep();
        bus.hw().setVideoClock(HwRegisters::CpuClockHz);
        bus.write32(0x04400018, 3);
        bus.write32(0x0440001C, 3);
        assert(emu.runVideoFrame() == 8);
        assert(emu.cpu().gpr()[3] == 0 && emu.cpu().gpr()[4] == 2);
        if (batched) assert(emu.counters().deviceTicks == 3);
    }
}

void test_frame_slot_handoff() {
    video::FrameMailbox box(16);
    auto* first = box.acquire();
    assert(first && box.take() == nullptr);
    first->width = first->height = 1;
    box.storage(*first)[0] = 17;
    box.publish(*first);
    assert(box.take() == first && box.lastPixels()[0] == 17);
    auto* second = box.acquire();
    assert(second && second != first);
    second->width = second->height = 1;
    box.storage(*second)[0] = 35;
    box.publish(*second);
    auto* third = box.acquire();
    auto* fourth = box.acquire();
    assert(third && fourth && box.acquire() == nullptr);
    assert(first->pixels()[0] == 17 && box.take() == second);
    fourth->release();
    third->release();
    second->release();
    first->release();
    box.reset();
    auto* free = box.acquire();
    assert(free && box.lastPixels().empty());
    free->release();
}

void test_scanout_dirty_tracking() {
    Emulator emu;
    auto& bus = emu.bus();
    bus.write32(0x04400000, 3);
    bus.write32(0x04400004, 0x2000);
    bus.write32(0x04400008, 4);
    bus.write32(0x04400024, (100 << 16) | 104);
    bus.write32(0x04400028, (30 << 16) | 34);
    bus.write32(0x04400030, 1024);
    bus.write32(0x04400034, 1024);
    bus.write32(0x2000, 0xFF0000FF);
    Emulator::ScanoutHistory history;
    std::array<u32, 8> previous{}, next{};
    assert(emu.scanoutForPresentation(previous, {}, history));
    assert(previous[0] == 0xFF0000FF);
    assert(!emu.scanoutForPresentation(next, previous, history) && next[0] == 0);
    bus.write32(0x2000, 0xFF0000FF);
    assert(!emu.scanoutForPresentation(next, previous, history));
    bus.write32(0x2000, 0x00FF00FF);
    assert(emu.scanoutForPresentation(next, previous, history) && next[0] == 0xFF00FF00);
    emu.scanout();
    assert(std::equal(next.begin(), next.end(), emu.framebuffer().begin()));
    bus.write32(0x04400000, 0);
    assert(emu.scanoutForPresentation(previous, next, history));
    assert(!emu.scanoutForPresentation(previous, {}, history));
}

void test_packed_scanout_active_window() {
    for (const u32 bpp : {2u, 4u}) {
        Emulator emu;
        auto& bus = emu.bus();
        constexpr u32 vi = 0x04400000, origin = 0x2000, pitch = 256, stride = 6;
        bus.write32(vi, bpp == 2 ? 2 : 3);
        bus.write32(vi + 0x04, origin);
        bus.write32(vi + 0x08, stride);
        bus.write32(vi + 0x24, (100 << 16) | 108);
        bus.write32(vi + 0x28, (30 << 16) | 38);
        bus.write32(vi + 0x30, (1024 << 16) | 512);
        bus.write32(vi + 0x34, (0xD00 << 16) | 1024); // Fractional phase, plus two ignored bits.
        for (u32 y = 0; y < 6; ++y) for (u32 x = 0; x < stride; ++x) {
            const u32 addr = origin + (y * stride + x) * bpp;
            if (bpp == 2) bus.write16(addr, 0xC000 | ((y * stride + x) << 1) | 1);
            else bus.write32(addr, 0x12340000 | ((y * stride + x) << 8) | 0xAB);
        }
        Emulator::ScanoutHistory history;
        std::array<u32, pitch> out{}, same{};
        assert(emu.scanoutForPresentation(out, {}, history, pitch));
        assert(emu.framebufferWidth() == 4 && emu.framebufferHeight() == 4);
        assert(emu.scanoutBytesPerPixel() == bpp && emu.scanoutPitch() == pitch);
        const auto* raw = reinterpret_cast<const u8*>(out.data());
        for (u32 y = 0; y < 4; ++y) {
            for (u32 x = 0; x < 4; ++x) {
                const auto* p = raw + y * pitch + x * bpp;
                const auto id = y * stride + x + 1;
                if (bpp == 2) assert(p[0] == 0xC0 && p[1] == (id * 2 + 1));
                else assert(p[0] == 0x12 && p[1] == 0x34 && p[2] == id && p[3] == 0xAB);
            }
            assert(std::all_of(raw + y * pitch + 4 * bpp, raw + (y + 1) * pitch,
                               [](u8 padding) { return padding == 0; }));
        }
        const auto previous = std::span<const u32>(out);
        bus.write8(origin, 0x88); // Same RDRAM line, outside the active window.
        assert(!emu.scanoutForPresentation(same, previous, history, pitch));
        bus.write8(origin + (3 * stride + 4) * bpp, 0xFE); // Bottom-right active pixel.
        assert(emu.scanoutForPresentation(same, previous, history, pitch));
    }
}

void test_hle_libultra() {
    HleLibultra hle;

    // Threads
    const u32 t1 = hle.createThread(0x80001000, 0x80200000, 10, 0);
    const u32 t2 = hle.createThread(0x80002000, 0x80201000, 20, 0);
    hle.startThread(t1);
    assert(hle.currentThread() != nullptr);
    assert(hle.currentThread()->id == t1);
    hle.startThread(t2);
    hle.yieldThread();
    assert(hle.currentThread()->id == t2); // higher priority wins
    hle.stopThread(t2);
    assert(hle.currentThread()->id == t1);

    // Message queues
    hle.createMesgQueue(0x80100000, 4);
    u32 msg = 0;
    assert(!hle.recvMesg(0x80100000, msg, HLE_MESG_NOBLOCK));
    assert(hle.sendMesg(0x80100000, 0x12345678, HLE_MESG_NOBLOCK));
    assert(hle.recvMesg(0x80100000, msg, HLE_MESG_NOBLOCK));
    assert(msg == 0x12345678);

    // Timers
    hle.createMesgQueue(0x80100004, 2);
    hle.createTimer(0x80101000, 1000, 0x80100004, 0xABCD);
    hle.tick(999);
    assert(!hle.recvMesg(0x80100004, msg, HLE_MESG_NOBLOCK));
    hle.tick(1000);
    assert(hle.recvMesg(0x80100004, msg, HLE_MESG_NOBLOCK));
    assert(msg == 0xABCD);

    // PI DMA via HLE shim
    constexpr size_t RomSize = 1024 * 1024;
    std::vector<u8> rom(RomSize, 0);
    rom[0] = 0x80; rom[1] = 0x37; rom[2] = 0x12; rom[3] = 0x40;
    rom[0x200] = 0xCA; rom[0x201] = 0xFE; rom[0x202] = 0xBA; rom[0x203] = 0xBE;
    Cartridge cart;
    assert(cart.loadFromBuffer(rom, RomFormat::Z64));
    Bus bus;
    bus.mountCartridge(&cart);
    hle.piDmaRead(0x10000200, 0x00000400, 4, bus);
    assert(bus.read32(0x00000400) == 0xCAFEBABE);

    // Graphics hooks: matrix load, multiply, push, viewport.
    const std::array<float, 16> scale = {
        2.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 3.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 4.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f,
    };
    hle.matrix(scale.data(), HLE_MTX_LOAD | HLE_MTX_NOPUSH);
    const auto& m1 = hle.currentMatrix();
    assert(m1[0] == 2.0f && m1[5] == 3.0f && m1[10] == 4.0f);

    const std::array<float, 16> trans = {
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        5.0f, 6.0f, 7.0f, 1.0f,
    };
    hle.matrix(trans.data(), HLE_MTX_LOAD | HLE_MTX_NOPUSH);
    hle.matrix(scale.data(), HLE_MTX_MUL | HLE_MTX_PUSH);
    const auto& m2 = hle.currentMatrix();
    // row-major row-vector convention: M' = M * N => translation scaled by N's diagonal.
    assert(m2[0] == 2.0f && m2[5] == 3.0f && m2[10] == 4.0f);
    assert(m2[12] == 10.0f && m2[13] == 18.0f && m2[14] == 28.0f);

    HleViewport vp{0.0f, 0.0f, 320.0f, 240.0f, 0.0f, 1.0f};
    hle.viewport(vp);
    assert(hle.currentViewport().width == 320.0f);
}

// RDP command encoding helpers for tests (fixed-point values are integer pixels * 4).
[[nodiscard]] u64 rdpSetColorImage(u32 width, u32 addr) {
    return (0x3FULL << 56) | (3ULL << 51) | ((static_cast<u64>(width - 1) & 0x3FF) << 32) |
           (addr & 0x3FFFFFF);
}
[[nodiscard]] u64 rdpSetScissor(u32 ulx, u32 uly, u32 lrx, u32 lry) {
    return (0x2DULL << 56) |
           ((static_cast<u64>(ulx * 4) & 0xFFF) << 44) |
           ((static_cast<u64>(uly * 4) & 0xFFF) << 32) |
           ((static_cast<u64>(lrx * 4) & 0xFFF) << 12) |
           (static_cast<u64>(lry * 4) & 0xFFF);
}
[[nodiscard]] u64 rdpSetFillColor(u32 rgba) {
    return (0x37ULL << 56) | rgba;
}
[[nodiscard]] u64 rdpFillRect(u32 ulx, u32 uly, u32 lrx, u32 lry) {
    return (0x36ULL << 56) |
           ((static_cast<u64>(lrx * 4) & 0xFFF) << 44) |
           ((static_cast<u64>(lry * 4) & 0xFFF) << 32) |
           ((static_cast<u64>(ulx * 4) & 0xFFF) << 12) |
           (static_cast<u64>(uly * 4) & 0xFFF);
}
[[nodiscard]] u64 rdpSyncFull() { return (0x29ULL << 56); }
[[nodiscard]] u64 rdpTriangleBase(u32 yh, u32 ym, u32 yl, u64 flags) {
    return (0x08ULL << 56) | flags |
           ((static_cast<u64>(yl * 4) & 0x3FFF) << 32) |
           ((static_cast<u64>(ym * 4) & 0x3FFF) << 16) |
           (static_cast<u64>(yh * 4) & 0x3FFF);
}
[[nodiscard]] u64 rdpEdgeFixed(s32 x16, s32 dxdy16) {
    const u32 xi = static_cast<u32>(x16 >> 16) & 0xFFF;
    const u32 xf = static_cast<u32>(x16) & 0xFFFF;
    const u32 di = static_cast<u32>(dxdy16 >> 16) & 0x3FFF;
    const u32 df = static_cast<u32>(dxdy16) & 0xFFFF;
    return (static_cast<u64>(xi) << 48) | (static_cast<u64>(xf) << 32) |
           (static_cast<u64>(di) << 16) | df;
}
[[nodiscard]] u64 rdpEdge(s32 x, s32 dxdy) {
    return rdpEdgeFixed(x << 16, dxdy);
}
[[nodiscard]] u64 rdpTriangleBaseQ(u32 yhq, u32 ymq, u32 ylq, u64 flags) {
    return (0x08ULL << 56) | flags |
           ((static_cast<u64>(ylq) & 0x3FFF) << 32) |
           ((static_cast<u64>(ymq) & 0x3FFF) << 16) |
           (static_cast<u64>(yhq) & 0x3FFF);
}
[[nodiscard]] u64 rdpRawTriangle(u32 op, u32 tile, u32 level, bool leftMajor,
                                 u32 yhq, u32 ymq, u32 ylq, bool shade, bool textured, bool zbuffer) {
    return (static_cast<u64>(op) << 56) | (shade ? 1ULL << 58 : 0ULL) |
           (textured ? 1ULL << 57 : 0ULL) | (zbuffer ? 1ULL << 56 : 0ULL) |
           (leftMajor ? 1ULL << 55 : 0ULL) | ((static_cast<u64>(level) & 7) << 51) |
           ((static_cast<u64>(tile) & 7) << 48) |
           ((static_cast<u64>(ylq) & 0x3FFF) << 32) |
           ((static_cast<u64>(ymq) & 0x3FFF) << 16) | (static_cast<u64>(yhq) & 0x3FFF);
}
[[nodiscard]] u64 rdpRawSetTile(u32 index, u32 fmt, u32 size, u32 line, u32 base, u32 palette,
                                bool clampT, bool mirrorT, u32 maskT, u32 shiftT,
                                bool clampS, bool mirrorS, u32 maskS, u32 shiftS) {
    return (0x35ULL << 56) | ((static_cast<u64>(fmt) & 7) << 53) |
           ((static_cast<u64>(size) & 3) << 51) | ((static_cast<u64>(line) & 511) << 41) |
           ((static_cast<u64>(base) & 511) << 32) | ((static_cast<u64>(index) & 7) << 24) |
           ((static_cast<u64>(palette) & 15) << 20) | (clampT ? 1ULL << 19 : 0ULL) |
           (mirrorT ? 1ULL << 18 : 0ULL) | ((static_cast<u64>(maskT) & 15) << 14) |
           ((static_cast<u64>(shiftT) & 15) << 10) | (clampS ? 1ULL << 9 : 0ULL) |
           (mirrorS ? 1ULL << 8 : 0ULL) | ((static_cast<u64>(maskS) & 15) << 4) |
           (static_cast<u64>(shiftS) & 15);
}
[[nodiscard]] u64 rdpRawSetTileSize(u32 op, u32 index, u32 sl, u32 tl, u32 sh, u32 th) {
    return (static_cast<u64>(op) << 56) | ((static_cast<u64>(sl) & 4095) << 44) |
           ((static_cast<u64>(tl) & 4095) << 32) | ((static_cast<u64>(index) & 7) << 24) |
           ((static_cast<u64>(sh) & 4095) << 12) | (static_cast<u64>(th) & 4095);
}
[[nodiscard]] u64 rdpRawSetCombine(u32 a0, u32 b0, u32 c0, u32 d0,
                                   u32 a1, u32 b1, u32 c1, u32 d1) {
    return (0x3CULL << 56) | ((static_cast<u64>(a0) & 15) << 52) |
           ((static_cast<u64>(c0) & 31) << 47) | ((static_cast<u64>(a1) & 15) << 37) |
           ((static_cast<u64>(c1) & 31) << 32) | ((static_cast<u64>(b0) & 15) << 28) |
           ((static_cast<u64>(b1) & 15) << 24) | ((static_cast<u64>(d0) & 7) << 15) |
           ((static_cast<u64>(d1) & 7) << 6);
}
[[nodiscard]] u64 rdpRawSetCombineFull(u32 a0, u32 b0, u32 c0, u32 d0,
                                       u32 alphaA0, u32 alphaB0, u32 alphaC0, u32 alphaD0,
                                       u32 a1, u32 b1, u32 c1, u32 d1,
                                       u32 alphaA1, u32 alphaB1, u32 alphaC1, u32 alphaD1) {
    return (0x3CULL << 56) | ((static_cast<u64>(a0) & 15) << 52) |
           ((static_cast<u64>(c0) & 31) << 47) | ((static_cast<u64>(alphaA0) & 7) << 44) |
           ((static_cast<u64>(alphaC0) & 7) << 41) | ((static_cast<u64>(a1) & 15) << 37) |
           ((static_cast<u64>(c1) & 31) << 32) | ((static_cast<u64>(b0) & 15) << 28) |
           ((static_cast<u64>(b1) & 15) << 24) | ((static_cast<u64>(alphaA1) & 7) << 21) |
           ((static_cast<u64>(alphaC1) & 7) << 18) | ((static_cast<u64>(d0) & 7) << 15) |
           ((static_cast<u64>(alphaB0) & 7) << 12) | ((static_cast<u64>(alphaD0) & 7) << 9) |
           ((static_cast<u64>(d1) & 7) << 6) | ((static_cast<u64>(alphaB1) & 7) << 3) |
           (static_cast<u64>(alphaD1) & 7);
}
[[nodiscard]] u64 rdpShadeWord(u8 r, u8 g, u8 b, u8 a) {
    return (static_cast<u64>(r & 0x1FF) << 48) |
           (static_cast<u64>(g & 0x1FF) << 32) |
           (static_cast<u64>(b & 0x1FF) << 16) |
           (a & 0x1FF);
}

void test_rdp_renderer() {
    Bus bus;
    HwRegisters hw;
    hw.mi().mask = MI_INTR_DP; // enable DP interrupt so Sync_Full raises irq()
    Rdp rdp;
    rdp.setBus(&bus);
    rdp.setHw(&hw);

    // Small 16x16 framebuffer, color image width = 16.
    const u32 red = 0xFF0000FF;
    const u32 green = 0xFF00FF00;
    const u32 blue = 0xFFFF0000;

    // Build a simple command list in RDRAM.
    u32 addr = 0x1000;
    bus.write64(addr + 0x00, rdpSetColorImage(16, 0x00100000));
    bus.write64(addr + 0x08, rdpSetScissor(0, 0, 16, 16));
    bus.write64(addr + 0x10, (0xEFULL << 56) | (3ULL << 52));
    bus.write64(addr + 0x18, rdpSetFillColor(0xFF0000FF));
    bus.write64(addr + 0x20, rdpFillRect(4, 4, 11, 11));
    bus.write64(addr + 0x28, rdpSetFillColor(0x0000FFFF));
    bus.write64(addr + 0x30, rdpFillRect(12, 12, 20, 20)); // partially clipped
    bus.write64(addr + 0x38, rdpSyncFull());
    addr += 0x40;

    hw.dp().start = 0x1000;
    hw.dp().end = addr;
    hw.dp().current = 0x1000;
    rdp.process();

    // Sync_Full should raise the DP interrupt.
    assert(hw.mi().irq());
    assert(hw.mi().interrupt & MI_INTR_DP);

    // Inside the first rectangle should be red.
    assert(rdp.pixel(4, 4) == red);
    assert(rdp.pixel(11, 11) == red);
    assert(rdp.pixel(7, 7) == red);

    // Outside should remain untouched (black).
    assert(rdp.pixel(3, 4) == 0);
    assert(rdp.pixel(4, 3) == 0);

    // The partially clipped blue rectangle should only affect the intersection.
    assert(rdp.pixel(12, 12) == blue);
    assert(rdp.pixel(15, 15) == blue);
    assert(rdp.pixel(11, 15) == 0); // left of the clipped rect
    assert(rdp.pixel(15, 11) == 0); // above the clipped rect

    // --- Deterministic shaded triangle rasterization ---
    // Triangle: (4,4) top-left, (12,8) middle-right, (4,12) bottom-left.
    // Edges: high-low vertical x=4; high-middle slope +2; middle-low slope -2.
    bus.reset();
    hw.reset();
    rdp.reset();
    rdp.setBus(&bus);
    rdp.setHw(&hw);
    rdp.setFramebufferSize(16, 16);

    addr = 0x1000;
    u32 yh = 4, ym = 8, yl = 12;
    u64 base = rdpTriangleBase(yh, ym, yl, (1ULL << 58) | (1ULL << 55)); // shaded, left-major
    bus.write64(addr + 0x00, rdpSetColorImage(16, 0x00100000));
    bus.write64(addr + 0x08, rdpSetScissor(0, 0, 16, 16));
    bus.write64(addr + 0x10, (0xFCULL << 56) | (31ULL << 32) | (4ULL << 6) | (7ULL << 18) | 4);
    // RGB and alpha: multiplier zero, D = shade.
    bus.write64(addr + 0x18, base);
    bus.write64(addr + 0x20, rdpEdge(12, -2 << 16)); // xl / dxldy at ym
    bus.write64(addr + 0x28, rdpEdge(4, 0));          // xh / dxhdy
    bus.write64(addr + 0x30, rdpEdge(4, 2 << 16));    // xm / dxmdy
    bus.write64(addr + 0x38, rdpShadeWord(0, 255, 0, 255)); // base shade = green
    for (int i = 1; i < 8; ++i) bus.write64(addr + 0x38 + i * 8, 0);
    bus.write64(addr + 0x78, rdpSyncFull());

    hw.dp().start = 0x1000;
    hw.dp().end = addr + 0x80;
    hw.dp().current = 0x1000;
    rdp.process();

    // Count green pixels inside the expected triangle.
    int greenCount = 0;
    for (int y = 0; y < 16; ++y) {
        for (int x = 0; x < 16; ++x) {
            if ((rdp.pixel(x, y) & 0xFFFFFF) == (green & 0xFFFFFF)) ++greenCount;
        }
    }
    // With AA disabled the RDP rejects a fragment when coverage bit 0
    // (x+0, y+0) is outside. The top-left vertex lies exactly on that sample,
    // so the half-open edge leaves pixel (4,4) empty.
    assert(greenCount > 0);
    assert(rdp.pixel(4, 4) == 0);
    assert((rdp.pixel(5, 5) & 0xFFFFFF) == (green & 0xFFFFFF));
    assert((rdp.pixel(8, 8) & 0xFFFFFF) == (green & 0xFFFFFF));
    assert((rdp.pixel(4, 11) & 0xFFFFFF) == (green & 0xFFFFFF));
    assert(rdp.pixel(3, 4) == 0);
    assert(rdp.pixel(8, 12) == 0);
}


void test_rdp_internal_resolution_scale() {
    Bus bus;
    HwRegisters hw;
    hw.mi().mask = MI_INTR_DP;
    Rdp rdp;
    rdp.setBus(&bus);
    rdp.setHw(&hw);
    rdp.setFramebufferSize(16, 16);

    // 2x internal resolution: the internal debug surface grows, but RDRAM
    // writeback stays at the native 16x16 layout. Verify bounds are respected
    // and the native pixel reads back correctly.
    rdp.setResolutionScale(2);
    u32 addr = 0x1000;
    bus.write64(addr + 0x00, rdpSetColorImage(16, 0x00100000));
    bus.write64(addr + 0x08, rdpSetScissor(0, 0, 16, 16));
    bus.write64(addr + 0x10, (0xEFULL << 56) | (3ULL << 52));
    bus.write64(addr + 0x18, rdpSetFillColor(0xFF0000FF));
    bus.write64(addr + 0x20, rdpFillRect(4, 4, 11, 11));
    bus.write64(addr + 0x28, rdpSyncFull());
    hw.dp().start = 0x1000;
    hw.dp().end = addr + 0x30;
    hw.dp().current = 0x1000;
    rdp.process();

    assert(rdp.width() == 32);
    assert(rdp.height() == 32);
    assert(rdp.resolutionScale() == 2);
    assert(rdp.pixel(4, 4) == 0xFF0000FF);
    assert(rdp.pixel(11, 11) == 0xFF0000FF);
    assert(rdp.pixel(3, 4) == 0);

    // Native RDRAM must contain the downsampled (here nearest) pixel and the
    // CPU readback address must be inside RDRAM.
    const u32 nativeAddr = 0x00100000 + (4 * 16 + 4) * 4;
    assert(nativeAddr + 4 <= Memory::RdramSize);
    assert(bus.read32(nativeAddr) != 0);

    // 4x should also stay bounded.
    rdp.setResolutionScale(4);
    assert(rdp.width() == 64);
    assert(rdp.height() == 64);
}

void test_rdp_textured_shaded_depth_and_tlut(u32 scale) {
    Bus bus;
    Rdp rdp;
    rdp.setResolutionScale(scale);
    rdp.setBus(&bus);
    rdp.setHw(&bus.hw());
    constexpr u32 Color = 0x4000, Depth = 0x5000, Palette = 0x6000, Texture = 0x7000;
    constexpr u64 Op = 1ULL << 56;
    constexpr u64 Tile7 = 7ULL << 24;
    constexpr u64 CI4 = 2ULL << 53;
    auto& dp = bus.hw().dp();
    dp.current = 0x1000;
    const auto emit = [&](u64 w) { bus.write64(dp.end, w); dp.end += 8; };
    dp.end = dp.current;
    emit(0x3F * Op | (2ULL << 51) | (7ULL << 32) | Color);
    emit(rdpSetScissor(0, 0, 8, 8));
    emit(0x3E * Op | Depth);
    // Clear Z with the RDP fill path, including its hidden bits.
    emit(0x3F * Op | (2ULL << 51) | (7ULL << 32) | Depth);
    emit(0x2F * Op | (3ULL << 52));
    emit(rdpSetFillColor(0xFFFCFFFC));
    emit(rdpFillRect(0, 0, 7, 7));
    emit(0x3F * Op | (2ULL << 51) | (7ULL << 32) | Color);
    // Load two RGBA5551 entries into CI4 palette bank 3, not bank 0.
    bus.write16(Palette, 0xF801);
    bus.write16(Palette + 2, 0x07C1);
    emit(0x3D * Op | (2ULL << 51) | (1ULL << 32) | Palette);
    emit(0x35 * Op | CI4 | (0x130ULL << 32) | Tile7);
    emit(0x30 * Op | Tile7 | (4ULL << 12));
    // Packed CI4 texels uploaded with the standard 16-bit LOADBLOCK alias.
    bus.write16(Texture, 0x0101);
    bus.write16(Texture + 2, 0x1010);
    emit(0x3D * Op | (2ULL << 51) | Texture);
    emit(0x35 * Op | (2ULL << 51) | Tile7);
    emit(0x33 * Op | Tile7 | (1ULL << 12));
    emit(0x35 * Op | CI4 | (1ULL << 41) | (3ULL << 20));
    emit(0x32 * Op | (12ULL << 12));
    // Two cycles: (texel0 - 0) * shade + 0, then pass combined through.
    emit(0x3C * Op | (1ULL << 52) | (4ULL << 47) | (7ULL << 41) |
         (8ULL << 28) | (7ULL << 15) | (6ULL << 9) | (7ULL << 18));
    emit(0x2F * Op | (1ULL << 52) | (1ULL << 51) | (1ULL << 47) | 0x30);
    const auto triangle = [&](u32 z, u32 s) {
        emit(rdpTriangleBase(0, 0, 4, (7ULL << 56) | (1ULL << 55)));
        emit(rdpEdge(4, 0));
        emit(rdpEdge(0, 0));
        emit(rdpEdge(4, 0));
        emit(rdpShadeWord(128, 128, 128, 255));
        for (int i = 0; i < 7; ++i) emit(0);
        // W = 0.5: the divider doubles S. Vary S by 0.5 texels per pixel.
        emit((u64{s} << 48) | (0x4000ULL << 16));
        emit(16ULL << 48);
        for (int i = 0; i < 6; ++i) emit(0);
        emit(u64{z} << 48);
        emit(0);
        rdp.process();
    };
    triangle(0x2000, 0);
    assert(bus.read16(Color) == 0x8001);
    assert(bus.read16(Color + 2) == 0x0401);
    assert(bus.read16(Depth) == 0x1000); // Z=0x10000, exponent 0, mantissa 0x400
    assert(bus.read16(Color + 8) == 0);
    const u64 pixels = rdp.stats().pixelsDrawn;
    triangle(0x3000, 16);
    assert(rdp.stats().pixelsDrawn == pixels && bus.read16(Color) == 0x8001);
    triangle(0x1000, 16);
    assert(bus.read16(Color) == 0x0401 && bus.read16(Depth) == 0x0800);

    // RDP "bilerp" interpolates three neighboring samples, not all four.
    // At S=0.5, T=0 the output is halfway between red and green.
    emit(0x3C * Op | (31ULL << 32) | (1ULL << 6) | (7ULL << 18) | 1);
    emit(0x2F * Op | (1ULL << 47) | (1ULL << 45) | (1ULL << 43));
    emit(0x24 * Op | (4ULL << 44) | (24ULL << 32) | 20);
    emit(16ULL << 48);
    rdp.process();
    assert(bus.read16(Color + 5 * 16) == 0x8401);

    // Replace one palette entry with IA88 and compare the decoded alpha to BLEND_COLOR.
    bus.write16(Palette, 0xA040);
    emit(0x3D * Op | (2ULL << 51) | Palette);
    emit(0x35 * Op | CI4 | (0x130ULL << 32) | Tile7);
    emit(0x30 * Op | Tile7);
    emit(0x39 * Op | 0x41);
    emit(0x2F * Op | (1ULL << 47) | (1ULL << 46) | 1);
    emit(0x24 * Op | (4ULL << 44) | (28ULL << 32) | 24);
    emit(0);
    rdp.process();
    assert(bus.read16(Color + 6 * 16) == 0);
    emit(0x39 * Op | 0x40);
    emit(0x24 * Op | (4ULL << 44) | (28ULL << 32) | 24);
    emit(0);
    rdp.process();
    assert(bus.read16(Color + 6 * 16) == 0xA529);

    // Force source-over blending of half-alpha red against blue in RDRAM.
    emit(0x2F * Op | (3ULL << 52));
    emit(rdpSetFillColor(0x003F003F));
    emit(rdpFillRect(0, 7, 0, 7));
    emit(0x3A * Op | 0xFF000080);
    emit(0x3C * Op | (31ULL << 32) | (3ULL << 6) | (7ULL << 18) | 3);
    emit(0x2F * Op | (1ULL << 22) | 0x4040);
    emit(rdpFillRect(0, 7, 1, 8));
    rdp.process();
    assert(bus.read16(Color + 7 * 16) == 0x781F);
}

void test_rdp_tmem_texel_formats() {
    struct FormatCase {
        u32 format, size;
        u64 rows;
        u32 row0, row1;
    };
    constexpr FormatCase cases[] = {
        {0, 3, 0x123456789ABCDEF0, 0x123456, 0x9ABCDE}, // RGBA32 split banks
        {3, 2, 0x9876674500000000, 0x989898, 0x676767}, // IA16
        {3, 1, 0xA5C7000000000000, 0xAAAAAA, 0xCCCCCC}, // IA8
        {3, 0, 0x5090000000000000, 0x494949, 0x929292}, // IA4
        {4, 1, 0x5281000000000000, 0x525252, 0x818181}, // I8
        {4, 0, 0x30E0000000000000, 0x333333, 0xEEEEEE}, // I4
    };
    for (const auto& [fmt, size, rows, row0, row1] : cases) {
        Bus bus;
        Rdp rdp;
        rdp.setBus(&bus);
        rdp.setHw(&bus.hw());
        // A 4-bit source row is padded to a byte, so transfer a two-texel image.
        const u32 width = size ? 1 : 2;
        bus.write64(0x2000, rows);
        constexpr u64 Op = 1ULL << 56;
        const u64 type = (u64{fmt} << 53) | (u64{size} << 51);
        const u64 commands[] = {
            rdpSetColorImage(2, 0x3000), rdpSetScissor(0, 0, 2, 2),
            0x3D * Op | type | (u64{width - 1} << 32) | 0x2000,
            0x35 * Op | type | (1ULL << 41), // one TMEM word per row
            0x34 * Op | 4, // LOADTILE: only S=0, two rows
            0x3C * Op | (31ULL << 32) | (1ULL << 6) | (7ULL << 18) | 1,
            0x2F * Op,
            0x24 * Op | (4ULL << 44) | (8ULL << 32),
            1024, // T step of one texel per pixel
        };
        for (u32 i = 0; i < std::size(commands); ++i) bus.write64(0x1000 + 8 * i, commands[i]);
        bus.hw().dp().current = 0x1000;
        bus.hw().dp().end = 0x1000 + sizeof(commands);
        rdp.process();
        assert(bus.read32(0x3000) >> 8 == row0);
        assert(bus.read32(0x3008) >> 8 == row1);
    }
}

void test_rdp_ci8_tlut_loaded_block_triangle() {
    constexpr u64 Op = 1ULL << 56;
    constexpr u64 Tile7 = 7ULL << 24;
    constexpr u32 Palette = 0x6000, Texture = 0x7000, Color = 0x3000;
    constexpr std::array<u16, 8> palette = {
        0xF801, 0x07C1, 0x003F, 0xFFFF, 0x0000, 0xF81F, 0x07FF, 0xF83F,
    };

    Bus bus;
    Rdp rdp;
    rdp.setFramebufferSize(8, 8);
    rdp.setBus(&bus);
    rdp.setHw(&bus.hw());
    auto& dp = bus.hw().dp();
    dp.current = dp.end = 0x1000;
    const auto emit = [&](u64 w) { bus.write64(dp.end, w); dp.end += 8; };

    for (u32 i = 0; i < palette.size(); ++i) bus.write16(Palette + i * 2, palette[i]);
    for (u32 i = 0; i < palette.size(); ++i) bus.write8(Texture + i, static_cast<u8>(i));

    // LOADTLUT quadruples eight RGBA5551 entries into upper TMEM word 0x100.
    emit(0x3D * Op | (2ULL << 51) | (7ULL << 32) | Palette);
    emit(0x35 * Op | (2ULL << 53) | (0x100ULL << 32) | Tile7);
    emit(0x30 * Op | Tile7 | (32ULL << 12));

    // LOADBLOCK transfers eight CI8 texels as one 64-bit word into lower TMEM.
    emit(0x3D * Op | (1ULL << 51) | (7ULL << 32) | Texture);
    emit(0x35 * Op | (1ULL << 51) | (1ULL << 41) | Tile7);
    emit(0x33 * Op | Tile7 | (7ULL << 12));
    emit(0x35 * Op | (2ULL << 53) | (1ULL << 51) | (1ULL << 41) | Tile7);
    // LOADBLOCK writes raw 12.0 extents; restore 10.2 bounds for sampling.
    emit(0x32 * Op | Tile7 | (28ULL << 12));

    emit(0x3F * Op | (2ULL << 51) | (7ULL << 32) | Color);
    emit(rdpSetScissor(0, 0, 8, 4));
    // One cycle, TLUT enabled, no perspective divide: pass TEXEL0 through so
    // the test observes the loaded palette rather than the combiner's shade.
    rdp.setCombineMode((31ULL << 32) | (1ULL << 6) | (7ULL << 18) | 1);
    emit(0x2F * Op | (1ULL << 47));

    // Textured triangle covering x=0..7, y=0..3 with S increasing one texel
    // per pixel. XH/XM/XL are 12.16; YH/YM/YL are 11.2 quarter pixels.
    emit(rdpTriangleBaseQ(0, 0, 16,
                          (1ULL << 58) | (1ULL << 57) | (1ULL << 55) | (7ULL << 48)));
    emit(rdpEdge(8, 0)); // XL at YM: right boundary
    emit(rdpEdge(0, 0)); // XH at YH: left boundary
    emit(rdpEdge(8, 0)); // XM at YH (unused when YM == YH)
    // Constant shade color; the combiner passes SHADE through.
    emit(rdpShadeWord(0, 255, 0, 255));
    for (int i = 0; i < 7; ++i) emit(0);
    // S, T, W coefficient quadruples: S base 0, S step 1<<5 (s10.5)/pixel.
    emit(0);
    emit(32ULL << 48);
    emit(0);
    emit(0);
    for (int i = 0; i < 4; ++i) emit(0);

    rdp.process();
    for (u32 x = 0; x < palette.size(); ++x) {
        // A textured triangle goes through the blender and stores coverage in
        // the RGBA16 alpha bit; compare the source RGB and full coverage.
        const u16 written = bus.read16(Color + x * 2);
        assert((written & 0xFFFE) == (palette[x] & 0xFFFE));
        assert((written & 1) == 1);
    }
    // The triangle covers four scanlines, so the first pixel of row 1 is
    // sampled again with the same S coordinate.
    const u16 nextRow = bus.read16(Color + 16);
    assert((nextRow & 0xFFFE) == (palette[0] & 0xFFFE));
    assert((nextRow & 1) == 1);
}

void test_rdp_perspective_strided_load_block() {
    constexpr u64 Op = 1ULL << 56, Tile7 = 7ULL << 24;
    constexpr u32 Palette = 0x6000, Texture = 0x7000, Color = 0x3000;
    constexpr std::array<u16, 4> palette{0xF801, 0x07C1, 0x003F, 0xFFFF};
    constexpr std::array<u16, 4> tinted{0xF801, 0x0401, 0x003F, 0xFC3F};
    for (const u32 size : {0u, 1u, 2u}) { // CI4, CI8, RGBA16
        Bus bus;
        Rdp rdp;
        rdp.setBus(&bus);
        rdp.setHw(&bus.hw());
        rdp.setFramebufferSize(4, 2);
        auto& dp = bus.hw().dp();
        dp.current = dp.end = 0x1000;
        const auto emit = [&](u64 command) { bus.write64(dp.end, command); dp.end += 8; };
        const u32 perWord = 16 >> size, width = perWord * 2;
        const auto index = [&](u32 x, u32 y) { return (x + y + x / perWord) & 3; };

        if (size < 2) {
            for (u32 i = 0; i < palette.size(); ++i) bus.write16(Palette + i * 2, palette[i]);
            emit(0x3D * Op | (2ULL << 51) | (3ULL << 32) | Palette);
            emit(0x35 * Op | (0x100ULL << 32) | Tile7);
            emit(0x30 * Op | Tile7 | (12ULL << 12)); // four palette entries
        }
        for (u32 y = 0; y < 2; ++y) for (u32 x = 0; x < width; ++x) {
            const u32 i = y * width + x, value = index(x, y);
            if (size == 2) bus.write16(Texture + i * 2, palette[value]);
            else if (size == 1) bus.write8(Texture + i, static_cast<u8>(value));
            else if (!(x & 1)) bus.write8(Texture + i / 2, static_cast<u8>((value << 4) | index(x + 1, y)));
        }
        // Render-format bits do not affect a 16-bit-alias block transfer.
        // DXT 0x400 wraps after two words; the programmed stride contributes
        // one more TMEM word, so the render tile's line is three words.
        emit(0x3D * Op | (2ULL << 51) | (7ULL << 32) | Texture);
        emit(0x35 * Op | (2ULL << 51) | (1ULL << 41) | Tile7);
        emit(0x33 * Op | Tile7 | (15ULL << 12) | 0x400);
        emit(0x35 * Op | (size < 2 ? 2ULL << 53 : 0) | (u64{size} << 51) | (3ULL << 41));
        emit(0x32 * Op | (u64{(width - 1) * 4} << 12) | 4);
        emit(0x3F * Op | (2ULL << 51) | (3ULL << 32) | Color);
        emit(rdpSetScissor(0, 0, 4, 2));
        emit(0x3B * Op | 0xFF80FFFF); // ENV: half green
        // Cycle 0 D = TEXEL0. Cycle 1 (COMBINED - 0) * ENV + 0.
        emit(0x3C * Op | (31ULL << 47) | (1ULL << 15) | (7ULL << 41) | (1ULL << 9) |
             (15ULL << 24) | (5ULL << 32) | (7ULL << 6) | (7ULL << 18));
        emit(0x2F * Op | (1ULL << 52) | (1ULL << 51) | (size < 2 ? 1ULL << 47 : 0));

        emit(rdpTriangleBaseQ(0, 0, 8, (1ULL << 57) | (1ULL << 55)));
        emit(rdpEdge(4, 0));
        emit(rdpEdge(0, 0));
        emit(rdpEdge(4, 0));
        emit(0x4000ULL << 16); // W = 0.5, S = T = 0
        emit(16ULL << 48); // S increases by 0.5 texels per pixel before division
        emit(0);
        emit(0);
        emit((8ULL << 32) | (0xE000ULL << 16)); // T += 0.25 texels, W -= 0.25 per line
        emit(0);
        emit(0);
        emit(0);
        emit(0x29 * Op);
        rdp.process();
        for (u32 y = 0; y < 2; ++y) for (u32 x = 0; x < 4; ++x)
            assert(bus.read16(Color + (y * 4 + x) * 2) == tinted[index(x * (y + 1), y)]);
        assert(rdp.stats().pixelsDrawn == 8 && rdp.stats().fullSyncs == 1);
    }
}

void test_rdp_adjacent_triangles_no_seam() {
    constexpr u64 Op = 1ULL << 56;
    constexpr u32 Color = 0x3000;
    const u32 green = 0xFF00FF00;
    const u32 blue = 0x000000FF;

    Bus bus;
    Rdp rdp;
    rdp.setFramebufferSize(16, 16);
    rdp.setBus(&bus);
    rdp.setHw(&bus.hw());
    auto& dp = bus.hw().dp();
    dp.current = dp.end = 0x1000;
    const auto emit = [&](u64 w) { bus.write64(dp.end, w); dp.end += 8; };
    emit(0x3F * Op | (2ULL << 51) | (15ULL << 32) | Color);
    emit(rdpSetScissor(0, 0, 16, 16));
    emit(0x2F * Op | (1ULL << 52)); // 1 cycle
    // Pass SHADE directly: (shade - 0) * 1 + 0 for RGB and alpha.
    rdp.setCombineMode((6ULL << 32) | (4ULL << 37) | (4ULL << 21) | (6ULL << 18));
    const auto shadeTriangle = [&](u32 color, bool leftMajor, const auto& words) {
        const u64 flags = (1ULL << 58) | (leftMajor ? (1ULL << 55) : 0);
        emit(words[0] | flags);
        for (u32 i = 1; i < words.size(); ++i) emit(words[i]);
        emit(rdpShadeWord((color >> 0) & 255, (color >> 8) & 255,
                          (color >> 16) & 255, (color >> 24) & 255));
        for (int i = 0; i < 7; ++i) emit(0);
    };

    // Shared diagonal from (4.75,4.5) to (12.75,12.5), split into two
    // 11.2-coordinate triangles. One command encodes the diagonal as XH
    // at floor(YH), the other as XL at fractional YM. Stepping both
    // edges from the fractional vertex leaves an unpainted diagonal here.
    constexpr s32 LeftX = 4 * 65536 + 32768;   // 4.5 pixels in 16.16
    constexpr s32 RightX = 12 * 65536 + 32768; // 12.5 pixels in 16.16
    constexpr s32 DiagonalSlope = 1 << 16;     // +0.25 pixel per quarter scanline
    const std::array<u64, 4> upper = {
        rdpTriangleBaseQ(16, 18, 50, 0),
        rdpEdgeFixed(LeftX + 16384, DiagonalSlope), // XL at YM: diagonal left edge
        rdpEdgeFixed(RightX, 0),                   // XH: right vertical edge
        rdpEdgeFixed(RightX, 0),                   // XM: degenerate above YM
    };
    const std::array<u64, 4> lower = {
        rdpTriangleBaseQ(18, 50, 50, 0),
        rdpEdgeFixed(LeftX, 0),                         // XL: unused
        rdpEdgeFixed(4 * 65536 + 16384, DiagonalSlope), // XH at floor(YH): diagonal right edge
        rdpEdgeFixed(LeftX, 0),                         // XM: left vertical edge
    };
    shadeTriangle(green, false, upper);
    shadeTriangle(blue, false, lower);
    rdp.process();

    // AA-off uses coverage bit 0 (x+0, y+0). The shared vertex lies on that
    // sample, so the filled region starts one pixel down/right. What matters
    // here is that both triangles together leave no black pixel inside it.
    assert(rdp.stats().pixelsDrawn == 64);
    for (u32 y = 5; y < 13; ++y) {
        for (u32 x = 5; x < 13; ++x) {
            const u32 c = rdp.pixel(x, y) & 0xFFFFFF;
            assert(c == ((x > y ? green : blue) & 0xFFFFFF));
        }
    }
    assert(rdp.pixel(3, 8) == 0);
    assert(rdp.pixel(4, 4) == 0);
    assert(rdp.pixel(13, 8) == 0);

    // The cached line origin is fractional (major edge 5.75 at y=5).
    // Mix signed and fractional gradients, including saturation in both
    // directions, so the NEON and portable shade paths must agree.
    rdp.reset();
    dp.current = dp.end = 0x1000;
    emit(0x3F * Op | (3ULL << 51) | (15ULL << 32) | Color);
    emit(rdpSetScissor(0, 0, 16, 16));
    emit(0x2F * Op | (1ULL << 52));
    rdp.setCombineMode((6ULL << 32) | (4ULL << 37) | (4ULL << 21) | (6ULL << 18));
    emit(rdpTriangleBaseQ(18, 18, 50, (1ULL << 58) | (1ULL << 55)));
    emit(rdpEdgeFixed(RightX, 0));
    emit(rdpEdgeFixed(4 * 65536 + 16384, DiagonalSlope));
    emit(rdpEdgeFixed(RightX, 0));
    emit(rdpShadeWord(128, 128, 1, 255));
    emit((64ULL << 48) | (0xFFC0ULL << 32)); // dr/dx=64, dg/dx=-64
    emit(0);
    emit(0x8000ULL << 16); // db/dx=0.5
    for (int i = 0; i < 4; ++i) emit(0);
    rdp.process();
    // Attributes are latched at XH on floor(YH), stepped to the whole scanline
    // origin (5.25 at y=5), not at the scanline midpoint (5.75). The gradient
    // therefore starts earlier: (r,g,b) = (144,112,1) at x=6 and (255,0,2)
    // at x=8. Mix signed and fractional gradients, including saturation, so
    // the NEON and portable shade paths must agree.
    assert((rdp.pixel(6, 5) & 0xFFFFFF) == 0x0150B0);
    assert((rdp.pixel(8, 5) & 0xFFFFFF) == 0x0200FF);

    // A left-major edge that slopes left latches attributes at the last
    // subpixel line. Mirror the previous command with a negative XH step.
    rdp.reset();
    dp.current = dp.end = 0x1000;
    emit(0x3F * Op | (3ULL << 51) | (15ULL << 32) | Color);
    emit(rdpSetScissor(0, 0, 16, 16));
    emit(0x2F * Op | (1ULL << 52));
    rdp.setCombineMode((6ULL << 32) | (4ULL << 37) | (4ULL << 21) | (6ULL << 18));
    emit(rdpTriangleBaseQ(18, 18, 50, (1ULL << 58) | (1ULL << 55)));
    emit(rdpEdgeFixed(RightX, 0));
    emit(rdpEdgeFixed(4 * 65536 + 16384, -16384));
    emit(rdpEdgeFixed(RightX, 0));
    emit(rdpShadeWord(128, 128, 1, 255));
    emit((64ULL << 48) | (0xFFC0ULL << 32));
    emit(0);
    emit(0x8000ULL << 16);
    emit(0); // DE = 0 along XH
    emit((16ULL << 48) | (0xFFF0ULL << 32) | (0xFFFFULL << 16));
    emit(0);
    emit(0xE000ULL << 16); // DY = -0.125 for blue
    rdp.process();
    // XH moves left by 0.25 px per scanline, so the do-offset latch shifts it
    // another 0.1875 px left. The matching coefficient correction keeps the
    // gradient continuous: (r,g,b) = (128,128,1), (255,0,2), (255,0,3).
    assert((rdp.pixel(4, 5) & 0xFFFFFF) == 0x018080);
    assert((rdp.pixel(6, 5) & 0xFFFFFF) == 0x0200FF);
    assert((rdp.pixel(8, 5) & 0xFFFFFF) == 0x0300FF);

    // The same top-left rule applies to the HLE flat-shade path.
    rdp.reset();
    rdp.setFramebufferSize(16, 16);
    rdp.drawFlatTriangle(4, 4, 12, 4, 12, 12, green);
    rdp.drawFlatTriangle(4, 4, 12, 12, 4, 12, blue);
    assert(rdp.stats().pixelsDrawn == 64);
    for (u32 y = 4; y < 12; ++y)
        for (u32 x = 4; x < 12; ++x)
            assert((rdp.pixel(x, y) & 0xFFFFFF) ==
                   (x >= y ? (green & 0xFFFFFF) : (blue & 0xFFFFFF)));
}

void test_rdp_raw_textured_triangle_loadtile_two_cycle() {
    constexpr u64 Op = 1ULL << 56;
    constexpr u32 Texture0 = 0x2000, Texture1 = 0x2400;
    constexpr u32 Color0 = 0x3000, Color1 = 0x4000, Color2 = 0x5000;
    // Both tiles are opaque: CVG_X_ALPHA must not turn any sampled texel into
    // a black hole. Tile 1 supplies the output RGB of the second cycle.
    constexpr std::array<u16, 8> tex0 = {
        0xF801, 0x07C1, 0x003F, 0xFFFF, 0xF81F, 0x07FF, 0xFC01, 0x03E1,
    };
    constexpr std::array<u16, 8> tex1 = {
        0x07FF, 0xF81F, 0xFC01, 0x03E1, 0xFFFF, 0xF800, 0x07C0, 0x003F,
    };

    Bus bus;
    Rdp rdp;
    rdp.setFramebufferSize(4, 2);
    rdp.setBus(&bus);
    rdp.setHw(&bus.hw());
    for (u32 i = 0; i < tex0.size(); ++i) {
        bus.write16(Texture0 + i * 2, tex0[i]);
        bus.write16(Texture1 + i * 2, tex1[i]);
    }

    std::array<u64, 8> tex{};
    const auto writeCoeff = [&](u32 channel, s32 base, s32 dx, s32 de) {
        const u32 shift = 48 - 16 * channel;
        const auto upper = [](s32 value) { return static_cast<u16>(static_cast<u32>(value) >> 16); };
        const auto lower = [](s32 value) { return static_cast<u16>(static_cast<u32>(value)); };
        tex[0] |= static_cast<u64>(upper(base)) << shift;
        tex[2] |= static_cast<u64>(lower(base)) << shift;
        tex[1] |= static_cast<u64>(upper(dx)) << shift;
        tex[3] |= static_cast<u64>(lower(dx)) << shift;
        tex[4] |= static_cast<u64>(upper(de)) << shift;
        tex[6] |= static_cast<u64>(lower(de)) << shift;
    };
    // One texel per pixel and per scanline in 16.16; perspective is disabled.
    writeCoeff(0, 0, 32 << 16, 0);
    writeCoeff(1, 0, 0, 32 << 16);
    writeCoeff(2, 0x8000 << 16, 0, 0);

    // Cycle 0 passes TEXEL0 (tile 0) through RGB and alpha. Cycle 1 selects
    // TEXEL0 again, which by then holds tile 1 (RH#001/RH#002), and inherits
    // cycle 0's COMBINED alpha. This makes both tiles observable in one pixel.
    const u64 tile1Combiner = rdpRawSetCombineFull(
        1, 0, 6, 0, 0, 0, 0, 1,
        1, 0, 6, 0, 0, 0, 0, 0);
    // Cycle 1 passes COMBINED through, proving cycle 0's TEXEL0 result feeds it.
    const u64 chainedCombiner = rdpRawSetCombineFull(
        1, 0, 6, 0, 0, 0, 0, 1,
        0, 0, 6, 0, 0, 0, 0, 0);
    const auto render = [&](u64 combine, u32 colorAddress, bool cvgXAlpha) {
        std::vector<u64> commands{
            (0x3DULL << 56) | (2ULL << 51) | (3ULL << 32) | Texture0,
            rdpRawSetTile(0, 0, 2, 1, 0, 0, false, false, 1, 0, false, false, 2, 0),
            rdpRawSetTileSize(0x34, 0, 0, 0, 12, 4),
            rdpRawSetTileSize(0x32, 0, 0, 0, 12, 4),
            (0x3DULL << 56) | (2ULL << 51) | (3ULL << 32) | Texture1,
            rdpRawSetTile(1, 0, 2, 1, 16, 0, false, false, 1, 0, false, false, 2, 0),
            rdpRawSetTileSize(0x34, 1, 0, 0, 12, 4),
            rdpRawSetTileSize(0x32, 1, 0, 0, 12, 4),
            rdpSetColorImage(4, colorAddress),
            rdpSetScissor(0, 0, 4, 2),
            (0x2FULL << 56) | (1ULL << 52) |
                (cvgXAlpha ? (1ULL << 13) | (1ULL << 12) : 0ULL),
            combine,
            rdpRawTriangle(0x0E, 0, 3, true, 0, 0, 8, true, true, false),
            rdpEdge(4, 0), rdpEdge(0, 0), rdpEdge(4, 0),
            rdpShadeWord(255, 255, 255, 255),
        };
        for (u32 i = 1; i < 8; ++i) commands.push_back(0);
        commands.insert(commands.end(), tex.begin(), tex.end());
        rdp.submitCommands(commands.data(), commands.size());
    };
    const auto expand = [](u16 raw) {
        const auto channel = [](u32 n) { return (n << 3) | (n >> 2); };
        return (channel((raw >> 1) & 31) << 16) |
               (channel((raw >> 6) & 31) << 8) | channel((raw >> 11) & 31);
    };

    // RGBA32 framebuffer writes store destination coverage in the alpha byte.
    const auto expectedPixel = [&](u16 texel) { return expand(texel) | 0xE0000000u; };

    render(tile1Combiner, Color0, false);
    for (u32 y = 0; y < 2; ++y) {
        for (u32 x = 0; x < 4; ++x) {
            const u32 expected = expectedPixel(tex1[y * 4 + x]);
            assert(rdp.pixel(x, y) == expected);
        }
    }

    render(chainedCombiner, Color2, false);
    for (u32 y = 0; y < 2; ++y)
        for (u32 x = 0; x < 4; ++x)
            assert(rdp.pixel(x, y) == expectedPixel(tex0[y * 4 + x]));

    // Opaque texels must survive CVG_X_ALPHA + ALPHA_CVG_SEL; only the stored
    // alpha changes. Black holes here were the original 3D dropout symptom.
    render(tile1Combiner, Color1, true);
    for (u32 y = 0; y < 2; ++y)
        for (u32 x = 0; x < 4; ++x)
            assert((rdp.pixel(x, y) & 0xFFFFFF) == expand(tex1[y * 4 + x]));
    assert(rdp.stats().triangleCommands == 3);
    assert(rdp.stats().texturedTriangles == 3);
    assert(rdp.stats().pixelsDrawn == 24);
}

void test_rdp_raw_loadtile_subrectangle() {
    constexpr u32 Texture = 0x2000, Color = 0x3000;
    // Four pixels per RDRAM row; LOADTILE must copy only columns 1..3 of rows
    // 1..2 and lay them out using the tile's 64-bit-word line stride.
    constexpr std::array<u16, 12> texels = {
        0xF801, 0x07C1, 0x003F, 0xFFFF,
        0xF81F, 0x07FF, 0xFC01, 0x03E1,
        0xFFFF, 0xF800, 0x07C0, 0x003F,
    };

    Bus bus;
    Rdp rdp;
    rdp.setFramebufferSize(3, 2);
    rdp.setBus(&bus);
    rdp.setHw(&bus.hw());
    for (u32 i = 0; i < texels.size(); ++i)
        bus.write16(Texture + i * 2, texels[i]);

    const u64 commands[] = {
        (0x3DULL << 56) | (2ULL << 51) | (3ULL << 32) | Texture,
        rdpRawSetTile(0, 0, 2, 4, 0, 0, false, false, 0, 0, false, false, 0, 0),
        rdpRawSetTileSize(0x34, 0, 4, 4, 12, 12),
        rdpRawSetTileSize(0x32, 0, 4, 4, 12, 12),
        (0x3FULL << 56) | (2ULL << 51) | (2ULL << 32) | Color, // RGBA16, width 3
        rdpSetScissor(0, 0, 3, 2),
        0x2FULL << 56, // 1-cycle
        rdpRawSetCombine(1, 0, 6, 0, 1, 0, 6, 0),
        (0x24ULL << 56) | (0ULL << 24) | (12ULL << 44) | (8ULL << 32),
        (32ULL << 48) | (32ULL << 32) | (1024ULL << 16) | 1024ULL,
    };
    rdp.submitCommands(commands, std::size(commands));

    const auto expand = [](u16 raw) {
        const auto channel = [](u32 n) { return (n << 3) | (n >> 2); };
        return (channel((raw >> 1) & 31) << 16) |
               (channel((raw >> 6) & 31) << 8) | channel((raw >> 11) & 31);
    };
    for (u32 y = 0; y < 2; ++y) {
        for (u32 x = 0; x < 3; ++x) {
            const u16 expected = texels[(y + 1) * 4 + x + 1];
            const u16 got = bus.read16(Color + (y * 3 + x) * 2);
            assert((got & 0xFFFE) == (expected & 0xFFFE));
        }
    }
    assert(rdp.stats().pixelsDrawn == 6);
}

void test_rdp_shared_edge_alpha_cvg_no_black_holes() {
    constexpr u32 Color = 0x3000;
    const u32 green = 0x4000FF00; // low alpha exercises CVG_X_ALPHA
    const u32 blue = 0x400000FF;

    Bus bus;
    Rdp rdp;
    rdp.setFramebufferSize(16, 16);
    rdp.setBus(&bus);
    rdp.setHw(&bus.hw());
    auto& dp = bus.hw().dp();
    dp.current = dp.end = 0x1000;
    const auto emit = [&](u64 w) { bus.write64(dp.end, w); dp.end += 8; };
    emit(rdpSetColorImage(16, Color));
    emit(rdpSetScissor(0, 0, 16, 16));
    // 1 cycle (cycle type 0), alpha-coverage selection multiplied by alpha,
    // no forced blend. P1 deliberately selects the black blend color: an
    // erroneous second stage would replace the edge pixels with black.
    emit((0x2FULL << 56) | (1ULL << 13) | (1ULL << 12) |
         (1ULL << 29) | (3ULL << 8));
    // Pass SHADE (including its low alpha) through the combiner.
    rdp.setCombineMode(rdpRawSetCombine(4, 0, 6, 0, 4, 0, 6, 0));
    const auto shadeTriangle = [&](u32 color, bool leftMajor, const auto& words) {
        const u64 flags = (1ULL << 58) | (leftMajor ? (1ULL << 55) : 0);
        emit(words[0] | flags);
        for (u32 i = 1; i < words.size(); ++i) emit(words[i]);
        emit(rdpShadeWord((color >> 0) & 255, (color >> 8) & 255,
                          (color >> 16) & 255, (color >> 24) & 255));
        for (int i = 0; i < 7; ++i) emit(0);
    };

    // The shared diagonal is the XH edge of one triangle and the XL edge of
    // the other. Both sides have single-sample pixels along that boundary.
    constexpr s32 LeftX = 4 * 65536 + 32768;
    constexpr s32 RightX = 12 * 65536 + 32768;
    constexpr s32 DiagonalSlope = 1 << 16;
    const std::array<u64, 4> upper = {
        rdpTriangleBaseQ(16, 18, 50, 0),
        rdpEdgeFixed(LeftX + 16384, DiagonalSlope),
        rdpEdgeFixed(RightX, 0),
        rdpEdgeFixed(RightX, 0),
    };
    const std::array<u64, 4> lower = {
        rdpTriangleBaseQ(18, 50, 50, 0),
        rdpEdgeFixed(LeftX, 0),
        rdpEdgeFixed(4 * 65536 + 16384, DiagonalSlope),
        rdpEdgeFixed(LeftX, 0),
    };
    shadeTriangle(green, false, upper);
    shadeTriangle(blue, false, lower);
    rdp.process();

    assert(rdp.stats().pixelsDrawn == 64);
    for (u32 y = 5; y < 13; ++y) {
        for (u32 x = 5; x < 13; ++x) {
            const u32 rgb = rdp.pixel(x, y) & 0x00FFFFFF;
            const u32 expected = (x > y ? green : blue) & 0x00FFFFFF;
            assert(rgb == expected);
        }
    }
}

void test_rdp_color_on_cvg_uses_stage0_m_input() {
    constexpr u32 Color = 0x3000;
    constexpr u32 fog = 0xFF0000FF; // RGBA8888 red from SET_FOG_COLOR
    constexpr u32 green = 0x40FFFF00;

    const auto render = [&](bool colorOnCvg, u8 memAlpha) {
        Bus bus;
        Rdp rdp;
        rdp.setFramebufferSize(16, 16);
        rdp.setBus(&bus);
        rdp.setHw(&bus.hw());
        for (u32 i = 0; i < 16 * 16; ++i)
            bus.write32(Color + i * 4, memAlpha);
        auto& dp = bus.hw().dp();
        dp.current = dp.end = 0x1000;
        const auto emit = [&](u64 w) { bus.write64(dp.end, w); dp.end += 8; };

        emit(rdpSetColorImage(16, Color));
        emit(rdpSetScissor(0, 0, 16, 16));
        // 1 cycle (cycle type 0), AA enabled, image read enabled, coverage
        // destination 3, stage-0 P=pixel, A=shade alpha, M=fog and B=1-A. Only
        // COLOR_ON_CVG is varied: normal partial blending mixes green with fog,
        // while the substitution must select fog alone.
        u64 modes = (0x2FULL << 56) | 8ULL | 0x40ULL | (3ULL << 8) |
                    (2ULL << 26) | (3ULL << 22);
        if (colorOnCvg) modes |= 0x80ULL;
        emit(modes);
        emit((0x38ULL << 56) | fog);
        rdp.setCombineMode(rdpRawSetCombine(4, 0, 6, 0, 4, 0, 6, 0));

        const auto shadeTriangle = [&](u32 color, const auto& words) {
            emit(words[0] | (1ULL << 58));
            for (u32 i = 1; i < words.size(); ++i) emit(words[i]);
            emit(rdpShadeWord((color >> 0) & 255, (color >> 8) & 255,
                              (color >> 16) & 255, (color >> 24) & 255));
            for (int i = 0; i < 7; ++i) emit(0);
        };
        // The diagonal left edge leaves single-sample pixels along the
        // boundary. Those have coverage 1 and, with a zero-alpha framebuffer,
        // do not prewrap; the interior has coverage 8 and does prewrap.
        constexpr s32 LeftX = 4 * 65536 + 32768;
        constexpr s32 RightX = 12 * 65536 + 32768;
        constexpr s32 DiagonalSlope = 1 << 16;
        const std::array<u64, 4> triangle = {
            rdpTriangleBaseQ(16, 18, 50, 0),
            rdpEdgeFixed(LeftX + 16384, DiagonalSlope),
            rdpEdgeFixed(RightX, 0),
            rdpEdgeFixed(RightX, 0),
        };
        shadeTriangle(green, triangle);
        rdp.process();

        std::array<u32, 16 * 16> out{};
        for (u32 y = 0; y < 16; ++y)
            for (u32 x = 0; x < 16; ++x) out[y * 16 + x] = rdp.pixel(x, y) & 0x00FFFFFF;
        return out;
    };

    const auto plain = render(false, 0);
    const auto onCvg = render(true, 0);
    const auto plainPrewrapped = render(false, 255);
    const auto onCvgPrewrapped = render(true, 255);

    u32 substituted = 0, unchanged = 0;
    for (u32 i = 0; i < plain.size(); ++i) {
        if (plain[i] != onCvg[i]) {
            ++substituted;
            // Partial coverage selects stage-0 M (fog), never the second MUX.
            assert(onCvg[i] == (fog & 0x00FFFFFF));
        } else {
            ++unchanged;
        }
        // coverage + memCoverage >= 8 suppresses COLOR_ON_CVG substitution.
        assert(onCvgPrewrapped[i] == plainPrewrapped[i]);
    }
    assert(substituted > 0 && unchanged > 0);
}

void test_rdp_cycle_blender_stage_selection() {
    constexpr u32 Color = 0x3000;
    // SHADE raw components are passed through rdpShadeWord; memory and fog are
    // RDP raw RRGGBBAA registers. Alpha 128 gives av=bv=16 with forced blend.
    constexpr u32 memoryRaw = 0x00FF0000; // green
    constexpr u32 fogRaw = 0x0000FF00;    // blue (RRGGBBAA)
    constexpr u32 onePlainRgb = 0x00007F7F; // (shade red + memory green) / 2
    constexpr u32 twoPlainRgb = 0x007F3F3F; // (onePlain + fog blue) / 2
    constexpr u32 oneCvgRgb = 0x0000FF00;   // stage-0 M = memory green
    constexpr u32 twoCvgRgb = 0x00FF0000;   // stage-1 M = fog blue

    const auto render = [&](u32 cycleType, bool colorOnCvg, bool forceBlend = true) {
        Bus bus;
        Rdp rdp;
        rdp.setFramebufferSize(16, 16);
        rdp.setBus(&bus);
        rdp.setHw(&bus.hw());
        for (u32 i = 0; i < 16 * 16; ++i) bus.write32(Color + i * 4, memoryRaw);
        auto& dp = bus.hw().dp();
        dp.current = dp.end = 0x1000;
        const auto emit = [&](u64 w) { bus.write64(dp.end, w); dp.end += 8; };
        emit(rdpSetColorImage(16, Color));
        emit(rdpSetScissor(0, 0, 16, 16));
        // Stage 0: P=pixel, M=memory, A=shade alpha, B=1-A.
        // Stage 1: P=combined, M=fog, A=shade alpha, B=1-A.
        u64 modes = (0x2FULL << 56) | (static_cast<u64>(cycleType) << 52) |
                    8ULL | 0x40ULL | (forceBlend ? 0x4000ULL : 0) | (3ULL << 8) |
                    (0ULL << 30) | (2ULL << 26) | (1ULL << 22) | (0ULL << 18) |
                    (0ULL << 28) | (2ULL << 24) | (3ULL << 20) | (0ULL << 16);
        if (colorOnCvg) modes |= 0x80ULL;
        emit(modes);
        emit((0x38ULL << 56) | fogRaw);
        rdp.setCombineMode(rdpRawSetCombine(4, 0, 6, 0, 4, 0, 6, 0));

        // A diagonal edge supplies partially covered pixels. Interior pixels
        // have coverage 8 and prewrap, suppressing COLOR_ON_CVG substitution.
        constexpr s32 LeftX = 4 * 65536 + 32768;
        constexpr s32 RightX = 12 * 65536 + 32768;
        constexpr s32 DiagonalSlope = 1 << 16;
        const std::array<u64, 4> triangle = {
            rdpTriangleBaseQ(16, 18, 50, 0),
            rdpEdgeFixed(LeftX + 16384, DiagonalSlope),
            rdpEdgeFixed(RightX, 0),
            rdpEdgeFixed(RightX, 0),
        };
        emit(triangle[0] | (1ULL << 58));
        for (u32 i = 1; i < triangle.size(); ++i) emit(triangle[i]);
        emit(rdpShadeWord(255, 0, 0, 128));
        for (int i = 0; i < 7; ++i) emit(0);
        rdp.process();

        std::array<u32, 16 * 16> out{};
        for (u32 y = 0; y < 16; ++y)
            for (u32 x = 0; x < 16; ++x) out[y * 16 + x] = rdp.pixel(x, y) & 0x00FFFFFF;
        return out;
    };

    const auto onePlain = render(0, false);
    const auto oneCvg = render(0, true);
    const auto twoPlain = render(1, false);
    const auto twoCvg = render(1, true);
    // At this fully covered pixel, the final stage is not enabled to mix.
    // Stage 0 must still run its fixed-shift equation before stage 1 reads P.
    assert(render(1, false, false)[6 * 16 + 9] == onePlainRgb);

    u32 onePartial = 0, oneFull = 0, twoPartial = 0, twoFull = 0;
    for (u32 i = 0; i < onePlain.size(); ++i) {
        if (!onePlain[i]) continue;
        // 1-cycle uses only stage 0; COLOR_ON_CVG substitutes stage-0 M.
        assert(onePlain[i] == onePlainRgb);
        if (oneCvg[i] == oneCvgRgb) ++onePartial;
        else {
            assert(oneCvg[i] == onePlainRgb);
            ++oneFull;
        }
        // 2-cycle feeds stage 0 into stage 1; COLOR_ON_CVG substitutes
        // stage-1 M, never stage-0 M and never a second one-cycle equation.
        assert(twoPlain[i] == twoPlainRgb);
        if (twoCvg[i] == twoCvgRgb) ++twoPartial;
        else {
            assert(twoCvg[i] == twoPlainRgb);
            ++twoFull;
        }
    }
    assert(onePartial > 0 && oneFull > 0 && twoPartial > 0 && twoFull > 0);
    assert(std::count(onePlain.begin(), onePlain.end(), onePlainRgb) ==
           std::count(twoPlain.begin(), twoPlain.end(), twoPlainRgb));
}

void test_rdp_internal_scale_native_writeback() {
    constexpr u32 Color = 0x18000, Width = 96, Height = 64;
    const auto render = [&](u32 scale, unsigned threads) {
        Emulator emu;
        auto& bus = emu.bus();
        auto& rdp = emu.rdp();
        rdp.setFramebufferSize(Width, Height);
        emu.setInternalResolution(scale);
        rdp.setThreadCount(threads);
        bus.write32(Color + Width * Height * 4, 0x12345678);
        const std::array<u64, 15> commands{
            rdpSetColorImage(Width, Color), rdpSetScissor(0, 0, Width, Height),
            0x2FULL << 56, rdpRawSetCombine(0, 0, 31, 7, 4, 0, 6, 0),
            rdpTriangleBaseQ(0, 0, Height * 4, (1ULL << 58) | (1ULL << 55)),
            rdpEdge(Width, 0), rdpEdge(0, 0), rdpEdge(Width, 0),
            rdpShadeWord(16, 0, 0, 255), 16ULL << 48, 0, 0, 0, 0, 0,
        };
        std::vector<u64> list(commands.begin(), commands.end());
        list.push_back(0);
        rdp.submitCommands(list.data(), list.size());
        assert(rdp.width() == Width * scale && rdp.height() == Height * scale);
        assert((rdp.pixel(0, 0) & 255) == 16 && (rdp.pixel(1, 0) & 255) == 32);
        for (u32 sx = 0; sx < scale; ++sx) {
            const u32 expected = scale == 1 ? 16 : 16 + (16 * (2 * sx + 1)) / (2 * scale);
            assert((rdp.framebuffer()[sx] & 255) == expected);
        }
        assert(bus.read32(Color + Width * Height * 4) == 0x12345678);
        std::vector<u8> native(Width * Height * 4);
        bus.rdram().copyRdram(Color, native);

        bus.write32(0x04400000, 3);
        bus.write32(0x04400004, Color);
        bus.write32(0x04400008, Width);
        bus.write32(0x04400024, Width);
        bus.write32(0x04400028, Height * 2);
        bus.write32(0x04400030, 1024);
        bus.write32(0x04400034, 1024);
        std::vector<u32> frame(Width * Height * scale * scale);
        Emulator::ScanoutHistory history;
        assert(emu.scanoutForPresentation(frame, {}, history));
        assert(emu.framebufferWidth() == Width * scale && emu.framebufferHeight() == Height * scale);
        assert((frame[0] & 255) == (rdp.framebuffer()[0] & 255));
        auto before = frame;
        assert(!emu.scanoutForPresentation(frame, before, history));
        // CPU/DMA writes win over the enhanced surface in VI presentation.
        bus.write32(Color, 0x00FF00FF);
        assert(emu.scanoutForPresentation(frame, before, history));
        for (u32 sx = 0; sx < scale; ++sx) assert(frame[sx] == 0xFF00FF00);
        emu.scanout();
        assert(emu.framebufferWidth() == Width && emu.framebufferHeight() == Height);
        return std::pair{std::move(native), std::move(frame)};
    };
    const auto native = render(1, 1).first;
    for (const u32 scale : {2u, 3u, 4u}) {
        const auto serial = render(scale, 1);
        assert(serial.first == native);
        assert(serial == render(scale, 0));
    }
}

void test_rdp_internal_scale_double_buffer_presentation() {
    constexpr u32 Width = 64, Height = 32, Front = 0x18000, Back = 0x20000;
    for (const u32 scale : {2u, 4u}) for (const unsigned threads : {1u, 0u}) {
        Emulator emu;
        auto& bus = emu.bus();
        auto& rdp = emu.rdp();
        rdp.setFramebufferSize(Width, Height);
        emu.setInternalResolution(scale);
        rdp.setThreadCount(threads);
        const std::array<u64, 15> commands{
            rdpSetColorImage(Width, Front), rdpSetScissor(0, 0, Width, Height),
            0x2FULL << 56, rdpRawSetCombine(0, 0, 31, 7, 4, 0, 6, 0),
            rdpTriangleBaseQ(0, 0, Height * 4, (1ULL << 58) | (1ULL << 55)),
            rdpEdge(Width, 0), rdpEdge(0, 0), rdpEdge(Width, 0),
            rdpShadeWord(16, 0, 0, 255), 16ULL << 48, 0, 0, 0, 0, 0,
        };
        std::vector<u64> list(commands.begin(), commands.end());
        list.push_back(0);
        rdp.submitCommands(list.data(), list.size());

        // Retire Front by pointing SET_COLOR_IMAGE at Back without drawing
        // into it. VI still scans out Front, which must retain its sidecar.
        const u64 setBack = rdpSetColorImage(Width, Back);
        rdp.submitCommands(&setBack, 1);
        bus.write32(0x04400000, 3);
        bus.write32(0x04400004, Front);
        bus.write32(0x04400008, Width);
        bus.write32(0x04400024, Width);
        bus.write32(0x04400028, Height * 2);
        bus.write32(0x04400030, 1024);
        bus.write32(0x04400034, 1024);

        std::vector<u32> frame(Width * Height * scale * scale);
        Emulator::ScanoutHistory history;
        assert(emu.scanoutForPresentation(frame, {}, history));
        assert(emu.framebufferWidth() == Width * scale && emu.framebufferHeight() == Height * scale);
        for (u32 sx = 0; sx < scale; ++sx) {
            const u32 expected = 16 + (16 * (2 * sx + 1)) / (2 * scale);
            assert((frame[sx] & 255) == expected);
        }
        const auto before = frame;
        // A CPU write to the scanned-out buffer still wins over the sidecar.
        bus.write32(Front, 0x00FF00FF);
        assert(emu.scanoutForPresentation(frame, before, history));
        for (u32 sx = 0; sx < scale; ++sx) assert(frame[sx] == 0xFF00FF00);

        // VI may scan a sub-rectangle of the refined buffer. One row down must
        // select the same sidecar without changing guest addressing.
        bus.write32(0x04400004, Front + Width * 4);
        bus.write32(0x04400028, (Height - 1) * 2);
        std::vector<u32> offsetFrame(Width * Height * scale * scale);
        Emulator::ScanoutHistory offsetHistory;
        assert(emu.scanoutForPresentation(offsetFrame, {}, offsetHistory));
        assert(emu.framebufferWidth() == Width * scale &&
               emu.framebufferHeight() == (Height - 1) * scale);
        for (u32 sx = 0; sx < scale; ++sx) {
            const u32 expected = 16 + (16 * (2 * sx + 1)) / (2 * scale);
            assert((offsetFrame[sx] & 255) == expected);
        }

        // Back was never refined, so its VI surface falls back to native.
        bus.write32(0x04400028, Height * 2);
        bus.write32(0x04400004, Back + Width * 4);
        std::vector<u32> native(Width * Height * scale * scale);
        Emulator::ScanoutHistory nativeHistory;
        assert(emu.scanoutForPresentation(native, {}, nativeHistory));
        assert(emu.framebufferWidth() == Width && emu.framebufferHeight() == Height);
    }
}

void test_rdp_textured_gouraud_three_point_gradient() {
    constexpr u32 Texture = 0x2000, Color = 0x3000;
    for (const u32 boundS : {3u, 4u}) for (const u64 filterMode :
         {0ULL, 1ULL << 45, 1ULL << 43, (1ULL << 45) | (1ULL << 43)}) {
        Bus bus;
        Rdp rdp;
        rdp.setFramebufferSize(5, 3);
        rdp.setBus(&bus);
        bus.write32(Texture, 0xF801FFC1);     // red, yellow
        bus.write32(Texture + 4, 0xF801F801); // red, red

        const std::array<u64, 21> commands{
            rdpSetColorImage(5, Color),
            rdpSetScissor(0, 0, 5, 3),
            (0x3DULL << 56) | (2ULL << 51) | (1ULL << 32) | Texture,
            rdpRawSetTile(0, 0, 2, 1, 0, 0, true, false, 0, 0, true, false, 0, 0),
            rdpRawSetTileSize(0x34, 0, 0, 0, 4, 4),
            rdpRawSetTileSize(0x32, 0, 0, 0, boundS, 4),
            // AA on, 1-cycle; sample type and the first texture filter MUX.
            (0x2FULL << 56) | filterMode | 8 | 1,
            (0x39ULL << 56) | 200, // alpha compare rejects missing LOD_FRAC
            // RGB = TEXEL0 * SHADE, A = TEXEL0 alpha * LOD_FRAC.
            rdpRawSetCombineFull(0, 0, 31, 7, 0, 0, 7, 7, 1, 8, 4, 7, 1, 7, 0, 7),
            // Right major XH=5. Minor edge starts at X=0.25, Y=0.25.
            rdpRawTriangle(0x0E, 0, 0, false, 1, 1, 12, true, true, false),
            rdpEdgeFixed(16384, 0), rdpEdge(5, 0), rdpEdgeFixed(16384, 0),
            rdpShadeWord(114, 255, 0, 255), 16ULL << 48, 0xC000ULL << 48, 0x8000ULL << 48,
            8ULL << 48, 8ULL << 48, 0x8000ULL << 48, 0x8000ULL << 48,
        };
        std::vector<u64> list(commands.begin(), commands.end());
        // S starts at 32 at XH; +8 per pixel. T is half a texel.
        list.insert(list.end(), {(32ULL << 48) | (16ULL << 32), 8ULL << 48, 0, 0, 0, 0, 0, 0});
        rdp.submitCommands(list.data(), list.size());

        // R = 32.25 + 16.5*x + 8.5*y, corrected to the first covered
        // subpixel at the edges. G is the three-point sample of the yellow
        // corner; the upper half falls back to red at the fourth corner.
        constexpr std::array<u32, 5> red{49, 57, 73, 90, 106};
        const std::array<u32, 5> green{0, 0, 64, 128, boundS == 3 ? 0u : 128u};
        const bool filter = filterMode == ((1ULL << 45) | (1ULL << 43));
        for (u32 x = 0; x < 5; ++x) {
            const u32 rgb = rdp.pixel(x, 1) & 0xFFFFFF;
            assert(rgb == (red[x] | ((filter ? green[x] : 0) << 8)));
        }
        assert((rdp.pixel(0, 0) & 255) == 38);
        assert(bus.read8(Color + (5 + 0) * 4 + 3) == 0xA0);
        assert(bus.read8(Color + (5 + 1) * 4 + 3) == 0xE0);
    }
}

void test_rdp_settile_shift_all_textured_triangles() {
    constexpr u32 Texture = 0x2000, Color = 0x3000;
    // Sixteen source rows of 16 RGBA16 texels; SET_TILE line is 64-bit words.
    const auto texel = [](u32 index) -> u16 {
        return static_cast<u16>(0xF801u + index * 0x33DDu);
    };

    // Hand-derived expectations, not a copy of the shift implementation.
    // Small coordinates exercise right shifts 0..10. For shifts 11..15 the
    // coordinate is 1 << (shift - 1): the signed-16-bit wrap makes it negative,
    // so clamped sampling selects texel 0. The old unbounded s64 path instead
    // clamped to the tile's far edge and selected texel 15/7.
    constexpr std::array<u32, 16> sCoordinate = {
        256, 256, 256, 256, 0, 0, 0, 0, 0, 0, 0, 1024, 2048, 4096, 8192, 16384,
    };
    constexpr std::array<u32, 16> sTexel = {
        8, 4, 2, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    };
    constexpr std::array<u32, 16> tCoordinate = {
        480, 480, 480, 480, 0, 0, 0, 0, 0, 0, 0, 1024, 2048, 4096, 8192, 16384,
    };
    constexpr std::array<u32, 16> tTexel = {
        15, 7, 3, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    };

    for (const u32 op : {0x0Eu, 0x0Fu}) {
        for (u32 shiftS = 0; shiftS < 16; ++shiftS) {
            for (u32 shiftT = 0; shiftT < 16; ++shiftT) {
                Bus bus;
                Rdp rdp;
                rdp.setFramebufferSize(4, 2);
                rdp.setBus(&bus);
                rdp.setHw(&bus.hw());
                for (u32 i = 0; i < 256; ++i)
                    bus.write16(Texture + i * 2, texel(i));

                const u32 expectedIndex = tTexel[shiftT] * 16 + sTexel[shiftS];
                const u16 raw = texel(expectedIndex);
                const auto expand = [](u32 n) { return (n << 3) | (n >> 2); };
                const u32 expected = (expand((raw >> 1) & 31) << 16) |
                                     (expand((raw >> 6) & 31) << 8) |
                                     expand((raw >> 11) & 31);

                std::array<u64, 8> tex{};
                const auto writeCoeff = [&](u32 channel, s32 base, s32 dx, s32 de) {
                    const u32 bit = 48 - 16 * channel;
                    const auto upper = [](s32 value) { return static_cast<u16>(static_cast<u32>(value) >> 16); };
                    const auto lower = [](s32 value) { return static_cast<u16>(static_cast<u32>(value)); };
                    tex[0] |= static_cast<u64>(upper(base)) << bit;
                    tex[2] |= static_cast<u64>(lower(base)) << bit;
                    tex[1] |= static_cast<u64>(upper(dx)) << bit;
                    tex[3] |= static_cast<u64>(lower(dx)) << bit;
                    tex[4] |= static_cast<u64>(upper(de)) << bit;
                    tex[6] |= static_cast<u64>(lower(de)) << bit;
                };
                // Constant S/T per pixel keeps the hand-derived indices exact;
                // perspective is off.
                writeCoeff(0, static_cast<s32>(sCoordinate[shiftS] << 16), 0, 0);
                writeCoeff(1, static_cast<s32>(tCoordinate[shiftT] << 16), 0, 0);
                writeCoeff(2, 0x8000 << 16, 0, 0);

                // 0x0E encodes SHADE | TEXTURE; 0x0F adds the z-buffer flag.
                const bool zbuffer = op == 0x0F;
                std::vector<u64> commands{
                    (0x3DULL << 56) | (2ULL << 51) | (15ULL << 32) | Texture,
                    rdpRawSetTile(0, 0, 2, 4, 0, 0, true, false, 0, shiftT,
                                  true, false, 0, shiftS),
                    rdpRawSetTileSize(0x34, 0, 0, 0, 60, 60),
                    rdpRawSetTileSize(0x32, 0, 0, 0, 60, 60),
                    rdpSetColorImage(4, Color),
                    rdpSetScissor(0, 0, 4, 2),
                    0x2FULL << 56, // 1-cycle
                    rdpRawSetCombine(1, 0, 6, 0, 1, 0, 6, 0),
                    rdpRawTriangle(op, 0, 3, true, 0, 0, 8, true, true, zbuffer),
                    rdpEdge(4, 0), rdpEdge(0, 0), rdpEdge(4, 0),
                    rdpShadeWord(255, 255, 255, 255),
                };
                for (int i = 0; i < 7; ++i) commands.push_back(0);
                commands.insert(commands.end(), tex.begin(), tex.end());
                if (zbuffer) {
                    commands.push_back(0);
                    commands.push_back(0);
                }
                rdp.submitCommands(commands.data(), commands.size());

                for (u32 y = 0; y < 2; ++y)
                    for (u32 x = 0; x < 4; ++x)
                        assert((rdp.pixel(x, y) & 0x00FFFFFF) == expected);
                assert(rdp.stats().pixelsDrawn == 8);
            }
        }
    }
}

void test_rdp_rgba32_wide_rectangle() {
    constexpr u32 Texture = 0x2000, Color = 0x3000, Width = 24, Height = 4;
    Bus bus;
    Rdp rdp;
    rdp.setFramebufferSize(Width, Height);
    rdp.setBus(&bus);
    const auto texel = [](u32 x, u32 y) {
        return ((x * 9 + 3) << 24) | ((y * 61 + 7) << 16) | ((x + 31 * y) << 8) | 255;
    };
    for (u32 y = 0; y < Height; ++y)
        for (u32 x = 0; x < Width; ++x)
            bus.write32(Texture + (y * Width + x) * 4, texel(x, y));
    const u64 commands[] = {
        (0x3DULL << 56) | (3ULL << 51) | (u64{Width - 1} << 32) | Texture,
        rdpRawSetTile(0, 0, 3, Width / 4, 0, 0, false, false, 0, 0, false, false, 0, 0),
        rdpRawSetTileSize(0x34, 0, 0, 0, (Width - 1) * 4, (Height - 1) * 4),
        rdpSetColorImage(Width, Color),
        rdpSetScissor(0, 0, Width, Height),
        0x2FULL << 56,
        rdpRawSetCombine(1, 0, 6, 0, 1, 0, 6, 0),
        (0x24ULL << 56) | (u64{Width * 4} << 44) | (u64{Height * 4} << 32),
        (1024ULL << 16) | 1024ULL,
    };
    rdp.submitCommands(commands, std::size(commands));
    for (u32 y = 0; y < Height; ++y)
        for (u32 x = 0; x < Width; ++x)
            assert((bus.read32(Color + (y * Width + x) * 4) & 0xFFFFFF00) ==
                   (texel(x, y) & 0xFFFFFF00));
}

void test_rdp_alpha_compare_uses_geometric_coverage() {
    for (const bool twoCycle : {false, true}) {
        for (const u32 threshold : {128u, 129u}) {
            Bus bus;
            Rdp rdp;
            rdp.setFramebufferSize(1, 1);
            rdp.setBus(&bus);
            constexpr u32 Color = 0x3000;
            const u64 commands[] = {
                rdpSetColorImage(1, Color), rdpSetScissor(0, 0, 1, 1),
                (0x2FULL << 56) | (u64{twoCycle} << 52) | 0x3001, // coverage times alpha, select, compare
                (0x39ULL << 56) | threshold, // BLEND_COLOR
                (0x3AULL << 56) | 0xF8F8F8FFULL, // PRIM_COLOR
                rdpRawSetCombineFull(3, 0, 6, 0, 0, 0, 0, 3,
                                     3, 0, 6, 0, 0, 0, 0, 3),
                rdpRawTriangle(0x08, 0, 0, true, 0, 0, 4, false, false, false),
                u64{0x8000} << 32, 0, u64{0x8000} << 32, // [0, 1/2) covers 4 samples
            };
            rdp.submitCommands(commands, std::size(commands));
            // 0xFF expands to 0x100 before modulating 4/8 coverage.
            assert((bus.read32(Color) != 0) == (threshold == 128));
        }
    }
}

void test_rdp_threaded_output_matches_serial() {
    constexpr u32 Texture = 0x2000, Color = 0x3000;
    constexpr u32 W = 128, H = 64;
    // Alternating alpha bits exercise the noise-threshold alpha compare path.
    constexpr std::array<u16, 8> texels = {
        0xF801, 0x07C0, 0x003F, 0xFFFE, 0xF81F, 0x07FE, 0xFC01, 0x03E0,
    };

    struct RenderResult {
        std::vector<u32> framebuffer;
        std::vector<u32> framebufferWords;
        u64 pixels = 0;
    };
    const auto render = [&](unsigned threads) {
        Bus bus;
        Rdp rdp;
        rdp.setFramebufferSize(W, H);
        rdp.setThreadCount(threads);
        rdp.setBus(&bus);
        rdp.setHw(&bus.hw());
        for (u32 i = 0; i < texels.size(); ++i)
            bus.write16(Texture + i * 2, texels[i]);

        std::array<u64, 8> tex{};
        const auto writeCoeff = [&](u32 channel, s32 base, s32 dx, s32 de) {
            const u32 shift = 48 - 16 * channel;
            const auto upper = [](s32 value) { return static_cast<u16>(static_cast<u32>(value) >> 16); };
            const auto lower = [](s32 value) { return static_cast<u16>(static_cast<u32>(value)); };
            tex[0] |= static_cast<u64>(upper(base)) << shift;
            tex[2] |= static_cast<u64>(lower(base)) << shift;
            tex[1] |= static_cast<u64>(upper(dx)) << shift;
            tex[3] |= static_cast<u64>(lower(dx)) << shift;
            tex[4] |= static_cast<u64>(upper(de)) << shift;
            tex[6] |= static_cast<u64>(lower(de)) << shift;
        };
        writeCoeff(0, 0, 32 << 16, 0);
        writeCoeff(1, 0, 0, 32 << 16);
        writeCoeff(2, 0x8000 << 16, 0, 0);

        std::vector<u64> commands{
            (0x3DULL << 56) | (2ULL << 51) | (7ULL << 32) | Texture,
            rdpRawSetTile(0, 0, 2, 1, 0, 0, false, false, 1, 0, false, false, 2, 0),
            rdpRawSetTileSize(0x34, 0, 0, 0, 28, 8),
            rdpRawSetTileSize(0x32, 0, 0, 0, 28, 8),
            rdpSetColorImage(W, Color),
            rdpSetScissor(0, 0, W, H),
            0x2FULL << 56 | 3ULL, // alpha compare with noise threshold
            rdpRawSetCombineFull(1, 0, 6, 0, 0, 0, 1, 1,
                                 1, 0, 6, 0, 0, 0, 1, 1),
            rdpRawTriangle(0x0A, 0, 0, true, 0, 0, H * 4, false, true, false),
            rdpEdge(W, 0), rdpEdge(0, 0), rdpEdge(W, 0),
        };
        commands.insert(commands.end(), tex.begin(), tex.end());
        rdp.submitCommands(commands.data(), commands.size());

        RenderResult result;
        result.framebuffer = rdp.framebuffer();
        result.framebufferWords.reserve(W * H);
        for (u32 i = 0; i < W * H; ++i)
            result.framebufferWords.push_back(bus.read32(Color + i * 4));
        result.pixels = rdp.stats().pixelsDrawn;
        return result;
    };

    Rdp threadCheck;
    threadCheck.setThreadCount(4);
    assert(threadCheck.effectiveThreadCount() == 4);

    const auto serial = render(1);
    const auto threaded = render(4);
    const auto automatic = render(0);
    assert(serial.pixels > 0);
    assert(threaded.pixels == serial.pixels);
    assert(threaded.framebuffer == serial.framebuffer);
    assert(threaded.framebufferWords == serial.framebufferWords);
    assert(automatic.pixels == serial.pixels && automatic.framebuffer == serial.framebuffer &&
           automatic.framebufferWords == serial.framebufferWords);
}

void test_rdp_ci8_texrect_copy_and_one_cycle() {
    constexpr u64 Op = 1ULL << 56;
    constexpr u64 Tile7 = 7ULL << 24;
    constexpr u32 Palette = 0x6000, Texture = 0x7000, Color = 0x3000;
    constexpr std::array<u16, 8> palette = {
        0xF801, 0x07C1, 0x003F, 0xFFFF, 0x0000, 0xF81F, 0x07FF, 0xF83F,
    };

    Bus bus;
    Rdp rdp;
    rdp.setFramebufferSize(8, 8);
    rdp.setBus(&bus);
    rdp.setHw(&bus.hw());
    auto& dp = bus.hw().dp();
    dp.current = dp.end = 0x1000;
    const auto emit = [&](u64 w) { bus.write64(dp.end, w); dp.end += 8; };

    for (u32 i = 0; i < palette.size(); ++i) bus.write16(Palette + i * 2, palette[i]);
    for (u32 i = 0; i < palette.size(); ++i) bus.write8(Texture + i, static_cast<u8>(i));
    emit(0x3D * Op | (2ULL << 51) | (7ULL << 32) | Palette);
    emit(0x35 * Op | (2ULL << 53) | (0x100ULL << 32) | Tile7);
    emit(0x30 * Op | Tile7 | (32ULL << 12));
    emit(0x3D * Op | (1ULL << 51) | (7ULL << 32) | Texture);
    emit(0x35 * Op | (1ULL << 51) | (1ULL << 41) | Tile7);
    emit(0x33 * Op | Tile7 | (7ULL << 12));
    emit(0x35 * Op | (2ULL << 53) | (1ULL << 51) | (1ULL << 41) | Tile7);
    // LOADBLOCK writes raw 12.0 extents; restore 10.2 bounds for sampling.
    emit(0x32 * Op | Tile7 | (28ULL << 12));
    emit(0x3F * Op | (2ULL << 51) | (7ULL << 32) | Color);
    emit(rdpSetScissor(0, 0, 8, 8));
    // One-cycle pass-through: (0 - 0) * 0 + TEXEL0. This isolates the CI8
    // TLUT and TEXRECT stepping behavior from the color combiner.
    emit(0x3C * Op | (31ULL << 32) | (1ULL << 6) | (7ULL << 18) | 1);

    // COPY mode is inclusive at the lower-right edge and advances four
    // pixels per clock, so DSDX is 4<<10 for one texel per output pixel.
    emit(0x2F * Op | (2ULL << 52) | (1ULL << 47));
    emit(0x24 * Op | Tile7 | (28ULL << 44));
    emit(4096ULL << 16);
    rdp.process();
    for (u32 x = 0; x < palette.size(); ++x) {
        assert(bus.read16(Color + x * 2) == palette[x]);
        assert(bus.read16(Color + 16 + x * 2) == 0);
    }

    // 1-cycle excludes the lower-right edge, uses DSDX=1<<10, and runs the
    // combiner/TLUT path against the same CI8 palette on the next row.
    emit(0x2F * Op | (1ULL << 47));
    emit(0x24 * Op | Tile7 | (32ULL << 44) | (8ULL << 32) | 4);
    emit(1024ULL << 16);
    rdp.process();
    for (u32 x = 0; x < palette.size(); ++x) {
        // COPY bypasses the blender and preserves texel alpha. The 1-cycle
        // path stores coverage in RGBA16's low alpha bit, so only RGB is
        // expected to match the source palette here.
        const u16 written = bus.read16(Color + 16 + x * 2);
        assert((written & 0xFFFE) == (palette[x] & 0xFFFE));
        assert((written & 1) == 1); // full-coverage fragments
    }
}

// ---------------------------------------------------------------------------
// OSTask / RSP HLE tests
// ---------------------------------------------------------------------------

static void writeDmem(Rsp& rsp, u32 offset, const u8* data, u32 len) {
    for (u32 i = 0; i < len; ++i) rsp.writeMem8((offset + i) & 0xFFF, data[i]);
}

static void writeRdramU32(Bus& bus, u32 addr, u32 value) {
    bus.write8(addr + 0, static_cast<u8>(value >> 24));
    bus.write8(addr + 1, static_cast<u8>(value >> 16));
    bus.write8(addr + 2, static_cast<u8>(value >> 8));
    bus.write8(addr + 3, static_cast<u8>(value));
}

void test_ostask_parsing() {
    Rsp rsp;
    // OSTask at 0xFC0: type=1, flags=2, data_ptr=0x80200000
    u8 task[64] = {};
    auto be = [](u32 v) {
        return std::array<u8, 4>{static_cast<u8>(v >> 24), static_cast<u8>(v >> 16),
                                 static_cast<u8>(v >> 8), static_cast<u8>(v)};
    };
    const auto t = be(1);
    const auto f = be(2);
    const auto dp = be(0x80200000);
    std::memcpy(task + 0x00, t.data(), 4);
    std::memcpy(task + 0x04, f.data(), 4);
    std::memcpy(task + 0x30, dp.data(), 4);
    writeDmem(rsp, 0xFC0, task, 64);

    u8 dmem[0x1000];
    for (u32 i = 0; i < 0x1000; ++i) dmem[i] = rsp.readMem8(i);
    const OSTask ot = parseOSTask(dmem);
    assert(ot.type == 1);
    assert(ot.flags == 2);
    assert(ot.dataPtr == 0x80200000);
}

void test_rsp_hle_microcode_detection() {
    Bus bus;
    Rdp rdp;
    rdp.setBus(&bus);
    rdp.setHw(&bus.hw());
    RspHle hle(&bus, &rdp);

    OSTask task{};
    task.type = 1;
    task.dataPtr = 0; // no display list needed for detection
    task.ucode = 0x80001000;
    task.ucodeSize = 0x1000;
    assert(hle.runGraphicsTask(task));
    assert(hle.detectedMicrocode() == MicrocodeVersion::F3DEX);
    assert(!hle.microcodeName().empty());
}

void test_rsp_hle_invalid_task() {
    Bus bus;
    Rdp rdp;
    RspHle hle(&bus, &rdp);
    OSTask task{};
    task.type = 2; // not graphics
    assert(!hle.runGraphicsTask(task));
}

void test_rsp_hle_display_list_end() {
    Bus bus;
    Rdp rdp;
    rdp.setBus(&bus);
    rdp.setHw(&bus.hw());
    RspHle hle(&bus, &rdp);

    // Display list at RDRAM 0x1000 containing just G_ENDDL.
    writeRdramU32(bus, 0x1000, 0xB8000000);
    writeRdramU32(bus, 0x1004, 0x00000000);

    OSTask task{};
    task.type = 1;
    task.dataPtr = 0x80001000; // KSEG0 address
    assert(hle.runGraphicsTask(task));
    assert(hle.commandsProcessed() >= 1);
    assert(hle.trianglesSubmitted() == 0);
}

void test_rsp_hle_matrix_transform() {
    Bus bus;
    Rdp rdp;
    rdp.setBus(&bus);
    rdp.setHw(&bus.hw());
    RspHle hle(&bus, &rdp);

    // Set projection matrix to identity and modelview to a translation.
    // Mtx format: column-major integer part followed by fraction part.
    auto makeTranslationMtx = [](float tx, float ty, float tz) {
        std::array<u8, 64> m{};
        // Identity columns, then translation in column 3.
        // Column 0: (1,0,0,0)
        m[0] = 0; m[1] = 1;   // int 1
        // Column 1: (0,1,0,0)
        m[8] = 0; m[9] = 1;   // int 1 at row1 col1
        // Column 2: (0,0,1,0)
        m[20] = 0; m[21] = 1; // int 1 at row2 col2
        // Column 3: translation (tx,ty,tz,1)
        const s16 ix = static_cast<s16>(tx);
        const s16 iy = static_cast<s16>(ty);
        const s16 iz = static_cast<s16>(tz);
        m[24] = static_cast<u8>(ix >> 8); m[25] = static_cast<u8>(ix);
        m[26] = static_cast<u8>(iy >> 8); m[27] = static_cast<u8>(iy);
        m[28] = static_cast<u8>(iz >> 8); m[29] = static_cast<u8>(iz);
        m[30] = 0; m[31] = 1;
        // Fraction part starts at +32; leave zero.
        return m;
    };

    const auto mtx = makeTranslationMtx(10.0f, 20.0f, 0.0f);
    for (u32 i = 0; i < 64; ++i) bus.write8(0x2000 + i, mtx[i]);

    // Load modelview matrix (params=load, no push, modelview -> 0x02)
    writeRdramU32(bus, 0x1000, 0x01020000);
    writeRdramU32(bus, 0x1004, 0x80002000);
    // Set viewport so screen coords are predictable.
    // G_MOVEMEM viewport at 0x1008
    writeRdramU32(bus, 0x1008, 0x03860000);
    writeRdramU32(bus, 0x100C, 0x80002040);
    // viewport data at 0x2040: vscale (160,120), vtrans (160,120)
    writeRdramU32(bus, 0x2040, 0x00A00078);
    writeRdramU32(bus, 0x2044, 0x00000000);
    writeRdramU32(bus, 0x2048, 0x00A00078);
    writeRdramU32(bus, 0x204C, 0x00000000);
    // Vertex at 0x2050: pos (0,0,0), tex (0,0), rgba (255,0,0,255)
    bus.write8(0x2050, 0); bus.write8(0x2051, 0);
    bus.write8(0x2052, 0); bus.write8(0x2053, 0);
    bus.write8(0x2054, 0); bus.write8(0x2055, 0);
    bus.write8(0x2056, 0); bus.write8(0x2057, 0);
    bus.write8(0x2058, 0); bus.write8(0x2059, 0);
    bus.write8(0x205A, 0); bus.write8(0x205B, 0);
    bus.write8(0x205C, 255); bus.write8(0x205D, 0);
    bus.write8(0x205E, 0); bus.write8(0x205F, 255);
    // F3DEX G_VTX: n=1, v0=0, address=0x80002050
    writeRdramU32(bus, 0x1010, 0x04001002);
    writeRdramU32(bus, 0x1014, 0x80002050);
    // G_ENDDL
    writeRdramU32(bus, 0x1018, 0xB8000000);
    writeRdramU32(bus, 0x101C, 0x00000000);

    OSTask task{};
    task.type = 1;
    task.dataPtr = 0x80001000;
    assert(hle.runGraphicsTask(task));
    assert(hle.verticesLoaded() == 1);
}

void test_rsp_hle_setothermode_encodings() {
    Bus bus;
    Rdp rdp;
    rdp.setBus(&bus);
    rdp.setHw(&bus.hw());
    RspHle hle(&bus, &rdp);

    constexpr u32 Dl = 0x1000;
    u32 p = Dl;
    const auto emit = [&](u32 hi, u32 lo) {
        writeRdramU32(bus, p, hi);
        writeRdramU32(bus, p + 4, lo);
        p += 8;
    };
    const auto run = [&](MicrocodeVersion expected) {
        OSTask task{};
        task.type = 1;
        task.ucode = 0x80001000;
        task.ucodeSize = 0x1000;
        task.dataPtr = 0x80001000;
        assert(hle.runGraphicsTask(task));
        assert(hle.detectedMicrocode() == expected);
        assert((hle.otherModes() & (3ULL << 4)) == (2ULL << 4));
        assert((hle.otherModes() & (3ULL << 52)) == (1ULL << 52));
        p = Dl;
    };

    // Legacy GBI stores the absolute low-mode shift and the field length.
    emit(0xB9000402, 2); // G_SETOTHERMODE_L: shift 4, length 2
    emit(0xBA001402, 1); // G_SETOTHERMODE_H: shift 20, length 2
    emit(0xB8000000, 0);
    run(MicrocodeVersion::F3DEX);

    // F3DEX2 stores 32 - shift - length for both halves and length - 1.
    emit(0xE2001A01, 2); // G_SETOTHERMODE_L: shift 4, length 2
    emit(0xE3000A01, 1); // G_SETOTHERMODE_H: half shift 20, length 2
    emit(0xDF000000, 0);
    run(MicrocodeVersion::F3DEX2);
}

void test_rsp_hle_f3dex2_textured_triangle_pipeline() {
    Bus bus;
    Rdp rdp;
    rdp.setBus(&bus);
    rdp.setHw(&bus.hw());
    RspHle hle(&bus, &rdp);

    constexpr u32 Dl = 0x1000, Vp = 0x2040, Vtx = 0x2060, Texture = 0x3000, Color = 0x4000;
    const auto writeVtx = [&](u32 address, s16 x, s16 y, s16 z, s16 s, s16 t,
                              u8 r, u8 g, u8 b, u8 a) {
        const auto word16 = [](s16 v) -> u8 { return static_cast<u8>(static_cast<u16>(v) >> 8); };
        bus.write8(address + 0, word16(x));
        bus.write8(address + 1, static_cast<u8>(x));
        bus.write8(address + 2, word16(y));
        bus.write8(address + 3, static_cast<u8>(y));
        bus.write8(address + 4, word16(z));
        bus.write8(address + 5, static_cast<u8>(z));
        bus.write8(address + 6, 0);
        bus.write8(address + 7, 0);
        bus.write8(address + 8, word16(s));
        bus.write8(address + 9, static_cast<u8>(s));
        bus.write8(address + 10, word16(t));
        bus.write8(address + 11, static_cast<u8>(t));
        bus.write8(address + 12, r);
        bus.write8(address + 13, g);
        bus.write8(address + 14, b);
        bus.write8(address + 15, a);
    };

    // Identity viewport: vertex X/Y are already screen pixels.
    writeRdramU32(bus, Vp + 0, 0x00010001);
    writeRdramU32(bus, Vp + 4, 0);
    writeRdramU32(bus, Vp + 8, 0);
    writeRdramU32(bus, Vp + 12, 0);

    writeVtx(Vtx + 0, 4, 0, 0, 0, 0, 255, 255, 255, 255);
    writeVtx(Vtx + 16, 0, 8, 0, 0, 96, 255, 255, 255, 255);
    writeVtx(Vtx + 32, 8, 16, 0, 96, 0, 255, 255, 255, 255);
    writeVtx(Vtx + 48, 12, 18, 0, 96, 96, 255, 255, 255, 255);

    // Four distinct RGBA16 texels loaded through a real LOADBLOCK.
    const u16 texels[] = {0xF800, 0x07C0, 0x001F, 0xFFE0};
    for (u32 i = 0; i < 4; ++i) bus.write16(Texture + i * 2, texels[i]);

    u32 p = Dl;
    const auto emit = [&](u32 hi, u32 lo) {
        writeRdramU32(bus, p, hi);
        writeRdramU32(bus, p + 4, lo);
        p += 8;
    };
    emit(0xE2001E01, 0);             // G_SETOTHERMODE_L: alpha compare off
    emit(0xE3000A01, 0);             // G_SETOTHERMODE_H: one cycle
    emit(0xE3000C00, 1);             // G_SETOTHERMODE_H: perspective enabled
    emit(0xFF10000F, Color);         // G_SETCIMG RGBA16, width 16
    emit(0xED000000, 0x00040040);    // G_SETSCISSOR 0,0 - 16,16
    emit(0xFD100003, Texture);       // G_SETTIMG RGBA16, width 4
    emit(0xF5100200, 0x00008020);    // G_SETTILE tile 0, masks 2
    emit(0xF200000C, 0x0000C000);    // G_SETTILESIZE tile 0, 4 texels
    emit(0xF3000000, 0x00003000);    // G_LOADBLOCK tile 0, 4 texels
    emit(0xFC131C26, 0xFF3BFFFF);    // G_SETCOMBINE: pass through TEXEL0
    emit(0xDC080008, 0x80002040);    // F3DEX2 G_MOVEMEM viewport
    emit(0xD7000002, 0x00010001);    // F3DEX2 G_TEXTURE tile 0, scale 1.0
    emit(0x01004008, 0x80002060);    // F3DEX2 G_VTX 4 vertices at v0
    emit(0x07000204, 0x00000406);    // F3DEX2 G_QUAD (v0,v1,v2) + (v0,v2,v3)
    emit(0xDF000000, 0);             // F3DEX2 G_ENDDL

    OSTask task{};
    task.type = 1;
    task.ucode = 0x80001000;
    task.ucodeSize = 0x1000;
    task.dataPtr = 0x80001000;
    assert(hle.runGraphicsTask(task));

    assert(hle.detectedMicrocode() == MicrocodeVersion::F3DEX2);
    assert(hle.microcodeName() == "F3DEX2");
    assert(hle.verticesLoaded() == 4);
    assert(hle.trianglesSubmitted() == 2);
    assert(rdp.stats().texturedTriangles >= 2);

    std::vector<u32> sampled;
    for (u32 y = 0; y < 16; ++y) {
        for (u32 x = 0; x < 16; ++x) {
            const u32 color = rdp.pixel(x, y);
            if ((color & 0xFFFFFF) != 0) sampled.push_back(color);
        }
    }
    assert(sampled.size() >= 20);
    std::sort(sampled.begin(), sampled.end());
    const auto distinct = std::unique(sampled.begin(), sampled.end()) - sampled.begin();
    assert(distinct >= 3);

    // A flat-shaded triangle would write the same vertex color everywhere.
    // Textured sampling must expose several different decoded texels.
    u32 minRed = 255, maxRed = 0;
    for (const u32 color : sampled) {
        const u32 red = color & 255;
        minRed = std::min(minRed, red);
        maxRed = std::max(maxRed, red);
    }
    assert(maxRed > minRed + 32);
}

void test_rsp_break_and_sp_cop0() {
    Bus bus;
    Rsp rsp;
    bus.attachRsp(&rsp);
    rsp.setBus(&bus);

    // Write OSTask to DMEM 0xFC0.
    u8 task[64] = {};
    auto be = [](u32 v) {
        return std::array<u8, 4>{static_cast<u8>(v >> 24), static_cast<u8>(v >> 16),
                                 static_cast<u8>(v >> 8), static_cast<u8>(v)};
    };
    std::memcpy(task + 0x00, be(1).data(), 4);          // type = M_GFXTASK
    std::memcpy(task + 0x30, be(0x80001000).data(), 4); // data_ptr (KSEG0)
    for (u32 i = 0; i < 64; ++i) bus.write8(0x1800 + i, task[i]);
    bus.write32(0x04040000, 0xFC0);
    bus.write32(0x04040004, 0x1800);
    bus.write32(0x04040008, 63);
    assert(rsp.readMem32(0xFC0) == 1);
    assert(rsp.readMem32(0xFF0) == 0x80001000);

    // Read the shared semaphore twice, set SIG2 in microcode, then BREAK.
    writeProgram(bus, 0x04001000, {
        0x40013800, 0x40023800, 0x40803800, // MFC0 / MTC0 semaphore
        addiu(3, 0, 0x4000), 0x40832000, // set SIG2 via SP_STATUS
        0x0000000D, addiu(4, 0, 1),
    });
    rsp.step();
    assert(rsp.pc() == 0); // HALT gates instruction execution
    bus.write32(0x04040010, 0x101);
    for (int i = 0; i < 7; ++i) rsp.step();

    assert(rsp.regs()[1] == 0 && rsp.regs()[2] == 1 && rsp.regs()[4] == 0);
    assert(rsp.pc() == 24);
    assert(bus.read32(0x0404001C) == 0);
    assert((bus.hw().sp().status & 0x0200) != 0); // TASKDONE
    assert((bus.hw().sp().status & 0x0001) != 0); // HALT
    assert((bus.hw().sp().status & 0x0002) != 0); // BROKE
    assert((bus.hw().mi().interrupt & 0x01) != 0); // MI SP interrupt
}

void test_rsp_cop0_dma() {
    Bus bus;
    Rsp rsp;
    bus.attachRsp(&rsp);
    rsp.setBus(&bus);
    bus.write64(0x200, 0x0102030405060708ULL);
    bus.write64(0x208, 0x1112131415161718ULL);
    writeProgram(bus, 0x04001000, {
        addiu(1, 0, 0xFF8), 0x40810000, // SP_MEM_ADDR
        addiu(1, 0, 0x200), 0x40810800, // SP_DRAM_ADDR
        addiu(1, 0, 15), 0x40811000, // SP_RD_LEN
        0x40020000, 0x40030800, // updated SP_MEM_ADDR / SP_DRAM_ADDR
        addiu(1, 0, 0xFF8), 0x40810000,
        0x40841800, // SP_WR_LEN = 0, aligned up to eight bytes
        0x0000000D,
    });
    bus.write32(0x04040010, 1); // interrupt on BREAK left disabled
    for (u32 i = 0; i < 12; ++i) rsp.step();
    assert(bus.read64(0x04000FF8) == 0x0102030405060708ULL);
    assert(bus.read64(0x04000000) == 0x1112131415161718ULL);
    assert(bus.read64(0x210) == 0x0102030405060708ULL);
    assert(rsp.regs()[2] == 8 && rsp.regs()[3] == 0x210);
    assert((bus.hw().sp().status & 0x203) == 3 && !(bus.hw().mi().interrupt & 1));
}

// ---------------------------------------------------------------------------

void test_cpu_regimm_likely() {
    for (const u32 sub : {2u, 3u, 0x12u, 0x13u}) {
        for (const s32 value : {-1, 0, 1}) {
            Bus bus;
            Cpu cpu;
            cpu.setPc(Kseg0 + 0x400);
            writeProgram(bus, 0x400, {
                addiu(1, 0, value),
                (1u << 26) | (1u << 21) | (sub << 16) | 2u,
                addiu(2, 0, 7),
                addiu(3, 0, 9),
                0,
            });
            cpu.step(bus);
            cpu.step(bus);
            const bool taken = sub & 1 ? value >= 0 : value < 0;
            assert(cpu.pc() == Kseg0 + (taken ? 0x408 : 0x40C));
            cpu.step(bus);
            assert(cpu.pc() == Kseg0 + 0x410 && cpu.gpr()[2] == (taken ? 7 : 0));
            assert(cpu.gpr()[31] == (sub & 0x10 ? Kseg0 + 0x40C : 0));
        }
    }
}

void test_cpu_trunc_word() {
    Bus bus;
    Cpu cpu;
    cpu.setPc(Kseg0 + 0x400);
    cpu.setIplHandoffRegisters(1, 1, 0, 0); // CU1 and FR enabled
    writeProgram(bus, 0x400, {
        0x3C01C070, //  -3.75f, upper half
        0x44810800, // MTC1 r1,f1
        0x4600088D, // TRUNC.W.S f2,f1
        0x44021000, // MFC1 r2,f2
        0x3C017F80, // +infinity
        0x44810800,
        0x4600088D,
    });
    for (int i = 0; i < 4; ++i) cpu.step(bus);
    assert(cpu.gpr()[2] == static_cast<u64>(-3) && (cpu.fcr31() & 0x1004) == 0x1004);
    for (int i = 0; i < 3; ++i) cpu.step(bus);
    assert(cpu.pc() == 0xFFFFFFFF80000180ULL);
    assert(((cpu.cop0()[13] >> 2) & 31) == 15 && (cpu.fcr31() & 0x3F000) == 0x20000);
    assert(static_cast<u32>(cpu.fpr()[2]) == static_cast<u32>(-3));
}

void test_cpu_cop1_word_rounding() {
    struct Case { double input; u32 op, fcr; s32 expected; u32 cause; };
    constexpr u32 Inexact = 1 << 12, Unimplemented = 1 << 17;
    constexpr Case cases[] = {
        { 2.5, 0x0C, 3,  2, Inexact}, // nearest even ignores RM
        { 3.5, 0x0C, 1,  4, Inexact},
        {-2.5, 0x0C, 2, -2, Inexact},
        {-3.5, 0x0C, 2, -4, Inexact},
        {-3.75,0x0D, 0, -3, Inexact},
        {-3.75,0x0E, 0, -3, Inexact},
        {-3.25,0x0F, 0, -4, Inexact},
        { 2.25,0x24, 2,  3, Inexact}, // CVT.W follows RM
        { 2.75,0x24, 1,  2, Inexact},
        {-2.25,0x24, 3, -3, Inexact},
        { 2.0, 0x0C, 4,  2, 0}, // Sticky flag and condition survive exact result.
        { 3.5, 0x0C, 1 << 7, 0, Inexact}, // Enabled inexact leaves destination unwritten.
        { 2147483647.75, 0x0C, 0, 0, Unimplemented},
    };
    for (const auto& tc : cases) for (const bool dbl : {false, true}) {
        if (!dbl && tc.input > 0x1p30) continue;
        Bus bus;
        Cpu cpu;
        cpu.setPc(Kseg0 + 0x400);
        cpu.setIplHandoffRegisters(1, 1, 0, 0);
        constexpr u32 Condition = 1 << 23;
        const bool traps = tc.cause == Unimplemented || ((tc.fcr & (1 << 7)) && tc.cause);
        if (dbl) bus.write64(0x700, std::bit_cast<u64>(tc.input));
        else bus.write32(0x700, std::bit_cast<u32>(static_cast<float>(tc.input)));
        writeProgram(bus, 0x400, {
            lui(1, 0x8000), lui(3, 0x0080), ori(3, 3, tc.fcr), encodeCop1(6, 3, 31),
            ori(4, 0, 0x1234), mtc1(4, 2),
            encodeI(dbl ? 0x35 : 0x31, 1, 1, 0x700),
            encodeCop1(dbl ? 0x11 : 0x10, 0, 1, (2 << 6) | tc.op), mfc1(5, 2),
        });
        for (int i = 0; i < (traps ? 8 : 9); ++i) cpu.stepCached(bus);
        assert((cpu.fcr31() & 0x3F000) == tc.cause && (cpu.fcr31() & Condition));
        if (traps) {
            assert(cpu.exceptionTrace().counts[15] == 1 && cpu.pc() == Kseg0 + 0x180);
            assert(static_cast<u32>(cpu.fpr()[2]) == 0x1234 && !(cpu.fcr31() & 4));
        } else {
            assert(cpu.gpr()[5] == static_cast<u64>(static_cast<s64>(tc.expected)));
            assert((cpu.fcr31() & 4) && !cpu.exceptionTrace().observed);
        }
    }
}

void test_cpu_cop1_compare() {
    enum Relation { Unordered, Less, Equal, Greater };
    // Bit i of each mask is the condition result for that relation.
    constexpr bool Expected[16][4] = {
        {false, false, false, false}, // 0x30 C.F
        {true,  false, false, false}, // 0x31 C.UN
        {false, false, true,  false}, // 0x32 C.EQ
        {true,  false, true,  false}, // 0x33 C.UEQ
        {false, true,  false, false}, // 0x34 C.OLT
        {true,  true,  false, false}, // 0x35 C.ULT
        {false, true,  true,  false}, // 0x36 C.OLE
        {true,  true,  true,  false}, // 0x37 C.ULE
        {false, false, false, false}, // 0x38 C.SF
        {true,  true,  false, false}, // 0x39 C.NGLE
        {false, false, true,  false}, // 0x3A C.SEQ
        {true,  false, true,  false}, // 0x3B C.NGL
        {false, true,  false, false}, // 0x3C C.LT
        {true,  true,  false, false}, // 0x3D C.NGE
        {false, true,  true,  false}, // 0x3E C.LE
        {true,  true,  true,  false}, // 0x3F C.NGT
    };
    struct Case { u64 a, b; Relation relation; };
    constexpr Case cases[] = {
        {0x7F800001ULL, 0x3F800000ULL, Unordered}, // signaling NaN (.S)
        {0xC0000000ULL, 0x3F800000ULL, Less},      // -2.0 < 1.0 (.S)
        {0x3F800000ULL, 0x3F800000ULL, Equal},
        {0x40000000ULL, 0x3F800000ULL, Greater},
        {0x7FF0000000000001ULL, 0x3FF0000000000000ULL, Unordered}, // signaling NaN (.D)
        {0xC000000000000000ULL, 0x3FF0000000000000ULL, Less},
        {0x3FF0000000000000ULL, 0x3FF0000000000000ULL, Equal},
        {0x4000000000000000ULL, 0x3FF0000000000000ULL, Greater},
    };
    constexpr u32 Condition = 1u << 23;
    constexpr u32 Invalid = (1u << 16) | (1u << 6); // cause | flag

    for (const bool dbl : {false, true}) {
        for (const auto& tc : cases) {
            if (dbl != (tc.a > 0xFFFFFFFFULL)) continue;
            for (u32 funct = 0; funct < 16; ++funct) {
                Bus bus;
                Cpu cpu;
                cpu.setPc(Kseg0 + 0x400);
                cpu.setIplHandoffRegisters(1, 1, 0, 0);
                if (dbl) {
                    bus.write64(0x700, tc.a);
                    bus.write64(0x708, tc.b);
                } else {
                    bus.write32(0x700, static_cast<u32>(tc.a));
                    bus.write32(0x704, static_cast<u32>(tc.b));
                }
                writeProgram(bus, 0x400, {
                    lui(1, 0x8000),
                    encodeI(dbl ? 0x35 : 0x31, 1, 1, 0x700),
                    encodeI(dbl ? 0x35 : 0x31, 1, 2, dbl ? 0x708 : 0x704),
                    encodeCop1(dbl ? 0x11 : 0x10, 2, 1, (3u << 6) | 0x30 | funct),
                });
                for (int i = 0; i < 3; ++i) cpu.step(bus);
                cpu.step(bus);

                const bool trap = funct >= 8 && tc.relation == Unordered;
                const bool cond = Expected[funct][tc.relation];
                assert((cpu.fcr31() & Condition) == (cond ? Condition : 0u));
                if (trap) {
                    assert(cpu.exceptionTrace().counts[15] == 1);
                    assert(cpu.pc() == Kseg0 + 0x180);
                    assert((cpu.fcr31() & Invalid) == Invalid);
                } else {
                    assert(!cpu.exceptionTrace().observed);
                    assert(cpu.pc() == Kseg0 + 0x410);
                    assert((cpu.fcr31() & Invalid) == 0);
                }
            }
        }
    }
}

void test_rcp_paired_writes() {
    Bus bus;

    // A pair written with both bits set leaves the state bit untouched in
    // either state. Clear first so each check starts from zero.
    const auto checkPair = [&bus](u32 address, u32 clearBit, u32 setBit, u32 stateBit) {
        bus.write32(address, clearBit);
        assert((bus.read32(address) & stateBit) == 0);
        bus.write32(address, clearBit | setBit);
        assert((bus.read32(address) & stateBit) == 0);
        bus.write32(address, setBit);
        assert((bus.read32(address) & stateBit) == stateBit);
        bus.write32(address, clearBit | setBit);
        assert((bus.read32(address) & stateBit) == stateBit);
    };

    constexpr u32 MiMask = 0x0430000C;
    constexpr u32 SpStatus = 0x04040010;
    constexpr u32 DpStatus = 0x0410000C;
    for (u32 i = 0; i < 6; ++i)
        checkPair(MiMask, 1u << (2 * i), 2u << (2 * i), 1u << i);

    checkPair(SpStatus, 0x01, 0x02, 0x01); // halt
    checkPair(SpStatus, 0x20, 0x40, 0x20); // single step
    checkPair(SpStatus, 0x80, 0x100, 0x40); // interrupt on break
    for (u32 i = 0; i < 8; ++i)
        checkPair(SpStatus, 0x200u << (2 * i), 0x400u << (2 * i), 0x80u << i); // signals

    checkPair(DpStatus, 0x01, 0x02, 0x01); // XBUS
    checkPair(DpStatus, 0x04, 0x08, 0x02); // FREEZE
    checkPair(DpStatus, 0x10, 0x20, 0x04); // FLUSH

    // SP manual interrupt uses MI pending, not an SP_STATUS state bit.
    bus.hw().mi().clearPending(1);
    bus.write32(SpStatus, 0x08 | 0x10);
    assert((bus.read32(0x04300008) & 1) == 0);
    bus.write32(SpStatus, 0x10);
    assert((bus.read32(0x04300008) & 1) == 1);
    bus.write32(SpStatus, 0x08 | 0x10);
    assert((bus.read32(0x04300008) & 1) == 1);
}

void test_cpu_branch_likely() {
    Bus bus;
    Cpu cpu;
    cpu.setPc(Kseg0 + 0x400);
    writeProgram(bus, 0x400, {
        0x24010001, // addiu r1,r0,1
        0x54200002, // bnel r1,r0,0x410
        0x24020002, // executed delay slot
        0x24020003, // skipped
        0x50200002, // beql r1,r0,0x41c
        0x24020004, // annulled delay slot
        0x24030005,
    });
    for (int i = 0; i < 5; ++i) cpu.step(bus);
    assert(cpu.gpr()[2] == 2 && cpu.gpr()[3] == 5);
}

void test_ipl2_checksum_table() {
    struct Entry {
        u64 checksum;
        CicVariant variant;
        u8 seed;
        u32 ntscId;
        u32 palId;
        u32 entryBias;
        bool challenge;
        bool fixedEntry;
    };
    constexpr Entry entries[] = {
        {0x45CC73EE317AULL, CicVariant::Cic6101, 0x3F, 6101, 6101, 0, false, false},
        {0xA536C0F1D859ULL, CicVariant::Cic6102, 0x3F, 6102, 7101, 0, false, false},
        {0x44160EC5D9AFULL, CicVariant::Cic7102, 0x3F, 7102, 7102, 0, false, true},
        {0x586FD4709867ULL, CicVariant::Cic6103, 0x78, 6103, 7103, 0x100000, false, false},
        {0x8618A45BC2D3ULL, CicVariant::Cic6105, 0x91, 6105, 7105, 0, true, false},
        {0x2BBAD4E6EB74ULL, CicVariant::Cic6106, 0x85, 6106, 7106, 0x200000, false, false},
        {0x93E983A8F152ULL, CicVariant::Cic5101, 0xAC, 5101, 5101, 0x100000, false, false},
    };
    for (const auto& entry : entries) {
        const auto profile = cicProfileForChecksum(entry.checksum);
        assert(profile.variant == entry.variant);
        assert(profile.seed == entry.seed);
        assert(profile.ntscId == entry.ntscId && profile.palId == entry.palId);
        assert(profile.entryBias == entry.entryBias);
        assert(profile.challenge == entry.challenge);
        assert(profile.fixedEntry == entry.fixedEntry);
    }
    const std::vector<u8> emptyBoot(0x2000);
    assert(detectCic(emptyBoot).variant == CicVariant::Unknown);
}

void test_ipl_handoff_profiles() {
    struct Case {
        CicProfile profile;
        u8 country;
        u32 headerEntry;
        u32 expectedEntry;
        u32 expectedId;
        u64 expectedS6;
        u32 expectedTv;
        u32 expectedVersion;
    };
    constexpr Case cases[] = {
        {{CicVariant::Cic6101, 0x3F, 6101, 6101}, 'E', 0x80000400, 0x80000400, 6101, 0x3F, 1, 1},
        {{CicVariant::Cic6102, 0x3F, 6102, 7101}, 'E', 0x80000400, 0x80000400, 6102, 0x3F, 1, 1},
        {{CicVariant::Cic6102, 0x3F, 6102, 7101}, 'P', 0x80000400, 0x80000400, 7101, 0x3F, 0, 7},
        {{CicVariant::Cic7102, 0x3F, 7102, 7102, 0, false, true, 0x80000480},
         'P', 0x80000400, 0x80000480, 7102, 0x3F, 0, 7},
        {{CicVariant::Cic6103, 0x78, 6103, 7103, 0x100000},
         'E', 0x80125C00, 0x80025C00, 6103, 0x78, 1, 1},
        {{CicVariant::Cic6105, 0x91, 6105, 7105, 0, true},
         'E', 0x80000400, 0x80000400, 6105, 0xFFFFFFFFA0000200ULL, 1, 1},
        {{CicVariant::Cic6106, 0x85, 6106, 7106, 0x200000},
         'E', 0x80200400, 0x80000400, 6106, 0x85, 1, 1},
        {{CicVariant::Cic5101, 0xAC, 5101, 5101, 0x100000},
         'E', 0x80100000, 0x80000000, 5101, 0xAC, 1, 1},
    };
    for (const auto& test : cases) {
        std::vector<u8> rom(0x2000);
        const auto word = [&](u32 offset, u32 value) {
            for (u32 i = 0; i < 4; ++i) rom[offset + i] = static_cast<u8>(value >> (24 - 8 * i));
        };
        word(0, 0x80371240);
        word(8, test.headerEntry);
        rom[0x3E] = test.country;
        word(0x1000, 0x24020001); // addiu r2,r0,1
        Cartridge cart;
        assert(cart.loadFromBuffer(std::move(rom)));
        Emulator emu;
        assert(emu.loadCartridge(std::move(cart)));
        EmulatorTestAccess::applyBootProfile(emu, test.profile);

        const auto& gpr = emu.cpu().gpr();
        assert(gpr[19] == 0 && gpr[20] == test.expectedTv && gpr[21] == 0);
        assert(gpr[22] == test.expectedS6);
        assert(gpr[23] == test.expectedVersion && gpr[29] == 0xFFFFFFFFA4001FF0ULL);
        assert(gpr[31] == (test.expectedTv == 1 ? 0xFFFFFFFFA4001550ULL : 0xFFFFFFFFA4001554ULL));
        assert(emu.bus().read32(0x310) == test.expectedId);
        assert(emu.bus().read32(0x318) == 0x00800000); // osMemSize
        assert(emu.bus().rdram().size() == 0x00800000 && emu.bus().read32(0x0470000C) == 0x14);
        emu.bus().write32(0x003FFFFC, 0x11223344);
        emu.bus().write32(0x007FFFFC, 0x55667788);
        assert(emu.bus().read32(0x003FFFFC) == 0x11223344);
        assert(emu.bus().read32(0x007FFFFC) == 0x55667788); // Expansion Pak is not a 4 MiB mirror.
        assert(emu.bus().hw().si().cic6105 == test.profile.challenge);
        assert(emu.bus().read32(0x1FC007FC) == 0);
        assert(emu.cpu().pc() == static_cast<u64>(static_cast<s64>(static_cast<s32>(test.expectedEntry))));
        emu.runOneStep();
        assert(gpr[2] == 1);
        emu.reset();
        assert(gpr[2] == 0);
        assert(emu.cpu().pc() == static_cast<u64>(static_cast<s64>(static_cast<s32>(test.headerEntry))));
    }
}

void test_cpu_cop1_memory() {
    Bus bus;
    Cpu cpu;
    cpu.setPc(Kseg0 + 0x400);
    bus.write64(0x200, 0x89ABCDEF01234567ULL);
    bus.write32(0x208, 0xFEDCBA98);
    writeProgram(bus, 0x400, {
        lui(1, 0x2000), mtc0(1, 12), // CU1 enabled, FR=0
        lui(2, 0x8000),
        encodeI(0x35, 2, 0, 0x200), // ldc1 f0
        encodeI(0x31, 2, 1, 0x208), // lwc1 f1 (upper word of f0 in FR=0)
        encodeI(0x39, 2, 1, 0x210), // swc1 f1
        encodeI(0x3D, 2, 0, 0x218), // sdc1 f0
    });
    for (int i = 0; i < 7; ++i) cpu.step(bus);
    assert(bus.read32(0x210) == 0xFEDCBA98);
    assert(bus.read64(0x218) == 0xFEDCBA9801234567ULL);
}

void test_cpu_count_compare() {
    Bus bus;
    Cpu cpu;
    cpu.setPc(Kseg0 + 0x400);
    writeProgram(bus, 0x400, {
        0x34010003, // compare = 3
        0x40815800,
        0x34028001, // enable timer interrupts
        0x40826000,
        0, 0,
    });
    bus.write32(0x180, 0x40805800); // mtc0 r0,Compare: acknowledge IP7
    for (int i = 0; i < 5; ++i) cpu.step(bus);
    assert(cpu.cop0()[9] == 2 && !(cpu.cop0()[13] & 0x8000));
    cpu.step(bus);
    assert(cpu.pc() == 0xFFFFFFFF80000180ULL && cpu.cop0()[9] == 3);
    assert(cpu.cop0()[14] == Kseg0 + 0x414 && (cpu.cop0()[13] & 0x8000));
    cpu.step(bus);
    assert(!(cpu.cop0()[13] & 0x8000));
}


[[nodiscard]] u32 beql(u32 rs, u32 rt, s16 off) { return encodeI(0x14, rs, rt, off); }
[[nodiscard]] u32 jr(u32 rs) { return encodeR(0x08, rs, 0, 0); }

void test_cpu_block_likely_annul() {
    Bus bus;
    Cpu cpu;
    cpu.setPc(0xFFFFFFFF80000100ULL);
    writeProgram(bus, 0x100, {
        addiu(1, 0, 1),          // r1 = 1
        beql(1, 0, 1),           // not taken -> annul delay slot
        addiu(2, 0, 99),         // annulled
        addiu(3, 0, 42),         // executed after annul
        jr(31),
        addiu(0, 0, 0),          // delay slot
    });
    cpu.attachToBus(bus);
    const u32 n = cpu.runBlock(bus);
    assert(n == 2);
    assert(cpu.gpr()[1] == 1);
    assert(cpu.gpr()[2] == 0);   // delay slot was annulled
    assert(cpu.pc() == 0xFFFFFFFF8000010CULL);
    (void)cpu.runBlock(bus);     // execute the instruction after the annulled slot
    assert(cpu.gpr()[3] == 42);
}

void test_cpu_block_delay_slot_resume() {
    Bus bus;
    Cpu cpu;
    cpu.setPc(0xFFFFFFFF80000100ULL);
    writeProgram(bus, 0x100, {
        beq(0, 0, 2),            // taken to 0x10c
        addiu(1, 0, 1),          // delay slot
        addiu(2, 0, 2),          // not reached
        addiu(3, 0, 3),
    });
    cpu.attachToBus(bus);
    const u32 n1 = cpu.runBlock(bus, 1); // execute only the branch
    assert(n1 == 1);
    assert(cpu.pc() == 0xFFFFFFFF80000104ULL);
    cpu.step(bus);                       // execute the delay slot (uncached)
    assert(cpu.gpr()[1] == 1);
    assert(cpu.gpr()[2] == 0);
    (void)cpu.runBlock(bus);             // execute the fall-through instruction
    assert(cpu.gpr()[3] == 3);
}

void test_cpu_block_self_modifying_invalidation() {
    Bus bus;
    Cpu cpu;
    cpu.setPc(0xFFFFFFFF80000100ULL);
    writeProgram(bus, 0x100, {
        addiu(4, 0, 1),
        jr(31),
    });
    cpu.attachToBus(bus);
    (void)cpu.runBlock(bus);
    assert(cpu.gpr()[4] == 1);
    cpu.setPc(0xFFFFFFFF80000100ULL);
    assert(cpu.runBlock(bus) == 3);
    assert(cpu.cacheStats().lookups == 6 && cpu.cacheStats().hits == 5);
    cpu.setPc(0xFFFFFFFF80000100ULL);
    bus.write32(0x100, addiu(4, 0, 99));
    (void)cpu.runBlock(bus);
    assert(cpu.gpr()[4] == 99);
    assert(cpu.cacheStats().lookups == 9 && cpu.cacheStats().hits == 7);
}

namespace {
struct DeferredWriteCallbackState {
    std::vector<std::pair<u32, u32>> ranges;
};
void recordDeferredWrite(void* context, u32 address, u32 size) noexcept {
    static_cast<DeferredWriteCallbackState*>(context)->ranges.emplace_back(address, size);
}
} // namespace

void test_memory_deferred_write_notifications() {
    Memory memory;
    DeferredWriteCallbackState state;
    memory.setWriteCallback(&recordDeferredWrite, &state);
    Memory::DeferredWriteSet writes;
    memory.storeRdramDeferred<u32>(0x100, 0x11223344u, writes);
    memory.storeRdramDeferred<u16>(0x120, 0x5566u, writes);
    memory.storeRdramDeferred<u8>(0x13F, 0x77u, writes);
    memory.storeRdramDeferred<u16>(0x140, 0x99AAu, writes); // adjacent line merges
    memory.storeRdramDeferred<u32>(0x300, 0x44556677u, writes);
    memory.storeRdramDeferred<u32>(0x2FF, 0xAB154D8Fu, writes); // spans two lines
    // Data is immediately visible to the guest; only coherency is deferred.
    assert(state.ranges.empty());
    assert(memory.loadRdram<u32>(0x100) == 0x11223344u);
    assert(memory.loadRdram<u16>(0x120) == 0x5566u);
    assert(memory.read8(0x13F) == 0x77);
    assert(memory.loadRdram<u16>(0x140) == 0x99AAu);
    memory.flushDeferred(writes);
    // Only touched 64-byte lines are reported. The gap between the color and
    // depth-like lines must not invalidate all of RDRAM between them.
    std::sort(state.ranges.begin(), state.ranges.end());
    const std::vector<std::pair<u32, u32>> expected{
        {0x100u, 64u}, {0x140u, 64u}, {0x2C0u, 64u}, {0x300u, 64u}};
    assert(state.ranges == expected);
    memory.flushDeferred(writes);
    assert(state.ranges.size() == expected.size());
    memory.storeRdram<u8>(0x300, 0x88u);
    assert(state.ranges.size() == expected.size() + 1);
    assert(state.ranges.back() == std::make_pair(0x300u, 1u));
}

void test_cached_fetch_line_and_sp_dma() {
    Emulator emu;
    auto& bus = emu.bus();
    auto& cpu = emu.cpu();
    constexpr u64 pc = 0xFFFFFFFF8000013CULL;
    writeProgram(bus, 0x13C, {0, addiu(4, 0, 1), beq(0, 0, -3), 0});
    cpu.setPc(pc);
    cpu.stepCached(bus); // predecode instructions on both sides of the cache line
    const auto updated = addiu(4, 0, 9);
    bus.rdram().write64(0x13C, updated); // the last four bytes are on the next line
    cpu.stepCached(bus);
    assert(cpu.gpr()[4] == 9);

    cpu.setPc(pc);
    cpu.stepCached(bus);
    bus.write32(0x04000000, addiu(4, 0, 17));
    bus.write32(0x04000004, beq(0, 0, -3));
    bus.write32(0x04040000, 0);
    bus.write32(0x04040004, 0x140);
    bus.write32(0x0404000C, 7); // SP DMA writes into the decoded block
    cpu.stepCached(bus);
    assert(cpu.gpr()[4] == 17);
}

void test_cpu_quiet_branch_chaining() {
    Bus bus, referenceBus;
    Cpu cpu, reference;
    constexpr u64 Base = 0xFFFFFFFF80000000ULL;
    writeProgram(bus, 0x100, {
        encodeJ(opJal, 0x180 >> 2),
        addiu(1, 1, 1), // JAL delay slot
    });
    writeProgram(bus, 0x180, {
        addiu(2, 2, 1),
        bne(1, 0, (0x100 - 0x188) / 4),
        addiu(3, 3, 1), // BNE delay slot
    });
    for (u32 count : {25u, 19u, 33u}) {
        cpu.setPc(Base + 0x100);
        reference.setPc(Base + 0x100);
        if (count == 19) bus.write32(0x180, addiu(2, 2, 4)); // invalidate cached link target
        if (count == 33) {
            Rsp rsp;
            bus.attachRsp(&rsp);
            rsp.writeMem32(0, addiu(2, 2, 7));
            rsp.writeMem32(4, bne(1, 0, (0x100 - 0x188) / 4));
            bus.write32(0x04040000, 0);
            bus.write32(0x04040004, 0x180);
            bus.write32(0x0404000C, 7);
            bus.attachRsp(nullptr);
        }
        const auto links = cpu.cacheStats().branchLinksTaken;
        assert(cpu.runQuietBlock(bus, count) == count);
        assert(cpu.cacheStats().branchLinksTaken > links);
        for (u32 a : {0x100u, 0x104u, 0x180u, 0x184u, 0x188u})
            referenceBus.write32(a, bus.read32(a));
        for (u32 i = 0; i < count; ++i) reference.step(referenceBus);
        assert(cpu.pc() == reference.pc() && cpu.nextPc() == reference.nextPc());
        assert(cpu.gpr() == reference.gpr() && cpu.cop0() == reference.cop0());
    }
}

#ifdef N64FORGE_TEST_STUDIO_VIDEO
int run_studio_video_tests(int argc, char** argv);
#endif

int compare_event_scheduler(const char* path, int fields) {
    Emulator reference, batched;
    if (!reference.loadCartridge(path) || !batched.loadCartridge(path)) return 2;
    reference.setEventBatchingEnabled(false);
    using Clock = std::chrono::steady_clock;
    Clock::duration refTime{}, batchTime{};
    u64 hash = 14695981039346656037ULL;
    u32 framesWithPixels = 0;
    for (int field = 0; field < fields; ++field) {
        auto start = Clock::now();
        const auto referenceCycles = reference.runVideoFrame();
        refTime += Clock::now() - start;
        start = Clock::now();
        const auto batchCycles = batched.runVideoFrame();
        batchTime += Clock::now() - start;
        const auto& a = reference.cpu();
        const auto& b = batched.cpu();
        assert(referenceCycles == batchCycles);
        assert(a.pc() == b.pc() && a.gpr() == b.gpr() && a.cop0() == b.cop0() && a.fpr() == b.fpr());
        assert(reference.framebufferWidth() == batched.framebufferWidth());
        assert(reference.framebufferHeight() == batched.framebufferHeight());
        assert(reference.framebuffer() == batched.framebuffer());
        size_t aAudio = 0, bAudio = 0;
        const auto* aSamples = reference.audioSamples(aAudio);
        const auto* bSamples = batched.audioSamples(bAudio);
        assert(aAudio == bAudio && (!aAudio || std::equal(aSamples, aSamples + aAudio * 2, bSamples)));
        bool nonBlack = false;
        for (const auto rgba : batched.framebuffer()) {
            hash = (hash ^ rgba) * 1099511628211ULL;
            nonBlack |= (rgba & 0xFFFFFF) != 0;
        }
        framesWithPixels += nonBlack;
    }
    const auto a = reference.counters(), b = batched.counters();
    assert(a.cycles == b.cycles && a.instructions == b.instructions && a.viFields == b.viFields);
    assert(a.gfx == b.gfx && a.audio == b.audio && a.other == b.other && a.triangles == b.triangles);
    assert(a.deviceTicks == a.cycles && b.deviceTicks < b.cycles);
    if (!framesWithPixels) return 3;
    std::cout << "Identical CPU state, audio and VI pixels; " << b.viFields << " fields ("
              << framesWithPixels << " non-black), RGBA hash " << std::hex << hash << std::dec
              << "\nrunVideoFrame: reference " << std::chrono::duration<double>(refTime).count()
              << "s, batched " << std::chrono::duration<double>(batchTime).count()
              << "s; device ticks " << a.deviceTicks << " -> " << b.deviceTicks << '\n';
    return 0;
}

int main(int argc, char** argv) {
    if (argc > 2 && std::strcmp(argv[1], "--compare-event-batching") == 0)
        return compare_event_scheduler(argv[2], argc > 3 ? std::max(1, std::atoi(argv[3])) : 120);
#ifdef N64FORGE_TEST_STUDIO_VIDEO
    if (argc > 1 && (std::strcmp(argv[1], "--studio-video") == 0 ||
                     std::strcmp(argv[1], "--studio-native") == 0 ||
                     std::strcmp(argv[1], "--studio-bench") == 0 ||
                     std::strcmp(argv[1], "--studio-art") == 0))
        return run_studio_video_tests(argc, argv);
#endif
    test_memory();
    test_cartridge_conversion();
    test_cartridge_files_and_zip();
    test_ipl2_checksum_table();
    test_ipl_handoff_profiles();
    test_bus_mapping();
    test_is_viewer();
    test_cpu_alu();
    test_cpu_load_store();
    test_cpu_shift_divide_edges();
    test_cpu_rdram_aliases();
    test_cpu_big_endian_merge_accesses();
    test_cpu_branch_delay_slot();
    test_cpu_branch_likely();
    test_cpu_regimm_likely();
    test_cpu_trunc_word();
    test_cpu_cop1_word_rounding();
    test_cpu_cop1_compare();
    test_cpu_jump_and_link();
    test_cpu_64_cop_tlb();
    test_cpu_cop1_memory();
    test_cpu_count_compare();
    test_cpu_tlb_exception_routing();
    test_vr4300_cop2_transfer_latch();
    test_vr4300_unaligned_access();
    test_cpu_quiet_self_branch();
    test_cpu_kseg2_overlay_remap();
    test_cpu_native_alu_and_smc();
    test_cpu_native_memory_fastpath();
    test_cpu_native_delay_slot_fallback();
    test_cpu_native_store_to_current_code();
    test_cpu_native_cop1_prefix();
    test_cpu_native_chain_loop_and_budget();
    test_cpu_native_chain_invalidation();
    runGraphicsApiTests();
    test_cpu_native_control_flow_and_calls();
    test_cpu_native_cop1_compare_predicates();
    test_cpu_native_bc1_delay_slot();
    test_cpu_native_cop1_cu1_disabled();
    test_cpu_kuseg_mapped_cache();
    test_cpu_load_linked_store_conditional();
    test_rsp_task_dp_submission();
    test_vi_scanout();
    test_ai_dma();
    test_audio_resample_to_host_fifo();
    test_rdp_copy_texture_and_split_dma();
    test_rsp_scalar();
    test_rsp_imem_decode_invalidation();
    test_rsp_vector_registers();
    test_rsp_vector_elements();
    test_rsp_cop2();
    test_host_audio_ring();
    test_audio_ring_producer_frames_during_render();
    test_audio_ring_signed_fade_and_tail();
    test_audio_ring_overrun_accounting();
    test_core_stats_sampler();
    test_vi_motion_interpolation();
    test_rsp_transpose();
    test_hw_registers();
    test_rcp_paired_writes();
    test_pi_ri_register_conformance();
    test_vi_scanline_interrupt();
    test_vi_timing_register_update();
    test_pif_ram_si_interrupt();
    test_controller_pak_validation_edges();
    test_pif_joybus_and_cic();
    test_pi_dma();
    test_flash_ram_pi();
    test_eeprom_joybus();
    test_auto_save_detection();
    test_sram_pi();
    test_video_frame_interleaves_cpu_and_dma();
    test_event_deadline_interrupts();
    test_frame_slot_handoff();
    test_scanout_dirty_tracking();
    test_packed_scanout_active_window();
    test_hle_libultra();
    test_cpu_block_likely_annul();
    test_cpu_block_delay_slot_resume();
    test_cpu_block_self_modifying_invalidation();
    test_memory_deferred_write_notifications();
    test_cached_fetch_line_and_sp_dma();
    test_cpu_quiet_branch_chaining();
    test_rdp_renderer();
    test_rdp_internal_resolution_scale();
    for (u32 scale : {1u, 2u, 4u}) test_rdp_textured_shaded_depth_and_tlut(scale);
    test_rdp_tmem_texel_formats();
    test_rdp_ci8_texrect_copy_and_one_cycle();
    test_rdp_ci8_tlut_loaded_block_triangle();
    test_rdp_perspective_strided_load_block();
    test_rdp_adjacent_triangles_no_seam();
    test_rdp_raw_textured_triangle_loadtile_two_cycle();
    test_rdp_raw_loadtile_subrectangle();
    test_rdp_shared_edge_alpha_cvg_no_black_holes();
    test_rdp_color_on_cvg_uses_stage0_m_input();
    test_rdp_cycle_blender_stage_selection();
    test_rdp_textured_gouraud_three_point_gradient();
    test_rdp_internal_scale_native_writeback();
    test_rdp_internal_scale_double_buffer_presentation();
    test_rdp_settile_shift_all_textured_triangles();
    test_rdp_rgba32_wide_rectangle();
    test_rdp_alpha_compare_uses_geometric_coverage();
    test_rdp_threaded_output_matches_serial();
    test_ostask_parsing();
    test_rsp_hle_microcode_detection();
    test_rsp_hle_invalid_task();
    test_rsp_hle_display_list_end();
    test_rsp_hle_matrix_transform();
    test_rsp_hle_setothermode_encodings();
    test_rsp_hle_f3dex2_textured_triangle_pipeline();
    test_rsp_break_and_sp_cop0();
    test_rsp_cop0_dma();
#ifdef N64FORGE_TEST_STUDIO_VIDEO
    run_frame_ring_tests();
#endif
    run_core_safety_tests();
    run_rdp_safety_tests();
    run_discord_tests();
    return run_recomp_tests() == 0 ? 0 : 1;
}
