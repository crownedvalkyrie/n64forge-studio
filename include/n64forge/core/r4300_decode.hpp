#pragma once
// Shared MIPS R4300i (VR4300) instruction table + decoder.
//
// This is the single source of truth for opcode identity, mnemonics and
// static semantics (branch / link / likely / load / store / register writes).
// It is consumed by the static recompiler analyzer today and is intended to
// replace the hard-coded switch in Cpu::execute and the RSP scalar decoder.
//
// The decoder is pure: no bus, no CPU state, fully deterministic.
#include "n64forge/core/types.hpp"
#include <string>
#include <string_view>

namespace n64forge {

namespace insn_flag {
inline constexpr u32 None      = 0;
inline constexpr u32 Branch    = 1u << 0;   // conditional PC-relative branch
inline constexpr u32 Jump      = 1u << 1;   // J / JAL (26-bit absolute)
inline constexpr u32 JumpReg   = 1u << 2;   // JR / JALR
inline constexpr u32 Link      = 1u << 3;   // writes return address ($ra or rd)
inline constexpr u32 Likely    = 1u << 4;   // branch-likely: delay slot annulled when not taken
inline constexpr u32 Load      = 1u << 5;
inline constexpr u32 Store     = 1u << 6;
inline constexpr u32 Fp        = 1u << 7;   // touches COP1
inline constexpr u32 Cop0      = 1u << 8;
inline constexpr u32 Trap      = 1u << 9;   // may raise a trap exception
inline constexpr u32 DstRd     = 1u << 10;  // writes GPR[rd]
inline constexpr u32 DstRt     = 1u << 11;  // writes GPR[rt]
inline constexpr u32 DstRa     = 1u << 12;  // writes GPR[31]
inline constexpr u32 Eret      = 1u << 13;
inline constexpr u32 Break     = 1u << 14;  // BREAK: no static successor
inline constexpr u32 Syscall   = 1u << 15;
inline constexpr u32 Dword     = 1u << 16;  // requires 64-bit operation semantics
inline constexpr u32 HasDelay  = Branch | Jump | JumpReg;
}  // namespace insn_flag

// X(Name, "mnemonic", flags)
#define N64FORGE_R4300_OPS(X)                                                          \
    X(Sll, "sll", insn_flag::DstRd)                                                    \
    X(Srl, "srl", insn_flag::DstRd)                                                    \
    X(Sra, "sra", insn_flag::DstRd)                                                    \
    X(Sllv, "sllv", insn_flag::DstRd)                                                  \
    X(Srlv, "srlv", insn_flag::DstRd)                                                  \
    X(Srav, "srav", insn_flag::DstRd)                                                  \
    X(Jr, "jr", insn_flag::JumpReg)                                                    \
    X(Jalr, "jalr", insn_flag::JumpReg | insn_flag::Link | insn_flag::DstRd)           \
    X(Syscall, "syscall", insn_flag::Syscall)                                          \
    X(Break, "break", insn_flag::Break)                                                \
    X(Sync, "sync", insn_flag::None)                                                   \
    X(Mfhi, "mfhi", insn_flag::DstRd)                                                  \
    X(Mthi, "mthi", insn_flag::None)                                                   \
    X(Mflo, "mflo", insn_flag::DstRd)                                                  \
    X(Mtlo, "mtlo", insn_flag::None)                                                   \
    X(Dsllv, "dsllv", insn_flag::DstRd | insn_flag::Dword)                             \
    X(Dsrlv, "dsrlv", insn_flag::DstRd | insn_flag::Dword)                             \
    X(Dsrav, "dsrav", insn_flag::DstRd | insn_flag::Dword)                             \
    X(Mult, "mult", insn_flag::None)                                                   \
    X(Multu, "multu", insn_flag::None)                                                 \
    X(Div, "div", insn_flag::None)                                                     \
    X(Divu, "divu", insn_flag::None)                                                   \
    X(Dmult, "dmult", insn_flag::Dword)                                                \
    X(Dmultu, "dmultu", insn_flag::Dword)                                              \
    X(Ddiv, "ddiv", insn_flag::Dword)                                                  \
    X(Ddivu, "ddivu", insn_flag::Dword)                                                \
    X(Add, "add", insn_flag::DstRd | insn_flag::Trap)                                  \
    X(Addu, "addu", insn_flag::DstRd)                                                  \
    X(Sub, "sub", insn_flag::DstRd | insn_flag::Trap)                                  \
    X(Subu, "subu", insn_flag::DstRd)                                                  \
    X(And, "and", insn_flag::DstRd)                                                    \
    X(Or, "or", insn_flag::DstRd)                                                      \
    X(Xor, "xor", insn_flag::DstRd)                                                    \
    X(Nor, "nor", insn_flag::DstRd)                                                    \
    X(Slt, "slt", insn_flag::DstRd)                                                    \
    X(Sltu, "sltu", insn_flag::DstRd)                                                  \
    X(Dadd, "dadd", insn_flag::DstRd | insn_flag::Trap | insn_flag::Dword)             \
    X(Daddu, "daddu", insn_flag::DstRd | insn_flag::Dword)                             \
    X(Dsub, "dsub", insn_flag::DstRd | insn_flag::Trap | insn_flag::Dword)             \
    X(Dsubu, "dsubu", insn_flag::DstRd | insn_flag::Dword)                             \
    X(Tge, "tge", insn_flag::Trap)                                                     \
    X(Tgeu, "tgeu", insn_flag::Trap)                                                   \
    X(Tlt, "tlt", insn_flag::Trap)                                                     \
    X(Tltu, "tltu", insn_flag::Trap)                                                   \
    X(Teq, "teq", insn_flag::Trap)                                                     \
    X(Tne, "tne", insn_flag::Trap)                                                     \
    X(Dsll, "dsll", insn_flag::DstRd | insn_flag::Dword)                               \
    X(Dsrl, "dsrl", insn_flag::DstRd | insn_flag::Dword)                               \
    X(Dsra, "dsra", insn_flag::DstRd | insn_flag::Dword)                               \
    X(Dsll32, "dsll32", insn_flag::DstRd | insn_flag::Dword)                           \
    X(Dsrl32, "dsrl32", insn_flag::DstRd | insn_flag::Dword)                           \
    X(Dsra32, "dsra32", insn_flag::DstRd | insn_flag::Dword)                           \
    X(Bltz, "bltz", insn_flag::Branch)                                                 \
    X(Bgez, "bgez", insn_flag::Branch)                                                 \
    X(Bltzl, "bltzl", insn_flag::Branch | insn_flag::Likely)                           \
    X(Bgezl, "bgezl", insn_flag::Branch | insn_flag::Likely)                           \
    X(Tgei, "tgei", insn_flag::Trap)                                                   \
    X(Tgeiu, "tgeiu", insn_flag::Trap)                                                 \
    X(Tlti, "tlti", insn_flag::Trap)                                                   \
    X(Tltiu, "tltiu", insn_flag::Trap)                                                 \
    X(Teqi, "teqi", insn_flag::Trap)                                                   \
    X(Tnei, "tnei", insn_flag::Trap)                                                   \
    X(Bltzal, "bltzal", insn_flag::Branch | insn_flag::Link | insn_flag::DstRa)        \
    X(Bgezal, "bgezal", insn_flag::Branch | insn_flag::Link | insn_flag::DstRa)        \
    X(Bltzall, "bltzall",                                                              \
      insn_flag::Branch | insn_flag::Link | insn_flag::DstRa | insn_flag::Likely)      \
    X(Bgezall, "bgezall",                                                              \
      insn_flag::Branch | insn_flag::Link | insn_flag::DstRa | insn_flag::Likely)      \
    X(J, "j", insn_flag::Jump)                                                         \
    X(Jal, "jal", insn_flag::Jump | insn_flag::Link | insn_flag::DstRa)                \
    X(Beq, "beq", insn_flag::Branch)                                                   \
    X(Bne, "bne", insn_flag::Branch)                                                   \
    X(Blez, "blez", insn_flag::Branch)                                                 \
    X(Bgtz, "bgtz", insn_flag::Branch)                                                 \
    X(Addi, "addi", insn_flag::DstRt | insn_flag::Trap)                                \
    X(Addiu, "addiu", insn_flag::DstRt)                                                \
    X(Slti, "slti", insn_flag::DstRt)                                                  \
    X(Sltiu, "sltiu", insn_flag::DstRt)                                                \
    X(Andi, "andi", insn_flag::DstRt)                                                  \
    X(Ori, "ori", insn_flag::DstRt)                                                    \
    X(Xori, "xori", insn_flag::DstRt)                                                  \
    X(Lui, "lui", insn_flag::DstRt)                                                    \
    X(Mfc0, "mfc0", insn_flag::Cop0 | insn_flag::DstRt)                                \
    X(Dmfc0, "dmfc0", insn_flag::Cop0 | insn_flag::DstRt | insn_flag::Dword)           \
    X(Mtc0, "mtc0", insn_flag::Cop0)                                                   \
    X(Dmtc0, "dmtc0", insn_flag::Cop0 | insn_flag::Dword)                              \
    X(Tlbr, "tlbr", insn_flag::Cop0)                                                   \
    X(Tlbwi, "tlbwi", insn_flag::Cop0)                                                 \
    X(Tlbwr, "tlbwr", insn_flag::Cop0)                                                 \
    X(Tlbp, "tlbp", insn_flag::Cop0)                                                   \
    X(Eret, "eret", insn_flag::Cop0 | insn_flag::Eret)                                 \
    X(Mfc1, "mfc1", insn_flag::Fp | insn_flag::DstRt)                                  \
    X(Dmfc1, "dmfc1", insn_flag::Fp | insn_flag::DstRt | insn_flag::Dword)             \
    X(Cfc1, "cfc1", insn_flag::Fp | insn_flag::DstRt)                                  \
    X(Mtc1, "mtc1", insn_flag::Fp)                                                     \
    X(Dmtc1, "dmtc1", insn_flag::Fp | insn_flag::Dword)                                \
    X(Ctc1, "ctc1", insn_flag::Fp)                                                     \
    X(Bc1f, "bc1f", insn_flag::Branch | insn_flag::Fp)                                 \
    X(Bc1t, "bc1t", insn_flag::Branch | insn_flag::Fp)                                 \
    X(Bc1fl, "bc1fl", insn_flag::Branch | insn_flag::Fp | insn_flag::Likely)           \
    X(Bc1tl, "bc1tl", insn_flag::Branch | insn_flag::Fp | insn_flag::Likely)           \
    X(FAdd, "add", insn_flag::Fp)                                                      \
    X(FSub, "sub", insn_flag::Fp)                                                      \
    X(FMul, "mul", insn_flag::Fp)                                                      \
    X(FDiv, "div", insn_flag::Fp)                                                      \
    X(FSqrt, "sqrt", insn_flag::Fp)                                                    \
    X(FAbs, "abs", insn_flag::Fp)                                                      \
    X(FMov, "mov", insn_flag::Fp)                                                      \
    X(FNeg, "neg", insn_flag::Fp)                                                      \
    X(FRoundL, "round.l", insn_flag::Fp)                                               \
    X(FTruncL, "trunc.l", insn_flag::Fp)                                               \
    X(FCeilL, "ceil.l", insn_flag::Fp)                                                 \
    X(FFloorL, "floor.l", insn_flag::Fp)                                               \
    X(FRoundW, "round.w", insn_flag::Fp)                                               \
    X(FTruncW, "trunc.w", insn_flag::Fp)                                               \
    X(FCeilW, "ceil.w", insn_flag::Fp)                                                 \
    X(FFloorW, "floor.w", insn_flag::Fp)                                               \
    X(FCvtS, "cvt.s", insn_flag::Fp)                                                   \
    X(FCvtD, "cvt.d", insn_flag::Fp)                                                   \
    X(FCvtW, "cvt.w", insn_flag::Fp)                                                   \
    X(FCvtL, "cvt.l", insn_flag::Fp)                                                   \
    X(FCmp, "c", insn_flag::Fp)                                                        \
    X(Beql, "beql", insn_flag::Branch | insn_flag::Likely)                             \
    X(Bnel, "bnel", insn_flag::Branch | insn_flag::Likely)                             \
    X(Blezl, "blezl", insn_flag::Branch | insn_flag::Likely)                           \
    X(Bgtzl, "bgtzl", insn_flag::Branch | insn_flag::Likely)                           \
    X(Daddi, "daddi", insn_flag::DstRt | insn_flag::Trap | insn_flag::Dword)           \
    X(Daddiu, "daddiu", insn_flag::DstRt | insn_flag::Dword)                           \
    X(Ldl, "ldl", insn_flag::Load | insn_flag::DstRt | insn_flag::Dword)               \
    X(Ldr, "ldr", insn_flag::Load | insn_flag::DstRt | insn_flag::Dword)               \
    X(Lb, "lb", insn_flag::Load | insn_flag::DstRt)                                    \
    X(Lh, "lh", insn_flag::Load | insn_flag::DstRt)                                    \
    X(Lwl, "lwl", insn_flag::Load | insn_flag::DstRt)                                  \
    X(Lw, "lw", insn_flag::Load | insn_flag::DstRt)                                    \
    X(Lbu, "lbu", insn_flag::Load | insn_flag::DstRt)                                  \
    X(Lhu, "lhu", insn_flag::Load | insn_flag::DstRt)                                  \
    X(Lwr, "lwr", insn_flag::Load | insn_flag::DstRt)                                  \
    X(Lwu, "lwu", insn_flag::Load | insn_flag::DstRt)                                  \
    X(Sb, "sb", insn_flag::Store)                                                      \
    X(Sh, "sh", insn_flag::Store)                                                      \
    X(Swl, "swl", insn_flag::Store)                                                    \
    X(Sw, "sw", insn_flag::Store)                                                      \
    X(Sdl, "sdl", insn_flag::Store | insn_flag::Dword)                                 \
    X(Sdr, "sdr", insn_flag::Store | insn_flag::Dword)                                 \
    X(Swr, "swr", insn_flag::Store)                                                    \
    X(Cache, "cache", insn_flag::None)                                                 \
    X(Ll, "ll", insn_flag::Load | insn_flag::DstRt)                                    \
    X(Lwc1, "lwc1", insn_flag::Load | insn_flag::Fp)                                   \
    X(Lld, "lld", insn_flag::Load | insn_flag::DstRt | insn_flag::Dword)               \
    X(Ldc1, "ldc1", insn_flag::Load | insn_flag::Fp | insn_flag::Dword)                \
    X(Ld, "ld", insn_flag::Load | insn_flag::DstRt | insn_flag::Dword)                 \
    X(Sc, "sc", insn_flag::Store | insn_flag::DstRt)                                   \
    X(Swc1, "swc1", insn_flag::Store | insn_flag::Fp)                                  \
    X(Scd, "scd", insn_flag::Store | insn_flag::DstRt | insn_flag::Dword)              \
    X(Sdc1, "sdc1", insn_flag::Store | insn_flag::Fp | insn_flag::Dword)               \
    X(Sd, "sd", insn_flag::Store | insn_flag::Dword)

enum class Op : u16 {
    Invalid = 0,
#define N64FORGE_X(name, mnem, flags) name,
    N64FORGE_R4300_OPS(N64FORGE_X)
#undef N64FORGE_X
    Count_
};

// COP1 format field (bits 25:21).
enum class FpFmt : u8 { None = 0, S = 16, D = 17, W = 20, L = 21 };

struct DecodedInsn {
    u32 raw = 0;
    Op op = Op::Invalid;
    u32 flags = 0;
    u8 rs = 0;      // also COP1 'fmt' source field for FP ops
    u8 rt = 0;      // also ft
    u8 rd = 0;      // also fs
    u8 sa = 0;      // also fd
    u8 cond = 0;    // C.cond.fmt condition (funct & 0xF)
    FpFmt fmt = FpFmt::None;
    s32 imm = 0;    // sign-extended 16-bit immediate
    u32 target = 0; // 26-bit jump field

    [[nodiscard]] bool valid() const noexcept { return op != Op::Invalid; }
    [[nodiscard]] bool has(u32 f) const noexcept { return (flags & f) != 0; }
    [[nodiscard]] bool hasDelaySlot() const noexcept { return has(insn_flag::HasDelay); }
    [[nodiscard]] bool isLikely() const noexcept { return has(insn_flag::Likely); }
    [[nodiscard]] bool isCall() const noexcept;           // JAL, JALR, BxxAL
    [[nodiscard]] bool isReturn() const noexcept;         // JR $ra
    [[nodiscard]] bool isUnconditionalBranch() const noexcept;  // b / beq r,r / bgez $0 / bc-less
    // Branch destination for PC-relative branches (pc = address of the branch).
    [[nodiscard]] u32 branchTarget(u32 pc) const noexcept {
        return pc + 4u + (static_cast<u32>(imm) << 2);
    }
    // Absolute destination for J/JAL (pc = address of the jump).
    [[nodiscard]] u32 jumpTarget(u32 pc) const noexcept {
        return ((pc + 4u) & 0xF0000000u) | (target << 2);
    }
    // GPR written by this instruction, or -1 (does not report $zero specially).
    [[nodiscard]] int dstGpr() const noexcept;
};

[[nodiscard]] DecodedInsn decodeR4300(u32 raw) noexcept;
[[nodiscard]] std::string_view mnemonic(Op op) noexcept;
[[nodiscard]] std::string disassemble(const DecodedInsn& insn, u32 pc);
[[nodiscard]] std::string_view gprName(unsigned index) noexcept;

}  // namespace n64forge
