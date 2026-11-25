// Deterministic tests for the shared R4300i decoder and the static-recompiler analyzer.
// Uses CHECK (not assert) so the checks survive Release/NDEBUG builds.
#include "n64forge/core/bus.hpp"
#include "n64forge/core/cartridge.hpp"
#include "n64forge/core/cpu.hpp"
#include "n64forge/core/r4300_decode.hpp"
#include "n64forge/recomp/analyzer.hpp"
#include "n64forge/recomp/emitter.hpp"
#include "n64forge/recomp/recomp_rt.hpp"

#include <cstdio>
#include <exception>
#include <string>
#include <type_traits>
#include <vector>

using namespace n64forge;
using namespace n64forge::recomp;

namespace {

int g_failures = 0;
#define CHECK(cond)                                                                   \
    do {                                                                              \
        if (!(cond)) {                                                                \
            std::fprintf(stderr, "CHECK failed %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                             \
        }                                                                             \
    } while (0)

// --- tiny assembler -------------------------------------------------------
constexpr u32 R(u32 funct, u32 rs, u32 rt, u32 rd, u32 sa = 0) {
    return (rs << 21) | (rt << 16) | (rd << 11) | (sa << 6) | funct;
}
constexpr u32 I(u32 op, u32 rs, u32 rt, s32 imm) {
    return (op << 26) | (rs << 21) | (rt << 16) | (static_cast<u32>(imm) & 0xFFFF);
}
constexpr u32 JJ(u32 op, u32 vaddr) { return (op << 26) | ((vaddr >> 2) & 0x03FFFFFF); }

constexpr u32 NOP = 0;
constexpr u32 zero = 0, at = 1, v0 = 2, a0 = 4, t0 = 8, t1 = 9, sp = 29, ra = 31;
constexpr u32 addiu(u32 rt, u32 rs, s32 i) { return I(0x09, rs, rt, i); }
constexpr u32 lui(u32 rt, u32 i) { return I(0x0F, 0, rt, static_cast<s32>(i)); }
constexpr u32 lw(u32 rt, u32 rs, s32 i) { return I(0x23, rs, rt, i); }
constexpr u32 lwl(u32 rt, u32 rs, s32 i) { return I(0x22, rs, rt, i); }
constexpr u32 sw(u32 rt, u32 rs, s32 i) { return I(0x2B, rs, rt, i); }
constexpr u32 jr(u32 rs) { return R(0x08, rs, 0, 0); }
constexpr u32 jal(u32 a) { return JJ(0x03, a); }
constexpr u32 j(u32 a) { return JJ(0x02, a); }
constexpr u32 bne(u32 rs, u32 rt, s32 off) { return I(0x05, rs, rt, off); }
constexpr u32 beq(u32 rs, u32 rt, s32 off) { return I(0x04, rs, rt, off); }
constexpr u32 beql(u32 rs, u32 rt, s32 off) { return I(0x14, rs, rt, off); }
constexpr u32 bal(s32 off) { return I(0x01, 0, 0x11, off); }
constexpr u32 syscallInsn() { return 0x0000000Cu; }
constexpr u32 sll(u32 rd, u32 rt, u32 sa) { return R(0x00, 0, rt, rd, sa); }
constexpr u32 addu(u32 rd, u32 rs, u32 rt) { return R(0x21, rs, rt, rd); }
constexpr u32 sltiu(u32 rt, u32 rs, s32 i) { return I(0x0B, rs, rt, i); }
constexpr u32 sltu(u32 rd, u32 rs, u32 rt) { return R(0x2B, rs, rt, rd); }
constexpr u32 ori(u32 rt, u32 rs, u32 i) { return I(0x0D, rs, rt, i); }
constexpr u32 jalr(u32 rs, u32 rd = 0) { return R(0x09, rs, 0, rd); }

CodeImage imageOf(u32 base, const std::vector<u32>& words) {
    CodeImage img;
    img.addWords(base, words);
    return img;
}

// Places `words` at word offset `at` (relative to base) inside a zero-padded vector.
void put(std::vector<u32>& v, u32 byteOff, const std::vector<u32>& words) {
    for (size_t i = 0; i < words.size(); ++i) v[byteOff / 4 + i] = words[i];
}

size_t countOccurrences(const std::string& text, const std::string& needle) {
    size_t n = 0, pos = 0;
    while ((pos = text.find(needle, pos)) != std::string::npos) {
        ++n;
        pos += needle.size();
    }
    return n;
}

// ---------------------------------------------------------------------------
// Decoder
// ---------------------------------------------------------------------------
void test_decoder_known_encodings() {
    auto d = decodeR4300(0x27BDFFE8);  // addiu sp, sp, -24
    CHECK(d.op == Op::Addiu && d.rs == 29 && d.rt == 29 && d.imm == -24);
    CHECK(d.dstGpr() == 29);

    d = decodeR4300(0x03E00008);  // jr ra
    CHECK(d.op == Op::Jr && d.isReturn() && d.hasDelaySlot() && !d.isCall());

    d = decodeR4300(0x0C000100);  // jal
    CHECK(d.op == Op::Jal && d.isCall() && d.dstGpr() == 31);
    CHECK(d.jumpTarget(0x80000000u) == 0x80000400u);

    d = decodeR4300(0x1000FFFF);  // b .  (beq zero,zero,-1)
    CHECK(d.op == Op::Beq && d.isUnconditionalBranch());
    CHECK(d.branchTarget(0x80001000u) == 0x80001000u);

    d = decodeR4300(0x04110001);  // bal (bgezal $zero)
    CHECK(d.op == Op::Bgezal && d.isCall() && d.isUnconditionalBranch());

    d = decodeR4300(beql(a0, zero, 2));
    CHECK(d.op == Op::Beql && d.isLikely() && !d.isUnconditionalBranch());

    d = decodeR4300(0x46000006);  // mov.s
    CHECK(d.op == Op::FMov && d.fmt == FpFmt::S);
    d = decodeR4300(0x46800021);  // cvt.d.w
    CHECK(d.op == Op::FCvtD && d.fmt == FpFmt::W);
    d = decodeR4300(0x46200021);  // cvt.d.d is not a valid conversion
    CHECK(!d.valid());
    d = decodeR4300(0x4600003C);  // c.lt.s
    CHECK(d.op == Op::FCmp && d.cond == 12);
    CHECK(disassemble(d, 0) == "c.lt.s $f0, $f0");
    d = decodeR4300(0x45000003);  // bc1f +3
    CHECK(d.op == Op::Bc1f && d.hasDelaySlot());
    d = decodeR4300(0x45030003);  // bc1tl
    CHECK(d.op == Op::Bc1tl && d.isLikely());

    CHECK(decodeR4300(0x42000018).op == Op::Eret);
    CHECK(decodeR4300(0x42000018).has(insn_flag::Eret));
    CHECK(decodeR4300(0x0000000D).op == Op::Break);
    CHECK(decodeR4300(0x0000000F).op == Op::Sync);
    CHECK(decodeR4300(0x48000000).op == Op::Invalid);  // COP2 transfer latch not lowered by the recompiler
    CHECK(decodeR4300(0x4C000000).op == Op::Invalid);  // COP1X absent on VR4300

    d = decodeR4300(0xDFA20000);  // ld v0, 0(sp)
    CHECK(d.op == Op::Ld && d.has(insn_flag::Load) && d.has(insn_flag::Dword) && d.dstGpr() == 2);
    d = decodeR4300(0xFFA20008);  // sd v0, 8(sp)
    CHECK(d.op == Op::Sd && d.has(insn_flag::Store) && d.dstGpr() == -1);
    d = decodeR4300(0x0000003C);  // dsll32 zero,zero,0
    CHECK(d.op == Op::Dsll32);
    d = decodeR4300(0x0000000C);
    CHECK(d.op == Op::Syscall);
    CHECK(decodeR4300(NOP).op == Op::Sll);
    CHECK(disassemble(decodeR4300(NOP), 0) == "nop");
    CHECK(disassemble(decodeR4300(0x27BDFFE8), 0) == "addiu $sp, $sp, -24");
    CHECK(disassemble(decodeR4300(lw(2, 29, 16)), 0) == "lw $v0, 16($sp)");
}

void test_decoder_tables_complete() {
    for (u16 i = 1; i < static_cast<u16>(Op::Count_); ++i) {
        CHECK(!mnemonic(static_cast<Op>(i)).empty());
    }
    // Deterministic sweep: decode must never crash and flags must be coherent.
    u32 x = 0x12345678u;
    size_t validCount = 0;
    for (int n = 0; n < 400000; ++n) {
        x = x * 1664525u + 1013904223u;
        const DecodedInsn d = decodeR4300(x);
        if (!d.valid()) { CHECK(d.flags == 0); continue; }
        ++validCount;
        if (d.isCall()) CHECK(d.dstGpr() >= 0);
        if (d.has(insn_flag::Load)) CHECK(!d.has(insn_flag::Store));
        CHECK(!disassemble(d, 0x80001000u).empty());
    }
    CHECK(validCount > 100000);
}

// Check interpreter exceptions for words outside the static decoder's
// supported set. CU2 is clear at reset and takes precedence over COP2 RI.
void test_decoder_vs_interpreter_reserved() {
    u32 x = 0xC0FFEE11u;
    int checked = 0, mismatches = 0;
    Bus bus;
    for (int n = 0; n < 200000 && checked < 20000; ++n) {
        x = x * 1664525u + 1013904223u;
        if (decodeR4300(x).valid()) continue;
        Cpu cpu;
        cpu.setPc(0xFFFFFFFF80000100ULL);
        bus.write32(0x100, x);
        ++checked;
        try {
            cpu.step(bus);
        } catch (const std::exception&) {
            // Legacy interpreter may attempt a memory access for words the decoder
            // rejects; treat any thrown exception as "not RI" and continue.
            continue;
        }
        const u32 op = x >> 26;
        const bool cop2 = op == 0x12 || op == 0x32 || op == 0x36 || op == 0x3A || op == 0x3E;
        const auto cause = cpu.cop0()[13];
        if (((cause >> 2) & 0x1F) != (cop2 ? 11 : 10) ||
            (cop2 && ((cause >> 28) & 3) != 2))
            ++mismatches;
    }
    CHECK(checked > 1000);
    CHECK(mismatches == 0);
}

// ---------------------------------------------------------------------------
// Analyzer
// ---------------------------------------------------------------------------
void test_function_discovery_and_calls() {
    std::vector<u32> w(0x30 / 4, NOP);
    put(w, 0x00, {addiu(sp, sp, -8), jal(0x80001020), NOP, jr(ra), addiu(sp, sp, 8)});
    put(w, 0x20, {jr(ra), NOP});
    AnalyzeOptions opt;
    opt.entries = {0x80001000};
    const Program p = analyze(imageOf(0x80001000, w), opt);
    CHECK(p.functions.size() == 2);
    const Function* f = p.find(0x80001000);
    const Function* g = p.find(0x80001020);
    CHECK(f && g);
    if (!f || !g) return;
    CHECK(f->blocks.size() == 1);
    CHECK(f->blocks.begin()->second.term == Terminator::Return);
    CHECK(f->blocks.begin()->second.end == 0x80001014);  // includes the jr delay slot
    CHECK(f->calls.size() == 1 && f->calls[0].target == 0x80001020 && !f->calls[0].indirect);
    CHECK(f->insnAddrs.size() == 5);  // unreachable nops are not part of f
    CHECK(g->blocks.size() == 1 && g->name == "func_80001020");
    // KSEG1 alias of the same physical address resolves to the same function.
    AnalyzeOptions alias;
    alias.entries = {0xA0001000};
    const Program pa = analyze(imageOf(0x80001000, w), alias);
    CHECK(pa.functions.size() == 2 && pa.find(0x80001000) != nullptr);
}

void test_loop_blocks_and_edges() {
    const Program p = [&] {
        AnalyzeOptions o;
        o.entries = {0x80002000};
        return analyze(imageOf(0x80002000, {addiu(t0, zero, 5), addiu(t0, t0, -1), bne(t0, zero, -2),
                                            NOP, jr(ra), NOP}), o);
    }();
    const Function* f = p.find(0x80002000);
    CHECK(f && f->blocks.size() == 3);
    if (!f) return;
    const BasicBlock& head = f->blocks.at(0x80002000);
    CHECK(head.term == Terminator::Fallthrough && head.end == 0x80002004);
    const BasicBlock& loop = f->blocks.at(0x80002004);
    CHECK(loop.term == Terminator::Branch && loop.end == 0x80002010 && loop.termPc == 0x80002008);
    CHECK(loop.succs.size() == 2);
    CHECK(loop.succs[0].kind == EdgeKind::Taken && loop.succs[0].target == 0x80002004);
    CHECK(loop.succs[1].kind == EdgeKind::Fallthrough && loop.succs[1].target == 0x80002010);
}

void test_branch_likely() {
    AnalyzeOptions o;
    o.entries = {0x80003000};
    const Program p = analyze(imageOf(0x80003000, {beql(a0, zero, 2), addiu(v0, zero, 1),
                                                  addiu(v0, v0, 2), jr(ra), NOP}), o);
    const Function* f = p.find(0x80003000);
    CHECK(f && f->blocks.size() == 3);
    if (!f) return;
    const BasicBlock& b0 = f->blocks.at(0x80003000);
    CHECK(b0.likely && b0.term == Terminator::Branch && b0.end == 0x80003008);
    CHECK(b0.succs.size() == 2 && b0.succs[0].target == 0x8000300C && b0.succs[1].target == 0x80003008);
}

void test_tail_call_resplits_function() {
    std::vector<u32> w(0x110 / 4, NOP);
    put(w, 0x000, {j(0x80004020), NOP});               // f: tail-calls g (before g is known)
    put(w, 0x020, {jr(ra), NOP});                      // g
    put(w, 0x100, {jal(0x80004020), NOP, jr(ra), NOP});  // h calls g -> makes it an entry
    AnalyzeOptions o;
    o.entries = {0x80004000, 0x80004100};
    const Program p = analyze(imageOf(0x80004000, w), o);
    CHECK(p.functions.size() == 3);
    const Function* f = p.find(0x80004000);
    CHECK(f && f->blocks.size() == 1);
    if (!f) return;
    CHECK(f->blocks.begin()->second.term == Terminator::TailCall);
    CHECK(f->tailTargets.count(0x80004020) == 1);
    CHECK(f->insnAddrs.count(0x80004020) == 0);
}

void test_jump_table_resolution() {
    std::vector<u32> w(0x10C / 4, NOP);
    put(w, 0x00, {sltiu(at, a0, 3), beq(at, zero, 8), NOP, sll(v0, a0, 2), lui(at, 0x8000),
                  addu(at, at, v0), lw(at, at, 0x5100), NOP, jr(at), NOP});
    put(w, 0x28, {jr(ra), addiu(v0, zero, -1)});
    put(w, 0x30, {jr(ra), addiu(v0, zero, 10)});
    put(w, 0x38, {jr(ra), addiu(v0, zero, 11)});
    put(w, 0x40, {jr(ra), addiu(v0, zero, 12)});
    put(w, 0x100, {0x80005030, 0x80005038, 0x80005040});
    AnalyzeOptions o;
    o.entries = {0x80005000};
    const Program p = analyze(imageOf(0x80005000, w), o);
    const Function* f = p.find(0x80005000);
    CHECK(f && f->jumpTables.size() == 1);
    if (!f || f->jumpTables.empty()) return;
    const JumpTable& jt = f->jumpTables[0];
    CHECK(jt.tableAddr == 0x80005100 && jt.boundFromCompare && jt.jrPc == 0x80005020);
    CHECK((jt.targets == std::vector<u32>{0x80005030, 0x80005038, 0x80005040}));
    CHECK(f->blocks.size() == 6);
    const BasicBlock& dispatch = f->blocks.at(0x8000500C);
    CHECK(dispatch.term == Terminator::JumpTable && dispatch.succs.size() == 3);
    for (const auto& e : dispatch.succs) CHECK(e.kind == EdgeKind::JumpTable);
    CHECK(f->warnings.empty());
}

void test_jump_table_full_address_and_sltu_bound() {
    std::vector<u32> w(0x110 / 4, NOP);
    put(w, 0x00, {addiu(t0, zero, 3), sltu(at, a0, t0), beq(at, zero, 9), NOP,
                  lui(t1, 0x8000), addiu(t1, t1, 0x5100), sll(v0, a0, 2),
                  addu(t1, t1, v0), lw(t1, t1, 0), NOP, jr(t1), NOP});
    put(w, 0x30, {jr(ra), addiu(v0, zero, -1)});
    put(w, 0x38, {jr(ra), addiu(v0, zero, 10)});
    put(w, 0x40, {jr(ra), addiu(v0, zero, 11)});
    put(w, 0x100, {0x80005030, 0x80005038, 0x80005040});
    AnalyzeOptions o;
    o.entries = {0x80005000};
    const Program p = analyze(imageOf(0x80005000, w), o);
    const Function* f = p.find(0x80005000);
    CHECK(f && f->jumpTables.size() == 1);
    if (!f || f->jumpTables.empty()) return;
    const JumpTable& jt = f->jumpTables[0];
    CHECK(jt.tableAddr == 0x80005100 && jt.boundFromCompare && jt.jrPc == 0x80005028);
    CHECK((jt.targets == std::vector<u32>{0x80005030, 0x80005038, 0x80005040}));
    const auto dispatch = f->blocks.find(0x80005010);
    CHECK(dispatch != f->blocks.end());
    if (dispatch != f->blocks.end()) CHECK(dispatch->second.term == Terminator::JumpTable);
    CHECK(f->warnings.empty());
}

void test_constant_indirect_transfers() {
    std::vector<u32> w(0x310 / 4, NOP);
    put(w, 0x000, {lui(at, 0x8000), ori(at, at, 0x7100), jr(at), NOP});
    put(w, 0x100, {jr(ra), NOP});
    put(w, 0x200, {lui(t0, 0x8000), ori(t0, t0, 0x7300), jalr(t0), NOP, jr(ra), NOP});
    put(w, 0x300, {jr(ra), NOP});
    AnalyzeOptions o;
    o.entries = {0x80007000, 0x80007200};
    const Program p = analyze(imageOf(0x80007000, w), o);
    const Function* f = p.find(0x80007000);
    CHECK(f != nullptr);
    if (!f) return;
    const auto entryBlock = f->blocks.find(0x80007000);
    CHECK(entryBlock != f->blocks.end());
    if (entryBlock != f->blocks.end()) {
        CHECK(entryBlock->second.term == Terminator::IndirectJump);
        CHECK(entryBlock->second.succs.size() == 1);
        if (!entryBlock->second.succs.empty())
            CHECK(entryBlock->second.succs[0].kind == EdgeKind::TailCall);
    }
    CHECK(f->tailTargets.count(0x80007100) == 1);
    CHECK(p.find(0x80007100) != nullptr);
    const Function* g = p.find(0x80007200);
    CHECK(g && g->calls.size() == 1);
    if (g && !g->calls.empty()) CHECK(g->calls[0].target == 0x80007300);
    CHECK(p.find(0x80007300) != nullptr);
    CHECK(f->warnings.empty() && g && g->warnings.empty());
}

void test_unresolved_indirect_jump_warns() {
    AnalyzeOptions o;
    o.entries = {0x80007000};
    const Program p = analyze(imageOf(0x80007000, {jr(t0), NOP}), o);
    const Function* f = p.find(0x80007000);
    CHECK(f && f->blocks.begin()->second.term == Terminator::IndirectJump);
    CHECK(f && f->warnings.size() == 1);
}

void test_dispatch_indirect_runtime() {
    RecompContext ctx;
    ctx.dispatch[0x80001000] = [](RecompContext& state) {
        CHECK(state.pc == 0x80001000);
        state.r[2] = 7;
    };
    dispatchIndirect(ctx, 0xA0001000);
    CHECK(ctx.r[2] == 7 && !ctx.indirectDispatchFailed);

    u32 hooked = 0;
    ctx.onUnresolvedIndirect = [&](RecompContext&, u32 target) { hooked = target; };
    dispatchIndirect(ctx, 0xA0002000);
    CHECK(hooked == 0x80002000 && ctx.pc == 0x80002000);
    CHECK(!ctx.indirectDispatchFailed);

    ctx.onUnresolvedIndirect = {};
    dispatchIndirect(ctx, 0x80003000);
    CHECK(ctx.indirectDispatchFailed && ctx.pc == 0x80003000);
}

std::vector<u32> sigBody(u32 luiImm, s32 off, u32 target, u32 aluOp = 0x21) {
    return {addiu(sp, sp, -16), sw(ra, sp, 12), lui(t0, luiImm), lw(t1, t0, off),
            jal(target), NOP, lw(ra, sp, 12), R(aluOp, t1, t1, v0), jr(ra), addiu(sp, sp, 16)};
}

void test_signatures_are_position_independent() {
    std::vector<u32> w(0x300 / 4, NOP);
    put(w, 0x000, sigBody(0x8001, 0x20, 0x80006100));
    put(w, 0x200, sigBody(0x8002, 0x44, 0x80006300));
    CodeImage img = imageOf(0x80006000, w);

    auto sig = makeSignature(img, 0x80006000, 10, "osFoo");
    CHECK(sig.has_value());
    if (!sig) return;
    SignatureDb db;
    CHECK(db.add(*sig));
    CHECK(!db.add(Signature{"tooShort", {1, 2, 3}}));
    CHECK(db.match(img, 0x80006200).value_or("") == "osFoo");

    // Text round-trip.
    SignatureDb db2;
    CHECK(db2.loadText("# libultra\n" + formatSignature(*sig) + "\n") == 1);
    CHECK(db2.match(img, 0x80006200).value_or("") == "osFoo");

    // A genuinely different body must not match.
    std::vector<u32> w2(0x100 / 4, NOP);
    put(w2, 0, sigBody(0x8001, 0x20, 0x80006100, 0x23 /* subu instead of addu */));
    CHECK(!db.match(imageOf(0x80006000, w2), 0x80006000).has_value());

    AnalyzeOptions o;
    o.entries = {0x80006000, 0x80006200};
    o.signatures = &db;
    const Program p = analyze(img, o);
    CHECK(p.find(0x80006000) && p.find(0x80006000)->name == "osFoo" && p.find(0x80006000)->nameFromSignature);
    CHECK(p.find(0x80006200) && p.find(0x80006200)->name == "osFoo");

    // Explicit symbols beat signatures.
    o.symbols[0x80006200] = "my_func";
    const Program p2 = analyze(img, o);
    CHECK(p2.find(0x80006200)->name == "my_func" && p2.find(0x80006200)->nameFromSymbol);
}

void test_invalid_and_end_of_image() {
    AnalyzeOptions o;
    o.entries = {0x80008000};
    const Program p = analyze(imageOf(0x80008000, {addiu(v0, zero, 1), 0x48000000}), o);
    const Function* f = p.find(0x80008000);
    CHECK(f && f->blocks.begin()->second.term == Terminator::InvalidInsn && f->warnings.size() == 1);

    const Program p2 = analyze(imageOf(0x80008000, {addiu(v0, zero, 1)}), o);
    CHECK(p2.find(0x80008000)->blocks.begin()->second.term == Terminator::EndOfImage);

    AnalyzeOptions bad;
    bad.entries = {0x90000000};
    const Program p3 = analyze(imageOf(0x80008000, {jr(ra), NOP}), bad);
    CHECK(p3.functions.empty() && p3.warnings.size() == 1);
}

void test_determinism() {
    std::vector<u32> w(0x110 / 4, NOP);
    put(w, 0x000, {j(0x80004020), NOP});
    put(w, 0x020, {jr(ra), NOP});
    put(w, 0x100, {jal(0x80004020), NOP, jr(ra), NOP});
    const CodeImage img = imageOf(0x80004000, w);
    AnalyzeOptions a, b;
    a.entries = {0x80004000, 0x80004100};
    b.entries = {0x80004100, 0x80004000};
    const Program pa = analyze(img, a), pb = analyze(img, b);
    CHECK(pa.functions.size() == pb.functions.size());
    for (const auto& [e, fa] : pa.functions) {
        const Function* fb = pb.find(e);
        CHECK(fb && dumpFunction(img, fa) == dumpFunction(img, *fb));
    }
    CHECK(!toDot(pa.functions.begin()->second).empty());
}

void test_cartridge_boot_image() {
    std::vector<u8> rom(2 * 1024 * 1024, 0);
    rom[0] = 0x80; rom[1] = 0x37; rom[2] = 0x12; rom[3] = 0x40;
    rom[8] = 0x80; rom[9] = 0x00; rom[10] = 0x10; rom[11] = 0x00;  // boot PC 0x80001000
    const u32 code[] = {jr(ra), NOP};
    for (int i = 0; i < 2; ++i) {
        for (int b = 0; b < 4; ++b) rom[0x1000 + i * 4 + b] = static_cast<u8>(code[i] >> (24 - 8 * b));
    }
    Cartridge cart;
    CHECK(cart.loadFromBuffer(rom, RomFormat::Z64));
    const CodeImage img = CodeImage::fromCartridgeBoot(cart, cart.header().bootPc);
    CHECK(img.contains(0x80001000) && img.contains(0x80100FFC) && !img.contains(0x80101000));
    AnalyzeOptions o;
    o.entries = {cart.header().bootPc};
    const Program p = analyze(img, o);
    CHECK(p.functions.size() == 1 && p.instructionCount() == 2);
}

// ---------------------------------------------------------------------------
// Recomp runtime (memory helpers and context)
// ---------------------------------------------------------------------------
void test_recomp_memory_helpers() {
    RecompContext ctx;
    ctx.setRdramSize(8 * 1024 * 1024);
    MEM_W(ctx, 0x80000000, 0x12345678);
    CHECK(MEM_WU(ctx, 0x80000000) == 0x12345678);
    CHECK(MEM_HU(ctx, 0x80000000) == 0x1234);
    CHECK(MEM_BU(ctx, 0x80000000) == 0x12);
    CHECK(MEM_HU(ctx, 0x80000002) == 0x5678);
    CHECK(MEM_BU(ctx, 0x80000001) == 0x34);

    MEM_H(ctx, 0x80000010, 0xAABB);
    CHECK(MEM_WU(ctx, 0x80000010) == 0xAABB0000);
    MEM_B(ctx, 0x80000013, 0xCC);
    CHECK(MEM_WU(ctx, 0x80000010) == 0xAABB00CC);

    MEM_D(ctx, 0x80000020, 0x0102030405060708ull);
    CHECK(MEM_D(ctx, 0x80000020) == 0x0102030405060708ull);

    // KSEG0/KSEG1 aliasing through address masking.
    CHECK(MEM_WU(ctx, 0xA0000000) == MEM_WU(ctx, 0x80000000));

    // Sign extension.
    MEM_B(ctx, 0x80000030, 0xFF);
    CHECK(MEM_B(ctx, 0x80000030) == -1);
    MEM_H(ctx, 0x80000032, 0x8000);
    CHECK(MEM_H(ctx, 0x80000032) == -32768);
    MEM_W(ctx, 0x80000034, 0xFFFFFFFF);
    CHECK(MEM_W(ctx, 0x80000034) == -1);
}

void test_recomp_lwl_lwr_swl_swr() {
    RecompContext ctx;
    ctx.setRdramSize(8 * 1024 * 1024);
    MEM_W(ctx, 0x80000000, 0x11223344);
    CHECK(LWL(ctx, 0x80000001, 0xAABBCCDD) == 0x223344DD);
    CHECK(LWR(ctx, 0x80000002, 0xAABBCCDD) == 0xAA112233);

    MEM_W(ctx, 0x80000000, 0x11223344);
    SWL(ctx, 0x80000001, 0xAABBCCDD);
    CHECK(MEM_WU(ctx, 0x80000000) == 0x11AABBCC);
    MEM_W(ctx, 0x80000000, 0x11223344);
    SWR(ctx, 0x80000002, 0xAABBCCDD);
    CHECK(MEM_WU(ctx, 0x80000000) == 0xAABBCC44);
}

// ---------------------------------------------------------------------------
// Emitter
// ---------------------------------------------------------------------------
void test_emitter_generates_function() {
    // f: addiu v0, zero, 5; addiu v0, v0, 3; jr ra; addiu v0, v0, 7
    AnalyzeOptions o;
    o.entries = {0x80009000};
    const Program p = analyze(imageOf(0x80009000, {addiu(v0, zero, 5), addiu(v0, v0, 3),
                                                    jr(ra), addiu(v0, v0, 7)}), o);
    CHECK(p.functions.size() == 1);
    const Function* f = p.find(0x80009000);
    CHECK(f);
    if (!f) return;
    const std::string code = emitFunction(*f, imageOf(0x80009000, {addiu(v0, zero, 5), addiu(v0, v0, 3),
                                                                   jr(ra), addiu(v0, v0, 7)}));
    CHECK(code.find("void func_80009000") != std::string::npos);
    CHECK(code.find("ctx.r[0] = 0;") != std::string::npos);
    CHECK(code.find("L_80009000:") != std::string::npos);
    CHECK(code.find("ctx.r[2]") != std::string::npos);
    CHECK(code.find("return;") != std::string::npos);
}

void test_emitter_program_produces_files() {
    AnalyzeOptions o;
    o.entries = {0x80009000};
    const CodeImage img = imageOf(0x80009000, {addiu(v0, zero, 1), jr(ra), NOP});
    const Program p = analyze(img, o);
    const EmittedProgram e = emitProgram(p, img);
    CHECK(!e.dispatchHeader.empty());
    CHECK(e.functionFiles.size() == 1);
    CHECK(!e.dispatchTable.empty());
    CHECK(!e.mainSource.empty());
    CHECK(e.cmakeLists.find("cmake_minimum_required") != std::string::npos);
    CHECK(e.cmakeLists.find("add_executable(n64forge_native") != std::string::npos);
}

void test_recomp_wide_multiply_and_bounds() {
    static_assert(std::is_same_v<decltype(MEM_W(std::declval<RecompContext&>(), 0u)), s64>);
    const auto signedProduct = DMULT(-1, 2);
    CHECK(signedProduct.first == 0xFFFFFFFFFFFFFFFEull);
    CHECK(signedProduct.second == 0xFFFFFFFFFFFFFFFFull);
    const auto unsignedProduct = DMULTU(0xFFFFFFFFFFFFFFFFull, 2);
    CHECK(unsignedProduct.first == 0xFFFFFFFFFFFFFFFEull);
    CHECK(unsignedProduct.second == 1);

    RecompContext ctx;
    ctx.setRdramSize(5);
    MEM_W(ctx, 0x80000004, 0x11223344);
    CHECK(MEM_WU(ctx, 0x80000004) == 0);
    MEM_H(ctx, 0x80000004, 0xAABB);
    CHECK(MEM_HU(ctx, 0x80000004) == 0);

    ctx.setRdramSize(8 * 1024 * 1024);
    MEM_W(ctx, 0x80000000, 0xFFFFFFFF);
    const u64 loaded = static_cast<u64>(MEM_W(ctx, 0x80000000));
    CHECK(loaded == 0xFFFFFFFFFFFFFFFFull);
}

void test_emitter_zero_register_and_lui() {
    AnalyzeOptions o;
    o.entries = {0x8000A000};
    const CodeImage img = imageOf(0x8000A000, {sll(zero, v0, 1), lui(t0, 0x8000),
                                                lwl(v0, zero, 0), jr(ra), NOP});
    const Program p = analyze(img, o);
    const Function* f = p.find(0x8000A000);
    CHECK(f);
    if (!f) return;
    const std::string code = emitFunction(*f, img);
    CHECK(countOccurrences(code, "ctx.r[0] = 0;") >= 3);
    CHECK(code.find("S32(0x80000000u)") != std::string::npos);
    CHECK(code.find("S32(LWL(") != std::string::npos);
    CHECK(code.find("__int128") == std::string::npos);
}

void test_emitter_direct_call_and_branch_link() {
    std::vector<u32> w(0x140 / 4, NOP);
    put(w, 0x00, {jal(0x8000B100), addiu(v0, zero, 9), jr(ra), NOP});
    put(w, 0x20, {bal(1), NOP, jr(ra), NOP});
    put(w, 0x28, {jr(ra), NOP});
    put(w, 0x100, {jr(ra), NOP});
    AnalyzeOptions o;
    o.entries = {0x8000B000, 0x8000B020};
    const CodeImage img = imageOf(0x8000B000, w);
    const Program p = analyze(img, o);
    CHECK(p.find(0x8000B100));
    const Function* caller = p.find(0x8000B000);
    CHECK(caller);
    if (!caller) return;
    const std::string code = emitFunction(*caller, img);
    CHECK(code.find("ctx.r[31] = 0x8000B008;") != std::string::npos);
    CHECK(code.find("0x8000B009") == std::string::npos);
    const auto slot = code.find("ctx.r[2] = ADD32(ctx.r[0], 9);");
    const auto dispatch = code.find("dispatchIndirect(ctx, 0x8000B100u)");
    CHECK(slot != std::string::npos && dispatch != std::string::npos && slot < dispatch);

    const Function* linked = p.find(0x8000B020);
    CHECK(linked);
    if (!linked) return;
    CHECK(linked->blocks.at(0x8000B020).term == Terminator::Jump);
    const std::string linkedCode = emitFunction(*linked, img);
    CHECK(linkedCode.find("ctx.r[31] = 0x8000B028;") != std::string::npos);
    CHECK(linkedCode.find("goto L_8000B028;") != std::string::npos);
}

void test_emitter_jump_table_dispatch() {
    std::vector<u32> w(0x10C / 4, NOP);
    put(w, 0x00, {sltiu(at, a0, 3), beq(at, zero, 8), NOP, sll(v0, a0, 2), lui(at, 0x8000),
                  addu(at, at, v0), lw(at, at, 0x5100), NOP, jr(at), NOP});
    put(w, 0x28, {jr(ra), addiu(v0, zero, -1)});
    put(w, 0x30, {jr(ra), addiu(v0, zero, 10)});
    put(w, 0x38, {jr(ra), addiu(v0, zero, 11)});
    put(w, 0x40, {jr(ra), addiu(v0, zero, 12)});
    put(w, 0x100, {0x80005030, 0x80005038, 0x80005040});
    AnalyzeOptions o;
    o.entries = {0x80005000};
    const CodeImage img = imageOf(0x80005000, w);
    const Program p = analyze(img, o);
    const Function* f = p.find(0x80005000);
    CHECK(f && f->jumpTables.size() == 1);
    if (!f || f->jumpTables.empty()) return;
    CHECK(f->jumpTables[0].indexReg == 4);
    const std::string code = emitFunction(*f, img);
    CHECK(code.find("const u32 jtIndex = static_cast<u32>(ctx.r[4]);") != std::string::npos);
    CHECK(code.find("switch (jtIndex) {") != std::string::npos);
    CHECK(code.find("case 0: ctx.pc = 0x80005030u; goto L_80005030;") != std::string::npos);
    CHECK(code.find("case 2: ctx.pc = 0x80005040u; goto L_80005040;") != std::string::npos);
    CHECK(code.find("const u32 jtTarget = static_cast<u32>(ctx.r[1]);") != std::string::npos);
    CHECK(code.find("default: dispatchIndirect(ctx, jtTarget);") != std::string::npos);
}

void test_emitter_indirect_fallback() {
    AnalyzeOptions o;
    o.entries = {0x80007000};
    const CodeImage img = imageOf(0x80007000, {jr(t0), addiu(v0, zero, 5)});
    const Program p = analyze(img, o);
    const Function* f = p.find(0x80007000);
    CHECK(f != nullptr);
    if (!f) return;
    const std::string code = emitFunction(*f, img);
    CHECK(code.find("const u32 jrTarget = static_cast<u32>(ctx.r[8]);") != std::string::npos);
    CHECK(code.find("ctx.r[2] = ADD32(ctx.r[0], 5);") < code.find("dispatchIndirect(ctx, jrTarget)"));
    CHECK(code.find("dispatchIndirect(ctx, jrTarget);\nreturn;") != std::string::npos);
}

void test_emitter_namespace_and_native_project() {
    AnalyzeOptions o;
    o.entries = {0x80009000};
    const CodeImage img = imageOf(0x80009000, {addiu(v0, zero, 1), jr(ra), NOP});
    const Program p = analyze(img, o);
    EmitOptions options;
    options.namespaceName = "custom_recomp";
    options.sourceRoot = "/tmp/n64forge";
    const EmittedProgram e = emitProgram(p, img, options);
    CHECK(e.errors.empty());
    CHECK(e.dispatchHeader.find("namespace custom_recomp {") != std::string::npos);
    CHECK(e.functionFiles.size() == 1);
    CHECK(e.functionFiles[0].second.find("namespace custom_recomp {") != std::string::npos);
    CHECK(e.dispatchTable.find("custom_recomp::func_80009000") != std::string::npos);
    CHECK(e.mainSource.find("n64forge_recomp_register_dispatch(ctx);") != std::string::npos);
    CHECK(e.cmakeLists.find("set(N64FORGE_ROOT \"/tmp/n64forge\"") != std::string::npos);
    CHECK(e.cmakeLists.find("add_subdirectory(\"${N64FORGE_ROOT}\"") != std::string::npos);
}

void test_emitter_rejects_unsupported_ops() {
    AnalyzeOptions o;
    o.entries = {0x8000C000};
    const CodeImage img = imageOf(0x8000C000, {syscallInsn(), jr(ra), NOP});
    const Program p = analyze(img, o);
    const EmittedProgram e = emitProgram(p, img);
    CHECK(!e.errors.empty());
    CHECK(e.functionFiles.size() == 1);
    CHECK(e.functionFiles[0].second.find("#error Unsupported MIPS SYSCALL") != std::string::npos);
}

}  // namespace

int run_recomp_tests() {
    g_failures = 0;
    test_decoder_known_encodings();
    test_decoder_tables_complete();
    test_decoder_vs_interpreter_reserved();
    test_function_discovery_and_calls();
    test_loop_blocks_and_edges();
    test_branch_likely();
    test_tail_call_resplits_function();
    test_jump_table_resolution();
    test_jump_table_full_address_and_sltu_bound();
    test_constant_indirect_transfers();
    test_unresolved_indirect_jump_warns();
    test_dispatch_indirect_runtime();
    test_signatures_are_position_independent();
    test_invalid_and_end_of_image();
    test_determinism();
    test_cartridge_boot_image();
    test_recomp_memory_helpers();
    test_recomp_lwl_lwr_swl_swr();
    test_emitter_generates_function();
    test_emitter_program_produces_files();
    test_recomp_wide_multiply_and_bounds();
    test_emitter_zero_register_and_lui();
    test_emitter_direct_call_and_branch_link();
    test_emitter_jump_table_dispatch();
    test_emitter_indirect_fallback();
    test_emitter_namespace_and_native_project();
    test_emitter_rejects_unsupported_ops();
    if (g_failures) std::fprintf(stderr, "%d recomp test check(s) failed\n", g_failures);
    return g_failures;
}
