#include "n64forge/core/r4300_decode.hpp"

#include <array>
#include <cstdio>

namespace n64forge {
namespace {

struct OpInfo {
    std::string_view mnemonic;
    u32 flags;
};

constexpr std::array<OpInfo, static_cast<size_t>(Op::Count_)> kOpInfo = {{
    {"invalid", insn_flag::None},
#define N64FORGE_X(name, mnem, flags) {mnem, flags},
    N64FORGE_R4300_OPS(N64FORGE_X)
#undef N64FORGE_X
}};

constexpr std::array<std::string_view, 32> kGpr = {
    "zero", "at", "v0", "v1", "a0", "a1", "a2", "a3", "t0", "t1", "t2",
    "t3",   "t4", "t5", "t6", "t7", "s0", "s1", "s2", "s3", "s4", "s5",
    "s6",   "s7", "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra"};

constexpr std::array<std::string_view, 16> kFpCond = {
    "f", "un", "eq", "ueq", "olt", "ult", "ole", "ule",
    "sf", "ngle", "seq", "ngl", "lt", "nge", "le", "ngt"};

Op decodeSpecial(u32 funct) noexcept {
    switch (funct) {
        case 0x00: return Op::Sll;
        case 0x02: return Op::Srl;
        case 0x03: return Op::Sra;
        case 0x04: return Op::Sllv;
        case 0x06: return Op::Srlv;
        case 0x07: return Op::Srav;
        case 0x08: return Op::Jr;
        case 0x09: return Op::Jalr;
        case 0x0C: return Op::Syscall;
        case 0x0D: return Op::Break;
        case 0x0F: return Op::Sync;
        case 0x10: return Op::Mfhi;
        case 0x11: return Op::Mthi;
        case 0x12: return Op::Mflo;
        case 0x13: return Op::Mtlo;
        case 0x14: return Op::Dsllv;
        case 0x16: return Op::Dsrlv;
        case 0x17: return Op::Dsrav;
        case 0x18: return Op::Mult;
        case 0x19: return Op::Multu;
        case 0x1A: return Op::Div;
        case 0x1B: return Op::Divu;
        case 0x1C: return Op::Dmult;
        case 0x1D: return Op::Dmultu;
        case 0x1E: return Op::Ddiv;
        case 0x1F: return Op::Ddivu;
        case 0x20: return Op::Add;
        case 0x21: return Op::Addu;
        case 0x22: return Op::Sub;
        case 0x23: return Op::Subu;
        case 0x24: return Op::And;
        case 0x25: return Op::Or;
        case 0x26: return Op::Xor;
        case 0x27: return Op::Nor;
        case 0x2A: return Op::Slt;
        case 0x2B: return Op::Sltu;
        case 0x2C: return Op::Dadd;
        case 0x2D: return Op::Daddu;
        case 0x2E: return Op::Dsub;
        case 0x2F: return Op::Dsubu;
        case 0x30: return Op::Tge;
        case 0x31: return Op::Tgeu;
        case 0x32: return Op::Tlt;
        case 0x33: return Op::Tltu;
        case 0x34: return Op::Teq;
        case 0x36: return Op::Tne;
        case 0x38: return Op::Dsll;
        case 0x3A: return Op::Dsrl;
        case 0x3B: return Op::Dsra;
        case 0x3C: return Op::Dsll32;
        case 0x3E: return Op::Dsrl32;
        case 0x3F: return Op::Dsra32;
        default: return Op::Invalid;
    }
}

Op decodeRegimm(u32 rt) noexcept {
    switch (rt) {
        case 0x00: return Op::Bltz;
        case 0x01: return Op::Bgez;
        case 0x02: return Op::Bltzl;
        case 0x03: return Op::Bgezl;
        case 0x08: return Op::Tgei;
        case 0x09: return Op::Tgeiu;
        case 0x0A: return Op::Tlti;
        case 0x0B: return Op::Tltiu;
        case 0x0C: return Op::Teqi;
        case 0x0E: return Op::Tnei;
        case 0x10: return Op::Bltzal;
        case 0x11: return Op::Bgezal;
        case 0x12: return Op::Bltzall;
        case 0x13: return Op::Bgezall;
        default: return Op::Invalid;
    }
}

Op decodeCop0(u32 raw) noexcept {
    const u32 rs = (raw >> 21) & 0x1F;
    if (raw & (1u << 25)) {  // CO bit: TLB / ERET group
        switch (raw & 0x3F) {
            case 0x01: return Op::Tlbr;
            case 0x02: return Op::Tlbwi;
            case 0x06: return Op::Tlbwr;
            case 0x08: return Op::Tlbp;
            case 0x18: return Op::Eret;
            default: return Op::Invalid;
        }
    }
    switch (rs) {
        case 0x00: return Op::Mfc0;
        case 0x01: return Op::Dmfc0;
        case 0x04: return Op::Mtc0;
        case 0x05: return Op::Dmtc0;
        default: return Op::Invalid;
    }
}

Op decodeFpArith(u32 funct, FpFmt fmt) noexcept {
    const bool sd = fmt == FpFmt::S || fmt == FpFmt::D;
    if (funct >= 0x30) return sd ? Op::FCmp : Op::Invalid;
    switch (funct) {
        case 0x00: return sd ? Op::FAdd : Op::Invalid;
        case 0x01: return sd ? Op::FSub : Op::Invalid;
        case 0x02: return sd ? Op::FMul : Op::Invalid;
        case 0x03: return sd ? Op::FDiv : Op::Invalid;
        case 0x04: return sd ? Op::FSqrt : Op::Invalid;
        case 0x05: return sd ? Op::FAbs : Op::Invalid;
        case 0x06: return sd ? Op::FMov : Op::Invalid;
        case 0x07: return sd ? Op::FNeg : Op::Invalid;
        case 0x08: return sd ? Op::FRoundL : Op::Invalid;
        case 0x09: return sd ? Op::FTruncL : Op::Invalid;
        case 0x0A: return sd ? Op::FCeilL : Op::Invalid;
        case 0x0B: return sd ? Op::FFloorL : Op::Invalid;
        case 0x0C: return sd ? Op::FRoundW : Op::Invalid;
        case 0x0D: return sd ? Op::FTruncW : Op::Invalid;
        case 0x0E: return sd ? Op::FCeilW : Op::Invalid;
        case 0x0F: return sd ? Op::FFloorW : Op::Invalid;
        case 0x20: return fmt == FpFmt::S ? Op::Invalid : Op::FCvtS;  // D, W, L -> S
        case 0x21: return fmt == FpFmt::D ? Op::Invalid : Op::FCvtD;  // S, W, L -> D
        case 0x24: return sd ? Op::FCvtW : Op::Invalid;
        case 0x25: return sd ? Op::FCvtL : Op::Invalid;
        default: return Op::Invalid;
    }
}

Op decodeCop1(u32 raw, FpFmt& fmtOut) noexcept {
    const u32 rs = (raw >> 21) & 0x1F;
    const u32 rt = (raw >> 16) & 0x1F;
    switch (rs) {
        case 0x00: return Op::Mfc1;
        case 0x01: return Op::Dmfc1;
        case 0x02: return Op::Cfc1;
        case 0x04: return Op::Mtc1;
        case 0x05: return Op::Dmtc1;
        case 0x06: return Op::Ctc1;
        case 0x08:
            switch (rt & 3) {
                case 0: return Op::Bc1f;
                case 1: return Op::Bc1t;
                case 2: return Op::Bc1fl;
                case 3: return Op::Bc1tl;
                default: return Op::Invalid;
            }
        case 16: case 17: case 20: case 21:
            fmtOut = static_cast<FpFmt>(rs);
            return decodeFpArith(raw & 0x3F, fmtOut);
        default:
            return Op::Invalid;
    }
}

char fmtChar(FpFmt f) noexcept {
    switch (f) {
        case FpFmt::S: return 's';
        case FpFmt::D: return 'd';
        case FpFmt::W: return 'w';
        case FpFmt::L: return 'l';
        default: return '?';
    }
}

}  // namespace

DecodedInsn decodeR4300(u32 raw) noexcept {
    DecodedInsn d;
    d.raw = raw;
    d.rs = static_cast<u8>((raw >> 21) & 0x1F);
    d.rt = static_cast<u8>((raw >> 16) & 0x1F);
    d.rd = static_cast<u8>((raw >> 11) & 0x1F);
    d.sa = static_cast<u8>((raw >> 6) & 0x1F);
    d.imm = static_cast<s32>(static_cast<s16>(raw & 0xFFFF));
    d.target = raw & 0x03FFFFFFu;

    const u32 opcode = raw >> 26;
    switch (opcode) {
        case 0x00: d.op = decodeSpecial(raw & 0x3F); break;
        case 0x01: d.op = decodeRegimm(d.rt); break;
        case 0x02: d.op = Op::J; break;
        case 0x03: d.op = Op::Jal; break;
        case 0x04: d.op = Op::Beq; break;
        case 0x05: d.op = Op::Bne; break;
        case 0x06: d.op = Op::Blez; break;
        case 0x07: d.op = Op::Bgtz; break;
        case 0x08: d.op = Op::Addi; break;
        case 0x09: d.op = Op::Addiu; break;
        case 0x0A: d.op = Op::Slti; break;
        case 0x0B: d.op = Op::Sltiu; break;
        case 0x0C: d.op = Op::Andi; break;
        case 0x0D: d.op = Op::Ori; break;
        case 0x0E: d.op = Op::Xori; break;
        case 0x0F: d.op = Op::Lui; break;
        case 0x10: d.op = decodeCop0(raw); break;
        case 0x11: d.op = decodeCop1(raw, d.fmt); break;
        case 0x14: d.op = Op::Beql; break;
        case 0x15: d.op = Op::Bnel; break;
        case 0x16: d.op = Op::Blezl; break;
        case 0x17: d.op = Op::Bgtzl; break;
        case 0x18: d.op = Op::Daddi; break;
        case 0x19: d.op = Op::Daddiu; break;
        case 0x1A: d.op = Op::Ldl; break;
        case 0x1B: d.op = Op::Ldr; break;
        case 0x20: d.op = Op::Lb; break;
        case 0x21: d.op = Op::Lh; break;
        case 0x22: d.op = Op::Lwl; break;
        case 0x23: d.op = Op::Lw; break;
        case 0x24: d.op = Op::Lbu; break;
        case 0x25: d.op = Op::Lhu; break;
        case 0x26: d.op = Op::Lwr; break;
        case 0x27: d.op = Op::Lwu; break;
        case 0x28: d.op = Op::Sb; break;
        case 0x29: d.op = Op::Sh; break;
        case 0x2A: d.op = Op::Swl; break;
        case 0x2B: d.op = Op::Sw; break;
        case 0x2C: d.op = Op::Sdl; break;
        case 0x2D: d.op = Op::Sdr; break;
        case 0x2E: d.op = Op::Swr; break;
        case 0x2F: d.op = Op::Cache; break;
        case 0x30: d.op = Op::Ll; break;
        case 0x31: d.op = Op::Lwc1; break;
        case 0x34: d.op = Op::Lld; break;
        case 0x35: d.op = Op::Ldc1; break;
        case 0x37: d.op = Op::Ld; break;
        case 0x38: d.op = Op::Sc; break;
        case 0x39: d.op = Op::Swc1; break;
        case 0x3C: d.op = Op::Scd; break;
        case 0x3D: d.op = Op::Sdc1; break;
        case 0x3F: d.op = Op::Sd; break;
        default: d.op = Op::Invalid; break;  // incl. COP2/COP3 (absent on VR4300)
    }
    d.flags = kOpInfo[static_cast<size_t>(d.op)].flags;
    if (d.op == Op::FCmp) d.cond = static_cast<u8>(raw & 0xF);
    return d;
}

bool DecodedInsn::isCall() const noexcept {
    return has(insn_flag::Link) && has(insn_flag::HasDelay);
}

bool DecodedInsn::isReturn() const noexcept { return op == Op::Jr && rs == 31; }

bool DecodedInsn::isUnconditionalBranch() const noexcept {
    switch (op) {
        case Op::Beq: case Op::Beql: return rs == rt;
        case Op::Bgez: case Op::Bgezl: case Op::Bgezal: case Op::Bgezall: return rs == 0;
        default: return false;
    }
}

int DecodedInsn::dstGpr() const noexcept {
    if (has(insn_flag::DstRd)) return rd;
    if (has(insn_flag::DstRt)) return rt;
    if (has(insn_flag::DstRa)) return 31;
    return -1;
}

std::string_view mnemonic(Op op) noexcept {
    const auto i = static_cast<size_t>(op);
    return i < kOpInfo.size() ? kOpInfo[i].mnemonic : "invalid";
}

std::string_view gprName(unsigned index) noexcept { return kGpr[index & 31u]; }

std::string disassemble(const DecodedInsn& d, u32 pc) {
    char buf[96];
    const auto r = [](unsigned i) { return std::string(gprName(i)); };
    const std::string m(mnemonic(d.op));
    auto fmt = [&](const char* f, auto... a) {
        std::snprintf(buf, sizeof buf, f, a...);
        return std::string(buf);
    };
    if (d.raw == 0) return "nop";
    using namespace insn_flag;
    switch (d.op) {
        case Op::Invalid: return fmt(".word 0x%08X", d.raw);
        case Op::Sll: case Op::Srl: case Op::Sra: case Op::Dsll: case Op::Dsrl:
        case Op::Dsra: case Op::Dsll32: case Op::Dsrl32: case Op::Dsra32:
            return fmt("%s $%s, $%s, %u", m.c_str(), r(d.rd).c_str(), r(d.rt).c_str(), d.sa);
        case Op::Sllv: case Op::Srlv: case Op::Srav: case Op::Dsllv: case Op::Dsrlv:
        case Op::Dsrav:
            return fmt("%s $%s, $%s, $%s", m.c_str(), r(d.rd).c_str(), r(d.rt).c_str(),
                       r(d.rs).c_str());
        case Op::Jr: case Op::Mthi: case Op::Mtlo:
            return fmt("%s $%s", m.c_str(), r(d.rs).c_str());
        case Op::Jalr:
            return fmt("jalr $%s, $%s", r(d.rd).c_str(), r(d.rs).c_str());
        case Op::Mfhi: case Op::Mflo:
            return fmt("%s $%s", m.c_str(), r(d.rd).c_str());
        case Op::Mult: case Op::Multu: case Op::Div: case Op::Divu: case Op::Dmult:
        case Op::Dmultu: case Op::Ddiv: case Op::Ddivu: case Op::Tge: case Op::Tgeu:
        case Op::Tlt: case Op::Tltu: case Op::Teq: case Op::Tne:
            return fmt("%s $%s, $%s", m.c_str(), r(d.rs).c_str(), r(d.rt).c_str());
        case Op::Add: case Op::Addu: case Op::Sub: case Op::Subu: case Op::And: case Op::Or:
        case Op::Xor: case Op::Nor: case Op::Slt: case Op::Sltu: case Op::Dadd:
        case Op::Daddu: case Op::Dsub: case Op::Dsubu:
            return fmt("%s $%s, $%s, $%s", m.c_str(), r(d.rd).c_str(), r(d.rs).c_str(),
                       r(d.rt).c_str());
        case Op::J: case Op::Jal:
            return fmt("%s 0x%08X", m.c_str(), d.jumpTarget(pc));
        case Op::Beq: case Op::Bne: case Op::Beql: case Op::Bnel:
            return fmt("%s $%s, $%s, 0x%08X", m.c_str(), r(d.rs).c_str(), r(d.rt).c_str(),
                       d.branchTarget(pc));
        case Op::Blez: case Op::Bgtz: case Op::Blezl: case Op::Bgtzl: case Op::Bltz:
        case Op::Bgez: case Op::Bltzl: case Op::Bgezl: case Op::Bltzal: case Op::Bgezal:
        case Op::Bltzall: case Op::Bgezall:
            return fmt("%s $%s, 0x%08X", m.c_str(), r(d.rs).c_str(), d.branchTarget(pc));
        case Op::Bc1f: case Op::Bc1t: case Op::Bc1fl: case Op::Bc1tl:
            return fmt("%s 0x%08X", m.c_str(), d.branchTarget(pc));
        case Op::Lui:
            return fmt("lui $%s, 0x%04X", r(d.rt).c_str(), static_cast<u32>(d.imm) & 0xFFFFu);
        case Op::Andi: case Op::Ori: case Op::Xori:
            return fmt("%s $%s, $%s, 0x%04X", m.c_str(), r(d.rt).c_str(), r(d.rs).c_str(),
                       static_cast<u32>(d.imm) & 0xFFFFu);
        case Op::Mfc0: case Op::Dmfc0: case Op::Mtc0: case Op::Dmtc0:
            return fmt("%s $%s, $%u", m.c_str(), r(d.rt).c_str(), d.rd);
        case Op::Mfc1: case Op::Dmfc1: case Op::Mtc1: case Op::Dmtc1: case Op::Cfc1:
        case Op::Ctc1:
            return fmt("%s $%s, $f%u", m.c_str(), r(d.rt).c_str(), d.rd);
        case Op::Syscall: case Op::Break: case Op::Sync: case Op::Tlbr: case Op::Tlbwi:
        case Op::Tlbwr: case Op::Tlbp: case Op::Eret:
            return m;
        case Op::Cache:
            return fmt("cache 0x%02X, %d($%s)", d.rt, d.imm, r(d.rs).c_str());
        case Op::FCmp:
            return fmt("c.%s.%c $f%u, $f%u", std::string(kFpCond[d.cond]).c_str(), fmtChar(d.fmt),
                       d.rd, d.rt);
        case Op::FAdd: case Op::FSub: case Op::FMul: case Op::FDiv:
            return fmt("%s.%c $f%u, $f%u, $f%u", m.c_str(), fmtChar(d.fmt), d.sa, d.rd, d.rt);
        case Op::FSqrt: case Op::FAbs: case Op::FMov: case Op::FNeg: case Op::FRoundL:
        case Op::FTruncL: case Op::FCeilL: case Op::FFloorL: case Op::FRoundW:
        case Op::FTruncW: case Op::FCeilW: case Op::FFloorW: case Op::FCvtS: case Op::FCvtD:
        case Op::FCvtW: case Op::FCvtL:
            return fmt("%s.%c $f%u, $f%u", m.c_str(), fmtChar(d.fmt), d.sa, d.rd);
        case Op::Lwc1: case Op::Ldc1: case Op::Swc1: case Op::Sdc1:
            return fmt("%s $f%u, %d($%s)", m.c_str(), d.rt, d.imm, r(d.rs).c_str());
        case Op::Tgei: case Op::Tgeiu: case Op::Tlti: case Op::Tltiu: case Op::Teqi:
        case Op::Tnei:
            return fmt("%s $%s, %d", m.c_str(), r(d.rs).c_str(), d.imm);
        default:
            break;
    }
    if (d.has(Load) || d.has(Store)) {
        return fmt("%s $%s, %d($%s)", m.c_str(), r(d.rt).c_str(), d.imm, r(d.rs).c_str());
    }
    // Remaining I-type ALU (addi, addiu, slti, sltiu, daddi, daddiu).
    return fmt("%s $%s, $%s, %d", m.c_str(), r(d.rt).c_str(), r(d.rs).c_str(), d.imm);
}

}  // namespace n64forge
