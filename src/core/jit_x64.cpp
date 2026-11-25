// Native x64 backend for the in-house R4300i recompiler.
//
// The guest CPU owns timing, TLB translation, interrupts and the instruction
// budget. This backend compiles canonical direct-RDRAM windows into small x64
// stubs. Each stub calls the scalar instruction implementation through a
// narrow C ABI helper, so guest semantics stay identical to the interpreter;
// the generated code owns chaining, the host budget and exact bail PCs.
//
// Windows SDK headers are confined to the executable-memory adapter below;
// the public interface stays renderer- and platform-neutral.
#include "jit_x64.hpp"

#include "n64forge/core/bus.hpp"
#include "n64forge/core/cpu.hpp"
#include "n64forge/core/memory.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(_M_X64) || (defined(__x86_64__) && !defined(__arm64e__))
#define N64FORGE_NATIVE_X64 1
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#endif
#endif

namespace n64forge {
#ifdef N64FORGE_NATIVE_X64
namespace {

[[nodiscard]] bool directFetch(u64 vaddr) noexcept {
    constexpr u32 mask = ~(Memory::RdramSize - 1u) & ~0x20000000u;
    const u32 low = static_cast<u32>(vaddr);
    return vaddr == static_cast<u64>(static_cast<s64>(static_cast<s32>(low))) &&
           (low & (mask | 3u)) == 0x80000000u;
}

[[nodiscard]] u32 physicalPc(u64 vaddr) noexcept {
    return static_cast<u32>(vaddr) & (Memory::RdramSize - 1);
}

[[nodiscard]] u32 readBe32(const u8* bytes) noexcept {
    return (u32{bytes[0]} << 24) | (u32{bytes[1]} << 16) | (u32{bytes[2]} << 8) | u32{bytes[3]};
}

} // namespace

struct X64Jit::State {
    static constexpr std::size_t Capacity = 8u * 1024 * 1024;
    static constexpr u32 LineCount = Memory::RdramSize / 64;

    struct Block {
        u64 vpc = 0;
        u64 epoch = 0;
        u32 mode = 0;
        u32 steps = 0;
        u8* entry = nullptr;
        u8* body = nullptr;
        u8 lineCount = 0;
        std::array<u32, 2> line{};
        std::array<u32, 2> lineGen{};
    };

    std::unordered_map<u64, std::unique_ptr<Block>> blocks;
    std::vector<Block*> dispatch;
    std::unordered_map<u32, std::vector<Block*>> lineBlocks;
    std::vector<u8> lineActive;
    u8* arena = nullptr;
    std::size_t used = 0;
    u64 epoch = 0;
    const std::atomic<u32>* lineGen = nullptr;

    State() : dispatch(Memory::RdramSize / 4), lineActive(LineCount, 0) {}

    Block* lookup(u64 pc) noexcept {
        auto*& slot = dispatch[physicalPc(pc) >> 2];
        if (!slot || slot->vpc == pc) return slot;
        const auto it = blocks.find(pc);
        return it == blocks.end() ? nullptr : it->second.get();
    }

    ~State() {
        if (!arena) return;
#if defined(_WIN32)
        VirtualFree(arena, 0, MEM_RELEASE);
#else
        munmap(arena, Capacity);
#endif
    }
};

static_assert(offsetof(X64Jit::Context, gpr) == 0 &&
              offsetof(X64Jit::Context, hi) == 8 &&
              offsetof(X64Jit::Context, lo) == 16 &&
              offsetof(X64Jit::Context, fpr) == 24 &&
              offsetof(X64Jit::Context, fcsr) == 32 &&
              offsetof(X64Jit::Context, budget) == 40 &&
              offsetof(X64Jit::Context, retired) == 44 &&
              offsetof(X64Jit::Context, pc) == 48 &&
              offsetof(X64Jit::Context, cu1) == 56 &&
              offsetof(X64Jit::Context, mode) == 60 &&
              offsetof(X64Jit::Context, rdram) == 64 &&
              offsetof(X64Jit::Context, lineGen) == 72 &&
              offsetof(X64Jit::Context, lineActive) == 80 &&
              offsetof(X64Jit::Context, dispatch) == 88 &&
              offsetof(X64Jit::Context, cpu) == 96 &&
              offsetof(X64Jit::Context, bus) == 104 &&
              offsetof(X64Jit::Context, currentPc) == 112 &&
              offsetof(X64Jit::Context, epoch) == 120 &&
              offsetof(X64Jit::Context, jitState) == 128);

namespace {

using State = X64Jit::State;
using Entry = void (*)(X64Jit::Context*);

// Callee-saved on both Windows x64 and System V: RBX holds the context, R12 the
// remaining host budget and R13 the number of retired guest instructions.
enum HostReg : u8 { RAX = 0, RCX = 1, RDX = 2, RBX = 3, RSP = 4, RBP = 5, RSI = 6, RDI = 7, R12 = 12, R13 = 13 };
constexpr u8 CTX = RBX, BUDGET = R12, RETIRED = R13, RET = RAX;

#if defined(_WIN32)
constexpr u8 Arg0 = RCX, Arg1 = RDX;
constexpr u32 StackReserve = 32; // Caller-owned shadow space; 3 pushes keep 16-byte alignment.
#else
constexpr u8 Arg0 = RDI, Arg1 = RSI;
constexpr u32 StackReserve = 0; // 3 pushes after the return address are already aligned.
#endif

enum Condition : u8 { CondB = 2, CondE = 4, CondNE = 5 };

struct Mem {
    int base = CTX;
    s32 disp = 0;
};

[[nodiscard]] constexpr Mem ctxMem(s32 disp) noexcept { return Mem{CTX, disp}; }

// Small hand-encoded x64 assembler. Every host branch uses a rel32 displacement
// so labels can be bound in a single pass.
class Asm {
public:
    std::vector<u8> bytes;
    std::vector<std::size_t> labels;
    struct Fixup { std::size_t at, label; };
    std::vector<Fixup> fixups;

    [[nodiscard]] std::size_t label() {
        labels.push_back(0);
        return labels.size() - 1;
    }
    void bind(std::size_t labelIndex) { labels[labelIndex] = bytes.size(); }

    void finalize() {
        for (const auto& fixup : fixups) {
            const s64 delta = static_cast<s64>(labels[fixup.label]) -
                              (static_cast<s64>(fixup.at) + 4);
            const u32 relative = static_cast<u32>(static_cast<u64>(delta));
            std::memcpy(bytes.data() + fixup.at, &relative, sizeof(relative));
        }
        fixups.clear();
    }

    void emit(u8 value) { bytes.push_back(value); }
    void emit32(u32 value) {
        for (int i = 0; i < 4; ++i) emit(u8(value >> (i * 8)));
    }
    void emit64(u64 value) {
        for (int i = 0; i < 8; ++i) emit(u8(value >> (i * 8)));
    }

private:
    void rex(bool wide, int reg, int rm) {
        const u8 value = u8(0x40 | (wide ? 8 : 0) | (((reg >> 3) & 1) << 2) | ((rm >> 3) & 1));
        if (value != 0x40) emit(value);
    }
    void modrm(int mod, int reg, int rm) {
        emit(u8((mod << 6) | ((reg & 7) << 3) | (rm & 7)));
    }
    void memoryOpcode(int opcode, const Mem& memory, int reg, bool wide) {
        const int base = memory.base;
        const int mod = ((base & 7) == 5 || memory.disp < -128 || memory.disp > 127) ? 2
                        : memory.disp != 0 ? 1 : 0;
        rex(wide, reg, base);
        emit(u8(opcode));
        modrm(mod, reg, base);
        if (mod == 1) emit(u8(static_cast<u8>(memory.disp)));
        else if (mod == 2) emit32(static_cast<u32>(memory.disp));
    }

public:
    void load64(int dst, const Mem& memory) { memoryOpcode(0x8B, memory, dst, true); }
    void store64(const Mem& memory, int src) { memoryOpcode(0x89, memory, src, true); }
    void load32(int dst, const Mem& memory) { memoryOpcode(0x8B, memory, dst, false); }
    void store32(const Mem& memory, int src) { memoryOpcode(0x89, memory, src, false); }

    void movReg(int dst, int src) {
        rex(true, src, dst);
        emit(0x89);
        modrm(3, src, dst);
    }
    void movImm32(int reg, u32 value) {
        // All immediate destinations used here are low registers.
        emit(u8(0xB8 + (reg & 7)));
        emit32(value);
    }
    void movImm64(int reg, u64 value) {
        rex(true, 0, reg);
        emit(u8(0xB8 + (reg & 7)));
        emit64(value);
    }
    void immediate(int digit, int reg, u32 value) {
        rex(true, 0, reg);
        emit(0x81);
        modrm(3, digit, reg);
        emit32(value);
    }
    void addImm(int reg, u32 value) { immediate(0, reg, value); }
    void subImm(int reg, u32 value) { immediate(5, reg, value); }
    void cmpImm(int reg, u32 value) { immediate(7, reg, value); }

    void testEax() { emit(0x85); emit(0xC0); }

    std::size_t jcc(Condition condition, std::size_t target) {
        emit(0x0F);
        emit(u8(0x80 + (condition & 7)));
        const auto at = bytes.size();
        emit32(0);
        fixups.push_back({at, target});
        return at;
    }
    std::size_t jump(std::size_t target) {
        emit(0xE9);
        const auto at = bytes.size();
        emit32(0);
        fixups.push_back({at, target});
        return at;
    }
    void callReg(int reg) { emit(0xFF); emit(u8(0xD0 + (reg & 7))); }
    void jumpReg(int reg) { emit(0xFF); emit(u8(0xE0 + (reg & 7))); }
    void ret() { emit(0xC3); }

    void push(int reg) {
        if (reg >= 8) emit(0x41);
        emit(u8(0x50 + (reg & 7)));
    }
    void pop(int reg) {
        if (reg >= 8) emit(0x41);
        emit(u8(0x58 + (reg & 7)));
    }
};

enum class BranchKind { None, Eq, Ne, Lez, Gtz, Bltz, Bgez, Bc1 };

extern "C" {
[[nodiscard]] int n64forge_x64_execute(X64Jit::Context* context, u32 raw) noexcept;
[[nodiscard]] const void* n64forge_x64_dispatch(X64Jit::Context* context) noexcept;
} // extern "C"

struct BranchDesc {
    u64 target = 0;
    bool likely = false;
    bool link = false;
    bool unconditional = false;
    bool indirect = false;
    bool bc1True = false;
    BranchKind kind = BranchKind::None;
    u32 rs = 0, rt = 0;
};

[[nodiscard]] bool isFpMemory(u32 op) noexcept {
    return op == 0x31 || op == 0x35 || op == 0x39 || op == 0x3D;
}
[[nodiscard]] bool isMemory(u32 op) noexcept {
    return op == 0x20 || op == 0x21 || op == 0x23 || op == 0x24 || op == 0x25 ||
           op == 0x27 || op == 0x28 || op == 0x29 || op == 0x2B || op == 0x37 ||
           op == 0x3F || isFpMemory(op);
}
[[nodiscard]] bool isControlFlow(u32 raw) noexcept {
    const u32 op = raw >> 26;
    if (op == 0) {
        const u32 fn = raw & 63;
        return fn == 0x08 || fn == 0x09 || fn == 0x0C || fn == 0x0D;
    }
    if (op == 0x01 || op == 0x10 || op == 0x12) return true;
    if (op == 0x11) return ((raw >> 21) & 31) == 8;
    return op == 0x02 || op == 0x03 || (op >= 0x04 && op <= 0x07) ||
           (op >= 0x14 && op <= 0x17);
}
[[nodiscard]] bool mayBail(u32 raw) noexcept {
    if (isMemory(raw >> 26)) return true;
    if ((raw >> 26) != 0x11) return false;
    const u32 rs = (raw >> 21) & 31;
    const u32 fn = raw & 63;
    return (rs == 0x10 || rs == 0x11) &&
           (fn == 0x0D || fn == 4 || (fn >= 0x38 && fn <= 0x3F));
}

[[nodiscard]] bool canTranslate(u32 raw) noexcept {
    const u32 op = raw >> 26, rs = (raw >> 21) & 31;
    const u32 rd = (raw >> 11) & 31, sa = (raw >> 6) & 31, fn = raw & 63;
    if (!raw) return true;
    if (op == 0x11) {
        if (rs == 8) return false; // Branch decode owns BC1.
        if (rs <= 1 || rs == 4 || rs == 5) return (raw & 0x7FF) == 0;
        if (rs == 0x14 || rs == 0x15) return fn == 0x20 || fn == 0x21;
        if (rs != 0x10 && rs != 0x11) return false;
        if (fn >= 0x30 && fn <= 0x3F) return true;
        if (fn <= 7 || fn == 0x0D) return true;
        return (rs == 0x10 && fn == 0x21) || (rs == 0x11 && fn == 0x20);
    }
    if (op == 9 || op == 0x19) return true;
    if (isMemory(op)) return true;
    if (op >= 0x0C && op <= 0x0E) return true;
    if (op == 0x0F && !rs) return true;
    if (op != 0) return false;
    if ((fn == 0 || fn == 2 || fn == 3 || fn == 0x38 || fn == 0x3A ||
         fn == 0x3B || fn == 0x3C || fn == 0x3E || fn == 0x3F) && !rs)
        return true;
    if ((fn == 0x10 || fn == 0x12) && !(rs | ((raw >> 16) & 31) | sa)) return true;
    if ((fn == 0x18 || fn == 0x19) && !(rd | sa)) return true;
    if (sa) return false;
    switch (fn) {
        case 0x21: case 0x23: case 0x24: case 0x25: case 0x26: case 0x27:
        case 0x2A: case 0x2B: case 0x2D: case 0x2F: return true;
        default: return false;
    }
}

[[nodiscard]] bool decodeBranch(u32 raw, u64 pc, BranchDesc& out) noexcept {
    const u32 op = raw >> 26, rs = (raw >> 21) & 31, rt = (raw >> 16) & 31;
    const s32 offset = static_cast<s16>(raw);
    const auto target = [&](s32 displacement) {
        return pc + 4 + static_cast<s64>(displacement) * 4;
    };
    if (op == 0 && ((raw & 63) == 0x08 || (raw & 63) == 0x09)) {
        const u32 rd = (raw >> 11) & 31;
        const bool jalr = (raw & 63) == 0x09;
        if (rt || ((raw >> 6) & 31) || (!jalr && rd)) return false;
        out.unconditional = out.indirect = true;
        out.rs = rs;
        out.link = jalr;
        return true;
    }
    if (op == 0x02 || op == 0x03) {
        out.target = ((pc + 4) & 0xFFFFFFFFF0000000ULL) |
                     (static_cast<u64>(raw & 0x03FFFFFFu) << 2);
        out.unconditional = true;
        out.link = op == 0x03;
        return true;
    }
    if (op == 0x04 || op == 0x05 || op == 0x06 || op == 0x07 ||
        op == 0x14 || op == 0x15 || op == 0x16 || op == 0x17) {
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
        out.likely = (sub & 2) != 0;
        out.link = (sub & 0x10) != 0;
        out.kind = (sub & 1) ? BranchKind::Bgez : BranchKind::Bltz;
        out.rs = rs;
        out.target = target(offset);
        return true;
    }
    if (op == 0x11 && rs == 8) {
        const u32 sub = rt;
        if (sub > 3) return false;
        out.likely = (sub & 2) != 0;
        out.bc1True = (sub & 1) != 0;
        out.kind = BranchKind::Bc1;
        out.target = target(offset);
        return true;
    }
    return false;
}

class Compiler {
public:
    Compiler(const Memory& ram, u64 vpc) : ram_(ram), vpc_(vpc) {}

    [[nodiscard]] bool plan() {
        u32 count = 0;
        while (count < 16) {
            const u64 slotPc = vpc_ + static_cast<u64>(count) * 4;
            if (!directFetch(slotPc)) break;
            const u32 raw = word(slotPc);
            BranchDesc branch;
            if (decodeBranch(raw, slotPc, branch)) {
                const u64 delayPc = slotPc + 4;
                if (!directFetch(delayPc)) break;
                const u32 delay = word(delayPc);
                if (isControlFlow(delay) || mayBail(delay) || !canTranslate(delay)) break;
                raws_.push_back(raw);
                raws_.push_back(delay);
                steps_ = count + 2;
                needsCop1_ = anyCop1();
                return steps_ >= 2;
            }
            if (!canTranslate(raw)) break;
            raws_.push_back(raw);
            ++count;
        }
        steps_ = count;
        needsCop1_ = anyCop1();
        return count >= 2;
    }

    void emit() {
        // Prologue: save the three host registers that stay live across chained
        // blocks, reserve a stable call frame, then cache the context pointer.
        asm_.push(RBX);
        asm_.push(R12);
        asm_.push(R13);
        if constexpr (StackReserve) asm_.subImm(RSP, StackReserve);

        asm_.movReg(CTX, Arg0);
        asm_.load64(BUDGET, ctxMem(40));
        asm_.load32(RETIRED, ctxMem(44));
        asm_.load64(RET, ctxMem(112));
        asm_.store64(ctxMem(48), RET);

        // Chained blocks enter here with the live context, budget, retired
        // count and current PC already in host registers. Reloading them from
        // Context here would restore the values from before the first block.
        chainLabel_ = asm_.label();
        asm_.bind(chainLabel_);

        epilogueLabel_ = asm_.label();
        const auto guardBail = asm_.label();
        if (needsCop1_) {
            asm_.load32(RET, ctxMem(56));
            asm_.testEax();
            asm_.jcc(CondE, guardBail);
        }
        asm_.cmpImm(BUDGET, steps_);
        asm_.jcc(CondB, guardBail);
        asm_.subImm(BUDGET, steps_);
        asm_.addImm(RETIRED, steps_);

        std::vector<std::size_t> bails;
        for (u32 raw : raws_) {
            call(executeAddress(), raw);
            asm_.testEax();
            const auto label = asm_.label();
            asm_.jcc(CondE, label);
            bails.push_back(label);
        }

        const auto exitCurrent = asm_.label();
        asm_.movReg(Arg0, CTX);
        asm_.movImm64(RET, dispatchAddress());
        asm_.callReg(RET);
        asm_.testEax();
        asm_.jcc(CondE, exitCurrent);
        asm_.jumpReg(RET);

        asm_.bind(exitCurrent);
        exitAtCurrent();
        asm_.bind(guardBail);
        exitAtCurrent();
        for (std::size_t i = 0; i < bails.size(); ++i) {
            asm_.bind(bails[i]);
            const u32 remaining = steps_ - static_cast<u32>(i);
            asm_.addImm(BUDGET, remaining);
            asm_.subImm(RETIRED, remaining);
            exitAtCurrent();
        }

        asm_.bind(epilogueLabel_);
        asm_.store32(ctxMem(40), BUDGET);
        asm_.store32(ctxMem(44), RETIRED);
        asm_.addImm(RSP, StackReserve);
        asm_.pop(R13);
        asm_.pop(R12);
        asm_.pop(RBX);
        asm_.ret();
        asm_.finalize();
    }

    [[nodiscard]] const Asm& code() const noexcept { return asm_; }
    [[nodiscard]] u32 steps() const noexcept { return steps_; }
    [[nodiscard]] std::size_t chainIndex() const noexcept { return asm_.labels[chainLabel_]; }

private:
    [[nodiscard]] u32 word(u64 pc) const noexcept {
        return readBe32(ram_.data() + physicalPc(pc));
    }
    [[nodiscard]] bool anyCop1() const noexcept {
        for (u32 raw : raws_) {
            const u32 op = raw >> 26;
            if (op == 0x11 || isFpMemory(op)) return true;
        }
        return false;
    }
    [[nodiscard]] static u64 executeAddress() noexcept {
        return reinterpret_cast<u64>(&n64forge_x64_execute);
    }
    [[nodiscard]] static u64 dispatchAddress() noexcept {
        return reinterpret_cast<u64>(&n64forge_x64_dispatch);
    }
    void call(u64 target, u32 raw) {
        asm_.movReg(Arg0, CTX);
        asm_.movImm32(Arg1, raw);
        asm_.movImm64(RET, target);
        asm_.callReg(RET);
    }
    void exitAtCurrent() {
        asm_.load64(RET, ctxMem(112));
        asm_.store64(ctxMem(48), RET);
        asm_.jump(epilogueLabel_);
    }

    const Memory& ram_;
    u64 vpc_;
    Asm asm_;
    std::vector<u32> raws_;
    u32 steps_ = 0;
    std::size_t chainLabel_ = 0;
    std::size_t epilogueLabel_ = 0;
    bool needsCop1_ = false;
};

extern "C" {

[[nodiscard]] int n64forge_x64_execute(X64Jit::Context* context, u32 raw) noexcept {
    auto* cpu = static_cast<Cpu*>(context->cpu);
    auto* bus = static_cast<Bus*>(context->bus);
    if (!cpu || !bus) return 0;
    if (!directFetch(context->currentPc)) return 0;
    if (context->rdram && readBe32(context->rdram + physicalPc(context->currentPc)) != raw)
        return 0;
    if (!cpu->stepJitInstruction(*bus, context->currentPc)) return 0;
    context->currentPc = cpu->pc();
    return 1;
}

[[nodiscard]] const void* n64forge_x64_dispatch(X64Jit::Context* context) noexcept {
    auto* state = static_cast<State*>(context->jitState);
    if (!state || !directFetch(context->currentPc)) return nullptr;
    auto* block = state->lookup(context->currentPc);
    if (!block || !block->entry || block->epoch != context->epoch ||
        block->mode != context->mode)
        return nullptr;
    if (state->lineGen) {
        for (u32 i = 0; i < block->lineCount; ++i)
            if (state->lineGen[block->line[i]].load(std::memory_order_acquire) != block->lineGen[i])
                return nullptr;
    }
    return block->body;
}

} // extern "C"

[[nodiscard]] bool linesValid(const State& state, const State::Block& block) noexcept {
    if (!state.lineGen) return true;
    for (u32 i = 0; i < block.lineCount; ++i)
        if (state.lineGen[block.line[i]].load(std::memory_order_acquire) != block.lineGen[i])
            return false;
    return true;
}

void removeBlock(State& state, State::Block* block) {
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
    auto*& slot = state.dispatch[physicalPc(vpc) >> 2];
    const bool wasPrimary = slot == block;
    state.blocks.erase(vpc);
    if (wasPrimary) {
        slot = nullptr;
        const auto alias = state.blocks.find(vpc ^ 0x20000000u);
        if (alias != state.blocks.end()) slot = alias->second.get();
    }
}

void resetAll(State& state) {
    std::fill(state.dispatch.begin(), state.dispatch.end(), nullptr);
    state.blocks.clear();
    state.lineBlocks.clear();
    std::fill(state.lineActive.begin(), state.lineActive.end(), u8{0});
    state.used = 0;
}

void recordUnsupported(State& state, u64 vpc, u64 epoch, u32 mode) {
    auto block = std::make_unique<State::Block>();
    block->vpc = vpc;
    block->epoch = epoch;
    block->mode = mode;
    block->lineCount = 1;
    block->line[0] = physicalPc(vpc) >> 6;
    block->lineGen[0] = state.lineGen
        ? state.lineGen[block->line[0]].load(std::memory_order_acquire) : 0;
    state.lineBlocks[block->line[0]].push_back(block.get());
    state.lineActive[block->line[0]] = 1;
    state.dispatch[physicalPc(vpc) >> 2] = block.get();
    state.blocks[vpc] = std::move(block);
}

void writeCode(void* destination, const void* source, std::size_t bytes) {
#if defined(_WIN32)
    DWORD previous = PAGE_READWRITE;
    VirtualProtect(destination, bytes, PAGE_READWRITE, &previous);
    std::memcpy(destination, source, bytes);
    VirtualProtect(destination, bytes, PAGE_EXECUTE_READ, &previous);
    FlushInstructionCache(GetCurrentProcess(), destination, bytes);
#else
    std::memcpy(destination, source, bytes);
    __builtin___clear_cache(static_cast<char*>(destination),
                            static_cast<char*>(destination) + bytes);
#endif
}

[[nodiscard]] State::Block* compileBlock(State& state, const Memory& ram, u64 vpc, u64 epoch,
                                         u32 mode) {
    Compiler compiler(ram, vpc);
    if (!compiler.plan()) return nullptr;
    compiler.emit();
    const auto& bytes = compiler.code().bytes;
    if (bytes.empty() || bytes.size() > State::Capacity) return nullptr;
    if (state.used + bytes.size() > State::Capacity) resetAll(state);

    auto* destination = state.arena + state.used;
    writeCode(destination, bytes.data(), bytes.size());
    state.used += bytes.size();

    auto block = std::make_unique<State::Block>();
    block->vpc = vpc;
    block->epoch = epoch;
    block->mode = mode;
    block->steps = compiler.steps();
    block->entry = destination;
    block->body = destination + compiler.chainIndex();

    const u32 firstLine = physicalPc(vpc) >> 6;
    const u32 lastLine = (physicalPc(vpc) + (block->steps - 1) * 4) >> 6;
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
    state.dispatch[physicalPc(vpc) >> 2] = raw;
    state.blocks[vpc] = std::move(block);
    return raw;
}

} // namespace

#else

struct X64Jit::State {};

#endif // N64FORGE_NATIVE_X64

X64Jit::X64Jit() : state_(std::make_unique<State>()) {}
X64Jit::~X64Jit() = default;

std::unique_ptr<X64Jit> X64Jit::create() {
#ifdef N64FORGE_NATIVE_X64
    auto result = std::unique_ptr<X64Jit>(new X64Jit);
#if defined(_WIN32)
    void* memory = VirtualAlloc(nullptr, State::Capacity, MEM_COMMIT | MEM_RESERVE,
                                PAGE_READWRITE);
    if (!memory) return {};
#else
    void* memory = mmap(nullptr, State::Capacity, PROT_READ | PROT_WRITE | PROT_EXEC,
                        MAP_PRIVATE | MAP_ANON, -1, 0);
    if (memory == MAP_FAILED) return {};
#endif
    result->state_->arena = static_cast<u8*>(memory);
    return result;
#else
    return {};
#endif
}

X64Jit::Result X64Jit::run([[maybe_unused]] const Memory& ram, [[maybe_unused]] u64 pc,
                           [[maybe_unused]] u64 epoch,
                           [[maybe_unused]] const std::atomic<u32>* lineGen,
                           [[maybe_unused]] u32 budget, [[maybe_unused]] u64* gpr,
                           [[maybe_unused]] u64* hi, [[maybe_unused]] u64* lo,
                           [[maybe_unused]] u64* fpr, [[maybe_unused]] u32* fcsr,
                           [[maybe_unused]] bool cop1, [[maybe_unused]] bool fullFpr,
                           [[maybe_unused]] Cpu* cpu, [[maybe_unused]] Bus* bus) noexcept {
#ifdef N64FORGE_NATIVE_X64
    if (!state_ || !state_->arena || !budget || !directFetch(pc) || !lineGen || !cpu || !bus)
        return {0, pc};
    State& state = *state_;
    state.lineGen = lineGen;
    if (epoch != state.epoch) {
        resetAll(state);
        state.epoch = epoch;
    }
    const u32 mode = cop1 ? (fullFpr ? 3u : 1u) : 0u;
    auto* block = state.lookup(pc);
    if (block && (block->mode != mode || block->epoch != epoch || !linesValid(state, *block))) {
        removeBlock(state, block);
        block = nullptr;
    }
    if (!block) {
        block = compileBlock(state, ram, pc, epoch, mode);
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
                    state.dispatch.data(), cpu, bus, pc, epoch, &state};
    reinterpret_cast<Entry>(block->entry)(&context);
    return {context.retired, context.pc};
#else
    return {0, pc};
#endif
}

void X64Jit::invalidateCode([[maybe_unused]] u32 physicalAddress,
                            [[maybe_unused]] u32 size) noexcept {
#ifdef N64FORGE_NATIVE_X64
    if (!state_ || !state_->arena || !size || physicalAddress >= Memory::RdramSize) return;
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
        if (it != state.blocks.end()) removeBlock(state, it->second.get());
    }
#endif
}

void X64Jit::invalidateAll() noexcept {
#ifdef N64FORGE_NATIVE_X64
    if (state_ && state_->arena) resetAll(*state_);
#endif
}

} // namespace n64forge
