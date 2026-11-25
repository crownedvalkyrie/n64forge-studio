#pragma once
#include "n64forge/core/bus.hpp"
#include "n64forge/core/cartridge.hpp"
#include "n64forge/core/cpu.hpp"
#include "n64forge/core/core_stats.hpp"
#include "n64forge/core/ipl.hpp"
#include "n64forge/core/rdp.hpp"
#include "n64forge/core/rsp.hpp"
#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

namespace n64forge {

class EmulatorTestAccess;

class Emulator {
public:
    Emulator();
    void reset();

    bool loadCartridge(const std::filesystem::path& path);
    bool loadCartridge(Cartridge cart);
    [[nodiscard]] bool hasCartridge() const noexcept { return cartridge_.loaded(); }

    void runOneStep();
    // Advance by a fixed diagnostic batch; refresh the VI scanout snapshot.
    void runFrame();
    // Advance to the next VI field for host presentation (with a boot-time cap).
    u32 runVideoFrame(bool capture = true);

    // 1x through 4x internal RDP presentation. RDRAM writeback remains native
    // so the CPU sees the conventional framebuffer layout.
    void setInternalResolution(u32 scale);
    void scanout();
    // Maximum VI window (10-bit endpoints, 12-bit 2.10 scale).
    static constexpr u32 MaxScanoutPixels = 4091 * 1024;
    struct ScanoutLayout {
        u32 width = 0, height = 0, stride = 0, rowWidth = 0, start = 0, bytes = 0;
        bool operator==(const ScanoutLayout&) const = default;
    };
    struct ScanoutHistory {
        ScanoutLayout layout;
        u64 epoch = 0;
        u32 pitch = 0;
        bool enhanced = false;
        std::vector<u32> lines = std::vector<u32>(Memory::RdramSize / 64);
        void reset() noexcept { layout = {}; epoch = 0; enhanced = false; }
    };
    // Writes directly into a caller-owned frame slot. Nonzero packedRowAlignment
    // copies guest-order scanout with padded rows rather than decoding RGBA.
    // The previous slot is read-only and includes the same row padding.
    bool scanoutForPresentation(std::span<u32> output, std::span<const u32> previous,
                                 ScanoutHistory& history, u32 packedRowAlignment = 0);
    [[nodiscard]] u32 scanoutBytesPerPixel() const noexcept { return scanoutBytesPerPixel_; }
    [[nodiscard]] u32 scanoutPitch() const noexcept { return scanoutPitch_; }
    void setTimingEnabled(bool enabled) noexcept { timingEnabled_ = enabled; }
    // Returns the effective mode (JIT may be unavailable on this host).
    CpuExecutionMode setCpuExecutionMode(CpuExecutionMode requested);
    void setBlockCacheEnabled(bool enabled) noexcept { blockCacheEnabled_ = enabled; }
    void setEventBatchingEnabled(bool enabled) noexcept { eventBatchingEnabled_ = enabled; }
    // Read on the thread that owns the emulated machine.
    [[nodiscard]] CoreCounters counters() const noexcept;

    // Host audio output queue: the emulated audio source pushes frames here and
    // the host output (Qt AudioOutput) drains them each frame.
    [[nodiscard]] size_t audioFrameCount() const;
    [[nodiscard]] const int16_t* audioSamples(size_t& frames) const;

    Bus& bus() noexcept { return bus_; }
    const Bus& bus() const noexcept { return bus_; }
    Cpu& cpu() noexcept { return cpu_; }
    const Cpu& cpu() const noexcept { return cpu_; }
    Rsp& rsp() noexcept { return rsp_; }
    const Rsp& rsp() const noexcept { return rsp_; }
    Rdp& rdp() noexcept { return rdp_; }
    const Rdp& rdp() const noexcept { return rdp_; }
    const Cartridge& cartridge() const noexcept { return cartridge_; }
    // CIC profile authenticated from the cartridge IPL2 checksum.
    [[nodiscard]] const CicProfile& cicProfile() const noexcept { return cicProfile_; }

    [[nodiscard]] u32 framebufferWidth() const noexcept { return scanoutWidth_; }
    [[nodiscard]] u32 framebufferHeight() const noexcept { return scanoutHeight_; }
    [[nodiscard]] const std::vector<u32>& framebuffer() const noexcept { return scanout_; }
    // Read-only diagnostic copy of the active VI window used for presentation.
    [[nodiscard]] ScanoutLayout presentationLayout() const noexcept { return scanoutLayout(); }

private:
    friend class EmulatorTestAccess;
    void applyBootProfile(const CicProfile& profile);
    void tickHardware(bool sample, u32 cycles = 1);
    u32 runCycles(u32 limit, bool stopAtField);
    [[nodiscard]] ScanoutLayout scanoutLayout() const noexcept;
    void renderScanout(const ScanoutLayout& layout, std::span<u32> destination) const;

    Bus bus_;
    Cartridge cartridge_;
    CicProfile cicProfile_{};
    Cpu cpu_;
    Rsp rsp_;
    Rdp rdp_;
    std::vector<u32> scanout_;
    u32 scanoutWidth_ = 0;
    u32 scanoutHeight_ = 0;
    u32 scanoutBytesPerPixel_ = 0;
    u32 scanoutPitch_ = 0;
    std::vector<int16_t> audio_;
    u64 audioResamplePhase_ = 0;
    CoreCounters counters_{};
    u32 timingCountdown_ = 0;
    bool timingEnabled_ = false;
    bool blockCacheEnabled_ = true;
    bool eventBatchingEnabled_ = true;
};

} // namespace n64forge
