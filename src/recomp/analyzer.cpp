#include "n64forge/recomp/analyzer.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <sstream>

namespace n64forge::recomp {

using namespace n64forge::insn_flag;

// ===========================================================================
// Addresses & code image
// ===========================================================================
u32 canonicalAddress(u32 vaddr) noexcept {
    if (vaddr >= 0x80000000u && vaddr < 0xC0000000u) return (vaddr & 0x1FFFFFFFu) | 0x80000000u;
    return vaddr;
}

void CodeImage::addRegion(u32 vaddr, std::vector<u8> bytes) {
    if (bytes.empty()) return;
    regions_.push_back(Region{canonicalAddress(vaddr), std::move(bytes)});
}

void CodeImage::addWords(u32 vaddr, const std::vector<u32>& words) {
    std::vector<u8> bytes;
    bytes.reserve(words.size() * 4);
    for (u32 w : words) {
        bytes.push_back(static_cast<u8>(w >> 24));
        bytes.push_back(static_cast<u8>(w >> 16));
        bytes.push_back(static_cast<u8>(w >> 8));
        bytes.push_back(static_cast<u8>(w));
    }
    addRegion(vaddr, std::move(bytes));
}

CodeImage CodeImage::fromCartridgeBoot(const Cartridge& cart, u32 entry) {
    constexpr size_t kBootOffset = 0x1000;
    constexpr size_t kBootSize = 0x100000;
    CodeImage img;
    const auto& rom = cart.rom();
    if (rom.size() > kBootOffset) {
        const size_t n = std::min(kBootSize, rom.size() - kBootOffset);
        img.addRegion(entry, std::vector<u8>(rom.begin() + kBootOffset,
                                             rom.begin() + static_cast<std::ptrdiff_t>(kBootOffset + n)));
    }
    return img;
}

bool CodeImage::read32(u32 vaddr, u32& out) const noexcept {
    if (vaddr & 3u) return false;
    const u64 a = canonicalAddress(vaddr);
    for (const auto& r : regions_) {
        if (a >= r.vaddr && a + 4 <= static_cast<u64>(r.vaddr) + r.bytes.size()) {
            const u8* p = r.bytes.data() + (a - r.vaddr);
            out = (static_cast<u32>(p[0]) << 24) | (static_cast<u32>(p[1]) << 16) |
                  (static_cast<u32>(p[2]) << 8) | p[3];
            return true;
        }
    }
    return false;
}

bool CodeImage::contains(u32 vaddr) const noexcept {
    u32 dummy;
    return read32(vaddr & ~3u, dummy);
}

// ===========================================================================
// Signatures
// ===========================================================================
namespace {

// Zeroes relocatable fields so one signature matches any link address.
std::optional<std::vector<u32>> normalizeWindow(const CodeImage& img, u32 entry, size_t count) {
    std::vector<u32> out;
    out.reserve(count);
    u32 luiRegs = 0;
    for (size_t i = 0; i < count; ++i) {
        u32 w;
        if (!img.read32(entry + static_cast<u32>(i * 4), w)) return std::nullopt;
        const DecodedInsn d = decodeR4300(w);
        u32 n = w;
        if (d.op == Op::J || d.op == Op::Jal) {
            n &= 0xFC000000u;
        } else if (d.op == Op::Lui) {
            n &= 0xFFFF0000u;
        } else if ((d.has(DstRt) || d.has(Load) || d.has(Store)) && ((luiRegs >> d.rs) & 1u)) {
            n &= 0xFFFF0000u;
        }
        if (d.op == Op::Lui) {
            luiRegs |= 1u << d.rt;
        } else if (const int dst = d.dstGpr(); dst >= 0) {
            luiRegs &= ~(1u << dst);
        }
        out.push_back(n);
    }
    return out;
}

}  // namespace

bool SignatureDb::add(Signature sig) {
    if (sig.words.size() < kMinWords || sig.name.empty()) return false;
    sigs_.push_back(std::move(sig));
    return true;
}

size_t SignatureDb::loadText(const std::string& text, std::vector<std::string>* errors) {
    std::istringstream in(text);
    std::string line;
    size_t added = 0, lineNo = 0;
    while (std::getline(in, line)) {
        ++lineNo;
        if (const auto hash = line.find('#'); hash != std::string::npos) line.erase(hash);
        std::istringstream ls(line);
        Signature sig;
        if (!(ls >> sig.name)) continue;
        std::string tok;
        bool bad = false;
        while (ls >> tok) {
            char* end = nullptr;
            const unsigned long v = std::strtoul(tok.c_str(), &end, 16);
            if (end == tok.c_str() || *end != '\0') { bad = true; break; }
            sig.words.push_back(static_cast<u32>(v));
        }
        if (bad || !add(std::move(sig))) {
            if (errors) errors->push_back("signature line " + std::to_string(lineNo) + " rejected");
        } else {
            ++added;
        }
    }
    return added;
}

std::optional<std::string> SignatureDb::match(const CodeImage& img, u32 entry) const {
    for (const auto& sig : sigs_) {
        const auto window = normalizeWindow(img, entry, sig.words.size());
        if (window && *window == sig.words) return sig.name;
    }
    return std::nullopt;
}

std::optional<Signature> makeSignature(const CodeImage& img, u32 entry, size_t count,
                                       std::string name) {
    auto w = normalizeWindow(img, entry, count);
    if (!w) return std::nullopt;
    return Signature{std::move(name), std::move(*w)};
}

std::string formatSignature(const Signature& sig) {
    std::string s = sig.name;
    char buf[16];
    for (u32 w : sig.words) {
        std::snprintf(buf, sizeof buf, " %08X", w);
        s += buf;
    }
    return s;
}

// ===========================================================================
// Jump-table resolution (backward pattern match over the linear instruction
// stream; matches the non-PIC GCC/IDO switch idiom used by N64 titles)
//
//     sltiu  at, idx, N      ; bounds check            (optional, gives N)
//     beq    at, zero, Ldefault
//     sll    t, idx, 2
//     lui    b, %hi(table)
//     addu   b, b, t
//     lw     r, %lo(table)(b)
//     jr     r
// ===========================================================================
namespace {

struct Def {
    u32 addr;
    DecodedInsn d;
};

bool readDecoded(const CodeImage& img, u32 pc, DecodedInsn& out) {
    u32 w;
    if (!img.read32(pc, w)) return false;
    out = decodeR4300(w);
    return true;
}

std::optional<Def> findDefBackward(const CodeImage& img, u32 from, int reg, int window = 40) {
    if (reg <= 0) return std::nullopt;
    for (int k = 1; k <= window; ++k) {
        const u32 a = from - static_cast<u32>(k) * 4u;
        DecodedInsn d;
        if (!readDecoded(img, a, d)) return std::nullopt;
        if (d.dstGpr() == reg) return Def{a, d};
        // Straight-line barriers: unconditional transfers and calls end the window.
        if (d.op == Op::J || d.op == Op::Jr || d.has(Eret) || d.has(Break) || d.isCall()) {
            return std::nullopt;
        }
    }
    return std::nullopt;
}

std::optional<u32> constantOperand(const CodeImage& img, u32 from, int reg) {
    if (reg == 0) return 0;
    const auto def = findDefBackward(img, from, reg);
    if (!def) return std::nullopt;
    if (def->d.op == Op::Lui) return static_cast<u32>(def->d.imm) << 16;
    if (def->d.op != Op::Addiu && def->d.op != Op::Ori) return std::nullopt;
    u32 base = 0;
    if (def->d.rs) {
        const auto upper = findDefBackward(img, def->addr, def->d.rs);
        if (!upper || upper->d.op != Op::Lui) return std::nullopt;
        base = static_cast<u32>(upper->d.imm) << 16;
    }
    return def->d.op == Op::Ori ? base | static_cast<u16>(def->d.imm)
                                : base + static_cast<u32>(def->d.imm);
}

std::optional<JumpTable> resolveJumpTable(const CodeImage& img, u32 jrPc, const DecodedInsn& jr,
                                          const AnalyzeOptions& opts) {
    const auto lw = findDefBackward(img, jrPc, jr.rs);
    if (!lw || (lw->d.op != Op::Lw && lw->d.op != Op::Lwu)) return std::nullopt;
    const int baseReg = lw->d.rs;
    const s32 lowOff = lw->d.imm;

    const auto add = findDefBackward(img, lw->addr, baseReg);
    if (!add || (add->d.op != Op::Addu && add->d.op != Op::Add && add->d.op != Op::Daddu)) {
        return std::nullopt;
    }

    std::optional<u32> tableBase;
    std::optional<Def> indexShift;
    for (const auto& [base, index] : {std::pair{add->d.rs, add->d.rt}, std::pair{add->d.rt, add->d.rs}}) {
        auto sll = findDefBackward(img, add->addr, index);
        if (!sll || sll->d.op != Op::Sll || sll->d.sa != 2) continue;
        if ((tableBase = constantOperand(img, add->addr, base))) {
            indexShift = sll;
            break;
        }
    }
    if (!tableBase || !indexShift) return std::nullopt;

    JumpTable jt;
    jt.jrPc = jrPc;
    jt.tableAddr = *tableBase + static_cast<u32>(lowOff);

    // Table size from a preceding `sltiu x, idx, N`.
    const int origIdx = indexShift->d.rt;
    if (origIdx <= 0) return std::nullopt;
    jt.indexReg = origIdx;
    size_t bound = 0;
    for (int k = 1; k <= 48 && bound == 0; ++k) {
        DecodedInsn d;
        if (!readDecoded(img, jrPc - static_cast<u32>(k) * 4u, d)) break;
        if (d.op == Op::Sltiu && d.rs == origIdx && d.imm > 0) {
            bound = static_cast<size_t>(d.imm);
        } else if (d.op == Op::Sltu && d.rs == origIdx) {
            const auto value = constantOperand(img, jrPc - static_cast<u32>(k) * 4u, d.rt);
            if (value && *value <= opts.maxJumpTableEntries) bound = *value;
        }
    }
    jt.boundFromCompare = bound != 0;

    const size_t limit = bound ? std::min(bound, opts.maxJumpTableEntries)
                               : std::min<size_t>(256, opts.maxJumpTableEntries);
    for (size_t i = 0; i < limit; ++i) {
        u32 t;
        if (!img.read32(jt.tableAddr + static_cast<u32>(i * 4), t)) break;
        const u32 ct = canonicalAddress(t);
        const u32 dist = ct > jrPc ? ct - jrPc : jrPc - ct;
        if ((t & 3u) || !img.contains(t) || dist > 0x10000u) break;
        jt.targets.push_back(ct);
    }
    if (jt.targets.empty()) return std::nullopt;
    return jt;
}

// ===========================================================================
// Per-function exploration
// ===========================================================================
struct Flow {
    Terminator term;
    std::vector<Edge> succs;
};

class FunctionBuilder {
public:
    FunctionBuilder(const CodeImage& img, const std::set<u32>& entries, const AnalyzeOptions& opts,
                    u32 entry)
        : img_(img), entries_(entries), opts_(opts), e_(entry) {}

    Function run() {
        fn_.entry = e_;
        leaders_.insert(e_);
        work_.push_back(e_);
        while (!work_.empty() && !capHit_) {
            u32 pc = work_.back();
            work_.pop_back();
            walk(pc);
        }
        buildBlocks();
        if (!fn_.insnAddrs.empty()) {
            fn_.minAddr = *fn_.insnAddrs.begin();
            fn_.maxAddr = *fn_.insnAddrs.rbegin() + 4;
        }
        return std::move(fn_);
    }

private:
    bool isOtherEntry(u32 a) const { return a != e_ && entries_.count(a) != 0; }

    // Queues `target` for exploration (or records it as leaving the function).
    Edge makeEdge(u32 rawTarget, EdgeKind kind, u32 fromPc) {
        const u32 t = canonicalAddress(rawTarget);
        if (isOtherEntry(t)) {
            fn_.tailTargets.insert(t);
            return Edge{t, EdgeKind::TailCall};
        }
        if (!img_.contains(t)) {
            fn_.tailTargets.insert(t);
            warn(fromPc, "transfer target outside loaded image");
            return Edge{t, EdgeKind::TailCall};
        }
        leaders_.insert(t);
        work_.push_back(t);
        return Edge{t, kind};
    }

    void warn(u32 pc, const std::string& msg) {
        char buf[16];
        std::snprintf(buf, sizeof buf, "%08X: ", pc);
        fn_.warnings.push_back(buf + msg);
    }

    void walk(u32 pc) {
        for (;;) {
            if (fn_.insnAddrs.count(pc)) return;
            if (pc != e_ && entries_.count(pc)) return;  // fell into another function
            DecodedInsn d;
            if (!readDecoded(img_, pc, d)) { flows_[pc] = {Terminator::EndOfImage, {}}; return; }
            if (!d.valid()) {
                fn_.insnAddrs.insert(pc);
                flows_[pc] = {Terminator::InvalidInsn, {}};
                warn(pc, "undecodable instruction");
                return;
            }
            fn_.insnAddrs.insert(pc);
            if (fn_.insnAddrs.size() > opts_.maxFunctionInsns) {
                capHit_ = true;
                warn(pc, "function exceeds maxFunctionInsns; truncated");
                return;
            }
            if (d.has(Eret)) { flows_[pc] = {Terminator::Eret, {}}; return; }
            if (d.has(Break)) { flows_[pc] = {Terminator::Break, {}}; return; }
            if (!d.hasDelaySlot()) { pc += 4; continue; }

            // --- instruction with a delay slot ---
            DecodedInsn slot;
            if (!readDecoded(img_, pc + 4, slot)) { flows_[pc] = {Terminator::EndOfImage, {}}; return; }
            fn_.insnAddrs.insert(pc + 4);
            if (slot.hasDelaySlot()) warn(pc + 4, "branch/jump in a delay slot");
            const u32 next = pc + 8;

            if (d.op == Op::Jal) {
                fn_.calls.push_back({pc, canonicalAddress(d.jumpTarget(pc)), false});
                pc = next;
                continue;
            }
            if (d.op == Op::Jalr) {
                const auto target = constantOperand(img_, pc, d.rs);
                fn_.calls.push_back({pc, target && (*target & 3) == 0 ? canonicalAddress(*target) : 0, true});
                pc = next;
                continue;
            }
            if (d.has(Branch) && d.has(Link)) {  // bal / bltzal / bgezal...
                // A branch-and-link is an intra-function control transfer; it
                // writes $ra but is not a call to a separate function.
                const Edge taken = makeEdge(d.branchTarget(pc), EdgeKind::Taken, pc);
                Flow flow{Terminator::Fallthrough, {}};
                flow.succs.push_back(taken);
                if (d.isUnconditionalBranch()) {
                    flow.term = taken.kind == EdgeKind::TailCall ? Terminator::TailCall : Terminator::Jump;
                } else {
                    flow.term = Terminator::Branch;
                    flow.succs.push_back(makeEdge(next, EdgeKind::Fallthrough, pc));
                }
                flows_[pc] = std::move(flow);
                return;
            }

            Flow flow{Terminator::Fallthrough, {}};
            if (d.op == Op::Jr) {
                if (d.isReturn()) {
                    flow.term = Terminator::Return;
                } else if (auto jt = resolveJumpTable(img_, pc, d, opts_)) {
                    flow.term = Terminator::JumpTable;
                    for (u32 t : jt->targets) flow.succs.push_back(makeEdge(t, EdgeKind::JumpTable, pc));
                    if (!jt->boundFromCompare) warn(pc, "jump table size guessed (no sltiu bound found)");
                    fn_.jumpTables.push_back(std::move(*jt));
                } else {
                    flow.term = Terminator::IndirectJump;
                    const auto target = constantOperand(img_, pc, d.rs);
                    if (target && (*target & 3) == 0 && img_.contains(*target) && canonicalAddress(*target) != e_) {
                        const auto t = canonicalAddress(*target);
                        fn_.tailTargets.insert(t);
                        flow.succs.push_back(Edge{t, EdgeKind::TailCall});
                    } else {
                        warn(pc, "unresolved indirect jump (jr)");
                    }
                }
            } else if (d.op == Op::J) {
                const u32 t = canonicalAddress(d.jumpTarget(pc));
                const bool tail = t != e_ && (entries_.count(t) || t < e_ || !img_.contains(t));
                Edge edge = tail ? Edge{t, EdgeKind::TailCall} : makeEdge(t, EdgeKind::Taken, pc);
                if (tail) {
                    fn_.tailTargets.insert(t);
                    if (!img_.contains(t)) warn(pc, "tail-call target outside loaded image");
                }
                flow.term = edge.kind == EdgeKind::TailCall ? Terminator::TailCall : Terminator::Jump;
                flow.succs.push_back(edge);
            } else {  // conditional/unconditional PC-relative branch
                const Edge taken = makeEdge(d.branchTarget(pc), EdgeKind::Taken, pc);
                flow.succs.push_back(taken);
                if (d.isUnconditionalBranch()) {
                    flow.term = taken.kind == EdgeKind::TailCall ? Terminator::TailCall : Terminator::Jump;
                } else {
                    flow.term = Terminator::Branch;
                    flow.succs.push_back(makeEdge(next, EdgeKind::Fallthrough, pc));
                }
            }
            flows_[pc] = std::move(flow);
            return;
        }
    }

    void buildBlocks() {
        std::vector<u32> leaders;
        for (u32 l : leaders_) {
            if (fn_.insnAddrs.count(l)) leaders.push_back(l);
        }
        for (u32 start : leaders) {
            BasicBlock bb;
            bb.start = start;
            u32 pc = start;
            for (;;) {
                DecodedInsn d;
                readDecoded(img_, pc, d);
                const bool slotInFn = d.hasDelaySlot() && fn_.insnAddrs.count(pc + 4);
                const u32 step = slotInFn ? 8u : 4u;
                if (auto it = flows_.find(pc); it != flows_.end()) {
                    bb.term = it->second.term;
                    bb.termPc = pc;
                    bb.likely = d.isLikely();
                    bb.succs = it->second.succs;
                    bb.end = pc + step;
                    break;
                }
                const u32 next = pc + step;
                if (leaders_.count(next) && fn_.insnAddrs.count(next)) {
                    bb.term = Terminator::Fallthrough;
                    bb.succs.push_back({next, EdgeKind::Fallthrough});
                    bb.end = next;
                    break;
                }
                if (!fn_.insnAddrs.count(next)) {
                    bb.end = next;
                    if (isOtherEntry(next)) {
                        bb.term = Terminator::TailCall;
                        bb.termPc = pc;
                        bb.succs.push_back({next, EdgeKind::TailCall});
                        fn_.tailTargets.insert(next);
                    } else {
                        bb.term = Terminator::EndOfImage;
                    }
                    break;
                }
                pc = next;
            }
            fn_.blocks.emplace(start, std::move(bb));
        }
    }

    const CodeImage& img_;
    const std::set<u32>& entries_;
    const AnalyzeOptions& opts_;
    u32 e_;
    Function fn_;
    std::set<u32> leaders_;
    std::vector<u32> work_;
    std::map<u32, Flow> flows_;
    bool capHit_ = false;
};

}  // namespace

// ===========================================================================
// Whole-program driver
// ===========================================================================
const Function* Program::find(u32 entry) const {
    const auto it = functions.find(canonicalAddress(entry));
    return it == functions.end() ? nullptr : &it->second;
}

size_t Program::blockCount() const {
    size_t n = 0;
    for (const auto& [_, f] : functions) n += f.blocks.size();
    return n;
}

size_t Program::instructionCount() const {
    size_t n = 0;
    for (const auto& [_, f] : functions) n += f.insnAddrs.size();
    return n;
}

Program analyze(const CodeImage& image, const AnalyzeOptions& options) {
    Program prog;
    std::set<u32> entries;
    std::deque<u32> pending;
    auto addEntry = [&](u32 raw, const char* why) {
        const u32 a = canonicalAddress(raw);
        if (!image.contains(a)) {
            if (why) {
                char buf[96];
                std::snprintf(buf, sizeof buf, "%s 0x%08X is outside the loaded image; skipped", why, raw);
                prog.warnings.push_back(buf);
            }
            return;
        }
        if (entries.insert(a).second) pending.push_back(a);
    };
    for (u32 e : options.entries) addEntry(e, "entry point");
    for (const auto& [addr, _] : options.symbols) addEntry(addr, "symbol");

    std::map<u32, int> reanalyses;
    for (;;) {
        while (!pending.empty()) {
            const u32 e = pending.front();
            pending.pop_front();
            if (prog.functions.count(e)) continue;
            Function fn = FunctionBuilder(image, entries, options, e).run();
            for (const auto& c : fn.calls) {
                if (c.target) addEntry(c.target, nullptr);
            }
            for (u32 t : fn.tailTargets) addEntry(t, nullptr);
            prog.functions.emplace(e, std::move(fn));
        }
        // A function whose body swallowed a later-discovered entry must be re-split.
        bool again = false;
        for (auto& [fe, fn] : prog.functions) {
            if (reanalyses[fe] >= 4) continue;
            const auto lo = entries.upper_bound(fe);
            for (auto it = lo; it != entries.end() && *it < fn.maxAddr; ++it) {
                if (fn.insnAddrs.count(*it)) {
                    ++reanalyses[fe];
                    pending.push_back(fe);
                    again = true;
                    break;
                }
            }
        }
        if (!again) break;
        for (u32 e : pending) prog.functions.erase(e);
    }

    // Naming: user symbol > signature > synthetic.
    for (auto& [e, fn] : prog.functions) {
        char buf[32];
        if (const auto sym = options.symbols.find(e); sym != options.symbols.end()) {
            fn.name = sym->second;
            fn.nameFromSymbol = true;
        } else if (const auto sig = options.signatures ? options.signatures->match(image, e)
                                                       : std::nullopt) {
            fn.name = *sig;
            fn.nameFromSignature = true;
        } else {
            std::snprintf(buf, sizeof buf, "func_%08X", e);
            fn.name = buf;
        }
    }
    return prog;
}

// ===========================================================================
// Debug output
// ===========================================================================
namespace {
const char* termName(Terminator t) {
    switch (t) {
        case Terminator::Fallthrough: return "fallthrough";
        case Terminator::Branch: return "branch";
        case Terminator::Jump: return "jump";
        case Terminator::TailCall: return "tailcall";
        case Terminator::Return: return "return";
        case Terminator::JumpTable: return "jumptable";
        case Terminator::IndirectJump: return "indirect";
        case Terminator::Eret: return "eret";
        case Terminator::Break: return "break";
        case Terminator::EndOfImage: return "end-of-image";
        case Terminator::InvalidInsn: return "invalid";
    }
    return "?";
}
}  // namespace

std::string toDot(const Function& fn) {
    std::string s = "digraph \"" + fn.name + "\" {\n  node [shape=box,fontname=monospace];\n";
    char buf[160];
    for (const auto& [start, bb] : fn.blocks) {
        std::snprintf(buf, sizeof buf, "  b%08X [label=\"%08X\\n%s\"];\n", start, start, termName(bb.term));
        s += buf;
        for (const auto& e : bb.succs) {
            const char* style = e.kind == EdgeKind::TailCall ? " [style=dashed]"
                                : e.kind == EdgeKind::JumpTable ? " [color=blue]" : "";
            std::snprintf(buf, sizeof buf, "  b%08X -> b%08X%s;\n", start, e.target, style);
            s += buf;
        }
    }
    return s + "}\n";
}

std::string dumpFunction(const CodeImage& image, const Function& fn) {
    std::string s;
    char buf[160];
    std::snprintf(buf, sizeof buf, "%s @ 0x%08X  [%08X,%08X) %zu blocks\n", fn.name.c_str(), fn.entry,
                  fn.minAddr, fn.maxAddr, fn.blocks.size());
    s += buf;
    for (const auto& [start, bb] : fn.blocks) {
        std::snprintf(buf, sizeof buf, " block %08X..%08X  %s%s\n", bb.start, bb.end, termName(bb.term),
                      bb.likely ? " (likely)" : "");
        s += buf;
        for (u32 pc = bb.start; pc < bb.end; pc += 4) {
            u32 w = 0;
            if (!image.read32(pc, w)) w = 0;
            std::snprintf(buf, sizeof buf, "   %08X: %08X  ", pc, w);
            s += buf;
            s += disassemble(decodeR4300(w), pc);
            s += '\n';
        }
        for (const auto& e : bb.succs) {
            static const char* kinds[] = {"fall", "taken", "table", "tail"};
            std::snprintf(buf, sizeof buf, "   -> %s %08X\n", kinds[static_cast<int>(e.kind)], e.target);
            s += buf;
        }
    }
    for (const auto& w : fn.warnings) s += " warning: " + w + "\n";
    return s;
}

}  // namespace n64forge::recomp
