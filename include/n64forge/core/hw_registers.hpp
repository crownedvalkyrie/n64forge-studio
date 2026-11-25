#pragma once
#include "n64forge/input/controller.hpp"
#include "n64forge/core/controller_pak.hpp"
#include <atomic>
// N64 memory-mapped hardware register blocks (MI, VI, AI, PI, SI, RI, SP, DP)
// and the interrupt controller / DMA engines used by the in-house hardware model.
#include "n64forge/core/types.hpp"

#include <array>
#include <cstddef>
#include <functional>
#include <optional>

namespace n64forge {

class Bus;

// ---------------------------------------------------------------------------
// MIPS Interface (MI): interrupt mask/pending, init mode.
// ---------------------------------------------------------------------------
struct MiRegisters {
    u32 initMode = 0;
    u32 version = 0;
    u32 interrupt = 0;   // pending bits
    u32 mask = 0;        // enabled bits

    [[nodiscard]] bool irq() const noexcept { return (interrupt & mask) != 0; }
    void setPending(u32 bits) noexcept { interrupt |= bits; }
    void clearPending(u32 bits) noexcept { interrupt &= ~bits; }
};

// ---------------------------------------------------------------------------
// Video Interface (VI): timing, origin, width, current vertical line.
// ---------------------------------------------------------------------------
struct ViRegisters {
    u32 status = 0;
    u32 origin = 0;
    u32 width = 0;
    u32 intr = 0x3FF;
    u32 current = 0;
    u32 burst = 0;
    u32 vSync = 0;
    u32 hSync = 0x7FF;
    u32 leap = 0;
    u32 hStart = 0;
    u32 vStart = 0;
    u32 vBurst = 0;
    u32 xScale = 0;
    u32 yScale = 0;
    u32 testAddr = 0;
    u32 stagedData = 0;

    [[nodiscard]] u32 read(u32 offset) const noexcept;
    void write(u32 offset, u32 value) noexcept;
};

// ---------------------------------------------------------------------------
// Audio Interface (AI): DRAM address, length, control, status.
// ---------------------------------------------------------------------------
struct AiRegisters {
    struct Buffer { u32 address = 0; u32 remaining = 0; };
    u32 dramAddr = 0;
    u32 control = 0;
    u32 dacrate = 0;
    u32 bitrate = 0;
    std::array<Buffer, 2> fifo{};
    u32 count = 0;
    bool addressCarry = false;
    // Host diagnostics: proves whether AI read silence or wrong addresses.
    u64 samplesDrained = 0;
    u64 nonZeroSamples = 0;

    [[nodiscard]] u32 read(u32 offset) const noexcept;
    void write(u32 offset, u32 value) noexcept;
};

// ---------------------------------------------------------------------------
// Peripheral Interface (PI): DMA to/from cartridge and DRAM.
// ---------------------------------------------------------------------------
struct PiRegisters {
    u32 dramAddr = 0;
    u32 cartAddr = 0;
    u32 readLength = 0;
    u32 writeLength = 0;
    u32 writeCartStart = 0;
    u32 status = 0;
    u32 dom1Latency = 0;
    u32 dom1PulseWidth = 0;
    u32 dom1PageSize = 0;
    u32 dom1Release = 0;
    u32 dom2Latency = 0;
    u32 dom2PulseWidth = 0;
    u32 dom2PageSize = 0;
    u32 dom2Release = 0;

    [[nodiscard]] u32 read(u32 offset) const noexcept;
    void write(u32 offset, u32 value, std::function<void(u32)> requestIrq);

    [[nodiscard]] bool dmaReadActive() const noexcept { return readLength != 0; }
    [[nodiscard]] bool dmaWriteActive() const noexcept { return writeLength != 0; }
    void clearRead() noexcept { readLength = 0; }
    void clearWrite() noexcept { writeLength = 0; }
};

// ---------------------------------------------------------------------------
// Serial Interface (SI): controller PIF RAM DMA.
// ---------------------------------------------------------------------------
class Eeprom;

struct SiRegisters {
    enum class Transfer { None, Read, Write, DirectWrite };
    u32 dramAddr = 0;
    std::array<u8, 64> pifRam{};
    u32 status = 0;
    u32 pifReadAddr = 0;
    u32 pifWriteAddr = 0;
    Transfer transfer = Transfer::None;
    void begin(Transfer direction) noexcept;
    void finishPifWrite() noexcept;
    void preparePifRead(const std::array<u64, 4>& controllers, Eeprom* eeprom) noexcept;
    std::array<ControllerPak, 4> paks;
    bool cic6105 = false;
    bool bootTerminated = false;
    std::array<u8, 5> joybusOffsets{0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

    [[nodiscard]] u32 read(u32 offset) const noexcept;
    void write(u32 offset, u32 value) noexcept;
};

// ---------------------------------------------------------------------------
// RDRAM Interface (RI): mode, config, current/load, select, refresh.
// ---------------------------------------------------------------------------
struct RiRegisters {
    u32 mode = 0;
    u32 config = 0;
    u32 current = 0;
    u32 select = 0;
    u32 refresh = 0;
    u32 latency = 0;
    u32 rError = 0;
    u32 wError = 0;

    [[nodiscard]] u32 read(u32 offset) const noexcept;
    void write(u32 offset, u32 value) noexcept;
};

// ---------------------------------------------------------------------------
// RSP Interface (SP): status, DMA, semaphore, program counter.
// ---------------------------------------------------------------------------
struct SpRegisters {
    u32 status = 1; // HALT
    u32 memAddr = 0;
    u32 dramAddr = 0;
    u32 dmaFull = 0;
    u32 dmaBusy = 0;
    mutable u32 semaphore = 0;
    u32 dmaLength = 0;

    [[nodiscard]] u32 read(u32 offset) const noexcept;
    void write(u32 offset, u32 value) noexcept;
};

// ---------------------------------------------------------------------------
// RDP Interface (DP): start/end of command buffer, current, status.
// ---------------------------------------------------------------------------
struct DpRegisters {
    u32 start = 0;
    u32 end = 0;
    u32 current = 0;
    u32 status = 0;
    u32 clock = 0;
    u32 bufBusy = 0;
    u32 pipeBusy = 0;
    u32 tmem = 0;

    [[nodiscard]] u32 read(u32 offset) const noexcept;
    void write(u32 offset, u32 value) noexcept;
};

// ---------------------------------------------------------------------------
// Aggregated hardware model exposed to the bus.
// ---------------------------------------------------------------------------
class HwRegisters {
public:
    HwRegisters();

    void reset() noexcept;

    [[nodiscard]] u32 read(u32 physicalAddress) const;
    void write(u32 physicalAddress, u32 value);

    [[nodiscard]] MiRegisters& mi() noexcept { return mi_; }
    [[nodiscard]] const MiRegisters& mi() const noexcept { return mi_; }

    [[nodiscard]] const ViRegisters& vi() const noexcept { return vi_; }
    [[nodiscard]] const AiRegisters& ai() const noexcept { return ai_; }
    [[nodiscard]] PiRegisters& pi() noexcept { return pi_; }
    [[nodiscard]] SiRegisters& si() noexcept { return si_; }
    [[nodiscard]] const SiRegisters& si() const noexcept { return si_; }
    [[nodiscard]] RiRegisters& ri() noexcept { return ri_; }
    [[nodiscard]] SpRegisters& sp() noexcept { return sp_; }
    [[nodiscard]] DpRegisters& dp() noexcept { return dp_; }

    // Run one tick of DMA engines. Returns true if any work was done.
    bool tickDma(Bus& bus) {
        return (pi_.dmaWriteActive() || pi_.dmaReadActive() || si_.transfer != SiRegisters::Transfer::None)
               && serviceDma(bus);
    }
    static constexpr u32 CpuClockHz = 93'750'000;
    static constexpr u32 NtscVideoClockHz = 48'681'818;
    static constexpr u32 PalVideoClockHz = 49'656'530;
    static constexpr u32 MpalVideoClockHz = 48'628'316;
    void setVideoClock(u32 hz) noexcept { videoClockHz_ = hz; }
    void tickVideo(u32 cpuCycles) noexcept {
        if (vi_.vSync == 0) {
            vi_.current = 0;
            viPhase_ = 0;
            viLeapField_ = 0;
            return;
        }
        viPhase_ += static_cast<u64>(cpuCycles) * videoClockHz_;
        if (viPhase_ >= viLineDuration_) advanceVideo(viLineDuration_);
    }
    using StereoSample = std::array<s16, 2>;
    // Multi-cycle calls must not cross more than one DAC sample deadline.
    [[nodiscard]] std::optional<StereoSample> tickAudio(const Bus& bus, u32 cpuCycles = 1) {
        if (!ai_.count || !ai_.control) {
            aiPhase_ = 0;
            return {};
        }
        aiPhase_ += u64{cpuCycles} * videoClockHz_;
        if (aiPhase_ < aiSamplePeriod_) return {};
        aiPhase_ %= aiSamplePeriod_;
        return drainAudio(bus);
    }
    [[nodiscard]] u32 videoClockHz() const noexcept { return videoClockHz_; }
    [[nodiscard]] u64 videoFields() const noexcept { return viFields_; }
    [[nodiscard]] u32 cyclesToDeviceEvent(u32 limit) const noexcept;
    void setController(unsigned port, input::ControllerState state, bool connected) noexcept;

    // Nominal CPU cycles until the next VI field boundary, without mutating
    // timing state. Zero when VI timing is disabled (V_SYNC == 0) or when the
    // distance cannot be represented. Callers use this to stop a basic block at
    // a field edge instead of running the CPU past it.
    [[nodiscard]] u32 cyclesToNextField() const noexcept;

    // Optional callback invoked when PI/SI DMA completes.
    std::function<void()> onPiDmaComplete;
    std::function<void()> onSiDmaComplete;

private:
    bool serviceDma(Bus& bus);
    [[nodiscard]] std::optional<StereoSample> drainAudio(const Bus& bus);
    [[nodiscard]] u64 viScanlineDuration() const noexcept {
        u32 clocks = (vi_.hSync & 0xFFF) + 1;
        if ((vi_.current >> 1) == 1) {
            const bool useB = (vi_.hSync >> (16 + viLeapField_)) & 1;
            const u32 leap = (vi_.leap >> (useB ? 0 : 16)) & 0xFFF;
            if (leap != 0) clocks = leap + 1;
        }
        return static_cast<u64>(clocks) * CpuClockHz;
    }
    void advanceVideo(u64 duration) noexcept;
    MiRegisters mi_;
    ViRegisters vi_;
    AiRegisters ai_;
    PiRegisters pi_;
    SiRegisters si_;
    RiRegisters ri_;
    SpRegisters sp_;
    DpRegisters dp_;
    u32 videoClockHz_ = NtscVideoClockHz;
    u64 viPhase_ = 0;
    u64 viLineDuration_ = 0;
    u32 viLeapField_ = 0;
    u64 viFields_ = 0;
    u64 aiPhase_ = 0;
    u64 aiSamplePeriod_ = CpuClockHz;
    // Atomic snapshots published by the host input thread and read only on
    // SI transactions. Bit 32 is connection state; low word is wire bytes.
    std::array<std::atomic<u64>, 4> controllers_{{u64{1} << 32, 0, 0, 0}};
};

} // namespace n64forge
