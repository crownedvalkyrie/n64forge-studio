#include "n64forge/recomp/emitter.hpp"
#include "n64forge/core/r4300_decode.hpp"
#include "n64forge/recomp/recomp_rt.hpp"

#include <algorithm>
#include <cassert>
#include <cctype>
#include <cstdio>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

namespace n64forge::recomp {

namespace {

using namespace n64forge;

[[nodiscard]] std::string hex(u32 v) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "%08X", v);
    return buf;
}

[[nodiscard]] std::string upper(std::string_view text) {
    std::string result(text);
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });
    return result;
}

[[nodiscard]] std::string label(u32 pc) {
    return "L_" + hex(pc);
}

[[nodiscard]] std::string funcName(u32 pc, const std::string& prefix) {
    return prefix + hex(pc);
}

[[nodiscard]] std::string sextImm(const DecodedInsn& d) {
    return std::to_string(d.imm);
}

[[nodiscard]] std::string zextImm(const DecodedInsn& d) {
    return std::to_string(static_cast<u16>(d.imm));
}

[[nodiscard]] std::string gpr(u8 idx) {
    if (idx == 0) return "ctx.r[0]";
    return "ctx.r[" + std::to_string(idx) + "]";
}

[[nodiscard]] std::string fpr(u8 idx) {
    return "ctx.f[" + std::to_string(idx) + "]";
}

[[nodiscard]] std::string cop0(u8 idx) {
    return "ctx.cop0[" + std::to_string(idx) + "]";
}

class Emitter {
public:
    Emitter(const Function& fn, const CodeImage& img, const EmitOptions& opt)
        : fn_(fn), img_(img), opt_(opt) {}

    [[nodiscard]] std::string emit() {
        std::ostringstream out;
        out << "void " << funcName(fn_.entry, opt_.functionPrefix) << "(RecompContext& ctx) {\n";
        out << "ctx.r[0] = 0;\n";
        for (const auto& [start, bb] : fn_.blocks) {
            (void)start;
            out << "{\n"; // Keep block-local temporaries out of other label scopes.
            emitBlock(out, bb);
            out << "}\n";
        }
        out << "}\n";
        if (unsupported_.empty()) return out.str();

        std::ostringstream result;
        for (const auto& message : unsupported_) result << "#error " << message << '\n';
        result << out.str();
        return result.str();
    }

    [[nodiscard]] const std::vector<std::string>& unsupported() const noexcept { return unsupported_; }

private:
    void markUnsupported(u32 pc, std::string_view what) {
        char buf[160];
        const std::string name = upper(what);
        std::snprintf(buf, sizeof buf, "Unsupported MIPS %.*s at 0x%08X",
                      static_cast<int>(name.size()), name.data(), pc);
        unsupported_.push_back(buf);
    }

    void emitBlock(std::ostringstream& out, const BasicBlock& bb) {
        out << label(bb.start) << ":\n";
        u32 pc = bb.start;
        while (pc < bb.end) {
            u32 w;
            if (!img_.read32(pc, w)) {
                out << "// unreadable at 0x" << hex(pc) << "\n";
                out << "return;\n";
                return;
            }
            const DecodedInsn d = decodeR4300(w);
            if (pc == bb.termPc) {
                emitTerminator(out, bb, pc, d);
                return;
            }
            if (d.op == Op::Jal || d.op == Op::Jalr) {
                emitCall(out, pc, d);
                pc += 8;  // the call's delay slot is emitted by emitCall
                continue;
            }
            emitInsn(out, pc, d);
            pc += 4;
        }
        // Reached end without terminator (should not happen for valid blocks).
        if (bb.term == Terminator::Fallthrough && !bb.succs.empty()) {
            out << "goto " << label(bb.succs.front().target) << ";\n";
        } else {
            out << "return;\n";
        }
    }

    void emitCall(std::ostringstream& out, u32 pc, const DecodedInsn& d) {
        u32 slotRaw = 0;
        DecodedInsn slot;
        if (img_.read32(pc + 4, slotRaw)) slot = decodeR4300(slotRaw);

        if (d.op == Op::Jalr) {
            // jalr reads its target before the delay slot executes.
            out << "{\nconst u32 callTarget = static_cast<u32>(ctx.r[" << (int)d.rs << "]);\n";
            if (d.rd != 0) out << "ctx.r[" << (int)d.rd << "] = 0x" << hex(pc + 8) << ";\n";
            if (slotRaw) emitInsn(out, pc + 4, slot);
            out << "dispatchIndirect(ctx, callTarget);\n}\n";
            return;
        }

        out << "ctx.r[31] = 0x" << hex(pc + 8) << ";\n";
        if (slotRaw) emitInsn(out, pc + 4, slot);
        u32 target = canonicalAddress(d.jumpTarget(pc));
        for (const auto& call : fn_.calls) {
            if (call.pc == pc && call.target) target = call.target;
        }
        out << "dispatchIndirect(ctx, 0x" << hex(target) << "u);\n";
    }

    void emitInsn(std::ostringstream& out, u32 pc, const DecodedInsn& d) {
        (void)pc;
        switch (d.op) {
            // Shifts
            case Op::Sll:  out << gpr(d.rd) << " = S32(ctx.r[" << (int)d.rt << "] << " << (int)d.sa << ");\n"; break;
            case Op::Srl:  out << gpr(d.rd) << " = S32(ctx.r[" << (int)d.rt << "] >> " << (int)d.sa << ");\n"; break;
            case Op::Sra:  out << gpr(d.rd) << " = S32(static_cast<s32>(ctx.r[" << (int)d.rt << "]) >> " << (int)d.sa << ");\n"; break;
            case Op::Sllv: out << gpr(d.rd) << " = S32(ctx.r[" << (int)d.rt << "] << (ctx.r[" << (int)d.rs << "] & 31));\n"; break;
            case Op::Srlv: out << gpr(d.rd) << " = S32(ctx.r[" << (int)d.rt << "] >> (ctx.r[" << (int)d.rs << "] & 31));\n"; break;
            case Op::Srav: out << gpr(d.rd) << " = S32(static_cast<s32>(ctx.r[" << (int)d.rt << "]) >> (ctx.r[" << (int)d.rs << "] & 31));\n"; break;
            case Op::Dsllv: out << gpr(d.rd) << " = ctx.r[" << (int)d.rt << "] << (ctx.r[" << (int)d.rs << "] & 63);\n"; break;
            case Op::Dsrlv: out << gpr(d.rd) << " = ctx.r[" << (int)d.rt << "] >> (ctx.r[" << (int)d.rs << "] & 63);\n"; break;
            case Op::Dsrav: out << gpr(d.rd) << " = static_cast<s64>(ctx.r[" << (int)d.rt << "]) >> (ctx.r[" << (int)d.rs << "] & 63);\n"; break;
            case Op::Dsll:  out << gpr(d.rd) << " = ctx.r[" << (int)d.rt << "] << " << (int)d.sa << ";\n"; break;
            case Op::Dsrl:  out << gpr(d.rd) << " = ctx.r[" << (int)d.rt << "] >> " << (int)d.sa << ";\n"; break;
            case Op::Dsra:  out << gpr(d.rd) << " = static_cast<s64>(ctx.r[" << (int)d.rt << "]) >> " << (int)d.sa << ";\n"; break;
            case Op::Dsll32: out << gpr(d.rd) << " = ctx.r[" << (int)d.rt << "] << (" << (int)d.sa << " + 32);\n"; break;
            case Op::Dsrl32: out << gpr(d.rd) << " = ctx.r[" << (int)d.rt << "] >> (" << (int)d.sa << " + 32);\n"; break;
            case Op::Dsra32: out << gpr(d.rd) << " = static_cast<s64>(ctx.r[" << (int)d.rt << "]) >> (" << (int)d.sa << " + 32);\n"; break;

            // Arithmetic
            case Op::Add:  out << gpr(d.rd) << " = ADD32(ctx.r[" << (int)d.rs << "], ctx.r[" << (int)d.rt << "]);\n"; break;
            case Op::Addu: out << gpr(d.rd) << " = ADD32(ctx.r[" << (int)d.rs << "], ctx.r[" << (int)d.rt << "]);\n"; break;
            case Op::Sub:  out << gpr(d.rd) << " = SUB32(ctx.r[" << (int)d.rs << "], ctx.r[" << (int)d.rt << "]);\n"; break;
            case Op::Subu: out << gpr(d.rd) << " = SUB32(ctx.r[" << (int)d.rs << "], ctx.r[" << (int)d.rt << "]);\n"; break;
            case Op::Dadd: out << gpr(d.rd) << " = ctx.r[" << (int)d.rs << "] + ctx.r[" << (int)d.rt << "];\n"; break;
            case Op::Daddu: out << gpr(d.rd) << " = ctx.r[" << (int)d.rs << "] + ctx.r[" << (int)d.rt << "];\n"; break;
            case Op::Dsub: out << gpr(d.rd) << " = ctx.r[" << (int)d.rs << "] - ctx.r[" << (int)d.rt << "];\n"; break;
            case Op::Dsubu: out << gpr(d.rd) << " = ctx.r[" << (int)d.rs << "] - ctx.r[" << (int)d.rt << "];\n"; break;
            case Op::Addi: out << gpr(d.rt) << " = ADD32(ctx.r[" << (int)d.rs << "], " << sextImm(d) << ");\n"; break;
            case Op::Addiu: out << gpr(d.rt) << " = ADD32(ctx.r[" << (int)d.rs << "], " << sextImm(d) << ");\n"; break;
            case Op::Daddi: out << gpr(d.rt) << " = static_cast<s64>(ctx.r[" << (int)d.rs << "]) + " << sextImm(d) << ";\n"; break;
            case Op::Daddiu: out << gpr(d.rt) << " = ctx.r[" << (int)d.rs << "] + " << sextImm(d) << ";\n"; break;

            // Logical
            case Op::And:  out << gpr(d.rd) << " = ctx.r[" << (int)d.rs << "] & ctx.r[" << (int)d.rt << "];\n"; break;
            case Op::Or:   out << gpr(d.rd) << " = ctx.r[" << (int)d.rs << "] | ctx.r[" << (int)d.rt << "];\n"; break;
            case Op::Xor:  out << gpr(d.rd) << " = ctx.r[" << (int)d.rs << "] ^ ctx.r[" << (int)d.rt << "];\n"; break;
            case Op::Nor:  out << gpr(d.rd) << " = ~(ctx.r[" << (int)d.rs << "] | ctx.r[" << (int)d.rt << "]);\n"; break;
            case Op::Andi: out << gpr(d.rt) << " = ctx.r[" << (int)d.rs << "] & " << zextImm(d) << ";\n"; break;
            case Op::Ori:  out << gpr(d.rt) << " = ctx.r[" << (int)d.rs << "] | " << zextImm(d) << ";\n"; break;
            case Op::Xori: out << gpr(d.rt) << " = ctx.r[" << (int)d.rs << "] ^ " << zextImm(d) << ";\n"; break;
            case Op::Lui:  out << gpr(d.rt) << " = S32(0x" << hex(static_cast<u32>(d.imm) << 16) << "u);\n"; break;

            // Compare
            case Op::Slt:  out << gpr(d.rd) << " = (static_cast<s64>(ctx.r[" << (int)d.rs << "]) < static_cast<s64>(ctx.r[" << (int)d.rt << "])) ? 1 : 0;\n"; break;
            case Op::Sltu: out << gpr(d.rd) << " = (ctx.r[" << (int)d.rs << "] < ctx.r[" << (int)d.rt << "]) ? 1 : 0;\n"; break;
            case Op::Slti: out << gpr(d.rt) << " = (static_cast<s64>(ctx.r[" << (int)d.rs << "]) < " << sextImm(d) << ") ? 1 : 0;\n"; break;
            case Op::Sltiu: out << gpr(d.rt) << " = (ctx.r[" << (int)d.rs << "] < static_cast<u64>(static_cast<s64>(" << sextImm(d) << "))) ? 1 : 0;\n"; break;

            // HI/LO
            case Op::Mfhi: out << gpr(d.rd) << " = ctx.hi;\n"; break;
            case Op::Mflo: out << gpr(d.rd) << " = ctx.lo;\n"; break;
            case Op::Mthi: out << "ctx.hi = ctx.r[" << (int)d.rs << "];\n"; break;
            case Op::Mtlo: out << "ctx.lo = ctx.r[" << (int)d.rs << "];\n"; break;

            case Op::Mult: {
                out << "{ s64 prod = static_cast<s32>(ctx.r[" << (int)d.rs << "]) * static_cast<s32>(ctx.r[" << (int)d.rt << "]); ctx.lo = S32(prod); ctx.hi = S32(prod >> 32); }\n";
                break;
            }
            case Op::Multu: {
                out << "{ u64 prod = static_cast<u32>(ctx.r[" << (int)d.rs << "]) * static_cast<u32>(ctx.r[" << (int)d.rt << "]); ctx.lo = S32(prod); ctx.hi = S32(prod >> 32); }\n";
                break;
            }
            case Op::Dmult: {
                out << "{ auto prod = DMULT(static_cast<s64>(ctx.r[" << (int)d.rs << "]), static_cast<s64>(ctx.r[" << (int)d.rt << "])); ctx.lo = prod.first; ctx.hi = prod.second; }\n";
                break;
            }
            case Op::Dmultu: {
                out << "{ auto prod = DMULTU(ctx.r[" << (int)d.rs << "], ctx.r[" << (int)d.rt << "]); ctx.lo = prod.first; ctx.hi = prod.second; }\n";
                break;
            }
            case Op::Div: {
                out << "{ ctx.lo = DIV32(static_cast<s64>(ctx.r[" << (int)d.rs << "]), static_cast<s64>(ctx.r[" << (int)d.rt << "])); "
                       "ctx.hi = REM32(static_cast<s64>(ctx.r[" << (int)d.rs << "]), static_cast<s64>(ctx.r[" << (int)d.rt << "])); }\n";
                break;
            }
            case Op::Divu: {
                out << "{ u32 a=static_cast<u32>(ctx.r[" << (int)d.rs << "]); u32 b=static_cast<u32>(ctx.r[" << (int)d.rt << "]); if(b){ctx.lo=S32(a/b);ctx.hi=S32(a%b);} }\n";
                break;
            }
            case Op::Ddiv: {
                out << "{ ctx.lo = DIV64(static_cast<s64>(ctx.r[" << (int)d.rs << "]), static_cast<s64>(ctx.r[" << (int)d.rt << "])); "
                       "ctx.hi = REM64(static_cast<s64>(ctx.r[" << (int)d.rs << "]), static_cast<s64>(ctx.r[" << (int)d.rt << "])); }\n";
                break;
            }
            case Op::Ddivu: {
                out << "{ u64 a=ctx.r[" << (int)d.rs << "]; u64 b=ctx.r[" << (int)d.rt << "]; if(b){ctx.lo=a/b;ctx.hi=a%b;} }\n";
                break;
            }

            // Loads
            case Op::Lb:  out << gpr(d.rt) << " = MEM_B(ctx, ctx.r[" << (int)d.rs << "] + " << sextImm(d) << ");\n"; break;
            case Op::Lh:  out << gpr(d.rt) << " = MEM_H(ctx, ctx.r[" << (int)d.rs << "] + " << sextImm(d) << ");\n"; break;
            case Op::Lw:  out << gpr(d.rt) << " = MEM_W(ctx, ctx.r[" << (int)d.rs << "] + " << sextImm(d) << ");\n"; break;
            case Op::Lbu: out << gpr(d.rt) << " = MEM_BU(ctx, ctx.r[" << (int)d.rs << "] + " << sextImm(d) << ");\n"; break;
            case Op::Lhu: out << gpr(d.rt) << " = MEM_HU(ctx, ctx.r[" << (int)d.rs << "] + " << sextImm(d) << ");\n"; break;
            case Op::Lwu: out << gpr(d.rt) << " = MEM_WU(ctx, ctx.r[" << (int)d.rs << "] + " << sextImm(d) << ");\n"; break;
            case Op::Ld:  out << gpr(d.rt) << " = MEM_D(ctx, ctx.r[" << (int)d.rs << "] + " << sextImm(d) << ");\n"; break;
            case Op::Lwl: out << gpr(d.rt) << " = S32(LWL(ctx, ctx.r[" << (int)d.rs << "] + " << sextImm(d) << ", static_cast<u32>(ctx.r[" << (int)d.rt << "])));\n"; break;
            case Op::Lwr: out << gpr(d.rt) << " = S32(LWR(ctx, ctx.r[" << (int)d.rs << "] + " << sextImm(d) << ", static_cast<u32>(ctx.r[" << (int)d.rt << "])));\n"; break;
            case Op::Ldl:
            case Op::Ldr: {
                const std::string name(mnemonic(d.op));
                markUnsupported(pc, name.c_str());
                out << "// " << name << " is not emitted\n";
                break;
            }

            // Stores
            case Op::Sb:  out << "MEM_B(ctx, ctx.r[" << (int)d.rs << "] + " << sextImm(d) << ", static_cast<u8>(ctx.r[" << (int)d.rt << "]));\n"; break;
            case Op::Sh:  out << "MEM_H(ctx, ctx.r[" << (int)d.rs << "] + " << sextImm(d) << ", static_cast<u16>(ctx.r[" << (int)d.rt << "]));\n"; break;
            case Op::Sw:  out << "MEM_W(ctx, ctx.r[" << (int)d.rs << "] + " << sextImm(d) << ", static_cast<u32>(ctx.r[" << (int)d.rt << "]));\n"; break;
            case Op::Sd:  out << "MEM_D(ctx, ctx.r[" << (int)d.rs << "] + " << sextImm(d) << ", ctx.r[" << (int)d.rt << "]);\n"; break;
            case Op::Swl: out << "SWL(ctx, ctx.r[" << (int)d.rs << "] + " << sextImm(d) << ", static_cast<u32>(ctx.r[" << (int)d.rt << "]));\n"; break;
            case Op::Swr: out << "SWR(ctx, ctx.r[" << (int)d.rs << "] + " << sextImm(d) << ", static_cast<u32>(ctx.r[" << (int)d.rt << "]));\n"; break;
            case Op::Sdl:
            case Op::Sdr:
                markUnsupported(pc, mnemonic(d.op).data());
                out << "// " << mnemonic(d.op) << " is not emitted\n";
                break;

            // COP0
            case Op::Mfc0:  out << gpr(d.rt) << " = S32(" << cop0(d.rd) << ");\n"; break;
            case Op::Dmfc0: out << gpr(d.rt) << " = " << cop0(d.rd) << ";\n"; break;
            case Op::Mtc0:  out << cop0(d.rd) << " = ctx.r[" << (int)d.rt << "];\n"; break;
            case Op::Dmtc0: out << cop0(d.rd) << " = ctx.r[" << (int)d.rt << "];\n"; break;
            case Op::Eret:
                markUnsupported(pc, mnemonic(d.op));
                out << "// ERET stub\nreturn;\n";
                break;
            case Op::Tlbr:
            case Op::Tlbwi:
            case Op::Tlbwr:
            case Op::Tlbp:
                markUnsupported(pc, mnemonic(d.op));
                out << "// TLB op stub\n";
                break;

            // COP1 move
            case Op::Mfc1:  out << gpr(d.rt) << " = S32(" << fpr(d.rs) << ");\n"; break;
            case Op::Dmfc1: out << gpr(d.rt) << " = " << fpr(d.rs) << ";\n"; break;
            case Op::Cfc1:  out << gpr(d.rt) << " = (" << (int)d.rs << " == 31) ? ctx.fcr31 : ctx.fcr0;\n"; break;
            case Op::Mtc1:  out << fpr(d.rs) << " = static_cast<u32>(ctx.r[" << (int)d.rt << "]);\n"; break;
            case Op::Dmtc1: out << fpr(d.rs) << " = ctx.r[" << (int)d.rt << "];\n"; break;
            case Op::Ctc1:  out << "if (" << (int)d.rs << " == 31) ctx.fcr31 = static_cast<u32>(ctx.r[" << (int)d.rt << "]);\n"; break;

            // FP arithmetic (pass-through via reinterpretation as double).
            case Op::FAdd:
            case Op::FSub:
            case Op::FMul:
            case Op::FDiv:
            case Op::FSqrt:
            case Op::FAbs:
            case Op::FMov:
            case Op::FNeg:
            case Op::FRoundL:
            case Op::FTruncL:
            case Op::FCeilL:
            case Op::FFloorL:
            case Op::FRoundW:
            case Op::FTruncW:
            case Op::FCeilW:
            case Op::FFloorW:
            case Op::FCvtS:
            case Op::FCvtD:
            case Op::FCvtW:
            case Op::FCvtL:
            case Op::FCmp:
                emitFpStub(out, pc, d);
                break;

            case Op::Lwc1: out << fpr(d.rt) << " = MEM_WU(ctx, ctx.r[" << (int)d.rs << "] + " << sextImm(d) << ");\n"; break;
            case Op::Ldc1: out << fpr(d.rt) << " = MEM_D(ctx, ctx.r[" << (int)d.rs << "] + " << sextImm(d) << ");\n"; break;
            case Op::Swc1: out << "MEM_W(ctx, ctx.r[" << (int)d.rs << "] + " << sextImm(d) << ", static_cast<u32>(" << fpr(d.rt) << "));\n"; break;
            case Op::Sdc1: out << "MEM_D(ctx, ctx.r[" << (int)d.rs << "] + " << sextImm(d) << ", " << fpr(d.rt) << ");\n"; break;

            // Traps / syscall / break / sync
            case Op::Tge:
            case Op::Tgeu:
            case Op::Tlt:
            case Op::Tltu:
            case Op::Teq:
            case Op::Tne:
            case Op::Tgei:
            case Op::Tgeiu:
            case Op::Tlti:
            case Op::Tltiu:
            case Op::Teqi:
            case Op::Tnei:
                markUnsupported(pc, mnemonic(d.op));
                out << "// trap stub\n";
                break;
            case Op::Syscall:
                markUnsupported(pc, mnemonic(d.op));
                out << "// SYSCALL stub\nreturn;\n";
                break;
            case Op::Break:
                markUnsupported(pc, mnemonic(d.op));
                out << "// BREAK stub\nreturn;\n";
                break;
            case Op::Sync:    out << "// SYNC\n"; break;
            case Op::Cache:   out << "// CACHE\n"; break;
            case Op::Ll:
            case Op::Lld:
            case Op::Sc:
            case Op::Scd:
                markUnsupported(pc, mnemonic(d.op));
                out << "// LL/SC stub\n";
                break;

            case Op::Invalid:
            default: {
                const std::string name(mnemonic(d.op));
                markUnsupported(pc, name.c_str());
                out << "// unimplemented op " << name << " at 0x" << hex(pc) << "\n";
                break;
            }
        }
        if (d.dstGpr() == 0) out << "ctx.r[0] = 0;\n";
    }

    void emitFpStub(std::ostringstream& out, u32 pc, const DecodedInsn& d) {
        markUnsupported(pc, mnemonic(d.op));
        out << "// FP op stub: " << mnemonic(d.op) << "\n";
    }

    void emitTerminator(std::ostringstream& out, const BasicBlock& bb, u32 pc, const DecodedInsn& d) {
        u32 slotPc = pc + 4;
        DecodedInsn slot;
        const bool hasSlot = (slotPc < bb.end) && img_.read32(slotPc, slot.raw);
        if (hasSlot) slot = decodeR4300(slot.raw);

        // Branch-and-link instructions link before the delay slot executes.
        // Plain jal/jalr are handled by emitCall() and never reach here.
        if (d.has(insn_flag::Link) && bb.term != Terminator::Branch) {
            out << "ctx.r[31] = 0x" << hex(pc + 8) << ";\n";
        }

        switch (bb.term) {
            case Terminator::Return:
                if (hasSlot) emitInsn(out, slotPc, slot);
                out << "return;\n";
                break;
            case Terminator::Jump:
                if (d.op == Op::J) {
                    if (hasSlot) emitInsn(out, slotPc, slot);
                    emitJump(out, d.jumpTarget(pc));
                } else {
                    // Unconditional PC-relative branch.
                    if (hasSlot) emitInsn(out, slotPc, slot);
                    out << "goto " << label(d.branchTarget(pc)) << ";\n";
                }
                break;
            case Terminator::TailCall: {
                if (hasSlot) emitInsn(out, slotPc, slot);
                u32 target = 0;
                if (d.op == Op::J) target = d.jumpTarget(pc);
                else if (bb.succs.size() == 1) target = bb.succs.front().target;
                if (target) {
                    out << "dispatchIndirect(ctx, 0x" << hex(target) << "u);\n";
                }
                out << "return;\n";
                break;
            }
            case Terminator::Branch:
                emitConditionalBranch(out, bb, pc, d, slotPc, slot);
                break;
            case Terminator::JumpTable:
                emitJumpTable(out, pc, slotPc, slot);
                break;
            case Terminator::IndirectJump: {
                // MIPS latches the jump target before the delay slot runs.
                out << "const u32 jrTarget = static_cast<u32>(ctx.r[" << (int)d.rs << "]);\n";
                if (hasSlot) emitInsn(out, slotPc, slot);
                out << "dispatchIndirect(ctx, jrTarget);\nreturn;\n";
                break;
            }
            case Terminator::Eret:
                markUnsupported(pc, mnemonic(d.op));
                if (hasSlot) emitInsn(out, slotPc, slot);
                out << "// ERET\nreturn;\n";
                break;
            case Terminator::Break:
                markUnsupported(pc, mnemonic(d.op));
                if (hasSlot) emitInsn(out, slotPc, slot);
                out << "// BREAK\nreturn;\n";
                break;
            case Terminator::EndOfImage:
            case Terminator::InvalidInsn:
            case Terminator::Fallthrough:
                if (hasSlot) emitInsn(out, slotPc, slot);
                out << "return;\n";
                break;
        }
    }

    void emitConditionalBranch(std::ostringstream& out, const BasicBlock& bb, u32 pc,
                               const DecodedInsn& d, u32 slotPc, const DecodedInsn& slot) {
        const bool likely = bb.likely;
        const u32 taken = bb.succs[0].target;
        const u32 notTaken = (bb.succs.size() > 1) ? bb.succs[1].target : (pc + 8);

        std::string cond;
        switch (d.op) {
            case Op::Beq:  cond = "ctx.r[" + std::to_string(d.rs) + "] == ctx.r[" + std::to_string(d.rt) + "]"; break;
            case Op::Bne:  cond = "ctx.r[" + std::to_string(d.rs) + "] != ctx.r[" + std::to_string(d.rt) + "]"; break;
            case Op::Blez: cond = "static_cast<s64>(ctx.r[" + std::to_string(d.rs) + "]) <= 0"; break;
            case Op::Bgtz: cond = "static_cast<s64>(ctx.r[" + std::to_string(d.rs) + "]) > 0"; break;
            case Op::Beql: cond = "ctx.r[" + std::to_string(d.rs) + "] == ctx.r[" + std::to_string(d.rt) + "]"; break;
            case Op::Bnel: cond = "ctx.r[" + std::to_string(d.rs) + "] != ctx.r[" + std::to_string(d.rt) + "]"; break;
            case Op::Blezl: cond = "static_cast<s64>(ctx.r[" + std::to_string(d.rs) + "]) <= 0"; break;
            case Op::Bgtzl: cond = "static_cast<s64>(ctx.r[" + std::to_string(d.rs) + "]) > 0"; break;
            case Op::Bltz:
            case Op::Bltzal:
            case Op::Bltzl:
            case Op::Bltzall: cond = "static_cast<s64>(ctx.r[" + std::to_string(d.rs) + "]) < 0"; break;
            case Op::Bgez:
            case Op::Bgezal:
            case Op::Bgezl:
            case Op::Bgezall: cond = "static_cast<s64>(ctx.r[" + std::to_string(d.rs) + "]) >= 0"; break;
            case Op::Bc1f: case Op::Bc1fl: cond = "!(ctx.fcr31 & (1u << 23))"; break;
            case Op::Bc1t: case Op::Bc1tl: cond = "(ctx.fcr31 & (1u << 23)) != 0"; break;
            default: cond = "false"; break;
        }

        out << "const bool branchTaken = " << cond << ";\n";
        if (d.has(insn_flag::Link)) out << "ctx.r[31] = 0x" << hex(pc + 8) << ";\n";
        if (likely) {
            out << "if (branchTaken) {\n";
            emitInsn(out, slotPc, slot);
            emitJump(out, taken);
            out << "} else {\n";
            emitJump(out, notTaken);
            out << "}\n";
        } else {
            emitInsn(out, slotPc, slot);
            out << "if (branchTaken) {\n";
            emitJump(out, taken);
            out << "}\n";
            emitJump(out, notTaken);
        }
    }

    void emitJumpTable(std::ostringstream& out, u32 pc, u32 slotPc, const DecodedInsn& slot) {
        bool found = false;
        for (const auto& jt : fn_.jumpTables) {
            if (jt.jrPc != pc || jt.indexReg <= 0) continue;
            found = true;
            u32 jrRaw = 0;
            DecodedInsn jr;
            if (img_.read32(pc, jrRaw)) jr = decodeR4300(jrRaw);
            // Capture the case index and the register target before the delay
            // slot can overwrite either one.
            out << "const u32 jtIndex = static_cast<u32>(ctx.r[" << jt.indexReg << "]);\n";
            out << "const u32 jtTarget = static_cast<u32>(ctx.r[" << (int)jr.rs << "]);\n";
            emitInsn(out, slotPc, slot);
            out << "switch (jtIndex) {\n";
            for (size_t i = 0; i < jt.targets.size(); ++i) {
                out << "case " << i << ": ctx.pc = 0x" << hex(jt.targets[i]) << "u; goto "
                    << label(jt.targets[i]) << ";\n";
            }
            out << "default: dispatchIndirect(ctx, jtTarget);\n}\n";
            break;
        }
        if (!found) {
            markUnsupported(pc, "jump table without index register");
            emitInsn(out, slotPc, slot);
            out << "return;\n";
        }
    }

    void emitJump(std::ostringstream& out, u32 target) {
        auto it = fn_.blocks.find(target);
        if (it != fn_.blocks.end()) {
            out << "goto " << label(target) << ";\n";
        } else {
            // Inter-function jump; treat as tail call with a runtime fallback.
            out << "dispatchIndirect(ctx, 0x" << hex(target) << "u);\nreturn;\n";
        }
    }

    const Function& fn_;
    const CodeImage& img_;
    const EmitOptions& opt_;
    std::vector<std::string> unsupported_;
};

} // namespace

std::string emitFunction(const Function& fn, const CodeImage& image, const EmitOptions& options) {
    Emitter e(fn, image, options);
    return e.emit();
}

namespace {

std::string cmakePath(std::string path) {
    for (char& c : path) {
        if (c == '\\') c = '/';
    }
    return path;
}

} // namespace

EmittedProgram emitProgram(const Program& prog, const CodeImage& image, const EmitOptions& options) {
    EmittedProgram result;
    const std::string ns = options.namespaceName.empty() ? std::string() : options.namespaceName;
    const auto openNs = [&](std::ostringstream& out) {
        if (!ns.empty()) out << "namespace " << ns << " {\n";
        if (options.externC) out << "extern \"C\" {\n";
    };
    const auto closeNs = [&](std::ostringstream& out) {
        if (options.externC) out << "}\n";
        if (!ns.empty()) out << "} // namespace " << ns << '\n';
    };

    std::ostringstream header;
    header << "#include \"n64forge/recomp/recomp_rt.hpp\"\n";
    header << "#include <cstdint>\n";
    header << "using namespace n64forge::recomp;\n";
    header << "using n64forge::u8; using n64forge::s8;\n"
              "using n64forge::u16; using n64forge::s16;\n"
              "using n64forge::u32; using n64forge::s32;\n"
              "using n64forge::u64; using n64forge::s64;\n";
    header << "static inline u64 S32(u64 v) { return static_cast<s64>(static_cast<s32>(static_cast<u32>(v))); }\n"
              "static inline u64 ADD32(u64 a, u64 b) { return S32(static_cast<u32>(a) + static_cast<u32>(b)); }\n"
              "static inline u64 SUB32(u64 a, u64 b) { return S32(static_cast<u32>(a) - static_cast<u32>(b)); }\n"
              "static inline u64 DIV32(s64 a, s64 b) {\n"
              "    if (b == 0) return S32(a);\n"
              "    if (a == (-2147483647LL - 1) && b == -1) return S32(a);\n"
              "    return S32(a / b);\n"
              "}\n"
              "static inline u64 REM32(s64 a, s64 b) {\n"
              "    if (b == 0 || (a == (-2147483647LL - 1) && b == -1)) return 0;\n"
              "    return S32(a % b);\n"
              "}\n"
              "static inline u64 DIV64(s64 a, s64 b) {\n"
              "    if (b == 0) return static_cast<u64>(a);\n"
              "    if (a == (-9223372036854775807LL - 1) && b == -1) return static_cast<u64>(a);\n"
              "    return static_cast<u64>(a / b);\n"
              "}\n"
              "static inline u64 REM64(s64 a, s64 b) {\n"
              "    if (b == 0 || (a == (-9223372036854775807LL - 1) && b == -1)) return 0;\n"
              "    return static_cast<u64>(a % b);\n"
              "}\n";
    if (options.externC) header << "extern \"C\" ";
    header << "void n64forge_recomp_register_dispatch(RecompContext&);\n";
    openNs(header);
    for (const auto& [addr, fn] : prog.functions) {
        (void)fn;
        header << "void " << funcName(addr, options.functionPrefix) << "(RecompContext&);\n";
    }
    closeNs(header);
    header << '\n';
    result.dispatchHeader = header.str();

    for (const auto& [addr, fn] : prog.functions) {
        Emitter emitter(fn, image, options);
        const std::string body = emitter.emit();
        for (const auto& error : emitter.unsupported()) {
            result.errors.push_back("0x" + hex(addr) + ": " + error);
        }
        std::ostringstream functionBody;
        functionBody << result.dispatchHeader;
        if (!ns.empty()) functionBody << "namespace " << ns << " {\n";
        functionBody << body;
        if (!ns.empty()) functionBody << "} // namespace " << ns << '\n';
        result.functionFiles.emplace_back("func_" + hex(addr) + ".cpp", functionBody.str());
    }

    std::ostringstream table;
    table << result.dispatchHeader;
    if (options.externC) table << "extern \"C\" {\n";
    table << "void n64forge_recomp_register_dispatch(RecompContext& ctx) {\n";
    for (const auto& [addr, fn] : prog.functions) {
        (void)fn;
        const std::string name = ns.empty() ? funcName(addr, options.functionPrefix)
                                            : ns + "::" + funcName(addr, options.functionPrefix);
        table << "    ctx.dispatch[0x" << hex(addr) << "] = " << name << ";\n";
    }
    table << "}\n";
    if (options.externC) table << "}\n";
    result.dispatchTable = table.str();

    std::ostringstream main;
    main << "#include \"n64forge/core/cartridge.hpp\"\n"
            "#include \"n64forge/recomp/recomp_rt.hpp\"\n"
            "#include <algorithm>\n"
            "#include <cstdio>\n"
            "#include <cstring>\n"
            "\n"
         << (options.externC ? "extern \"C\" " : "")
         << "void n64forge_recomp_register_dispatch(n64forge::recomp::RecompContext&);\n"
            "\n"
            "int main(int argc, char** argv) {\n"
            "    const bool report = argc == 3 && std::strcmp(argv[2], \"--print-result\") == 0;\n"
            "    if (argc != 2 && !report) {\n"
            "        std::fprintf(stderr, \"Usage: n64forge_native <rom> [--print-result]\\n\");\n"
            "        return 2;\n"
            "    }\n"
            "    n64forge::Cartridge cart;\n"
            "    if (!cart.load(argv[1])) {\n"
            "        std::fprintf(stderr, \"Failed to load ROM: %s\\n\", argv[1]);\n"
            "        return 1;\n"
            "    }\n"
            "    n64forge::recomp::RecompContext ctx;\n"
            "    ctx.setRdramSize(n64forge::recomp::RecompContext::kMaxRdram);\n"
            "    const auto& rom = cart.rom();\n"
            "    constexpr std::size_t bootOffset = 0x1000;\n"
            "    const auto entry = cart.header().bootPc;\n"
            "    const auto physical = entry & 0x1fffffffu;\n"
            "    if (entry < 0x80000000u || entry >= 0xc0000000u || (entry & 3) ||\n"
            "        physical >= ctx.rdramSize() || rom.size() <= bootOffset) {\n"
            "        std::fprintf(stderr, \"Boot payload is not mapped in RDRAM\\n\");\n"
            "        return 1;\n"
            "    }\n"
            "    const auto bytes = std::min({rom.size() - bootOffset, ctx.rdramSize() - physical,\n"
            "                                 std::size_t{0x100000}});\n"
            "    std::memcpy(ctx.rdram + physical, rom.data() + bootOffset, bytes);\n"
            "    n64forge_recomp_register_dispatch(ctx);\n"
            "    ctx.pc = physical | 0x80000000u;\n"
            "    const auto it = ctx.dispatch.find(ctx.pc);\n"
            "    if (it == ctx.dispatch.end()) {\n"
            "        std::fprintf(stderr, \"No recompiled function for boot PC 0x%08X\\n\", ctx.pc);\n"
            "        return 1;\n"
            "    }\n"
            "    it->second(ctx);\n"
            "    if (report) std::printf(\"v0=0x%llX\\n\", static_cast<unsigned long long>(ctx.r[2]));\n"
            "    return 0;\n"
            "}\n";
    result.mainSource = main.str();

    std::ostringstream cmake;
    cmake << "cmake_minimum_required(VERSION 3.24)\n"
             "project(n64forge_recomp_out LANGUAGES CXX)\n"
             "set(CMAKE_CXX_STANDARD 20)\n"
             "set(CMAKE_CXX_STANDARD_REQUIRED ON)\n"
             "set(CMAKE_CXX_EXTENSIONS OFF)\n"
             "set(N64FORGE_ROOT \"" << cmakePath(options.sourceRoot) << "\" CACHE PATH \"n64forge source root\")\n"
             "if(NOT N64FORGE_ROOT OR NOT EXISTS \"${N64FORGE_ROOT}/CMakeLists.txt\")\n"
             "    message(FATAL_ERROR \"Pass -DN64FORGE_ROOT=<n64forge-studio source root>\")\n"
             "endif()\n"
             "set(N64FORGE_BUILD_TESTS OFF CACHE BOOL \"\" FORCE)\n"
             "set(N64FORGE_BUILD_STUDIO OFF CACHE BOOL \"\" FORCE)\n"
             "set(N64FORGE_ENABLE_D3D12 OFF CACHE BOOL \"\" FORCE)\n"
             "set(N64FORGE_ENABLE_METAL OFF CACHE BOOL \"\" FORCE)\n"
             "set(N64FORGE_ENABLE_HARDWARE_TESTS OFF CACHE BOOL \"\" FORCE)\n"
             "add_subdirectory(\"${N64FORGE_ROOT}\" n64forge_runtime EXCLUDE_FROM_ALL)\n"
             "file(GLOB GENERATED_SOURCES \"${CMAKE_CURRENT_SOURCE_DIR}/*.cpp\")\n"
             "add_executable(n64forge_native ${GENERATED_SOURCES})\n"
             "target_link_libraries(n64forge_native PRIVATE n64forge_recomp)\n";
    result.cmakeLists = cmake.str();

    return result;
}

} // namespace n64forge::recomp
