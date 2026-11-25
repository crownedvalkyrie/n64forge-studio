#include "n64forge/core/rsp.hpp"
#include <algorithm>
#include <bit>
#if defined(__ARM_NEON) && defined(__aarch64__) && !defined(N64FORGE_FORCE_SCALAR_COP2)
#include <arm_neon.h>
#endif

// Algorithms cross-checked against the ares SISD implementation (ISC);
// see docs/REFERENCES.md for attribution and the license notice.
namespace n64forge {
namespace {

u8 byte(const Rsp::Vreg& v, u32 i) {
    return static_cast<u8>(v[(i & 15) / 2] >> ((i & 1) ? 0 : 8));
}

void setByte(Rsp::Vreg& v, u32 i, u8 value) {
    auto& lane = v[(i & 15) / 2];
    lane = (i & 1) ? (lane & 0xFF00) | value : (lane & 0xFF) | (value << 8);
}

u16 signedClamp(s64 x) {
    return static_cast<u16>(std::clamp<s64>(x, -32768, 32767));
}

s64 wrap48(s64 x) {
    constexpr s64 sign = s64{1} << 47;
    return ((x & ((sign << 1) - 1)) ^ sign) - sign;
}

#if defined(__ARM_NEON) && defined(__aarch64__) && !defined(N64FORGE_FORCE_SCALAR_COP2)
uint16x8_t selectLanes(const Rsp::Vreg& reg, u32 element) {
    if (element >= 8) return vdupq_n_u16(reg[element & 7]);
    const auto raw = vld1q_u16(reg.data());
    if (element < 2) return raw;
    static constexpr auto indices = [] {
        std::array<std::array<u8, 16>, 8> table{};
        for (u32 e = 2; e < 8; ++e) {
            for (u32 i = 0; i < 8; ++i) {
                const auto lane = e < 4 ? (i & 6) | (e & 1) : (i & 4) | (e & 3);
                table[e][i * 2] = lane * 2;
                table[e][i * 2 + 1] = lane * 2 + 1;
            }
        }
        return table;
    }();
    return vreinterpretq_u16_u8(vqtbl1q_u8(vreinterpretq_u8_u16(raw), vld1q_u8(indices[element].data())));
}

constexpr u16 laneBits[] = {1, 2, 4, 8, 16, 32, 64, 128};
uint16x8_t unpackFlags(u16 flags) {
    return vtstq_u16(vdupq_n_u16(flags), vld1q_u16(laneBits));
}

u16 packFlags(uint16x8_t lanes) {
    return vaddvq_u16(vandq_u16(lanes, vld1q_u16(laneBits)));
}
#endif

struct DividerTables {
    std::array<u16, 512> reciprocal{}, squareRoot{};
    DividerTables() {
        reciprocal[0] = 0xFFFF;
        for (u32 i = 0; i < 512; ++i) {
            if (i) reciprocal[i] = (((u64{1} << 34) / (i + 512)) + 1) >> 8;
            const u64 a = (i + 512) >> (i & 1);
            // Find the greatest b with a*b*b < 2^44.
            u64 lo = 1 << 17, hi = 1 << 18;
            while (hi - lo > 1) {
                const u64 b = (hi + lo) / 2;
                if (a * b * b < (u64{1} << 44)) lo = b;
                else hi = b;
            }
            squareRoot[i] = lo >> 1;
        }
    }
};

u32 divide(s32 input, bool sqrt) {
    if (!input) return 0x7FFFFFFF;
    if (input == -32768) return 0xFFFF0000;
    const u32 mask = input < 0 ? 0xFFFFFFFF : 0;
    const u32 magnitude = (static_cast<u32>(input) ^ mask) - (input > -32768 ? mask : 0);
    const u32 shift = std::countl_zero(magnitude);
    const u32 index = ((magnitude << shift) >> 22) & 511;
    static const DividerTables tables;
    const u32 entry = sqrt ? tables.squareRoot[(index & 510) | (shift & 1)] : tables.reciprocal[index];
    return (((0x10000 | entry) << 14) >> ((31 - shift) >> (sqrt ? 1 : 0))) ^ mask;
}

} // namespace

void Rsp::executeCop2(const DecodedInstr& instr) {
    const u32 rs = instr.rs, vt = instr.rt, vs = instr.rd, vd = instr.shamt;
    auto& [vco, vcc, vce] = vectorFlags_;
    if (rs < 16) {
        const u32 e = instr.element;
        auto& v = vregs_[vs];
        const u32 flag = std::min(vs & 3, 2u);
        switch (rs) {
            case 0: writeGpr(vt, static_cast<s16>((byte(v, e) << 8) | byte(v, e + 1))); break;
            case 2: writeGpr(vt, static_cast<s16>(vectorFlags_[flag])); break;
            case 4:
                setByte(v, e, regs_[vt] >> 8);
                if (e != 15) setByte(v, e + 1, regs_[vt]);
                break;
            case 6: vectorFlags_[flag] = regs_[vt] & (flag == 2 ? 0xFF : 0xFFFF); break;
            default: break;
        }
        return;
    }

    const u32 e = instr.vectorElement, op = instr.funct;
    if (op == 0x37 || op == 0x3F) return; // VNOP, VNULL
    Vreg selected;
#if defined(__ARM_NEON) && defined(__aarch64__) && !defined(N64FORGE_FORCE_SCALAR_COP2)
    const auto bVec = selectLanes(vregs_[vt], e);
    vst1q_u16(selected.data(), bVec);
#else
    for (u32 i = 0; i < Vlanes; ++i) {
        const u32 lane = e < 2 ? i : e < 4 ? (i & 6) | (e & 1)
                                   : e < 8 ? (i & 4) | (e & 3) : e & 7;
        selected[i] = vregs_[vt][lane];
    }
#endif
    const auto setLow = [&](u32 i, u16 x) { accum_[i] = (accum_[i] & ~s64{0xFFFF}) | x; };
    if (op >= 0x30 && op <= 0x36) {
        const u32 lane = vs & 7;
        const u16 source = vregs_[vt][e & 7];
        for (u32 i = 0; i < Vlanes; ++i) setLow(i, selected[i]);
        if (op == 0x33) { // VMOV uses the broadcast mapping, unlike the divider.
            vregs_[vd][lane] = selected[lane];
        } else if (op == 0x32 || op == 0x36) {
            divIn_ = source;
            divHigh_ = true;
            vregs_[vd][lane] = divOut_;
        } else {
            const bool low = op == 0x31 || op == 0x35;
            const s32 input = low && divHigh_ ? static_cast<s32>((u32{divIn_} << 16) | source)
                                               : static_cast<s16>(source);
            const u32 result = divide(input, op >= 0x34);
            divHigh_ = false;
            divOut_ = result >> 16;
            vregs_[vd][lane] = result;
        }
        return;
    }

    Vreg result{};
#if defined(__ARM_NEON) && defined(__aarch64__) && !defined(N64FORGE_FORCE_SCALAR_COP2)
    // Widen before doubling: -32768 * -32768 * 2 does not fit in s32.
    // The architectural accumulator wraps at 48 bits, before result saturation.
    if (op < 16 && ((op & 7) < 2 || (op & 7) >= 4)) {
        const auto aVec = vld1q_u16(vregs_[vs].data());
        const auto a = vreinterpretq_s16_u16(aVec);
        const auto b = vreinterpretq_s16_u16(bVec);
        const auto kind = op & 7;
        const auto accumulate = (op & 8) != 0;
        const auto finish = [&](int32x4_t products, u32 lane) {
            auto lo = vmovl_s32(vget_low_s32(products));
            auto hi = vmovl_high_s32(products);
            if (kind == 7) {
                lo = vshlq_n_s64(lo, 16);
                hi = vshlq_n_s64(hi, 16);
            } else if (kind < 2) {
                lo = vshlq_n_s64(lo, 1);
                hi = vshlq_n_s64(hi, 1);
                if (!accumulate) {
                    lo = vaddq_s64(lo, vdupq_n_s64(0x8000));
                    hi = vaddq_s64(hi, vdupq_n_s64(0x8000));
                }
            }
            if (accumulate) {
                lo = vaddq_s64(lo, vld1q_s64(accum_.data() + lane));
                hi = vaddq_s64(hi, vld1q_s64(accum_.data() + lane + 2));
                lo = vshrq_n_s64(vshlq_n_s64(lo, 16), 16);
                hi = vshrq_n_s64(vshlq_n_s64(hi, 16), 16);
            }
            vst1q_s64(accum_.data() + lane, lo);
            vst1q_s64(accum_.data() + lane + 2, hi);
            const auto mid = vcombine_s32(vshrn_n_s64(lo, 16), vshrn_n_s64(hi, 16));
            if (kind == 4 || kind == 6) {
                // Extract low 16 bits separately from the signed middle clamp.
                const auto value = vreinterpret_u16_s16(vmovn_s32(vmovn_high_s64(vmovn_s64(lo), hi)));
                if (!accumulate) return value;
                const auto valid = vmovn_u32(vandq_u32(vcgeq_s32(mid, vdupq_n_s32(-32768)),
                                                      vcleq_s32(mid, vdupq_n_s32(32767))));
                return vbsl_u16(valid, value, vmovn_u32(vcgtq_s32(mid, vdupq_n_s32(32767))));
            }
            if (kind != 1) return vreinterpret_u16_s16(vqmovn_s32(mid));
            const auto overflow = vmovn_u32(vcgtq_s32(mid, vdupq_n_s32(32767)));
            return vorr_u16(vqmovun_s32(mid), overflow);
        };
        int32x4_t loProducts, hiProducts;
        if (kind < 2 || kind == 7) {
            loProducts = vmull_s16(vget_low_s16(a), vget_low_s16(b));
            hiProducts = vmull_high_s16(a, b);
        } else {
            auto low = vmull_u16(vget_low_u16(aVec), vget_low_u16(bVec));
            auto high = vmull_high_u16(aVec, bVec);
            if (kind == 4) {
                low = vshrq_n_u32(low, 16);
                high = vshrq_n_u32(high, 16);
            } else {
                // Reinterpret one multiplicand as signed by subtracting the
                // other multiplicand times 65536 in its negative lanes.
                const auto correction = kind == 5 ? vandq_u16(vcltzq_s16(a), bVec)
                                                   : vandq_u16(vcltzq_s16(b), aVec);
                low = vsubq_u32(low, vshll_n_u16(vget_low_u16(correction), 16));
                high = vsubq_u32(high, vshll_high_n_u16(correction, 16));
            }
            loProducts = vreinterpretq_s32_u32(low);
            hiProducts = vreinterpretq_s32_u32(high);
        }
        const auto lo = finish(loProducts, 0);
        const auto hi = finish(hiProducts, 4);
        vst1q_u16(vregs_[vd].data(), vcombine_u16(lo, hi));
        return;
    }
    if (op == 0x10 || op == 0x11 || op == 0x13 || op == 0x14 || op == 0x15 ||
        (op >= 0x20 && op <= 0x2D)) {
        const auto a = vld1q_u16(vregs_[vs].data());
        const auto b = bVec;
        uint16x8_t lowValue, value;
        if (op >= 0x28) {
            lowValue = op < 0x2A ? vandq_u16(a, b) : op < 0x2C ? vorrq_u16(a, b) : veorq_u16(a, b);
            if (op & 1) lowValue = vmvnq_u16(lowValue);
            value = lowValue;
        } else if (op >= 0x20) {
            const auto lhs = vreinterpretq_s16_u16(a), rhs = vreinterpretq_s16_u16(b);
            const auto coLow = unpackFlags(vco), coHigh = unpackFlags(vco >> 8);
            if (op <= 0x23) {
                const auto equal = vceqq_u16(a, b), both = vandq_u16(coLow, coHigh);
                uint16x8_t select;
                switch (op) {
                    case 0x20: select = vorrq_u16(vcltq_s16(lhs, rhs), vandq_u16(equal, both)); break;
                    case 0x21: select = vbicq_u16(equal, coHigh); break;
                    case 0x22: select = vorrq_u16(vmvnq_u16(equal), coHigh); break;
                    default: select = vorrq_u16(vcgtq_s16(lhs, rhs), vbicq_u16(equal, both)); break;
                }
                vcc = packFlags(select);
                value = vbslq_u16(select, a, b);
            } else if (op == 0x24) { // VCL updates only lanes not latched by VCH.
                const auto sum = vaddq_u16(a, b);
                const auto noCarry = vcgeq_u16(sum, a);
                const auto zero = vceqzq_u16(sum);
                const auto ext = unpackFlags(vce);
                const auto select = vbslq_u16(ext, vorrq_u16(zero, noCarry), vandq_u16(zero, noCarry));
                const auto lo = vbslq_u16(vbicq_u16(coLow, coHigh), select, unpackFlags(vcc));
                const auto hi = vbslq_u16(vmvnq_u16(vorrq_u16(coLow, coHigh)),
                                          vcgeq_u16(a, b), unpackFlags(vcc >> 8));
                vcc = packFlags(lo) | (packFlags(hi) << 8);
                value = vbslq_u16(coLow, vbslq_u16(lo, vsubq_u16(vdupq_n_u16(0), b), a), vbslq_u16(hi, b, a));
                vce = 0;
            } else if (op <= 0x26) { // VCH/VCR: sum for opposite signs, difference otherwise.
                const auto negative = vcltzq_s16(rhs);
                const auto opposite = vcltzq_s16(vreinterpretq_s16_u16(veorq_u16(a, b)));
                const auto sum = vaddq_s16(lhs, rhs);
                const auto diff = vsubq_s16(lhs, rhs);
                const auto lo = vbslq_u16(opposite, op == 0x25 ? vclezq_s16(sum) : vcltzq_s16(sum), negative);
                const auto hi = vbslq_u16(opposite, negative, vcgezq_s16(diff));
                vcc = packFlags(lo) | (packFlags(hi) << 8);
                value = vbslq_u16(opposite, vbslq_u16(lo, op == 0x25 ? vsubq_u16(vdupq_n_u16(0), b) : vmvnq_u16(b), a),
                                   vbslq_u16(hi, b, a));
                if (op == 0x25) {
                    const auto nonzero = vmvnq_u16(vbslq_u16(opposite, vceqzq_s16(sum), vceqzq_s16(diff)));
                    vco = packFlags(opposite) | (packFlags(vbicq_u16(nonzero, vceqq_u16(a, vmvnq_u16(b)))) << 8);
                    vce = packFlags(vandq_u16(opposite, vceqq_s16(sum, vdupq_n_s16(-1))));
                } else {
                    vce = 0;
                }
            } else {
                value = vbslq_u16(unpackFlags(vcc), a, b);
            }
            if (op != 0x25) vco = 0;
            lowValue = value;
        } else if (op == 0x14 || op == 0x15) {
            const bool add = op == 0x14;
            lowValue = value = add ? vaddq_u16(a, b) : vsubq_u16(a, b);
            vco = packFlags(add ? vcltq_u16(value, a) : vcltq_u16(a, b));
            if (!add) vco |= packFlags(vmvnq_u16(vceqq_u16(a, b))) << 8;
        } else if (op == 0x13) {
            const auto signs = vreinterpretq_s16_u16(a);
            const auto src = vreinterpretq_s16_u16(b);
            const auto negative = vcltzq_s16(signs), nonzero = vmvnq_u16(vceqzq_u16(a));
            lowValue = vandq_u16(nonzero, vbslq_u16(negative, vreinterpretq_u16_s16(vnegq_s16(src)), b));
            value = vandq_u16(nonzero, vbslq_u16(negative, vreinterpretq_u16_s16(vqnegq_s16(src)), b));
        } else if (!(vco & 0xFF)) {
            const auto lhs = vreinterpretq_s16_u16(a), rhs = vreinterpretq_s16_u16(b);
            lowValue = op == 0x10 ? vaddq_u16(a, b) : vsubq_u16(a, b);
            value = vreinterpretq_u16_s16(op == 0x10 ? vqaddq_s16(lhs, rhs) : vqsubq_s16(lhs, rhs));
            vco = 0;
        } else {
            const auto carry = vshrq_n_u16(unpackFlags(vco), 15);
            const auto lhs = vreinterpretq_s16_u16(a), rhs = vreinterpretq_s16_u16(b);
            const bool add = op == 0x10;
            const auto lo = add ? vaddw_s16(vaddl_s16(vget_low_s16(lhs), vget_low_s16(rhs)),
                                           vget_low_s16(vreinterpretq_s16_u16(carry)))
                                : vsubw_s16(vsubl_s16(vget_low_s16(lhs), vget_low_s16(rhs)),
                                           vget_low_s16(vreinterpretq_s16_u16(carry)));
            const auto hi = add ? vaddw_high_s16(vaddl_high_s16(lhs, rhs), vreinterpretq_s16_u16(carry))
                                : vsubw_high_s16(vsubl_high_s16(lhs, rhs), vreinterpretq_s16_u16(carry));
            lowValue = vreinterpretq_u16_s16(vmovn_high_s32(vmovn_s32(lo), hi));
            value = vreinterpretq_u16_s16(vqmovn_high_s32(vqmovn_s32(lo), hi));
            vco = 0;
        }
        vst1q_u16(vregs_[vd].data(), value);
        const auto replaceLow = [&](uint32x4_t lanes, u32 lane) {
            const auto low = vmovl_u32(vget_low_u32(lanes));
            const auto high = vmovl_high_u32(lanes);
            const auto mask = vdupq_n_u64(0xFFFF);
            for (u32 pair = 0; pair < 2; ++pair) {
                auto* dst = accum_.data() + lane + pair * 2;
                const auto acc = vreinterpretq_u64_s64(vld1q_s64(dst));
                vst1q_s64(dst, vreinterpretq_s64_u64(vbslq_u64(mask, pair ? high : low, acc)));
            }
        };
        replaceLow(vmovl_u16(vget_low_u16(lowValue)), 0);
        replaceLow(vmovl_high_u16(lowValue), 4);
        return;
    }
#endif
    for (u32 i = 0; i < Vlanes; ++i) {
        const u32 a = vregs_[vs][i], b = selected[i];
        const s32 sa = static_cast<s16>(a), sb = static_cast<s16>(b);
        auto& acc = accum_[i];
        const u32 bit = 1u << i, high = bit << 8;
        const auto flag = [](u16& f, u32 mask, bool on) {
            f = on ? f | mask : f & ~mask;
            return on;
        };
        if (op < 16) {
            const auto lowClamp = [&] {
                const s64 mid = acc >> 16;
                return mid < -32768 ? 0 : mid > 32767 ? 0xFFFF : static_cast<u16>(acc);
            };
            const auto product = [&](s64 n) { acc = wrap48((op & 8 ? acc : 0) + n); };
            switch (op) {
                case 0: case 1: case 8: case 9: { // VMULF/U, VMACF/U
                    product(s64{sa} * sb * 2 + (op < 8 ? 0x8000 : 0));
                    const s64 mid = acc >> 16;
                    result[i] = !(op & 1) ? signedClamp(mid) :
                                mid < 0 ? 0 : mid > 32767 ? 0xFFFF : static_cast<u16>(mid);
                    break;
                }
                case 2: case 10: // VRNDP/N
                    if ((acc < 0) == (op == 10)) acc = wrap48(acc + s64{sb} * (vs & 1 ? 65536 : 1));
                    result[i] = signedClamp(acc >> 16);
                    break;
                case 3: case 11: { // VMULQ, VMACQ
                    s32 p = op == 3 ? sa * sb : static_cast<s32>(acc >> 16);
                    if (op == 3) {
                        if (p < 0) p += 31;
                    } else if (!(p & 32)) {
                        if (p < 0) p += 32;
                        else if (p >= 32) p -= 32;
                    }
                    acc = s64{p} * 65536 + (op == 11 ? static_cast<u16>(acc) : 0);
                    result[i] = signedClamp(p >> 1) & 0xFFF0;
                    break;
                }
                case 4: case 12: // VMUDL / VMADL
                    product((a * b) >> 16);
                    result[i] = lowClamp();
                    break;
                case 5: case 13: // VMUDM / VMADM
                    product(s64{sa} * b);
                    result[i] = signedClamp(acc >> 16);
                    break;
                case 6: case 14: // VMUDN / VMADN
                    product(s64{a} * sb);
                    result[i] = op == 6 ? static_cast<u16>(acc) : lowClamp();
                    break;
                case 7: case 15: // VMUDH / VMADH
                    product(s64{sa} * sb * 65536);
                    result[i] = signedClamp(acc >> 16);
                    break;
            }
            continue;
        }
        if (op == 0x1D) { // VSAR
            if (e >= 8 && e <= 10) result[i] = acc >> (16 * (10 - e));
            continue;
        }

        s32 low = 0;
        switch (op) {
            case 0x10: low = sa + sb + bool(vco & bit); break;
            case 0x11: low = sa - sb - bool(vco & bit); break;
            case 0x13: low = sa == 0 ? 0 : sa < 0 ? -sb : sb; break;
            case 0x14: // VADDC
                low = a + b;
                flag(vco, bit, low > 0xFFFF);
                flag(vco, high, false);
                break;
            case 0x15: // VSUBC
                low = static_cast<s32>(a) - static_cast<s32>(b);
                flag(vco, bit, a < b);
                flag(vco, high, a != b);
                break;
            case 0x20: case 0x21: case 0x22: case 0x23: {
                const bool equalCarry = (vco & bit) && (vco & high);
                const bool select = op == 0x20 ? sa < sb || (sa == sb && equalCarry) :
                                    op == 0x21 ? sa == sb && !(vco & high) :
                                    op == 0x22 ? sa != sb || (vco & high) :
                                                 sa > sb || (sa == sb && !equalCarry);
                flag(vcc, high, false);
                low = flag(vcc, bit, select) ? a : b;
                break;
            }
            case 0x24: // VCL
                if (vco & bit) {
                    if (!(vco & high)) {
                        const u32 sum = a + b;
                        flag(vcc, bit, (vce & bit) ? !(sum & 0xFFFF) || sum <= 0xFFFF : sum == 0);
                    }
                    low = vcc & bit ? -sb : sa;
                } else {
                    if (!(vco & high)) flag(vcc, high, a >= b);
                    low = vcc & high ? sb : sa;
                }
                break;
            case 0x25: { // VCH
                const bool opposite = (sa < 0) != (sb < 0);
                const s32 compare = opposite ? sa + sb : sa - sb;
                flag(vco, bit, opposite);
                flag(vco, high, compare != 0 && a != (b ^ 0xFFFF));
                flag(vce, bit, opposite && compare == -1);
                if (opposite) {
                    flag(vcc, high, sb < 0);
                    low = flag(vcc, bit, compare <= 0) ? -sb : sa;
                } else {
                    flag(vcc, bit, sb < 0);
                    low = flag(vcc, high, compare >= 0) ? sb : sa;
                }
                break;
            }
            case 0x26: // VCR
                if ((sa < 0) != (sb < 0)) {
                    flag(vcc, high, sb < 0);
                    low = flag(vcc, bit, sa + sb + 1 <= 0) ? ~sb : sa;
                } else {
                    flag(vcc, bit, sb < 0);
                    low = flag(vcc, high, sa >= sb) ? sb : sa;
                }
                break;
            case 0x27: low = vcc & bit ? a : b; break;
            case 0x28: low = a & b; break;
            case 0x29: low = ~(a & b); break;
            case 0x2A: low = a | b; break;
            case 0x2B: low = ~(a | b); break;
            case 0x2C: low = a ^ b; break;
            case 0x2D: low = ~(a ^ b); break;
            default: // VZERO aliases write zero to VD but retain the sum in ACC_LO.
                setLow(i, sa + sb);
                continue;
        }
        setLow(i, low);
        result[i] = op == 0x10 || op == 0x11 || op == 0x13 ? signedClamp(low) : static_cast<u16>(low);
    }
    if (op == 0x10 || op == 0x11 || (op >= 0x20 && op <= 0x27 && op != 0x25)) vco = 0;
    if (op == 0x24 || op == 0x26) vce = 0;
    vregs_[vd] = result;
}

void Rsp::vectorMemory(const DecodedInstr& instr, bool store) {
    const u32 op = instr.rd, e = instr.element, vt = instr.rt;
    if (op > 11 || (!store && op == 10)) return; // LWV is not decoded on N64.
    constexpr u32 scale[] = {1, 2, 4, 8, 16, 16, 8, 8, 16, 16, 16, 16};
    const u32 addr = regs_[instr.rs] + static_cast<u32>(instr.vectorOffset) * scale[op];
    const u32 base8 = addr & ~7u, lo8 = addr & 7, lo16 = addr & 15;
    auto& v = vregs_[vt];
    const auto read = [&](u32 a) { return dmem_[a & 0xFFF]; };
    const auto write = [&](u32 a, u8 data) { dmem_[a & 0xFFF] = data; };

#if defined(__ARM_NEON) && defined(__aarch64__) && !defined(N64FORGE_FORCE_SCALAR_COP2)
    // Aligned LQV/SQV with element zero transfer all sixteen bytes, with no
    // truncation, element rotation or DMEM wrap possible.
    if (op == 4 && e == 0 && lo16 == 0) {
        auto* bytes = dmem_.data() + (addr & 0xFFF);
        if (store) vst1q_u8(bytes, vrev16q_u8(vreinterpretq_u8_u16(vld1q_u16(v.data()))));
        else vst1q_u16(v.data(), vreinterpretq_u16_u8(vrev16q_u8(vld1q_u8(bytes))));
        return;
    }
#endif
    if (op <= 4) {
        const u32 size = op == 4 ? 16 - lo16 : scale[op];
        for (u32 i = 0; i < size; ++i) {
            if (store) write(addr + i, byte(v, e + i));
            else if (e + i < 16) setByte(v, e + i, read(addr + i));
        }
    } else if (op == 5) {
        const u32 base = addr & ~15u;
        if (store) {
            for (u32 i = 0; i < lo16; ++i) write(base + i, byte(v, e + 16 - lo16 + i));
        } else if (lo16 > e) {
            for (u32 i = 0; i < lo16 - e; ++i) setByte(v, 16 - lo16 + e + i, read(base + i));
        }
    } else if (op == 11) {
        const u32 group = vt & ~7u;
        if (store) {
            const u32 element = e & ~1u;
            for (u32 i = 0; i < 16; ++i)
                write(base8 + ((lo8 - element + i) & 15), byte(vregs_[group + i / 2], i - element));
        } else {
            for (u32 i = 0; i < 16; ++i)
                setByte(vregs_[group + ((e / 2 + i / 2) & 7)], i, read(base8 + ((e + (addr & 8) + i) & 15)));
        }
    } else if (!store) {
        Vreg tmp{};
        for (u32 i = 0; i < Vlanes; ++i) {
            const u32 stride = op == 8 ? 2 : op == 9 ? 4 : 1;
            const u32 index = op == 9 ? (i & 3) * 4 + ((i & 4) << 1) : i * stride;
            tmp[i] = read(base8 + ((lo8 - e + index) & 15)) << (op == 6 ? 8 : 7);
        }
        if (op != 9) v = tmp;
        else for (u32 i = e; i < std::min(e + 8, 16u); ++i) setByte(v, i, byte(tmp, i));
    } else if (op == 6 || op == 7) {
        for (u32 i = 0; i < Vlanes; ++i) {
            const u32 j = (e + i) & 15;
            write(addr + i, v[j & 7] >> (((j < 8) == (op == 6)) ? 8 : 7));
        }
    } else if (op == 8) {
        for (u32 i = 0; i < Vlanes; ++i)
            write(base8 + ((lo8 + 2 * i) & 15), (byte(v, e + 2 * i) << 1) | (byte(v, e + 2 * i + 1) >> 7));
    } else if (op == 9) {
        constexpr int first[] = {0, 6, -1, -1, 1, 7, -1, -1, 4, -1, -1, 3, 5, -1, -1, 0};
        for (u32 i = 0; i < 4; ++i) {
            const int lane = first[e];
            write(base8 + ((lo8 + 4 * i) & 15),
                  lane < 0 ? 0 : v[(lane & 4) | ((lane + i) & 3)] >> 7);
        }
    } else { // SWV
        for (u32 i = 0; i < 16; ++i) write(base8 + ((lo8 + i) & 15), byte(v, e + i));
    }
}

} // namespace n64forge
