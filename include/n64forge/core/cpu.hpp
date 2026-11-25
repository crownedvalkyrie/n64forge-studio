#pragma once
#include "n64forge/core/bus.hpp"
#include "n64forge/core/types.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace n64forge {
class Arm64Jit;
class X64Jit;

enum class CpuExecutionMode { Interpreter, Cached, Jit };
[[nodiscard]] constexpr CpuExecutionMode defaultDesktopCpuMode() noexcept {
#if (defined(__APPLE__) && defined(__aarch64__) && !defined(__arm64e__)) || \
    (defined(_WIN32) && defined(_M_X64))
    return CpuExecutionMode::Jit;
#else
    return CpuExecutionMode::Cached;
#endif
}

[[nodiscard]] constexpr std::optional<CpuExecutionMode> parseCpuMode(std::string_view name) {
    if (name == "interpreter") return CpuExecutionMode::Interpreter;
    if (name == "cached") return CpuExecutionMode::Cached;
    if (name == "jit") return CpuExecutionMode::Jit;
    return std::nullopt;
}

struct TlbEntry {
    u32 pageMask = 0;
    u32 vpn2 = 0;
    u32 region = 0;
    u32 asid = 0;
    u32 pfn[2] = {};
    bool valid[2] = {};
    bool dirty[2] = {};
    bool global = false;
    bool written = false;
};

// Thrown when an address translation or bus access cannot proceed before the
// CPU can raise the corresponding Coprocessor exception. Carries the exception
// cause code (TLB invalid/miss/modified, ...) so step() can route failures
// through the normal exception state machine instead of propagating.
class AccessException : public std::runtime_error {
public:
    AccessException(u32 causeCode, const std::string& message, bool refill = false);
    [[nodiscard]] u32 causeCode() const noexcept { return causeCode_; }
    [[nodiscard]] bool refill() const noexcept { return refill_; }

private:
    u32 causeCode_;
    bool refill_;
};

class Cpu {
public:
    Cpu();
    ~Cpu();
    bool setJitEnabled(bool enabled);
    [[nodiscard]] u64 jitSteps() const noexcept { return jitSteps_; }
    // Dispatch attempts include unsupported blocks and budget rejections.
    // A successful attempt may chain through many blocks.
    [[nodiscard]] u64 jitInvocations() const noexcept { return jitInvocations_; }

    void reset() noexcept;
    void step(Bus& bus);
    // Resume the decoded block for one nominal CPU step. The caller can
    // service devices before the next instruction, including delay slots.
    void stepCached(Bus& bus);
    // Execute an event-free prefix of a cached block. MMIO writes, faulting
    // accesses and COP0 operations are barriers; zero means single-step next.
    [[nodiscard]] u32 runQuietBlock(Bus& bus, u32 budget);
    // Single-step entry used by the native x64 backend. It restores all guest
    // state if the instruction faults, so the scalar CPU can retry it exactly
    // once from the reported PC.
    [[nodiscard]] bool stepJitInstruction(Bus& bus, u64 pc) noexcept;

    [[nodiscard]] u64 pc() const noexcept { return pc_; }
    [[nodiscard]] u64 nextPc() const noexcept { return nextPc_; }
    [[nodiscard]] const std::array<u64, 32>& gpr() const noexcept { return gpr_; }
    [[nodiscard]] u64 hi() const noexcept { return hi_; }
    [[nodiscard]] u64 lo() const noexcept { return lo_; }
    [[nodiscard]] const std::array<u64, 32>& cop0() const noexcept { return cop0_; }
    [[nodiscard]] const std::array<TlbEntry, 32>& tlb() const noexcept { return tlb_; }
    [[nodiscard]] const std::array<u64, 32>& fpr() const noexcept { return fpr_; }
    [[nodiscard]] u32 fcr31() const noexcept { return fcr31_; }

    void setPc(u64 pc) noexcept;
    void setIplHandoffRegisters(u32 tvType, u32 version, u64 s6, u64 returnAddress) noexcept;

    // Basic-block interpreter cache. Invalidation is driven by RDRAM writes so
    // self-modifying code and DMA stay coherent with the recompiled blocks.
    void attachToBus(Bus& bus);
    void invalidateBlockCache(u32 physicalAddress, u32 size) noexcept;
    void clearBlockCache() noexcept;

    struct CacheStats {
        u64 lookups = 0;
        u64 hits = 0;
        u64 blocksCompiled = 0;
        u64 blocksRun = 0;
        u64 branchLinksTaken = 0;
    };
    [[nodiscard]] const CacheStats& cacheStats() const noexcept { return cacheStats_; }
    [[nodiscard]] u64 instructionsDispatched() const noexcept { return instructionsDispatched_; }
    struct ExceptionTrace {
        std::array<u64, 32> counts{};
        std::array<u64, 32> lastPcByCode{};
        u64 firstPc = 0, lastPc = 0;
        u32 firstCode = 0, lastCode = 0;
        bool observed = false;
    };
    [[nodiscard]] const ExceptionTrace& exceptionTrace() const noexcept { return exceptionTrace_; }
    // Generation counters are written by the RDRAM write observer, which can
    // run on the RDP worker thread while the CPU thread reads them.
    [[nodiscard]] u32 rdramLineGeneration(u32 line) const noexcept {
        return lineGen_[line].load(std::memory_order_acquire);
    }
    [[nodiscard]] u64 rdramGenerationEpoch() const noexcept {
        return lineEpoch_.load(std::memory_order_acquire);
    }

private:
    template<bool Cached> void stepImpl(Bus& bus);
    void writeGpr(u32 index, u64 value) noexcept;
    [[nodiscard]] u32 fetch(Bus& bus);
    template<class T> [[nodiscard]] T load(Bus& bus, u64 address);
    template<class T> void store(Bus& bus, u64 address, T value);
    [[nodiscard]] u64 translateAddress(u64 vaddr, bool write);
    [[nodiscard]] u64 translateAddressTlb(u64 vaddr, bool write, bool* writable = nullptr);
    [[nodiscard]] u32 resolveMemory(u64 address, bool write);
    void latchFaultAddress(u64 address) noexcept;
    [[noreturn]] void addressError(u64 address, bool write);
    void clearMappedPages() noexcept { mappedPages_.fill({}); }
    // Cache physical offsets, not pointers that can dangle across Bus::reset.
    struct MappedPage {
        u64 tag = ~u64{0};
        u32 base = 0;
        bool writable = false;
    };
    std::array<MappedPage, 256> mappedPages_{};
    void execute(u32 instr, Bus& bus);
    void executeLegacy(u32 instr, Bus& bus);
    void raiseException(u32 code, bool refill = false, u32 coprocessor = 0);

    void cop0Execute(u32 instr, u32 rs, u32 rt, u32 rd, u32 funct);
    void cop1Execute(u32 instr, u32 rs, u32 rt, u32 rd, u32 funct);

public:
    // -----------------------------------------------------------------------
    // Decoded instruction and cached basic-block interpreter.
    // Public only so the internal dispatch table can be assembled outside
    // the class; these are implementation details not meant for callers.
    // -----------------------------------------------------------------------
    struct DecodedInstr {
        u32 raw = 0;
        u32 target = 0;
        s16 imm = 0;
        u8 op = 0, rs = 0, rt = 0, rd = 0, shamt = 0, quiet = 0;
        void (*handler)(Cpu&, Bus&, const DecodedInstr&) = nullptr;
    };

    static DecodedInstr decodeInstr(u32 instr) noexcept;
    // CPU-only probe helper. Returns nominal steps, including exception entry.
    // The emulator uses stepCached() to interleave hardware with each step.
    [[nodiscard]] u32 runBlock(Bus& bus, u32 maxInstructions = 0);
    void compileBlock(u64 vaddr, Bus& bus);

    // Fallback interpreter used for any instruction without a dedicated handler.
    static void executeInstr(Cpu& c, Bus& bus, const DecodedInstr& i);

    // Fast handlers for common integer and branch instructions.
    static void h_addiu(Cpu& c, Bus&, const DecodedInstr& i);
    static void h_addi(Cpu& c, Bus&, const DecodedInstr& i);
    static void h_addu(Cpu& c, Bus&, const DecodedInstr& i);
    static void h_add(Cpu& c, Bus&, const DecodedInstr& i);
    static void h_subu(Cpu& c, Bus&, const DecodedInstr& i);
    static void h_sub(Cpu& c, Bus&, const DecodedInstr& i);
    static void h_and(Cpu& c, Bus&, const DecodedInstr& i);
    static void h_or(Cpu& c, Bus&, const DecodedInstr& i);
    static void h_xor(Cpu& c, Bus&, const DecodedInstr& i);
    static void h_nor(Cpu& c, Bus&, const DecodedInstr& i);
    static void h_slt(Cpu& c, Bus&, const DecodedInstr& i);
    static void h_sltu(Cpu& c, Bus&, const DecodedInstr& i);
    static void h_slti(Cpu& c, Bus&, const DecodedInstr& i);
    static void h_sltiu(Cpu& c, Bus&, const DecodedInstr& i);
    static void h_andi(Cpu& c, Bus&, const DecodedInstr& i);
    static void h_ori(Cpu& c, Bus&, const DecodedInstr& i);
    static void h_xori(Cpu& c, Bus&, const DecodedInstr& i);
    static void h_lui(Cpu& c, Bus&, const DecodedInstr& i);
    static void h_sll(Cpu& c, Bus&, const DecodedInstr& i);
    static void h_srl(Cpu& c, Bus&, const DecodedInstr& i);
    static void h_sra(Cpu& c, Bus&, const DecodedInstr& i);
    static void h_sllv(Cpu& c, Bus&, const DecodedInstr& i);
    static void h_srlv(Cpu& c, Bus&, const DecodedInstr& i);
    static void h_srav(Cpu& c, Bus&, const DecodedInstr& i);
    static void h_dsllv(Cpu& c, Bus&, const DecodedInstr& i);
    static void h_dsrlv(Cpu& c, Bus&, const DecodedInstr& i);
    static void h_dsrav(Cpu& c, Bus&, const DecodedInstr& i);
    static void h_daddiu(Cpu& c, Bus&, const DecodedInstr& i);
    static void h_daddi(Cpu& c, Bus&, const DecodedInstr& i);
    static void h_daddu(Cpu& c, Bus&, const DecodedInstr& i);
    static void h_dadd(Cpu& c, Bus&, const DecodedInstr& i);
    static void h_dsubu(Cpu& c, Bus&, const DecodedInstr& i);
    static void h_dsub(Cpu& c, Bus&, const DecodedInstr& i);
    static void h_dsll(Cpu& c, Bus&, const DecodedInstr& i);
    static void h_dsrl(Cpu& c, Bus&, const DecodedInstr& i);
    static void h_dsra(Cpu& c, Bus&, const DecodedInstr& i);
    static void h_dsll32(Cpu& c, Bus&, const DecodedInstr& i);
    static void h_dsrl32(Cpu& c, Bus&, const DecodedInstr& i);
    static void h_dsra32(Cpu& c, Bus&, const DecodedInstr& i);
    static void h_mult(Cpu& c, Bus&, const DecodedInstr& i);
    static void h_multu(Cpu& c, Bus&, const DecodedInstr& i);
    static void h_div(Cpu& c, Bus&, const DecodedInstr& i);
    static void h_divu(Cpu& c, Bus&, const DecodedInstr& i);
    static void h_dmult(Cpu& c, Bus&, const DecodedInstr& i);
    static void h_dmultu(Cpu& c, Bus&, const DecodedInstr& i);
    static void h_ddiv(Cpu& c, Bus&, const DecodedInstr& i);
    static void h_ddivu(Cpu& c, Bus&, const DecodedInstr& i);
    static void h_mfhi(Cpu& c, Bus&, const DecodedInstr& i);
    static void h_mthi(Cpu& c, Bus&, const DecodedInstr& i);
    static void h_mflo(Cpu& c, Bus&, const DecodedInstr& i);
    static void h_mtlo(Cpu& c, Bus&, const DecodedInstr& i);
    static void h_j(Cpu& c, Bus&, const DecodedInstr& i);
    static void h_jal(Cpu& c, Bus&, const DecodedInstr& i);
    static void h_jr(Cpu& c, Bus&, const DecodedInstr& i);
    static void h_jalr(Cpu& c, Bus&, const DecodedInstr& i);
    static void h_beq(Cpu& c, Bus&, const DecodedInstr& i);
    static void h_bne(Cpu& c, Bus&, const DecodedInstr& i);
    static void h_blez(Cpu& c, Bus&, const DecodedInstr& i);
    static void h_bgtz(Cpu& c, Bus&, const DecodedInstr& i);
    static void h_beql(Cpu& c, Bus&, const DecodedInstr& i);
    static void h_bnel(Cpu& c, Bus&, const DecodedInstr& i);
    static void h_blezr(Cpu& c, Bus&, const DecodedInstr& i);
    static void h_bgtzl(Cpu& c, Bus&, const DecodedInstr& i);
    static void h_bltz(Cpu& c, Bus&, const DecodedInstr& i);
    static void h_bgez(Cpu& c, Bus&, const DecodedInstr& i);
    static void h_bltzal(Cpu& c, Bus&, const DecodedInstr& i);
    static void h_bgezal(Cpu& c, Bus&, const DecodedInstr& i);
    static void h_bltzl(Cpu& c, Bus&, const DecodedInstr& i);
    static void h_bgezl(Cpu& c, Bus&, const DecodedInstr& i);
    static void h_bltzall(Cpu& c, Bus&, const DecodedInstr& i);
    static void h_bgezall(Cpu& c, Bus&, const DecodedInstr& i);
    static void h_regimm(Cpu& c, Bus&, const DecodedInstr& i);

    static void h_lb(Cpu& c, Bus& bus, const DecodedInstr& i);
    static void h_lh(Cpu& c, Bus& bus, const DecodedInstr& i);
    static void h_lw(Cpu& c, Bus& bus, const DecodedInstr& i);
    static void h_lbu(Cpu& c, Bus& bus, const DecodedInstr& i);
    static void h_lhu(Cpu& c, Bus& bus, const DecodedInstr& i);
    static void h_lwu(Cpu& c, Bus& bus, const DecodedInstr& i);
    static void h_ld(Cpu& c, Bus& bus, const DecodedInstr& i);
    static void h_lwl(Cpu& c, Bus& bus, const DecodedInstr& i);
    static void h_lwr(Cpu& c, Bus& bus, const DecodedInstr& i);
    static void h_ldl(Cpu& c, Bus& bus, const DecodedInstr& i);
    static void h_ldr(Cpu& c, Bus& bus, const DecodedInstr& i);
    static void h_sb(Cpu& c, Bus& bus, const DecodedInstr& i);
    static void h_sh(Cpu& c, Bus& bus, const DecodedInstr& i);
    static void h_sw(Cpu& c, Bus& bus, const DecodedInstr& i);
    static void h_sd(Cpu& c, Bus& bus, const DecodedInstr& i);
    static void h_swl(Cpu& c, Bus& bus, const DecodedInstr& i);
    static void h_swr(Cpu& c, Bus& bus, const DecodedInstr& i);
    static void h_sdl(Cpu& c, Bus& bus, const DecodedInstr& i);
    static void h_sdr(Cpu& c, Bus& bus, const DecodedInstr& i);

    // Block cache: keyed by physical PC. Each cached block records the
    // generation of every 64-byte RDRAM line it occupies, so a write to any
    // line that holds part of the block forces a recompile.
    static constexpr u32 BlockCacheLines = Memory::RdramSize / 64;
    static constexpr u32 BlockCacheSize = 4096;
    static constexpr u32 MaxBlockLines = 6;
    struct Block {
        u32 physPc = 0;
        u16 length = 0;
        u8 lineCount = 0;
        std::array<u32, MaxBlockLines> lineIdx{};
        std::array<u32, MaxBlockLines> lineGen{};
        std::array<DecodedInstr, 64> instrs{};
        Block* staticSuccessor = nullptr;
    };
    Block* activeBlock_ = nullptr;
    [[nodiscard]] const DecodedInstr& cachedInstruction(Bus& bus);
    std::vector<Block> blocks_;
    std::vector<std::atomic<u32>> lineGen_;
    std::atomic<u64> lineEpoch_{0};
    Bus* attachedBus_ = nullptr;
    CacheStats cacheStats_{};
    u64 instructionsDispatched_ = 0;
    std::unique_ptr<Arm64Jit> jit_;
    std::unique_ptr<X64Jit> x64Jit_;
    u64 jitSteps_ = 0;
    u64 jitInvocations_ = 0;
    ExceptionTrace exceptionTrace_{};

    struct JitResult { u32 retired; u64 pc; };
    [[nodiscard]] bool hasJit() const noexcept;
    [[nodiscard]] JitResult runJit(const Memory& ram, u64 pc, u64 epoch,
                                   const std::atomic<u32>* lineGen, u32 budget, bool cop1,
                                   bool fullFpr, Bus& bus) noexcept;
    void invalidateJitCode(u32 physicalAddress, u32 size) noexcept;
    void invalidateJitAll() noexcept;

    static bool isDirectFetch(u64 vaddr) noexcept;
    static u32 fetchDirect(u64 vaddr, const Bus& bus) noexcept;

    [[nodiscard]] u32 fprPhys(u32 index) const noexcept;
    [[nodiscard]] u32 fprWord(u32 index) const noexcept;
    void setFprWord(u32 index, u32 value) noexcept;
    [[nodiscard]] bool requireCop1();
    [[nodiscard]] float getFprS(u32 index) const noexcept;
    [[nodiscard]] double getFprD(u32 index) const noexcept;
    void setFprS(u32 index, float value) noexcept;
    void setFprD(u32 index, double value) noexcept;

    std::array<u64, 32> gpr_{};
    u64 pc_ = 0xFFFFFFFFA4000040ULL;
    u64 nextPc_ = 0xFFFFFFFFA4000044ULL;
    u64 branchTarget_ = 0;
    u64 hi_ = 0;
    u64 lo_ = 0;
    std::array<u64, 32> cop0_{};
    std::array<TlbEntry, 32> tlb_{};
    int tlbRandom_ = 31;

    std::array<u64, 32> fpr_{};
    u64 cop2Latch_ = 0;
    u32 fcr0_ = 0x500;
    u32 fcr31_ = 0;

    bool delaySlot_ = false;
    bool exceptionOrEretTaken_ = false;
    bool llBit_ = false;
    bool countHalfCycle_ = false;

};

} // namespace n64forge
