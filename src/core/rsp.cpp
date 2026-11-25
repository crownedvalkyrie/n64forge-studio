#include "n64forge/core/rsp.hpp"
#include <stdexcept>

namespace n64forge {

namespace {

constexpr u32 MI_INTR_SP = 1;

[[nodiscard]] u32 readU32Be(const u8* p) {
    return (static_cast<u32>(p[0]) << 24) |
           (static_cast<u32>(p[1]) << 16) |
           (static_cast<u32>(p[2]) << 8)  |
           static_cast<u32>(p[3]);
}

} // namespace

Rsp::Rsp() { reset(); }

void Rsp::reset() {
    regs_.fill(0);
    pc_ = 0;
    nextPc_ = 4;
    dmem_.fill(0);
    imem_.fill(0);
    for (auto& instr : decoded_) instr.valid = false;
    for (auto& v : vregs_) v.fill(0);
    accum_.fill(0);
    vectorFlags_.fill(0);
    divIn_ = divOut_ = 0;
    divHigh_ = false;
    taskTrace_ = {};
    taskLaunches_ = 0;
    taskType_ = 0;
}

void Rsp::vload(u32 vrt, u32 element, u32 dmemOffset) {
    const u32 base = dmemOffset & 0xFFF;
    Vreg v{};
    for (u32 i = 0; i < Vlanes; ++i) {
        const u32 lane = (element + i) & 0x7;
        const u32 addr = (base + i * 2) & 0xFFF;
        v[lane] = static_cast<u16>((dmem_[addr] << 8) | dmem_[(addr + 1) & 0xFFF]);
    }
    setVreg(vrt, v);
}

void Rsp::vstore(u32 vrt, u32 element, u32 dmemOffset) const {
    const u32 base = dmemOffset & 0xFFF;
    const Vreg& v = vreg(vrt);
    for (u32 i = 0; i < Vlanes; ++i) {
        const u32 lane = (element + i) & 0x7;
        const u32 addr = (base + i * 2) & 0xFFF;
        const_cast<std::array<u8, DmemSize>&>(dmem_)[addr] = static_cast<u8>(v[lane] >> 8);
        const_cast<std::array<u8, DmemSize>&>(dmem_)[(addr + 1) & 0xFFF] = static_cast<u8>(v[lane]);
    }
}

u8 Rsp::readMem8(u32 a) const {
    if (a < DmemSize) return dmem_[a];
    if (a < DmemSize + ImemSize) return imem_[a - DmemSize];
    throw std::out_of_range("Rsp read8 out of range");
}

void Rsp::writeMem8(u32 a, u8 v) {
    if (a < DmemSize) {
        dmem_[a] = v;
        return;
    }
    if (a < DmemSize + ImemSize) {
        imem_[a - DmemSize] = v;
        decoded_[(a - DmemSize) / 4].valid = false;
        return;
    }
    throw std::out_of_range("Rsp write8 out of range");
}

u32 Rsp::readMem32(u32 a) const {
    return (static_cast<u32>(readMem8(a)) << 24) |
           (static_cast<u32>(readMem8(a + 1)) << 16) |
           (static_cast<u32>(readMem8(a + 2)) << 8) |
           static_cast<u32>(readMem8(a + 3));
}

void Rsp::writeMem32(u32 a, u32 v) {
    writeMem8(a, static_cast<u8>(v >> 24));
    writeMem8(a + 1, static_cast<u8>(v >> 16));
    writeMem8(a + 2, static_cast<u8>(v >> 8));
    writeMem8(a + 3, static_cast<u8>(v));
}

void Rsp::dmaRead(u32 dramAddr, u32 spAddr, u32 length) {
    if (bus_ == nullptr) return;
    for (u32 i = 0; i < length; ++i) {
        const u8 v = bus_->read8(dramAddr + i);
        writeMem8((spAddr + i) & 0x1FFF, v);
    }
}

void Rsp::dmaWrite(u32 dramAddr, u32 spAddr, u32 length) {
    if (bus_ == nullptr) return;
    for (u32 i = 0; i < length; ++i) {
        const u8 v = readMem8((spAddr + i) & 0x1FFF);
        bus_->write8(dramAddr + i, v);
    }
}

const Rsp::DecodedInstr& Rsp::fetch() noexcept {
    auto& cached = decoded_[pc_ / 4];
    if (!cached.valid) {
        const u32 raw = readU32Be(&imem_[pc_]);
        cached = {
            raw & 0x03FFFFFF, static_cast<s16>(raw),
            static_cast<u8>(raw >> 26), static_cast<u8>((raw >> 21) & 31),
            static_cast<u8>((raw >> 16) & 31), static_cast<u8>((raw >> 11) & 31),
            static_cast<u8>((raw >> 6) & 31), static_cast<u8>(raw & 63),
            static_cast<u8>((raw >> 7) & 15), static_cast<u8>((raw >> 21) & 15),
            static_cast<s16>(static_cast<s32>(raw & 63) - static_cast<s32>(raw & 64)),
            true,
        };
    }
    return cached;
}

void Rsp::writeGpr(u32 index, u32 value) noexcept {
    if (index != 0) regs_[index] = value;
}

void Rsp::setPc(u32 pc) noexcept {
    pc_ = pc & 0xFFC;
    nextPc_ = (pc_ + 4) & 0xFFC;
}

void Rsp::observeLaunch() noexcept {
    ++taskLaunches_;
    taskType_ = readU32Be(&dmem_[0xFC0]);
    if (taskType_ < taskTrace_.size()) ++taskTrace_[taskType_].launches;
}

void Rsp::execute(const DecodedInstr& instr) {
    const u32 rs = instr.rs, rt = instr.rt, rd = instr.rd;
    const s32 imm = instr.imm;

    const u32 instructionPc = pc_;
    pc_ = nextPc_;
    nextPc_ = (pc_ + 4) & 0xFFC;
    const auto jump = [this](u32 address) { nextPc_ = address & 0xFFC; };
    const auto takeBranch = [&](s32 offset) {
        jump(instructionPc + 4 + (static_cast<u32>(offset) << 2));
    };
    const u32 link = (instructionPc + 8) & 0xFFF;
    const u32 addr = regs_[rs] + static_cast<u32>(imm);
    const auto load = [&](u32 bytes) {
        u32 value = 0;
        for (u32 i = 0; i < bytes; ++i) value = (value << 8) | dmem_[(addr + i) & 0xFFF];
        return value;
    };
    const auto store = [&](u32 bytes) {
        for (u32 i = 0; i < bytes; ++i)
            dmem_[(addr + i) & 0xFFF] = static_cast<u8>(regs_[rt] >> (8 * (bytes - i - 1)));
    };

    switch (instr.op) {
        case 0x00: { // SPECIAL
            switch (instr.funct) {
                case 0x00: writeGpr(rd, regs_[rt] << instr.shamt); break; // SLL
                case 0x02: writeGpr(rd, regs_[rt] >> instr.shamt); break; // SRL
                case 0x03: writeGpr(rd, static_cast<s32>(regs_[rt]) >> instr.shamt); break; // SRA
                case 0x04: writeGpr(rd, regs_[rt] << (regs_[rs] & 0x1F)); break; // SLLV
                case 0x06: writeGpr(rd, regs_[rt] >> (regs_[rs] & 0x1F)); break; // SRLV
                case 0x07: writeGpr(rd, static_cast<s32>(regs_[rt]) >> (regs_[rs] & 0x1F)); break; // SRAV
                case 0x08: jump(regs_[rs]); break; // JR
                case 0x09: { // JALR
                    jump(regs_[rs]);
                    writeGpr(rd, link);
                    break;
                }
                case 0x0D: { // BREAK does not set any of the software signal bits.
                    if (!bus_) break;
                    auto& hw = bus_->hw();
                    auto& status = hw.sp().status;
                    status |= 3;
                    if (status & 0x40) hw.mi().setPending(MI_INTR_SP);
                    if (taskType_ < taskTrace_.size()) {
                        auto& trace = taskTrace_[taskType_];
                        ++trace.breaks;
                        if (status & 0x200) ++trace.doneAtBreak;
                    }
                    break;
                }
                case 0x20: // ADD
                case 0x21: // ADDU
                    writeGpr(rd, regs_[rs] + regs_[rt]);
                    break;
                case 0x22: // SUB
                case 0x23: // SUBU
                    writeGpr(rd, regs_[rs] - regs_[rt]);
                    break;
                case 0x24: writeGpr(rd, regs_[rs] & regs_[rt]); break; // AND
                case 0x25: writeGpr(rd, regs_[rs] | regs_[rt]); break; // OR
                case 0x26: writeGpr(rd, regs_[rs] ^ regs_[rt]); break; // XOR
                case 0x27: writeGpr(rd, ~(regs_[rs] | regs_[rt])); break; // NOR
                case 0x2A: writeGpr(rd, static_cast<s32>(regs_[rs]) < static_cast<s32>(regs_[rt]) ? 1 : 0); break; // SLT
                case 0x2B: writeGpr(rd, regs_[rs] < regs_[rt] ? 1 : 0); break; // SLTU
                default: break; // ignore unknown SPECIAL
            }
            break;
        }
        case 0x01: { // REGIMM
            const u32 sub = rt;
            const s32 v = static_cast<s32>(regs_[rs]);
            bool take = false;
            switch (sub) {
                case 0x00: take = v < 0; break;
                case 0x01: take = v >= 0; break;
                case 0x10: writeGpr(31, link); take = v < 0; break;
                case 0x11: writeGpr(31, link); take = v >= 0; break;
                default: break;
            }
            if (take) takeBranch(imm);
            break;
        }
        case 0x02: jump(instr.target << 2); break; // J
        case 0x03: { // JAL
            writeGpr(31, link);
            jump(instr.target << 2);
            break;
        }
        case 0x04: if (regs_[rs] == regs_[rt]) takeBranch(imm); break; // BEQ
        case 0x05: if (regs_[rs] != regs_[rt]) takeBranch(imm); break; // BNE
        case 0x06: if (static_cast<s32>(regs_[rs]) <= 0) takeBranch(imm); break; // BLEZ
        case 0x07: if (static_cast<s32>(regs_[rs]) > 0) takeBranch(imm); break; // BGTZ
        case 0x08: // ADDI
        case 0x09: // ADDIU
            writeGpr(rt, regs_[rs] + static_cast<u32>(imm));
            break;
        case 0x0A: writeGpr(rt, static_cast<s32>(regs_[rs]) < imm ? 1 : 0); break; // SLTI
        case 0x0B: writeGpr(rt, regs_[rs] < static_cast<u32>(imm) ? 1 : 0); break; // SLTIU
        case 0x0C: writeGpr(rt, regs_[rs] & static_cast<u32>(static_cast<u16>(imm))); break; // ANDI
        case 0x0D: writeGpr(rt, regs_[rs] | static_cast<u32>(static_cast<u16>(imm))); break; // ORI
        case 0x0E: writeGpr(rt, regs_[rs] ^ static_cast<u32>(static_cast<u16>(imm))); break; // XORI
        case 0x0F: writeGpr(rt, static_cast<u32>(imm) << 16); break; // LUI
        case 0x10: { // COP0
            if (!bus_) break;
            const u32 index = rd & 15;
            const u32 address = index < 8 ? 0x04040000 + 4 * index : 0x04100000 + 4 * (index - 8);
            switch (rs) {
                case 0x00: writeGpr(rt, bus_->read32(address)); break; // MFC0
                case 0x04: bus_->write32(address, regs_[rt]); break; // MTC0
                default: break;
            }
            break;
        }
        case 0x12: executeCop2(instr); break;
        case 0x20: writeGpr(rt, static_cast<s8>(load(1))); break;
        case 0x21: writeGpr(rt, static_cast<s16>(load(2))); break;
        case 0x23: writeGpr(rt, load(4)); break;
        case 0x24: writeGpr(rt, load(1)); break;
        case 0x25: writeGpr(rt, load(2)); break;
        case 0x27: writeGpr(rt, load(4)); break; // LWU aliases LW
        case 0x28: store(1); break;
        case 0x29: store(2); break;
        case 0x2B: store(4); break;
        case 0x32: vectorMemory(instr, false); break;
        case 0x3A: vectorMemory(instr, true); break;
        default: break;
    }
}

void Rsp::step() {
    if (bus_ && (bus_->hw().sp().status & 1)) return;
    execute(fetch());
    if (bus_ && (bus_->hw().sp().status & 0x20)) bus_->hw().sp().status |= 1;
}

} // namespace n64forge
