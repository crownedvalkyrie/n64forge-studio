#include "n64forge/core/cpu.hpp"
#include "n64forge/core/wide_product.hpp"
#include "jit_arm64.hpp"
#include "jit_x64.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>

namespace n64forge {

namespace {

// COP0 register indices
constexpr u32 C0_INDEX = 0;
constexpr u32 C0_ENTRYLO0 = 2;
constexpr u32 C0_ENTRYLO1 = 3;
constexpr u32 C0_CONTEXT = 4;
constexpr u32 C0_PAGEMASK = 5;
constexpr u32 C0_WIRED = 6;
constexpr u32 C0_BADVADDR = 8;
constexpr u32 C0_XCONTEXT = 20;
constexpr u32 C0_COUNT = 9;
constexpr u32 C0_ENTRYHI = 10;
constexpr u32 C0_COMPARE = 11;
constexpr u32 C0_STATUS = 12;
constexpr u32 C0_CAUSE = 13;
constexpr u32 C0_EPC = 14;
constexpr u32 C0_PRID = 15;
constexpr u32 C0_CONFIG = 16;
constexpr u32 C0_LLADDR = 17;
constexpr u32 C0_ERROREPC = 30;

// FCSR cause/flag bits used by COP1 conversions and signaling compares.
constexpr u32 FcsrCauseMask = 0x3F000;
constexpr u32 FcsrCauseInvalid = 1u << 16;
constexpr u32 FcsrFlagInvalid = 1u << 6;

// MIPS III COP1 compare predicates. Bit i of resultMask is the condition
// result for the corresponding FpOrder. Signaling predicates (funct 0x38-0x3F)
// raise Invalid Operation when either operand is NaN.
enum class FpOrder : u32 { Unordered, Less, Equal, Greater };
struct FpCompareInfo {
    u8 resultMask;
    bool signaling;
};
constexpr std::array<FpCompareInfo, 16> FpCompareTable{{
    {0b0000, false}, // 0x30 C.F
    {0b0001, false}, // 0x31 C.UN
    {0b0100, false}, // 0x32 C.EQ
    {0b0101, false}, // 0x33 C.UEQ
    {0b0010, false}, // 0x34 C.OLT
    {0b0011, false}, // 0x35 C.ULT
    {0b0110, false}, // 0x36 C.OLE
    {0b0111, false}, // 0x37 C.ULE
    {0b0000, true }, // 0x38 C.SF
    {0b0011, true }, // 0x39 C.NGLE
    {0b0100, true }, // 0x3A C.SEQ
    {0b0101, true }, // 0x3B C.NGL
    {0b0010, true }, // 0x3C C.LT
    {0b0011, true }, // 0x3D C.NGE
    {0b0110, true }, // 0x3E C.LE
    {0b0111, true }, // 0x3F C.NGT
}};

template<class T>
[[nodiscard]] FpOrder fpOrder(T a, T b) noexcept {
    if (std::isnan(a) || std::isnan(b)) return FpOrder::Unordered;
    if (a < b) return FpOrder::Less;
    if (a > b) return FpOrder::Greater;
    return FpOrder::Equal;
}

struct FpCompareResult {
    bool condition;
    bool invalid;
};

template<class T>
[[nodiscard]] FpCompareResult fpCompare(T a, T b, u32 funct) noexcept {
    const auto& info = FpCompareTable[funct & 0x0F];
    const FpOrder order = fpOrder(a, b);
    return {
        (info.resultMask & (1u << static_cast<u32>(order))) != 0,
        info.signaling && order == FpOrder::Unordered,
    };
}

[[nodiscard]] u32 lower32(u64 v) noexcept { return static_cast<u32>(v); }

[[nodiscard]] u64 signExtend32(u64 v) noexcept {
    return static_cast<s64>(static_cast<s32>(v));
}

[[nodiscard]] u64 shiftWordArithmetic(u64 value, u32 amount) noexcept {
    // VR4300 shifts the full source register before sign-extending the word.
    return signExtend32(static_cast<s64>(value) >> amount);
}

template<class T>
void divide(T n, T d, u64& lo, u64& hi) noexcept {
    const auto extend = [](T value) -> u64 {
        if constexpr (sizeof(T) == 4) return signExtend32(value);
        else return static_cast<u64>(value);
    };
    if (!d) {
        lo = ~u64{0};
        if constexpr (std::numeric_limits<T>::is_signed)
            if (n < 0) lo = 1;
        hi = extend(n);
        return;
    }
    if constexpr (std::numeric_limits<T>::is_signed) {
        if (n == std::numeric_limits<T>::min() && d == -1) {
            lo = extend(n);
            hi = 0;
            return;
        }
    }
    lo = extend(n / d);
    hi = extend(n % d);
}

// Portable 64x64 -> 128 signed arithmetic. Wide values are kept as two limbs
// so overflow is decided before anything is narrowed to a C++ signed type;
// this avoids performing a signed add/subtract in the narrow type (UB) and
// avoids __int128 entirely (MSVC has no such type).
struct WideSigned {
    u64 lo = 0;
    s64 hi = 0;
};

[[nodiscard]] WideSigned wideAdd(s64 a, s64 b) noexcept {
    const u64 ua = static_cast<u64>(a), ub = static_cast<u64>(b);
    const u64 lo = ua + ub;
    const u64 signA = a < 0 ? ~u64{0} : u64{0};
    const u64 signB = b < 0 ? ~u64{0} : u64{0};
    const u64 carry = lo < ua ? u64{1} : u64{0};
    return {lo, std::bit_cast<s64>(signA + signB + carry)};
}

[[nodiscard]] WideSigned wideSub(s64 a, s64 b) noexcept {
    const u64 ua = static_cast<u64>(a), ub = static_cast<u64>(b);
    const u64 lo = ua - ub;
    const u64 signA = a < 0 ? ~u64{0} : u64{0};
    const u64 signB = b < 0 ? ~u64{0} : u64{0};
    const u64 borrow = ua < ub ? u64{1} : u64{0};
    return {lo, std::bit_cast<s64>(signA - signB - borrow)};
}

[[nodiscard]] bool wideFitsS64(const WideSigned& value) noexcept {
    const s64 signExtension = (value.lo >> 63) ? s64{-1} : s64{0};
    return value.hi == signExtension;
}

[[nodiscard]] s64 wideValue(const WideSigned& value) noexcept {
    return std::bit_cast<s64>(value.lo);
}

// 32-bit operands are widened to s64, whose range cannot overflow for a
// single add or subtract of two s32 values.
[[nodiscard]] bool checkedAdd32(s32 a, s32 b, s32& out) noexcept {
    const s64 wide = static_cast<s64>(a) + static_cast<s64>(b);
    if (wide < std::numeric_limits<s32>::min() || wide > std::numeric_limits<s32>::max())
        return false;
    out = static_cast<s32>(wide);
    return true;
}

[[nodiscard]] bool checkedSub32(s32 a, s32 b, s32& out) noexcept {
    const s64 wide = static_cast<s64>(a) - static_cast<s64>(b);
    if (wide < std::numeric_limits<s32>::min() || wide > std::numeric_limits<s32>::max())
        return false;
    out = static_cast<s32>(wide);
    return true;
}

[[nodiscard]] bool checkedAdd64(s64 a, s64 b, s64& out) noexcept {
    const WideSigned wide = wideAdd(a, b);
    if (!wideFitsS64(wide)) return false;
    out = wideValue(wide);
    return true;
}

[[nodiscard]] bool checkedSub64(s64 a, s64 b, s64& out) noexcept {
    const WideSigned wide = wideSub(a, b);
    if (!wideFitsS64(wide)) return false;
    out = wideValue(wide);
    return true;
}

} // namespace

AccessException::AccessException(u32 causeCode, const std::string& message, bool refill)
    : std::runtime_error(message), causeCode_(causeCode), refill_(refill) {}

Cpu::Cpu() : blocks_(BlockCacheSize), lineGen_(BlockCacheLines) { reset(); }
Cpu::~Cpu() = default;

bool Cpu::setJitEnabled(bool enabled) {
    if (!enabled) {
        jit_.reset();
        x64Jit_.reset();
        return true;
    }
#if defined(_M_X64) || (defined(__x86_64__) && !defined(__arm64e__))
    if (!x64Jit_) x64Jit_ = X64Jit::create();
    return bool(x64Jit_);
#else
    if (!jit_) jit_ = Arm64Jit::create();
    return bool(jit_);
#endif
}

bool Cpu::hasJit() const noexcept { return bool(jit_) || bool(x64Jit_); }

Cpu::JitResult Cpu::runJit(const Memory& ram, u64 pc, u64 epoch,
                           const std::atomic<u32>* lineGen, u32 budget, bool cop1,
                           bool fullFpr, Bus& bus) noexcept {
    if (x64Jit_) {
        const auto native = x64Jit_->run(ram, pc, epoch, lineGen, budget, gpr_.data(), &hi_, &lo_,
                                         fpr_.data(), &fcr31_, cop1, fullFpr, this, &bus);
        return {native.retired, native.pc};
    }
    if (jit_) {
        const auto native = jit_->run(ram, pc, epoch, lineGen, budget, gpr_.data(), &hi_, &lo_,
                                      fpr_.data(), &fcr31_, cop1, fullFpr);
        return {native.retired, native.pc};
    }
    return {0, pc};
}

void Cpu::invalidateJitCode(u32 physicalAddress, u32 size) noexcept {
    if (jit_) jit_->invalidateCode(physicalAddress, size);
    if (x64Jit_) x64Jit_->invalidateCode(physicalAddress, size);
}

void Cpu::invalidateJitAll() noexcept {
    if (jit_) jit_->invalidateAll();
    if (x64Jit_) x64Jit_->invalidateAll();
}

bool Cpu::stepJitInstruction(Bus& bus, u64 requestedPc) noexcept {
    const auto sameTrace = [](const ExceptionTrace& a, const ExceptionTrace& b) noexcept {
        return a.counts == b.counts && a.lastPcByCode == b.lastPcByCode &&
               a.firstPc == b.firstPc && a.lastPc == b.lastPc &&
               a.firstCode == b.firstCode && a.lastCode == b.lastCode &&
               a.observed == b.observed;
    };
    struct Snapshot {
        std::array<u64, 32> gpr;
        std::array<u64, 32> cop0;
        std::array<TlbEntry, 32> tlb;
        std::array<u64, 32> fpr;
        ExceptionTrace exceptions;
        u64 pc, nextPc, branchTarget, hi, lo, instructions, cop2Latch;
        u32 fcr31, fcr0;
        int tlbRandom;
        bool delaySlot, exceptionTaken, llBit, countHalf;
    } before{
        gpr_, cop0_, tlb_, fpr_, exceptionTrace_,
        pc_, nextPc_, branchTarget_, hi_, lo_, instructionsDispatched_, cop2Latch_,
        fcr31_, fcr0_, tlbRandom_,
        delaySlot_, exceptionOrEretTaken_, llBit_, countHalfCycle_};

    // Keep delay-slot ownership intact: the generated block calls this entry
    // once for the branch and again for its delay instruction. Re-seeding only
    // the PC lets the existing state machine retire the pair exactly once.
    pc_ = requestedPc;
    nextPc_ = requestedPc + 4;

    const auto restore = [&]() noexcept {
        gpr_ = before.gpr;
        cop0_ = before.cop0;
        tlb_ = before.tlb;
        fpr_ = before.fpr;
        exceptionTrace_ = before.exceptions;
        pc_ = before.pc;
        nextPc_ = before.nextPc;
        branchTarget_ = before.branchTarget;
        hi_ = before.hi;
        lo_ = before.lo;
        instructionsDispatched_ = before.instructions;
        fcr31_ = before.fcr31;
        fcr0_ = before.fcr0;
        cop2Latch_ = before.cop2Latch;
        tlbRandom_ = before.tlbRandom;
        delaySlot_ = before.delaySlot;
        exceptionOrEretTaken_ = before.exceptionTaken;
        llBit_ = before.llBit;
        countHalfCycle_ = before.countHalf;
    };

    try {
        step(bus);
    } catch (...) {
        restore();
        return false;
    }
    if (instructionsDispatched_ == before.instructions ||
        !sameTrace(exceptionTrace_, before.exceptions)) {
        // An interrupt or guest exception was taken before/at this instruction.
        // Let the scalar CPU retry it so EXL, EPC and Cause are exact.
        restore();
        return false;
    }
    // The generated block reports retired instructions back to
    // Cpu::runQuietBlock, which owns timing and dispatch accounting. Keep the
    // architectural effects of the scalar step but undo its per-instruction
    // counters so they are not applied twice.
    instructionsDispatched_ = before.instructions;
    cop0_[C0_COUNT] = before.cop0[C0_COUNT];
    cop0_[C0_CAUSE] = before.cop0[C0_CAUSE];
    countHalfCycle_ = before.countHalf;
    return true;
}

void Cpu::reset() noexcept {
    instructionsDispatched_ = 0;
    jitSteps_ = 0;
    jitInvocations_ = 0;
    exceptionTrace_ = {};
    cacheStats_ = {};
    gpr_.fill(0);
    hi_ = 0;
    lo_ = 0;
    cop0_.fill(0);
    cop0_[C0_PRID] = 0x00000B22;
    cop0_[C0_CONFIG] = 0x7006E463;
    cop0_[C0_STATUS] = 0x241000E0;
    tlb_.fill(TlbEntry{});
    clearMappedPages();
    tlbRandom_ = 31;
    fpr_.fill(0);
    cop2Latch_ = 0;
    fcr0_ = 0x500;
    fcr31_ = 0;
    llBit_ = false;
    countHalfCycle_ = false;
    pc_ = 0xFFFFFFFFA4000040ULL;
    nextPc_ = pc_ + 4;
    branchTarget_ = 0;
    delaySlot_ = false;
    exceptionOrEretTaken_ = false;
    clearBlockCache();
}

void Cpu::writeGpr(u32 index, u64 value) noexcept {
    if (index != 0) gpr_[index] = value;
}

u64 Cpu::translateAddress(u64 vaddr, bool write) {
    const u64 low = vaddr & 0xFFFFFFFFULL;
    // KSEG0 (cached) and KSEG1 (uncached) are identity-mapped with the low 29 bits.
    if ((low >= 0x80000000ULL && low <= 0x9FFFFFFFULL) ||
        (low >= 0xA0000000ULL && low <= 0xBFFFFFFFULL)) {
        return low & 0x1FFFFFFFULL;
    }
    // Error level maps KUSEG to uncached physical addresses; normal KUSEG
    // accesses perform a TLB lookup just like the other mapped segments.
    if (vaddr < 0x80000000ULL && (cop0_[C0_STATUS] & 4)) {
        return vaddr;
    }
    // All other regions require the TLB (KUSEG, KSSEG, KSEG3 and their sign-extended forms).
    return translateAddressTlb(vaddr, write);
}

void Cpu::latchFaultAddress(u64 address) noexcept {
    cop0_[C0_BADVADDR] = address;
    cop0_[C0_CONTEXT] = (cop0_[C0_CONTEXT] & ~u64{0x7FFFFF}) | ((address >> 9) & 0x7FFFF0);
    cop0_[C0_XCONTEXT] = (cop0_[C0_XCONTEXT] & ~u64{0x1FFFFFFFF}) |
                        ((address >> 9) & 0x7FFFFFF0) | ((address >> 62) << 31);
}

void Cpu::addressError(u64 address, bool write) {
    latchFaultAddress(address);
    throw AccessException(write ? 5 : 4, "Cpu: unaligned access");
}

u64 Cpu::translateAddressTlb(u64 vaddr, bool write, bool* writable) {
    const u32 asid = static_cast<u32>(cop0_[C0_ENTRYHI] & 0xFF);
    const u32 vpn2 = static_cast<u32>((vaddr >> 13) & 0x7FFFFFF);
    const u32 region = static_cast<u32>(vaddr >> 62);
    const auto fault = [&](u32 code, const char* message, bool refill = false) -> u64 {
        latchFaultAddress(vaddr);
        cop0_[C0_ENTRYHI] = (cop0_[C0_ENTRYHI] & ~u64{0xC00000FFFFFFE000}) |
                            (vaddr & 0xC00000FFFFFFE000ULL);
        throw AccessException(code, message, refill);
    };

    const u32 code = write ? 3 : 2;
    for (const auto& e : tlb_) {
        if (!e.written || e.region != region || ((e.vpn2 ^ vpn2) & ~e.pageMask) != 0) continue;
        if (!e.global && e.asid != asid) continue;

        const u32 mask = (e.pageMask << 12) | 0xFFF;
        const int side = (vaddr & (mask + 1)) ? 1 : 0;
        if (!e.valid[side]) return fault(code, "Cpu: TLB invalid");
        if (write && !e.dirty[side]) return fault(1, "Cpu: TLB modified");
        if (writable) *writable = e.dirty[side];
        return ((u64{e.pfn[side]} << 12) & ~u64{mask}) | (vaddr & mask);
    }
    return fault(code, "Cpu: TLB miss", true);
}

u32 Cpu::resolveMemory(u64 address, bool write) {
    // The hot KSEG0/KSEG1 case is handled by directRdram before this call.
    // Cache canonically sign-extended mapped RDRAM pages. MMIO, extended
    // addresses and ERL's unmapped KUSEG retain the slow translation path.
    const u32 low = static_cast<u32>(address);
    const bool kuseg = low < 0x80000000;
    if (address != signExtend32(low) || (!kuseg && low < 0xC0000000) ||
        (kuseg && (cop0_[C0_STATUS] & 4)))
        return static_cast<u32>(translateAddress(address, write));
    const u64 page = address >> 12;
    const u32 offset = static_cast<u32>(address) & 0xFFF;
    auto& entry = mappedPages_[page % mappedPages_.size()];
    if (entry.tag == page && (!write || entry.writable)) return entry.base + offset;
    bool writable = false;
    const u64 phys = translateAddressTlb(address, write, &writable);
    if (phys < Memory::RdramSize) entry = {page, static_cast<u32>(phys) - offset, writable};
    return static_cast<u32>(phys);
}

namespace {

template<class T> bool directRdram(u64 address) noexcept {
    // Two fixed direct-map windows: no table or cached host pointers to
    // invalidate on reset. Noncanonical addresses use the existing slow path.
    constexpr u32 mask = ~(Memory::RdramSize - 1u) & ~0x20000000u;
    const u32 low = static_cast<u32>(address);
    return address == signExtend32(low) && (low & mask) == 0x80000000u &&
           !(low & (sizeof(T) - 1));
}

} // namespace

template<class T> T Cpu::load(Bus& bus, u64 address) {
    if (directRdram<T>(address))
        return bus.rdram().loadRdram<T>(address & (Memory::RdramSize - 1));
    if (address & (sizeof(T) - 1)) addressError(address, false);
    const u32 phys = resolveMemory(address, false);
    if (phys < Memory::RdramSize) return bus.rdram().loadRdram<T>(phys);
    if constexpr (sizeof(T) == 1) return bus.read8(phys);
    if constexpr (sizeof(T) == 2) return bus.read16(phys);
    if constexpr (sizeof(T) == 4) return bus.read32(phys);
    if constexpr (sizeof(T) == 8) return bus.read64(phys);
}

template<class T> void Cpu::store(Bus& bus, u64 address, T value) {
    if (directRdram<T>(address)) {
        bus.rdram().storeRdram<T>(address & (Memory::RdramSize - 1), value);
        return;
    }
    if (address & (sizeof(T) - 1)) addressError(address, true);
    const u32 phys = resolveMemory(address, true);
    if (phys < Memory::RdramSize) {
        bus.rdram().storeRdram<T>(phys, value);
        return;
    }
    if constexpr (sizeof(T) == 1) bus.write8(phys, value);
    if constexpr (sizeof(T) == 2) bus.write16(phys, value);
    if constexpr (sizeof(T) == 4) bus.write32(phys, value);
    if constexpr (sizeof(T) == 8) bus.write64(phys, value);
}

u32 Cpu::fetch(Bus& bus) {
    return load<u32>(bus, pc_);
}

void Cpu::raiseException(u32 code, bool refill, u32 coprocessor) {
    code &= 31;
    ++exceptionTrace_.counts[code];
    exceptionTrace_.lastPcByCode[code] = pc_;
    if (!exceptionTrace_.observed) {
        exceptionTrace_.observed = true;
        exceptionTrace_.firstPc = pc_;
        exceptionTrace_.firstCode = code;
    }
    exceptionTrace_.lastPc = pc_;
    exceptionTrace_.lastCode = code;
    const u64 epc = delaySlot_ ? pc_ - 4 : pc_;
    const auto status = cop0_[C0_STATUS];
    cop0_[C0_CAUSE] = (cop0_[C0_CAUSE] & ~0x3000007CULL) |
                     (u64{coprocessor} << 28) | (u64{code} << 2);
    if (!(status & 2)) {
        cop0_[C0_EPC] = epc;
        cop0_[C0_CAUSE] = (cop0_[C0_CAUSE] & ~(1ULL << 31)) | (u64{delaySlot_} << 31);
    }
    cop0_[C0_STATUS] |= (1ULL << 1); // EXL

    const bool bev = status & (1ULL << 22);
    branchTarget_ = refill && !(status & 2) ?
        (bev ? 0xFFFFFFFFBFC00200ULL : 0xFFFFFFFF80000000ULL) :
        (bev ? 0xFFFFFFFFBFC00380ULL : 0xFFFFFFFF80000180ULL);
    exceptionOrEretTaken_ = true;
    delaySlot_ = false;
}

void Cpu::cop0Execute(u32 instr, u32 rs, u32 rt, u32 rd, u32 funct) {
    constexpr u64 EntryHiMask = 0xC00000FFFFFFE0FFULL;
    const auto writeTlb = [&](u32 idx) {
        clearMappedPages();
        // Decoded blocks use physical PCs; mapped instruction fetches always
        // translate afresh. Drop the active direct-block cursor at the barrier.
        activeBlock_ = nullptr;
        // Native code and native chains only use direct segments. Mapped
        // accesses fall back and observe the updated page map.
        auto& e = tlb_[idx];
        const auto hi = cop0_[C0_ENTRYHI];
        e.written = true;
        e.pageMask = static_cast<u32>((cop0_[C0_PAGEMASK] >> 13) & 0xFFF);
        e.vpn2 = static_cast<u32>((hi >> 13) & 0x7FFFFFF);
        e.region = static_cast<u32>(hi >> 62);
        e.asid = static_cast<u32>(hi & 0xFF);
        const u64 lo[] = {cop0_[C0_ENTRYLO0], cop0_[C0_ENTRYLO1]};
        e.global = (lo[0] & lo[1] & 1) != 0;
        for (int i = 0; i < 2; ++i) {
            e.pfn[i] = static_cast<u32>((lo[i] >> 6) & 0xFFFFFF);
            e.dirty[i] = (lo[i] & 4) != 0;
            e.valid[i] = (lo[i] & 2) != 0;
        }
    };
    switch (rs) {
        case 0x00: // MFC0
            writeGpr(rt, signExtend32(cop0_[rd]));
            break;
        case 0x01: // DMFC0
            writeGpr(rt, cop0_[rd]);
            break;
        case 0x04: // MTC0
            // VR4300 EntryHi consumes region and extended VPN bits from the
            // full source register even with the MTC0 encoding.
            cop0_[rd] = rd == C0_ENTRYHI ? gpr_[rt] & EntryHiMask :
                         (rd == C0_EPC || rd == C0_ERROREPC) ? gpr_[rt] :
                         (cop0_[rd] & ~0xFFFFFFFFULL) | (gpr_[rt] & 0xFFFFFFFFULL);
            if (rd == C0_COMPARE) cop0_[C0_CAUSE] &= ~0x8000ULL;
            if (rd == C0_ENTRYHI || rd == C0_ENTRYLO0 || rd == C0_ENTRYLO1 || rd == C0_STATUS)
                clearMappedPages();
            break;
        case 0x05: // DMTC0
            cop0_[rd] = rd == C0_ENTRYHI ? gpr_[rt] & EntryHiMask : gpr_[rt];
            if (rd == C0_COMPARE) cop0_[C0_CAUSE] &= ~0x8000ULL;
            if (rd == C0_ENTRYHI || rd == C0_ENTRYLO0 || rd == C0_ENTRYLO1 || rd == C0_STATUS)
                clearMappedPages();
            break;
        case 0x08: { // BC (ERET is encoded via rs=0x10, not here)
            // COP0 has no defined BC instructions; treat as RI.
            raiseException(10);
            return;
        }
        case 0x10: { // COP0 co-processor functions (TLB/ERET)
            switch (funct) {
                case 0x01: { // TLBR
                    clearMappedPages();
                    const u32 idx = static_cast<u32>(cop0_[C0_INDEX]) & 0x1F;
                    const auto& e = tlb_[idx];
                    cop0_[C0_PAGEMASK] = (static_cast<u64>(e.pageMask) << 13);
                    cop0_[C0_ENTRYHI] =
                        (u64{e.region} << 62) | (u64{e.vpn2} << 13) | e.asid;
                    for (int i = 0; i < 2; ++i) {
                        u64 lo = (static_cast<u64>(e.pfn[i]) << 6) |
                                 (e.dirty[i] ? 0x4ULL : 0) | (e.valid[i] ? 0x2ULL : 0) |
                                 (e.global ? 0x1ULL : 0);
                        if (i == 0) cop0_[C0_ENTRYLO0] = lo;
                        else cop0_[C0_ENTRYLO1] = lo;
                    }
                    break;
                }
                case 0x02: { // TLBWI
                    writeTlb(static_cast<u32>(cop0_[C0_INDEX]) & 31);
                    break;
                }
                case 0x06: { // TLBWR
                    writeTlb(static_cast<u32>(tlbRandom_) & 31);
                    // Random decrements down to Wired + 1 then wraps to 31.
                    const u32 wired = static_cast<u32>(cop0_[C0_WIRED]) & 0x1F;
                    if (--tlbRandom_ < static_cast<int>(wired) + 1) tlbRandom_ = 31;
                    break;
                }
                case 0x08: { // TLBP
                    const u32 asid = static_cast<u32>(cop0_[C0_ENTRYHI] & 0xFF);
                    const u32 vpn2 = static_cast<u32>((cop0_[C0_ENTRYHI] >> 13) & 0x7FFFFFF);
                    const u32 region = static_cast<u32>(cop0_[C0_ENTRYHI] >> 62);
                    bool found = false;
                    for (size_t i = 0; i < tlb_.size(); ++i) {
                        const auto& e = tlb_[i];
                        if (!e.written || e.region != region || ((e.vpn2 ^ vpn2) & ~e.pageMask) != 0) continue;
                        if (!e.global && e.asid != asid) continue;
                        cop0_[C0_INDEX] = static_cast<u64>(i);
                        found = true;
                        break;
                    }
                    if (!found) cop0_[C0_INDEX] = 0x80000000ULL;
                    break;
                }
                case 0x18: { // ERET
                    clearMappedPages();
                    llBit_ = false;
                    if (cop0_[C0_STATUS] & (1ULL << 2)) {
                        branchTarget_ = cop0_[C0_ERROREPC];
                        cop0_[C0_STATUS] &= ~(1ULL << 2); // clear ERL
                    } else {
                        branchTarget_ = cop0_[C0_EPC];
                        cop0_[C0_STATUS] &= ~(1ULL << 1); // clear EXL
                    }
                    exceptionOrEretTaken_ = true;
                    delaySlot_ = false;
                    break;
                }
                default:
                    raiseException(10);
                    return;
            }
            break;
        }
        default:
            raiseException(10);
            return;
    }
}

u32 Cpu::fprPhys(u32 index) const noexcept {
    const bool fr = (cop0_[C0_STATUS] & (1ULL << 26)) != 0;
    return fr ? index : (index & ~1u);
}

u32 Cpu::fprWord(u32 index) const noexcept {
    const auto phys = fprPhys(index);
    return static_cast<u32>(fpr_[phys] >> (phys == index ? 0 : 32));
}

void Cpu::setFprWord(u32 index, u32 value) noexcept {
    const auto phys = fprPhys(index);
    const unsigned shift = phys == index ? 0 : 32;
    fpr_[phys] = (fpr_[phys] & ~(0xFFFFFFFFULL << shift)) | (static_cast<u64>(value) << shift);
}

float Cpu::getFprS(u32 index) const noexcept {
    const u32 raw = fprWord(index);
    float v = 0.0f;
    std::memcpy(&v, &raw, sizeof(v));
    return v;
}

double Cpu::getFprD(u32 index) const noexcept {
    const u64 raw = fpr_[fprPhys(index)];
    double v = 0.0;
    std::memcpy(&v, &raw, sizeof(v));
    return v;
}

void Cpu::setFprS(u32 index, float value) noexcept {
    u32 raw = 0;
    std::memcpy(&raw, &value, sizeof(raw));
    setFprWord(index, raw);
}

void Cpu::setFprD(u32 index, double value) noexcept {
    const u32 phys = fprPhys(index);
    u64 raw = 0;
    std::memcpy(&raw, &value, sizeof(raw));
    fpr_[phys] = raw;
}

bool Cpu::requireCop1() {
    if ((cop0_[C0_STATUS] & (1ULL << 29)) == 0) {
        raiseException(11, false, 1);
        return false;
    }
    return true;
}

void Cpu::cop1Execute(u32 instr, u32 rs, u32 rt, u32 rd, u32 funct) {
    if (!requireCop1()) return;

    const u32 fmt = rs; // fmt field lives in the normal rs position for COP1
    switch (rs) {
        case 0x00: { // MFC1
            const u32 raw = fprWord(rd);
            writeGpr(rt, signExtend32(raw));
            return;
        }
        case 0x01: { // DMFC1
            writeGpr(rt, fpr_[fprPhys(rd)]);
            return;
        }
        case 0x04: { // MTC1
            setFprWord(rd, static_cast<u32>(gpr_[rt]));
            return;
        }
        case 0x05: { // DMTC1
            fpr_[fprPhys(rd)] = gpr_[rt];
            return;
        }
        case 0x02: { // CFC1
            if (rd == 0) writeGpr(rt, signExtend32(fcr0_));
            else if (rd == 31) writeGpr(rt, signExtend32(fcr31_));
            return;
        }
        case 0x06: { // CTC1
            if (rd == 31) fcr31_ = static_cast<u32>(gpr_[rt]);
            return;
        }
        default:
            break;
    }

    // Branch (BC) within COP1: rs == 0x08, rt encodes the branch condition.
    if (rs == 0x08) {
        const bool cond = (fcr31_ & (1u << 23)) != 0;
        bool take = false;
        switch (rt & 3) {
            case 0: take = !cond; break; // BC1F
            case 1: take = cond;  break; // BC1T
            case 2: take = !cond; break; // BC1FL
            case 3: take = cond;  break; // BC1TL
        }
        if (take) {
            const s32 offset = static_cast<s16>(instr & 0xFFFF);
            branchTarget_ = pc_ + 4 + (static_cast<s64>(offset) << 2);
            delaySlot_ = true;
        } else if (rt & 2) {
            // Likely branch not taken: skip the delay slot.
            pc_ = nextPc_;
            nextPc_ = pc_ + 4;
        } else {
            branchTarget_ = pc_ + 8;
            delaySlot_ = true;
        }
        return;
    }

    // Compute / convert / compare.  MIPS COP1 R-type layout:
    //   fd = bits 6-10, fs = bits 11-15, ft = bits 16-20.
    const u32 fd = (instr >> 6) & 0x1F;
    const u32 fs = (instr >> 11) & 0x1F;
    const u32 ft = (instr >> 16) & 0x1F;

    // CVT.L.S / CVT.L.D: reject NaN, infinities and values outside the signed
    // 64-bit range before any host conversion. The indefinite integer pattern
    // gives invalid conversions one deterministic result on every host.
    const auto convertToLong = [this, fd](double value) {
        fcr31_ &= ~FcsrCauseMask;
        if (!std::isfinite(value) || value >= 0x1p63 || value < -0x1p63) {
            fcr31_ |= FcsrCauseInvalid | FcsrFlagInvalid;
            fpr_[fprPhys(fd)] = 0x8000000000000000ULL;
            raiseException(15); // FPU invalid operation
            return;
        }
        fpr_[fprPhys(fd)] = static_cast<u64>(static_cast<s64>(value));
    };

    const auto compare = [this, funct](auto a, auto b) {
        const FpCompareResult result = fpCompare(a, b, funct);
        if (result.condition) fcr31_ |= 1u << 23;
        else fcr31_ &= ~(1u << 23);
        if (result.invalid) {
            fcr31_ &= ~FcsrCauseMask;
            fcr31_ |= FcsrCauseInvalid | FcsrFlagInvalid;
            raiseException(15); // FPU invalid operation
        }
    };

    if ((fmt == 0x10 || fmt == 0x11) && ((funct >= 0x0C && funct <= 0x0F) || funct == 0x24)) {
        // ROUND/TRUNC/CEIL/FLOOR.W select an instruction-local rounding mode.
        // CVT.W uses FCR31.RM. Do not change the host thread's FP environment.
        const u32 rounding = funct == 0x24 ? fcr31_ & 3 : funct - 0x0C;
        const auto convert = [this, fd, rounding](auto operand) {
            constexpr u32 CauseMask = 0x3F000;
            constexpr u32 CauseUnimplemented = 1u << 17;
            constexpr u32 CauseInexact = 1u << 12;
            constexpr u32 EnableInexact = 1u << 7;
            constexpr u32 FlagInexact = 1u << 2;
            fcr31_ &= ~CauseMask;
            const double value = operand;
            if (!std::isfinite(value) || std::fpclassify(operand) == FP_SUBNORMAL ||
                value >= 0x1p31 || value < -0x1p31) {
                fcr31_ |= CauseUnimplemented;
                raiseException(15);
                return;
            }
            double rounded;
            switch (rounding) {
                case 1: rounded = std::trunc(value); break;
                case 2: rounded = std::ceil(value); break;
                case 3: rounded = std::floor(value); break;
                default: {
                    const double lower = std::floor(value);
                    const double fraction = value - lower;
                    rounded = lower + (fraction > 0.5 ||
                                       (fraction == 0.5 && (static_cast<s64>(lower) & 1)));
                    break;
                }
            }
            if (rounded >= 0x1p31 || rounded < -0x1p31) {
                fcr31_ |= CauseUnimplemented;
                raiseException(15);
                return;
            }
            if (value != rounded) {
                fcr31_ |= CauseInexact;
                if (fcr31_ & EnableInexact) {
                    raiseException(15);
                    return;
                }
                fcr31_ |= FlagInexact;
            }
            setFprWord(fd, static_cast<u32>(static_cast<s32>(rounded)));
        };
        if (fmt == 0x10) convert(getFprS(fs));
        else convert(getFprD(fs));
        return;
    }

    switch (fmt) {
        case 0x10: { // S (single)
            const float a = getFprS(fs);
            const float b = getFprS(ft);
            switch (funct) {
                case 0x00: setFprS(fd, a + b); break;
                case 0x01: setFprS(fd, a - b); break;
                case 0x02: setFprS(fd, a * b); break;
                case 0x03: setFprS(fd, a / b); break;
                case 0x04: setFprS(fd, std::sqrt(a)); break;
                case 0x05: setFprS(fd, std::fabs(a)); break;
                case 0x06: setFprS(fd, a); break;
                case 0x07: setFprS(fd, -a); break;
                case 0x21: // CVT.D.S
                    setFprD(fd, static_cast<double>(a));
                    break;
                case 0x25: // CVT.L.S
                    convertToLong(static_cast<double>(a));
                    break;
                default:
                    if ((funct & 0x30) == 0x30) compare(a, b);
                    else raiseException(10);
                    break;
            }
            break;
        }
        case 0x11: { // D (double)
            const double a = getFprD(fs);
            const double b = getFprD(ft);
            switch (funct) {
                case 0x00: setFprD(fd, a + b); break;
                case 0x01: setFprD(fd, a - b); break;
                case 0x02: setFprD(fd, a * b); break;
                case 0x03: setFprD(fd, a / b); break;
                case 0x04: setFprD(fd, std::sqrt(a)); break;
                case 0x05: setFprD(fd, std::fabs(a)); break;
                case 0x06: setFprD(fd, a); break;
                case 0x07: setFprD(fd, -a); break;
                case 0x20: setFprS(fd, static_cast<float>(a)); break; // CVT.S.D
                case 0x25: // CVT.L.D (store as raw s64 in lower 64 bits)
                    convertToLong(a);
                    break;
                default:
                    if ((funct & 0x30) == 0x30) compare(a, b);
                    else raiseException(10);
                    break;
            }
            break;
        }
        case 0x14: { // W (32-bit fixed point)
            const s32 a = static_cast<s32>(lower32(fpr_[fprPhys(fs)]));
            switch (funct) {
                case 0x20: setFprS(fd, static_cast<float>(a)); break; // CVT.S.W
                case 0x21: setFprD(fd, static_cast<double>(a)); break; // CVT.D.W
                default: raiseException(10); break;
            }
            break;
        }
        case 0x15: { // L (64-bit fixed point)
            const s64 a = static_cast<s64>(fpr_[fprPhys(fs)]);
            switch (funct) {
                case 0x20: setFprS(fd, static_cast<float>(a)); break; // CVT.S.L
                case 0x21: setFprD(fd, static_cast<double>(a)); break; // CVT.D.L
                default: raiseException(10); break;
            }
            break;
        }
        default:
            raiseException(10);
            break;
    }
}

// ---------------------------------------------------------------------------
// Fast instruction handlers and dispatch table for the block interpreter.
// Each handler mirrors the corresponding case in the original interpreter.
// ---------------------------------------------------------------------------

namespace {

// Per-opcode dispatch table.  For SPECIAL (op == 0) the second index is funct;
// for all other opcodes only index [op][0] is used.
using Handler = void (*)(Cpu&, Bus&, const Cpu::DecodedInstr&);
std::array<std::array<Handler, 64>, 64> g_handlers{};
struct HandlerInit {
    HandlerInit() {
        for (auto& row : g_handlers) row.fill(nullptr);
        auto& H = g_handlers;
        for (u32 op = 0; op < 64; ++op) H[op][0] = &Cpu::executeInstr;
        // SPECIAL
        H[0][0x00] = &Cpu::h_sll;
        H[0][0x02] = &Cpu::h_srl;
        H[0][0x03] = &Cpu::h_sra;
        H[0][0x04] = &Cpu::h_sllv;
        H[0][0x06] = &Cpu::h_srlv;
        H[0][0x07] = &Cpu::h_srav;
        H[0][0x08] = &Cpu::h_jr;
        H[0][0x09] = &Cpu::h_jalr;
        H[0][0x10] = &Cpu::h_mfhi;
        H[0][0x11] = &Cpu::h_mthi;
        H[0][0x12] = &Cpu::h_mflo;
        H[0][0x13] = &Cpu::h_mtlo;
        H[0][0x14] = &Cpu::h_dsllv;
        H[0][0x16] = &Cpu::h_dsrlv;
        H[0][0x17] = &Cpu::h_dsrav;
        H[0][0x18] = &Cpu::h_mult;
        H[0][0x19] = &Cpu::h_multu;
        H[0][0x1A] = &Cpu::h_div;
        H[0][0x1B] = &Cpu::h_divu;
        H[0][0x1C] = &Cpu::h_dmult;
        H[0][0x1D] = &Cpu::h_dmultu;
        H[0][0x1E] = &Cpu::h_ddiv;
        H[0][0x1F] = &Cpu::h_ddivu;
        H[0][0x20] = &Cpu::h_add;
        H[0][0x21] = &Cpu::h_addu;
        H[0][0x22] = &Cpu::h_sub;
        H[0][0x23] = &Cpu::h_subu;
        H[0][0x24] = &Cpu::h_and;
        H[0][0x25] = &Cpu::h_or;
        H[0][0x26] = &Cpu::h_xor;
        H[0][0x27] = &Cpu::h_nor;
        H[0][0x2A] = &Cpu::h_slt;
        H[0][0x2B] = &Cpu::h_sltu;
        H[0][0x2C] = &Cpu::h_dadd;
        H[0][0x2D] = &Cpu::h_daddu;
        H[0][0x2E] = &Cpu::h_dsub;
        H[0][0x2F] = &Cpu::h_dsubu;
        H[0][0x38] = &Cpu::h_dsll;
        H[0][0x3A] = &Cpu::h_dsrl;
        H[0][0x3B] = &Cpu::h_dsra;
        H[0][0x3C] = &Cpu::h_dsll32;
        H[0][0x3E] = &Cpu::h_dsrl32;
        H[0][0x3F] = &Cpu::h_dsra32;
        // Main opcodes
        H[0x01][0] = &Cpu::h_regimm;
        H[0x02][0] = &Cpu::h_j;
        H[0x03][0] = &Cpu::h_jal;
        H[0x04][0] = &Cpu::h_beq;
        H[0x05][0] = &Cpu::h_bne;
        H[0x06][0] = &Cpu::h_blez;
        H[0x07][0] = &Cpu::h_bgtz;
        H[0x08][0] = &Cpu::h_addi;
        H[0x09][0] = &Cpu::h_addiu;
        H[0x0A][0] = &Cpu::h_slti;
        H[0x0B][0] = &Cpu::h_sltiu;
        H[0x0C][0] = &Cpu::h_andi;
        H[0x0D][0] = &Cpu::h_ori;
        H[0x0E][0] = &Cpu::h_xori;
        H[0x0F][0] = &Cpu::h_lui;
        H[0x10][0] = &Cpu::executeInstr; // COP0
        H[0x11][0] = &Cpu::executeInstr; // COP1
        H[0x14][0] = &Cpu::h_beql;
        H[0x15][0] = &Cpu::h_bnel;
        H[0x16][0] = &Cpu::h_blezr;
        H[0x17][0] = &Cpu::h_bgtzl;
        H[0x18][0] = &Cpu::h_daddi;
        H[0x19][0] = &Cpu::h_daddiu;
        H[0x20][0] = &Cpu::h_lb;
        H[0x21][0] = &Cpu::h_lh;
        H[0x22][0] = &Cpu::h_lwl;
        H[0x23][0] = &Cpu::h_lw;
        H[0x24][0] = &Cpu::h_lbu;
        H[0x25][0] = &Cpu::h_lhu;
        H[0x26][0] = &Cpu::h_lwr;
        H[0x27][0] = &Cpu::h_lwu;
        H[0x28][0] = &Cpu::h_sb;
        H[0x29][0] = &Cpu::h_sh;
        H[0x2A][0] = &Cpu::h_swl;
        H[0x2B][0] = &Cpu::h_sw;
        H[0x2C][0] = &Cpu::h_sdl;
        H[0x2D][0] = &Cpu::h_sdr;
        H[0x2E][0] = &Cpu::h_swr;
        H[0x2F][0] = &Cpu::executeInstr; // CACHE
        H[0x30][0] = &Cpu::executeInstr; // LL
        H[0x31][0] = &Cpu::executeInstr; // LWC1
        H[0x35][0] = &Cpu::executeInstr; // LDC1
        H[0x37][0] = &Cpu::h_ld;
        H[0x38][0] = &Cpu::executeInstr; // SC
        H[0x39][0] = &Cpu::executeInstr; // SWC1
        H[0x3D][0] = &Cpu::executeInstr; // SDC1
        H[0x3F][0] = &Cpu::h_sd;
    }
} g_handlerInit;

enum QuietKind : u8 { Barrier, Register, Branch, Read1, Read2, Read4, Read8,
                     Write1, Write2, Write4, Write8 };

[[nodiscard]] bool isBranchEnding(const Cpu::DecodedInstr& i) noexcept {
    if (i.op == 0) {
        return (i.raw & 63) == 0x08 || (i.raw & 63) == 0x09; // JR / JALR
    }
    if (i.op == 0x01) {
        return i.rt <= 0x03 || (i.rt >= 0x10 && i.rt <= 0x13);
    }
    return i.op == 0x02 || i.op == 0x03 ||
           (i.op == 0x11 && i.rs == 8) ||
           (i.op >= 0x04 && i.op <= 0x07) ||
           (i.op >= 0x14 && i.op <= 0x17);
}

} // namespace

Cpu::DecodedInstr Cpu::decodeInstr(u32 instr) noexcept {
    DecodedInstr i;
    i.raw = instr;
    i.op = (instr >> 26) & 0x3F;
    i.rs = (instr >> 21) & 0x1F;
    i.rt = (instr >> 16) & 0x1F;
    i.rd = (instr >> 11) & 0x1F;
    i.shamt = (instr >> 6) & 0x1F;
    const u32 funct = instr & 0x3F;
    i.imm = static_cast<s16>(instr & 0xFFFF);
    i.target = instr & 0x03FFFFFF;
    i.handler = (i.op == 0) ? g_handlers[i.op][funct] : g_handlers[i.op][0];
    if (i.handler == nullptr) i.handler = &Cpu::executeInstr;
    if (i.handler != &Cpu::executeInstr) {
        // Only known non-trapping integer operations can defer Count and
        // device clocks. Memory operations additionally check the runtime EA.
        if (isBranchEnding(i)) i.quiet = Branch;
        else if ((i.op == 0 && funct != 0x20 && funct != 0x22 && funct != 0x2C && funct != 0x2E) ||
                 (i.op >= 9 && i.op <= 15) || i.op == 0x19) i.quiet = Register;
        else switch (i.op) {
            case 0x20: case 0x24: i.quiet = Read1; break;
            case 0x21: case 0x25: i.quiet = Read2; break;
            case 0x23: case 0x27: i.quiet = Read4; break;
            case 0x37: i.quiet = Read8; break;
            case 0x28: i.quiet = Write1; break;
            case 0x29: i.quiet = Write2; break;
            case 0x2B: i.quiet = Write4; break;
            case 0x3F: i.quiet = Write8; break;
        }
    }
    return i;
}

void Cpu::h_addiu(Cpu& c, Bus&, const DecodedInstr& i) { c.writeGpr(i.rt, static_cast<s32>(c.gpr_[i.rs] + static_cast<s64>(i.imm))); }
void Cpu::h_addi(Cpu& c, Bus&, const DecodedInstr& i) {
    const s32 a = static_cast<s32>(c.gpr_[i.rs]);
    const s32 b = i.imm;
    s32 sum = 0;
    if (!checkedAdd32(a, b, sum)) { c.raiseException(12); return; }
    c.writeGpr(i.rt, static_cast<s64>(sum));
}
void Cpu::h_addu(Cpu& c, Bus&, const DecodedInstr& i) { c.writeGpr(i.rd, static_cast<s32>(c.gpr_[i.rs] + c.gpr_[i.rt])); }
void Cpu::h_add(Cpu& c, Bus&, const DecodedInstr& i) {
    const s32 a = static_cast<s32>(c.gpr_[i.rs]);
    const s32 b = static_cast<s32>(c.gpr_[i.rt]);
    s32 sum = 0;
    if (!checkedAdd32(a, b, sum)) { c.raiseException(12); return; }
    c.writeGpr(i.rd, static_cast<s64>(sum));
}
void Cpu::h_subu(Cpu& c, Bus&, const DecodedInstr& i) { c.writeGpr(i.rd, static_cast<s32>(c.gpr_[i.rs] - c.gpr_[i.rt])); }
void Cpu::h_sub(Cpu& c, Bus&, const DecodedInstr& i) {
    const s32 a = static_cast<s32>(c.gpr_[i.rs]);
    const s32 b = static_cast<s32>(c.gpr_[i.rt]);
    s32 diff = 0;
    if (!checkedSub32(a, b, diff)) { c.raiseException(12); return; }
    c.writeGpr(i.rd, static_cast<s64>(diff));
}
void Cpu::h_and(Cpu& c, Bus&, const DecodedInstr& i) { c.writeGpr(i.rd, c.gpr_[i.rs] & c.gpr_[i.rt]); }
void Cpu::h_or(Cpu& c, Bus&, const DecodedInstr& i) { c.writeGpr(i.rd, c.gpr_[i.rs] | c.gpr_[i.rt]); }
void Cpu::h_xor(Cpu& c, Bus&, const DecodedInstr& i) { c.writeGpr(i.rd, c.gpr_[i.rs] ^ c.gpr_[i.rt]); }
void Cpu::h_nor(Cpu& c, Bus&, const DecodedInstr& i) { c.writeGpr(i.rd, ~(c.gpr_[i.rs] | c.gpr_[i.rt])); }
void Cpu::h_slt(Cpu& c, Bus&, const DecodedInstr& i) { c.writeGpr(i.rd, static_cast<s64>(c.gpr_[i.rs]) < static_cast<s64>(c.gpr_[i.rt]) ? 1 : 0); }
void Cpu::h_sltu(Cpu& c, Bus&, const DecodedInstr& i) { c.writeGpr(i.rd, c.gpr_[i.rs] < c.gpr_[i.rt] ? 1 : 0); }
void Cpu::h_slti(Cpu& c, Bus&, const DecodedInstr& i) { c.writeGpr(i.rt, static_cast<s64>(c.gpr_[i.rs]) < static_cast<s64>(i.imm) ? 1 : 0); }
void Cpu::h_sltiu(Cpu& c, Bus&, const DecodedInstr& i) { c.writeGpr(i.rt, c.gpr_[i.rs] < static_cast<u64>(static_cast<s64>(i.imm)) ? 1 : 0); }
void Cpu::h_andi(Cpu& c, Bus&, const DecodedInstr& i) { c.writeGpr(i.rt, c.gpr_[i.rs] & static_cast<u64>(static_cast<u16>(i.imm))); }
void Cpu::h_ori(Cpu& c, Bus&, const DecodedInstr& i) { c.writeGpr(i.rt, c.gpr_[i.rs] | static_cast<u64>(static_cast<u16>(i.imm))); }
void Cpu::h_xori(Cpu& c, Bus&, const DecodedInstr& i) { c.writeGpr(i.rt, c.gpr_[i.rs] ^ static_cast<u64>(static_cast<u16>(i.imm))); }
void Cpu::h_lui(Cpu& c, Bus&, const DecodedInstr& i) { c.writeGpr(i.rt, static_cast<s32>(static_cast<u32>(static_cast<u16>(i.imm)) << 16)); }
void Cpu::h_sll(Cpu& c, Bus&, const DecodedInstr& i) { c.writeGpr(i.rd, static_cast<s32>((c.gpr_[i.rt] & 0xFFFFFFFFULL) << i.shamt)); }
void Cpu::h_srl(Cpu& c, Bus&, const DecodedInstr& i) { c.writeGpr(i.rd, static_cast<s32>((c.gpr_[i.rt] & 0xFFFFFFFFULL) >> i.shamt)); }
void Cpu::h_sra(Cpu& c, Bus&, const DecodedInstr& i) { c.writeGpr(i.rd, shiftWordArithmetic(c.gpr_[i.rt], i.shamt)); }
void Cpu::h_sllv(Cpu& c, Bus&, const DecodedInstr& i) { c.writeGpr(i.rd, static_cast<s32>((c.gpr_[i.rt] & 0xFFFFFFFFULL) << (c.gpr_[i.rs] & 0x1F))); }
void Cpu::h_srlv(Cpu& c, Bus&, const DecodedInstr& i) { c.writeGpr(i.rd, static_cast<s32>((c.gpr_[i.rt] & 0xFFFFFFFFULL) >> (c.gpr_[i.rs] & 0x1F))); }
void Cpu::h_srav(Cpu& c, Bus&, const DecodedInstr& i) { c.writeGpr(i.rd, shiftWordArithmetic(c.gpr_[i.rt], c.gpr_[i.rs] & 31)); }
void Cpu::h_daddiu(Cpu& c, Bus&, const DecodedInstr& i) { c.writeGpr(i.rt, c.gpr_[i.rs] + static_cast<s64>(i.imm)); }
void Cpu::h_daddi(Cpu& c, Bus&, const DecodedInstr& i) {
    const s64 a = static_cast<s64>(c.gpr_[i.rs]);
    const s64 b = static_cast<s64>(i.imm);
    s64 sum = 0;
    if (!checkedAdd64(a, b, sum)) { c.raiseException(12); return; }
    c.writeGpr(i.rt, sum);
}
void Cpu::h_daddu(Cpu& c, Bus&, const DecodedInstr& i) { c.writeGpr(i.rd, c.gpr_[i.rs] + c.gpr_[i.rt]); }
void Cpu::h_dadd(Cpu& c, Bus&, const DecodedInstr& i) {
    const s64 a = static_cast<s64>(c.gpr_[i.rs]);
    const s64 b = static_cast<s64>(c.gpr_[i.rt]);
    s64 sum = 0;
    if (!checkedAdd64(a, b, sum)) { c.raiseException(12); return; }
    c.writeGpr(i.rd, sum);
}
void Cpu::h_dsubu(Cpu& c, Bus&, const DecodedInstr& i) { c.writeGpr(i.rd, c.gpr_[i.rs] - c.gpr_[i.rt]); }
void Cpu::h_dsub(Cpu& c, Bus&, const DecodedInstr& i) {
    const s64 a = static_cast<s64>(c.gpr_[i.rs]);
    const s64 b = static_cast<s64>(c.gpr_[i.rt]);
    s64 diff = 0;
    if (!checkedSub64(a, b, diff)) { c.raiseException(12); return; }
    c.writeGpr(i.rd, diff);
}
void Cpu::h_dsll(Cpu& c, Bus&, const DecodedInstr& i) { c.writeGpr(i.rd, c.gpr_[i.rt] << i.shamt); }
void Cpu::h_dsrl(Cpu& c, Bus&, const DecodedInstr& i) { c.writeGpr(i.rd, c.gpr_[i.rt] >> i.shamt); }
void Cpu::h_dsra(Cpu& c, Bus&, const DecodedInstr& i) { c.writeGpr(i.rd, static_cast<s64>(c.gpr_[i.rt]) >> i.shamt); }
void Cpu::h_dsll32(Cpu& c, Bus&, const DecodedInstr& i) { c.writeGpr(i.rd, c.gpr_[i.rt] << (i.shamt + 32)); }
void Cpu::h_dsrl32(Cpu& c, Bus&, const DecodedInstr& i) { c.writeGpr(i.rd, c.gpr_[i.rt] >> (i.shamt + 32)); }
void Cpu::h_dsra32(Cpu& c, Bus&, const DecodedInstr& i) { c.writeGpr(i.rd, static_cast<s64>(c.gpr_[i.rt]) >> (i.shamt + 32)); }
void Cpu::h_dsllv(Cpu& c, Bus&, const DecodedInstr& i) { c.writeGpr(i.rd, c.gpr_[i.rt] << (c.gpr_[i.rs] & 0x3F)); }
void Cpu::h_dsrlv(Cpu& c, Bus&, const DecodedInstr& i) { c.writeGpr(i.rd, c.gpr_[i.rt] >> (c.gpr_[i.rs] & 0x3F)); }
void Cpu::h_dsrav(Cpu& c, Bus&, const DecodedInstr& i) { c.writeGpr(i.rd, static_cast<s64>(c.gpr_[i.rt]) >> (c.gpr_[i.rs] & 0x3F)); }

void Cpu::h_mult(Cpu& c, Bus&, const DecodedInstr& i) {
    const s64 result = static_cast<s64>(static_cast<s32>(c.gpr_[i.rs])) * static_cast<s64>(static_cast<s32>(c.gpr_[i.rt]));
    c.lo_ = static_cast<s32>(result & 0xFFFFFFFFLL);
    c.hi_ = static_cast<s32>(result >> 32);
}
void Cpu::h_multu(Cpu& c, Bus&, const DecodedInstr& i) {
    const u64 result = static_cast<u64>(static_cast<u32>(c.gpr_[i.rs])) * static_cast<u64>(static_cast<u32>(c.gpr_[i.rt]));
    c.lo_ = static_cast<s32>(result & 0xFFFFFFFFULL);
    c.hi_ = static_cast<s32>(result >> 32);
}
void Cpu::h_div(Cpu& c, Bus&, const DecodedInstr& i) {
    divide<s32>(c.gpr_[i.rs], c.gpr_[i.rt], c.lo_, c.hi_);
}
void Cpu::h_divu(Cpu& c, Bus&, const DecodedInstr& i) {
    divide<u32>(c.gpr_[i.rs], c.gpr_[i.rt], c.lo_, c.hi_);
}
void Cpu::h_dmult(Cpu& c, Bus&, const DecodedInstr& i) {
    const auto product = multiplyWideSigned(c.gpr_[i.rs], c.gpr_[i.rt]);
    c.lo_ = product.lo;
    c.hi_ = product.hi;
}
void Cpu::h_dmultu(Cpu& c, Bus&, const DecodedInstr& i) {
    const auto product = multiplyWideUnsigned(c.gpr_[i.rs], c.gpr_[i.rt]);
    c.lo_ = product.lo;
    c.hi_ = product.hi;
}
void Cpu::h_ddiv(Cpu& c, Bus&, const DecodedInstr& i) {
    divide<s64>(c.gpr_[i.rs], c.gpr_[i.rt], c.lo_, c.hi_);
}
void Cpu::h_ddivu(Cpu& c, Bus&, const DecodedInstr& i) {
    divide<u64>(c.gpr_[i.rs], c.gpr_[i.rt], c.lo_, c.hi_);
}
void Cpu::h_mfhi(Cpu& c, Bus&, const DecodedInstr& i) { c.writeGpr(i.rd, c.hi_); }
void Cpu::h_mthi(Cpu& c, Bus&, const DecodedInstr& i) { c.hi_ = c.gpr_[i.rs]; }
void Cpu::h_mflo(Cpu& c, Bus&, const DecodedInstr& i) { c.writeGpr(i.rd, c.lo_); }
void Cpu::h_mtlo(Cpu& c, Bus&, const DecodedInstr& i) { c.lo_ = c.gpr_[i.rs]; }

void Cpu::h_j(Cpu& c, Bus&, const DecodedInstr& i) {
    c.branchTarget_ = ((c.pc_ + 4) & 0xFFFFFFFFF0000000ULL) | (static_cast<u64>(i.target) << 2);
    c.delaySlot_ = true;
}
void Cpu::h_jal(Cpu& c, Bus&, const DecodedInstr& i) {
    c.writeGpr(31, c.pc_ + 8);
    c.branchTarget_ = ((c.pc_ + 4) & 0xFFFFFFFFF0000000ULL) | (static_cast<u64>(i.target) << 2);
    c.delaySlot_ = true;
}
void Cpu::h_jr(Cpu& c, Bus&, const DecodedInstr& i) { c.branchTarget_ = c.gpr_[i.rs]; c.delaySlot_ = true; }
void Cpu::h_jalr(Cpu& c, Bus&, const DecodedInstr& i) {
    c.branchTarget_ = c.gpr_[i.rs];
    c.writeGpr(i.rd, c.pc_ + 8);
    c.delaySlot_ = true;
}

static void takeBranch(Cpu& c, s32 imm) {
    c.branchTarget_ = c.pc_ + 4 + static_cast<s64>(imm) * 4;
    c.delaySlot_ = true;
}

// An untaken ordinary branch still executes a delay slot and reports BD on
// faults there. Its sequential target is PC + 8 (branch displacement +1).
void Cpu::h_beq(Cpu& c, Bus&, const DecodedInstr& i) { takeBranch(c, c.gpr_[i.rs] == c.gpr_[i.rt] ? i.imm : 1); }
void Cpu::h_bne(Cpu& c, Bus&, const DecodedInstr& i) { takeBranch(c, c.gpr_[i.rs] != c.gpr_[i.rt] ? i.imm : 1); }
void Cpu::h_blez(Cpu& c, Bus&, const DecodedInstr& i) { takeBranch(c, static_cast<s64>(c.gpr_[i.rs]) <= 0 ? i.imm : 1); }
void Cpu::h_bgtz(Cpu& c, Bus&, const DecodedInstr& i) { takeBranch(c, static_cast<s64>(c.gpr_[i.rs]) > 0 ? i.imm : 1); }
void Cpu::h_beql(Cpu& c, Bus&, const DecodedInstr& i) {
    if (c.gpr_[i.rs] == c.gpr_[i.rt]) takeBranch(c, i.imm);
    else { c.pc_ = c.nextPc_; c.nextPc_ = c.pc_ + 4; }
}
void Cpu::h_bnel(Cpu& c, Bus&, const DecodedInstr& i) {
    if (c.gpr_[i.rs] != c.gpr_[i.rt]) takeBranch(c, i.imm);
    else { c.pc_ = c.nextPc_; c.nextPc_ = c.pc_ + 4; }
}
void Cpu::h_blezr(Cpu& c, Bus&, const DecodedInstr& i) {
    if (static_cast<s64>(c.gpr_[i.rs]) <= 0) takeBranch(c, i.imm);
    else { c.pc_ = c.nextPc_; c.nextPc_ = c.pc_ + 4; }
}
void Cpu::h_bgtzl(Cpu& c, Bus&, const DecodedInstr& i) {
    if (static_cast<s64>(c.gpr_[i.rs]) > 0) takeBranch(c, i.imm);
    else { c.pc_ = c.nextPc_; c.nextPc_ = c.pc_ + 4; }
}

void Cpu::h_bltz(Cpu& c, Bus&, const DecodedInstr& i) { if (static_cast<s64>(c.gpr_[i.rs]) < 0) takeBranch(c, i.imm); }
void Cpu::h_bgez(Cpu& c, Bus&, const DecodedInstr& i) { if (static_cast<s64>(c.gpr_[i.rs]) >= 0) takeBranch(c, i.imm); }
void Cpu::h_bltzal(Cpu& c, Bus&, const DecodedInstr& i) { c.writeGpr(31, c.pc_ + 8); if (static_cast<s64>(c.gpr_[i.rs]) < 0) takeBranch(c, i.imm); }
void Cpu::h_bgezal(Cpu& c, Bus&, const DecodedInstr& i) { c.writeGpr(31, c.pc_ + 8); if (static_cast<s64>(c.gpr_[i.rs]) >= 0) takeBranch(c, i.imm); }
void Cpu::h_bltzl(Cpu& c, Bus&, const DecodedInstr& i) {
    c.writeGpr(31, c.pc_ + 8);
    if (static_cast<s64>(c.gpr_[i.rs]) < 0) takeBranch(c, i.imm);
    else { c.pc_ = c.nextPc_; c.nextPc_ = c.pc_ + 4; }
}
void Cpu::h_bgezl(Cpu& c, Bus&, const DecodedInstr& i) {
    c.writeGpr(31, c.pc_ + 8);
    if (static_cast<s64>(c.gpr_[i.rs]) >= 0) takeBranch(c, i.imm);
    else { c.pc_ = c.nextPc_; c.nextPc_ = c.pc_ + 4; }
}
void Cpu::h_bltzall(Cpu& c, Bus&, const DecodedInstr& i) {
    c.writeGpr(31, c.pc_ + 8);
    if (static_cast<s64>(c.gpr_[i.rs]) < 0) takeBranch(c, i.imm);
    else { c.pc_ = c.nextPc_; c.nextPc_ = c.pc_ + 4; }
}
void Cpu::h_bgezall(Cpu& c, Bus&, const DecodedInstr& i) {
    c.writeGpr(31, c.pc_ + 8);
    if (static_cast<s64>(c.gpr_[i.rs]) >= 0) takeBranch(c, i.imm);
    else { c.pc_ = c.nextPc_; c.nextPc_ = c.pc_ + 4; }
}

void Cpu::h_regimm(Cpu& c, Bus&, const DecodedInstr& i) {
    const s64 v = static_cast<s64>(c.gpr_[i.rs]);
    bool take = false;
    switch (i.rt) {
        case 0x00: take = v < 0; break;
        case 0x01: take = v >= 0; break;
        case 0x02: take = v < 0; break;
        case 0x03: take = v >= 0; break;
        case 0x10: case 0x12: c.writeGpr(31, c.pc_ + 8); take = v < 0; break;
        case 0x11: case 0x13: c.writeGpr(31, c.pc_ + 8); take = v >= 0; break;
        default: c.raiseException(10); return;
    }
    if (take) {
        takeBranch(c, i.imm);
    } else if (i.rt & 2) {
        c.pc_ = c.nextPc_;
        c.nextPc_ = c.pc_ + 4;
    } else {
        takeBranch(c, 1);
    }
}

void Cpu::h_lb(Cpu& c, Bus& bus, const DecodedInstr& i) { c.writeGpr(i.rt, static_cast<s8>(c.load<u8>(bus, c.gpr_[i.rs] + static_cast<s64>(i.imm)))); }
void Cpu::h_lh(Cpu& c, Bus& bus, const DecodedInstr& i) { c.writeGpr(i.rt, static_cast<s16>(c.load<u16>(bus, c.gpr_[i.rs] + static_cast<s64>(i.imm)))); }
void Cpu::h_lw(Cpu& c, Bus& bus, const DecodedInstr& i) { c.writeGpr(i.rt, static_cast<s32>(c.load<u32>(bus, c.gpr_[i.rs] + static_cast<s64>(i.imm)))); }
void Cpu::h_lbu(Cpu& c, Bus& bus, const DecodedInstr& i) { c.writeGpr(i.rt, c.load<u8>(bus, c.gpr_[i.rs] + static_cast<s64>(i.imm))); }
void Cpu::h_lhu(Cpu& c, Bus& bus, const DecodedInstr& i) { c.writeGpr(i.rt, c.load<u16>(bus, c.gpr_[i.rs] + static_cast<s64>(i.imm))); }
void Cpu::h_lwu(Cpu& c, Bus& bus, const DecodedInstr& i) { c.writeGpr(i.rt, c.load<u32>(bus, c.gpr_[i.rs] + static_cast<s64>(i.imm))); }
void Cpu::h_ld(Cpu& c, Bus& bus, const DecodedInstr& i) { c.writeGpr(i.rt, c.load<u64>(bus, c.gpr_[i.rs] + static_cast<s64>(i.imm))); }
void Cpu::h_lwl(Cpu& c, Bus& bus, const DecodedInstr& i) {
    const u64 address = c.gpr_[i.rs] + static_cast<s64>(i.imm);
    const u32 word = c.load<u32>(bus, address & ~3ULL);
    const u32 offset = address & 3;
    const u32 shift = offset * 8;
    const u32 keep = (1u << shift) - 1;
    c.writeGpr(i.rt, signExtend32((c.gpr_[i.rt] & keep) | (word << shift)));
}
void Cpu::h_lwr(Cpu& c, Bus& bus, const DecodedInstr& i) {
    const u64 address = c.gpr_[i.rs] + static_cast<s64>(i.imm);
    const u32 word = c.load<u32>(bus, address & ~3ULL);
    const u32 offset = address & 3;
    if (offset == 3) { c.writeGpr(i.rt, signExtend32(word)); return; }
    const u32 shift = (3 - offset) * 8;
    const u64 mask = 0xFFFFFFFFu >> shift;
    c.writeGpr(i.rt, (c.gpr_[i.rt] & ~mask) | (word >> shift));
}
void Cpu::h_ldl(Cpu& c, Bus& bus, const DecodedInstr& i) {
    const u64 address = c.gpr_[i.rs] + static_cast<s64>(i.imm);
    const u64 word = c.load<u64>(bus, address & ~7ULL);
    const u32 offset = address & 7;
    const u32 shift = offset * 8;
    const u64 keep = (1ULL << shift) - 1;
    c.writeGpr(i.rt, (c.gpr_[i.rt] & keep) | (word << shift));
}
void Cpu::h_ldr(Cpu& c, Bus& bus, const DecodedInstr& i) {
    const u64 address = c.gpr_[i.rs] + static_cast<s64>(i.imm);
    const u64 word = c.load<u64>(bus, address & ~7ULL);
    const u32 offset = address & 7;
    const u32 shift = (7 - offset) * 8;
    const u64 mask = ~0ULL >> shift;
    c.writeGpr(i.rt, (c.gpr_[i.rt] & ~mask) | (word >> shift));
}

void Cpu::h_sb(Cpu& c, Bus& bus, const DecodedInstr& i) { c.store(bus, c.gpr_[i.rs] + static_cast<s64>(i.imm), static_cast<u8>(c.gpr_[i.rt])); }
void Cpu::h_sh(Cpu& c, Bus& bus, const DecodedInstr& i) { c.store(bus, c.gpr_[i.rs] + static_cast<s64>(i.imm), static_cast<u16>(c.gpr_[i.rt])); }
void Cpu::h_sw(Cpu& c, Bus& bus, const DecodedInstr& i) { c.store(bus, c.gpr_[i.rs] + static_cast<s64>(i.imm), static_cast<u32>(c.gpr_[i.rt])); }
void Cpu::h_sd(Cpu& c, Bus& bus, const DecodedInstr& i) { c.store(bus, c.gpr_[i.rs] + static_cast<s64>(i.imm), c.gpr_[i.rt]); }

static void storeUnaligned(Cpu& c, Bus& bus, u32 address, u64 value, u32 size, bool left) {
    const u32 aligned = address & ~(size - 1);
    const u32 start = left ? address : aligned;
    const u32 end = left ? aligned + size : address + 1;
    const u32 sourceEnd = left ? start + size : end;
    for (u32 dst = start; dst != end;) {
        const u32 remaining = end - dst;
        const u32 width = remaining >= 8 && !(dst & 7) ? 8 :
                          remaining >= 4 && !(dst & 3) ? 4 :
                          remaining >= 2 && !(dst & 1) ? 2 : 1;
        const u64 part = value >> ((sourceEnd - dst - width) * 8);
        if (width == 8) bus.write64(dst, part);
        else if (width == 4) bus.write32(dst, static_cast<u32>(part));
        else if (width == 2) bus.write16(dst, static_cast<u16>(part));
        else bus.write8(dst, static_cast<u8>(part));
        dst += width;
    }
}

void Cpu::h_swl(Cpu& c, Bus& bus, const DecodedInstr& i) {
    const u32 address = static_cast<u32>(c.translateAddress(c.gpr_[i.rs] + static_cast<s64>(i.imm), true));
    storeUnaligned(c, bus, address, c.gpr_[i.rt], 4, true);
}
void Cpu::h_swr(Cpu& c, Bus& bus, const DecodedInstr& i) {
    const u32 address = static_cast<u32>(c.translateAddress(c.gpr_[i.rs] + static_cast<s64>(i.imm), true));
    storeUnaligned(c, bus, address, c.gpr_[i.rt], 4, false);
}
void Cpu::h_sdl(Cpu& c, Bus& bus, const DecodedInstr& i) {
    const u32 address = static_cast<u32>(c.translateAddress(c.gpr_[i.rs] + static_cast<s64>(i.imm), true));
    storeUnaligned(c, bus, address, c.gpr_[i.rt], 8, true);
}
void Cpu::h_sdr(Cpu& c, Bus& bus, const DecodedInstr& i) {
    const u32 address = static_cast<u32>(c.translateAddress(c.gpr_[i.rs] + static_cast<s64>(i.imm), true));
    storeUnaligned(c, bus, address, c.gpr_[i.rt], 8, false);
}

void Cpu::executeInstr(Cpu& c, Bus& bus, const DecodedInstr& i) {
    // Mirror of the original interpreter for any instruction not covered by a
    // dedicated fast handler (COP0, COP1, LL/SC, CACHE, unrecognised opcodes).
    c.executeLegacy(i.raw, bus);
}

bool Cpu::isDirectFetch(u64 vaddr) noexcept {
    constexpr u32 mask = ~(Memory::RdramSize - 1u) & ~0x20000000u;
    const u32 low = static_cast<u32>(vaddr);
    return vaddr == signExtend32(low) && (low & (mask | 3u)) == 0x80000000u;
}

u32 Cpu::fetchDirect(u64 vaddr, const Bus& bus) noexcept {
    return bus.rdram().loadRdram<u32>(vaddr & (Memory::RdramSize - 1));
}

void Cpu::clearBlockCache() noexcept {
    activeBlock_ = nullptr;
    lineEpoch_.fetch_add(1, std::memory_order_acq_rel);
    for (auto& b : blocks_) b.length = 0;
    for (auto& generation : lineGen_)
        generation.store(0, std::memory_order_release);
    invalidateJitAll();
}

void Cpu::attachToBus(Bus& bus) {
    if (attachedBus_ != &bus) clearBlockCache();
    attachedBus_ = &bus;
    bus.rdram().setWriteCallback(
        [](void* context, u32 address, u32 size) {
            static_cast<Cpu*>(context)->invalidateBlockCache(address, size);
        },
        this);
}

void Cpu::invalidateBlockCache(u32 physicalAddress, u32 size) noexcept {
    if (!size || physicalAddress >= Memory::RdramSize) return;
    invalidateJitCode(physicalAddress, size);
    const u32 last = physicalAddress + std::min(size, Memory::RdramSize - physicalAddress) - 1;
    for (u32 line = physicalAddress >> 6; line <= (last >> 6); ++line)
        if (lineGen_[line].fetch_add(1, std::memory_order_acq_rel) ==
            std::numeric_limits<u32>::max())
            clearBlockCache();
}

void Cpu::compileBlock(u64 vaddr, Bus& bus) {
    const u32 physPc = static_cast<u32>(translateAddress(vaddr, false));
    const u32 index = (physPc >> 2) & (BlockCacheSize - 1);
    Block& block = blocks_[index];
    block.length = 0;
    block.lineCount = 0;
    block.staticSuccessor = nullptr;
    u64 current = vaddr;
    u16 length = 0;
    constexpr u16 Capacity = static_cast<u16>(std::tuple_size_v<decltype(block.instrs)>);

    // Record every distinct 64-byte code line the block occupies together with
    // its generation at compile time. Execution compares them again so a write
    // to *any* line that holds part of the block forces a recompile.
    const auto noteLine = [&](u64 address) {
        const u32 line = (static_cast<u32>(address) & (Memory::RdramSize - 1)) >> 6;
        if (block.lineCount != 0 && block.lineIdx[block.lineCount - 1] == line) return;
        if (block.lineCount == MaxBlockLines) return;
        block.lineIdx[block.lineCount] = line;
        block.lineGen[block.lineCount] = lineGen_[line].load(std::memory_order_acquire);
        ++block.lineCount;
    };

    while (length < Capacity) {
        if (!isDirectFetch(current)) break;
        const u32 raw = fetchDirect(current, bus);
        DecodedInstr di = decodeInstr(raw);
        noteLine(current);
        block.instrs[length++] = di;
        if (isBranchEnding(di)) {
            // Include the architecturally-visible delay slot and stop.
            current += 4;
            if (isDirectFetch(current) && length < Capacity) {
                noteLine(current);
                block.instrs[length++] = decodeInstr(fetchDirect(current, bus));
            }
            break;
        }
        current += 4;
    }
    if (length == 0) return;
    ++cacheStats_.blocksCompiled;
    block.physPc = physPc;
    block.length = length;
}

const Cpu::DecodedInstr& Cpu::cachedInstruction(Bus& bus) {
    const u32 phys = static_cast<u32>(pc_) & (Memory::RdramSize - 1);
    const u32 line = phys >> 6;
    const u32 generation = lineGen_[line].load(std::memory_order_acquire);
    ++cacheStats_.lookups;
    const auto valid = [&](const Block& block) {
        return (phys - block.physPc) / 4 < block.length &&
               block.lineGen[line - (block.physPc >> 6)] == generation;
    };
    if (activeBlock_ && valid(*activeBlock_)) {
        ++cacheStats_.hits;
    } else {
        auto& block = blocks_[(phys >> 2) & (BlockCacheSize - 1)];
        if (valid(block)) ++cacheStats_.hits;
        else compileBlock(pc_, bus);
        activeBlock_ = &block;
        ++cacheStats_.blocksRun;
    }
    // DMA, a device or the preceding CPU instruction may have written this
    // line since the previous step. Validate every cached fetch, not just entry.
    return activeBlock_->instrs[(phys - activeBlock_->physPc) / 4];
}

u32 Cpu::runBlock(Bus& bus, u32 maxInstructions) {
    const u32 limit = maxInstructions ? maxInstructions : 64;
    u32 steps = 0;
    while (steps < limit && isDirectFetch(pc_)) {
        const u64 previousPc = pc_;
        const bool wasDelaySlot = delaySlot_;
        stepCached(bus);
        ++steps;
        const u32 nextPhys = static_cast<u32>(pc_) & (Memory::RdramSize - 1);
        if (wasDelaySlot || pc_ != previousPc + 4 || !activeBlock_ ||
            (nextPhys - activeBlock_->physPc) / 4 >= activeBlock_->length) break;
    }
    return steps;
}

u32 Cpu::runQuietBlock(Bus& bus, u32 budget) {
    if (!budget || !isDirectFetch(pc_)) return 0;
    constexpr u64 MiPending = 1u << 10;
    const auto cause = (cop0_[C0_CAUSE] & ~MiPending) | (bus.hw().mi().irq() ? MiPending : 0);
    const auto status = cop0_[C0_STATUS];
    if ((status & 7) == 1 && (status & cause & 0xFF00) != 0) return 0;
    const u32 distance = static_cast<u32>(cop0_[C0_COMPARE] - cop0_[C0_COUNT]);
    // Step the match cycle normally: the compare interrupt can take effect
    // before dispatch, not after the instruction at the deadline.
    if (distance)
        budget = static_cast<u32>(std::min<u64>(budget, u64{distance} * 2 - 1 - countHalfCycle_));
    if (!budget) return 0;
    if (attachedBus_ != &bus) attachToBus(bus);
    const auto finish = [&](u32 steps, u64 currentPc) {
        instructionsDispatched_ += steps;
        const u32 increments = (steps + u32{countHalfCycle_}) / 2;
        if (increments) cop0_[C0_COUNT] = static_cast<u32>(cop0_[C0_COUNT] + increments);
        countHalfCycle_ ^= (steps & 1) != 0;
        cop0_[C0_CAUSE] = cause;
        pc_ = currentPc;
        nextPc_ = currentPc + 4;
        return steps;
    };
    if (hasJit() && !delaySlot_) {
        u32 retired = 0;
        u64 next = pc_;
        while (budget - retired >= 2 && isDirectFetch(next)) {
            ++jitInvocations_;
            // A COP0 write earlier in this batch can change CU1 or FR. Native
            // translation never crosses such an instruction, but every new
            // entry must see the live mode so the CU1 guard and block key stay
            // exact at the faulting PC.
            const auto nativeStatus = cop0_[C0_STATUS];
            const auto native = runJit(bus.rdram(), next,
                                       lineEpoch_.load(std::memory_order_acquire),
                                       lineGen_.data(), budget - retired,
                                       (nativeStatus & (1ULL << 29)) != 0,
                                       (nativeStatus & (1ULL << 26)) != 0, bus);
            if (!native.retired) break;
            retired += native.retired;
            next = native.pc;
        }
        if (retired) {
            jitSteps_ += retired;
            return finish(retired, next);
        }
    }
    const u64 lookups = cacheStats_.lookups, hits = cacheStats_.hits;
    const auto* first = &cachedInstruction(bus);
    u64 currentPc = pc_, staticBranchPc = 0;
    u32 steps = 0;
    u32 entries = 1;
    for (;;) {
      auto& block = *activeBlock_;
      const u32 phys = static_cast<u32>(currentPc) & (Memory::RdramSize - 1);
      const u32 firstSlot = (phys - block.physPc) / 4;
      // Revalidate at every linked entry. Within an entry, stop at a line
      // boundary or store so writes cannot leave stale decoded instructions.
      const u32 limit = std::min({budget - steps, block.length - firstSlot, 16 - ((phys >> 2) & 15)});
      bool link = false;
      for (u32 slot = 0; slot < limit; ++slot) {
        const auto& i = first[slot];
        const auto kind = i.quiet;
        if (kind == Barrier) break;
        if (kind >= Read1) {
            const u64 address = gpr_[i.rs] + static_cast<s64>(i.imm);
            const u64 alignment = (1u << ((kind - Read1) & 3)) - 1;
            constexpr u64 mask = ~(u64{Memory::RdramSize - 1} | 0x20000000u);
            if ((address & (mask | alignment)) != 0xFFFFFFFF80000000ULL) {
                // VI_CURRENT reads do not acknowledge the interrupt and the
                // value is constant until the scheduler's next VI line event.
                // No other MMIO (including writes to VI_CURRENT) is admitted.
                const auto alias = address & ~u64{0x20000000};
                if (kind != Read4 || alias != 0xFFFFFFFF84400010ULL) break;
            }
        }
        const auto sequentialPc = currentPc + 4;
        const bool wasDelay = delaySlot_;
        if (kind == Branch) {
            pc_ = currentPc;
            nextPc_ = sequentialPc;
        }
        if (i.raw) i.handler(*this, bus, i);
        ++steps;
        if (wasDelay) {
            currentPc = branchTarget_;
            delaySlot_ = false;
            branchTarget_ = 0;
            link = staticBranchPc && kind != Branch && kind < Write1;
            if (link && currentPc == staticBranchPc && !i.raw) {
                // A taken self-branch with a NOP delay slot repeats exactly
                // the same architectural state (a link writes the same RA).
                // Count all retired instructions, but don't redispatch them
                // inside this already bounded device/Compare horizon.
                steps += (budget - steps) & ~1u;
                link = false;
            }
            break;
        }
        if (kind == Branch) {
            if (delaySlot_ && i.op >= 2 && i.op <= 5 && branchTarget_ != currentPc + 8)
                staticBranchPc = currentPc;
            currentPc = nextPc_;
            if (currentPc != sequentialPc) {
                // Branch-likely handlers may advance both PC and next PC.
                // A nonsequential next instruction is not in this prefix.
                break;
            }
        } else {
            currentPc = sequentialPc;
        }
        if (kind >= Write1) break;
      }
      // PC semantics stay virtual; only the decoded cache is physical.
      if (!link || steps == budget || !isDirectFetch(currentPc) ||
          ((staticBranchPc ^ currentPc) >> 12) != 0) break;
      const u32 target = static_cast<u32>(currentPc) & (Memory::RdramSize - 1);
      auto* successor = block.staticSuccessor;
      if (successor && successor->length && successor->physPc == target &&
          successor->lineGen[0] == lineGen_[target >> 6].load(std::memory_order_acquire)) {
          activeBlock_ = successor;
          first = &successor->instrs[0];
          ++cacheStats_.lookups;
          ++cacheStats_.hits;
      } else {
          pc_ = currentPc;
          first = &cachedInstruction(bus);
          if (activeBlock_->physPc == target) block.staticSuccessor = activeBlock_;
      }
      staticBranchPc = 0;
      ++entries;
      ++cacheStats_.branchLinksTaken;
    }
    if (!steps) {
        cacheStats_.lookups = lookups;
        cacheStats_.hits = hits;
        return 0;
    }
    if (steps > entries) {
        cacheStats_.lookups += steps - entries;
        cacheStats_.hits += steps - entries;
    }
    return finish(steps, currentPc);
}

void Cpu::execute(u32 instr, Bus& bus) {
    const DecodedInstr i = decodeInstr(instr);
    i.handler(*this, bus, i);
}

void Cpu::executeLegacy(u32 instr, Bus& bus) {
    const u32 op = (instr >> 26) & 0x3F;
    const u32 rs = (instr >> 21) & 0x1F;
    const u32 rt = (instr >> 16) & 0x1F;
    const u32 rd = (instr >> 11) & 0x1F;
    const u32 shamt = (instr >> 6) & 0x1F;
    const u32 funct = instr & 0x3F;
    const s32 imm = static_cast<s16>(instr & 0xFFFF);
    const u32 target = instr & 0x03FFFFFF;

    const auto takeBranch = [this, pc = pc_](s32 offset) {
        branchTarget_ = pc + 4 + static_cast<s64>(offset) * 4;
        delaySlot_ = true;
    };

    switch (op) {
        case 0x00: { // SPECIAL
            switch (funct) {
                case 0x00: // SLL
                    writeGpr(rd, static_cast<s32>((gpr_[rt] & 0xFFFFFFFFULL) << shamt));
                    break;
                case 0x02: // SRL
                    writeGpr(rd, static_cast<s32>((gpr_[rt] & 0xFFFFFFFFULL) >> shamt));
                    break;
                case 0x03: // SRA
                    writeGpr(rd, shiftWordArithmetic(gpr_[rt], shamt));
                    break;
                case 0x04: // SLLV
                    writeGpr(rd, static_cast<s32>((gpr_[rt] & 0xFFFFFFFFULL) << (gpr_[rs] & 0x1F)));
                    break;
                case 0x06: // SRLV
                    writeGpr(rd, static_cast<s32>((gpr_[rt] & 0xFFFFFFFFULL) >> (gpr_[rs] & 0x1F)));
                    break;
                case 0x07: // SRAV
                    writeGpr(rd, shiftWordArithmetic(gpr_[rt], gpr_[rs] & 31));
                    break;
                case 0x08: // JR
                    branchTarget_ = gpr_[rs];
                    delaySlot_ = true;
                    break;
                case 0x09: { // JALR
                    const u64 returnAddr = pc_ + 8;
                    branchTarget_ = gpr_[rs];
                    delaySlot_ = true;
                    writeGpr(rd, returnAddr);
                    break;
                }
                case 0x0C: // SYSCALL
                    raiseException(8);
                    return;
                case 0x0D: // BREAK
                    raiseException(9);
                    return;
                case 0x0F: // SYNC
                    // CPU bus accesses are synchronous in this model. Keep
                    // SYNC out of quiet batching so device time is flushed.
                    break;
                case 0x10: // MFHI
                    writeGpr(rd, hi_);
                    break;
                case 0x11: // MTHI
                    hi_ = gpr_[rs];
                    break;
                case 0x12: // MFLO
                    writeGpr(rd, lo_);
                    break;
                case 0x13: // MTLO
                    lo_ = gpr_[rs];
                    break;
                case 0x14: { // DSLLV
                    const u32 sa = static_cast<u32>(gpr_[rs]) & 0x3F;
                    writeGpr(rd, gpr_[rt] << sa);
                    break;
                }
                case 0x16: { // DSRLV
                    const u32 sa = static_cast<u32>(gpr_[rs]) & 0x3F;
                    writeGpr(rd, gpr_[rt] >> sa);
                    break;
                }
                case 0x17: { // DSRAV
                    const u32 sa = static_cast<u32>(gpr_[rs]) & 0x3F;
                    writeGpr(rd, static_cast<s64>(gpr_[rt]) >> sa);
                    break;
                }
                case 0x18: { // MULT
                    const s64 result = static_cast<s64>(static_cast<s32>(gpr_[rs])) *
                                       static_cast<s64>(static_cast<s32>(gpr_[rt]));
                    lo_ = static_cast<s32>(result & 0xFFFFFFFFLL);
                    hi_ = static_cast<s32>(result >> 32);
                    break;
                }
                case 0x19: { // MULTU
                    const u64 result = static_cast<u64>(static_cast<u32>(gpr_[rs])) *
                                       static_cast<u64>(static_cast<u32>(gpr_[rt]));
                    lo_ = static_cast<s32>(result & 0xFFFFFFFFULL);
                    hi_ = static_cast<s32>(result >> 32);
                    break;
                }
                case 0x1A: { // DIV
                    divide<s32>(gpr_[rs], gpr_[rt], lo_, hi_);
                    break;
                }
                case 0x1B: { // DIVU
                    divide<u32>(gpr_[rs], gpr_[rt], lo_, hi_);
                    break;
                }
                case 0x1C: { // DMULT
                    const auto product = multiplyWideSigned(gpr_[rs], gpr_[rt]);
                    lo_ = product.lo;
                    hi_ = product.hi;
                    break;
                }
                case 0x1D: { // DMULTU
                    const auto product = multiplyWideUnsigned(gpr_[rs], gpr_[rt]);
                    lo_ = product.lo;
                    hi_ = product.hi;
                    break;
                }
                case 0x1E: { // DDIV
                    divide<s64>(gpr_[rs], gpr_[rt], lo_, hi_);
                    break;
                }
                case 0x1F: { // DDIVU
                    divide<u64>(gpr_[rs], gpr_[rt], lo_, hi_);
                    break;
                }
                case 0x20: { // ADD
                    const s32 a = static_cast<s32>(gpr_[rs]);
                    const s32 b = static_cast<s32>(gpr_[rt]);
                    s32 sum = 0;
                    if (!checkedAdd32(a, b, sum)) {
                        raiseException(12); // Ov
                        return;
                    }
                    writeGpr(rd, static_cast<s64>(sum));
                    break;
                }
                case 0x21: // ADDU
                    writeGpr(rd, static_cast<s32>(gpr_[rs] + gpr_[rt]));
                    break;
                case 0x22: { // SUB
                    const s32 a = static_cast<s32>(gpr_[rs]);
                    const s32 b = static_cast<s32>(gpr_[rt]);
                    s32 diff = 0;
                    if (!checkedSub32(a, b, diff)) {
                        raiseException(12);
                        return;
                    }
                    writeGpr(rd, static_cast<s64>(diff));
                    break;
                }
                case 0x23: // SUBU
                    writeGpr(rd, static_cast<s32>(gpr_[rs] - gpr_[rt]));
                    break;
                case 0x24: // AND
                    writeGpr(rd, gpr_[rs] & gpr_[rt]);
                    break;
                case 0x25: // OR
                    writeGpr(rd, gpr_[rs] | gpr_[rt]);
                    break;
                case 0x26: // XOR
                    writeGpr(rd, gpr_[rs] ^ gpr_[rt]);
                    break;
                case 0x27: // NOR
                    writeGpr(rd, ~(gpr_[rs] | gpr_[rt]));
                    break;
                case 0x2A: // SLT
                    writeGpr(rd, static_cast<s64>(gpr_[rs]) < static_cast<s64>(gpr_[rt]) ? 1 : 0);
                    break;
                case 0x2B: // SLTU
                    writeGpr(rd, gpr_[rs] < gpr_[rt] ? 1 : 0);
                    break;
                case 0x2C: { // DADD
                    const s64 a = static_cast<s64>(gpr_[rs]);
                    const s64 b = static_cast<s64>(gpr_[rt]);
                    s64 sum = 0;
                    if (!checkedAdd64(a, b, sum)) {
                        raiseException(12);
                        return;
                    }
                    writeGpr(rd, sum);
                    break;
                }
                case 0x2D: // DADDU
                    writeGpr(rd, gpr_[rs] + gpr_[rt]);
                    break;
                case 0x2E: { // DSUB
                    const s64 a = static_cast<s64>(gpr_[rs]);
                    const s64 b = static_cast<s64>(gpr_[rt]);
                    s64 diff = 0;
                    if (!checkedSub64(a, b, diff)) {
                        raiseException(12);
                        return;
                    }
                    writeGpr(rd, diff);
                    break;
                }
                case 0x2F: // DSUBU
                    writeGpr(rd, gpr_[rs] - gpr_[rt]);
                    break;
                case 0x38: // DSLL
                    writeGpr(rd, gpr_[rt] << shamt);
                    break;
                case 0x3A: // DSRL
                    writeGpr(rd, gpr_[rt] >> shamt);
                    break;
                case 0x3B: // DSRA
                    writeGpr(rd, static_cast<s64>(gpr_[rt]) >> shamt);
                    break;
                case 0x3C: // DSLL32
                    writeGpr(rd, gpr_[rt] << (shamt + 32));
                    break;
                case 0x3E: // DSRL32
                    writeGpr(rd, gpr_[rt] >> (shamt + 32));
                    break;
                case 0x3F: // DSRA32
                    writeGpr(rd, static_cast<s64>(gpr_[rt]) >> (shamt + 32));
                    break;
                default:
                    raiseException(10); // RI
                    return;
            }
            break;
        }
        case 0x01: { // REGIMM
            const u32 sub = rt;
            const s64 v = static_cast<s64>(gpr_[rs]);
            bool take = false;
            switch (sub) {
                case 0x00: take = v < 0; break;  // BLTZ
                case 0x01: take = v >= 0; break; // BGEZ
                case 0x02: take = v < 0; break;  // BLTZL
                case 0x03: take = v >= 0; break; // BGEZL
                case 0x10: // BLTZAL
                case 0x12: // BLTZALL
                    writeGpr(31, pc_ + 8);
                    take = v < 0;
                    break;
                case 0x11: // BGEZAL
                case 0x13: // BGEZALL
                    writeGpr(31, pc_ + 8);
                    take = v >= 0;
                    break;
                default:
                    raiseException(10);
                    return;
            }
            if (take) takeBranch(imm);
            else if (sub & 2) nextPc_ = pc_ + 8;
            else takeBranch(1);
            break;
        }
        case 0x02: // J
            branchTarget_ = ((pc_ + 4) & 0xFFFFFFFFF0000000ULL) | (static_cast<u64>(target) << 2);
            delaySlot_ = true;
            break;
        case 0x03: { // JAL
            writeGpr(31, pc_ + 8);
            branchTarget_ = ((pc_ + 4) & 0xFFFFFFFFF0000000ULL) | (static_cast<u64>(target) << 2);
            delaySlot_ = true;
            break;
        }
        case 0x04: // BEQ
            takeBranch(gpr_[rs] == gpr_[rt] ? imm : 1);
            break;
        case 0x05: // BNE
            takeBranch(gpr_[rs] != gpr_[rt] ? imm : 1);
            break;
        case 0x06: // BLEZ
            takeBranch(static_cast<s64>(gpr_[rs]) <= 0 ? imm : 1);
            break;
        case 0x07: // BGTZ
            takeBranch(static_cast<s64>(gpr_[rs]) > 0 ? imm : 1);
            break;
        case 0x14: case 0x15: case 0x16: case 0x17: { // BEQL, BNEL, BLEZL, BGTZL
            const auto value = static_cast<s64>(gpr_[rs]);
            const bool take = op == 0x14 ? gpr_[rs] == gpr_[rt] :
                              op == 0x15 ? gpr_[rs] != gpr_[rt] :
                              op == 0x16 ? value <= 0 : value > 0;
            if (take) takeBranch(imm);
            else nextPc_ = pc_ + 8; // annul the delay-slot instruction
            break;
        }
        case 0x08: { // ADDI
            const s32 a = static_cast<s32>(gpr_[rs]);
            const s32 b = imm;
            s32 sum = 0;
            if (!checkedAdd32(a, b, sum)) {
                raiseException(12);
                return;
            }
            writeGpr(rt, static_cast<s64>(sum));
            break;
        }
        case 0x09: // ADDIU
            writeGpr(rt, static_cast<s32>(gpr_[rs] + static_cast<s64>(imm)));
            break;
        case 0x0A: // SLTI
            writeGpr(rt, static_cast<s64>(gpr_[rs]) < static_cast<s64>(imm) ? 1 : 0);
            break;
        case 0x0B: // SLTIU
            writeGpr(rt, gpr_[rs] < static_cast<u64>(static_cast<s64>(imm)) ? 1 : 0);
            break;
        case 0x0C: // ANDI
            writeGpr(rt, gpr_[rs] & static_cast<u64>(static_cast<u16>(imm)));
            break;
        case 0x0D: // ORI
            writeGpr(rt, gpr_[rs] | static_cast<u64>(static_cast<u16>(imm)));
            break;
        case 0x0E: // XORI
            writeGpr(rt, gpr_[rs] ^ static_cast<u64>(static_cast<u16>(imm)));
            break;
        case 0x0F: // LUI
            writeGpr(rt, static_cast<s32>(static_cast<u32>(static_cast<u16>(imm)) << 16));
            break;
        case 0x18: { // DADDI
            const s64 a = static_cast<s64>(gpr_[rs]);
            const s64 b = static_cast<s64>(imm);
            s64 sum = 0;
            if (!checkedAdd64(a, b, sum)) {
                raiseException(12);
                return;
            }
            writeGpr(rt, sum);
            break;
        }
        case 0x19: // DADDIU
            writeGpr(rt, gpr_[rs] + static_cast<s64>(imm));
            break;
        case 0x10: // COP0
            cop0Execute(instr, rs, rt, rd, funct);
            break;
        case 0x11: // COP1
            cop1Execute(instr, rs, rt, rd, funct);
            break;
        case 0x12: // COP2 register transfers
        case 0x32: case 0x36: case 0x3A: case 0x3E: { // COP2 memory encodings
            const auto cop2Exception = [&](u32 code) { raiseException(code, false, 2); };
            if (!(cop0_[C0_STATUS] & (1ULL << 30))) {
                cop2Exception(11);
                break;
            }
            // VR4300 has a single transfer latch and no COP2 execution unit.
            if (op != 0x12) {
                cop2Exception(10);
                break;
            }
            switch (rs) {
                case 0x00: case 0x02: writeGpr(rt, signExtend32(cop2Latch_)); break;
                case 0x01: writeGpr(rt, cop2Latch_); break;
                case 0x04: case 0x05: case 0x06: cop2Latch_ = gpr_[rt]; break;
                default: cop2Exception(10); break;
            }
            break;
        }
        case 0x20: { // LB
            writeGpr(rt, static_cast<s8>(load<u8>(bus, gpr_[rs] + static_cast<s64>(imm))));
            break;
        }
        case 0x21: { // LH
            writeGpr(rt, static_cast<s16>(load<u16>(bus, gpr_[rs] + static_cast<s64>(imm))));
            break;
        }
        case 0x22: case 0x26: { // LWL / LWR (big-endian)
            const u64 address = gpr_[rs] + static_cast<s64>(imm);
            const u32 word = load<u32>(bus, address & ~3ULL);
            const u32 offset = address & 3;
            if (op == 0x22) {
                const u32 shift = offset * 8;
                const u32 keep = (1u << shift) - 1;
                writeGpr(rt, signExtend32((gpr_[rt] & keep) | (word << shift)));
            } else if (offset == 3) {
                writeGpr(rt, signExtend32(word));
            } else {
                // A partial LWR does not replace bit 31 and preserves the high word.
                const u32 shift = (3 - offset) * 8;
                const u64 mask = 0xFFFFFFFFu >> shift;
                writeGpr(rt, (gpr_[rt] & ~mask) | (word >> shift));
            }
            break;
        }
        case 0x23: { // LW
            writeGpr(rt, static_cast<s32>(load<u32>(bus, gpr_[rs] + static_cast<s64>(imm))));
            break;
        }
        case 0x24: { // LBU
            writeGpr(rt, load<u8>(bus, gpr_[rs] + static_cast<s64>(imm)));
            break;
        }
        case 0x25: { // LHU
            writeGpr(rt, load<u16>(bus, gpr_[rs] + static_cast<s64>(imm)));
            break;
        }
        case 0x1A: case 0x1B: { // LDL / LDR (big-endian)
            const u64 address = gpr_[rs] + static_cast<s64>(imm);
            const u64 word = load<u64>(bus, address & ~7ULL);
            const u32 offset = address & 7;
            if (op == 0x1A) {
                const u32 shift = offset * 8;
                const u64 keep = (1ULL << shift) - 1;
                writeGpr(rt, (gpr_[rt] & keep) | (word << shift));
            } else {
                const u32 shift = (7 - offset) * 8;
                const u64 mask = ~0ULL >> shift;
                writeGpr(rt, (gpr_[rt] & ~mask) | (word >> shift));
            }
            break;
        }
        case 0x27: { // LWU
            writeGpr(rt, load<u32>(bus, gpr_[rs] + static_cast<s64>(imm)));
            break;
        }
        case 0x37: { // LD
            writeGpr(rt, load<u64>(bus, gpr_[rs] + static_cast<s64>(imm)));
            break;
        }
        case 0x28: { // SB
            store(bus, gpr_[rs] + static_cast<s64>(imm), static_cast<u8>(gpr_[rt]));
            break;
        }
        case 0x29: { // SH
            store(bus, gpr_[rs] + static_cast<s64>(imm), static_cast<u16>(gpr_[rt]));
            break;
        }
        case 0x2A: case 0x2E: case 0x2C: case 0x2D: { // SWL / SWR / SDL / SDR
            const u32 address = static_cast<u32>(translateAddress(gpr_[rs] + static_cast<s64>(imm), true));
            const u32 size = (op == 0x2C || op == 0x2D) ? 8 : 4;
            const u32 aligned = address & ~(size - 1);
            const bool left = op == 0x2A || op == 0x2C;
            const u32 start = left ? address : aligned;
            const u32 end = left ? aligned + size : address + 1;
            // Use only the enabled byte lanes; do not read/modify/write neighboring bytes.
            const u32 sourceEnd = left ? start + size : end;
            for (u32 dst = start; dst != end;) {
                const u32 remaining = end - dst;
                const u32 width = remaining >= 8 && !(dst & 7) ? 8 :
                                  remaining >= 4 && !(dst & 3) ? 4 :
                                  remaining >= 2 && !(dst & 1) ? 2 : 1;
                const u64 value = gpr_[rt] >> ((sourceEnd - dst - width) * 8);
                if (width == 8) bus.write64(dst, value);
                else if (width == 4) bus.write32(dst, static_cast<u32>(value));
                else if (width == 2) bus.write16(dst, static_cast<u16>(value));
                else bus.write8(dst, static_cast<u8>(value));
                dst += width;
            }
            break;
        }
        case 0x2B: { // SW
            store(bus, gpr_[rs] + static_cast<s64>(imm), static_cast<u32>(gpr_[rt]));
            break;
        }
        case 0x2F: { // CACHE: no cache state in this interpreter.
            // Only direct RDRAM instruction lines can be native blocks. Scope
            // the coherency hint to that line instead of dropping the graph.
            const u64 address = gpr_[rs] + static_cast<s64>(imm);
            const u32 low = static_cast<u32>(address);
            constexpr u32 mask = ~(Memory::RdramSize - 1u) & ~0x20000000u;
            if (jit_ && address == signExtend32(low) &&
                (low & mask) == 0x80000000u)
                jit_->invalidateCode((low & (Memory::RdramSize - 1u)) & ~63u, 64);
            break;
        }
        case 0x30: case 0x34: { // LL / LLD
            const bool wide = op == 0x34;
            const u64 address = gpr_[rs] + static_cast<s64>(imm);
            if (address & (wide ? 7 : 3)) addressError(address, false);
            const auto paddr = resolveMemory(address, false);
            writeGpr(rt, wide ? bus.read64(paddr) : signExtend32(bus.read32(paddr)));
            llBit_ = true;
            cop0_[C0_LLADDR] = paddr >> 4;
            break;
        }
        case 0x38: case 0x3C: { // SC / SCD
            if (!llBit_) {
                writeGpr(rt, 0);
                break;
            }
            const bool wide = op == 0x3C;
            const u64 address = gpr_[rs] + static_cast<s64>(imm);
            if (wide) store(bus, address, gpr_[rt]);
            else store(bus, address, static_cast<u32>(gpr_[rt]));
            writeGpr(rt, 1);
            break;
        }
        case 0x31: case 0x35: case 0x39: case 0x3D: { // LWC1, LDC1, SWC1, SDC1
            if (!requireCop1()) return;
            const bool store = (op & 8) != 0;
            const bool wide = (op & 4) != 0;
            const u64 address = gpr_[rs] + static_cast<s64>(imm);
            if (store) {
                if (wide) this->store(bus, address, fpr_[fprPhys(rt)]);
                else this->store(bus, address, fprWord(rt));
            } else {
                if (wide) fpr_[fprPhys(rt)] = load<u64>(bus, address);
                else setFprWord(rt, load<u32>(bus, address));
            }
            break;
        }
        case 0x3F: { // SD
            store(bus, gpr_[rs] + static_cast<s64>(imm), gpr_[rt]);
            break;
        }
        default:
            raiseException(10); // RI
            return;
    }
}

void Cpu::step(Bus& bus) { stepImpl<false>(bus); }
void Cpu::stepCached(Bus& bus) { stepImpl<true>(bus); }

template<bool Cached> void Cpu::stepImpl(Bus& bus) {
    if (attachedBus_ != &bus) attachToBus(bus);

    countHalfCycle_ = !countHalfCycle_;
    if (!countHalfCycle_) {
        cop0_[C0_COUNT] = static_cast<u32>(cop0_[C0_COUNT] + 1);
        if (cop0_[C0_COUNT] == static_cast<u32>(cop0_[C0_COMPARE])) {
            cop0_[C0_CAUSE] |= 0x8000;
        }
    }
    constexpr u64 MiPending = 1u << 10; // COP0 Cause IP2
    cop0_[C0_CAUSE] = (cop0_[C0_CAUSE] & ~MiPending) |
                      (bus.hw().mi().irq() ? MiPending : 0);
    const auto status = cop0_[C0_STATUS];
    if (!delaySlot_ && (status & 7) == 1 &&
        (status & cop0_[C0_CAUSE] & 0xFF00) != 0) {
        raiseException(0);
        setPc(branchTarget_);
        return;
    }
    bool wasDelaySlot = false;
    try {
        wasDelaySlot = delaySlot_;
        if constexpr (Cached) {
            if (isDirectFetch(pc_)) {
                const auto& instr = cachedInstruction(bus);
                ++instructionsDispatched_;
                if (instr.raw != 0) instr.handler(*this, bus, instr); // SLL r0,r0,0
            } else {
                const u32 instr = fetch(bus);
                ++instructionsDispatched_;
                execute(instr, bus);
            }
        } else {
            const u32 instr = fetch(bus);
            ++instructionsDispatched_;
            execute(instr, bus);
        }
    } catch (const AccessException& ex) {
        // Address-translation failure: raise the mapped Coprocessor exception
        // (TLB inval/miss/modified) and let the state machine in this function
        // dispatch to the exception vector instead of propagating.
        raiseException(ex.causeCode(), ex.refill());
    } catch (const std::out_of_range&) {
        // Bus-level access failure (e.g. unmapped physical address reached via
        // a successful translation): raise as a Coprocessor address error and
        // let the state machine dispatch instead of crashing the host.
        raiseException(4); // EXCP cause 4 = Address error
    }

    if (exceptionOrEretTaken_) {
        pc_ = branchTarget_;
        nextPc_ = pc_ + 4;
        exceptionOrEretTaken_ = false;
        delaySlot_ = false;
        branchTarget_ = 0;
    } else if (wasDelaySlot) {
        pc_ = branchTarget_;
        nextPc_ = pc_ + 4;
        delaySlot_ = false;
        branchTarget_ = 0;
    } else {
        pc_ = nextPc_;
        nextPc_ = pc_ + 4;
    }
}

void Cpu::setPc(u64 pc) noexcept {
    pc_ = pc;
    nextPc_ = pc + 4;
    delaySlot_ = false;
    exceptionOrEretTaken_ = false;
    branchTarget_ = 0;
}

void Cpu::setIplHandoffRegisters(u32 tvType, u32 version, u64 s6, u64 returnAddress) noexcept {
    gpr_[19] = 0; // cartridge
    gpr_[20] = tvType;
    gpr_[21] = 0; // cold reset
    gpr_[22] = s6;
    gpr_[23] = version;
    gpr_[29] = 0xFFFFFFFFA4001FF0ULL;
    gpr_[31] = returnAddress;
    cop0_[C0_STATUS] = 0x34000000; // IPL1 enables CU0, CU1 and FR
    cop0_[C0_CONFIG] = 0x0006E463;
}

} // namespace n64forge
