#pragma once
// n64forge_recomp — static analysis front-end for the N64 static recompiler.
//
// Scope (Track A, step 1): discover functions, build per-function control-flow
// graphs, resolve switch-style jump tables and name known libultra functions.
// No C++ is emitted here; the emitter consumes `Program`.
#include "n64forge/core/cartridge.hpp"
#include "n64forge/core/r4300_decode.hpp"
#include "n64forge/core/types.hpp"

#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace n64forge::recomp {

// ---------------------------------------------------------------------------
// Code image: one or more regions of big-endian ROM/RDRAM bytes addressed by
// virtual address. KSEG0/KSEG1 aliases of the same physical address are the
// same byte, so lookups mask with 0x1FFFFFFF for 0x8xxxxxxx / 0xAxxxxxxx.
// ---------------------------------------------------------------------------
class CodeImage {
public:
    struct Region {
        u32 vaddr = 0;
        std::vector<u8> bytes;
    };

    // Adds a region; later regions do not override earlier overlapping bytes.
    void addRegion(u32 vaddr, std::vector<u8> bytes);
    void addWords(u32 vaddr, const std::vector<u32>& words);  // test helper

    // Models what IPL3 does at boot: ROM[0x1000, 0x1000 + 1 MiB) -> `entry`.
    // Code loaded later via PI DMA (overlays, the main game binary of many
    // titles) is NOT visible until added with addRegion().
    static CodeImage fromCartridgeBoot(const Cartridge& cart, u32 entry);

    [[nodiscard]] bool read32(u32 vaddr, u32& out) const noexcept;
    [[nodiscard]] bool contains(u32 vaddr) const noexcept;
    [[nodiscard]] const std::vector<Region>& regions() const noexcept { return regions_; }

private:
    std::vector<Region> regions_;
};

// KSEG0/KSEG1 -> canonical KSEG0 address; other addresses returned unchanged.
[[nodiscard]] u32 canonicalAddress(u32 vaddr) noexcept;

// ---------------------------------------------------------------------------
// Signatures: position-independent fingerprints of function prefixes.
// A word is "normalized" by zeroing relocatable fields (J/JAL targets, LUI
// immediates and immediates of ops based on a LUI-built register). Signatures
// are stored already normalized, so one file works for any link address.
// No signatures are built in: supply them from your own libultra build.
// ---------------------------------------------------------------------------
struct Signature {
    std::string name;
    std::vector<u32> words;  // normalized
};

class SignatureDb {
public:
    static constexpr size_t kMinWords = 8;  // shorter signatures are rejected as too ambiguous

    bool add(Signature sig);
    // Text format, one per line: `name 0xW0 0xW1 ...` (hex, normalized). '#' comments.
    size_t loadText(const std::string& text, std::vector<std::string>* errors = nullptr);
    [[nodiscard]] std::optional<std::string> match(const CodeImage& img, u32 entry) const;
    [[nodiscard]] size_t size() const noexcept { return sigs_.size(); }

private:
    std::vector<Signature> sigs_;
};

// Builds a signature from `count` words at `entry` (for authoring signature files).
[[nodiscard]] std::optional<Signature> makeSignature(const CodeImage& img, u32 entry, size_t count,
                                                     std::string name);
[[nodiscard]] std::string formatSignature(const Signature& sig);

// ---------------------------------------------------------------------------
// Analysis results
// ---------------------------------------------------------------------------
enum class EdgeKind : u8 {
    Fallthrough,  // sequential flow (also the not-taken side of a branch)
    Taken,        // taken side of a conditional/unconditional branch or intra-function J
    JumpTable,    // one case of a resolved jr table
    TailCall,     // leaves the function (J / branch / fallthrough into another entry)
};

struct Edge {
    u32 target = 0;
    EdgeKind kind = EdgeKind::Fallthrough;
};

enum class Terminator : u8 {
    Fallthrough,   // block ends only because the next address is a leader
    Branch,        // conditional branch (+delay slot)
    Jump,          // unconditional jump/branch within the function
    TailCall,      // unconditional transfer to another function
    Return,        // jr $ra
    JumpTable,     // jr through a resolved table
    IndirectJump,  // jr through an unresolved register
    Eret,
    Break,
    EndOfImage,    // ran off the loaded image
    InvalidInsn,   // undecodable word
};

struct BasicBlock {
    u32 start = 0;
    u32 end = 0;  // exclusive; includes the delay slot of the terminating instruction
    Terminator term = Terminator::Fallthrough;
    u32 termPc = 0;  // address of the terminating instruction (valid unless Fallthrough)
    bool likely = false;  // terminator is branch-likely (delay slot annulled when not taken)
    std::vector<Edge> succs;
};

struct CallSite {
    u32 pc = 0;
    u32 target = 0;       // 0 when indirect
    bool indirect = false;  // jalr
};

struct JumpTable {
    u32 jrPc = 0;
    u32 tableAddr = 0;
    int indexReg = -1;  // original case-index register (before address scaling)
    std::vector<u32> targets;
    bool boundFromCompare = false;  // size came from a preceding sltiu rather than a heuristic
};

struct Function {
    u32 entry = 0;
    std::string name;
    bool nameFromSignature = false;
    bool nameFromSymbol = false;
    std::map<u32, BasicBlock> blocks;  // keyed by block start
    std::vector<CallSite> calls;
    std::vector<JumpTable> jumpTables;
    std::set<u32> tailTargets;
    u32 minAddr = 0;
    u32 maxAddr = 0;  // exclusive
    std::set<u32> insnAddrs;  // every reachable instruction address (incl. delay slots)
    std::vector<std::string> warnings;
};

struct AnalyzeOptions {
    std::vector<u32> entries;                 // roots: boot entry, thread entries, exception vectors...
    std::map<u32, std::string> symbols;       // user-supplied names (override signatures)
    const SignatureDb* signatures = nullptr;
    size_t maxFunctionInsns = 1u << 16;       // safety cap per function
    size_t maxJumpTableEntries = 1024;
};

struct Program {
    std::map<u32, Function> functions;  // keyed by canonical entry
    std::vector<std::string> warnings;

    [[nodiscard]] const Function* find(u32 entry) const;
    [[nodiscard]] size_t blockCount() const;
    [[nodiscard]] size_t instructionCount() const;
};

[[nodiscard]] Program analyze(const CodeImage& image, const AnalyzeOptions& options);

// Graphviz dump of one function's CFG (debug aid).
[[nodiscard]] std::string toDot(const Function& fn);
// Human-readable listing of one function (blocks, edges, disassembly).
[[nodiscard]] std::string dumpFunction(const CodeImage& image, const Function& fn);

// CLI: `n64forge analyze <rom> [options]`. Returns process exit code.
int runAnalyzeCommand(int argc, char** argv);

// CLI: `n64forge recompile <rom> -o <out_dir>`. Returns process exit code.
int runRecompileCommand(int argc, char** argv);

}  // namespace n64forge::recomp
