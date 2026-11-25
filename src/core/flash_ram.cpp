#include "n64forge/core/flash_ram.hpp"
#include "n64forge/core/hw_registers.hpp"
#include <algorithm>

namespace n64forge {
bool FlashRam::load(std::span<const u8> data) noexcept {
    if (data.size() != Size) return false;
    std::copy(data.begin(), data.end(), storage_.begin());
    dirty_ = false;
    return true;
}

void FlashRam::reset() noexcept {
    dirty_ = false;
    mode_ = Mode::Array;
    setup_ = busy_ = Operation::Idle;
    offset_ = cycles_ = 0;
    commandHigh_ = 0;
    commandHighValid_ = false;
    page_.fill(0xFF);
    status_ = 0x8C; // WSM ready, program/erase OK
}

void FlashRam::commandHalf(u32 offset, u16 value) noexcept {
    if (offset == 0) {
        commandHigh_ = value;
        commandHighValid_ = true;
        return;
    }
    if (offset != 2 || !commandHighValid_) return;
    commandHighValid_ = false;
    command((u32{commandHigh_} << 16) | value);
}

void FlashRam::command(u32 value) noexcept {
    const u32 opcode = value >> 24;
    if (opcode == 0xD2) { mode_ = Mode::Status; return; }
    if (cycles_) return;
    const auto page = value & 0x3FF;
    constexpr u32 ClocksPerMs = HwRegisters::CpuClockHz / 1000;
    switch (opcode) {
        case 0x3C: setup_ = Operation::ChipErase; break;
        case 0x4B: setup_ = Operation::SectorErase; offset_ = (page * Page) & ~(Sector - 1); break;
        case 0x78:
            if (setup_ == Operation::Idle) break;
            busy_ = setup_;
            setup_ = Operation::Idle;
            cycles_ = ClocksPerMs * 85;
            status_ = (status_ & ~0x88u) | 2;
            mode_ = Mode::Status;
            break;
        case 0xA5:
            offset_ = page * Page;
            busy_ = Operation::Program;
            cycles_ = ClocksPerMs * 7 / 2;
            status_ = (status_ & ~0x84u) | 1;
            mode_ = Mode::Status;
            break;
        case 0xB4: mode_ = Mode::LoadPage; break;
        case 0xE1: mode_ = Mode::SiliconId; break;
        case 0xF0: mode_ = Mode::Array; break;
    }
}

void FlashRam::advance(u32 cycles) noexcept {
    if (!cycles_) return;
    if (cycles < cycles_) { cycles_ -= cycles; return; }
    cycles_ = 0;
    if (busy_ == Operation::Program) {
        for (u32 i = 0; i < Page; ++i) storage_[offset_ + i] &= page_[i];
        page_.fill(0xFF);
        status_ = (status_ & ~1u) | 0x84;
    } else {
        if (busy_ == Operation::ChipErase) storage_.fill(0xFF);
        else std::fill_n(storage_.begin() + offset_, Sector, 0xFF);
        status_ = (status_ & ~2u) | 0x88;
    }
    busy_ = Operation::Idle;
    dirty_ = true;
}

u8 FlashRam::read(u32 offset) const noexcept {
    switch (mode_) {
        case Mode::Array: return offset < Size ? storage_[offset] : 0;
        case Mode::Status: return (offset & 1) ? static_cast<u8>(status_) : 0;
        case Mode::SiliconId: {
            constexpr u8 id[]{0x11, 0x11, 0x80, 0x01, 0x00, 0xC2, 0x00, 0x1D};
            return id[offset & 7];
        }
        case Mode::LoadPage: return page_[offset & (Page - 1)];
    }
    return 0;
}

void FlashRam::write(u32 offset, u8 value) noexcept {
    if (mode_ == Mode::LoadPage && !cycles_) page_[offset & (Page - 1)] = value;
    if (mode_ == Mode::Status && !cycles_ && offset == 1 && !value) status_ |= 0x0C;
}
} // namespace n64forge
