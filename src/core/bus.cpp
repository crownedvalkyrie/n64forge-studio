#include "n64forge/core/bus.hpp"
#include "n64forge/core/cpu.hpp"
#include "n64forge/core/rsp.hpp"
#include <cstddef>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace n64forge {

namespace {

constexpr u32 PifBase = 0x1FC00000;
constexpr u32 PifEnd = 0x1FD00000;
constexpr u32 PifRamOffset = 0x7C0;
constexpr u32 RdramRegisters = 0x03F00000;
constexpr u32 RdramBroadcast = 0x03F80000;
constexpr u32 RdramRegistersEnd = 0x04000000;
constexpr u32 RdramDelay = 8;

[[nodiscard]] std::string hexAddr(u32 a) {
    std::ostringstream oss;
    oss << "0x" << std::hex << std::uppercase << std::setw(8) << std::setfill('0') << a;
    return oss.str();
}

bool flashCommand(u32 value) noexcept {
    switch (value & 0xFF000000) {
        case 0x3C000000: case 0x78000000: case 0xB4000000:
        case 0xD2000000: case 0xE1000000: case 0xF0000000:
            return (value & 0xFFFFFF) == 0;
        case 0x4B000000: case 0xA5000000:
            return (value & 0xFFFC00) == 0;
        default: return false;
    }
}
} // namespace

Bus::Bus() { reset(); }

void Bus::reset() {
    rdram_ = Memory{};
    rdramDelay_.fill(0);
    autoPiSave_ = false;
    autoFlashHigh_ = 0;
    piSaveMedia_ = PiSaveMedia::None;
    flash_.reset();
    isViewer_.reset();
    eeprom_.reset();
    sram_.reset();
    hw_.reset();
    if (cpu_ != nullptr) {
        cpu_->clearBlockCache();
        // Bus::reset() replaces the Memory object, which clears its write
        // callback. Re-register so self-modifying-code invalidation survives.
        cpu_->attachToBus(*this);
    }
}

void Bus::mountCartridge(const Cartridge* cartridge) {
    cartridge_ = cartridge;
}

void Bus::configureAutoPiSave(bool hasFlashImage) noexcept {
    flash_.attach(true);
    sram_.attach(true);
    autoPiSave_ = true;
    autoFlashHigh_ = 0;
    piSaveMedia_ = hasFlashImage ? PiSaveMedia::FlashRam : PiSaveMedia::None;
}

void Bus::observePiSaveAccess(u32 address) const noexcept {
    if (autoPiSave_ && piSaveMedia_ == PiSaveMedia::None &&
        address >= Sram::Base && address < Sram::Base + Sram::Size)
        piSaveMedia_ = PiSaveMedia::Sram;
}

void Bus::invalidateCodeCache(u32 physicalAddress, u32 size) const noexcept {
    if (cpu_ != nullptr && size != 0) {
        cpu_->invalidateBlockCache(physicalAddress, size);
    }
}

u8 Bus::read8(u32 a) const {
    if (a < Memory::RdramSize) {
        return rdram_.read8(a);
    }
    if (a >= FlashRam::Base && a < 0x10000000) {
        observePiSaveAccess(a);
        if (piSaveMedia_ == PiSaveMedia::Sram && sram_.attached())
            return sram_.read(a - Sram::Base);
        if (piSaveMedia_ == PiSaveMedia::FlashRam && flash_.attached())
            return flash_.read(a - FlashRam::Base);
    }
    if (a >= 0x04000000 && a < 0x04002000) {
        if (rsp_ == nullptr) return 0;
        return rsp_->readMem8(a - 0x04000000);
    }
    if (a >= 0x04300000 && a < 0x05000000) {
        // HW registers are 32-bit; byte reads return the addressed byte of the word.
        const u32 word = hw_.read(a & ~3u);
        return static_cast<u8>(word >> (8 * (3 - (a & 3u))));
    }
    // Unpopulated 64DD PI domains return open bus, not an ASIC register value.
    if (a >= 0x05000000 && a < 0x08000000) return static_cast<u8>((a & ~3u) >> ((a & 1) ? 0 : 8));
    if (a >= IsViewer::Base && a < IsViewer::End)
        return isViewer_.read8(a - IsViewer::Base);
    if (a >= Cartridge::BusBase && a < Cartridge::BusEnd) {
        const u32 offset = a - Cartridge::BusBase;
        if (cartridge_ && offset < cartridge_->size()) {
            return cartridge_->rom()[offset];
        }
        // A direct PI word read repeats the aligned address's low halfword.
        return static_cast<u8>((a & ~3u) >> ((a & 1) ? 0 : 8));
    }
    if (a >= PifBase && a < PifEnd) {
        const u32 offset = a & 0x7FF;
        // Post-IPL handoff: PIF ROM is locked out.
        return offset < PifRamOffset ? 0 : hw_.si().pifRam[offset - PifRamOffset];
    }
    throw std::out_of_range("Bus read8 out of range: " + hexAddr(a));
}

u16 Bus::read16(u32 a) const {
    return static_cast<u16>((static_cast<u16>(read8(a)) << 8) | read8(a + 1));
}

u32 Bus::read32(u32 a) const {
    if ((a & 3u) == 0) {
        if (a >= RdramRegisters && a < RdramBroadcast && (a & 0x3FF) == RdramDelay) {
            const u32 chip = (a - RdramRegisters) >> 11;
            return chip < rdramDelay_.size() ? rdramDelay_[chip] : 0;
        }
        if (a >= 0x05000000 && a < 0x08000000) return (a << 16) | (a & 0xFFFF);
        if (a == 0x04080000) return rsp_ ? rsp_->pc() & 0xFFC : 0;
        if (a >= 0x04040000 && a < 0x04040020) {
            return hw_.read(a);
        }
        if (a >= 0x04100000 && a < 0x04100020) return hw_.read(a);
        if (a >= 0x04300000 && a < 0x05000000) {
            return hw_.read(a);
        }
    }
    return (static_cast<u32>(read8(a)) << 24) |
           (static_cast<u32>(read8(a + 1)) << 16) |
           (static_cast<u32>(read8(a + 2)) << 8) |
           static_cast<u32>(read8(a + 3));
}

u64 Bus::read64(u32 a) const {
    return (static_cast<u64>(read32(a)) << 32) | read32(a + 4);
}

void Bus::write8(u32 a, u8 v) {
    if (a < Memory::RdramSize) {
        rdramWrite8(a, v);
        return;
    }
    if (a >= FlashRam::Base && a < 0x10000000) {
        observePiSaveAccess(a);
        if (piSaveMedia_ == PiSaveMedia::Sram && sram_.attached()) {
            sram_.write(a - Sram::Base, v);
            return;
        }
        if (piSaveMedia_ == PiSaveMedia::FlashRam && flash_.attached()) {
            flash_.write(a - FlashRam::Base, v);
            return;
        }
    }
    if (a >= 0x04000000 && a < 0x04002000) {
        if (rsp_ != nullptr) {
            rsp_->writeMem8(a - 0x04000000, v);
        }
        return;
    }
    if (a >= 0x04300000 && a < 0x05000000) {
        // Unsupported byte write to HW register; ignored.
        return;
    }
    if (a >= IsViewer::Base && a < IsViewer::End) {
        isViewer_.write8(a - IsViewer::Base, v);
        return;
    }
    if (a >= Cartridge::BusBase && a < Cartridge::BusEnd) {
        // Neither ROM nor an unmapped cartridge address accepts writes.
        return;
    }
    if (a >= PifBase && a < PifEnd) {
        const u32 offset = a & 0x7FF;
        if (offset >= PifRamOffset) hw_.si().pifRam[offset - PifRamOffset] = v;
        return;
    }
    throw std::out_of_range("Bus write8 out of range: " + hexAddr(a));
}

void Bus::write16(u32 a, u16 v) {
    // Compare in size_t: `a + 1` in u32 wraps at 0xFFFFFFFF.
    if (static_cast<std::size_t>(a) <= static_cast<std::size_t>(Memory::RdramSize) - sizeof(u16)) {
        rdramWrite16(a, v);
        return;
    }
    if (autoPiSave_ && piSaveMedia_ != PiSaveMedia::FlashRam) {
        if (a == FlashRam::Command && flashCommand(u32{v} << 16)) {
            autoFlashHigh_ = v;
            return;
        }
        if (a == FlashRam::Command + 2 && autoFlashHigh_) {
            const u32 cmd = (u32{autoFlashHigh_} << 16) | v;
            autoFlashHigh_ = 0;
            if (flashCommand(cmd)) {
                piSaveMedia_ = PiSaveMedia::FlashRam;
                flash_.command(cmd);
                return;
            }
        }
    }
    if (a >= FlashRam::Command && a < FlashRam::Command + 4 &&
        piSaveMedia_ == PiSaveMedia::FlashRam && flash_.attached()) {
        flash_.commandHalf(a - FlashRam::Command, v);
        return;
    }
    if (a >= IsViewer::Base && a < IsViewer::End) {
        isViewer_.write16(a - IsViewer::Base, v);
        return;
    }
    write8(a, static_cast<u8>(v >> 8));
    write8(a + 1, static_cast<u8>(v));
}

void Bus::write32(u32 a, u32 v) {
    if (autoPiSave_ && a == FlashRam::Command && flashCommand(v)) {
        piSaveMedia_ = PiSaveMedia::FlashRam;
        autoFlashHigh_ = 0;
    }
    if (a == FlashRam::Command && piSaveMedia_ == PiSaveMedia::FlashRam && flash_.attached()) {
        flash_.command(v);
        return;
    }
    if (a >= IsViewer::Base && a < IsViewer::End) {
        isViewer_.write32(a - IsViewer::Base, v);
        return;
    }
    if ((a & 3u) == 0) {
        if (a < Memory::RdramSize) {
            rdramWrite32(a, v);
            return;
        }
        if (a >= RdramRegisters && a < RdramRegistersEnd && (a & 0x3FF) == RdramDelay) {
            // RI register-space writes use device IDs in bits 18:10;
            // broadcast ignores the ID. Delay timing itself is not modeled.
            if (a >= RdramBroadcast) rdramDelay_.fill(v);
            else if (const u32 chip = (a - RdramRegisters) >> 11; chip < rdramDelay_.size())
                rdramDelay_[chip] = v;
            return;
        }
        if (a >= 0x05000000 && a < 0x08000000) return;
        if (a >= PifBase && a < PifEnd) {
            auto& si = hw_.si();
            const bool idle = si.transfer == SiRegisters::Transfer::None;
            si.begin(SiRegisters::Transfer::DirectWrite);
            if (!idle) return;
            for (u32 i = 0; i < 4; ++i) write8(a + i, static_cast<u8>(v >> (24 - 8 * i)));
            return;
        }
        if (a == 0x04080000) {
            if (rsp_) rsp_->setPc(v & 0xFFC);
            return;
        }
        if (a >= 0x04040000 && a < 0x04040020) {
            if ((a == 0x04040008 || a == 0x0404000C) && rsp_) {
                auto& sp = hw_.sp();
                const u32 length = (v & 0xFF8) + 8;
                const u32 count = ((v >> 12) & 0xFF) + 1;
                const u32 skip = (v >> 20) & 0xFF8;
                for (u32 row = 0; row < count; ++row) {
                    for (u32 i = 0; i < length; ++i) {
                        const u32 spAddr = (sp.memAddr & 0x1000) | ((sp.memAddr + i) & 0xFFF);
                        const u32 dram = (sp.dramAddr + i) & 0xFFFFFF;
                        if (a == 0x04040008) {
                            rsp_->writeMem8(spAddr, dram < Memory::RdramSize ? rdram_.read8(dram) : 0);
                        } else if (dram < Memory::RdramSize) {
                            rdram_.write8(dram, rsp_->readMem8(spAddr));
                        }
                    }
                    sp.memAddr = (sp.memAddr & 0x1000) | ((sp.memAddr + length) & 0xFFF);
                    sp.dramAddr = (sp.dramAddr + length + skip) & 0xFFFFFF;
                }
                sp.dmaLength = (v & 0xFFF00000) | 0xFF8;
                return;
            }
            const bool wasHalted = hw_.sp().status & 1;
            hw_.write(a, v);
            if (a == 0x04040010 && rsp_ && wasHalted && !(hw_.sp().status & 1)) rsp_->observeLaunch();
            return;
        }
        if (a >= 0x04300000 && a < 0x05000000) {
            hw_.write(a, v);
            return;
        }
        if (a >= 0x04100000 && a < 0x04100020) {
            hw_.write(a, v);
            return;
        }
    }
    write8(a, static_cast<u8>(v >> 24));
    write8(a + 1, static_cast<u8>(v >> 16));
    write8(a + 2, static_cast<u8>(v >> 8));
    write8(a + 3, static_cast<u8>(v));
}

void Bus::write64(u32 a, u64 v) {
    if (static_cast<std::size_t>(a) <= static_cast<std::size_t>(Memory::RdramSize) - sizeof(u64)) {
        rdramWrite64(a, v);
        return;
    }
    write32(a, static_cast<u32>(v >> 32));
    write32(a + 4, static_cast<u32>(v));
}

// Every write below flows through Memory, whose write observer the CPU uses to
// invalidate its block cache. Do not invalidate again here: duplicate generation
// bumps only add work and make stale-block detection noisier.
void Bus::rdramWrite8(u32 offset, u8 value) {
    rdram_.write8(offset, value);
}
void Bus::rdramWrite16(u32 offset, u16 value) {
    rdram_.write16(offset, value);
}
void Bus::rdramWrite32(u32 offset, u32 value) {
    rdram_.write32(offset, value);
}
void Bus::rdramWrite64(u32 offset, u64 value) {
    rdram_.write64(offset, value);
}

template<class T> void Bus::rdramWrite(u32 offset, T value) {
    if constexpr (sizeof(T) == 1) rdramWrite8(offset, static_cast<u8>(value));
    else if constexpr (sizeof(T) == 2) rdramWrite16(offset, static_cast<u16>(value));
    else if constexpr (sizeof(T) == 4) rdramWrite32(offset, static_cast<u32>(value));
    else if constexpr (sizeof(T) == 8) rdramWrite64(offset, static_cast<u64>(value));
}

} // namespace n64forge
