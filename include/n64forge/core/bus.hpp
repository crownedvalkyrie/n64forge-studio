#pragma once
#include "n64forge/core/cartridge.hpp"
#include "n64forge/core/eeprom.hpp"
#include "n64forge/core/flash_ram.hpp"
#include "n64forge/core/hw_registers.hpp"
#include "n64forge/core/is_viewer.hpp"
#include "n64forge/core/sram.hpp"
#include "n64forge/core/memory.hpp"
#include "n64forge/core/types.hpp"
#include <array>
#include <cstddef>
#include <utility>

namespace n64forge {

class Rsp;

class Cpu;

class Bus {
public:
    static constexpr u32 PifRamSize = 0x40;

    Bus();
    void reset();

    [[nodiscard]] Memory& rdram() noexcept { return rdram_; }
    [[nodiscard]] const Memory& rdram() const noexcept { return rdram_; }
    [[nodiscard]] const Cartridge* cartridge() const noexcept { return cartridge_; }
    void mountCartridge(const Cartridge* cartridge);
    void attachRsp(Rsp* rsp) noexcept { rsp_ = rsp; }
    [[nodiscard]] Rsp* rsp() const noexcept { return rsp_; }
    void attachCpu(Cpu* cpu) noexcept { cpu_ = cpu; }
    [[nodiscard]] Cpu* cpu() const noexcept { return cpu_; }

    [[nodiscard]] HwRegisters& hw() noexcept { return hw_; }
    [[nodiscard]] const HwRegisters& hw() const noexcept { return hw_; }
    [[nodiscard]] FlashRam& flash() noexcept { return flash_; }
    [[nodiscard]] IsViewer& isViewer() noexcept { return isViewer_; }
    [[nodiscard]] const IsViewer& isViewer() const noexcept { return isViewer_; }
    void setIsViewerLogCallback(IsViewer::LogCallback callback) {
        isViewer_.setLogCallback(std::move(callback));
    }
    [[nodiscard]] Eeprom& eeprom() noexcept { return eeprom_; }
    [[nodiscard]] const Eeprom& eeprom() const noexcept { return eeprom_; }
    [[nodiscard]] Sram& sram() noexcept { return sram_; }
    [[nodiscard]] const Sram& sram() const noexcept { return sram_; }

    // SRAM and FlashRAM overlap at 0x08000000; the active cartridge save
    // medium is selected explicitly or by bus operations, not ROM identity.
    enum class PiSaveMedia { None, Sram, FlashRam };
    void setPiSaveMedia(PiSaveMedia media) noexcept { piSaveMedia_ = media; autoPiSave_ = false; }
    void configureAutoPiSave(bool hasFlashImage = false) noexcept;
    void observePiSaveAccess(u32 physicalAddress) const noexcept;
    [[nodiscard]] PiSaveMedia piSaveMedia() const noexcept { return piSaveMedia_; }

    [[nodiscard]] u8 read8(u32 physicalAddress) const;
    [[nodiscard]] u16 read16(u32 physicalAddress) const;
    [[nodiscard]] u32 read32(u32 physicalAddress) const;
    [[nodiscard]] u64 read64(u32 physicalAddress) const;

    void write8(u32 physicalAddress, u8 value);
    void write16(u32 physicalAddress, u16 value);
    void write32(u32 physicalAddress, u32 value);
    void write64(u32 physicalAddress, u64 value);

    // Fast RDRAM write paths that also invalidate the CPU code cache.
    template<class T> void rdramWrite(u32 offset, T value);
    void rdramWrite8(u32 offset, u8 value);
    void rdramWrite16(u32 offset, u16 value);
    void rdramWrite32(u32 offset, u32 value);
    void rdramWrite64(u32 offset, u64 value);

    void invalidateCodeCache(u32 physicalAddress, u32 size) const noexcept;

private:
    Memory rdram_;
    FlashRam flash_;
    IsViewer isViewer_;
    Eeprom eeprom_;
    Sram sram_;
    HwRegisters hw_;
    mutable PiSaveMedia piSaveMedia_ = PiSaveMedia::None;
    bool autoPiSave_ = false;
    u16 autoFlashHigh_ = 0;
    // Four 2 MiB Rambus modules, with consecutive even device IDs.
    std::array<u32, Memory::RdramSize / (2 * 1024 * 1024)> rdramDelay_{};
    const Cartridge* cartridge_ = nullptr;
    Rsp* rsp_ = nullptr;
    Cpu* cpu_ = nullptr;
};

} // namespace n64forge
