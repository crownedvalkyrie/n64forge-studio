#pragma once
// n64forge_recomp_rt — runtime context for statically-recompiled N64 code.
//
// The emitter lowers each discovered MIPS function to a plain C++20 function
// `void func_<vaddr>(RecompContext& ctx)`. All state lives in `RecompContext`:
// 64-bit GPRs, HI/LO, PC, and a contiguous RDRAM buffer. MMIO fallbacks go
// through `ctx.bus` when the physical address is outside RDRAM.
#include "n64forge/core/bus.hpp"
#include "n64forge/core/types.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <unordered_map>
#include <utility>

namespace n64forge::recomp {

class RecompContext {
public:
    // RDRAM may be 4 MiB or 8 MiB; we always allocate the maximum and limit
    // accesses through rdramSize_.
    static constexpr size_t kMaxRdram = 8 * 1024 * 1024;

    RecompContext();
    ~RecompContext();

    RecompContext(const RecompContext&) = delete;
    RecompContext& operator=(const RecompContext&) = delete;

    // Reset GPRs/PC/HI/LO but keep RDRAM allocation.
    void reset();

    [[nodiscard]] size_t rdramSize() const noexcept { return rdramSize_; }
    void setRdramSize(size_t size) noexcept { rdramSize_ = (size <= kMaxRdram) ? size : kMaxRdram; }

    [[nodiscard]] bool isRdramAddress(u32 phys) const noexcept { return phys < rdramSize_; }

    std::array<u64, 32> r{};   // GPRs; r[0] is wired to zero on read.
    u64 hi = 0;
    u64 lo = 0;
    u32 pc = 0;

    // COP0 registers (minimal set; exceptions and TLB are runtime stubs).
    std::array<u64, 32> cop0{};

    // COP1 state: 32 64-bit FPRs + FCR0/FCR31.
    std::array<u64, 32> f{};
    u32 fcr0 = 0;
    u32 fcr31 = 0;

    // Contiguous RDRAM buffer. Allocated on construction.
    u8* rdram = nullptr;

    // Optional MMIO fallback. When a memory access is outside RDRAM, the
    // emitter calls `ctx.bus->readX` / `ctx.bus->writeX` if set.
    n64forge::Bus* bus = nullptr;

    // Function dispatch table used for indirect calls / tail calls.
    using FuncPtr = void(*)(RecompContext&);
    std::unordered_map<u32, FuncPtr> dispatch;

    // Runtime fallback for targets that were not discovered statically. The
    // callback may install a new dispatch entry or use an interpreter shim.
    using IndirectFallback = std::function<void(RecompContext&, u32)>;
    IndirectFallback onUnresolvedIndirect;
    bool indirectDispatchFailed = false;

private:
    size_t rdramSize_ = 4 * 1024 * 1024;
};

// ---------------------------------------------------------------------------
// Big-endian memory helpers. `vaddr` is masked with 0x1FFFFFFF to obtain the
// physical address used by N64 KSEG0/KSEG1 aliases. RDRAM accesses are direct
// byte-swapped loads/stores; MMIO goes through ctx.bus when present.
// ---------------------------------------------------------------------------

// A 32-bit load sign-extends to the full 64-bit GPR, so this returns s64 even
// though the guest memory transaction is 32 bits wide.
[[nodiscard]] s64 MEM_W(RecompContext& ctx, u32 vaddr);
[[nodiscard]] u32 MEM_WU(RecompContext& ctx, u32 vaddr);
[[nodiscard]] s16 MEM_H(RecompContext& ctx, u32 vaddr);
[[nodiscard]] u16 MEM_HU(RecompContext& ctx, u32 vaddr);
[[nodiscard]] s8  MEM_B(RecompContext& ctx, u32 vaddr);
[[nodiscard]] u8  MEM_BU(RecompContext& ctx, u32 vaddr);
[[nodiscard]] u64 MEM_D(RecompContext& ctx, u32 vaddr);

void MEM_W(RecompContext& ctx, u32 vaddr, u32 value);
void MEM_H(RecompContext& ctx, u32 vaddr, u16 value);
void MEM_B(RecompContext& ctx, u32 vaddr, u8 value);
void MEM_D(RecompContext& ctx, u32 vaddr, u64 value);

// Unaligned partial-word transfers.
[[nodiscard]] u32 LWL(RecompContext& ctx, u32 vaddr, u32 old);
[[nodiscard]] u32 LWR(RecompContext& ctx, u32 vaddr, u32 old);
void SWL(RecompContext& ctx, u32 vaddr, u32 value);
void SWR(RecompContext& ctx, u32 vaddr, u32 value);

// Portable 64x64 -> 128 helpers. Emitted code uses these instead of __int128
// so generated projects remain valid C++20 on MSVC.
[[nodiscard]] std::pair<u64, u64> DMULT(s64 a, s64 b) noexcept;
[[nodiscard]] std::pair<u64, u64> DMULTU(u64 a, u64 b) noexcept;

// Normalizes KSEG0/KSEG1 aliases to the canonical KSEG0 dispatch key. Other
// addresses are returned unchanged, so unmapped targets stay observable.
[[nodiscard]] u32 canonicalIndirectAddress(u32 target_pc) noexcept;

// Dispatches an indirect JR/JALR or jump-table default. A hit calls the
// recompiled function; a miss invokes `onUnresolvedIndirect` when installed,
// otherwise it records the failure and reports the target once.
void dispatchIndirect(RecompContext& state, u32 target_pc);

// Helpers for MMIO reads/writes when no bus is attached. They return zeros
// and warn once per unique address to keep logs manageable.
u32 mmioReadW(RecompContext& ctx, u32 phys);
u64 mmioReadD(RecompContext& ctx, u32 phys);
u16 mmioReadH(RecompContext& ctx, u32 phys);
u8  mmioReadB(RecompContext& ctx, u32 phys);
void mmioWriteW(RecompContext& ctx, u32 phys, u32 value);
void mmioWriteD(RecompContext& ctx, u32 phys, u64 value);
void mmioWriteH(RecompContext& ctx, u32 phys, u16 value);
void mmioWriteB(RecompContext& ctx, u32 phys, u8 value);

} // namespace n64forge::recomp
