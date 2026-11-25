// Bit-exact COP2 golden tests. The identical instruction stream is executed
// by the normal core (NEON on Apple Silicon) and by a second binary built with
// N64FORGE_FORCE_SCALAR_COP2. Both must produce the exact same observable
// vector state, proving the NEON and scalar paths are equivalent.
#include "n64forge/core/rsp.hpp"
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace n64forge;

namespace {

struct Snapshot {
    std::string label;
    std::array<Rsp::Vreg, 4> vregs;
    std::array<std::int64_t, 8> accum;
    std::array<std::uint16_t, 3> flags;
    std::array<std::uint16_t, 3> divider;
};

std::string hex16(std::uint16_t v) {
    char buf[8];
    std::snprintf(buf, sizeof(buf), "%04X", v);
    return buf;
}

std::string serialize(const Snapshot& s) {
    std::string out = s.label + ":";
    for (const auto& r : s.vregs)
        for (auto v : r) out += hex16(v) + ",";
    for (auto v : s.accum) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%lld,", static_cast<long long>(v));
        out += buf;
    }
    for (auto v : s.flags) out += hex16(v) + ",";
    for (auto v : s.divider) out += hex16(v) + ",";
    return out;
}

Snapshot capture(Rsp& rsp, const char* label) {
    Snapshot s;
    s.label = label;
    for (u32 i = 0; i < 4; ++i) s.vregs[i] = rsp.vreg(i);
    s.accum = rsp.accum();
    s.flags = rsp.vectorFlags();
    s.divider = rsp.dividerState();
    return s;
}

// Encodes a COP2 vector ALU instruction in the same layout used by the core
// decoder: bits 21-24 = element, 16-20 = VT, 11-15 = VS, 6-10 = VD, 0-5 = op.
std::uint32_t alu(std::uint32_t op, std::uint32_t vd, std::uint32_t vs,
                  std::uint32_t vt, std::uint32_t e = 0) {
    return 0x4A000000u | (e << 21) | (vt << 16) | (vs << 11) | (vd << 6) | op;
}

std::vector<Snapshot> runStream() {
    Rsp rsp;
    std::vector<Snapshot> snaps;
    const auto run = [&](std::uint32_t instr) {
        rsp.writeMem32(0x1000 + rsp.pc(), instr);
        rsp.step();
    };

    // Deterministic seeds covering positive, negative, zero, and extreme lanes.
    const Rsp::Vreg A{0x0001, 0x7FFF, 0x8000, 0xFFFF, 0x1234, 0x8001, 0x4000, 0x0000};
    const Rsp::Vreg B{0xFFFF, 0x0002, 0x7FFF, 0x8000, 0x1111, 0x7FFE, 0xC000, 0x8000};

    // --- NEON-covered multiply/MAC family ---
    rsp.setVreg(1, A);
    rsp.setVreg(2, B);
    run(alu(0x00, 3, 1, 2)); snaps.push_back(capture(rsp, "VMULF"));
    run(alu(0x01, 3, 1, 2)); snaps.push_back(capture(rsp, "VMULU"));
    run(alu(0x07, 3, 1, 2)); snaps.push_back(capture(rsp, "VMUDH"));
    run(alu(0x08, 3, 1, 2)); snaps.push_back(capture(rsp, "VMACF"));
    run(alu(0x09, 3, 1, 2)); snaps.push_back(capture(rsp, "VMACU"));
    run(alu(0x0F, 3, 1, 2)); snaps.push_back(capture(rsp, "VMADH"));

    // Element broadcast and aliasing (VD == VS == VT).
    rsp.setVreg(6, A);
    run(alu(0x00, 6, 6, 6, 15)); snaps.push_back(capture(rsp, "VMULF-alias"));
    run(alu(0x0F, 6, 6, 6, 8)); snaps.push_back(capture(rsp, "VMADH-alias"));

    // --- NEON-covered bitwise family ---
    rsp.setVreg(1, A);
    rsp.setVreg(2, B);
    for (u32 op = 0x28; op <= 0x2D; ++op) {
        char label[32];
        std::snprintf(label, sizeof(label), "bitwise-%02X", op);
        run(alu(op, 3, 1, 2)); snaps.push_back(capture(rsp, label));
    }
    run(alu(0x2D, 1, 1, 2)); snaps.push_back(capture(rsp, "VNXOR-alias"));

    // --- Remaining arithmetic and flag paths ---
    rsp.setVreg(1, A);
    rsp.setVreg(2, B);
    run(alu(0x02, 3, 1, 2)); snaps.push_back(capture(rsp, "VRNDP"));
    run(alu(0x03, 3, 1, 2)); snaps.push_back(capture(rsp, "VMULQ"));
    run(alu(0x04, 3, 1, 2)); snaps.push_back(capture(rsp, "VMUDL"));
    run(alu(0x05, 3, 1, 2)); snaps.push_back(capture(rsp, "VMUDM"));
    run(alu(0x06, 3, 1, 2)); snaps.push_back(capture(rsp, "VMUDN"));
    run(alu(0x10, 3, 1, 2)); snaps.push_back(capture(rsp, "VADD"));
    run(alu(0x11, 3, 1, 2)); snaps.push_back(capture(rsp, "VSUB"));
    run(alu(0x13, 3, 1, 2)); snaps.push_back(capture(rsp, "VABS"));
    run(alu(0x14, 3, 1, 2)); snaps.push_back(capture(rsp, "VADDC"));
    run(alu(0x15, 3, 1, 2)); snaps.push_back(capture(rsp, "VSUBC"));
    run(alu(0x20, 3, 1, 2)); snaps.push_back(capture(rsp, "VLT"));
    run(alu(0x21, 3, 1, 2)); snaps.push_back(capture(rsp, "VEQ"));
    run(alu(0x22, 3, 1, 2)); snaps.push_back(capture(rsp, "VNE"));
    run(alu(0x23, 3, 1, 2)); snaps.push_back(capture(rsp, "VGT"));
    run(alu(0x24, 3, 1, 2)); snaps.push_back(capture(rsp, "VCL"));
    run(alu(0x25, 3, 1, 2)); snaps.push_back(capture(rsp, "VCH"));
    run(alu(0x26, 3, 1, 2)); snaps.push_back(capture(rsp, "VCR"));
    run(alu(0x27, 3, 1, 2)); snaps.push_back(capture(rsp, "VMRG"));
    run(alu(0x1D, 4, 0, 0, 8)); snaps.push_back(capture(rsp, "VSAR-HI"));
    run(alu(0x1D, 4, 0, 0, 9)); snaps.push_back(capture(rsp, "VSAR-MID"));
    run(alu(0x1D, 4, 0, 0, 10)); snaps.push_back(capture(rsp, "VSAR-LO"));

    // --- Divider ---
    rsp.setVreg(2, B);
    run(alu(0x30, 3, 0, 2, 8)); snaps.push_back(capture(rsp, "VRCP"));
    run(alu(0x31, 3, 0, 2, 8)); snaps.push_back(capture(rsp, "VRCPL"));
    run(alu(0x32, 3, 1, 2, 11)); snaps.push_back(capture(rsp, "VRCPH"));
    run(alu(0x33, 3, 0, 2, 8)); snaps.push_back(capture(rsp, "VMOV"));
    run(alu(0x34, 3, 0, 2, 8)); snaps.push_back(capture(rsp, "VRSQ"));
    run(alu(0x35, 3, 0, 2, 8)); snaps.push_back(capture(rsp, "VRSQL"));
    run(alu(0x36, 3, 1, 2, 11)); snaps.push_back(capture(rsp, "VRSQH"));

    return snaps;
}

u64 mixedArithmeticDigest() {
    Rsp rsp;
    u32 rng = 0x93A87165;
    u64 digest = 14695981039346656037ull;
    const auto next = [&] {
        rng ^= rng << 13;
        rng ^= rng >> 17;
        rng ^= rng << 5;
        return rng;
    };
    const auto fold = [&](u64 v) {
        // Hash fixed-width values, independent of host byte order.
        for (unsigned byte = 0; byte < 8; ++byte) {
            digest = (digest ^ (v & 0xFF)) * 1099511628211ull;
            v >>= 8;
        }
    };
    const auto run = [&](u32 instr) {
        rsp.writeMem32(0x1000 + rsp.pc(), instr);
        rsp.step();
    };
    constexpr u32 ops[] = {0x04, 0x05, 0x06, 0x0C, 0x0D, 0x0E, 0x10, 0x11, 0x13};
    for (u32 i = 0; i < 4096; ++i) {
        Rsp::Vreg a, b;
        for (u32 lane = 0; lane < 8; ++lane) {
            a[lane] = static_cast<u16>(next());
            b[lane] = static_cast<u16>(next());
        }
        rsp.setVreg(1, a);
        rsp.setVreg(2, b);
        const auto element = i & 15;
        run(alu(0x15, 3, 1, 2, element)); // Seed both halves of VCO.
        const auto dst = 1 + ((i >> 4) % 3);
        run(alu(ops[(i >> 4) % std::size(ops)], dst, 1, 2, element));
        for (const auto v : rsp.vreg(dst)) fold(v);
        for (const auto v : rsp.accum()) fold(static_cast<u64>(v));
        for (const auto v : rsp.vectorFlags()) fold(v);
    }
    return digest;
}

} // namespace

#ifdef N64FORGE_EMIT_COP2_GOLDEN
int main() {
    for (const auto& s : runStream()) std::printf("%s\n", serialize(s).c_str());
    std::fprintf(stderr, "Mixed arithmetic digest: %016llx\n",
                 static_cast<unsigned long long>(mixedArithmeticDigest()));
    return 0;
}
#else
// Embedded golden values, captured from the portable scalar implementation.
// Both the normal and forced-scalar binaries must match these exactly.
#include "rsp_cop2_golden_data.inc"
int main() {
    const auto actual = runStream();
    if (actual.size() != kGolden.size()) {
        std::fprintf(stderr, "snapshot count mismatch: got %zu, expected %zu\n",
                     actual.size(), kGolden.size());
        return 1;
    }
    int failures = 0;
    for (std::size_t i = 0; i < actual.size(); ++i) {
        const auto got = serialize(actual[i]);
        if (got != kGolden[i]) {
            std::fprintf(stderr, "COP2 golden mismatch at %s:\n  got      %s\n  expected %s\n",
                         actual[i].label.c_str(), got.c_str(), kGolden[i].c_str());
            ++failures;
        }
    }
    const auto digest = mixedArithmeticDigest();
    if (digest != 0x9fdbc2f6ab27ec70ull) {
        std::fprintf(stderr, "Mixed arithmetic digest mismatch: %016llx\n",
                     static_cast<unsigned long long>(digest));
        return 1;
    }
    if (failures) {
        std::fprintf(stderr, "%d/%zu COP2 golden comparisons failed\n", failures, actual.size());
        return 1;
    }
    std::printf("All %zu COP2 golden snapshots matched.\n", actual.size());
    return 0;
}
#endif
