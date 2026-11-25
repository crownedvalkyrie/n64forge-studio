#include "jit_arm64.hpp"
#include "n64forge/core/memory.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(__APPLE__) && defined(__aarch64__) && !defined(__arm64e__)
#define N64FORGE_NATIVE_JIT 1
#include <libkern/OSCacheControl.h>
#include <pthread.h>
#include <sys/mman.h>
#endif

namespace n64forge {
namespace {

// Number of whole instructions that may be fetched starting at a physical RDRAM
// PC without leaving the 64-byte line or RDRAM itself. Exposed for the safety
// tests that pin the native translation window.
[[nodiscard]] bool directFetch(u64 vaddr) noexcept {
    constexpr u32 mask = ~(Memory::RdramSize - 1u) & ~0x20000000u;
    const u32 low = static_cast<u32>(vaddr);
    return vaddr == static_cast<u64>(static_cast<s64>(static_cast<s32>(low))) &&
           (low & (mask | 3u)) == 0x80000000u;
}

[[nodiscard]] u32 physicalPc(u64 vaddr) noexcept {
    return static_cast<u32>(vaddr) & (Memory::RdramSize - 1);
}

} // namespace

[[nodiscard]] u32 jitTranslationWindow(u32 pc) noexcept {
    if (pc >= Memory::RdramSize) return 0;
    const u32 lineWindow = 16 - ((pc >> 2) & 15);
    const u32 rdramWindow = (Memory::RdramSize - pc) / 4;
    return std::min(lineWindow, rdramWindow);
}

#ifdef N64FORGE_NATIVE_JIT
namespace {
// Callee-saved host registers used across a whole chained native invocation.
// The block budget and retired count stay live across chained blocks. A
// block-local GPR cache in volatile registers is flushed before transferring.
constexpr u32 Ctx = 19;     // Arm64Jit::Context*
constexpr u32 GprBase = 20; // u64[32]
constexpr u32 HiBase = 21;
constexpr u32 LoBase = 22;
constexpr u32 FprBase = 23;
constexpr u32 FcsrBase = 24;
constexpr u32 Budget = 25;
constexpr u32 Retired = 26;
constexpr u32 Pc = 27;
constexpr u32 Tmp = 28;

constexpr u32 A = 9;
constexpr u32 B = 10;
constexpr u32 T1 = 11;
constexpr u32 T2 = 12;
constexpr u32 T3 = 13;

static_assert(offsetof(Arm64Jit::Context, gpr) == 0 &&
              offsetof(Arm64Jit::Context, hi) == 8 &&
              offsetof(Arm64Jit::Context, lo) == 16 &&
              offsetof(Arm64Jit::Context, fpr) == 24 &&
              offsetof(Arm64Jit::Context, fcsr) == 32 &&
              offsetof(Arm64Jit::Context, budget) == 40 &&
              offsetof(Arm64Jit::Context, retired) == 44 &&
              offsetof(Arm64Jit::Context, pc) == 48 &&
              offsetof(Arm64Jit::Context, cu1) == 56 &&
              offsetof(Arm64Jit::Context, mode) == 60 &&
              offsetof(Arm64Jit::Context, rdram) == 64 &&
              offsetof(Arm64Jit::Context, lineGen) == 72 &&
              offsetof(Arm64Jit::Context, lineActive) == 80 &&
              offsetof(Arm64Jit::Context, dispatch) == 88);
static_assert(sizeof(std::atomic<u32>) == sizeof(u32) && std::atomic<u32>::is_always_lock_free);

enum Cond : u32 {
    CondEq = 0, CondNe = 1, CondCs = 2, CondCc = 3, CondMi = 4, CondPl = 5,
    CondVs = 6, CondVc = 7, CondHi = 8, CondLs = 9, CondGe = 10, CondLt = 11,
    CondGt = 12, CondLe = 13, CondAl = 14,
};

class Asm {
public:
    struct Fixup { u32 at, label, kind; };

    std::vector<u32> code;
    std::vector<u32> labels;
    std::vector<Fixup> fixups;

    u32 label() {
        labels.push_back(0xFFFFFFFFu);
        return static_cast<u32>(labels.size() - 1);
    }
    void bind(u32 label) { labels[label] = static_cast<u32>(code.size()); }
    void emit(u32 word) { code.push_back(word); }
    void reg(u32 op, u32 d, u32 n, u32 m) { emit(op | (m << 16) | (n << 5) | d); }
    void bits(u32 op, u32 d, u32 n, u32 r, u32 s) {
        emit(op | (r << 16) | (s << 10) | (n << 5) | d);
    }

    void b(u32 target) {
        emit(0x14000000u);
        fixups.push_back({static_cast<u32>(code.size() - 1), target, 0});
    }
    void bcond(u32 cond, u32 target) {
        emit(0x54000000u | (cond & 0xFu));
        fixups.push_back({static_cast<u32>(code.size() - 1), target, 1});
    }
    void cbnz(u32 rt, u32 target) {
        emit(0x35000000u | rt);
        fixups.push_back({static_cast<u32>(code.size() - 1), target, 2});
    }
    void cbz(u32 rt, u32 target) {
        emit(0x34000000u | rt);
        fixups.push_back({static_cast<u32>(code.size() - 1), target, 2});
    }
    void cbz64(u32 rt, u32 target) {
        cbz(rt, target);
        code.back() |= 1u << 31;
    }
    void tbnz(u32 rt, u32 bit, u32 target) {
        emit(0x37000000u | ((bit & 31u) << 19) | rt);
        fixups.push_back({static_cast<u32>(code.size() - 1), target, 3});
    }
    void tbz(u32 rt, u32 bit, u32 target) {
        emit(0x36000000u | ((bit & 31u) << 19) | rt);
        fixups.push_back({static_cast<u32>(code.size() - 1), target, 3});
    }

    void movImm64(u32 d, u64 value) {
        emit(0xd2800000u | (static_cast<u32>(value & 0xFFFFu) << 5) | d);
        for (u32 shift = 16; shift < 64; shift += 16) {
            const u32 part = static_cast<u32>((value >> shift) & 0xFFFFu);
            if (part) emit(0xf2800000u | ((shift / 16) << 21) | (part << 5) | d);
        }
    }
    void movVirtualPc(u32 low) {
        emit(0x52800000u | ((low & 0xFFFFu) << 5) | Pc);
        if (low >> 16)
            emit(0x72a00000u | (1u << 21) | (((low >> 16) & 0xFFFFu) << 5) | Pc);
        emit(0x93407c00u | (Pc << 5) | Pc); // sxtw x27, w27
    }
    void addImm32(u32 rd, u32 imm) {
        emit(0x11000000u | ((imm & 0xFFFu) << 10) | (rd << 5) | rd);
    }
    void addImm64(u32 rd, s64 imm) {
        if (!imm) return;
        if (imm > 0 && imm <= 4095) {
            emit(0x91000000u | ((static_cast<u32>(imm) & 0xFFFu) << 10) | (rd << 5) | rd);
        } else if (imm < 0 && imm >= -4095) {
            emit(0xD1000000u | ((static_cast<u32>(-imm) & 0xFFFu) << 10) | (rd << 5) | rd);
        } else {
            // B holds the 64-bit two's-complement value; ADD applies both
            // positive and negative large offsets without flipping the sign.
            movImm64(B, static_cast<u64>(imm));
            reg(0x8B000000u, rd, rd, B);
        }
    }
    void subImm32(u32 rd, u32 imm) {
        emit(0x51000000u | ((imm & 0xFFFu) << 10) | (rd << 5) | rd);
    }
    void addReg64(u32 rd, u32 rn, u32 rm) { reg(0x8B000000u, rd, rn, rm); }
    void movReg64(u32 rd, u32 rn) { reg(0xAA000000u, rd, 31, rn); }
    void lsr64(u32 rd, u32 rn, u32 shift) {
        emit(0xD3400000u | ((shift & 63u) << 16) | (63u << 10) | (rn << 5) | rd);
    }
    void lsl64(u32 rd, u32 rn, u32 shift) {
        const u32 s = shift & 63u;
        emit(0xD3400000u | (((64u - s) & 63u) << 16) | ((63u - s) << 10) |
             (rn << 5) | rd);
    }
    void cmpImm32(u32 rn, u32 imm) {
        emit(0x71000000u | ((imm & 0xFFFu) << 10) | (rn << 5) | 31);
    }
    void cmpZero64(u32 rn) { emit(0xf100001fu | (rn << 5)); }
    void cmpReg64(u32 rn, u32 rm) { emit(0xeb000000u | (rm << 16) | (rn << 5) | 31); }
    void cset32(u32 rd, u32 cond) {
        emit(0x1a9f07e0u | (((cond ^ 1u) & 0xFu) << 12) | rd);
    }
    void orr32(u32 d, u32 n, u32 m) { reg(0x2a000000u, d, n, m); }

    // Only leaf code uses these; x0 is free after the prologue. Each cold
    // exit records which writes had executed when that exit was emitted.
    static constexpr std::array<u32, 10> CacheHosts{0, 1, 2, 3, 4, 5, 6, 7, 14, 15};
    std::array<u32, CacheHosts.size()> cachedGuests{};
    u32 dirtyGpr = 0;
    u32 pinnedGpr = 0;

    void pinGprs() {
        for (u32 i = 0; i < cachedGuests.size() && cachedGuests[i]; ++i) {
            emit(0xf9400000u | (cachedGuests[i] << 10) | (GprBase << 5) | CacheHosts[i]);
            pinnedGpr |= 1u << i;
        }
    }
    u32 writebackMask() const { return dirtyGpr | pinnedGpr; }

    u32 gprOperand(u32 guest, u32 scratch) {
        if (!guest) return 31; // XZR/WZR for register forms only.
        for (u32 i = 0; i < cachedGuests.size(); ++i) {
            if (cachedGuests[i] == guest && (writebackMask() & (1u << i))) return CacheHosts[i];
        }
        emit(0xf9400000u | (guest << 10) | (GprBase << 5) | scratch);
        return scratch;
    }
    void ldrGpr(u32 dst, u32 guest) {
        const u32 source = gprOperand(guest, dst);
        if (source != dst) movReg64(dst, source);
    }
    u32 gprDestination(u32 guest, u32 scratch) const {
        if (guest)
            for (u32 i = 0; i < cachedGuests.size(); ++i)
                if (cachedGuests[i] == guest) return CacheHosts[i];
        return scratch;
    }
    void strGpr(u32 guest, u32 src) {
        if (!guest) return;
        for (u32 i = 0; i < cachedGuests.size(); ++i) {
            if (cachedGuests[i] == guest) {
                if (CacheHosts[i] != src) movReg64(CacheHosts[i], src);
                dirtyGpr |= 1u << i;
                return;
            }
        }
        storeGpr(guest, src);
    }
    void storeGpr(u32 guest, u32 src) {
        emit(0xf9000000u | (guest << 10) | (GprBase << 5) | src);
    }
    void flushGpr(u32 mask) {
        for (u32 i = 0; i < cachedGuests.size(); ++i)
            if (mask & (1u << i)) storeGpr(cachedGuests[i], CacheHosts[i]);
    }
    void ldrHi(u32 dst) { emit(0xf9400000u | (HiBase << 5) | dst); }
    void strHi(u32 src) { emit(0xf9000000u | (HiBase << 5) | src); }
    void ldrLo(u32 dst) { emit(0xf9400000u | (LoBase << 5) | dst); }
    void strLo(u32 src) { emit(0xf9000000u | (LoBase << 5) | src); }

    bool fullFpr = true;
    void fprMemory(bool load, bool wide, u32 reg, u32 guest) {
        const u32 slot = fullFpr ? guest : (guest & ~1u);
        // S registers occupy the low word except for odd paired registers
        // with FR=0. STR S updates exactly that word without clearing its pair.
        const u32 offset = wide ? slot : slot * 2u + (slot != guest ? 1u : 0u);
        emit((load ? 0xbd400000u : 0xbd000000u) | (wide ? (1u << 30) : 0u) |
             (offset << 10) | (FprBase << 5) | reg);
    }
    void fprBits(bool load, bool wide, u32 reg, u32 guest, bool pairedWord = true) {
        const u32 slot = fullFpr ? guest : (guest & ~1u);
        const u32 offset = wide ? slot : slot * 2u + (pairedWord && slot != guest ? 1u : 0u);
        emit((load ? 0xb9400000u : 0xb9000000u) | (wide ? (1u << 30) : 0u) |
             (offset << 10) | (FprBase << 5) | reg);
    }
    void ldrFcsr(u32 dst) { emit(0xb9400000u | (FcsrBase << 5) | dst); }
    void strFcsr(u32 src) { emit(0xb9000000u | (FcsrBase << 5) | src); }
    void ldrBudget(u32 dst) { emit(0xb9400000u | (10u << 10) | (Ctx << 5) | dst); }
    void strRetired(u32 src) { emit(0xb9000000u | (11u << 10) | (Ctx << 5) | src); }
    void strPc(u32 src) { emit(0xf9000000u | (6u << 10) | (Ctx << 5) | src); }
    void ldrCu1(u32 dst) { emit(0xb9400000u | (14u << 10) | (Ctx << 5) | dst); }
    void ldrRdram(u32 dst) { emit(0xf9400000u | (8u << 10) | (Ctx << 5) | dst); }
    void ldrLineGen(u32 dst) { emit(0xf9400000u | (9u << 10) | (Ctx << 5) | dst); }
    void ldrLineActive(u32 dst) { emit(0xf9400000u | (10u << 10) | (Ctx << 5) | dst); }

    void loadMemory(u32 dst, u32 base, u32 size) {
        const u32 opcode = size == 8 ? 0xF9400000u : size == 4 ? 0xB9400000u :
                           size == 2 ? 0x79400000u : 0x39400000u;
        emit(opcode | (base << 5) | dst);
        reverseBytes(dst, size);
    }
    void storeMemory(u32 src, u32 base, u32 size) {
        const u32 opcode = size == 8 ? 0xF9000000u : size == 4 ? 0xB9000000u :
                           size == 2 ? 0x79000000u : 0x39000000u;
        emit(opcode | (base << 5) | src);
    }
    void reverseBytes(u32 rd, u32 size) {
        if (size == 1) return;
        const u32 op = size == 8 ? 0xdac00c00u : size == 4 ? 0x5ac00800u : 0x5ac00400u;
        emit(op | (rd << 5) | rd);
    }
    void staddl32(u32 value, u32 base) {
        emit(0xb860001fu | (value << 16) | (base << 5));
    }
    void ldrbIndex(u32 dst, u32 base, u32 index) {
        emit(0x38606800u | (index << 16) | (base << 5) | dst);
    }

    void fcsrSetBit23(u32 cond) {
        cset32(A, cond);
        ldrFcsr(B);
        emit(0x3309012au); // bfi w10, w9, #23, #1
        strFcsr(B);
    }
    void fcsrClearBit23() {
        ldrFcsr(B);
        emit(0x330903eau); // bfi w10, wzr, #23, #1
        strFcsr(B);
    }

    void prologue() {
        emit(0xd10183ffu); // sub sp, sp, #96
        emit(0xa90053f3u); // stp x19, x20, [sp]
        emit(0xa9015bf5u); // stp x21, x22, [sp, #16]
        emit(0xa90263f7u); // stp x23, x24, [sp, #32]
        emit(0xa9036bf9u); // stp x25, x26, [sp, #48]
        emit(0xa90473fbu); // stp x27, x28, [sp, #64]
        reg(0xaa000000u, Ctx, 31, 0); // mov x19, x0
        emit(0xf9400000u | (Ctx << 5) | GprBase);
        emit(0xf9400000u | (1u << 10) | (Ctx << 5) | HiBase);
        emit(0xf9400000u | (2u << 10) | (Ctx << 5) | LoBase);
        emit(0xf9400000u | (3u << 10) | (Ctx << 5) | FprBase);
        emit(0xf9400000u | (4u << 10) | (Ctx << 5) | FcsrBase);
        ldrBudget(Budget);
        emit(0x2a1f03e0u | Retired); // mov w26, wzr
    }
    void epilogue() {
        strRetired(Retired);
        strPc(Pc);
        emit(0xa94053f3u);
        emit(0xa9415bf5u);
        emit(0xa94263f7u);
        emit(0xa9436bf9u);
        emit(0xa94473fbu);
        emit(0x910183ffu); // add sp, sp, #96
        emit(0xd65f03c0u); // ret
    }

    void finish() {
        for (const auto& f : fixups) {
            const s64 delta = static_cast<s64>(labels[f.label]) - static_cast<s64>(f.at);
            const u32 d = static_cast<u32>(delta);
            switch (f.kind) {
                case 0: code[f.at] |= d & 0x03FFFFFFu; break;
                case 1: code[f.at] |= (d & 0x7FFFFu) << 5; break;
                case 2: code[f.at] |= (d & 0x7FFFFu) << 5; break;
                case 3: code[f.at] |= (d & 0x3FFFu) << 5; break;
                default: break;
            }
        }
    }
};

enum class BranchKind { None, Eq, Ne, Lez, Gtz, Bltz, Bgez, Bc1 };

struct BranchDesc {
    u32 index = 0;
    u64 target = 0;
    bool likely = false;
    bool link = false;
    bool unconditional = false;
    bool bc1True = false;
    bool indirect = false;
    BranchKind kind = BranchKind::None;
    u32 rs = 0, rt = 0, linkReg = 31;
};

struct Bail {
    u32 label = 0;
    u32 executedBefore = 0;
    u64 pc = 0;
    u32 dirtyGpr = 0;
};

struct EdgeSpec {
    u64 targetVpc = 0;
    u32 site = 0;
    u32 thunk = 0;
};

class Compiler {
public:
    Compiler(const Memory& ram, u64 vpc, bool cu1, bool fullFpr)
        : ram_(ram), vpc_(vpc), cu1_(cu1) {
        asm_.fullFpr = fullFpr;
    }

    bool plan() {
        u32 count = 0;
        while (count < 16) {
            const u64 slotPc = vpc_ + static_cast<u64>(count) * 4;
            if (!directFetch(slotPc)) break;
            const u32 raw = ram_.loadRdram<u32>(physicalPc(slotPc));
            BranchDesc branch;
            if (decodeBranch(raw, slotPc, branch)) {
                const u64 delayPc = slotPc + 4;
                if (!directFetch(delayPc)) break;
                const u32 delay = ram_.loadRdram<u32>(physicalPc(delayPc));
                // A non-linking branch can be replayed if its delay slot
                // bails: no branch side effects were committed. Linking
                // branches with faultable delay slots stay in the interpreter.
                if (isControlFlow(delay) || (branch.link && mayBail(delay)) ||
                    !canTranslate(delay)) break;
                raws_.push_back(raw);
                raws_.push_back(delay);
                branch_ = branch;
                branch_.index = count;
                hasBranch_ = true;
                steps_ = count + 2;
                needsCop1_ = anyCop1(raws_.data(), raws_.size());
                return true;
            }
            if (!canTranslate(raw)) break;
            raws_.push_back(raw);
            ++count;
        }
        steps_ = count;
        needsCop1_ = anyCop1(raws_.data(), raws_.size());
        // A one-instruction native block costs more than it saves: the caller
        // would return to C++ after every instruction. Keep those prefixes in
        // the interpreter, as the original JIT contract did.
        return count >= 2;
    }

    void emit() {
        std::array<u32, 32> uses{};
        for (u32 i = 0; i < raws_.size(); ++i) {
            const u32 raw = raws_[i];
            const u32 op = raw >> 26, rs = (raw >> 21) & 31, rt = (raw >> 16) & 31;
            if (!raw || op == 2 || op == 3) continue;
            if (op == 0x11) {
                if (rs <= 1 || rs == 4 || rs == 5) ++uses[rt];
                continue;
            }
            ++uses[rs];
            if (!isFpMemory(op) && op != 1 && op != 6 && op != 7 && op != 0x16 && op != 0x17)
                ++uses[rt];
            if (!op) ++uses[(raw >> 11) & 31];
        }
        uses[0] = 0;
        for (auto& guest : asm_.cachedGuests) {
            const auto best = std::max_element(uses.begin(), uses.end());
            if (*best < 2) break;
            guest = static_cast<u32>(best - uses.begin());
            *best = 0;
        }
        asm_.prologue();
        // Every entry, direct or chained, commits the whole block here. A
        // chained branch lands after the prologue but before the guard so
        // budget and CU1 remain exact for each guest block.
        chainLabel_ = asm_.label();
        asm_.bind(chainLabel_);
        startBailLabel_ = asm_.label();
        epilogueLabel_ = asm_.label();

        if (needsCop1_) {
            asm_.ldrCu1(Tmp);
            asm_.cbz(Tmp, startBailLabel_);
        }
        asm_.cmpImm32(Budget, steps_);
        asm_.bcond(CondCc, startBailLabel_); // unsigned lower: budget < steps
        if (hasBranch_ && !branch_.link && !branch_.indirect && branch_.target == vpc_ &&
            !(branch_.index == 0 && raws_[1] == 0)) {
            asm_.pinGprs();
            loopLabel_ = asm_.label();
            asm_.bind(loopLabel_);
        }
        asm_.subImm32(Budget, steps_);
        asm_.addImm32(Retired, steps_);

        for (u32 i = 0; i < raws_.size(); ++i) {
            if (hasBranch_ && i == branch_.index) {
                emitBranch();
                i += 1; // the delay slot was emitted by emitBranch
                continue;
            }
            emitInstruction(raws_[i], vpc_ + static_cast<u64>(i) * 4, i);
        }

        if (!hasBranch_) emitChain(vpc_ + static_cast<u64>(steps_) * 4);

        // Cold exits: budget/CU1 guard and any signaling-compare rollback.
        asm_.bind(startBailLabel_);
        asm_.movVirtualPc(static_cast<u32>(vpc_));
        asm_.b(epilogueLabel_);
        for (const auto& bail : bails_) {
            asm_.bind(bail.label);
            asm_.flushGpr(bail.dirtyGpr);
            const u32 remaining = steps_ - bail.executedBefore;
            if (remaining) {
                asm_.addImm32(Budget, remaining);
                asm_.subImm32(Retired, remaining);
            }
            asm_.movVirtualPc(static_cast<u32>(bail.pc));
            asm_.b(epilogueLabel_);
        }
        asm_.bind(epilogueLabel_);
        asm_.epilogue();
        for (const auto& edge : edges_) {
            asm_.bind(edge.thunk);
            asm_.movVirtualPc(static_cast<u32>(edge.targetVpc));
            asm_.b(epilogueLabel_);
        }
    }

    [[nodiscard]] const Asm& asmCode() const noexcept { return asm_; }
    [[nodiscard]] Asm& asmCode() noexcept { return asm_; }
    [[nodiscard]] u32 steps() const noexcept { return steps_; }
    [[nodiscard]] const std::vector<EdgeSpec>& edges() const noexcept { return edges_; }
    [[nodiscard]] u32 chainIndex() const noexcept { return asm_.labels[chainLabel_]; }

private:
    [[nodiscard]] static bool anyCop1(const u32* raws, std::size_t count) noexcept {
        for (std::size_t i = 0; i < count; ++i)
            if ((raws[i] >> 26) == 0x11 || isFpMemory(raws[i] >> 26)) return true;
        return false;
    }

    [[nodiscard]] bool decodeBranch(u32 raw, u64 pc, BranchDesc& out) const noexcept {
        const u32 op = raw >> 26, rs = (raw >> 21) & 31, rt = (raw >> 16) & 31;
        const s32 offset = static_cast<s16>(raw & 0xFFFF);
        const auto target = [&](s32 displacement) {
            return pc + 4 + static_cast<s64>(displacement) * 4;
        };
        if (op == 0 && ((raw & 63) == 8 || (raw & 63) == 9)) {
            const u32 rd = (raw >> 11) & 31;
            const bool jalr = (raw & 63) == 9;
            if (rt || ((raw >> 6) & 31) || (!jalr && rd)) return false;
            out.unconditional = out.indirect = true;
            out.rs = rs;
            out.link = jalr;
            out.linkReg = rd;
            return true;
        }
        if (op == 0x02 || op == 0x03) { // J / JAL
            out.target = ((pc + 4) & 0xFFFFFFFFF0000000ULL) |
                         (static_cast<u64>(raw & 0x03FFFFFFu) << 2);
            out.unconditional = true;
            out.link = op == 0x03;
            out.kind = BranchKind::None;
            return true;
        }
        if (op == 0x04 || op == 0x05 || op == 0x06 || op == 0x07 ||
            op == 0x14 || op == 0x15 || op == 0x16 || op == 0x17) {
            out.unconditional = false;
            out.likely = op >= 0x14;
            out.rs = rs;
            out.rt = rt;
            out.target = target(offset);
            switch (op & 3) {
                case 0: out.kind = BranchKind::Eq; break;
                case 1: out.kind = BranchKind::Ne; break;
                case 2: out.kind = BranchKind::Lez; break;
                default: out.kind = BranchKind::Gtz; break;
            }
            return true;
        }
        if (op == 0x01) {
            const u32 sub = rt;
            if (!(sub <= 0x03 || (sub >= 0x10 && sub <= 0x13))) return false;
            out.unconditional = false;
            out.likely = (sub & 2) != 0;
            out.link = (sub & 0x10) != 0;
            out.kind = (sub & 1) ? BranchKind::Bgez : BranchKind::Bltz;
            out.rs = rs;
            out.target = target(offset);
            return true;
        }
        if (op == 0x11 && rs == 8) {
            if (!cu1_) return false;
            const u32 condition = rt & 3;
            out.unconditional = false;
            out.likely = (condition & 2) != 0;
            out.bc1True = (condition & 1) != 0;
            out.kind = BranchKind::Bc1;
            out.target = target(offset);
            return true;
        }
        return false;
    }

    [[nodiscard]] static bool isControlFlow(u32 raw) noexcept {
        const u32 op = raw >> 26, rs = (raw >> 21) & 31;
        if (op == 0) {
            const u32 fn = raw & 63;
            return fn == 0x08 || fn == 0x09 || fn == 0x0C || fn == 0x0D;
        }
        if (op == 0x01) return true;
        if (op == 0x10 || op == 0x12) return true;
        if (op == 0x11) return rs == 8;
        return op == 0x02 || op == 0x03 ||
               (op >= 0x04 && op <= 0x07) || (op >= 0x14 && op <= 0x17);
    }

    [[nodiscard]] static bool isFpMemory(u32 op) noexcept {
        return op == 0x31 || op == 0x35 || op == 0x39 || op == 0x3d;
    }

    [[nodiscard]] static bool isMemory(u32 op) noexcept {
        return op == 0x20 || op == 0x21 || op == 0x23 || op == 0x24 ||
               op == 0x25 || op == 0x27 || op == 0x28 || op == 0x29 ||
               op == 0x2B || op == 0x37 || op == 0x3F || isFpMemory(op);
    }

    [[nodiscard]] static bool mayBail(u32 raw) noexcept {
        if (isMemory(raw >> 26)) return true;
        if ((raw >> 26) != 0x11) return false;
        const u32 rs = (raw >> 21) & 31;
        const u32 fn = raw & 63;
        return (rs == 0x10 || rs == 0x11) &&
               (fn == 0x0d || fn == 4 || (fn >= 0x38 && fn <= 0x3F));
    }

    [[nodiscard]] bool canTranslate(u32 raw) const noexcept {
        const u32 op = raw >> 26, rs = (raw >> 21) & 31, rt = (raw >> 16) & 31;
        const u32 rd = (raw >> 11) & 31, sa = (raw >> 6) & 31, fn = raw & 63;
        if (!raw) return true;
        if (op == 0x11) {
            if (!cu1_ || rs == 8) return false;
            if (rs <= 1 || rs == 4 || rs == 5) return (raw & 0x7ff) == 0;
            if (rs == 0x14 || rs == 0x15) return fn == 0x20 || fn == 0x21;
            if (rs != 0x10 && rs != 0x11) return false;
            if (fn >= 0x30 && fn <= 0x3F) return true;
            if (fn <= 7 || fn == 0x0d) return true;
            return (rs == 0x10 && fn == 0x21) || (rs == 0x11 && fn == 0x20);
        }
        if (op == 9 || op == 0x19) return true;
        // Aligned byte/halfword/word loads and stores use the inline direct
        // RDRAM path; unaligned or mapped addresses bail to the interpreter.
        if (isMemory(op)) return !isFpMemory(op) || cu1_;
        if (op >= 0x0C && op <= 0x0E) return true;
        if (op == 0x0F && !rs) return true;
        if (op != 0) return false;
        if ((fn == 0 || fn == 2 || fn == 3 || fn == 0x38 || fn == 0x3a ||
             fn == 0x3b || fn == 0x3c || fn == 0x3e || fn == 0x3f) && !rs)
            return true;
        if ((fn == 0x10 || fn == 0x12) && !(rs | rt | sa)) return true;
        if ((fn == 0x18 || fn == 0x19) && !(rd | sa)) return true;
        if (sa) return false;
        switch (fn) {
            case 0x21: case 0x23: case 0x24: case 0x25: case 0x26: case 0x27:
            case 0x2A: case 0x2B: case 0x2D: case 0x2F:
                return true;
            default:
                return false;
        }
    }

    void emitInstruction(u32 raw, u64 pc, u32 index) {
        const u32 op = raw >> 26, rs = (raw >> 21) & 31, rt = (raw >> 16) & 31;
        const u32 rd = (raw >> 11) & 31, sa = (raw >> 6) & 31, fn = raw & 63;
        if (!raw) return;

        if (op == 0x11) {
            if (rs == 8) return; // branch is emitted by emitBranch
            if (rs <= 1 || rs == 4 || rs == 5) {
                if (rs < 2) {
                    if (!rt) return;
                    asm_.fprBits(true, rs == 1, A, rd);
                    if (rs == 0) asm_.bits(0x93400000u, A, A, 0, 31);
                    asm_.strGpr(rt, A);
                } else {
                    asm_.fprBits(false, rs == 5, asm_.gprOperand(rt, A), rd);
                }
                return;
            }
            if (fn >= 0x30 && fn <= 0x3F) {
                emitCompare(rs, rt, rd, fn, pc, index);
                return;
            }
            emitFloatingPoint(rs, rt, rd, sa, fn, pc, index);
            return;
        }

        if (isMemory(op)) {
            emitMemory(op, rs, rt, static_cast<s16>(raw), pc, index);
            return;
        }

        if (op == 9 || op == 0x19) { // ADDIU / DADDIU
            if (!rt) return;
            const u32 dest = asm_.gprDestination(rt, A);
            const u32 source = asm_.gprOperand(rs, A);
            const s32 offset = static_cast<s16>(raw);
            const auto magnitude = static_cast<u32>(offset < 0 ? -offset : offset);
            const bool wide = op == 0x19;
            if (!rs) {
                asm_.movImm64(dest, static_cast<u64>(static_cast<s64>(offset)));
            } else if (magnitude <= 4095) {
                const u32 base = offset < 0 ? 0x51000000u : 0x11000000u;
                asm_.emit(base | (wide ? 0x80000000u : 0u) | (magnitude << 10) |
                          (source << 5) | dest);
            } else {
                asm_.movImm64(B, static_cast<u64>(static_cast<s64>(offset)));
                asm_.reg(wide ? 0x8b000000u : 0x0b000000u, dest, source, B);
            }
            if (!wide && rs) asm_.bits(0x93400000u, dest, dest, 0, 31);
            asm_.strGpr(rt, dest);
            return;
        }
        if (op >= 0x0C && op <= 0x0E) { // ANDI / ORI / XORI
            asm_.ldrGpr(A, rs);
            asm_.movImm64(B, static_cast<u64>(raw & 0xFFFFu));
            constexpr u32 ops[] = {0x8a000000u, 0xaa000000u, 0xca000000u};
            asm_.reg(ops[op - 0x0C], A, A, B);
            asm_.strGpr(rt, A);
            return;
        }
        if (op == 0x0F && !rs) { // LUI
            asm_.emit(0xd2a00000u | ((raw & 0xFFFFu) << 5) | A);
            asm_.bits(0x93400000u, A, A, 0, 31);
            asm_.strGpr(rt, A);
            return;
        }
        if (op != 0) return;

        if ((fn == 0 || fn == 2 || fn == 3 || fn == 0x38 || fn == 0x3a ||
             fn == 0x3b || fn == 0x3c || fn == 0x3e || fn == 0x3f) && !rs) {
            if (!rd) return;
            const u32 source = asm_.gprOperand(rt, A), dest = asm_.gprDestination(rd, A);
            const bool wide = fn >= 0x38;
            const u32 mask = wide ? 63 : 31, shift = sa + (fn >= 0x3c ? 32u : 0u);
            const u32 base = wide ? 0xd3400000u : 0x53000000u;
            if ((fn & 3) == 0) asm_.bits(base, dest, source, (-shift) & mask, mask - shift);
            else asm_.bits(base ^ ((fn & 3) == 3 ? (1u << 30) : 0u), dest, source, shift, mask);
            if (!wide) asm_.bits(0x93400000u, dest, dest, 0, 31);
            asm_.strGpr(rd, dest);
            return;
        }
        if ((fn == 0x10 || fn == 0x12) && !(rs | rt | sa)) { // MFHI / MFLO
            if (fn == 0x10) asm_.ldrHi(A); else asm_.ldrLo(A);
            asm_.strGpr(rd, A);
            return;
        }
        if ((fn == 0x18 || fn == 0x19) && !(rd | sa)) { // MULT / MULTU
            asm_.ldrGpr(A, rs);
            asm_.ldrGpr(B, rt);
            asm_.reg(fn == 0x18 ? 0x9b207c00u : 0x9ba07c00u, A, A, B);
            asm_.bits(0x93400000u, B, A, 32, 63);
            asm_.strHi(B);
            asm_.bits(0x93400000u, A, A, 0, 31);
            asm_.strLo(A);
            return;
        }
        if (sa) return;
        u32 code = 0;
        switch (fn) {
            case 0x21: code = 0x0b000000u; break; // ADDU
            case 0x23: code = 0x4b000000u; break; // SUBU
            case 0x24: code = 0x8a000000u; break; // AND
            case 0x25: case 0x27: code = 0xaa000000u; break; // OR / NOR
            case 0x26: code = 0xca000000u; break; // XOR
            case 0x2A: case 0x2B: code = 0xeb000000u; break; // SLT / SLTU
            case 0x2D: code = 0x8b000000u; break; // DADDU
            case 0x2F: code = 0xcb000000u; break; // DSUBU
            default: return;
        }
        asm_.ldrGpr(A, rs);
        asm_.ldrGpr(B, rt);
        if (fn == 0x2A || fn == 0x2B) {
            asm_.reg(0xeb000000u, 31, A, B);
            asm_.emit(0x9a9f07e0u | ((fn == 0x2A ? 10u : 2u) << 12) | A);
        } else {
            asm_.reg(code, A, A, B);
            if (fn == 0x27) asm_.reg(0xaa200000u, A, 31, A);
            if (fn == 0x21 || fn == 0x23)
                asm_.bits(0x93400000u, A, A, 0, 31);
        }
        asm_.strGpr(rd, A);
    }

    void emitMemory(u32 op, u32 rs, u32 rt, s64 imm, u64 pc, u32 index) {
        const bool fp = isFpMemory(op);
        const bool store = op == 0x28 || op == 0x29 || op == 0x2B || op == 0x3F ||
                           (fp && (op & 8));
        const u32 size = (op == 0x37 || op == 0x3F || (fp && (op & 4))) ? 8u :
                         (op == 0x20 || op == 0x24 || op == 0x28) ? 1u :
                         (op == 0x21 || op == 0x25 || op == 0x29) ? 2u : 4u;
        const bool sign = op == 0x20 || op == 0x21 || op == 0x23;
        const u32 fallback = asm_.label();
        bails_.push_back({fallback, index, pc, asm_.writebackMask()});

        asm_.ldrGpr(A, rs);
        asm_.addImm64(A, imm);

        // Canonical sign-extended KSEG0/KSEG1 address in RDRAM, aligned for
        // the access width. Anything else returns before the instruction.
        static_assert(Memory::RdramSize == (1u << 23));
        asm_.bits(0x93400000u, Tmp, A, 23, 63); // ASR Xtmp, Xa, #23
        asm_.emit(0x9279fb9cu); // AND x28, x28, #~0x40 (ignore KSEG alias bit)
        asm_.emit(0xb104039fu); // CMN x28, #256 (canonical direct RDRAM)
        asm_.bcond(CondNe, fallback);
        if (size > 1) {
            const u32 imms = size == 8 ? 2 : size == 4 ? 1 : 0;
            asm_.emit(0x7200001fu | (imms << 10) | (A << 5)); // TST w9, #(size-1)
            asm_.bcond(CondNe, fallback);
        }
        asm_.bits(0xd3400000u, A, A, 0, 22); // UBFX Xa, Xa, #0, #23
        asm_.ldrRdram(B);
        asm_.addReg64(B, B, A);

        if (!store) {
            asm_.loadMemory(A, B, size);
            if (fp) {
                asm_.fprBits(false, size == 8, A, rt);
                return;
            }
            if (sign) {
                const u32 extend = size == 4 ? 0x93407C00u :
                                   size == 2 ? 0x93403C00u : 0x93401C00u;
                asm_.emit(extend | (A << 5) | A);
            }
            asm_.strGpr(rt, A);
            return;
        }

        // Leave stores to translated code to the interpreter before any write.
        // This also prevents executing stale instructions in the current block.
        asm_.lsr64(T1, A, 6);
        asm_.ldrLineActive(T2);
        asm_.ldrbIndex(T3, T2, T1);
        asm_.cbnz(T3, fallback);
        asm_.lsl64(T1, T1, 2);
        asm_.ldrLineGen(T2);
        asm_.addReg64(T2, T2, T1);
        asm_.emit(0xb9400000u | (T2 << 5) | T3); // LDR w13, [x12]
        asm_.emit(0x310005bfu); // CMN w13, #1; let C++ handle generation wrap
        asm_.bcond(CondEq, fallback);
        if (fp) asm_.fprBits(true, size == 8, Tmp, rt);
        else asm_.ldrGpr(Tmp, rt);
        asm_.reverseBytes(Tmp, size);
        asm_.storeMemory(Tmp, B, size);

        // Apple Silicon supports LSE. Publish to the interpreter's generation
        // table with one release RMW instead of an exclusive retry loop.
        asm_.emit(0x5280002du); // MOV w13, #1
        asm_.staddl32(T3, T2);

    }

    u32 instructionBail(u64 pc, u32 index) {
        const u32 label = asm_.label();
        bails_.push_back({label, index, pc, asm_.writebackMask()});
        return label;
    }

    void emitTruncateWord(bool wide, u32 fs, u32 fd, u64 pc, u32 index) {
        const u32 fallback = instructionBail(pc, index);
        const u32 normalized = asm_.label();
        asm_.fprBits(true, wide, T1, fs);
        asm_.bits(wide ? 0xd3400000u : 0x53000000u, T2, T1,
                  wide ? 52 : 23, wide ? 62 : 30);
        asm_.cbnz(T2, normalized);
        asm_.bits(wide ? 0xd3400000u : 0x53000000u, T2, T1, 0, wide ? 62 : 30);
        if (wide) {
            // CBNZ X, not W: double subnormals can have no low word bits.
            asm_.cbnz(T2, fallback);
            asm_.code.back() |= 1u << 31;
        } else {
            asm_.cbnz(T2, fallback);
        }
        asm_.bind(normalized);
        asm_.emit(wide ? 0x9e670160u : 0x1e270160u); // FMOV D0,X11 / S0,W11
        if (!wide) asm_.emit(0x1e22c000u); // FCVT D0,S0
        asm_.movImm64(T1, 0x41e0000000000000ULL); // +2^31
        asm_.emit(0x9e670162u); // FMOV D2,X11
        asm_.emit(0x1e622000u); // FCMP D0,D2
        asm_.bcond(CondCs, fallback); // >= upper bound or unordered
        asm_.emit(0x1e614042u); // FNEG D2,D2
        asm_.emit(0x1e622000u);
        asm_.bcond(CondMi, fallback); // < lower bound
        asm_.emit(0x1e65c001u); // FRINTZ D1,D0
        asm_.emit(0x1e780029u); // FCVTZS W9,D1
        asm_.emit(0x1e612000u); // FCMP D0,D1
        asm_.ldrFcsr(B);
        asm_.emit(0x120e654au); // AND W10,W10,#~0x3f000 (clear cause bits)
        const u32 commit = asm_.label();
        asm_.bcond(CondEq, commit);
        asm_.tbnz(B, 7, fallback); // enabled Inexact: scalar owns exception
        asm_.emit(0x3214014au); // ORR W10,W10,#0x1000 (inexact cause)
        asm_.emit(0x321e014au); // ORR W10,W10,#4 (sticky inexact)
        asm_.bind(commit);
        asm_.strFcsr(B);
        asm_.fprBits(false, false, A, fd);
    }

    void emitFloatingPoint(u32 fmt, u32 ft, u32 fs, u32 fd, u32 fn, u64 pc, u32 index) {
        const bool wide = fmt == 0x11;
        if (fn == 0x0d) {
            emitTruncateWord(wide, fs, fd, pc, index);
            return;
        }
        if (fmt == 0x14 || fmt == 0x15) { // CVT.S/D.W/L
            asm_.fprBits(true, fmt == 0x15, A, fs, false);
            asm_.emit(0x1e220000u | (fmt == 0x15 ? (1u << 31) : 0u) |
                      (fn == 0x21 ? (1u << 22) : 0u) | (A << 5)); // SCVTF
            asm_.fprMemory(false, fn == 0x21, 0, fd);
            return;
        }
        if (fn <= 3) {
            constexpr u32 operations[] = {0x1e202800u, 0x1e203800u,
                                          0x1e200800u, 0x1e201800u};
            asm_.fprMemory(true, wide, 0, fs);
            asm_.fprMemory(true, wide, 1, ft);
            asm_.reg(operations[fn] | (wide ? (1u << 22) : 0u), 0, 0, 1);
            asm_.fprMemory(false, wide, 0, fd);
            return;
        }
        if (fn <= 7) {
            asm_.fprMemory(true, wide, 0, fs);
            constexpr u32 operations[] = {0x1e21c000u, 0x1e20c000u, 0u, 0x1e214000u};
            if (fn == 4) {
                // Keep negative and NaN cases on the reference path.
                asm_.emit(0x1e202008u | (wide ? (1u << 22) : 0u)); // FCMP S0/D0,#0
                asm_.bcond(CondLt, instructionBail(pc, index));
            }
            if (fn != 6) asm_.emit(operations[fn - 4] | (wide ? (1u << 22) : 0u));
            asm_.fprMemory(false, wide, 0, fd);
            return;
        }
        if ((!wide && fn == 0x21) || (wide && fn == 0x20)) {
            asm_.fprMemory(true, wide, 0, fs);
            asm_.emit(wide ? 0x1e624000u : 0x1e22c000u);
            asm_.fprMemory(false, !wide, 0, fd);
        }
    }

    void emitCompare(u32 fmt, u32 ft, u32 fs, u32 fn, u64 pc, u32 index) {
        const bool wide = fmt == 0x11;
        const bool signaling = fn >= 0x38;
        const u32 predicate = fn & 0x0Fu;
        asm_.fprMemory(true, wide, 0, fs);
        asm_.fprMemory(true, wide, 1, ft);
        asm_.emit(wide ? 0x1e612000u : 0x1e212000u); // fcmp d0,d1 / s0,s1

        if (signaling) {
            const u32 bail = asm_.label();
            asm_.bcond(CondVs, bail);
            bails_.push_back({bail, index, pc, asm_.writebackMask()});
        }

        if (predicate == 0 || predicate == 8) {
            asm_.fcsrClearBit23();
            return;
        }
        if (predicate == 3) { // UEQ: ordered equal or unordered
            if (signaling) {
                // NGL reaches here only after the unordered bail.
                asm_.fcsrSetBit23(CondEq);
            } else {
                asm_.cset32(A, CondEq);
                asm_.cset32(B, CondVs);
                asm_.orr32(A, A, B);
                asm_.ldrFcsr(B);
                asm_.emit(0x3309012au); // bfi w10, w9, #23, #1
                asm_.strFcsr(B);
            }
            return;
        }
        u32 cond = CondEq;
        switch (predicate) {
            case 1: cond = CondVs; break;  // UN
            case 2: cond = CondEq; break;  // EQ
            case 4: cond = CondMi; break;  // OLT
            case 5: cond = CondLt; break;  // ULT
            case 6: cond = CondLs; break;  // OLE
            case 7: cond = CondLe; break;  // ULE
            case 9: cond = CondLt; break;  // NGLE: N != V (unordered or less)
            case 10: cond = CondEq; break; // SEQ
            case 11: cond = CondEq; break; // NGL (ordered after bail)
            case 12: cond = CondMi; break; // LT
            case 13: cond = CondLt; break; // NGE: N != V (unordered or less)
            case 14: cond = CondLs; break; // LE
            case 15: cond = CondLs; break; // NGT
            default: break;
        }
        asm_.fcsrSetBit23(cond);
    }

    void emitBranch() {
        const BranchDesc& br = branch_;
        const u64 sequential = vpc_ + static_cast<u64>(steps_) * 4;
        const u64 branchPc = sequential - 8;
        const auto emitDelay = [&] {
            // A faultable slot exits at the branch, not at the delay PC.
            emitInstruction(raws_[br.index + 1], branchPc, br.index);
        };
        if (br.indirect) asm_.ldrGpr(Pc, br.rs);
        const bool linkAliasesCondition = br.link && !br.unconditional && br.rs == br.linkReg;
        if (linkAliasesCondition) asm_.ldrGpr(T3, br.rs);
        if (br.link) {
            // JAL/JALR link to the instruction after the delay slot. The
            // branch need not be the first instruction in a compiled block.
            asm_.movImm64(A, vpc_ + static_cast<u64>(br.index) * 4 + 8);
            asm_.strGpr(br.linkReg, A);
        }

        u32 takenLabel = 0;
        if (!br.unconditional) {
            takenLabel = asm_.label();
            switch (br.kind) {
                case BranchKind::Eq:
                    asm_.cmpReg64(asm_.gprOperand(br.rs, A), asm_.gprOperand(br.rt, B));
                    asm_.bcond(CondEq, takenLabel);
                    break;
                case BranchKind::Ne:
                    asm_.cmpReg64(asm_.gprOperand(br.rs, A), asm_.gprOperand(br.rt, B));
                    asm_.bcond(CondNe, takenLabel);
                    break;
                case BranchKind::Lez:
                    asm_.ldrGpr(A, br.rs);
                    asm_.cmpZero64(A);
                    asm_.bcond(CondLe, takenLabel);
                    break;
                case BranchKind::Gtz:
                    asm_.ldrGpr(A, br.rs);
                    asm_.cmpZero64(A);
                    asm_.bcond(CondGt, takenLabel);
                    break;
                case BranchKind::Bltz:
                    asm_.cmpZero64(linkAliasesCondition ? T3 : asm_.gprOperand(br.rs, A));
                    asm_.bcond(CondLt, takenLabel);
                    break;
                case BranchKind::Bgez:
                    asm_.cmpZero64(linkAliasesCondition ? T3 : asm_.gprOperand(br.rs, A));
                    asm_.bcond(CondGe, takenLabel);
                    break;
                case BranchKind::Bc1:
                    asm_.ldrFcsr(A);
                    if (br.bc1True) asm_.tbnz(A, 23, takenLabel);
                    else asm_.tbz(A, 23, takenLabel);
                    break;
                default:
                    break;
            }
        }

        if (br.unconditional) {
            emitDelay();
            if (br.indirect) {
                asm_.flushGpr(asm_.writebackMask());
                if (!br.link && br.rs == 31) emitReturnDispatch();
                else asm_.b(epilogueLabel_);
            }
            else emitTaken(br);
            return;
        }

        const u32 branchDirty = asm_.dirtyGpr;
        if (br.likely) {
            // Not taken: likely branches nullify the delay slot.
            asm_.addImm32(Budget, 1);
            asm_.subImm32(Retired, 1);
            emitChain(sequential);
            asm_.bind(takenLabel);
            asm_.dirtyGpr = branchDirty;
            emitDelay();
            emitTaken(br);
            return;
        }

        // Ordinary conditional branch: the delay slot runs on both paths.
        emitDelay();
        emitChain(sequential);
        asm_.bind(takenLabel);
        asm_.dirtyGpr = branchDirty;
        emitDelay();
        emitTaken(br);
    }

    void emitReturnDispatch() {
        // The flat table is maintained by the same synchronous invalidation
        // as the direct edges. Check the full virtual PC (including alias and
        // alignment) and the compilation mode before using a chained entry.
        asm_.bits(0xd3400000u, A, Pc, 2, 22); // (PC & 0x7fffff) >> 2
        asm_.emit(0xf9400000u | (11u << 10) | (Ctx << 5) | B);
        asm_.emit(0xf8607800u | (A << 16) | (B << 5) | A); // LDR Xa, [Xb, Xa, LSL #3]
        asm_.cbz64(A, epilogueLabel_);
        asm_.emit(0xf9400000u | (A << 5) | B); // descriptor virtual PC
        asm_.cmpReg64(Pc, B);
        asm_.bcond(CondNe, epilogueLabel_);
        asm_.emit(0xb9400000u | (6u << 10) | (A << 5) | B); // descriptor mode
        asm_.emit(0xb9400000u | (15u << 10) | (Ctx << 5) | T1);
        asm_.reg(0x6b000000u, 31, B, T1);
        asm_.bcond(CondNe, epilogueLabel_);
        asm_.emit(0xf9400000u | (5u << 10) | (A << 5) | B); // descriptor body
        asm_.cbz64(B, epilogueLabel_);
        asm_.emit(0xd61f0000u | (B << 5)); // BR Xb
    }

    void emitTaken(const BranchDesc& br) {
        if (loopLabel_ != NoLoop) {
            // The cycle charge happens at loopLabel_ on every iteration.
            // CU1 cannot change in this block; no guest state crosses a
            // scheduler deadline. The pinned values are already live here.
            asm_.cmpImm32(Budget, steps_);
            asm_.bcond(CondCs, loopLabel_);
            asm_.flushGpr(asm_.writebackMask());
            asm_.movVirtualPc(static_cast<u32>(vpc_));
            asm_.b(epilogueLabel_);
            return;
        }
        // Same state on each iteration: consume whole branch/NOP pairs up to
        // the scheduler's existing device/interrupt deadline.
        if (br.index == 0 && br.target == vpc_ && raws_[1] == 0 && !br.link) {
            asm_.emit(0x121f7b29u); // AND w9, w25, #0xfffffffe
            asm_.reg(0x4b000000u, Budget, Budget, A);
            asm_.reg(0x0b000000u, Retired, Retired, A);
            asm_.flushGpr(asm_.writebackMask());
            asm_.movVirtualPc(static_cast<u32>(vpc_));
            asm_.b(epilogueLabel_);
            return;
        }
        emitChain(br.target);
    }

    void emitChain(u64 target) {
        asm_.flushGpr(asm_.writebackMask());
        const u32 thunk = asm_.label();
        asm_.b(thunk);
        edges_.push_back({target, static_cast<u32>(asm_.code.size() - 1), thunk});
    }

    const Memory& ram_;
    u64 vpc_;
    bool cu1_;
    Asm asm_;
    std::vector<u32> raws_;
    BranchDesc branch_;
    std::vector<Bail> bails_;
    std::vector<EdgeSpec> edges_;
    u32 steps_ = 0;
    u32 chainLabel_ = 0;
    static constexpr u32 NoLoop = ~u32{0};
    u32 loopLabel_ = NoLoop;
    u32 epilogueLabel_ = 0;
    u32 startBailLabel_ = 0;
    bool needsCop1_ = false;
    bool hasBranch_ = false;
};
} // namespace

struct Arm64Jit::State {
    static constexpr std::size_t Capacity = 8u * 1024 * 1024;
    static constexpr u32 LineCount = Memory::RdramSize / 64;

    using Entry = void (*)(Context*);

    struct Block {
        u64 vpc = 0;
        u32 phys = 0;
        u64 epoch = 0;
        u32 mode = 0;
        u32 steps = 0;
        u8* entry = nullptr;
        u8* body = nullptr;
        u8 lineCount = 0;
        std::array<u32, 2> line{};
        std::array<u32, 2> lineGen{};
        std::array<std::pair<u64, u32*>, 2> outgoing{};
        u32 edgeCount = 0;
    };
    struct Edge {
        u32 mode = 0;
        u32* site = nullptr;
        u8* thunk = nullptr;
    };

    std::unordered_map<u64, std::unique_ptr<Block>> blocks;
    std::vector<Block*> dispatch;
    std::unordered_map<u32, std::vector<Block*>> lineBlocks;
    std::vector<u8> lineActive;
    std::unordered_map<u64, std::vector<Edge>> inbound;
    u8* arena = nullptr;
    std::size_t used = 0;
    u64 epoch = 0;
    const std::atomic<u32>* lineGen = nullptr;
    u64 compiles = 0, unsupported = 0, flushes = 0, invalidations = 0, patches = 0;

    State() : dispatch(Memory::RdramSize / 4), lineActive(LineCount, 0) {}
    Block* lookup(u64 pc) noexcept {
        auto*& slot = dispatch[physicalPc(pc) >> 2];
        if (!slot || slot->vpc == pc) return slot;
        const auto it = blocks.find(pc); // The other KSEG alias has its own PC.
        return it == blocks.end() ? nullptr : it->second.get();
    }
    ~State() {
        if (arena) munmap(arena, Capacity);
    }
};

static_assert(offsetof(Arm64Jit::State::Block, vpc) == 0 &&
              offsetof(Arm64Jit::State::Block, mode) == 24 &&
              offsetof(Arm64Jit::State::Block, body) == 40);

namespace {
void writeProtected(void* destination, const void* source, std::size_t bytes) {
    pthread_jit_write_protect_np(0);
    std::memcpy(destination, source, bytes);
    pthread_jit_write_protect_np(1);
    sys_icache_invalidate(destination, bytes);
}

void patchBranch(u32* site, const u8* destination) {
    const auto delta = static_cast<s64>(
        (reinterpret_cast<std::intptr_t>(destination) -
         reinterpret_cast<std::intptr_t>(site)) / 4);
    if (delta < -(1 << 25) || delta > ((1 << 25) - 1)) return;
    const u32 word = 0x14000000u | (static_cast<u32>(delta) & 0x03FFFFFFu);
    if (*site == word) return;
    pthread_jit_write_protect_np(0);
    *site = word;
    pthread_jit_write_protect_np(1);
    sys_icache_invalidate(site, sizeof(u32));
}

[[nodiscard]] bool linesValid(const Arm64Jit::State& state,
                              const Arm64Jit::State::Block& block) noexcept {
    if (!state.lineGen) return true;
    for (u32 i = 0; i < block.lineCount; ++i)
        if (state.lineGen[block.line[i]].load(std::memory_order_acquire) != block.lineGen[i])
            return false;
    return true;
}

void patchTarget(Arm64Jit::State& state, u64 targetVpc,
                 Arm64Jit::State::Block* target) {
    if (target && (!target->entry || target->epoch != state.epoch ||
                   !linesValid(state, *target)))
        target = nullptr;
    const auto it = state.inbound.find(targetVpc);
    if (it == state.inbound.end()) return;
    for (const auto& edge : it->second) {
        patchBranch(edge.site,
                    target && target->mode == edge.mode ? target->body : edge.thunk);
    }
}

void removeBlock(Arm64Jit::State& state, Arm64Jit::State::Block* block) {
    patchTarget(state, block->vpc, nullptr);
    for (u32 i = 0; i < block->edgeCount; ++i) {
        const auto [target, site] = block->outgoing[i];
        auto it = state.inbound.find(target);
        std::erase_if(it->second, [site](const auto& edge) { return edge.site == site; });
        if (it->second.empty()) state.inbound.erase(it);
    }
    for (u32 i = 0; i < block->lineCount; ++i) {
        auto it = state.lineBlocks.find(block->line[i]);
        if (it == state.lineBlocks.end()) continue;
        auto& list = it->second;
        list.erase(std::remove(list.begin(), list.end(), block), list.end());
        if (list.empty()) {
            state.lineBlocks.erase(it);
            state.lineActive[block->line[i]] = 0;
        }
    }
    const u64 vpc = block->vpc;
    auto*& slot = state.dispatch[block->phys >> 2];
    const bool wasPrimary = slot == block;
    state.blocks.erase(vpc);
    if (wasPrimary) {
        const auto alias = state.blocks.find(vpc ^ 0x20000000u);
        slot = alias == state.blocks.end() ? nullptr : alias->second.get();
    }
}

void resetAll(Arm64Jit::State& state) {
    ++state.flushes;
    for (const auto& [pc, block] : state.blocks) state.dispatch[block->phys >> 2] = nullptr;
    state.blocks.clear();
    state.lineBlocks.clear();
    state.inbound.clear();
    std::fill(state.lineActive.begin(), state.lineActive.end(), u8{0});
    state.used = 0;
}

void recordUnsupported(Arm64Jit::State& state, u64 vpc, u64 epoch, u32 mode) {
    ++state.unsupported;
    auto block = std::make_unique<Arm64Jit::State::Block>();
    block->vpc = vpc;
    block->phys = physicalPc(vpc);
    block->epoch = epoch;
    block->mode = mode;
    block->lineCount = 1;
    block->line[0] = block->phys >> 6;
    block->lineGen[0] = state.lineGen
        ? state.lineGen[block->line[0]].load(std::memory_order_acquire) : 0;
    state.lineBlocks[block->line[0]].push_back(block.get());
    state.lineActive[block->line[0]] = 1;
    state.dispatch[block->phys >> 2] = block.get();
    state.blocks[vpc] = std::move(block);
}

[[nodiscard]] Arm64Jit::State::Block* compileBlock(Arm64Jit::State& state,
                                                   const Memory& ram, u64 vpc,
                                                   u64 epoch, u32 mode,
                                                   bool cu1, bool fullFpr) {
    Compiler compiler(ram, vpc, cu1, fullFpr);
    if (!compiler.plan()) return nullptr;
    ++state.compiles;
    compiler.emit();
    compiler.asmCode().finish();

    const auto& code = compiler.asmCode().code;
    const std::size_t bytes = code.size() * sizeof(u32);
    if (state.used + bytes > Arm64Jit::State::Capacity) resetAll(state);
    if (bytes > Arm64Jit::State::Capacity) return nullptr;

    auto* destination = state.arena + state.used;
    writeProtected(destination, code.data(), bytes);
    state.used += bytes;

    auto block = std::make_unique<Arm64Jit::State::Block>();
    block->vpc = vpc;
    block->phys = physicalPc(vpc);
    block->epoch = epoch;
    block->mode = mode;
    block->steps = compiler.steps();
    block->entry = destination;
    block->body = destination + compiler.chainIndex() * sizeof(u32);

    const u32 firstLine = block->phys >> 6;
    const u32 lastPc = block->phys + (block->steps - 1) * 4;
    const u32 lastLine = lastPc >> 6;
    block->lineCount = static_cast<u8>(lastLine - firstLine + 1);
    for (u32 i = 0; i < block->lineCount; ++i) {
        const u32 line = firstLine + i;
        block->line[i] = line;
        block->lineGen[i] = state.lineGen
            ? state.lineGen[line].load(std::memory_order_acquire) : 0;
        state.lineBlocks[line].push_back(block.get());
        state.lineActive[line] = 1;
    }

    auto* raw = block.get();
    state.dispatch[raw->phys >> 2] = raw;
    state.blocks[vpc] = std::move(block);

    const auto& labels = compiler.asmCode().labels;
    for (const auto& spec : compiler.edges()) {
        if (!directFetch(spec.targetVpc)) continue;
        const Arm64Jit::State::Edge edge{
            mode,
            reinterpret_cast<u32*>(destination + spec.site * sizeof(u32)),
            destination + labels[spec.thunk] * sizeof(u32),
        };
        raw->outgoing[raw->edgeCount++] = {spec.targetVpc, edge.site};
        state.inbound[spec.targetVpc].push_back(edge);
        auto* target = state.lookup(spec.targetVpc);
        if (target && (!target->entry || target->mode != mode ||
                       target->epoch != state.epoch || !linesValid(state, *target)))
            target = nullptr;
        if (target) patchBranch(edge.site, target->body);
    }
    patchTarget(state, vpc, raw);
    return raw;
}
} // namespace
#else
struct Arm64Jit::State {};
#endif

Arm64Jit::Arm64Jit() : state_(std::make_unique<State>()) {}
Arm64Jit::~Arm64Jit() = default;

std::unique_ptr<Arm64Jit> Arm64Jit::create() {
#ifdef N64FORGE_NATIVE_JIT
    if (!pthread_jit_write_protect_supported_np()) return {};
    auto result = std::unique_ptr<Arm64Jit>(new Arm64Jit);
    void* memory = mmap(nullptr, State::Capacity, PROT_READ | PROT_WRITE | PROT_EXEC,
                        MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
    if (memory == MAP_FAILED) return {};
    result->state_->arena = static_cast<u8*>(memory);
    return result;
#else
    return {};
#endif
}

Arm64Jit::Result Arm64Jit::run([[maybe_unused]] const Memory& ram,
                               [[maybe_unused]] u64 pc,
                               [[maybe_unused]] u64 epoch,
                               [[maybe_unused]] const std::atomic<u32>* lineGen,
                               [[maybe_unused]] u32 budget,
                               [[maybe_unused]] u64* gpr,
                               [[maybe_unused]] u64* hi,
                               [[maybe_unused]] u64* lo,
                               [[maybe_unused]] u64* fpr,
                               [[maybe_unused]] u32* fcsr,
                               [[maybe_unused]] bool cop1,
                               [[maybe_unused]] bool fullFpr) noexcept {
#ifdef N64FORGE_NATIVE_JIT
    if (!state_ || !state_->arena || !budget || !directFetch(pc) || !lineGen) return {0, pc};
    State& state = *state_;
    state.lineGen = lineGen;
    if (epoch != state.epoch) {
        resetAll(state);
        state.epoch = epoch;
    }

    const u32 mode = cop1 ? (fullFpr ? 3u : 1u) : 0u;
    auto* block = state.lookup(pc);
    if (block && (block->mode != mode || block->epoch != state.epoch ||
                  !linesValid(state, *block))) {
        removeBlock(state, block);
        block = nullptr;
    }
    if (!block) {
        block = compileBlock(state, ram, pc, epoch, mode, cop1, fullFpr);
        if (!block) {
            recordUnsupported(state, pc, epoch, mode);
            return {0, pc};
        }
    }
    if (!block->entry || block->steps > budget) return {0, pc};

    Context context{gpr, hi, lo, fpr, fcsr, budget, 0, pc,
                    static_cast<u32>(cop1 ? 1 : 0), mode,
                    const_cast<u8*>(ram.data()),
                    const_cast<std::atomic<u32>*>(lineGen), state.lineActive.data(),
                    state.dispatch.data()};
    reinterpret_cast<State::Entry>(block->entry)(&context);
    return {context.retired, context.pc};
#else
    return {0, pc};
#endif
}

void Arm64Jit::invalidateCode([[maybe_unused]] u32 physicalAddress,
                              [[maybe_unused]] u32 size) noexcept {
#ifdef N64FORGE_NATIVE_JIT
    if (!state_ || !state_->arena || !size || physicalAddress >= Memory::RdramSize)
        return;
    State& state = *state_;
    const u32 last = physicalAddress +
                     std::min(size, Memory::RdramSize - physicalAddress) - 1;
    std::vector<u64> victims;
    for (u32 line = physicalAddress >> 6; line <= (last >> 6); ++line) {
        if (!state.lineActive[line]) continue;
        auto it = state.lineBlocks.find(line);
        if (it == state.lineBlocks.end()) continue;
        for (State::Block* block : it->second) victims.push_back(block->vpc);
    }
    std::sort(victims.begin(), victims.end());
    victims.erase(std::unique(victims.begin(), victims.end()), victims.end());
    for (u64 vpc : victims) {
        auto it = state.blocks.find(vpc);
        if (it != state.blocks.end()) {
            ++state.invalidations;
            removeBlock(state, it->second.get());
        }
    }
#endif
}

void Arm64Jit::invalidateAll() noexcept {
#ifdef N64FORGE_NATIVE_JIT
    if (state_) resetAll(*state_);
#endif
}
} // namespace n64forge
