#pragma once
// Deterministic software RDP command processor and reference renderer.
// Parses the 64-bit RDP command stream and draws into guest RDRAM. Also
// maintains an RGBA8888 debug view of color writes. The reference pipeline
// is not cycle- or bit-accurate; see docs/ROADMAP.md for its limits.
#include "n64forge/core/memory.hpp"
#include "n64forge/core/types.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace n64forge {

class Bus;
class HwRegisters;

inline constexpr u32 MI_INTR_DP = 0x20;

// ---------------------------------------------------------------------------
// RDP command processor / reference renderer
// ---------------------------------------------------------------------------
class Rdp {
public:
    Rdp();

    void reset();
    void setBus(Bus* bus) noexcept { bus_ = bus; }
    void setHw(HwRegisters* hw) noexcept { hw_ = hw; }

    // Resize/inspect the internal RGBA8888 framebuffer.
    void setFramebufferSize(u32 width, u32 height);
    void setResolutionScale(u32 scale);
    // 0 selects hardware concurrency (clamped to 8); explicit values are
    // clamped to 1..8. Scanlines are partitioned by y % count, so output is
    // identical to serial execution. Reconfiguration is safe while a span is
    // in flight: workers hold a shared snapshot of the pool.
    void setThreadCount(unsigned count);
    [[nodiscard]] unsigned threadCount() const noexcept {
        return threadCount_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] unsigned effectiveThreadCount() const noexcept;
    [[nodiscard]] u32 resolutionScale() const noexcept { return resolutionScale_; }
    [[nodiscard]] u32 width() const noexcept { return fbWidth_; }
    [[nodiscard]] u32 height() const noexcept { return fbHeight_; }
    [[nodiscard]] const std::vector<u32>& framebuffer() const noexcept { return fb_; }
    [[nodiscard]] u32 pixel(u32 x, u32 y) const noexcept;
    // Copy the presentation surface only when it matches the VI's current
    // source image. Guest RDRAM is still the source for pixels changed by CPU/DMA.
    [[nodiscard]] bool canPresentScaled(u32 start, u32 stride, u32 width, u32 height, u32 bytes) const noexcept;
    [[nodiscard]] bool hasFineSamples() const noexcept { return hasFineSamples_; }
    // Diagnostic view of the state checked by canPresentScaled(). This is
    // read-only and does not make scaled presentation valid by itself.
    struct ScaledSourceInfo {
        u32 colorAddress = 0;
        u32 colorWidth = 0;
        u32 colorSize = 0;
        u32 colorFormat = 0;
        u32 nativeWidth = 0;
        u32 nativeHeight = 0;
        bool busReady = false;
        bool fineSamples = false;
    };
    [[nodiscard]] ScaledSourceInfo scaledSourceInfo() const noexcept;
    struct FineSurfaceSummary {
        u32 address = 0;
        u32 width = 0;
        u32 size = 0;
        u32 fmt = 0;
        u32 nativeWidth = 0;
        u32 nativeHeight = 0;
        u32 epoch = 0;
    };
    [[nodiscard]] std::vector<FineSurfaceSummary> fineSurfaceSummaries() const noexcept;
    void copyScaledColor(std::span<u32> out, u32 pitchWords, u32 start, u32 stride,
                         u32 width, u32 height, u32 bytes) const;

    // Process commands from the DP ring buffer (DP_START .. DP_END).
    // Advances DP_CURRENT and raises DP interrupts on Sync_Full.
    void process();

    // Public helpers used by the HLE graphics interpreter. These are
    // translated RDP operations consumed by the same internal pipeline.
    void setFillColor(u32 color);
    void setEnvColor(u32 color);
    void setPrimColor(u32 minLevel, u32 lodFrac, u32 color);
    void setBlendColor(u32 color);
    void setFogColor(u32 color);
    void setCombineMode(u64 mode);
    void setOtherModes(u64 mode);
    void setScissor(s32 minX, s32 minY, s32 maxX, s32 maxY);
    void fillRectangle(s32 xl, s32 yl, s32 xr, s32 yr);
    void drawFlatTriangle(s32 x0, s32 y0, s32 x1, s32 y1, s32 x2, s32 y2, u32 color);

    // Submit a complete RDP command (word 0 followed by its parameter words)
    // directly, without going through the guest DP DMA ring. Used by the RSP
    // HLE to drive the exact same state machine and rasterizer as hardware.
    void submitCommands(const u64* words, std::size_t count);

    // Rendering statistics (reset on Rdp::reset()).
    struct Stats {
        std::atomic<u64> commandsProcessed = 0;
        std::atomic<u64> fillRectangles = 0;
        std::atomic<u64> triangles = 0;
        std::atomic<u64> triangleCommands = 0;
        std::atomic<u64> texturedTriangles = 0;
        std::atomic<u64> shadedTriangles = 0;
        std::atomic<u64> pixelsDrawn = 0;
        std::atomic<u64> fullSyncs = 0;
    };
    [[nodiscard]] Stats stats() const noexcept {
        return Stats{statsCommandsProcessed_.load(), statsFillRectangles_.load(),
                     statsTriangles_.load(), statsTriangleCommands_.load(),
                     statsTexturedTriangles_.load(), statsShadedTriangles_.load(),
                     statsPixelsDrawn_.load(), statsFullSyncs_.load()};
    }
    void resetStats() noexcept {
        statsCommandsProcessed_ = 0;
        statsFillRectangles_ = 0;
        statsTriangles_ = 0;
        statsTriangleCommands_ = 0;
        statsTexturedTriangles_ = 0;
        statsShadedTriangles_ = 0;
        statsPixelsDrawn_ = 0;
        statsFullSyncs_ = 0;
    }

    std::atomic<u64> statsCommandsProcessed_ = 0;
    std::atomic<u64> statsFillRectangles_ = 0;
    std::atomic<u64> statsTriangles_ = 0;
    std::atomic<u64> statsTriangleCommands_ = 0;
    std::atomic<u64> statsTexturedTriangles_ = 0;
    std::atomic<u64> statsShadedTriangles_ = 0;
    std::atomic<u64> statsPixelsDrawn_ = 0;
    std::atomic<u64> statsFullSyncs_ = 0;

    // Deterministic horizontal-scanline worker pool. Tasks process disjoint
    // `y % num_threads` rows, so rasterization order and results stay stable.
    struct ThreadPool {
        static constexpr unsigned kMaxThreads = 8;

        explicit ThreadPool(unsigned count) : count_(count) {
            threads_.reserve(count - 1);
            try {
                for (unsigned i = 1; i < count; ++i)
                    threads_.emplace_back([this, i] { worker(i); });
            } catch (...) {
                stop();
                throw;
            }
        }

        ~ThreadPool() { stop(); }

        void stop() {
            {
                std::unique_lock<std::mutex> lock(mutex_);
                stopping_ = true;
            }
            cv_.notify_all();
            for (auto& thread : threads_) {
                if (thread.joinable()) thread.join();
            }
        }

        template<class Fn>
        void execute(s64 y0, s64 y1, const Fn& fn) {
            const auto run = [&](unsigned lane) {
                const s64 start = y0 + (lane + count_ - y0 % count_) % count_;
                for (s64 y = start; y < y1; y += count_) fn(static_cast<u32>(y), lane);
            };
            {
                std::unique_lock<std::mutex> lock(mutex_);
                task_ = [&run](unsigned lane) { run(lane); };
                pending_ = count_ - 1;
                ++epoch_;
            }
            cv_.notify_all();
            run(0);
            std::unique_lock<std::mutex> lock(mutex_);
            doneCv_.wait(lock, [this] { return pending_ == 0; });
            task_ = {};
        }

    private:
        void worker(unsigned lane) {
            u64 seen = 0;
            while (true) {
                {
                    std::unique_lock<std::mutex> lock(mutex_);
                    cv_.wait(lock, [&] { return seen != epoch_ || stopping_; });
                    if (stopping_) return;
                    seen = epoch_;
                }
                task_(lane);
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    if (--pending_ == 0) doneCv_.notify_one();
                }
            }
        }

        const unsigned count_;
        std::vector<std::jthread> threads_;
        std::function<void(unsigned)> task_;
        std::mutex mutex_;
        std::condition_variable cv_;
        std::condition_variable doneCv_;
        unsigned pending_ = 0;
        u64 epoch_ = 0;
        bool stopping_ = false;
    };

    // executeSpan takes a shared snapshot under configMutex_, so a concurrent
    // setThreadCount() cannot destroy a pool that still has work in flight.
    std::mutex configMutex_;
    std::shared_ptr<ThreadPool> threadPool_;
    // Core/tests remain deterministic and serial by default; CLI/Studio can
    // opt into hardware-concurrency selection with setThreadCount(0).
    std::atomic<unsigned> threadCount_{1};

    // One touched-line set per scanline worker plus lane 0 for serial commands.
    // Each range is flushed before the guest can fetch code again.
    std::array<Memory::DeferredWriteSet, ThreadPool::kMaxThreads> deferredWrites_{};

private:
    template<class Fn> void executeSpan(s32 y0, s32 y1, s32 width, bool depth, Fn&& fn);
    void flushDeferredWrites(unsigned lane) noexcept;
    [[nodiscard]] bool resizeFramebuffer(u32 nativeWidth, u32 nativeHeight);
    [[nodiscard]] static u32 fragmentNoise(s32 x, s32 y) noexcept;
    [[nodiscard]] bool fragmentUsesNoise(u32 cycle) const noexcept;

    Bus* bus_ = nullptr;
    HwRegisters* hw_ = nullptr;

    // State
    struct ColorImage {
        u32 fmt = 0;
        u32 size = 0;
        u32 width = 320;
        u32 address = 0;
    } colorImage_;
    ColorImage textureImage_;
    // Recent native buffers with their own subpixel sidecars. Games commonly
    // render into one buffer while VI scans out another, so eligibility is
    // keyed by buffer configuration rather than the latest SET_COLOR_IMAGE,
    // and each retired buffer keeps its samples until it leaves the ring.
    struct FineSurface {
        u32 address = 0;
        u32 width = 0;
        u32 size = 0;
        u32 fmt = 0;
        u32 nativeWidth = 0;
        u32 nativeHeight = 0;
        u32 epoch = 0;
        std::vector<u32> fb;
        std::vector<u32> nativeColor;
        std::vector<u32> colorStamp;
    };
    struct SurfaceView {
        bool valid = false;
        u32 epoch = 0;
        u32 nativeWidth = 0;
        u32 nativeHeight = 0;
        u32 xOffset = 0;
        u32 yOffset = 0;
        const std::vector<u32>* fb = nullptr;
        const std::vector<u32>* nativeColor = nullptr;
        const std::vector<u32>* colorStamp = nullptr;
    };
    std::vector<FineSurface> fineSurfaces_;
    [[nodiscard]] SurfaceView findFineSurface(u32 start, u32 stride,
                                              u32 width, u32 height, u32 bytes) const noexcept;
    struct Tile {
        u32 fmt = 0, size = 0, line = 0, base = 0, palette = 0;
        u32 sMode = 0, sMask = 0, sShift = 0, tMode = 0, tMask = 0, tShift = 0;
        u32 sl = 0, tl = 0, sh = 0, th = 0;
    };
    std::array<Tile, 8> tiles_{};
    std::array<u16, 2048> tmem_{};
    // Carry a split command between DMA batches. DPC_CURRENT tracks the DMA
    // read, not execution of the eventual complete command.
    std::array<u64, 22> fifo_{};
    std::size_t fifoSize_ = 0;
    std::size_t commandSize_ = 0;

    struct Scissor {
        s32 minX = 0;
        s32 minY = 0;
        s32 maxX = 320;
        s32 maxY = 240;
    } scissor_;

    u32 fillColor_ = 0;
    u32 envColor_ = 0;
    u32 primColor_ = 0;
    u32 blendColor_ = 0;
    u32 fogColor_ = 0;
    u32 depthImage_ = 0;
    u32 primitiveDepth_ = 0, primitiveDz_ = 0, primitiveLod_ = 0;
    // level[2:0] from the current triangle's base word (bits 53:51). Hardware
    // treats it as the maximum LOD for the primitive. Level-zero fraction
    // is modeled separately from the still-missing mip tile selection.
    u32 triangleLevel_ = 0;
    // RDRAM's two hidden bits per halfword, used for depth delta and coverage.
    std::vector<u8> hidden_;
    struct CombineMux {
        std::array<u8, 4> rgb{}, alpha{};
    };
    std::array<CombineMux, 2> combineMux_{};
    struct BlendMux {
        u8 p = 0, a = 0, m = 0, b = 0;
    };
    std::array<BlendMux, 2> blendMux_{};
    u64 otherModes_ = 0;

    std::vector<u32> fb_;
    std::vector<u32> nativeColor_;
    std::vector<u32> colorStamp_;
    u32 colorEpoch_ = 1;
    bool hasFineSamples_ = false;
    u32 fbWidth_ = 320;
    u32 fbHeight_ = 240;
    u32 nativeFbWidth_ = 320;
    u32 nativeFbHeight_ = 240;
    u32 resolutionScale_ = 1;

    [[nodiscard]] u64 readWord64(u32 address) const;
    [[nodiscard]] static std::size_t commandWordCount(u32 op) noexcept;
    void executeCommand(const u64* words, std::size_t count);
    bool writePixel(s32 x, s32 y, u32 color, bool fill = false, bool refine = false);
    [[nodiscard]] bool scissorContains(s32 x, s32 y) const noexcept;

    void cmdSetColorImage(u64 word0, u64 word1);
    void cmdSetDepthImage(u64 word1);
    void cmdSetTextureImage(u64 word0, u64 word1);
    void cmdSetTile(u64 word);
    void cmdSetTileSize(u64 word);
    void cmdLoadTexture(u64 word, bool block);
    void cmdLoadTlut(u64 word);
    [[nodiscard]] u32 readTextureImage(u64 index) const;
    [[nodiscard]] u32 readTexel(const Tile& tile, u32 s, u32 t) const;
    [[nodiscard]] u32 sampleTile(const Tile& tile, s64 s, s64 t, bool filter) const;
    void cmdTextureRectangle(u64 word, u64 params, bool flip);
    void cmdSetScissor(u64 word0);
    void cmdSetFillColor(u64 word1);
    void cmdSetEnvColor(u64 word1);
    void cmdSetPrimColor(u64 word0, u64 word1);
    void cmdSetBlendColor(u64 word1);
    void cmdSetFogColor(u64 word1);
    void cmdSetCombineMode(u64 word0, u64 word1);
    void cmdSetOtherModes(u64 word0, u64 word1);
    void cmdSyncFull();
    void cmdFillRectangle(u64 word0);
    void cmdFillTriangle(u64 word0, const u64* words, std::size_t wordCount);
    struct CombineResult {
        u32 color = 0;
        u8 alpha0 = 0; // first-cycle alpha, used by alpha compare in 2-cycle mode
    };
    [[nodiscard]] CombineResult combine(u32 shade, u32 tex0, u32 tex1, bool twoCycle,
                                        u32 noise, u32 lodFraction = 0) const;
    bool drawFragment(s32 x, s32 y, u32 shade, u32 tex0, u32 tex1,
                      u32 coverage, bool firstSample, s32 z, u32 dz, bool hasZ,
                      u32 noise, const CombineResult* precombined = nullptr, u32 lodFraction = 0, bool refine = false);

    // Fixed-point helpers (RDP uses a mix of 2.0 and 16.16; we normalize to 16.16).
    [[nodiscard]] static s32 signExtend14(u32 v) noexcept;
};

} // namespace n64forge
