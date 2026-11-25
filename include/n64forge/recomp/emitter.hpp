#pragma once
// n64forge_recomp emitter — lowers an analyzed Program into compilable C++20.
//
// Output is a self-contained translation unit that can be linked against
// n64forge_recomp_rt. The CLI writes one `.cpp` per function plus a dispatch
// table and a small CMakeLists.txt.
#include "n64forge/recomp/analyzer.hpp"

#include <string>
#include <vector>

namespace n64forge::recomp {

struct EmitOptions {
    std::string namespaceName = "n64forge_recomp_out";
    std::string functionPrefix = "func_";
    // Absolute path to the n64forge source root. Written into the generated
    // CMake project so output directories can live anywhere.
    std::string sourceRoot;
    // If true, the emitter wraps the generated code in an extern "C" block.
    bool externC = false;
};

struct EmittedProgram {
    std::string dispatchHeader;   // includes + forward declarations + dispatch table
    std::vector<std::pair<std::string, std::string>> functionFiles;  // filename -> body
    std::string dispatchTable;    // function pointer table definition
    std::string mainSource;       // native executable entry point
    std::string cmakeLists;       // CMake project that builds the native executable
    std::vector<std::string> errors;  // unsupported ops that make emission non-runnable
};

// Lower every function in `prog` to C++20. Individual function bodies are
// returned separately so the CLI can write one file per function.
[[nodiscard]] EmittedProgram emitProgram(const Program& prog, const CodeImage& image,
                                         const EmitOptions& options = {});

// Lower a single function to a C++20 function body (without surrounding
// namespace/includes). Useful for tests.
[[nodiscard]] std::string emitFunction(const Function& fn, const CodeImage& image,
                                       const EmitOptions& options = {});

} // namespace n64forge::recomp
