#include "n64forge/recomp/analyzer.hpp"
#include "n64forge/recomp/emitter.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace n64forge::recomp {
namespace {

bool parseU32(const std::string& s, u32& out) {
    char* end = nullptr;
    const unsigned long long v = std::strtoull(s.c_str(), &end, 0);
    if (s.empty() || *end != '\0' || v > 0xFFFFFFFFull) return false;
    out = static_cast<u32>(v);
    return true;
}

bool readFile(const std::string& path, std::string& out) {
    std::ifstream f(path);
    if (!f) return false;
    std::stringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

void usage() {
    std::cerr <<
        "Usage: n64forge analyze <rom> [options]\n"
        "  --entry <addr>             extra root function (repeatable)\n"
        "  --boot-entry <addr>        override the IPL3 load/entry address (default: header)\n"
        "  --region <romoff,vaddr,size>  map extra ROM bytes (e.g. DMA'd overlays); repeatable\n"
        "  --symbols <file>           lines of `0xADDR name`\n"
        "  --signatures <file>        libultra signature file (see SignatureDb::loadText)\n"
        "  --functions                list every function\n"
        "  --dump <addr>              disassembled CFG of one function\n"
        "  --dot <addr>               Graphviz CFG of one function\n"
        "  --emit-signature <addr,count,name>   print a signature line for a function prefix\n";
}

}  // namespace

int runAnalyzeCommand(int argc, char** argv) {
    if (argc < 1) { usage(); return 1; }
    const std::string romPath = argv[0];
    std::vector<u32> extraEntries;
    std::vector<std::string> regionSpecs;
    std::string symbolsPath, sigPath, emitSig;
    std::optional<u32> bootEntry, dumpAddr, dotAddr;
    bool listFunctions = false;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : std::string(); };
        u32 v = 0;
        if (a == "--entry") { if (!parseU32(next(), v)) { usage(); return 1; } extraEntries.push_back(v); }
        else if (a == "--boot-entry") { if (!parseU32(next(), v)) { usage(); return 1; } bootEntry = v; }
        else if (a == "--region") regionSpecs.push_back(next());
        else if (a == "--symbols") symbolsPath = next();
        else if (a == "--signatures") sigPath = next();
        else if (a == "--functions") listFunctions = true;
        else if (a == "--dump") { if (!parseU32(next(), v)) { usage(); return 1; } dumpAddr = v; }
        else if (a == "--dot") { if (!parseU32(next(), v)) { usage(); return 1; } dotAddr = v; }
        else if (a == "--emit-signature") emitSig = next();
        else { std::cerr << "unknown option: " << a << "\n"; usage(); return 1; }
    }

    Cartridge cart;
    if (!cart.load(romPath)) { std::cerr << "Failed to load ROM: " << romPath << "\n"; return 1; }
    const u32 entry = bootEntry.value_or(cart.header().bootPc);
    CodeImage image = CodeImage::fromCartridgeBoot(cart, entry);

    for (const auto& spec : regionSpecs) {
        std::stringstream ss(spec);
        std::string a, b, c;
        u32 off = 0, va = 0, size = 0;
        if (!std::getline(ss, a, ',') || !std::getline(ss, b, ',') || !std::getline(ss, c, ',') ||
            !parseU32(a, off) || !parseU32(b, va) || !parseU32(c, size) ||
            off >= cart.rom().size()) {
            std::cerr << "bad --region (want romoff,vaddr,size): " << spec << "\n";
            return 1;
        }
        const size_t n = std::min<size_t>(size, cart.rom().size() - off);
        image.addRegion(va, std::vector<u8>(cart.rom().begin() + off, cart.rom().begin() + off + static_cast<std::ptrdiff_t>(n)));
    }

    AnalyzeOptions opt;
    opt.entries.push_back(entry);
    opt.entries.insert(opt.entries.end(), extraEntries.begin(), extraEntries.end());

    if (!symbolsPath.empty()) {
        std::string text;
        if (!readFile(symbolsPath, text)) { std::cerr << "cannot read " << symbolsPath << "\n"; return 1; }
        std::istringstream in(text);
        std::string line;
        while (std::getline(in, line)) {
            if (const auto h = line.find('#'); h != std::string::npos) line.erase(h);
            std::istringstream ls(line);
            std::string addr, name;
            u32 v = 0;
            if (ls >> addr >> name && parseU32(addr, v)) opt.symbols[canonicalAddress(v)] = name;
        }
    }

    SignatureDb sigs;
    if (!sigPath.empty()) {
        std::string text;
        if (!readFile(sigPath, text)) { std::cerr << "cannot read " << sigPath << "\n"; return 1; }
        std::vector<std::string> errs;
        sigs.loadText(text, &errs);
        for (const auto& e : errs) std::cerr << "warning: " << e << "\n";
        opt.signatures = &sigs;
    }

    if (!emitSig.empty()) {
        std::stringstream ss(emitSig);
        std::string a, b, name;
        u32 addr = 0, count = 0;
        if (!std::getline(ss, a, ',') || !std::getline(ss, b, ',') || !std::getline(ss, name) ||
            !parseU32(a, addr) || !parseU32(b, count)) { usage(); return 1; }
        const auto sig = makeSignature(image, addr, count, name);
        if (!sig) { std::cerr << "address range not in loaded image\n"; return 1; }
        std::cout << formatSignature(*sig) << "\n";
        return 0;
    }

    const Program prog = analyze(image, opt);

    if (dumpAddr || dotAddr) {
        const Function* fn = prog.find(dumpAddr ? *dumpAddr : *dotAddr);
        if (!fn) { std::cerr << "no function at that address\n"; return 1; }
        std::cout << (dumpAddr ? dumpFunction(image, *fn) : toDot(*fn));
        return 0;
    }

    size_t named = 0, jtables = 0, calls = 0, warns = prog.warnings.size();
    for (const auto& [_, f] : prog.functions) {
        named += (f.nameFromSignature || f.nameFromSymbol);
        jtables += f.jumpTables.size();
        calls += f.calls.size();
        warns += f.warnings.size();
    }
    std::printf("ROM            : %s (%s)\n", cart.header().name.c_str(), cart.formatName().c_str());
    std::printf("Entry / load   : 0x%08X\n", entry);
    for (const auto& r : image.regions()) {
        std::printf("Region         : 0x%08X + 0x%zX\n", r.vaddr, r.bytes.size());
    }
    std::printf("Functions      : %zu (%zu named by symbol/signature)\n", prog.functions.size(), named);
    std::printf("Basic blocks   : %zu\n", prog.blockCount());
    std::printf("Instructions   : %zu reachable\n", prog.instructionCount());
    std::printf("Call sites     : %zu\n", calls);
    std::printf("Jump tables    : %zu resolved\n", jtables);
    std::printf("Warnings       : %zu\n", warns);
    for (const auto& w : prog.warnings) std::printf("  %s\n", w.c_str());
    for (const auto& [_, f] : prog.functions)
        for (const auto& w : f.warnings) std::printf("  %s\n", w.c_str());
    if (listFunctions) {
        for (const auto& [e, f] : prog.functions) {
            std::printf("  %08X  %-32s %4zu blocks  %5zu insns\n", e, f.name.c_str(), f.blocks.size(),
                        f.insnAddrs.size());
        }
    }
    return 0;
}

namespace {

void recompileUsage() {
    std::cerr <<
        "Usage: n64forge recompile <rom> -o <out_dir> [options]\n"
        "  -o <dir>                   output directory (required)\n"
        "  --entry <addr>             extra root function (repeatable)\n"
        "  --boot-entry <addr>        override the IPL3 load/entry address (default: header)\n"
        "  --region <romoff,vaddr,size>  map extra ROM bytes; repeatable\n"
        "  --symbols <file>           lines of `0xADDR name`\n"
        "  --signatures <file>        libultra signature file\n";
}

} // namespace

int runRecompileCommand(int argc, char** argv) {
    if (argc < 1) { recompileUsage(); return 1; }
    const std::string romPath = argv[0];
    std::string outDir;
    std::vector<u32> extraEntries;
    std::vector<std::string> regionSpecs;
    std::string symbolsPath, sigPath;
    std::optional<u32> bootEntry;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : std::string(); };
        u32 v = 0;
        if (a == "-o") outDir = next();
        else if (a == "--entry") { if (!parseU32(next(), v)) { recompileUsage(); return 1; } extraEntries.push_back(v); }
        else if (a == "--boot-entry") { if (!parseU32(next(), v)) { recompileUsage(); return 1; } bootEntry = v; }
        else if (a == "--region") regionSpecs.push_back(next());
        else if (a == "--symbols") symbolsPath = next();
        else if (a == "--signatures") sigPath = next();
        else { std::cerr << "unknown option: " << a << "\n"; recompileUsage(); return 1; }
    }

    if (outDir.empty()) { std::cerr << "-o <out_dir> is required\n"; recompileUsage(); return 1; }

    Cartridge cart;
    if (!cart.load(romPath)) { std::cerr << "Failed to load ROM: " << romPath << "\n"; return 1; }
    const u32 entry = bootEntry.value_or(cart.header().bootPc);
    CodeImage image = CodeImage::fromCartridgeBoot(cart, entry);

    for (const auto& spec : regionSpecs) {
        std::stringstream ss(spec);
        std::string a, b, c;
        u32 off = 0, va = 0, size = 0;
        if (!std::getline(ss, a, ',') || !std::getline(ss, b, ',') || !std::getline(ss, c, ',') ||
            !parseU32(a, off) || !parseU32(b, va) || !parseU32(c, size) ||
            off >= cart.rom().size()) {
            std::cerr << "bad --region: " << spec << "\n";
            return 1;
        }
        const size_t n = std::min<size_t>(size, cart.rom().size() - off);
        image.addRegion(va, std::vector<u8>(cart.rom().begin() + off, cart.rom().begin() + off + static_cast<std::ptrdiff_t>(n)));
    }

    AnalyzeOptions opt;
    opt.entries.push_back(entry);
    opt.entries.insert(opt.entries.end(), extraEntries.begin(), extraEntries.end());

    if (!symbolsPath.empty()) {
        std::string text;
        if (!readFile(symbolsPath, text)) { std::cerr << "cannot read " << symbolsPath << "\n"; return 1; }
        std::istringstream in(text);
        std::string line;
        while (std::getline(in, line)) {
            if (const auto h = line.find('#'); h != std::string::npos) line.erase(h);
            std::istringstream ls(line);
            std::string addr, name;
            u32 v = 0;
            if (ls >> addr >> name && parseU32(addr, v)) opt.symbols[canonicalAddress(v)] = name;
        }
    }

    SignatureDb sigs;
    if (!sigPath.empty()) {
        std::string text;
        if (!readFile(sigPath, text)) { std::cerr << "cannot read " << sigPath << "\n"; return 1; }
        std::vector<std::string> errs;
        sigs.loadText(text, &errs);
        for (const auto& e : errs) std::cerr << "warning: " << e << "\n";
        opt.signatures = &sigs;
    }

    const Program prog = analyze(image, opt);
    bool analysisFailed = false;
    for (const auto& [addr, fn] : prog.functions) {
        for (const auto& warning : fn.warnings) {
            if (warning.find("unresolved indirect jump") != std::string::npos) {
                // Generated code routes dynamic JR/JALR through
                // dispatchIndirect(), so an unknown static target is not fatal.
                std::cerr << "warning: 0x" << std::hex << addr << std::dec
                          << ": " << warning << " (runtime fallback will be used)\n";
            } else if (warning.find("truncated") != std::string::npos ||
                       warning.find("branch/jump in a delay slot") != std::string::npos) {
                std::cerr << "error: 0x" << std::hex << addr << std::dec << ": " << warning << "\n";
                analysisFailed = true;
            }
        }
    }
    if (analysisFailed) return 1;

    EmitOptions emitOptions;
#ifdef N64FORGE_SOURCE_DIR
    emitOptions.sourceRoot = N64FORGE_SOURCE_DIR;
#endif
    const EmittedProgram emitted = emitProgram(prog, image, emitOptions);
    if (!emitted.errors.empty()) {
        for (const auto& error : emitted.errors) std::cerr << "error: " << error << "\n";
        std::cerr << "Refusing to emit a native executable with unsupported instructions.\n";
        return 1;
    }

    // A generated project is written only to a new or empty directory. Do not
    // replace a user's sources or mix stale translation units with this run.
    std::error_code outputError;
    std::filesystem::create_directories(outDir, outputError);
    if (outputError || !std::filesystem::is_empty(outDir, outputError) || outputError) {
        std::cerr << "Output must be a writable, empty directory: " << outDir << "\n";
        return 1;
    }
    for (const auto& [file, body] : emitted.functionFiles) {
        std::ofstream f(std::filesystem::path(outDir) / file);
        if (!f) { std::cerr << "failed to write " << file << "\n"; return 1; }
        f << body;
    }
    {
        std::ofstream f(std::filesystem::path(outDir) / "dispatch_table.cpp");
        if (!f) { std::cerr << "failed to write dispatch_table.cpp\n"; return 1; }
        f << emitted.dispatchTable;
    }
    {
        std::ofstream f(std::filesystem::path(outDir) / "main.cpp");
        if (!f) { std::cerr << "failed to write main.cpp\n"; return 1; }
        f << emitted.mainSource;
    }
    {
        std::ofstream f(std::filesystem::path(outDir) / "CMakeLists.txt");
        if (!f) { std::cerr << "failed to write CMakeLists.txt\n"; return 1; }
        f << emitted.cmakeLists;
    }

    std::cout << "Wrote " << emitted.functionFiles.size() << " function(s) to " << outDir << "\n";
    return 0;
}

}  // namespace n64forge::recomp
