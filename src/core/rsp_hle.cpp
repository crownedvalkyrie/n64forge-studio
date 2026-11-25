#include "n64forge/core/rsp_hle.hpp"

#include "n64forge/core/bus.hpp"
#include "n64forge/core/rdp.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <vector>

namespace n64forge {

namespace {

// SP/MI constants
constexpr u32 SP_STATUS_HALT = 0x0001;
constexpr u32 SP_STATUS_BROKE = 0x0002;
constexpr u32 SP_STATUS_INTR_BREAK = 0x0040;
constexpr u32 SP_STATUS_TASKDONE = 0x0200;
constexpr u32 MI_INTR_SP = 0x01;

[[nodiscard]] s16 readS16Be(const u8* p) {
    return static_cast<s16>((static_cast<u16>(p[0]) << 8) | static_cast<u8>(p[1]));
}

[[nodiscard]] u16 readU16Be(const u8* p) {
    return static_cast<u16>((static_cast<u16>(p[0]) << 8) | static_cast<u8>(p[1]));
}

[[nodiscard]] std::array<float, 16> identityMatrix() {
    return std::array<float, 16>{1, 0, 0, 0,
                                  0, 1, 0, 0,
                                  0, 0, 1, 0,
                                  0, 0, 0, 1};
}

[[nodiscard]] std::array<float, 16> multiply(const std::array<float, 16>& a,
                                             const std::array<float, 16>& b) {
    std::array<float, 16> r{};
    for (int col = 0; col < 4; ++col) {
        for (int row = 0; row < 4; ++row) {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k) {
                sum += a[col * 4 + k] * b[k * 4 + row];
            }
            r[col * 4 + row] = sum;
        }
    }
    return r;
}

[[nodiscard]] std::array<float, 16> loadMtx(const Bus& bus, u32 address) {
    // Mtx is 64 bytes: 16 s16 integers followed by 16 s16 fractions, grouped
    // by column. Convert to a column-major float 4x4 matrix.
    std::array<float, 16> m = identityMatrix();
    for (int col = 0; col < 4; ++col) {
        for (int row = 0; row < 4; ++row) {
            const u32 off = static_cast<u32>(col * 4 + row) * 2;
            const u8 intLo[2] = {bus.read8(address + off + 0), bus.read8(address + off + 1)};
            const u8 fracLo[2] = {bus.read8(address + 32 + off + 0), bus.read8(address + 32 + off + 1)};
            const s16 ip = readS16Be(intLo);
            const u16 fp = readU16Be(fracLo);
            m[col * 4 + row] = static_cast<float>(ip) + static_cast<float>(fp) / 65536.0f;
        }
    }
    return m;
}

constexpr u32 G_ZBUFFER = 0x00000001;
constexpr u32 G_SHADE = 0x00000004;

[[nodiscard]] s32 scaleTexCoord(s16 raw, s32 scale) {
    const s64 product = static_cast<s64>(raw) * scale;
    const s64 shifted = product >= 0 ? product >> 16 : -((-product + 0xFFFF) >> 16);
    return static_cast<s32>(std::clamp<s64>(shifted, std::numeric_limits<s32>::min(),
                                            std::numeric_limits<s32>::max()));
}

[[nodiscard]] s16 inverseW(float w) {
    if (w == 0.0f || !std::isfinite(w)) return 0x7FFF;
    const double value = std::round(32768.0 / static_cast<double>(w));
    return static_cast<s16>(std::clamp<double>(value, -32768.0, 32767.0));
}

[[nodiscard]] s32 toFixed32(double value) {
    const double rounded = std::round(value);
    return static_cast<s32>(std::clamp<double>(rounded, std::numeric_limits<s32>::min(),
                                               std::numeric_limits<s32>::max()));
}

// RDP attribute blocks hold eight words: base/dx/de/dy for four channels, with
// each 32-bit coefficient split across two words at a fixed bit position.
void writeCoefficient(std::array<u64, 8>& words, u32 channel, s32 base, s32 dx, s32 de, s32 dy) {
    const u32 shift = 48 - 16 * channel;
    const auto upper = [](s32 value) { return static_cast<u16>(static_cast<u32>(value) >> 16); };
    const auto lower = [](s32 value) { return static_cast<u16>(static_cast<u32>(value)); };
    words[0] |= static_cast<u64>(upper(base)) << shift;
    words[2] |= static_cast<u64>(lower(base)) << shift;
    words[1] |= static_cast<u64>(upper(dx)) << shift;
    words[3] |= static_cast<u64>(lower(dx)) << shift;
    words[4] |= static_cast<u64>(upper(de)) << shift;
    words[6] |= static_cast<u64>(lower(de)) << shift;
    words[5] |= static_cast<u64>(upper(dy)) << shift;
    words[7] |= static_cast<u64>(lower(dy)) << shift;
}

[[nodiscard]] u64 encodeEdge(double x, double slope) {
    const s32 x16 = toFixed32(x * 65536.0);
    const s32 d16 = toFixed32(slope * 65536.0);
    return (static_cast<u64>(static_cast<u32>(x16 >> 16) & 0xFFF) << 48) |
           (static_cast<u64>(static_cast<u32>(x16) & 0xFFFF) << 32) |
           (static_cast<u64>(static_cast<u32>(d16 >> 16) & 0x3FFF) << 16) |
           (static_cast<u32>(d16) & 0xFFFF);
}

struct Plane { double a = 0, b = 0, c = 0; };

template<class Vertex, class Fn>
[[nodiscard]] Plane fitPlane(const Vertex& v0, const Vertex& v1, const Vertex& v2, Fn value) {
    const double x0 = v0.pos.x, y0 = v0.pos.y, w0 = value(v0);
    const double x1 = v1.pos.x, y1 = v1.pos.y, w1 = value(v1);
    const double x2 = v2.pos.x, y2 = v2.pos.y, w2 = value(v2);
    const double det = x0 * (y1 - y2) + x1 * (y2 - y0) + x2 * (y0 - y1);
    if (std::abs(det) < 1e-9) return {};
    return Plane{
        (w0 * (y1 - y2) + w1 * (y2 - y0) + w2 * (y0 - y1)) / det,
        (x0 * (w1 - w2) + x1 * (w2 - w0) + x2 * (w0 - w1)) / det,
        (x0 * (y1 * w2 - y2 * w1) + x1 * (y2 * w0 - y0 * w2) + x2 * (y0 * w1 - y1 * w0)) / det};
}

} // namespace

RspHle::RspHle(Bus* bus, Rdp* rdp)
    : bus_(bus), rdp_(rdp) {
    resetState();
}

void RspHle::resetState() {
    detectedUcode_ = MicrocodeVersion::Unknown;
    ucodeName_ = "Unknown";
    displayListAddress_ = 0;
    commandsProcessed_ = 0;
    trianglesSubmitted_ = 0;
    verticesLoaded_ = 0;

    gbi_ = GbiVersion::F3DEX;
    segmentBase_.fill(0);
    vertexBuffer_.fill({});

    projectionMatrix_ = identityMatrix();
    modelviewStack_.fill(identityMatrix());
    modelviewIndex_ = 0;
    currentModelview_ = identityMatrix();
    combinedMatrix_ = identityMatrix();

    vpScaleX_ = 160.0f;
    vpScaleY_ = 120.0f;
    vpTransX_ = 160.0f;
    vpTransY_ = 120.0f;

    geometryMode_ = 0;
    fillColor_ = 0;
    envColor_ = 0;
    primColor_ = 0;
    combineMode_ = 0;
    otherModes_ = 0;

    dlStack_.fill(0);
    dlStackIndex_ = -1;
    currentDl_ = 0;

    textureEnabled_ = false;
    textureScaleS_ = 0x10000;
    textureScaleT_ = 0x10000;
    textureTile_ = 0;
    textureLevel_ = 0;
}

void RspHle::detectMicrocode(const OSTask& task) {
    detectedUcode_ = MicrocodeVersion::Unknown;
    ucodeName_ = "Unknown";
    gbi_ = GbiVersion::F3DEX;

    if (task.ucode == 0 || task.ucodeSize == 0) return;

    // Identify the GBI from the display list itself. F3DEX2 renumbered the SP
    // commands, so encountering one of its opcodes is unambiguous and avoids
    // guessing from ucode addresses or ROM identities.
    bool f3dex2 = false;
    if (task.dataPtr != 0) {
        u32 address = task.dataPtr;
        for (u32 i = 0; i < 256; ++i) {
            const u8 op = static_cast<u8>(readCommand(address) >> 56);
            if (op == 0x05 || op == 0x06 || op == 0x07 || op == 0xD7 || op == 0xD8 ||
                op == 0xD9 || op == 0xDA || op == 0xDB || op == 0xDC || op == 0xDE ||
                op == 0xDF || op == 0xE2 || op == 0xE3) {
                f3dex2 = true;
                break;
            }
            if (op == 0xB8 || op == 0xE0) break;
            address += 8;
        }
    }

    if (f3dex2) {
        detectedUcode_ = MicrocodeVersion::F3DEX2;
        gbi_ = GbiVersion::F3DEX2;
        ucodeName_ = "F3DEX2";
    } else {
        detectedUcode_ = MicrocodeVersion::F3DEX;
        gbi_ = GbiVersion::F3DEX;
        ucodeName_ = "F3DEX";
    }
}

u8 RspHle::read8(u32 address) const {
    return bus_ ? bus_->read8(address) : 0;
}

s16 RspHle::readS16(u32 address) const {
    const u8 b0 = read8(address);
    const u8 b1 = read8(address + 1);
    return static_cast<s16>((static_cast<u16>(b0) << 8) | b1);
}

u16 RspHle::readU16(u32 address) const {
    const u8 b0 = read8(address);
    const u8 b1 = read8(address + 1);
    return static_cast<u16>((static_cast<u16>(b0) << 8) | b1);
}

u32 RspHle::readU32(u32 address) const {
    return (static_cast<u32>(read8(address)) << 24) |
           (static_cast<u32>(read8(address + 1)) << 16) |
           (static_cast<u32>(read8(address + 2)) << 8) |
           static_cast<u32>(read8(address + 3));
}

u32 RspHle::resolveAddress(u32 address) const {
    if (address >= 0x80000000) {
        return address & 0x1FFFFFFF;
    }
    const u32 seg = (address >> 24) & 0xF;
    if (seg != 0) {
        return segmentBase_[seg] + (address & 0xFFFFFF);
    }
    return address;
}

u64 RspHle::readCommand(u32 address) const {
    const u32 p = resolveAddress(address);
    const u64 hi = static_cast<u64>(readU32(p));
    const u64 lo = static_cast<u64>(readU32(p + 4));
    return (hi << 32) | lo;
}

bool RspHle::runGraphicsTask(const OSTask& task) {
    if (task.type != 1) {
        return false;
    }

    resetState();
    displayListAddress_ = task.dataPtr;
    detectMicrocode(task);

    if (rdp_) {
        rdp_->setOtherModes(otherModes_);
        rdp_->setCombineMode(combineMode_);
    }
    if (displayListAddress_ != 0) {
        processDisplayList(displayListAddress_, 4096);
    }
    return true;
}

void RspHle::processDisplayList(u32 address, u32 maxCommands) {
    currentDl_ = address;
    dlStackIndex_ = -1;

    for (u32 i = 0; i < maxCommands; ++i) {
        const u64 cmd = readCommand(currentDl_);
        const u8 op = static_cast<u8>(cmd >> 56);
        const u64 w0 = cmd >> 32;            // high 32-bit word
        const u64 w1 = cmd & 0xFFFFFFFFULL;  // low 32-bit word

        ++commandsProcessed_;
        currentDl_ += 8;

        if (op == 0x00 || op == 0xC0) {
            // G_SPNOOP / G_NOOP
            continue;
        }
        if (op == 0xB8 || op == 0xDF) {
            // G_ENDDL (F3DEX / F3DEX2)
            if (dlStackIndex_ >= 0) {
                currentDl_ = dlStack_[dlStackIndex_--];
                continue;
            }
            break;
        }

        switch (gbi_) {
            case GbiVersion::F3DEX:
            case GbiVersion::Fast3D:
                executeF3DEX(op, w0, w1);
                break;
            case GbiVersion::F3DEX2:
                executeF3DEX2(op, w0, w1);
                break;
        }
    }
}

void RspHle::executeF3DEX(u8 op, u64 w0, u64 w1) {
    switch (op) {
        case 0x01: cmdMtx(w0, w1); break;
        case 0x03: cmdMoveMem(w0, w1); break;
        case 0x04: cmdVtx(w0, w1); break;
        case 0x06: cmdDl(w0, w1); break;
        case 0xB1: cmdTri2(w0, w1); break;
        case 0xB6: cmdClearGeometryMode(w1); break;
        case 0xB7: cmdSetGeometryMode(w1); break;
        case 0xB9: cmdRdpCommand(op, w0, w1); break; // G_SETOTHERMODE_L
        case 0xBA: cmdRdpCommand(op, w0, w1); break; // G_SETOTHERMODE_H
        case 0xBB: cmdTexture(w0, w1); break;
        case 0xBC: cmdMoveWord(w0, w1); break;
        case 0xBD: cmdPopMtx(); break;
        case 0xBF: cmdTri1(w0, w1); break;
        case 0xE4: case 0xE5: // G_TEXRECT / G_TEXRECTFLIP
        case 0xED: // G_SETSCISSOR
        case 0xF0: case 0xF2: case 0xF3: case 0xF4: case 0xF5:
        case 0xF6: case 0xF7: case 0xF8: case 0xF9: case 0xFA:
        case 0xFB: case 0xFC: case 0xFD: case 0xFE: case 0xFF:
            cmdRdpCommand(op, w0, w1);
            break;
        default:
            // Unsupported command: log once for diagnostics.
            static bool warned = false;
            if (!warned) {
                std::cerr << "[RspHle] unsupported F3DEX command 0x" << std::hex
                          << static_cast<int>(op) << std::dec << std::endl;
                warned = true;
            }
            break;
    }
}

void RspHle::executeF3DEX2(u8 op, u64 w0, u64 w1) {
    switch (op) {
        case 0x01: cmdVtx(w0, w1); break;
        case 0x05: cmdTri1(w0, w1); break;
        case 0x06: cmdTri2(w0, w1); break;
        case 0x07: cmdQuad(w0, w1); break;
        case 0xD7: cmdTexture(w0, w1); break;
        case 0xD8: cmdPopMtx(); break;
        case 0xD9: cmdGeometryMode(w0, w1); break;
        case 0xDA: cmdMtx(w0, w1); break;
        case 0xDB: cmdMoveWord(w0, w1); break;
        case 0xDC: cmdMoveMem(w0, w1); break;
        case 0xDE: cmdDl(w0, w1); break;
        case 0xE2: case 0xE3:
        case 0xE4: case 0xE5:
        case 0xED:
        case 0xF0: case 0xF2: case 0xF3: case 0xF4: case 0xF5:
        case 0xF6: case 0xF7: case 0xF8: case 0xF9: case 0xFA:
        case 0xFB: case 0xFC: case 0xFD: case 0xFE: case 0xFF:
            cmdRdpCommand(op, w0, w1);
            break;
        default:
            static bool warned = false;
            if (!warned) {
                std::cerr << "[RspHle] unsupported F3DEX2 command 0x" << std::hex
                          << static_cast<int>(op) << std::dec << std::endl;
                warned = true;
            }
            break;
    }
}

void RspHle::cmdMtx(u64 w0, u64 w1) {
    const u32 params = static_cast<u32>(gbi_ == GbiVersion::F3DEX2 ? (w0 & 0xFF)
                                                                   : ((w0 >> 16) & 0xFF));
    const bool load = (params & 0x02) != 0;
    // F3DEX2 moved PUSH to bit 0 (and inverts it in gsSPMatrix); legacy GBI
    // uses bit 2 for PUSH and bit 0 for PROJECTION.
    const bool push = gbi_ == GbiVersion::F3DEX2 ? ((params & 0x01) == 0)
                                                 : ((params & 0x04) != 0);
    const bool projection = gbi_ == GbiVersion::F3DEX2 ? ((params & 0x04) != 0)
                                                       : ((params & 0x01) != 0);

    const u32 address = resolveAddress(static_cast<u32>(w1));
    const auto m = loadMtx(*bus_, address);

    if (projection) {
        if (load) {
            projectionMatrix_ = m;
        } else {
            projectionMatrix_ = multiply(projectionMatrix_, m);
        }
    } else {
        if (push) {
            if (modelviewIndex_ < static_cast<int>(modelviewStack_.size()) - 1) {
                modelviewStack_[++modelviewIndex_] = currentModelview_;
            }
        }
        if (load) {
            currentModelview_ = m;
        } else {
            currentModelview_ = multiply(currentModelview_, m);
        }
    }

    combinedMatrix_ = multiply(projectionMatrix_, currentModelview_);
}

void RspHle::cmdPopMtx() {
    if (modelviewIndex_ > 0) {
        currentModelview_ = modelviewStack_[--modelviewIndex_];
    }
    combinedMatrix_ = multiply(projectionMatrix_, currentModelview_);
}

void RspHle::cmdVtx(u64 w0, u64 w1) {
    u32 n = 0, v0 = 0;
    if (gbi_ == GbiVersion::Fast3D) {
        // Fast3D: (n - 1) in bits 20..23, v0 in bits 16..19.
        n = ((static_cast<u32>(w0) >> 20) & 0xF) + 1;
        v0 = (static_cast<u32>(w0) >> 16) & 0xF;
    } else {
        // F3DEX and F3DEX2: n in bits 12..19, last vertex (v0 + n) in bits 1..7.
        n = (static_cast<u32>(w0) >> 12) & 0xFF;
        const u32 last = (static_cast<u32>(w0) >> 1) & 0x7F;
        v0 = last >= n ? last - n : 0;
    }
    const u32 address = resolveAddress(static_cast<u32>(w1));

    if (n == 0 || n > 32 || v0 >= 32) return;

    for (u32 i = 0; i < n; ++i) {
        const u32 base = address + i * 16;
        Vertex v;
        const s16 x = readS16(base + 0);
        const s16 y = readS16(base + 2);
        const s16 z = readS16(base + 4);
        // flag at base+6 ignored
        v.pos = transformVertex({static_cast<float>(x), static_cast<float>(y),
                                 static_cast<float>(z), 1.0f});
        v.s = readS16(base + 8);
        v.t = readS16(base + 10);
        v.r = read8(base + 12);
        v.g = read8(base + 13);
        v.b = read8(base + 14);
        v.a = read8(base + 15);

        if ((v0 + i) < vertexBuffer_.size()) {
            vertexBuffer_[v0 + i] = v;
        }
        ++verticesLoaded_;
    }
}

RspHle::Vec4 RspHle::transformVertex(const Vec4& v) const {
    const auto& m = combinedMatrix_;
    Vec4 o;
    o.x = m[0] * v.x + m[4] * v.y + m[8] * v.z + m[12] * v.w;
    o.y = m[1] * v.x + m[5] * v.y + m[9] * v.z + m[13] * v.w;
    o.z = m[2] * v.x + m[6] * v.y + m[10] * v.z + m[14] * v.w;
    o.w = m[3] * v.x + m[7] * v.y + m[11] * v.z + m[15] * v.w;
    return screenFromClip(o);
}

RspHle::Vec4 RspHle::screenFromClip(const Vec4& v) const {
    const float w = (v.w != 0.0f) ? v.w : 1.0f;
    Vec4 s;
    s.x = (v.x / w) * vpScaleX_ + vpTransX_;
    s.y = (v.y / w) * vpScaleY_ + vpTransY_;
    s.z = v.z / w;
    s.w = w;
    return s;
}

void RspHle::cmdTri1(u64 w0, u64 w1) {
    (void)w1;
    const u32 a = ((static_cast<u32>(w0) >> 16) & 0xFF) / 2;
    const u32 b = ((static_cast<u32>(w0) >> 8) & 0xFF) / 2;
    const u32 c = (static_cast<u32>(w0) & 0xFF) / 2;
    if (a < vertexBuffer_.size() && b < vertexBuffer_.size() && c < vertexBuffer_.size()) {
        flushTriangle(vertexBuffer_[a], vertexBuffer_[b], vertexBuffer_[c]);
    }
}

void RspHle::cmdTri2(u64 w0, u64 w1) {
    (void)w1;
    const auto tri = [&](u32 a, u32 b, u32 c) {
        if (a < vertexBuffer_.size() && b < vertexBuffer_.size() && c < vertexBuffer_.size()) {
            flushTriangle(vertexBuffer_[a], vertexBuffer_[b], vertexBuffer_[c]);
        }
    };
    tri(((static_cast<u32>(w0) >> 16) & 0xFF) / 2,
        ((static_cast<u32>(w0) >> 8) & 0xFF) / 2,
        (static_cast<u32>(w0) & 0xFF) / 2);
    tri(((static_cast<u32>(w1) >> 24) & 0xFF) / 2,
        ((static_cast<u32>(w1) >> 16) & 0xFF) / 2,
        ((static_cast<u32>(w1) >> 8) & 0xFF) / 2);
}

void RspHle::cmdQuad(u64 w0, u64 w1) {
    const auto tri = [&](u32 packed) {
        const u32 a = ((packed >> 16) & 0xFF) / 2;
        const u32 b = ((packed >> 8) & 0xFF) / 2;
        const u32 c = (packed & 0xFF) / 2;
        if (a < vertexBuffer_.size() && b < vertexBuffer_.size() && c < vertexBuffer_.size()) {
            flushTriangle(vertexBuffer_[a], vertexBuffer_[b], vertexBuffer_[c]);
        }
    };
    // G_QUAD packs both triangles in the low bytes of its two words; G_TRI2
    // uses the high bytes of word 1 for its second triangle.
    tri(static_cast<u32>(w0));
    tri(static_cast<u32>(w1));
}

void RspHle::flushTriangle(const Vertex& a, const Vertex& b, const Vertex& c) {
    ++trianglesSubmitted_;
    if (!rdp_) return;

    std::array<const Vertex*, 3> sorted{&a, &b, &c};
    std::sort(sorted.begin(), sorted.end(), [](const Vertex* lhs, const Vertex* rhs) {
        if (lhs->pos.y != rhs->pos.y) return lhs->pos.y < rhs->pos.y;
        return lhs->pos.x < rhs->pos.x;
    });
    const Vertex& top = *sorted[0];
    const Vertex& mid = *sorted[1];
    const Vertex& bot = *sorted[2];
    if (bot.pos.y <= top.pos.y) return;

    const bool topFlat = mid.pos.y == top.pos.y;
    const bool bottomFlat = mid.pos.y == bot.pos.y;
    const Vertex* majorTop = &top;
    const Vertex* majorBot = &bot;
    const Vertex* minorTop = &top;
    const Vertex* minorMid = &mid;
    const Vertex* minorBot = &bot;
    if (topFlat) {
        // Two vertices share YH: the more vertical edge is the major edge and
        // the other top vertex starts the single minor edge down to bot.
        if (std::abs(bot.pos.x - top.pos.x) <= std::abs(bot.pos.x - mid.pos.x)) {
            majorTop = &top;
            minorTop = &mid;
        } else {
            majorTop = &mid;
            minorTop = &top;
        }
        minorMid = minorTop;
    } else if (bottomFlat) {
        // Two vertices share YL: the major edge ends at one of them and the
        // minor edge ends at the other. YM == YL, so the minor edge’s upper
        // control point is its bottom vertex.
        if (std::abs(mid.pos.x - top.pos.x) <= std::abs(bot.pos.x - top.pos.x)) {
            majorBot = &mid;
            minorBot = &bot;
        } else {
            majorBot = &bot;
            minorBot = &mid;
        }
        minorMid = minorBot;
    }

    const double majorDy = majorBot->pos.y - majorTop->pos.y;
    if (majorDy <= 0.0) return;
    const double majorSlope = (majorBot->pos.x - majorTop->pos.x) / majorDy;
    const double minorDyHigh = minorMid->pos.y - minorTop->pos.y;
    const double minorSlopeHigh = minorDyHigh > 0.0
                                      ? (minorMid->pos.x - minorTop->pos.x) / minorDyHigh : 0.0;
    const double minorDyLow = minorBot->pos.y - minorMid->pos.y;
    const double minorSlopeLow = minorDyLow > 0.0
                                     ? (minorBot->pos.x - minorMid->pos.x) / minorDyLow : 0.0;
    const double majorAtMid = majorTop->pos.x + majorSlope * (mid.pos.y - majorTop->pos.y);
    const bool leftMajor = majorAtMid <= minorMid->pos.x;
    // XH/XM and the attribute bases are latched on the whole YH scanline.
    const double yOrigin = std::floor(std::round(double(top.pos.y) * 4.0) / 4.0);
    const double majorAtOrigin = majorTop->pos.x + majorSlope * (yOrigin - majorTop->pos.y);

    const bool textured = textureEnabled_;
    const bool shade = (geometryMode_ & G_SHADE) != 0;
    const bool zbuffer = (geometryMode_ & G_ZBUFFER) != 0;
    const u8 op = static_cast<u8>(0x08 | (zbuffer ? 1 : 0) | (textured ? 2 : 0) |
                                  (shade ? 4 : 0));
    const auto yFixed = [](float y) {
        return static_cast<u32>(std::lround(y * 4.0f)) & 0x3FFFu;
    };
    u64 command = (static_cast<u64>(op) << 56) |
                  (static_cast<u64>(textureTile_ & 7) << 48) |
                  (leftMajor ? (1ULL << 55) : 0ULL) |
                  (static_cast<u64>(yFixed(bot.pos.y)) << 32) |
                  (static_cast<u64>(yFixed(mid.pos.y)) << 16) |
                  static_cast<u64>(yFixed(top.pos.y));

    std::vector<u64> words;
    words.reserve(3 + (shade ? 8 : 0) + (textured ? 8 : 0) + (zbuffer ? 2 : 0));
    words.push_back(encodeEdge(minorMid->pos.x, minorSlopeLow));
    words.push_back(encodeEdge(majorAtOrigin, majorSlope));
    words.push_back(encodeEdge(minorTop->pos.x + minorSlopeHigh * (yOrigin - minorTop->pos.y),
                               minorSlopeHigh));

    const auto emitCoefficients = [&](u32 channels, const auto& valueAt) {
        std::array<u64, 8> block{};
        for (u32 channel = 0; channel < channels; ++channel) {
            const Plane plane = fitPlane(top, mid, bot, [&](const Vertex& v) {
                return valueAt(v, channel);
            });
            const s32 base = toFixed32(plane.a * majorAtOrigin + plane.b * yOrigin + plane.c);
            const s32 dx = toFixed32(plane.a);
            const s32 de = toFixed32(plane.b + plane.a * majorSlope);
            writeCoefficient(block, channel, base, dx, de, toFixed32(plane.b));
        }
        words.insert(words.end(), block.begin(), block.end());
    };

    if (shade) {
        emitCoefficients(4, [](const Vertex& v, u32 channel) {
            const u8 component[] = {v.r, v.g, v.b, v.a};
            return static_cast<double>(component[channel]) * 65536.0;
        });
    }
    if (textured) {
        emitCoefficients(3, [this](const Vertex& v, u32 channel) {
            if (channel == 0)
                return static_cast<double>(scaleTexCoord(v.s, textureScaleS_)) * 65536.0;
            if (channel == 1)
                return static_cast<double>(scaleTexCoord(v.t, textureScaleT_)) * 65536.0;
            return static_cast<double>(inverseW(v.pos.w)) * 65536.0;
        });
    }
    if (zbuffer) {
        const Plane plane = fitPlane(top, mid, bot, [](const Vertex& v) {
            return static_cast<double>(v.pos.z) * 65536.0;
        });
        const s32 base = toFixed32(plane.a * majorAtOrigin + plane.b * yOrigin + plane.c);
        const s32 dx = toFixed32(plane.a);
        const s32 de = toFixed32(plane.b + plane.a * majorSlope);
        words.push_back((static_cast<u64>(static_cast<u32>(base)) << 32) |
                        static_cast<u32>(dx));
        words.push_back((static_cast<u64>(static_cast<u32>(de)) << 32) |
                        static_cast<u32>(toFixed32(plane.b)));
    }

    std::vector<u64> commandWords;
    commandWords.reserve(words.size() + 1);
    commandWords.push_back(command);
    commandWords.insert(commandWords.end(), words.begin(), words.end());
    rdp_->submitCommands(commandWords.data(), commandWords.size());
}

void RspHle::cmdDl(u64 w0, u64 w1) {
    const u32 params = static_cast<u32>((w0 >> 16) & 0xFF);
    const u32 address = resolveAddress(static_cast<u32>(w1));
    if (params == 0) {
        // Push return address.
        if (dlStackIndex_ + 1 < static_cast<int>(dlStack_.size())) {
            dlStack_[++dlStackIndex_] = currentDl_;
        }
    }
    currentDl_ = address;
}

void RspHle::cmdMoveMem(u64 w0, u64 w1) {
    const u32 type = static_cast<u32>(gbi_ == GbiVersion::F3DEX2 ? (w0 & 0xFF)
                                                                 : ((w0 >> 8) & 0xFF));
    const u32 address = resolveAddress(static_cast<u32>(w1));

    if (type == 0x86 || (gbi_ == GbiVersion::F3DEX2 && type == 0x08)) {
        // Viewport: vscale (x,y) then vtrans (x,y), each s16.
        vpScaleX_ = static_cast<float>(readS16(address + 0));
        vpScaleY_ = static_cast<float>(readS16(address + 2));
        vpTransX_ = static_cast<float>(readS16(address + 8));
        vpTransY_ = static_cast<float>(readS16(address + 10));
    }
}

void RspHle::cmdMoveWord(u64 w0, u64 w1) {
    u32 index = 0, offset = 0;
    if (gbi_ == GbiVersion::F3DEX2) {
        index = static_cast<u32>(w0 & 0xFF);
        offset = static_cast<u32>((w0 >> 8) & 0xFFFF);
    } else {
        index = static_cast<u32>((w0 >> 16) & 0xFF);
        offset = static_cast<u32>(w0 & 0xFFFF);
    }
    if (index == 0x06) {
        // G_MW_SEGMENT
        const u32 segment = (offset / 4) & 0xF;
        segmentBase_[segment] = static_cast<u32>(w1) & 0x1FFFFFFF;
    }
}

void RspHle::cmdGeometryMode(u64 w0, u64 w1) {
    const u32 clear = ~static_cast<u32>(w0);
    geometryMode_ = (geometryMode_ & clear) | static_cast<u32>(w1);
}

void RspHle::cmdSetGeometryMode(u64 w1) {
    geometryMode_ |= static_cast<u32>(w1);
}

void RspHle::cmdClearGeometryMode(u64 w1) {
    geometryMode_ &= ~static_cast<u32>(w1);
}

void RspHle::cmdTexture(u64 w0, u64 w1) {
    // F3DEX and F3DEX2 share the tile/level bit layout; only the enable bit
    // moved from bit 0 to bit 1.
    textureLevel_ = (static_cast<u32>(w0) >> 11) & 7;
    textureTile_ = (static_cast<u32>(w0) >> 8) & 7;
    textureEnabled_ = gbi_ == GbiVersion::F3DEX2 ? ((static_cast<u32>(w0) >> 1) & 1) != 0
                                                 : ((static_cast<u32>(w0) >> 0) & 1) != 0;
    // G_TEXTURE stores 16.16 scales as signed 16-bit words in the command.
    textureScaleS_ = static_cast<s32>(static_cast<s16>((w1 >> 16) & 0xFFFF)) << 16;
    textureScaleT_ = static_cast<s32>(static_cast<s16>(w1 & 0xFFFF)) << 16;
}

void RspHle::forwardRdpCommand(u8 gbiOp, u64 w0, u64 w1) {
    if (!rdp_) return;
    u8 rawOp = 0;
    switch (gbiOp) {
        case 0xED: rawOp = 0x2D; break; // G_SETSCISSOR
        case 0xF0: rawOp = 0x30; break; // G_LOADTLUT
        case 0xF2: rawOp = 0x32; break; // G_SETTILESIZE
        case 0xF3: rawOp = 0x33; break; // G_LOADBLOCK
        case 0xF4: rawOp = 0x34; break; // G_LOADTILE
        case 0xF5: rawOp = 0x35; break; // G_SETTILE
        case 0xF6: rawOp = 0x36; break; // G_FILLRECT
        case 0xF7: rawOp = 0x37; break; // G_SETFILLCOLOR
        case 0xF8: rawOp = 0x38; break; // G_SETFOGCOLOR
        case 0xF9: rawOp = 0x39; break; // G_SETBLENDCOLOR
        case 0xFA: rawOp = 0x3A; break; // G_SETPRIMCOLOR
        case 0xFB: rawOp = 0x3B; break; // G_SETENVCOLOR
        case 0xFC: rawOp = 0x3C; break; // G_SETCOMBINE
        case 0xFD: rawOp = 0x3D; break; // G_SETTIMG
        case 0xFE: rawOp = 0x3E; break; // G_SETZIMG
        case 0xFF: rawOp = 0x3F; break; // G_SETCIMG
        default: return;
    }
    const u64 command = (static_cast<u64>(rawOp) << 56) |
                        ((w0 & 0xFFFFFFFFULL) << 32) | (w1 & 0xFFFFFFFFULL);
    rdp_->submitCommands(&command, 1);
}

void RspHle::cmdRdpCommand(u8 op, u64 w0, u64 w1) {
    const auto applyOtherMode = [&](bool high, bool f3dex2) {
        const int shiftField = static_cast<int>((w0 >> 8) & 0xFF);
        const int lengthField = static_cast<int>(w0 & 0xFF);
        // Legacy GBI stores shift and length directly. F3DEX2 stores the
        // complement of the shift and length minus one.
        const int length = f3dex2 ? lengthField + 1 : lengthField;
        const int shift = f3dex2 ? 32 - shiftField - length + (high ? 32 : 0)
                                 : shiftField + (high ? 32 : 0);
        if (length <= 0 || length > 64 || shift < 0 || shift + length > 64) return;
        const u64 fieldMask = length == 64 ? ~0ULL : ((1ULL << length) - 1ULL);
        const u64 mask = fieldMask << shift;
        // w1 carries the field value at bit 0; shift it into the absolute
        // position in the 64-bit mode word (high fields sit at bit 32+).
        otherModes_ = (otherModes_ & ~mask) | (((w1 & fieldMask) << shift) & mask);
        if (rdp_) rdp_->setOtherModes(otherModes_);
    };

    switch (op) {
        case 0xB9: // legacy G_SETOTHERMODE_L
            applyOtherMode(false, false);
            break;
        case 0xBA: // legacy G_SETOTHERMODE_H
            applyOtherMode(true, false);
            break;
        case 0xE2: // F3DEX2 G_SETOTHERMODE_L
            applyOtherMode(false, true);
            break;
        case 0xE3: // F3DEX2 G_SETOTHERMODE_H
            applyOtherMode(true, true);
            break;
        case 0xEF: { // G_RDPSETOTHERMODE
            otherModes_ = ((w0 & 0xFFFFFFULL) << 32) | (w1 & 0xFFFFFFFFULL);
            if (rdp_) rdp_->setOtherModes(otherModes_);
            break;
        }
        case 0xE4: { // G_TEXRECT (first word geometry; consume the two texture words)
            const s32 xh = static_cast<s32>((w0 >> 20) & 0xFFF) >> 2;
            const s32 yh = static_cast<s32>((w0 >> 8) & 0xFFF) >> 2;
            const s32 xl = static_cast<s32>((w1 >> 20) & 0xFFF) >> 2;
            const s32 yl = static_cast<s32>((w1 >> 8) & 0xFFF) >> 2;
            if (rdp_) rdp_->fillRectangle(xl, yl, xh, yh);
            currentDl_ += 16; // skip the remaining two words of this 24-byte command
            break;
        }
        case 0xF7:
            fillColor_ = static_cast<u32>(w1);
            forwardRdpCommand(op, w0, w1);
            break;
        case 0xFA:
            primColor_ = static_cast<u32>(w1);
            forwardRdpCommand(op, w0, w1);
            break;
        case 0xFB:
            envColor_ = static_cast<u32>(w1);
            forwardRdpCommand(op, w0, w1);
            break;
        case 0xFC:
            combineMode_ = ((w0 & 0xFFFFFFFFULL) << 32) | (w1 & 0xFFFFFFFFULL);
            forwardRdpCommand(op, w0, w1);
            break;
        default:
            forwardRdpCommand(op, w0, w1);
            break;
    }
}

} // namespace n64forge
