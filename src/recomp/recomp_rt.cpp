#include "n64forge/recomp/recomp_rt.hpp"
#include "n64forge/core/wide_product.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <iostream>
#include <mutex>
#include <unordered_set>

namespace n64forge::recomp {

namespace {

[[nodiscard]] u32 phys(u32 vaddr) noexcept {
    return vaddr & 0x1FFFFFFFu;
}

bool inRdram(const RecompContext& ctx, u32 p, size_t bytes) noexcept {
    const size_t offset = p;
    return offset < ctx.rdramSize() && bytes <= ctx.rdramSize() - offset;
}

void warnOnce(u32 p, const char* kind) {
    static std::mutex mutex;
    static std::unordered_set<u32> seen;
    std::lock_guard<std::mutex> lock(mutex);
    if (seen.insert(p).second) {
        std::cerr << "[recomp] " << kind << " to unmapped address 0x" << std::hex
                  << p << std::dec << "\n";
    }
}

} // namespace

RecompContext::RecompContext() {
    rdram = new u8[kMaxRdram]{};
}

RecompContext::~RecompContext() {
    delete[] rdram;
}

void RecompContext::reset() {
    r.fill(0);
    hi = 0;
    lo = 0;
    pc = 0;
}

// ---------------------------------------------------------------------------
// Big-endian loads
// ---------------------------------------------------------------------------
u32 MEM_WU(RecompContext& ctx, u32 vaddr) {
    const u32 p32 = phys(vaddr);
    const size_t p = p32;
    if (inRdram(ctx, p32, sizeof(u32))) {
        const u8* d = ctx.rdram + p;
        return (static_cast<u32>(d[0]) << 24) | (static_cast<u32>(d[1]) << 16) |
               (static_cast<u32>(d[2]) << 8) | d[3];
    }
    if (ctx.bus) return ctx.bus->read32(p32);
    return mmioReadW(ctx, p32);
}

s64 MEM_W(RecompContext& ctx, u32 vaddr) {
    return static_cast<s64>(static_cast<s32>(MEM_WU(ctx, vaddr)));
}

u16 MEM_HU(RecompContext& ctx, u32 vaddr) {
    const u32 p = phys(vaddr);
    if (inRdram(ctx, p, sizeof(u16))) {
        const u8* d = ctx.rdram + p;
        return (static_cast<u16>(d[0]) << 8) | d[1];
    }
    if (ctx.bus) return ctx.bus->read16(p);
    return mmioReadH(ctx, p);
}

s16 MEM_H(RecompContext& ctx, u32 vaddr) {
    return static_cast<s16>(MEM_HU(ctx, vaddr));
}

u8 MEM_BU(RecompContext& ctx, u32 vaddr) {
    const u32 p = phys(vaddr);
    if (inRdram(ctx, p, sizeof(u8))) return ctx.rdram[p];
    if (ctx.bus) return ctx.bus->read8(p);
    return mmioReadB(ctx, p);
}

s8 MEM_B(RecompContext& ctx, u32 vaddr) {
    return static_cast<s8>(MEM_BU(ctx, vaddr));
}

u64 MEM_D(RecompContext& ctx, u32 vaddr) {
    const u32 p = phys(vaddr);
    if (inRdram(ctx, p, sizeof(u64))) {
        const u8* d = ctx.rdram + p;
        return (static_cast<u64>(d[0]) << 56) | (static_cast<u64>(d[1]) << 48) |
               (static_cast<u64>(d[2]) << 40) | (static_cast<u64>(d[3]) << 32) |
               (static_cast<u64>(d[4]) << 24) | (static_cast<u64>(d[5]) << 16) |
               (static_cast<u64>(d[6]) << 8) | d[7];
    }
    if (ctx.bus) {
        return (static_cast<u64>(ctx.bus->read32(p)) << 32) |
               ctx.bus->read32(p + 4);
    }
    return mmioReadD(ctx, p);
}

// ---------------------------------------------------------------------------
// Big-endian stores
// ---------------------------------------------------------------------------
void MEM_W(RecompContext& ctx, u32 vaddr, u32 value) {
    const u32 p = phys(vaddr);
    if (inRdram(ctx, p, sizeof(u32))) {
        u8* d = ctx.rdram + p;
        d[0] = static_cast<u8>(value >> 24);
        d[1] = static_cast<u8>(value >> 16);
        d[2] = static_cast<u8>(value >> 8);
        d[3] = static_cast<u8>(value);
        return;
    }
    if (ctx.bus) {
        ctx.bus->write32(p, value);
        return;
    }
    mmioWriteW(ctx, p, value);
}

void MEM_H(RecompContext& ctx, u32 vaddr, u16 value) {
    const u32 p = phys(vaddr);
    if (inRdram(ctx, p, sizeof(u16))) {
        u8* d = ctx.rdram + p;
        d[0] = static_cast<u8>(value >> 8);
        d[1] = static_cast<u8>(value);
        return;
    }
    if (ctx.bus) {
        ctx.bus->write16(p, value);
        return;
    }
    mmioWriteH(ctx, p, value);
}

void MEM_B(RecompContext& ctx, u32 vaddr, u8 value) {
    const u32 p = phys(vaddr);
    if (inRdram(ctx, p, sizeof(u8))) {
        ctx.rdram[p] = value;
        return;
    }
    if (ctx.bus) {
        ctx.bus->write8(p, value);
        return;
    }
    mmioWriteB(ctx, p, value);
}

void MEM_D(RecompContext& ctx, u32 vaddr, u64 value) {
    const u32 p = phys(vaddr);
    if (inRdram(ctx, p, sizeof(u64))) {
        u8* d = ctx.rdram + p;
        d[0] = static_cast<u8>(value >> 56);
        d[1] = static_cast<u8>(value >> 48);
        d[2] = static_cast<u8>(value >> 40);
        d[3] = static_cast<u8>(value >> 32);
        d[4] = static_cast<u8>(value >> 24);
        d[5] = static_cast<u8>(value >> 16);
        d[6] = static_cast<u8>(value >> 8);
        d[7] = static_cast<u8>(value);
        return;
    }
    if (ctx.bus) {
        ctx.bus->write32(p, static_cast<u32>(value >> 32));
        ctx.bus->write32(p + 4, static_cast<u32>(value));
        return;
    }
    mmioWriteD(ctx, p, value);
}

// ---------------------------------------------------------------------------
// LWL / LWR / SWL / SWR
// ---------------------------------------------------------------------------
u32 LWL(RecompContext& ctx, u32 vaddr, u32 old) {
    const u32 p = phys(vaddr);
    const u32 aligned = p & ~3u;
    const u32 shift = (p & 3u) * 8;
    const u32 word = MEM_WU(ctx, aligned | (vaddr & 0xE0000000u)); // keep segment bits
    const u32 mask = 0xFFFFFFFFu << shift;
    return (old & ~mask) | (word << shift);
}

u32 LWR(RecompContext& ctx, u32 vaddr, u32 old) {
    const u32 p = phys(vaddr);
    const u32 aligned = p & ~3u;
    const u32 shift = (3u - (p & 3u)) * 8;
    const u32 word = MEM_WU(ctx, aligned | (vaddr & 0xE0000000u));
    const u32 mask = 0xFFFFFFFFu >> shift;
    return (old & ~mask) | (word >> shift);
}

void SWL(RecompContext& ctx, u32 vaddr, u32 value) {
    const u32 p = phys(vaddr);
    const u32 aligned = p & ~3u;
    const u32 o = p & 3u;
    const u32 old = MEM_WU(ctx, aligned | (vaddr & 0xE0000000u));
    const u32 keepMask = o == 0 ? 0u : (0xFFFFFFFFu << ((4u - o) * 8));
    const u32 nv = (old & keepMask) | ((value >> (o * 8)) & ~keepMask);
    MEM_W(ctx, aligned | (vaddr & 0xE0000000u), nv);
}

void SWR(RecompContext& ctx, u32 vaddr, u32 value) {
    const u32 p = phys(vaddr);
    const u32 aligned = p & ~3u;
    const u32 o = p & 3u;
    const u32 old = MEM_WU(ctx, aligned | (vaddr & 0xE0000000u));
    const u32 keepMask = (1u << ((3u - o) * 8)) - 1u;
    const u32 nv = (old & keepMask) | (value & ~keepMask);
    MEM_W(ctx, aligned | (vaddr & 0xE0000000u), nv);
}

// ---------------------------------------------------------------------------
// Wide multiply runtime helpers
// ---------------------------------------------------------------------------
std::pair<u64, u64> DMULT(s64 a, s64 b) noexcept {
    const auto [lo, hi] = multiplyWideSigned(static_cast<u64>(a), static_cast<u64>(b));
    return {lo, hi};
}

std::pair<u64, u64> DMULTU(u64 a, u64 b) noexcept {
    const auto [lo, hi] = multiplyWideUnsigned(a, b);
    return {lo, hi};
}

// ---------------------------------------------------------------------------
// Indirect dispatch fallback
// ---------------------------------------------------------------------------
u32 canonicalIndirectAddress(u32 target_pc) noexcept {
    if ((target_pc & 0xE0000000u) == 0x80000000u ||
        (target_pc & 0xE0000000u) == 0xA0000000u)
        return 0x80000000u | (target_pc & 0x1FFFFFFFu);
    return target_pc;
}

void dispatchIndirect(RecompContext& state, u32 target_pc) {
    state.pc = canonicalIndirectAddress(target_pc);
    if (const auto it = state.dispatch.find(state.pc); it != state.dispatch.end()) {
        it->second(state);
        return;
    }
    if (state.onUnresolvedIndirect) {
        state.onUnresolvedIndirect(state, state.pc);
        return;
    }
    state.indirectDispatchFailed = true;
    warnOnce(state.pc, "indirect dispatch");
}

// ---------------------------------------------------------------------------
// MMIO stubs
// ---------------------------------------------------------------------------
u32 mmioReadW(RecompContext& ctx, u32 p) {
    (void)ctx;
    warnOnce(p, "read32");
    return 0;
}

u64 mmioReadD(RecompContext& ctx, u32 p) {
    (void)ctx;
    warnOnce(p, "read64");
    return 0;
}

u16 mmioReadH(RecompContext& ctx, u32 p) {
    (void)ctx;
    warnOnce(p, "read16");
    return 0;
}

u8 mmioReadB(RecompContext& ctx, u32 p) {
    (void)ctx;
    warnOnce(p, "read8");
    return 0;
}

void mmioWriteW(RecompContext& ctx, u32 p, u32 value) {
    (void)ctx;
    (void)value;
    warnOnce(p, "write32");
}

void mmioWriteD(RecompContext& ctx, u32 p, u64 value) {
    (void)ctx;
    (void)value;
    warnOnce(p, "write64");
}

void mmioWriteH(RecompContext& ctx, u32 p, u16 value) {
    (void)ctx;
    (void)value;
    warnOnce(p, "write16");
}

void mmioWriteB(RecompContext& ctx, u32 p, u8 value) {
    (void)ctx;
    (void)value;
    warnOnce(p, "write8");
}

} // namespace n64forge::recomp
