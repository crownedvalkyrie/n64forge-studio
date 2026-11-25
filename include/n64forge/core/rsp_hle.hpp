#pragma once
#include "n64forge/core/ostask.hpp"
#include "n64forge/core/types.hpp"
#include <array>
#include <string>

namespace n64forge {

class Bus;
class Rdp;

enum class MicrocodeVersion { Unknown, Fast3D, F3DEX, F3DEX2 };

// ---------------------------------------------------------------------------
// High-level emulation of the RSP graphics microcode used by commercial N64
// titles. Parses the OSTask header, identifies the microcode variant, walks
// the display list and translates geometry/RDP commands for the reference RDP.
// ---------------------------------------------------------------------------
class RspHle {
public:
    explicit RspHle(Bus* bus, Rdp* rdp);

    // Execute a graphics task (type == 1). Returns false on error.
    bool runGraphicsTask(const OSTask& task);

    [[nodiscard]] MicrocodeVersion detectedMicrocode() const noexcept { return detectedUcode_; }
    [[nodiscard]] const std::string& microcodeName() const noexcept { return ucodeName_; }
    [[nodiscard]] u32 displayListAddress() const noexcept { return displayListAddress_; }
    [[nodiscard]] u32 commandsProcessed() const noexcept { return commandsProcessed_; }
    [[nodiscard]] u32 trianglesSubmitted() const noexcept { return trianglesSubmitted_; }
    [[nodiscard]] u32 verticesLoaded() const noexcept { return verticesLoaded_; }
    [[nodiscard]] u64 otherModes() const noexcept { return otherModes_; }

private:
    struct Vec4 { float x, y, z, w; };
    struct Vertex {
        Vec4 pos;
        s16 s = 0, t = 0; // raw G_VTX texture coordinates (S10.5)
        u8 r = 0, g = 0, b = 0, a = 0;
    };

    enum class GbiVersion { Fast3D, F3DEX, F3DEX2 };

    void resetState();
    void detectMicrocode(const OSTask& task);

    [[nodiscard]] u8 read8(u32 address) const;
    [[nodiscard]] s16 readS16(u32 address) const;
    [[nodiscard]] u16 readU16(u32 address) const;
    [[nodiscard]] u32 readU32(u32 address) const;

    [[nodiscard]] u32 resolveAddress(u32 address) const;
    [[nodiscard]] u64 readCommand(u32 address) const;

    void processDisplayList(u32 address, u32 maxCommands = 4096);
    void executeF3DEX(u8 op, u64 w0, u64 w1);
    void executeF3DEX2(u8 op, u64 w0, u64 w1);

    void cmdMtx(u64 w0, u64 w1);
    void cmdPopMtx();
    void cmdVtx(u64 w0, u64 w1);
    void cmdTri1(u64 w0, u64 w1);
    void cmdTri2(u64 w0, u64 w1);
    void cmdQuad(u64 w0, u64 w1);
    void cmdDl(u64 w0, u64 w1);
    void cmdMoveMem(u64 w0, u64 w1);
    void cmdMoveWord(u64 w0, u64 w1);
    void cmdSetGeometryMode(u64 w1);
    void cmdClearGeometryMode(u64 w1);
    void cmdTexture(u64 w0, u64 w1);
    void cmdGeometryMode(u64 w0, u64 w1);
    void cmdRdpCommand(u8 op, u64 w0, u64 w1);
    void forwardRdpCommand(u8 gbiOp, u64 w0, u64 w1);

    [[nodiscard]] Vec4 transformVertex(const Vec4& v) const;
    [[nodiscard]] Vec4 screenFromClip(const Vec4& v) const;
    void flushTriangle(const Vertex& a, const Vertex& b, const Vertex& c);

    Bus* bus_;
    Rdp* rdp_;

    MicrocodeVersion detectedUcode_ = MicrocodeVersion::Unknown;
    std::string ucodeName_ = "Unknown";
    u32 displayListAddress_ = 0;
    u32 commandsProcessed_ = 0;
    u32 trianglesSubmitted_ = 0;
    u32 verticesLoaded_ = 0;

    GbiVersion gbi_ = GbiVersion::F3DEX;
    std::array<u32, 16> segmentBase_{};
    std::array<Vertex, 32> vertexBuffer_{};

    // Matrix state (column-major 4x4 floats).
    std::array<float, 16> projectionMatrix_;
    std::array<std::array<float, 16>, 10> modelviewStack_{};
    int modelviewIndex_ = 0;
    std::array<float, 16> currentModelview_;
    std::array<float, 16> combinedMatrix_;

    // Viewport (from G_MOVEMEM viewport).
    float vpScaleX_ = 160.0f;
    float vpScaleY_ = 120.0f;
    float vpTransX_ = 160.0f;
    float vpTransY_ = 120.0f;

    u32 geometryMode_ = 0;
    u32 fillColor_ = 0;
    u32 envColor_ = 0;
    u32 primColor_ = 0;
    u64 combineMode_ = 0;
    u64 otherModes_ = 0;

    std::array<u32, 8> dlStack_{};
    int dlStackIndex_ = -1;
    u32 currentDl_ = 0;

    bool textureEnabled_ = false;
    s32 textureScaleS_ = 0x10000;
    s32 textureScaleT_ = 0x10000;
    u32 textureTile_ = 0;
    u32 textureLevel_ = 0;
};

} // namespace n64forge
