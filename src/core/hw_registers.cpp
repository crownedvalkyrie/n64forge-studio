#include "n64forge/core/hw_registers.hpp"
#include "n64forge/core/eeprom.hpp"
#include "n64forge/core/sram.hpp"
#include "n64forge/core/bus.hpp"
#include "n64forge/core/cartridge.hpp"

#include <algorithm>
#include <cstring>

namespace n64forge {

namespace {

// MI register offsets relative to 0x04300000.
constexpr u32 MI_INIT_MODE = 0x00;
constexpr u32 MI_VERSION = 0x04;
constexpr u32 MI_INTR = 0x08;
constexpr u32 MI_INTR_MASK = 0x0C;

// VI register offsets relative to 0x04400000.
constexpr u32 VI_CONTROL = 0x00;
constexpr u32 VI_ORIGIN = 0x04;
constexpr u32 VI_WIDTH = 0x08;
constexpr u32 VI_V_INTR = 0x0C;
constexpr u32 VI_CURRENT = 0x10;
constexpr u32 VI_BURST = 0x14;
constexpr u32 VI_V_SYNC = 0x18;
constexpr u32 VI_H_SYNC = 0x1C;
constexpr u32 VI_H_SYNC_LEAP = 0x20;
constexpr u32 VI_H_START = 0x24;
constexpr u32 VI_V_START = 0x28;
constexpr u32 VI_V_BURST = 0x2C;
constexpr u32 VI_X_SCALE = 0x30;
constexpr u32 VI_Y_SCALE = 0x34;
constexpr u32 VI_TEST_ADDR = 0x38;
constexpr u32 VI_STAGED_DATA = 0x3C;

// AI register offsets relative to 0x04500000.
constexpr u32 AI_DRAM_ADDR = 0x00;
constexpr u32 AI_LENGTH = 0x04;
constexpr u32 AI_CONTROL = 0x08;
constexpr u32 AI_STATUS = 0x0C;
constexpr u32 AI_DACRATE = 0x10;
constexpr u32 AI_BITRATE = 0x14;

// PI register offsets relative to 0x04600000.
constexpr u32 PI_DRAM_ADDR = 0x00;
constexpr u32 PI_CART_ADDR = 0x04;
constexpr u32 PI_RD_LEN = 0x08;
constexpr u32 PI_WR_LEN = 0x0C;
constexpr u32 PI_STATUS = 0x10;
constexpr u32 PI_BSD_DOM1_LAT = 0x14;
constexpr u32 PI_BSD_DOM1_PWD = 0x18;
constexpr u32 PI_BSD_DOM1_PGS = 0x1C;
constexpr u32 PI_BSD_DOM1_RLS = 0x20;
constexpr u32 PI_BSD_DOM2_LAT = 0x24;
constexpr u32 PI_BSD_DOM2_PWD = 0x28;
constexpr u32 PI_BSD_DOM2_PGS = 0x2C;
constexpr u32 PI_BSD_DOM2_RLS = 0x30;

// SI register offsets relative to 0x04800000.
constexpr u32 SI_DRAM_ADDR = 0x00;
constexpr u32 SI_PIF_ADDR_RD64B = 0x04;
constexpr u32 SI_PIF_ADDR_WR64B = 0x10;
constexpr u32 SI_STATUS = 0x18;

// RI register offsets relative to 0x04700000.
constexpr u32 RI_MODE = 0x00;
constexpr u32 RI_CONFIG = 0x04;
constexpr u32 RI_CURRENT_LOAD = 0x08;
constexpr u32 RI_SELECT = 0x0C;
constexpr u32 RI_REFRESH = 0x10;
constexpr u32 RI_LATENCY = 0x14;
constexpr u32 RI_RERROR = 0x18;
constexpr u32 RI_WERROR = 0x1C;

// SP register offsets relative to 0x04040000.
constexpr u32 SP_STATUS = 0x10;
constexpr u32 SP_DMA_FULL = 0x14;
constexpr u32 SP_DMA_BUSY = 0x18;
constexpr u32 SP_SEMAPHORE = 0x1C;

// DP register offsets relative to 0x04100000.
constexpr u32 DP_START = 0x00;
constexpr u32 DP_END = 0x04;
constexpr u32 DP_CURRENT = 0x08;
constexpr u32 DP_STATUS = 0x0C;
constexpr u32 DP_CLOCK = 0x10;
constexpr u32 DP_BUFBUSY = 0x14;
constexpr u32 DP_PIPEBUSY = 0x18;
constexpr u32 DP_TMEM = 0x1C;

// RCP registers use paired write-one-to-clear / write-one-to-set bits. If both
// bits of a pair are written, hardware leaves the state bit unchanged.
enum class SetClearAction { None, Clear, Set };
[[nodiscard]] SetClearAction setClearAction(u32 value, u32 clearBit, u32 setBit) noexcept {
    const bool clear = (value & clearBit) != 0;
    const bool set = (value & setBit) != 0;
    if (clear == set) return SetClearAction::None;
    return clear ? SetClearAction::Clear : SetClearAction::Set;
}

} // namespace

// ---------------------------------------------------------------------------
// VI
// ---------------------------------------------------------------------------
u32 ViRegisters::read(u32 offset) const noexcept {
    switch (offset) {
        case VI_CONTROL: return status;
        case VI_ORIGIN: return origin;
        case VI_WIDTH: return width;
        case VI_V_INTR: return intr;
        case VI_CURRENT: return current;
        case VI_BURST: return burst;
        case VI_V_SYNC: return vSync;
        case VI_H_SYNC: return hSync;
        case VI_H_SYNC_LEAP: return leap;
        case VI_H_START: return hStart;
        case VI_V_START: return vStart;
        case VI_V_BURST: return vBurst;
        case VI_X_SCALE: return xScale;
        case VI_Y_SCALE: return yScale;
        case VI_TEST_ADDR: return testAddr;
        case VI_STAGED_DATA: return stagedData;
        default: return 0;
    }
}

void ViRegisters::write(u32 offset, u32 value) noexcept {
    switch (offset) {
        case VI_CONTROL: status = value; break;
        case VI_ORIGIN: origin = value & 0x00FFFFFF; break;
        case VI_WIDTH: width = value & 0x00000FFF; break;
        case VI_V_INTR: intr = value & 0x000003FF; break;
        case VI_CURRENT: break; // MI acknowledges the interrupt
        case VI_BURST: burst = value; break;
        case VI_V_SYNC: vSync = value & 0x000003FF; break;
        case VI_H_SYNC: hSync = value & 0x001F0FFF; break;
        case VI_H_SYNC_LEAP: leap = value & 0x0FFF0FFF; break;
        case VI_H_START: hStart = value; break;
        case VI_V_START: vStart = value; break;
        case VI_V_BURST: vBurst = value; break;
        case VI_X_SCALE: xScale = value; break;
        case VI_Y_SCALE: yScale = value; break;
        default: break;
    }
}

// ---------------------------------------------------------------------------
// AI
// ---------------------------------------------------------------------------
u32 AiRegisters::read(u32 offset) const noexcept {
    if (offset != AI_STATUS) return count ? fifo[0].remaining : 0;
    return 0x01100000 | (control << 25) | (count ? 0x40000000 : 0) |
           (count == 2 ? 0x80000001 : 0);
}

void AiRegisters::write(u32 offset, u32 value) noexcept {
    switch (offset) {
        case AI_DRAM_ADDR: if (count < 2) dramAddr = value & 0x00FFFFF8; break;
        case AI_LENGTH:
            if (count < 2) fifo[count++] = {dramAddr, value & 0x0003FFF8};
            break;
        case AI_CONTROL: control = value & 1; break;
        case AI_DACRATE: dacrate = value & 0x00003FFF; break;
        case AI_BITRATE: bitrate = value & 0x0000000F; break;
        default: break;
    }
}

// ---------------------------------------------------------------------------
// PI
// ---------------------------------------------------------------------------
u32 PiRegisters::read(u32 offset) const noexcept {
    switch (offset) {
        case PI_DRAM_ADDR: return dramAddr;
        case PI_CART_ADDR: return cartAddr;
        case PI_RD_LEN: return readLength;
        case PI_WR_LEN: return writeLength;
        case PI_STATUS: return status;
        case PI_BSD_DOM1_LAT: return dom1Latency;
        case PI_BSD_DOM1_PWD: return dom1PulseWidth;
        case PI_BSD_DOM1_PGS: return dom1PageSize;
        case PI_BSD_DOM1_RLS: return dom1Release;
        case PI_BSD_DOM2_LAT: return dom2Latency;
        case PI_BSD_DOM2_PWD: return dom2PulseWidth;
        case PI_BSD_DOM2_PGS: return dom2PageSize;
        case PI_BSD_DOM2_RLS: return dom2Release;
        default: return 0;
    }
}

void PiRegisters::write(u32 offset, u32 value, std::function<void(u32)> requestIrq) {
    // Only PI_STATUS may be written while a DMA is pending. Real hardware sets
    // the error bit and ignores the access; the guest must reset the interface.
    if (offset != PI_STATUS && (status & 1u) != 0) {
        status |= 0x04;
        return;
    }
    switch (offset) {
        case PI_DRAM_ADDR: dramAddr = value & 0x00FFFFFE; break;
        case PI_CART_ADDR: cartAddr = value & 0xFFFFFFFE; break;
        case PI_RD_LEN: {
            readLength = (value & 0x00FFFFFF) + 1;
            status |= 1;
            break;
        }
        case PI_WR_LEN: {
            writeLength = (value & 0x00FFFFFF) + 1;
            writeCartStart = cartAddr;
            status |= 1;
            break;
        }
        case PI_STATUS: {
            if (value & 0x01) {
                status &= ~(0x01u | 0x04u);
                clearRead();
                clearWrite();
            }
            if (value & 0x02) { status &= ~0x08u; requestIrq(0); } // clear interrupt
            break;
        }
        case PI_BSD_DOM1_LAT: dom1Latency = value & 0xFF; break;
        case PI_BSD_DOM1_PWD: dom1PulseWidth = value & 0xFF; break;
        case PI_BSD_DOM1_PGS: dom1PageSize = value & 0x0F; break;
        case PI_BSD_DOM1_RLS: dom1Release = value & 0x03; break;
        case PI_BSD_DOM2_LAT: dom2Latency = value & 0xFF; break;
        case PI_BSD_DOM2_PWD: dom2PulseWidth = value & 0xFF; break;
        case PI_BSD_DOM2_PGS: dom2PageSize = value & 0xFF; break;
        case PI_BSD_DOM2_RLS: dom2Release = value & 0xFF; break;
        default: break;
    }
}

// ---------------------------------------------------------------------------
// SI
// ---------------------------------------------------------------------------
u32 SiRegisters::read(u32 offset) const noexcept {
    switch (offset) {
        case SI_DRAM_ADDR: return dramAddr;
        case SI_PIF_ADDR_RD64B: return pifReadAddr;
        case SI_PIF_ADDR_WR64B: return pifWriteAddr;
        case SI_STATUS: return status;
        default: return 0;
    }
}

void SiRegisters::begin(Transfer direction) noexcept {
    if (transfer != Transfer::None) {
        status |= 8; // overlapping request
        return;
    }
    transfer = direction;
    status |= direction == Transfer::DirectWrite ? 3 : 1;
}

void SiRegisters::finishPifWrite() noexcept {
    auto& control = pifRam.back();
    if (!bootTerminated && (control & 0x08)) {
        bootTerminated = true;
        control &= ~0x08;
    }
    if (!(control & 0x01)) return;
    control &= ~0x01;
    joybusOffsets.fill(0xFF);
    for (unsigned offset = 0, channel = 0; offset < 63 && channel < joybusOffsets.size();) {
        const u8 send = pifRam[offset];
        if (send == 0xFE) break;
        if (send == 0xFF) { ++offset; continue; }
        if (send == 0 || send == 0xFD) { ++offset; ++channel; continue; }
        if (offset + 1 >= 63) break;
        const unsigned next = offset + 2 + (send & 0x3F) + (pifRam[offset + 1] & 0x3F);
        if (next > 63) break;
        joybusOffsets[channel++] = static_cast<u8>(offset);
        offset = next;
    }
}

void SiRegisters::preparePifRead(const std::array<u64, 4>& controllers, Eeprom* eeprom) noexcept {
    auto& control = pifRam.back();
    if (control & 0x02) {
        if (!cic6105) return; // no implemented challenge-capable CIC
        // CIC-6105 nibble transform; adapted from ares (ISC, see REFERENCES.md).
        constexpr u8 keys[2][16] = {
            {4, 7, 10, 7, 14, 5, 14, 1, 12, 15, 8, 15, 6, 3, 6, 9},
            {4, 1, 10, 7, 14, 5, 14, 1, 12, 9, 8, 5, 6, 3, 12, 9},
        };
        unsigned key = 11;
        bool table = false;
        for (unsigned i = 0x30; i < 0x3F; ++i) {
            unsigned result = 0;
            for (unsigned shift : {4u, 0u}) {
                const unsigned nibble = (key + 5 * ((pifRam[i] >> shift) & 15)) & 15;
                result |= nibble << shift;
                key = keys[table][nibble];
                const bool negative = nibble >= 8;
                const unsigned magnitude = (negative ? ~nibble : nibble) & 7;
                bool next = negative != (magnitude % 3 != 1);
                if (table && (nibble == 1 || nibble == 9)) next = true;
                if (table && (nibble == 11 || nibble == 14)) next = false;
                table = next;
            }
            pifRam[i] = static_cast<u8>(result);
        }
        control &= ~0x02;
        return;
    }

    for (unsigned channel = static_cast<unsigned>(joybusOffsets.size()); channel-- > 0;) {
        const unsigned offset = joybusOffsets[channel];
        if (offset >= 62) continue;
        const u8 send = pifRam[offset];
        if (send & 0xC0) continue;
        const unsigned tx = send & 0x3F;
        auto& receive = pifRam[offset + 1];
        const unsigned rx = receive & 0x3F;
        receive = static_cast<u8>(rx);
        if (!tx || offset + 2 + tx + rx > 63) continue;
        const u8 command = pifRam[offset + 2];
        auto replyBegin = pifRam.begin() + offset + 2 + tx;

        // EEPROM lives on Joybus channel 4 with distinct 0x04/0x05 commands.
        if (channel == 4 && eeprom != nullptr && eeprom->attached()) {
            if ((command == 0 || command == 0xFF) && tx == 1 && rx == 3) {
                eeprom->markAccessed();
                const u16 id = eeprom->infoId();
                replyBegin[0] = static_cast<u8>(id >> 8);
                replyBegin[1] = static_cast<u8>(id);
                replyBegin[2] = eeprom->infoStatus();
                continue;
            }
            if (command == 0x04 && tx == 2 && rx == 8) {
                const u8 block = pifRam[offset + 3];
                eeprom->observeBlock(block);
                std::array<u8, Eeprom::BlockSize> data{};
                if (!eeprom->readBlock(block, data)) receive |= 0x80;
                else std::copy_n(data.begin(), 8, replyBegin);
                continue;
            }
            if (command == 0x05 && tx == 10 && rx == 1) {
                const u8 block = pifRam[offset + 3];
                if (!eeprom->busy()) eeprom->observeBlock(block);
                const std::span<const u8> dataSpan(pifRam.data() + offset + 4, 8);
                *replyBegin = eeprom->writeBlock(block, dataSpan) ? 0 : 0x80;
                continue;
            }
            receive |= 0x80;
            continue;
        }

        const bool info = command == 0 || command == 0xFF;
        const unsigned length = info ? 3 : command == 1 ? 4 : 0;
        const u64 state = channel < controllers.size() ? controllers[channel] : 0;
        const bool connected = state >> 32;
        if (connected && command == 2 && tx == 3 && rx == 33) {
            const u16 address = (u16{pifRam[offset + 3]} << 8) | pifRam[offset + 4];
            replyBegin[32] = paks[channel].read(address, ControllerPak::Block(&*replyBegin, 32));
            continue;
        }
        if (connected && command == 3 && tx == 35 && rx == 1) {
            const u16 address = (u16{pifRam[offset + 3]} << 8) | pifRam[offset + 4];
            *replyBegin = paks[channel].write(address, ControllerPak::ConstBlock(&pifRam[offset + 5], 32));
            continue;
        }
        if (!connected || tx != 1 || length == 0) {
            receive |= 0x80;
            continue;
        }
        const std::array<u8, 4> reply = info ? std::array<u8, 4>{5, 0, paks[channel].info(command == 0xFF), 0}
            : std::array<u8, 4>{u8(state >> 24), u8(state >> 16), u8(state >> 8), u8(state)};
        if (rx != length) receive |= 0x40;
        std::copy_n(reply.begin(), std::min(rx, length), replyBegin);
    }
}

void SiRegisters::write(u32 offset, u32 value) noexcept {
    switch (offset) {
        case SI_DRAM_ADDR: dramAddr = value & 0x00FFFFF8; break;
        case SI_PIF_ADDR_RD64B:
            if (transfer == Transfer::None) pifReadAddr = value & 0x7FC;
            begin(Transfer::Read);
            break;
        case SI_PIF_ADDR_WR64B:
            if (transfer == Transfer::None) pifWriteAddr = value & 0x7FC;
            begin(Transfer::Write);
            break;
        case SI_STATUS: status &= ~0x1000u; break;
        default: break;
    }
}

// ---------------------------------------------------------------------------
// RI
// ---------------------------------------------------------------------------
u32 RiRegisters::read(u32 offset) const noexcept {
    switch (offset) {
        case RI_MODE: return mode;
        case RI_CONFIG: return config;
        case RI_CURRENT_LOAD: return current;
        case RI_SELECT: return select;
        case RI_REFRESH: return refresh;
        case RI_LATENCY: return latency;
        case RI_RERROR: return rError;
        case RI_WERROR: return wError;
        default: return 0;
    }
}

void RiRegisters::write(u32 offset, u32 value) noexcept {
    switch (offset) {
        case RI_MODE: mode = value; break;
        case RI_CONFIG: config = value; break;
        case RI_CURRENT_LOAD: current = value; break;
        case RI_SELECT: select = value; break;
        case RI_REFRESH: refresh = value; break;
        case RI_LATENCY: latency = value; break;
        case RI_RERROR: rError = value; break;
        case RI_WERROR: wError = value; break;
        default: break;
    }
}

// ---------------------------------------------------------------------------
// SP
// ---------------------------------------------------------------------------
u32 SpRegisters::read(u32 offset) const noexcept {
    switch (offset) {
        case 0: return memAddr;
        case 4: return dramAddr;
        case 8: case 12: return dmaLength;
        case SP_STATUS: return status;
        case SP_DMA_FULL: return dmaFull;
        case SP_DMA_BUSY: return dmaBusy;
        case SP_SEMAPHORE: {
            const u32 previous = semaphore;
            semaphore = 1;
            return previous;
        }
        default: return 0;
    }
}

void SpRegisters::write(u32 offset, u32 value) noexcept {
    switch (offset) {
        case 0: memAddr = value & 0x1FF8; break;
        case 4: dramAddr = value & 0xFFFFF8; break;
        case SP_STATUS: {
            const auto paired = [this, value](u32 clearBit, u32 setBit, u32 stateBit) {
                switch (setClearAction(value, clearBit, setBit)) {
                    case SetClearAction::Clear: status &= ~stateBit; break;
                    case SetClearAction::Set: status |= stateBit; break;
                    case SetClearAction::None: break;
                }
            };
            paired(0x00000001, 0x00000002, 0x0001); // halt
            if (value & 4) status &= ~2u;           // clear broke (RSP-only set)
            paired(0x20, 0x40, 0x20);               // single step
            paired(0x80, 0x100, 0x40);              // interrupt on break
            for (u32 i = 0; i < 8; ++i)
                paired(0x200u << (2 * i), 0x400u << (2 * i), 0x80u << i); // signals
            break;
        }
        case SP_SEMAPHORE: semaphore = 0; break; // writing releases
        default: break;
    }
}

// ---------------------------------------------------------------------------
// DP
// ---------------------------------------------------------------------------
u32 DpRegisters::read(u32 offset) const noexcept {
    switch (offset) {
        case DP_START: return start;
        case DP_END: return end;
        case DP_CURRENT: return current;
        case DP_STATUS: return status;
        case DP_CLOCK: return clock;
        case DP_BUFBUSY: return bufBusy;
        case DP_PIPEBUSY: return pipeBusy;
        case DP_TMEM: return tmem;
        default: return 0;
    }
}

void DpRegisters::write(u32 offset, u32 value) noexcept {
    switch (offset) {
        case DP_START:
            start = value & 0x00FFFFF8;
            status |= 0x400; // start pending until END
            break;
        case DP_END:
            end = value & 0x00FFFFF8;
            if (status & 0x400) {
                current = start;
                status &= ~0x400u;
            }
            if (status & 4) current = end;
            break;
        case DP_STATUS: {
            const auto paired = [this, value](u32 clearBit, u32 setBit, u32 stateBit) {
                switch (setClearAction(value, clearBit, setBit)) {
                    case SetClearAction::Clear: status &= ~stateBit; break;
                    case SetClearAction::Set: status |= stateBit; break;
                    case SetClearAction::None: break;
                }
            };
            paired(0x0001, 0x0002, 0x0001); // XBUS
            paired(0x0004, 0x0008, 0x0002); // FREEZE
            paired(0x0010, 0x0020, 0x0004); // FLUSH
            if (setClearAction(value, 0x0010, 0x0020) == SetClearAction::Set) {
                status = (status & ~0x520u) | 4;
                current = end;
            }
            if (value & 0x0040) tmem = 0;
            if (value & 0x0080) pipeBusy = 0;
            if (value & 0x0100) bufBusy = 0;
            if (value & 0x0200) clock = 0;
            break;
        }
        default: break;
    }
}

// ---------------------------------------------------------------------------
// Aggregated HW registers
// ---------------------------------------------------------------------------
HwRegisters::HwRegisters() { reset(); }

void HwRegisters::reset() noexcept {
    mi_ = {};
    vi_ = {};
    ai_ = {};
    pi_ = {};
    si_ = {};
    ri_ = {};
    sp_ = {};
    dp_ = {};
    videoClockHz_ = NtscVideoClockHz;
    viPhase_ = 0;
    viLeapField_ = 0;
    viFields_ = 0;
    viLineDuration_ = viScanlineDuration();
    aiPhase_ = 0;
    aiSamplePeriod_ = CpuClockHz;
}

u32 HwRegisters::read(u32 a) const {
    if (a >= 0x04300000 && a < 0x04300020) {
        switch (a & 0x1C) {
            case MI_INIT_MODE: return mi_.initMode;
            case MI_VERSION: return mi_.version;
            case MI_INTR: return mi_.interrupt;
            case MI_INTR_MASK: return mi_.mask;
        }
    } else if (a >= 0x04400000 && a < 0x04400040) {
        return vi_.read(a - 0x04400000);
    } else if (a >= 0x04500000 && a < 0x04500020) {
        return ai_.read(a - 0x04500000);
    } else if (a >= 0x04600000 && a < 0x04700000) {
        return pi_.read(a & 0x3F);
    } else if (a >= 0x04700000 && a < 0x04800000) {
        return ri_.read(a & 0x1F);
    } else if (a >= 0x04800000 && a < 0x04800020) {
        return si_.read(a - 0x04800000);
    } else if (a >= 0x04040000 && a < 0x04040020) {
        return sp_.read(a - 0x04040000);
    } else if (a >= 0x04100000 && a < 0x04100020) {
        return dp_.read(a - 0x04100000);
    }
    return 0;
}

void HwRegisters::write(u32 a, u32 value) {
    if (a >= 0x04300000 && a < 0x04300020) {
        switch (a & 0x1C) {
            case MI_INIT_MODE:
                mi_.initMode = value;
                if (value & 0x800) mi_.clearPending(0x20);
                break;
            case MI_INTR_MASK:
                for (u32 i = 0; i < 6; ++i) {
                    const u32 bit = 1u << i;
                    switch (setClearAction(value, 1u << (2 * i), 2u << (2 * i))) {
                        case SetClearAction::Clear: mi_.mask &= ~bit; break;
                        case SetClearAction::Set: mi_.mask |= bit; break;
                        case SetClearAction::None: break;
                    }
                }
                break;
            default: break;
        }
    } else if (a >= 0x04400000 && a < 0x04400040) {
        vi_.write(a - 0x04400000, value);
        viLineDuration_ = viScanlineDuration();
        if (a - 0x04400000 == VI_CURRENT) mi_.clearPending(0x08);
    } else if (a >= 0x04500000 && a < 0x04500020) {
        const u32 offset = a - 0x04500000;
        const bool idle = ai_.count == 0;
        ai_.write(offset, value);
        if (offset == AI_DACRATE) aiSamplePeriod_ = u64{CpuClockHz} * (ai_.dacrate + 1u);
        if (offset == AI_LENGTH && idle && ai_.count) mi_.setPending(4);
        if (offset == AI_STATUS) mi_.clearPending(4);
    } else if (a >= 0x04600000 && a < 0x04700000) {
        pi_.write(a & 0x3F, value, [this](u32) { mi_.clearPending(0x10); });
    } else if (a >= 0x04700000 && a < 0x04800000) {
        ri_.write(a & 0x1F, value);
    } else if (a >= 0x04800000 && a < 0x04800020) {
        si_.write(a - 0x04800000, value);
        if (a - 0x04800000 == SI_STATUS) mi_.clearPending(0x02);
    } else if (a >= 0x04040000 && a < 0x04040020) {
        sp_.write(a - 0x04040000, value);
        if (a - 0x04040000 == SP_STATUS) {
            switch (setClearAction(value, 8, 16)) {
                case SetClearAction::Clear: mi_.clearPending(1); break;
                case SetClearAction::Set: mi_.setPending(1); break;
                case SetClearAction::None: break;
            }
        }
    } else if (a >= 0x04100000 && a < 0x04100020) {
        dp_.write(a - 0x04100000, value);
    }
}

std::optional<HwRegisters::StereoSample> HwRegisters::drainAudio(const Bus& bus) {
    auto& front = ai_.fifo[0];
    std::optional<StereoSample> sample;
    if (front.remaining) {
        if (ai_.addressCarry) front.address = (front.address + 0x2000) & 0xFFFFFF;
        const u32 word = front.address < Memory::RdramSize ? bus.rdram().read32(front.address) : 0;
        sample = StereoSample{static_cast<s16>(word >> 16), static_cast<s16>(word)};
        ++ai_.samplesDrained;
        ai_.nonZeroSamples += sample->front() != 0 || sample->back() != 0;
        ai_.addressCarry = (front.address & 0x1FFF) == 0x1FFC;
        front.address = (front.address & ~0x1FFFu) | ((front.address + 4) & 0x1FFF);
        front.remaining -= 4;
    }
    if (!front.remaining && --ai_.count) {
        front = ai_.fifo[1];
        mi_.setPending(4); // the next buffer becomes active, not an end-of-DMA IRQ
    }
    return sample;
}

u32 HwRegisters::cyclesToDeviceEvent(u32 limit) const noexcept {
    // The current reference model transfers a PI chunk per step, completes SI
    // in one step, executes SP DMA synchronously, and issues one RSP step per
    // CPU step while unhalted. These are not safe to batch.
    if (pi_.dmaWriteActive() || pi_.dmaReadActive() ||
        si_.transfer != SiRegisters::Transfer::None || !(sp_.status & 1) ||
        dp_.current < dp_.end || (dp_.status & 4)) return 1;
    if (!vi_.vSync && (vi_.current || viPhase_ || viLeapField_)) return 1;
    const auto clip = [&](u64 period, u64 phase) {
        if (phase >= period) {
            limit = 1; // A timing register write may shorten a period.
        } else {
            const u64 delta = period - phase;
            if (delta <= u64{limit} * videoClockHz_)
                limit = static_cast<u32>((delta + videoClockHz_ - 1) / videoClockHz_);
        }
    };
    if (vi_.vSync) clip(viLineDuration_, viPhase_);
    if (ai_.count && ai_.control) clip(aiSamplePeriod_, aiPhase_);
    return limit;
}

void HwRegisters::setController(unsigned port, input::ControllerState state, bool connected) noexcept {
    if (port >= controllers_.size()) return;
    const u64 packed = (u64{connected} << 32) | (u64{state.buttons} << 16) |
                       (u64{static_cast<u8>(state.stickX)} << 8) | static_cast<u8>(state.stickY);
    controllers_[port].store(packed, std::memory_order_relaxed);
}

u32 HwRegisters::cyclesToNextField() const noexcept {
    if (vi_.vSync == 0) return 0;
    const u64 clock = videoClockHz_;
    u64 phase = viPhase_;
    u32 current = vi_.current;
    u64 total = 0;
    // Leap-field selection is fixed until the field wraps, and a wrap is the
    // boundary this function is looking for, so only the line counter changes.
    for (u32 guard = 0; guard < (1u << 16); ++guard) {
        u32 clocks = (vi_.hSync & 0xFFF) + 1;
        if ((current >> 1) == 1) {
            const bool useB = (vi_.hSync >> (16 + viLeapField_)) & 1;
            const u32 leap = (vi_.leap >> (useB ? 0 : 16)) & 0xFFF;
            if (leap != 0) clocks = leap + 1;
        }
        const u64 duration = static_cast<u64>(clocks) * CpuClockHz;
        if (phase < duration) {
            const u64 need = duration - phase;
            const u64 steps = (need + clock - 1) / clock;
            total += steps;
            phase += steps * clock - duration;
        } else {
            phase -= duration;
        }
        current += 2;
        if (current > vi_.vSync) {
            return total > 0xFFFFFFFFu ? 0xFFFFFFFFu : static_cast<u32>(total);
        }
    }
    return 0;
}

void HwRegisters::advanceVideo(u64 duration) noexcept {
    do {
        viPhase_ -= duration;

        const bool progressive = vi_.vSync & 1;
        const u32 field = vi_.current & 1;
        vi_.current += 2;
        if (vi_.current > vi_.vSync) {
            vi_.current = progressive ? field : field ^ 1;
            viLeapField_ = (viLeapField_ + 1) % 5;
            ++viFields_;
        }
        const u32 current = vi_.current;
        const bool lineMatch = (progressive || (vi_.intr & 1))
            ? (current >> 1) == (vi_.intr >> 1)
            : (current & 1) ? current + 1 == vi_.intr
                            : current == vi_.intr || (current == vi_.vSync && vi_.intr == 0);
        if (lineMatch) mi_.setPending(0x08);
        duration = viScanlineDuration();
    } while (viPhase_ >= duration);
    viLineDuration_ = duration;
}

bool HwRegisters::serviceDma(Bus& bus) {
    bool work = false;
    const Cartridge* cart = bus.cartridge();

    // PI read: cartridge -> RDRAM
    if (pi_.dmaWriteActive()) {
        work = true;
        const u32 len = std::min<u32>(pi_.writeLength, 128);
        bus.observePiSaveAccess(pi_.cartAddr);
        const u32 pageMask = (1u << ((pi_.dom1PageSize & 15) + 2)) - 1u;
        for (u32 i = 0; i < len; ++i) {
            const u32 dram = (pi_.dramAddr + i) & 0x00FFFFFF;
            const u32 address = pi_.cartAddr + i;
            if (dram < Memory::RdramSize && address >= FlashRam::Base &&
                address < 0x10000000 && bus.piSaveMedia() == Bus::PiSaveMedia::Sram &&
                bus.sram().attached()) {
                bus.rdram().write8(dram, bus.sram().read(address - Sram::Base));
            } else if (dram < Memory::RdramSize && address >= FlashRam::Base &&
                address < 0x10000000 && bus.piSaveMedia() == Bus::PiSaveMedia::FlashRam &&
                bus.flash().attached()) {
                bus.rdram().write8(dram, bus.flash().read(address - FlashRam::Base));
            } else if (dram < Memory::RdramSize &&
                       address >= Cartridge::BusBase && address < Cartridge::BusEnd) {
                const u32 offset = address - Cartridge::BusBase;
                if (cart && offset < cart->size()) {
                    bus.write8(dram, cart->rom()[offset]);
                } else {
                    const u32 pageAddress = std::max(pi_.writeCartStart, address & ~pageMask);
                    bus.write8(dram, static_cast<u8>(pageAddress >> ((address & 1) ? 0 : 8)));
                }
            }
        }
        pi_.dramAddr += len;
        pi_.cartAddr += len;
        if (pi_.writeLength <= len) {
            pi_.clearWrite();
            pi_.status &= ~1u;
            pi_.status |= 0x08;
            mi_.setPending(0x10);
            if (onPiDmaComplete) onPiDmaComplete();
        } else {
            pi_.writeLength -= len;
        }
    }

    // PI write: RDRAM -> cartridge (writes to ROM are ignored)
    if (pi_.dmaReadActive()) {
        work = true;
        const u32 len = std::min<u32>(pi_.readLength, 128);
        bus.observePiSaveAccess(pi_.cartAddr);
        if (pi_.cartAddr >= FlashRam::Base && pi_.cartAddr < 0x10000000 &&
            bus.piSaveMedia() == Bus::PiSaveMedia::Sram && bus.sram().attached()) {
            for (u32 i = 0; i < len; ++i) {
                const u32 dram = (pi_.dramAddr + i) & 0xFFFFFF;
                const u8 value = dram < Memory::RdramSize ? bus.rdram().read8(dram) : 0;
                bus.sram().write(pi_.cartAddr + i - Sram::Base, value);
            }
        } else if (pi_.cartAddr >= FlashRam::Base && pi_.cartAddr < 0x10000000 &&
            bus.piSaveMedia() == Bus::PiSaveMedia::FlashRam && bus.flash().attached()) {
            for (u32 i = 0; i < len; ++i) {
                const u32 dram = (pi_.dramAddr + i) & 0xFFFFFF;
                const u8 value = dram < Memory::RdramSize ? bus.rdram().read8(dram) : 0;
                bus.flash().write(pi_.cartAddr + i - FlashRam::Base, value);
            }
        }
        pi_.dramAddr += len;
        pi_.cartAddr += len;
        if (pi_.readLength <= len) {
            pi_.clearRead();
            pi_.status &= ~1u;
            pi_.status |= 0x08;
            mi_.setPending(0x10);
            if (onPiDmaComplete) onPiDmaComplete();
        } else {
            pi_.readLength -= len;
        }
    }

    if (si_.transfer != SiRegisters::Transfer::None) {
        const bool read = si_.transfer == SiRegisters::Transfer::Read;
        if (read) {
            std::array<u64, 4> inputs;
            for (unsigned i = 0; i < inputs.size(); ++i)
                inputs[i] = controllers_[i].load(std::memory_order_relaxed);
            si_.preparePifRead(inputs, &bus.eeprom());
        }
        if (read || si_.transfer == SiRegisters::Transfer::Write) {
            const u32 pif = read ? si_.pifReadAddr : si_.pifWriteAddr;
            for (u32 i = 0; i < 64; ++i) {
                const u32 dram = (si_.dramAddr + i) & 0xFFFFFF;
                if (dram >= Memory::RdramSize) continue;
                const u32 address = 0x1FC00000 + ((pif + i) & 0x7FF);
                if (read) bus.rdram().write8(dram, bus.read8(address));
                else bus.write8(address, bus.rdram().read8(dram));
            }
            si_.dramAddr = (si_.dramAddr + 64) & 0xFFFFFF;
        }
        if (!read) si_.finishPifWrite();
        si_.transfer = SiRegisters::Transfer::None;
        si_.status = (si_.status & ~3u) | 0x1000;
        mi_.setPending(0x02);
        work = true;
        if (onSiDmaComplete) onSiDmaComplete();
    }

    return work;
}

} // namespace n64forge
