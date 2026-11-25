#pragma once

#include "n64forge/core/types.hpp"

#include <atomic>
#include <memory>

namespace n64forge {
class Memory;

// Native code is optional. The platform adapter owns executable memory; the
// guest CPU owns timing, interrupt checks and address translation.
class Arm64Jit {
public:
    // The native entry receives a pointer to this host-owned context. Chained
    // blocks keep the instruction budget and retired count in callee-saved
    // registers and publish `retired`/`pc` only when they return to C++.
    struct Context {
        u64* gpr;
        u64* hi;
        u64* lo;
        u64* fpr;
        u32* fcsr;
        u32 budget;
        u32 retired;
        u64 pc;
        u32 cu1;
        u32 mode;
        u8* rdram;
        std::atomic<u32>* lineGen;
        const u8* lineActive;
        const void* dispatch;
    };
    struct Result {
        u32 retired = 0;
        u64 pc = 0;
    };

    static std::unique_ptr<Arm64Jit> create();
    ~Arm64Jit();

    // `pc` is the canonical virtual PC. Execution stays within direct-fetch
    // KSEG0/KSEG1 code; the JIT converts to physical RDRAM itself. `lineGen`
    // is the CPU's per-64-byte-line generation table, used to reject stale
    // chained targets without touching the interpreter cache.
    Result run(const Memory& ram, u64 pc, u64 epoch,
               const std::atomic<u32>* lineGen, u32 budget, u64* gpr, u64* hi,
               u64* lo, u64* fpr, u32* fcsr, bool cop1, bool fullFpr) noexcept;

    // Called from the RDRAM write observer. Only code-bearing lines unlink the
    // compiled graph; ordinary data stores stay off the slow path.
    void invalidateCode(u32 physicalAddress, u32 size) noexcept;
    // A bus/reset or generation-epoch change drops every chained edge.
    void invalidateAll() noexcept;

    struct State;

private:
    Arm64Jit();
    std::unique_ptr<State> state_;
};
} // namespace n64forge
