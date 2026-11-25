#pragma once

#include "n64forge/core/types.hpp"

#include <atomic>
#include <memory>

namespace n64forge {
class Bus;
class Cpu;
class Memory;

// Native x64 recompiler for canonical direct-RDRAM guest code. It mirrors the
// ARM64 JIT contract: host-owned instruction budget, exact continuation PC on
// bails, generation-checked invalidation, and a scalar-interpreter fallback for
// anything outside the translated subset.
class X64Jit {
public:
    // The generated blocks keep the budget and retired count in host registers;
    // the context is only written on entry and on return to C++. Fields after
    // `dispatch` are x64-only and carry the scalar step callback state.
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
        void* cpu;
        void* bus;
        u64 currentPc;
        u64 epoch;
        void* jitState;
    };

    struct Result {
        u32 retired = 0;
        u64 pc = 0;
    };

    static std::unique_ptr<X64Jit> create();
    ~X64Jit();

    [[nodiscard]] Result run(const Memory& ram, u64 pc, u64 epoch,
                             const std::atomic<u32>* lineGen, u32 budget, u64* gpr, u64* hi,
                             u64* lo, u64* fpr, u32* fcsr, bool cop1, bool fullFpr,
                             Cpu* cpu = nullptr, Bus* bus = nullptr) noexcept;

    // Called from the RDRAM write observer. Only code-bearing lines unlink the
    // compiled graph; ordinary data stores stay off the slow path.
    void invalidateCode(u32 physicalAddress, u32 size) noexcept;
    void invalidateAll() noexcept;

    struct State;

private:
    X64Jit();
    std::unique_ptr<State> state_;
};

} // namespace n64forge
