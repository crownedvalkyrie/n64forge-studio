#pragma once
#include "n64forge/core/bus.hpp"
#include "n64forge/core/types.hpp"
#include <array>
#include <cstdint>

namespace n64forge {

class Rsp {
public:
    static constexpr u32 DmemSize = 0x1000;
    static constexpr u32 ImemSize = 0x1000;

    Rsp();

    void reset();
    void setBus(Bus* bus) noexcept { bus_ = bus; }
    void step();

    struct TaskTrace {
        u64 launches = 0;
        u64 breaks = 0;
        u64 doneAtBreak = 0;
    };
    // Observational only: DMEM OSTask type at CPU unhalt, and SIG2 at BREAK.
    [[nodiscard]] const auto& taskTrace() const noexcept { return taskTrace_; }
    [[nodiscard]] u64 taskLaunches() const noexcept { return taskLaunches_; }
    void observeLaunch() noexcept;

    [[nodiscard]] u32 pc() const noexcept { return pc_; }
    void setPc(u32 pc) noexcept;
    [[nodiscard]] const std::array<u32, 32>& regs() const noexcept { return regs_; }

    // RSP vector unit: 32 128-bit vector registers (8 lanes x 16-bit).
    static constexpr u32 Vlanes = 8;
    static constexpr u32 Vregs = 32;
    using Vreg = std::array<u16, Vlanes>;
    [[nodiscard]] const std::array<Vreg, Vregs>& vregs() const noexcept { return vregs_; }
    [[nodiscard]] const Vreg& vreg(u32 index) const noexcept { return vregs_[index & 0x1F]; }
    void setVreg(u32 index, const Vreg& value) noexcept { vregs_[index & 0x1F] = value; }
    [[nodiscard]] const std::array<s64, Vlanes>& accum() const noexcept { return accum_; }
    [[nodiscard]] const std::array<u16, 3>& vectorFlags() const noexcept { return vectorFlags_; }
    [[nodiscard]] std::array<u16, 3> dividerState() const noexcept {
        return {divIn_, static_cast<u16>(divHigh_ ? 1 : 0), divOut_};
    }

    // Load a vector from DMEM in one of the LWV/SWV-style elemental formats.
    // `offset` selects the starting byte in DMEM; `element` selects the first lane.
    void vload(u32 vrt, u32 element, u32 dmemOffset);
    void vstore(u32 vrt, u32 element, u32 dmemOffset) const;

    // CPU-visible SP memory access
    [[nodiscard]] u8 readMem8(u32 spAddress) const;
    void writeMem8(u32 spAddress, u8 value);
    [[nodiscard]] u32 readMem32(u32 spAddress) const;
    void writeMem32(u32 spAddress, u32 value);

    void dmaRead(u32 dramAddr, u32 spAddr, u32 length);
    void dmaWrite(u32 dramAddr, u32 spAddr, u32 length);

private:
    struct DecodedInstr {
        u32 target;
        s16 imm;
        u8 op, rs, rt, rd, shamt, funct, element, vectorElement;
        s16 vectorOffset;
        bool valid;
    };
    void writeGpr(u32 index, u32 value) noexcept;
    [[nodiscard]] const DecodedInstr& fetch() noexcept;
    void execute(const DecodedInstr& instr);
    void executeCop2(const DecodedInstr& instr);
    void vectorMemory(const DecodedInstr& instr, bool store);

    Bus* bus_ = nullptr;
    std::array<u32, 32> regs_{};
    u32 pc_ = 0;
    u32 nextPc_ = 4;
    std::array<u8, DmemSize> dmem_{};
    std::array<u8, ImemSize> imem_{};
    std::array<DecodedInstr, ImemSize / 4> decoded_{};
    std::array<Vreg, Vregs> vregs_{};

    std::array<s64, Vlanes> accum_{};
    std::array<u16, 3> vectorFlags_{};
    u16 divIn_ = 0;
    u16 divOut_ = 0;
    bool divHigh_ = false;
    std::array<TaskTrace, 5> taskTrace_{};
    u64 taskLaunches_ = 0;
    u32 taskType_ = 0;
};

} // namespace n64forge
