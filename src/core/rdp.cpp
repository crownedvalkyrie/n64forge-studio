#include "n64forge/core/rdp.hpp"
#if defined(__ARM_NEON) && defined(__aarch64__)
#include <arm_neon.h>
#endif
#include "n64forge/core/bus.hpp"
#include "n64forge/core/hw_registers.hpp"
#include <algorithm>
#include <bit>
#include <condition_variable>
#include <functional>
#include <limits>
#include <mutex>
#include <thread>
#include <utility>

namespace n64forge {

namespace {

// Command ids are in bits 56..61 of the first 64-bit word.
[[nodiscard]] u32 cmdId(u64 word) noexcept { return static_cast<u32>((word >> 56) & 0x3F); }

[[nodiscard]] u32 u10_2ToPixel(u64 v) noexcept { return static_cast<u32>((v >> 2) & 0x3FF); }

[[nodiscard]] u32 packRgba(u8 r, u8 g, u8 b, u8 a) noexcept {
    return (static_cast<u32>(a) << 24) | (static_cast<u32>(b) << 16) |
           (static_cast<u32>(g) << 8) | static_cast<u32>(r);
}

[[nodiscard]] u32 expandRgba16(u16 rgba) noexcept {
    const auto expand = [](u32 n) -> u8 { return (n << 3) | (n >> 2); };
    return packRgba(expand(rgba >> 11), expand((rgba >> 6) & 31),
                    expand((rgba >> 1) & 31), rgba & 1 ? 255 : 0);
}

[[nodiscard]] u32 channel(u32 color, u32 i) { return (color >> (8 * i)) & 255; }
[[nodiscard]] u32 fromRegister(u32 rgba) {
    return packRgba(rgba >> 24, rgba >> 16, rgba >> 8, rgba);
}

constexpr u64 RdramLimit = Memory::RdramSize;
// 64 MiB of RGBA8888 debug surface: large enough for every native N64 mode
// at 4x internal scale, small enough to reject accidental wrap allocations.
constexpr u64 MaxFramebufferPixels = 1ULL << 24;

// Edge functions of two s32 endpoints can reach 2^64 as a signed product and
// exceed s64 even though each operand difference fits in s64. Keep only the
// sign and unsigned magnitude: rasterization needs the sign and a zero test,
// never the full value.
struct WideSigned {
    int sign = 0; // -1, 0 or 1
    u64 magnitude = 0;
};

[[nodiscard]] WideSigned wideProduct(s64 a, s64 b) noexcept {
    const bool negative = (a < 0) != (b < 0);
    const auto magnitude = [](s64 n) noexcept {
        return n < 0 ? static_cast<u64>(-(n + 1)) + 1 : static_cast<u64>(n);
    };
    const u64 ma = magnitude(a), mb = magnitude(b);
    if (ma == 0 || mb == 0) return {};
    return {negative ? -1 : 1, ma * mb};
}

[[nodiscard]] WideSigned wideSubtract(WideSigned a, WideSigned b) noexcept {
    if (a.sign == 0) return {-b.sign, b.magnitude};
    if (b.sign == 0) return a;
    if (a.sign == b.sign) {
        if (a.magnitude == b.magnitude) return {};
        return a.magnitude > b.magnitude ? a : WideSigned{-a.sign, b.magnitude};
    }
    // Opposite signs: magnitudes add. Saturate on overflow; only the sign and
    // non-zero status are observable, and neither changes.
    const u64 sum = a.magnitude + b.magnitude;
    if (sum < a.magnitude) return {a.sign, std::numeric_limits<u64>::max()};
    return {a.sign, sum};
}

// Each rasterizer thread records the RDRAM lines it touches in a private set.
// The owning command flushes that set before the guest runs another instruction.
thread_local Memory::DeferredWriteSet* activeDeferredWrites = nullptr;

class DeferredWriteScope {
public:
    explicit DeferredWriteScope(Memory::DeferredWriteSet& set) noexcept
        : previous_(activeDeferredWrites) {
        activeDeferredWrites = &set;
    }
    ~DeferredWriteScope() { activeDeferredWrites = previous_; }
    DeferredWriteScope(const DeferredWriteScope&) = delete;
    DeferredWriteScope& operator=(const DeferredWriteScope&) = delete;

private:
    Memory::DeferredWriteSet* previous_;
};

} // namespace

Rdp::Rdp() { reset(); }

void Rdp::flushDeferredWrites(unsigned lane) noexcept {
    if (bus_ && lane < deferredWrites_.size()) bus_->rdram().flushDeferred(deferredWrites_[lane]);
}

u32 Rdp::fragmentNoise(s32 x, s32 y) noexcept {
    // A deterministic split-mix keeps dither noise independent of execution
    // order. Hardware noise is time-varying and remains an approximation.
    u32 n = 1u ^ (static_cast<u32>(x) * 0x9E3779B1u) ^ (static_cast<u32>(y) * 0x85EBCA6Bu);
    n ^= n >> 16;
    n *= 0x7FEB352Du;
    n ^= n >> 15;
    n *= 0x846CA68Bu;
    n ^= n >> 16;
    return n;
}

bool Rdp::fragmentUsesNoise(u32 cycle) const noexcept {
    return cycle != 3 && ((otherModes_ & 3) == 3 ||
        (cycle < 2 && (combineMux_[1].rgb[0] == 7 || (cycle == 1 && combineMux_[0].rgb[0] == 7))));
}

unsigned Rdp::effectiveThreadCount() const noexcept {
    const unsigned configured = threadCount_.load(std::memory_order_relaxed);
    if (configured != 0) return configured;
    return std::clamp(std::thread::hardware_concurrency(), 1u, 4u);
}

void Rdp::setThreadCount(unsigned count) {
    const unsigned clamped = count == 0 ? 0u
                                        : std::max(1u, std::min<unsigned>(ThreadPool::kMaxThreads, count));
    std::lock_guard<std::mutex> lock(configMutex_);
    if (clamped == threadCount_.load(std::memory_order_relaxed)) return;
    threadCount_.store(clamped, std::memory_order_relaxed);
    threadPool_.reset();
}

template<class Fn>
void Rdp::executeSpan(s32 y0, s32 y1, s32 width, bool depth, Fn&& fn) {
    // Clamp in s64 before partitioning. Negative scissor bounds used to be
    // cast to u32, changing the modulo partition and dropping scanlines.
    const s64 start = std::max<s64>(y0, 0);
    const s64 end = std::max<s64>(y1, start);
    if (end <= start) return;
    // Different logical rows can still alias in physical RDRAM if the guest
    // binds overlapping color and depth surfaces or uses a narrow stride.
    const u64 stride = colorImage_.width;
    const u64 bytes = colorImage_.size == 2 ? 2 : 4;
    const u64 c0 = colorImage_.address + start * stride * bytes;
    const u64 c1 = colorImage_.address + end * stride * bytes;
    const u64 z0 = depthImage_ + start * stride * 2;
    const u64 z1 = depthImage_ + end * stride * 2;
    const bool alias = nativeFbWidth_ != stride ||
                       (depth && c0 < z1 && z0 < c1);
    const auto runSerial = [&] {
        auto& writes = deferredWrites_[0];
        writes.reset();
        DeferredWriteScope scope(writes);
        for (s64 y = start; y < end; ++y) fn(static_cast<u32>(y), 0);
        flushDeferredWrites(0);
    };
    // Waking workers costs more than drawing small primitives. Keep the
    // short spans inline, and never reorder fragments with aliased surfaces.
    if (alias || end - start < 8 || (end - start) * width < 4096) {
        runSerial();
        return;
    }
    std::shared_ptr<ThreadPool> pool;
    unsigned effective = 1;
    {
        std::lock_guard<std::mutex> lock(configMutex_);
        effective = effectiveThreadCount();
        if (!threadPool_ && effective > 1)
            threadPool_ = std::make_shared<ThreadPool>(effective);
        pool = threadPool_;
    }
    // Fragment state is coordinate-derived and each worker owns disjoint scanlines,
    // so threaded output matches serial execution byte for byte. Each lane's
    // write range is flushed before the next guest instruction can run.
    if (pool && effective > 1) {
        for (unsigned lane = 0; lane < effective; ++lane) deferredWrites_[lane].reset();
        pool->execute(start, end, [&](u32 y, unsigned lane) {
            DeferredWriteScope scope(deferredWrites_[lane]);
            fn(y, lane);
        });
        for (unsigned lane = 0; lane < effective; ++lane) flushDeferredWrites(lane);
        return;
    }
    runSerial();
}

bool Rdp::resizeFramebuffer(u32 nativeWidth, u32 nativeHeight) {
    if (nativeWidth == 0 || nativeHeight == 0) return false;
    const u64 width = u64{nativeWidth} * resolutionScale_;
    const u64 height = u64{nativeHeight} * resolutionScale_;
    const u64 maxDimension = std::numeric_limits<u32>::max();
    if (width == 0 || height == 0 || width > maxDimension || height > maxDimension) return false;
    const u64 pixels = width * height; // each dimension is at most u32::max
    if (pixels == 0 || pixels > MaxFramebufferPixels) return false;
    // Commit only after every check so a rejected request cannot leave native
    // dimensions larger than the allocated debug surface.
    nativeFbWidth_ = nativeWidth;
    nativeFbHeight_ = nativeHeight;
    fbWidth_ = static_cast<u32>(width);
    fbHeight_ = static_cast<u32>(height);
    fb_.assign(static_cast<std::size_t>(pixels), 0);
    const auto nativePixels = resolutionScale_ > 1 ? std::size_t{nativeWidth} * nativeHeight : 0;
    nativeColor_.assign(nativePixels, 0);
    colorStamp_.assign(nativePixels, 0);
    hasFineSamples_ = false;
    return true;
}

void Rdp::reset() {
    fineSurfaces_.clear();
    colorImage_ = ColorImage{};
    textureImage_ = ColorImage{};
    tiles_ = {};
    tmem_ = {};
    fifoSize_ = commandSize_ = 0;
    scissor_ = Scissor{};
    fillColor_ = 0;
    envColor_ = 0;
    primColor_ = 0;
    blendColor_ = 0;
    fogColor_ = 0;
    depthImage_ = primitiveDepth_ = primitiveDz_ = primitiveLod_ = 0;
    triangleLevel_ = 0;
    hidden_.assign(Memory::RdramSize / 2, 3);
    setCombineMode(0);
    setOtherModes(0);
    resizeFramebuffer(nativeFbWidth_, nativeFbHeight_);
    resetStats();
}

void Rdp::setFramebufferSize(u32 width, u32 height) {
    resizeFramebuffer(width, height);
}

void Rdp::setResolutionScale(u32 scale) {
    if (scale == resolutionScale_ || scale < 1 || scale > 4) return;
    const u32 previous = resolutionScale_;
    resolutionScale_ = scale;
    fineSurfaces_.clear();
    if (!resizeFramebuffer(nativeFbWidth_, nativeFbHeight_)) resolutionScale_ = previous;
}

u32 Rdp::pixel(u32 x, u32 y) const noexcept {
    if (x >= nativeFbWidth_ || y >= nativeFbHeight_) return 0;
    if (resolutionScale_ > 1) return nativeColor_[std::size_t{y} * nativeFbWidth_ + x];
    const u64 ix = u64{x} * resolutionScale_;
    const u64 iy = u64{y} * resolutionScale_;
    if (ix >= fbWidth_ || iy >= fbHeight_) return 0;
    return fb_[static_cast<std::size_t>(iy) * fbWidth_ + static_cast<std::size_t>(ix)];
}

Rdp::ScaledSourceInfo Rdp::scaledSourceInfo() const noexcept {
    return ScaledSourceInfo{colorImage_.address, colorImage_.width, colorImage_.size,
                            colorImage_.fmt, nativeFbWidth_, nativeFbHeight_,
                            bus_ != nullptr, hasFineSamples_};
}

std::vector<Rdp::FineSurfaceSummary> Rdp::fineSurfaceSummaries() const noexcept {
    std::vector<FineSurfaceSummary> result;
    result.reserve(fineSurfaces_.size());
    for (const auto& surface : fineSurfaces_)
        result.push_back(FineSurfaceSummary{surface.address, surface.width, surface.size,
                                            surface.fmt, surface.nativeWidth,
                                            surface.nativeHeight, surface.epoch});
    return result;
}

Rdp::SurfaceView Rdp::findFineSurface(u32 start, u32 stride, u32 width, u32 height,
                                      u32 bytes) const noexcept {
    SurfaceView none{};
    if (bus_ == nullptr || resolutionScale_ <= 1 || width == 0 || height == 0 || bytes == 0)
        return none;
    // VI may scan a sub-rectangle of the RDP surface, so the origin can be a
    // positive byte offset into it. Derive the x/y offset from the buffer layout
    // and reject origins outside the refined surface.
    const auto locate = [&](u32 address, u32 configWidth, u32 size, u32 fmt,
                            u32 nativeWidth, u32 nativeHeight, u32& xOffset, u32& yOffset) {
        xOffset = yOffset = 0;
        if (fmt != 0 || size < 2 || configWidth != stride || nativeWidth != stride || stride == 0 ||
            bytes != (size == 2 ? 2u : 4u) || start < address) return false;
        const u64 delta = u64{start} - address;
        if (delta % bytes != 0) return false;
        const u64 pixelOffset = delta / bytes;
        xOffset = static_cast<u32>(pixelOffset % stride);
        yOffset = static_cast<u32>(pixelOffset / stride);
        if (xOffset + width > nativeWidth || yOffset + height > nativeHeight) return false;
        return u64{start} + (u64{height - 1} * stride + width) * bytes <= RdramLimit;
    };
    const auto sized = [&](u32 nativeWidth, u32 nativeHeight, const std::vector<u32>& fb,
                           const std::vector<u32>& color, const std::vector<u32>& stamp) {
        const u64 nativePixels = u64{nativeWidth} * nativeHeight;
        return color.size() >= nativePixels && stamp.size() >= nativePixels &&
               fb.size() >= nativePixels * resolutionScale_ * resolutionScale_;
    };

    u32 xOffset = 0, yOffset = 0;
    if (hasFineSamples_ &&
        locate(colorImage_.address, colorImage_.width, colorImage_.size, colorImage_.fmt,
               nativeFbWidth_, nativeFbHeight_, xOffset, yOffset) &&
        sized(nativeFbWidth_, nativeFbHeight_, fb_, nativeColor_, colorStamp_)) {
        return SurfaceView{true, colorEpoch_, nativeFbWidth_, nativeFbHeight_, xOffset, yOffset,
                           &fb_, &nativeColor_, &colorStamp_};
    }
    for (auto it = fineSurfaces_.rbegin(); it != fineSurfaces_.rend(); ++it) {
        const auto& surface = *it;
        if (!locate(surface.address, surface.width, surface.size, surface.fmt,
                    surface.nativeWidth, surface.nativeHeight, xOffset, yOffset) ||
            !sized(surface.nativeWidth, surface.nativeHeight, surface.fb,
                   surface.nativeColor, surface.colorStamp)) continue;
        return SurfaceView{true, surface.epoch, surface.nativeWidth, surface.nativeHeight,
                           xOffset, yOffset, &surface.fb, &surface.nativeColor, &surface.colorStamp};
    }
    return none;
}

bool Rdp::canPresentScaled(u32 start, u32 stride, u32 width, u32 height, u32 bytes) const noexcept {
    return findFineSurface(start, stride, width, height, bytes).valid;
}

void Rdp::copyScaledColor(std::span<u32> out, u32 pitch, u32 start, u32 stride,
                          u32 width, u32 height, u32 bytes) const {
    const u32 scale = resolutionScale_;
    const auto view = findFineSurface(start, stride, width, height, bytes);
    if (!view.valid || pitch < width * scale || out.size() < u64{pitch} * height * scale) return;
    constexpr auto rgb = [](u32 c) { return c & 0x00FF'FFFF; };
    const u32 sourceWidth = view.nativeWidth * scale;
    for (u32 y = 0; y < height; ++y) {
        for (u32 x = 0; x < width; ++x) {
            const auto sx = x + view.xOffset;
            const auto syNative = y + view.yOffset;
            const auto index = std::size_t{syNative} * view.nativeWidth + sx;
            const auto addr = start + static_cast<u32>(std::size_t{y} * stride + x) * bytes;
            const u32 guest = bytes == 2 ? expandRgba16(bus_->rdram().loadRdram<u16>(addr))
                                         : fromRegister(bus_->rdram().loadRdram<u32>(addr));
            const bool useSidecar = (*view.colorStamp)[index] == view.epoch &&
                                    rgb(guest) == rgb((*view.nativeColor)[index]);
            for (u32 sy = 0; sy < scale; ++sy) {
                auto* dst = out.data() + (std::size_t{y} * scale + sy) * pitch + x * scale;
                if (useSidecar) {
                    const auto* src = view.fb->data() +
                        (std::size_t{syNative} * scale + sy) * sourceWidth + sx * scale;
                    for (u32 sx = 0; sx < scale; ++sx) dst[sx] = src[sx] | 0xFF00'0000;
                } else {
                    std::fill_n(dst, scale, rgb(guest) | 0xFF00'0000);
                }
            }
        }
        for (u32 sy = 0; sy < scale; ++sy) {
            auto* row = out.data() + (std::size_t{y} * scale + sy) * pitch;
            std::fill(row + width * scale, row + pitch, 0);
        }
    }
}

u64 Rdp::readWord64(u32 address) const {
    if (bus_ == nullptr || hw_ == nullptr) return 0;
    if (hw_->dp().status & 1) address = 0x04000000 | (address & 0xFFF);
    else {
        if (address > Memory::RdramSize - sizeof(u64)) return 0;
        return bus_->rdram().loadRdram<u64>(address);
    }
    return (static_cast<u64>(bus_->read32(address)) << 32) |
           static_cast<u64>(bus_->read32(address + 4));
}

bool Rdp::writePixel(s32 x, s32 y, u32 color, bool fill, bool refine) {
    if (!scissorContains(x, y)) return false;
    if (x < 0 || y < 0 || static_cast<u32>(x) >= nativeFbWidth_ || static_cast<u32>(y) >= nativeFbHeight_) return false;
    // Native-resolution writeback to guest RDRAM. The CPU expects the
    // conventional 1x framebuffer layout, so the address is computed from
    // native coordinates regardless of the internal resolution scale.
    if (bus_ && colorImage_.fmt == 0 && colorImage_.width && colorImage_.size >= 2) {
        const u32 bytes = colorImage_.size == 2 ? 2 : 4;
        const u64 nativeIndex = u64{static_cast<u32>(y)} * nativeFbWidth_ + static_cast<u32>(x);
        const u64 address = u64{colorImage_.address} + nativeIndex * bytes;
        if (address <= RdramLimit - bytes) {
            const u32 physical = static_cast<u32>(address);
            const auto store = [&](auto value) {
                if (activeDeferredWrites)
                    bus_->rdram().storeRdramDeferred(physical, value, *activeDeferredWrites);
                else
                    bus_->rdram().storeRdram(physical, value);
            };
            if (bytes == 2) {
                const u16 rgba = fill ? static_cast<u16>(color >> ((physical & 2) ? 0 : 16)) :
                    ((color & 0xF8) << 8) | ((color & 0xF800) >> 5) |
                    ((color & 0xF80000) >> 18) | (color >> 31);
                store(rgba);
                if (fill) hidden_[physical / 2] = (rgba & 1) ? 3 : 0;
                color = expandRgba16(rgba);
            } else {
                const u32 rgba = fill ? color : ((color & 0xFF) << 24) | ((color & 0xFF00) << 8) |
                                                 ((color >> 8) & 0xFF00) | (color >> 24);
                store(rgba);
                if (fill) color = packRgba(rgba >> 24, rgba >> 16, rgba >> 8, rgba);
            }
        }
    }
    // Upscale the native fragment into a resolutionScale_ x resolutionScale_
    // block in the internal debug framebuffer. Bounds are checked against the
    // scaled surface so CPU readback of RDRAM never overflows.
    const u64 baseX = u64{static_cast<u32>(x)} * resolutionScale_;
    const u64 baseY = u64{static_cast<u32>(y)} * resolutionScale_;
    if (baseX >= fbWidth_ || baseY >= fbHeight_) return false;
    if (resolutionScale_ == 1) {
        fb_[static_cast<std::size_t>(baseY) * fbWidth_ + static_cast<std::size_t>(baseX)] = color;
    } else {
        const auto index = std::size_t{static_cast<u32>(y)} * nativeFbWidth_ + static_cast<u32>(x);
        nativeColor_[index] = color;
        colorStamp_[index] = colorEpoch_;
        if (refine) return true; // The calling scanline will write the fine samples.
        for (u32 dy = 0; dy < resolutionScale_; ++dy) {
            const u64 iy = baseY + dy;
            if (iy >= fbHeight_) break;
            std::fill_n(fb_.data() + static_cast<std::size_t>(iy) * fbWidth_ + baseX, resolutionScale_, color);
        }
    }
    return true;
}

bool Rdp::scissorContains(s32 x, s32 y) const noexcept {
    return x >= scissor_.minX && x < scissor_.maxX && y >= scissor_.minY && y < scissor_.maxY;
}

std::size_t Rdp::commandWordCount(u32 op) noexcept {
    if (op >= 8 && op <= 15)
        return 4 + (op & 4 ? 8 : 0) + (op & 2 ? 8 : 0) + (op & 1 ? 2 : 0);
    if (op == 0x24 || op == 0x25) return 2;
    return 1;
}

void Rdp::executeCommand(const u64* words, std::size_t count) {
    if (count == 0) return;
    const u64 cmdWord = words[0];
    const u64 cmdLow = cmdWord & 0xFFFFFFFFULL;
    const u32 op = cmdId(cmdWord);
    statsCommandsProcessed_.fetch_add(1, std::memory_order_relaxed);
    if (hw_) hw_->dp().status |= 0x20;
    switch (op) {
        case 0x08: case 0x09: case 0x0A: case 0x0B:
        case 0x0C: case 0x0D: case 0x0E: case 0x0F:
            cmdFillTriangle(cmdWord, words + 1, count - 1);
            break;
        case 0x24: // Texture Rectangle
        case 0x25: // Texture Rectangle Flip
            if (count >= 2) cmdTextureRectangle(cmdWord, words[1], op == 0x25);
            break;
        case 0x26: case 0x27: case 0x28:
            // Deterministic stalls; nothing to do for a reference renderer.
            break;
        case 0x29:
            cmdSyncFull();
            break;
        case 0x2D:
            cmdSetScissor(cmdWord);
            break;
        case 0x2E:
            primitiveDepth_ = static_cast<u32>(((cmdLow >> 16) & 0x7FFF) << 3);
            primitiveDz_ = cmdLow & 0xFFFF;
            break;
        case 0x2F:
            cmdSetOtherModes(cmdWord, cmdLow);
            break;
        case 0x30:
            cmdLoadTlut(cmdWord);
            break;
        case 0x32:
            cmdSetTileSize(cmdWord);
            break;
        case 0x33:
        case 0x34:
            cmdLoadTexture(cmdWord, op == 0x33);
            break;
        case 0x35:
            cmdSetTile(cmdWord);
            break;
        case 0x36:
            cmdFillRectangle(cmdWord);
            break;
        case 0x37:
            cmdSetFillColor(cmdLow);
            break;
        case 0x38:
            cmdSetFogColor(cmdLow);
            break;
        case 0x39:
            cmdSetBlendColor(cmdLow);
            break;
        case 0x3A:
            cmdSetPrimColor(cmdWord, cmdLow);
            break;
        case 0x3B:
            cmdSetEnvColor(cmdLow);
            break;
        case 0x3C:
            cmdSetCombineMode(cmdWord, cmdLow);
            break;
        case 0x3D:
            cmdSetTextureImage(cmdWord, cmdLow);
            break;
        case 0x3E:
            cmdSetDepthImage(cmdLow);
            break;
        case 0x3F:
            cmdSetColorImage(cmdWord, cmdLow);
            break;
        default:
            break;
    }
}

void Rdp::submitCommands(const u64* words, std::size_t count) {
    while (count) {
        const u32 op = cmdId(words[0]);
        const std::size_t size = std::min(commandWordCount(op), count);
        executeCommand(words, size);
        words += size;
        count -= size;
    }
}

void Rdp::process() {
    if (hw_ == nullptr) return;
    if (hw_->dp().status & 4) fifoSize_ = 0;
    if (hw_->dp().status & 6) return;

    u32 cur = hw_->dp().current;
    const u32 end = hw_->dp().end;

    // Compare in u64: cur + 8 wraps in u32 near UINT32_MAX and would read a
    // second, unrelated ring at address zero.
    while (u64{cur} + 8 <= u64{end}) {
        if (!fifoSize_) {
            fifo_[fifoSize_++] = readWord64(cur);
            cur += 8;
            const u32 op = cmdId(fifo_[0]);
            commandSize_ = 1;
            if (op >= 8 && op <= 15)
                commandSize_ = 4 + (op & 4 ? 8 : 0) + (op & 2 ? 8 : 0) + (op & 1 ? 2 : 0);
            else if (op == 0x24 || op == 0x25)
                commandSize_ = 2;
        }
        while (fifoSize_ < commandSize_ && u64{cur} + 8 <= u64{end}) {
            fifo_[fifoSize_++] = readWord64(cur);
            cur += 8;
        }
        if (fifoSize_ < commandSize_) break;
        executeCommand(fifo_.data(), commandSize_);
        fifoSize_ = 0;
    }

    hw_->dp().current = cur;
}

void Rdp::cmdSetColorImage(u64 word0, u64 word1) {
    const auto previous = colorImage_;
    const ColorImage next{static_cast<u32>((word0 >> 53) & 0x7),
                          static_cast<u32>((word0 >> 51) & 0x3),
                          static_cast<u32>(((word0 >> 32) & 0x3FF) + 1),
                          static_cast<u32>(word1 & 0x3FFFFFF)};
    const bool changed = previous.address != next.address || previous.width != next.width ||
                         previous.size != next.size || previous.fmt != next.fmt;
    if (resolutionScale_ > 1 && changed && hasFineSamples_) {
        // Preserve the retiring buffer's sidecar before the shared working
        // surfaces start receiving the next buffer's fragments.
        fineSurfaces_.push_back(FineSurface{previous.address, previous.width, previous.size,
                                            previous.fmt, nativeFbWidth_, nativeFbHeight_,
                                            colorEpoch_, fb_, nativeColor_, colorStamp_});
        constexpr std::size_t kMaxFineSurfaces = 4;
        if (fineSurfaces_.size() > kMaxFineSurfaces)
            fineSurfaces_.erase(fineSurfaces_.begin(),
                                fineSurfaces_.begin() + (fineSurfaces_.size() - kMaxFineSurfaces));
    }
    if (changed) {
        hasFineSamples_ = false;
        if (++colorEpoch_ == 0) colorEpoch_ = 1;
        std::fill(colorStamp_.begin(), colorStamp_.end(), 0);
    }
    colorImage_ = next;
    // Keep internal stride consistent with the color-image width. An invalid
    // guest width is ignored rather than leaving native dimensions larger than
    // the allocated debug surface.
    if (colorImage_.width != nativeFbWidth_)
        resizeFramebuffer(colorImage_.width, nativeFbHeight_);
}

void Rdp::cmdSetDepthImage(u64 word1) {
    depthImage_ = word1 & 0x3FFFFFF;
}

void Rdp::cmdSetTextureImage(u64 word0, u64 word1) {
    textureImage_ = {static_cast<u32>((word0 >> 53) & 7), static_cast<u32>((word0 >> 51) & 3),
                     static_cast<u32>(((word0 >> 32) & 1023) + 1), static_cast<u32>(word1 & 0x3FFFFFF)};
}

void Rdp::cmdSetTile(u64 w) {
    auto& tile = tiles_[(w >> 24) & 7];
    tile.fmt = (w >> 53) & 7;
    tile.size = (w >> 51) & 3;
    tile.line = (w >> 41) & 511;
    tile.base = (w >> 32) & 511;
    tile.palette = (w >> 20) & 15;
    tile.tMode = (w >> 18) & 3;
    tile.tMask = (w >> 14) & 15;
    tile.tShift = (w >> 10) & 15;
    tile.sMode = (w >> 8) & 3;
    tile.sMask = (w >> 4) & 15;
    tile.sShift = w & 15;
}

void Rdp::cmdSetTileSize(u64 w) {
    auto& tile = tiles_[(w >> 24) & 7];
    tile.sl = (w >> 44) & 4095;
    tile.tl = (w >> 32) & 4095;
    tile.sh = (w >> 12) & 4095;
    tile.th = w & 4095;
}

void Rdp::cmdLoadTexture(u64 w, bool block) {
    cmdSetTileSize(w);
    const auto& tile = tiles_[(w >> 24) & 7];
    if (!bus_ || textureImage_.size != tile.size || tile.fmt == 1) return;
    const u32 bits = 4u << tile.size;
    const auto store = [&](u64 base, u32 x, bool odd, u32 value) {
        const u64 bank = odd ? 2u : 0u;
        if (bits == 32) {
            const u32 word = static_cast<u32>(((base + x) ^ bank) & 1023u);
            tmem_[word] = value >> 16;
            tmem_[word | 1024] = value;
        } else {
            const u32 bit = x * bits;
            const u32 word = static_cast<u32>(((base + bit / 16) ^ bank) & 2047u);
            const u32 shift = 16 - bits - (bit & 15);
            const u32 mask = ((1u << bits) - 1) << shift;
            tmem_[word] = (tmem_[word] & ~mask) | (value << shift);
        }
    };
    if (block) {
        const u32 count = (tile.sh - tile.sl + 1) & 0xFFF;
        const u32 perWord = 64 / bits;
        const u64 start = u64{tile.tl} * textureImage_.width + tile.sl;
        for (u32 i = 0; i < count; i += perWord) {
            // DXT advances T once per 64-bit transfer. T contributes the
            // programmed tile stride as well as the odd-row bank swap; the
            // source continues linearly in RDRAM.
            const u32 row = (i / perWord * tile.th) >> 11;
            const u64 base = (u64{tile.base} + u64{row} * tile.line) * 4;
            for (u32 j = 0; j < perWord; ++j)
                store(base, i + j, row & 1, readTextureImage(start + i + j));
        }
    } else {
        const u32 s = tile.sl >> 2, t = tile.tl >> 2;
        const u32 endS = tile.sh >> 2, endT = tile.th >> 2;
        for (u32 y = t; y <= endT; ++y) {
            const u32 row = y - t;
            for (u32 x = s; x <= endS; ++x)
                store((u64{tile.base} + u64{row} * tile.line) * 4, x - s, row & 1,
                      readTextureImage(u64{y} * textureImage_.width + x));
        }
    }
}

u32 Rdp::readTextureImage(u64 index) const {
    const u32 bits = 4u << textureImage_.size;
    const u64 access = std::max<u64>(1ULL, bits / 8);
    const u64 address = u64{textureImage_.address} + (index * bits) / 8;
    if (address > RdramLimit - access) return 0;
    const u32 physical = static_cast<u32>(address);
    const auto& ram = bus_->rdram();
    if (bits == 32) return ram.read32(physical);
    if (bits == 16) return ram.read16(physical);
    if (bits == 8) return ram.read8(physical);
    return (ram.read8(physical) >> ((index & 1) ? 0 : 4)) & 15;
}

void Rdp::cmdLoadTlut(u64 w) {
    cmdSetTileSize(w);
    const auto& tile = tiles_[(w >> 24) & 7];
    if (!bus_ || textureImage_.size != 2 || tile.tl != tile.th) return;
    for (u32 s = tile.sl >> 2; s <= (tile.sh >> 2); ++s) {
        const u16 entry = readTextureImage(u64{tile.tl >> 2} * textureImage_.width + s);
        const u32 word = static_cast<u32>(((u64{tile.base} + s - (tile.sl >> 2)) & 511u) * 4);
        std::fill_n(tmem_.begin() + word, 4, entry);
    }
}

u32 Rdp::readTexel(const Tile& tile, u32 s, u32 t) const {
    const u32 bits = 4u << tile.size;
    const u32 base = (tile.base + tile.line * t) * 4;
    const u32 swap = (t & 1) * 2;
    if (bits == 32 && tile.fmt == 0) {
        // One 16-bit word per bank per texel, at the same address in each
        // half of TMEM. Doubling S aliases the following row at tight stride.
        const u32 w = ((base + s) ^ swap) & 1023;
        return fromRegister((u32{tmem_[w]} << 16) | tmem_[w | 1024]);
    }
    if (bits > 16) return 0;
    const u32 bit = s * bits;
    const bool tlut = otherModes_ & (1ULL << 47);
    const u32 word = ((base + bit / 16) ^ swap) & (tlut ? 1023 : 2047);
    u32 value = (tmem_[word] >> (16 - bits - (bit & 15))) & ((1u << bits) - 1);
    const auto ia = [](u32 i, u32 a) { return packRgba(i, i, i, a); };
    if (tlut && bits <= 8) {
        if (bits == 4) value |= tile.palette << 4;
        const u16 entry = tmem_[1024 + value * 4];
        return otherModes_ & (1ULL << 46) ? ia(entry >> 8, entry & 255) : expandRgba16(entry);
    }
    if (tile.fmt == 0 && bits == 16) return expandRgba16(value);
    if (tile.fmt == 3) {
        if (bits == 16) return ia(value >> 8, value & 255);
        if (bits == 8) return ia((value >> 4) * 17, (value & 15) * 17);
        const u32 i = value >> 1;
        return ia((i << 5) | (i << 2) | (i >> 1), (value & 1) * 255);
    }
    if (tile.fmt == 4 || ((tile.fmt == 0 || tile.fmt == 2) && bits <= 8)) {
        if (bits == 4) value *= 17;
        return ia(value, value);
    }
    return 0; // YUV conversion is not implemented.
}

u32 Rdp::sampleTile(const Tile& tile, s64 s, s64 t, bool filter) const {
    // Coordinates are in 1/32 texel units; tile extents are in 1/4 texels.
    // The RDP shifts a 16-bit signed coordinate. Amounts >= 11 therefore wrap
    // to a left shift of 16 - amount instead of growing the s64 value.
    const auto shift = [](s64 n, u32 amount) -> s64 {
        const s16 value = static_cast<s16>(n);
        return amount < 11 ? value >> amount
                           : static_cast<s16>(static_cast<u16>(value) << (16 - amount));
    };
    s = shift(s, tile.sShift) - tile.sl * 8;
    t = shift(t, tile.tShift) - tile.tl * 8;
    const auto coord = [](s64 n, u32 mode, u32 mask) {
        n >>= 5;
        if (mask) {
            const u32 span = 1u << std::min(mask, 10u);
            if ((mode & 1) && (n & span)) n = ~n;
            n &= span - 1;
        }
        return static_cast<u32>(n);
    };
    const s32 sw = (static_cast<s32>(tile.sh) - static_cast<s32>(tile.sl)) * 8;
    const s32 th = (static_cast<s32>(tile.th) - static_cast<s32>(tile.tl)) * 8;
    // Clamp before splitting off the filter fractions. Outside the tile the
    // RDP clears that axis's fraction, rather than blending the edge texel
    // with the next texel using the original out-of-range fraction.
    const auto clampSample = [](s64 n, u32 mode, u32 mask, s32 extent, u32 lo, u32 hi) {
        if (!(mode & 2) && mask) return n;
        if (n < 0) return s64{0};
        if (n >= extent) return s64{((hi >> 2) - (lo >> 2)) & 1023} * 32;
        return n;
    };
    const auto fetch = [&](s64 x, s64 y) {
        return readTexel(tile, coord(x, tile.sMode, tile.sMask), coord(y, tile.tMode, tile.tMask));
    };
    if (filter) {
        s = clampSample(s, tile.sMode, tile.sMask, sw, tile.sl, tile.sh);
        t = clampSample(t, tile.tMode, tile.tMask, th, tile.tl, tile.th);
    } else {
        if ((tile.sMode & 2) || !tile.sMask) s = std::clamp<s64>(s, 0, std::max(0, sw));
        if ((tile.tMode & 2) || !tile.tMask) t = std::clamp<s64>(t, 0, std::max(0, th));
        return fetch(s, t);
    }
    // The taps are adjacent to the clamped base. Wrap/mirror applies to each
    // tap, but the high tile bound does not independently clamp the +1 taps.
    const u32 a = fetch(s, t);
    const u32 b = fetch(s + 32, t), c = fetch(s, t + 32), d = fetch(s + 32, t + 32);
    const s32 fs = s & 31, ft = t & 31;
    u32 result = 0;
    for (u32 i = 0; i < 4; ++i) {
        const s32 A = channel(a, i), B = channel(b, i), C = channel(c, i), D = channel(d, i);
        const s32 n = (fs == 16 && ft == 16 && (otherModes_ & (1ULL << 44))) ?
            (A + B + C + D + 2) >> 2 :
            fs + ft < 32 ? A + (((B - A) * fs + (C - A) * ft + 16) >> 5) :
                           D + (((C - D) * (32 - fs) + (B - D) * (32 - ft) + 16) >> 5);
        result |= static_cast<u32>(n) << (i * 8);
    }
    return result;
}

void Rdp::cmdTextureRectangle(u64 w, u64 params, bool flip) {
    const u32 index = (w >> 24) & 7;
    const u32 cycle = (otherModes_ >> 52) & 3;
    if (cycle == 3) {
        cmdFillRectangle(w);
        return;
    }
    const bool copy = cycle == 2;
    const s32 x0 = u10_2ToPixel((w >> 12) & 4095), y0 = u10_2ToPixel(w & 4095);
    const s32 x1 = u10_2ToPixel((w >> 44) & 4095) + copy;
    const s32 y1 = u10_2ToPixel((w >> 32) & 4095) + copy;
    const s32 s0 = static_cast<s16>(params >> 48) * 32, t0 = static_cast<s16>(params >> 32) * 32;
    const s32 ds = static_cast<s16>(params >> 16) >> (copy ? 2 : 0);
    const s32 dt = static_cast<s16>(params);
    const bool quad = otherModes_ & (1ULL << 45);
    const bool noise = fragmentUsesNoise(cycle);
    const s32 left = std::max(x0, scissor_.minX);
    const s32 right = std::min({x1, scissor_.maxX, s32(nativeFbWidth_)});
    const s32 top = std::max(y0, scissor_.minY);
    const s32 bottom = std::min({y1, scissor_.maxY, s32(nativeFbHeight_)});
    if (left >= right) return;
    executeSpan(top, bottom, right - left, !copy && (otherModes_ & 4) && (otherModes_ & 0x30),
                [&](u32 row, unsigned) {
        const s32 y = static_cast<s32>(row);
        u32 pixels = 0;
        for (s32 x = left; x < right; ++x) {
            const s64 s = (s64{s0} + s64{flip ? y - y0 : x - x0} * ds) >> 5;
            const s64 t = (s64{t0} + s64{flip ? x - x0 : y - y0} * dt) >> 5;
            const u32 tex0 = sampleTile(tiles_[index], s, t, !copy && quad && (otherModes_ & (1ULL << 43)));
            const u32 tex1 = cycle == 1 ? sampleTile(tiles_[(index + 1) & 7], s, t,
                                               quad && (otherModes_ & (1ULL << 42))) : 0;
            pixels += drawFragment(x, y, 0, tex0, tex1, 8, true, 0, 0, false,
                                   noise ? fragmentNoise(x, y) : 0);
        }
        statsPixelsDrawn_.fetch_add(pixels, std::memory_order_relaxed);
    });
}

void Rdp::cmdSetScissor(u64 word0) {
    scissor_.minX = static_cast<s32>(u10_2ToPixel((word0 >> 44) & 0xFFF));
    scissor_.minY = static_cast<s32>(u10_2ToPixel((word0 >> 32) & 0xFFF));
    scissor_.maxX = static_cast<s32>(u10_2ToPixel((word0 >> 12) & 0xFFF));
    scissor_.maxY = static_cast<s32>(u10_2ToPixel(word0 & 0xFFF));
}

void Rdp::cmdSetFillColor(u64 word1) { fillColor_ = static_cast<u32>(word1); }
void Rdp::cmdSetEnvColor(u64 word1) { envColor_ = static_cast<u32>(word1); }
void Rdp::cmdSetBlendColor(u64 word1) { blendColor_ = static_cast<u32>(word1); }
void Rdp::cmdSetFogColor(u64 word1) { fogColor_ = static_cast<u32>(word1); }

void Rdp::cmdSetPrimColor(u64 word0, u64 word1) {
    primitiveLod_ = (word0 >> 32) & 255;
    primColor_ = static_cast<u32>(word1);
}

void Rdp::cmdSetCombineMode(u64 word0, u64 word1) {
    setCombineMode((word0 & 0x00FFFFFF00000000ULL) | (word1 & 0xFFFFFFFF));
}

void Rdp::cmdSetOtherModes(u64 word0, u64 word1) {
    setOtherModes((word0 & 0x00FFFFFF00000000ULL) | (word1 & 0xFFFFFFFF));
}

void Rdp::cmdSyncFull() {
    statsFullSyncs_.fetch_add(1, std::memory_order_relaxed);
    if (hw_ == nullptr) return;
    hw_->dp().status &= ~0x20u;
    hw_->mi().setPending(MI_INTR_DP);
}

void Rdp::setFillColor(u32 color) { fillColor_ = color; }
void Rdp::setEnvColor(u32 color) { envColor_ = color; }
void Rdp::setPrimColor(u32 /*minLevel*/, u32 lodFrac, u32 color) {
    primitiveLod_ = lodFrac & 255;
    primColor_ = color;
}
void Rdp::setBlendColor(u32 color) { blendColor_ = color; }
void Rdp::setFogColor(u32 color) { fogColor_ = color; }
void Rdp::setCombineMode(u64 mode) {
    const auto select = [&](u32 shift, u32 mask) { return static_cast<u8>((mode >> shift) & mask); };
    combineMux_[0] = {{select(52, 15), select(28, 15), select(47, 31), select(15, 7)},
                      {select(44, 7), select(12, 7), select(41, 7), select(9, 7)}};
    combineMux_[1] = {{select(37, 15), select(24, 15), select(32, 31), select(6, 7)},
                      {select(21, 7), select(3, 7), select(18, 7), select(0, 7)}};
}
void Rdp::setOtherModes(u64 mode) {
    otherModes_ = mode;
    for (u32 i = 0; i < 2; ++i) {
        const auto select = [&](u32 shift) { return static_cast<u8>((mode >> (shift - i * 2)) & 3); };
        blendMux_[i] = {select(30), select(26), select(22), select(18)};
    }
}

void Rdp::setScissor(s32 minX, s32 minY, s32 maxX, s32 maxY) {
    scissor_.minX = minX;
    scissor_.minY = minY;
    scissor_.maxX = maxX;
    scissor_.maxY = maxY;
}

void Rdp::fillRectangle(s32 xl, s32 yl, s32 xr, s32 yr) {
    const s32 x0 = std::max(xl, scissor_.minX);
    const s32 y0 = std::max(yl, scissor_.minY);
    const s32 x1 = std::min(xr, scissor_.maxX);
    const s32 y1 = std::min(yr, scissor_.maxY);
    statsFillRectangles_.fetch_add(1, std::memory_order_relaxed);
    u64 pixels = 0;
    for (s32 y = y0; y < y1; ++y) {
        for (s32 x = x0; x < x1; ++x) {
            pixels += writePixel(x, y, fillColor_);
        }
    }
    statsPixelsDrawn_.fetch_add(pixels, std::memory_order_relaxed);
}

void Rdp::drawFlatTriangle(s32 x0, s32 y0, s32 x1, s32 y1, s32 x2, s32 y2, u32 color) {
    const s64 ax0 = x0, ay0 = y0, ax1 = x1, ay1 = y1, ax2 = x2, ay2 = y2;
    const s64 minX = std::min({ax0, ax1, ax2});
    const s64 minY = std::min({ay0, ay1, ay2});
    const s64 maxX = std::max({ax0, ax1, ax2});
    const s64 maxY = std::max({ay0, ay1, ay2});

    // All endpoint deltas are formed in s64, then kept as sign + magnitude so
    // even INT_MIN..INT_MAX triangles cannot overflow an edge product.
    const s64 dx10 = ax1 - ax0, dy10 = ay1 - ay0;
    const s64 dx20 = ax2 - ax0, dy20 = ay2 - ay0;
    const s64 dx21 = ax2 - ax1, dy21 = ay2 - ay1;
    const WideSigned area = wideSubtract(wideProduct(dx10, dy20), wideProduct(dy10, dx20));
    if (area.sign == 0) return;

    statsTriangles_.fetch_add(1, std::memory_order_relaxed);

    // Clip in s64 before walking rows. The bounding box of an unclipped
    // extreme triangle would take billions of iterations, all rejected later.
    const s64 clipMinX = std::max({minX, s64{scissor_.minX}, s64{0}});
    const s64 clipMinY = std::max({minY, s64{scissor_.minY}, s64{0}});
    const s64 clipMaxX = std::min({maxX, s64{scissor_.maxX}, s64{nativeFbWidth_} - 1});
    const s64 clipMaxY = std::min({maxY, s64{scissor_.maxY}, s64{nativeFbHeight_} - 1});
    if (clipMinX > clipMaxX || clipMinY > clipMaxY) return;

    // Top-left fill rule: a sample exactly on an edge is covered only if that
    // edge is a "top" (horizontal, triangle below) or "left" (triangle right)
    // edge. This guarantees adjacent triangles share boundary pixels without
    // gaps or double-draws, matching hardware's deterministic rasterization.
    const bool positive = area.sign > 0;
    const auto edgeInclusive = [&](s64 edgeDx, s64 edgeDy) {
        if (edgeDy == 0) return positive ? edgeDx > 0 : edgeDx < 0;
        return positive ? edgeDy < 0 : edgeDy > 0;
    };

    u64 pixels = 0;
    for (s64 y = clipMinY; y <= clipMaxY; ++y) {
        for (s64 x = clipMinX; x <= clipMaxX; ++x) {
            const WideSigned w0 = wideSubtract(wideProduct(dx10, y - ay0),
                                               wideProduct(dy10, x - ax0));
            const WideSigned w1 = wideSubtract(wideProduct(dx21, y - ay1),
                                               wideProduct(dy21, x - ax1));
            const WideSigned w2 = wideSubtract(wideProduct(-dx20, y - ay2),
                                               wideProduct(-dy20, x - ax2));
            const auto test = [&](const WideSigned& w, bool inclusive) {
                return positive ? (inclusive ? w.sign >= 0 : w.sign > 0)
                                : (inclusive ? w.sign <= 0 : w.sign < 0);
            };
            const bool inside =
                test(w0, w0.sign == 0 && edgeInclusive(dx10, dy10)) &&
                test(w1, w1.sign == 0 && edgeInclusive(dx21, dy21)) &&
                test(w2, w2.sign == 0 && edgeInclusive(-dx20, -dy20));
            if (inside) pixels += writePixel(static_cast<s32>(x), static_cast<s32>(y), color);
        }
    }
    statsPixelsDrawn_.fetch_add(pixels, std::memory_order_relaxed);
}

void Rdp::cmdFillRectangle(u64 word0) {
    const s32 ulx = static_cast<s32>(u10_2ToPixel((word0 >> 12) & 0xFFF));
    const s32 uly = static_cast<s32>(u10_2ToPixel(word0 & 0xFFF));
    const s32 lrx = static_cast<s32>(u10_2ToPixel((word0 >> 44) & 0xFFF));
    const s32 lry = static_cast<s32>(u10_2ToPixel((word0 >> 32) & 0xFFF));

    const s32 x0 = std::max(ulx, scissor_.minX);
    const s32 y0 = std::max(uly, scissor_.minY);
    const u32 cycle = (otherModes_ >> 52) & 3;
    if (cycle == 2) return; // Fill Rectangle is not valid in COPY mode.
    const s32 x1 = std::min({lrx + (cycle == 3), scissor_.maxX, s32(nativeFbWidth_)});
    const s32 y1 = std::min({lry + (cycle == 3), scissor_.maxY, s32(nativeFbHeight_)});
    statsFillRectangles_.fetch_add(1, std::memory_order_relaxed);

    if (y0 < y1) {
        const bool noise = fragmentUsesNoise(cycle);
        const auto constant = !noise && cycle < 2 ? combine(0, 0, 0, cycle == 1, 0) : CombineResult{};
        executeSpan(y0, y1, x1 - x0, cycle != 3 && (otherModes_ & 4) && (otherModes_ & 0x30),
                    [&](u32 y, unsigned) {
            u32 pixels = 0;
            for (s32 x = x0; x < x1; ++x) {
                pixels += drawFragment(x, static_cast<s32>(y), 0, 0, 0, 8, true, 0, 0, false,
                                       noise ? fragmentNoise(x, static_cast<s32>(y)) : 0,
                                       noise ? nullptr : &constant);
            }
            statsPixelsDrawn_.fetch_add(pixels, std::memory_order_relaxed);
        });
    }
}

Rdp::CombineResult Rdp::combine(u32 shade, u32 tex0, u32 tex1, bool twoCycle, u32 noise,
                               u32 lodFraction) const {
    std::array<s32, 4> combined{};
    std::array<u32, 6> input{0, tex0, tex1, fromRegister(primColor_), shade, fromRegister(envColor_)};
    const auto mux = [&](u32 select, u32 i) -> s32 {
        return select == 0 ? combined[i] : select < input.size() ? channel(input[select], i) : 0;
    };
    const auto rgb = [&](u32 slot, u32 select, u32 i) -> s32 {
        if (select < 6) return mux(select, i);
        // RGB source 6 is the constant 1.0 in every combiner slot.
        if (select == 6) return 256;
        if (slot == 0 && select == 7) return ((noise >> 23) & 0x1C0) | 32;
        if (slot == 2 && select >= 7 && select <= 12) return mux(select - 7, 3);
        if (slot == 2 && select == 13) return lodFraction;
        if (slot == 2 && select == 14) return primitiveLod_;
        return 0;
    };
    const auto alpha = [&](bool mul, u32 select) -> s32 {
        if (mul && select == 0) return lodFraction;
        if (select == 6) return mul ? primitiveLod_ : 256;
        return mux(select, 3);
    };
    const auto eval = [](s32 a, s32 b, s32 c, s32 d) {
        // No intermediate clamp: 2-cycle results feed the next cycle at full
        // precision and are saturated only after the final cycle.
        return ((a - b) * c + d * 256 + 128) >> 8;
    };
    CombineResult result;
    for (u32 cycle = twoCycle ? 0 : 1; cycle < 2; ++cycle) {
        const auto& [r, a] = combineMux_[cycle];
        std::array<s32, 4> output{};
        for (u32 i = 0; i < 3; ++i)
            output[i] = eval(rgb(0, r[0], i), rgb(1, r[1], i), rgb(2, r[2], i), rgb(3, r[3], i));
        output[3] = eval(alpha(false, a[0]), alpha(false, a[1]), alpha(true, a[2]), alpha(false, a[3]));
        combined = output;
        if (cycle == 0) {
            result.alpha0 = static_cast<u8>(std::clamp(output[3], 0, 255));
            // RH#001/RH#002: TEXEL1 advances into TEXEL0 between cycles.
            std::swap(input[1], input[2]);
        }
    }
    result.color = packRgba(std::clamp(combined[0], 0, 255), std::clamp(combined[1], 0, 255),
                            std::clamp(combined[2], 0, 255), std::clamp(combined[3], 0, 255));
    if (!twoCycle) result.alpha0 = static_cast<u8>(std::clamp(combined[3], 0, 255));
    return result;
}

bool Rdp::drawFragment(s32 x, s32 y, u32 shade, u32 tex0, u32 tex1,
                       u32 coverage, bool firstSample, s32 z, u32 dz, bool hasZ, u32 noise,
                       const CombineResult* precombined, u32 lodFraction, bool refine) {
    if (x < 0 || y < 0 || static_cast<u32>(x) >= nativeFbWidth_ ||
        static_cast<u32>(y) >= nativeFbHeight_) return false;
    const u32 cycle = (otherModes_ >> 52) & 3;
    if (cycle == 3) {
        return writePixel(x, y, fillColor_, true);
    }
    const auto threshold = [&] { return (otherModes_ & 2) ? (noise >> 24) : (blendColor_ & 255); };
    u32 color;
    u8 alphaRef;
    if (cycle == 2) {
        color = tex0;
        alphaRef = static_cast<u8>(channel(color, 3));
        if ((otherModes_ & 1) && alphaRef < threshold()) return false;
        return writePixel(x, y, color);
    }
    const CombineResult result = precombined ? *precombined : combine(shade, tex0, tex1, cycle == 1, noise, lodFraction);
    color = result.color;
    const bool timesAlpha = (otherModes_ & 0x1000) != 0;
    const bool selectCoverage = (otherModes_ & 0x2000) != 0;
    const auto expandAlpha = [](u32 alpha) { return alpha + ((alpha + 1) >> 8); };
    const auto alphaFromCoverage = [&](u32 alpha) {
        return timesAlpha ? (expandAlpha(alpha) * coverage + 4) >> 3 : coverage << 5;
    };
    // In two-cycle mode compare uses the first-cycle alpha fixed up against
    // geometric coverage; only the final cycle changes the stored coverage.
    const u32 firstAlpha = selectCoverage ? alphaFromCoverage(result.alpha0) : result.alpha0;
    const u32 finalAlpha = channel(color, 3);
    const u32 modulated = alphaFromCoverage(finalAlpha);
    if (timesAlpha) coverage = modulated >> 5;
    u32 blendAlpha = selectCoverage ? modulated : finalAlpha;
    blendAlpha = std::min(255u, blendAlpha);
    color = (color & 0xFFFFFF) | (blendAlpha << 24);
    alphaRef = static_cast<u8>(cycle == 1 ? std::min(255u, firstAlpha) : blendAlpha);
    if ((otherModes_ & 1) && alphaRef < threshold()) return false;

    // With AA disabled the hardware rejects a fragment when coverage bit 0 is
    // clear, not when its pixel-center sample is outside the triangle. The
    // caller supplies that exact geometric bit for raw triangle spans.
    const bool aa = otherModes_ & 8;
    if (!aa ? !firstSample : !coverage) return false;

    // Guest address arithmetic is widened: color/depth image bases are 26-bit
    // values and a large guest width used to wrap before the bounds check.
    const u64 index = u64{static_cast<u32>(y)} * colorImage_.width + static_cast<u32>(x);
    const u32 bytes = colorImage_.size == 2 ? 2 : 4;
    const u64 address = u64{colorImage_.address} + index * bytes;
    const bool colorInRange = address <= RdramLimit - bytes;
    const u32 addr = colorInRange ? static_cast<u32>(address) : 0;
    u32 mem = 0, memCoverage = 7;
    if (bus_ && colorInRange && (otherModes_ & 0x40)) {
        if (bytes == 2) {
            const u16 raw = bus_->rdram().read16(addr);
            mem = expandRgba16(raw);
            memCoverage = ((raw & 1) << 2) | hidden_[addr / 2];
        } else {
            mem = fromRegister(bus_->rdram().read32(addr));
            memCoverage = channel(mem, 3) >> 5;
        }
    }
    const bool overflow = coverage + memCoverage >= 8;
    bool blend = (otherModes_ & 0x4000) || (aa && !overflow);
    const u64 zAddress = u64{depthImage_} + index * 2;
    const bool depthInRange = zAddress <= RdramLimit - 2;
    const u32 zaddr = depthInRange ? static_cast<u32>(zAddress) : 0;
    if (otherModes_ & 4) {
        z = primitiveDepth_;
        dz = primitiveDz_;
        hasZ = true;
    }
    if (hasZ && bus_ && depthInRange && (otherModes_ & 0x30)) {
        if (otherModes_ & 0x10) {
            const u16 raw = bus_->rdram().loadRdram<u16>(zaddr);
            const s32 exp = raw >> 13;
            const s32 oldZ = 0x40000 - (0x40000 >> exp) + (((raw >> 2) & 0x7FF) << std::max(6 - exp, 0));
            const bool farthest = oldZ == 0x3FFFF;
            const bool front = z < oldZ;
            u32 oldDz = 1u << (((raw & 3) << 2) | hidden_[zaddr / 2]);
            const bool coplanar = exp < 3 && oldDz == 0x8000;
            if (exp < 3) oldDz = coplanar ? 0xFFFF : std::max(oldDz * 2, 16u >> exp);
            const s32 tolerance = std::bit_floor(dz | oldDz) * 8;
            const bool farther = coplanar || z + tolerance >= oldZ;
            const bool nearer = coplanar || z - tolerance <= oldZ;
            blend = (otherModes_ & 0x4000) || (aa && !overflow && farther);
            switch ((otherModes_ >> 10) & 3) {
                case 1:
                    if (front && farther && overflow) {
                        const u32 shift = std::bit_width(static_cast<u32>(tolerance >> 3)) - 1;
                        coverage = std::min(8u, (coverage * (((oldZ >> shift) - (z >> shift)) & 15)) >> 3);
                        if (!coverage) return false;
                        break;
                    }
                    [[fallthrough]];
                case 0: if (!farthest && !(overflow ? front : nearer)) return false; break;
                case 2: if (!farthest && !front) return false; break;
                case 3: if (!farther || !nearer || farthest) return false; break;
            }
        }
        if (otherModes_ & 0x20) {
            const s32 exp = std::min(7, 18 - static_cast<s32>(std::bit_width(std::max(1u, 0x3FFFFu - z))));
            const u32 mantissa = (z >> std::max(6 - exp, 0)) & 0x7FF;
            const u32 delta = std::bit_width(std::max(dz, 1u)) - 1;
            const u16 depthWord = static_cast<u16>((exp << 13) | (mantissa << 2) | (delta >> 2));
            if (activeDeferredWrites)
                bus_->rdram().storeRdramDeferred(zaddr, depthWord, *activeDeferredWrites);
            else
                bus_->rdram().storeRdram(zaddr, depthWord);
            hidden_[zaddr / 2] = delta & 3;
        }
    }
    const u32 alpha = channel(color, 3);
    const auto blender = [&](u32 pixel, u32 stage, bool mix, bool preserveMemory, bool intermediate = false) {
        const std::array<u32, 4> rgb{pixel, mem, fromRegister(blendColor_), fromRegister(fogColor_)};
        const std::array<u32, 4> a{alpha, fogColor_ & 255, channel(shade, 3), 0};
        const auto& mux = blendMux_[stage];
        const u32 p = rgb[mux.p];
        const u32 m = rgb[mux.m];
        if (preserveMemory) return m;
        if (!mix) return p;
        const u32 av = a[mux.a] >> 3;
        const std::array<u32, 4> b{31 - av, memCoverage << 2, 31, 0};
        const u32 bv = b[mux.b] + 1;
        const bool fixedShift = intermediate || (otherModes_ & 0x4000);
        const u32 divisor = fixedShift ? 32 : av + bv;
        u32 rgbOut = 0;
        for (u32 i = 0; i < 3; ++i) {
            const u32 value = (channel(p, i) * av + channel(m, i) * bv) / divisor;
            rgbOut |= (fixedShift ? value & 255 : std::min(255u, value)) << (8 * i);
        }
        return rgbOut;
    };
    const bool colorOnCvg = (otherModes_ & 0x80) && !overflow;
    if (cycle == 0) {
        // 1-cycle mode executes only stage 0. COLOR_ON_CVG substitutes that
        // stage's M input; the second MUX is ignored in this mode.
        color = blender(color, 0, blend, colorOnCvg);
    } else {
        // The first two-cycle equation (e.g. fog) runs regardless of the
        // final blend enable. The enable and prewrap only gate stage 1.
        color = blender(color, 0, true, false, true);
        color = blender(color, 1, blend, colorOnCvg);
    }
    color &= 0xFFFFFF;
    u32 outCoverage;
    switch ((otherModes_ >> 8) & 3) {
        case 0: outCoverage = blend ? std::min(coverage + memCoverage, 7u) : coverage - 1; break;
        case 1: outCoverage = (coverage + memCoverage) & 7; break;
        case 2: outCoverage = 7; break;
        default: outCoverage = memCoverage; break;
    }
    if (bus_ && colorImage_.fmt == 0 && colorImage_.size >= 2 && colorInRange) {
        if (bytes == 2) hidden_[addr / 2] = outCoverage & 3;
        // RGBA32 framebuffer alpha stores coverage, not the blender alpha.
        color = (color & 0x00FFFFFF) | (outCoverage << 29);
    }
    return writePixel(x, y, color, false, refine);
}

void Rdp::cmdFillTriangle(u64 word0, const u64* words, std::size_t wordCount) {
    statsTriangleCommands_.fetch_add(1, std::memory_order_relaxed);
    const bool shade = ((word0 >> 58) & 1) != 0;
    const bool textured = ((word0 >> 57) & 1) != 0;
    const bool zbuffer = ((word0 >> 56) & 1) != 0;
    if (textured) statsTexturedTriangles_.fetch_add(1, std::memory_order_relaxed);
    if (shade) statsShadedTriangles_.fetch_add(1, std::memory_order_relaxed);
    const bool leftMajor = word0 & (1ULL << 55);
    // The tile and level fields live in the upper 32-bit half of word 0:
    // upper-half bits 18:16 and 21:19 are 64-bit bits 50:48 and 53:51.
    const u32 tile = (word0 >> 48) & 7;
    triangleLevel_ = (word0 >> 51) & 7;
    const u32 cycle = (otherModes_ >> 52) & 3;
    if (wordCount < 3 + 8u * shade + 8u * textured + 2u * zbuffer || cycle == 2) return;
    const s32 yh = signExtend14(word0 & 0x3FFF);
    const s32 ym = signExtend14((word0 >> 16) & 0x3FFF);
    const s32 yl = signExtend14((word0 >> 32) & 0x3FFF);
    const s32 yBase = yh & ~3;
    struct Edge {
        s64 x, quarterStep;
        s64 at(s32 dy) const {
            // The edge accumulator wraps to signed 12.16.
            return static_cast<s32>(static_cast<u32>(x + quarterStep * dy) << 4) >> 4;
        }
    };
    const auto decodeEdge = [&](u32 i) -> Edge {
        return {static_cast<s32>(static_cast<u32>(words[i] >> 32) << 4) >> 4 & ~1,
                (static_cast<s32>(static_cast<u32>(words[i]) << 2) >> 4) & ~1};
    };
    const Edge xl = decodeEdge(0), xh = decodeEdge(1), xm = decodeEdge(2);
    const auto snap = [](s64 x) -> s64 {
        // Keep three fractional bits plus a sticky low bit. The same
        // quantization is applied to both sides before the half-open test.
        return ((x >> 13) | ((x & 0x1FFF) != 0)) * 8192;
    };
    struct Coeff {
        s32 base = 0, dx = 0, de = 0, dy = 0;
    };
    const auto decode = [](const u64* w, u32 i) -> Coeff {
        const u32 shift = 48 - 16 * i;
        const auto get = [&](u32 n) {
            return static_cast<s32>((((w[n] >> shift) & 65535) << 16) | ((w[n + 2] >> shift) & 65535));
        };
        return {get(0), get(1), get(4), get(5)};
    };
    std::array<Coeff, 4> rgba{};
    std::array<Coeff, 3> stw{};
    // word0 and three edge words precede the optional SHADE, STW and Z blocks.
    const u64* suffix = words + 3;
    if (shade) {
        for (u32 i = 0; i < 4; ++i) rgba[i] = decode(suffix, i);
        suffix += 8;
    }
    if (textured) {
        for (u32 i = 0; i < 3; ++i) stw[i] = decode(suffix, i);
        suffix += 8;
    }
    Coeff depth{};
    u32 dz = 0;
    if (zbuffer) {
        depth = {s32(suffix[0] >> 32), s32(suffix[0]), s32(suffix[1] >> 32), s32(suffix[1])};
        const auto magnitude = [](s32 n) -> u32 { return n < 0 ? ~n : n; };
        const u32 sum = magnitude(depth.dx >> 16) + magnitude(depth.dy >> 16);
        dz = sum >= 0x4000 ? 0x8000 : std::max(1u, std::bit_floor(sum) * 2);
    }
    const bool quad = otherModes_ & (1ULL << 45);
    const bool noise = fragmentUsesNoise(cycle);
    // With max level zero and neither detail nor sharpen, both magnification
    // and minification select the distant level, giving LOD_FRAC=0xff. The
    // combiner can request this fraction even when texture LOD is disabled.
    // Other max levels still need derivative-based LOD and tile selection.
    const u32 lodFraction = textured && triangleLevel_ == 0 && !(otherModes_ & (3ULL << 49)) ? 255 : 0;
    // This presentation pass refines opaque fragment interiors. Native
    // coverage/depth, image reads and dependent passes are still resolved once
    // at 1x. Do not invent high-resolution results for mixed/alpha-tested
    // pixels or stochastic combiners.
    const auto& firstBlend = blendMux_[0];
    const auto& finalBlend = blendMux_[cycle == 1];
    const bool refine = resolutionScale_ > 1 && cycle < 2 && (shade || textured) &&
        !noise && !(otherModes_ & 0x5001) && finalBlend.p != 1 &&
        (cycle == 0 || (firstBlend.p != 1 && firstBlend.m != 1 && firstBlend.b != 1));
    const u32 scale = resolutionScale_;
    std::array<std::array<s64, 4>, 16> shadeOffsets;
    std::array<std::array<s64, 3>, 16> textureOffsets;
    const auto offsets = [scale](auto& out, const auto& coefficients) {
        for (u32 y = 0; y < scale; ++y) for (u32 x = 0; x < scale; ++x)
            for (u32 i = 0; i < coefficients.size(); ++i) {
                // Calculate these once, not per pixel. At 3x repeated
                // division/truncation would drift at the texel boundaries.
                const s64 numerator = s64{coefficients[i].dx & ~31} * (2 * x + 1) +
                                      s64{coefficients[i].dy} * (2 * y + 1);
                out[y * scale + x][i] = numerator / (2 * scale);
            }
    };
    if (refine && shade) offsets(shadeOffsets, rgba);
    if (refine && textured) offsets(textureOffsets, stw);
    // Combiner source slots 2 and 3 are constant for the whole triangle, and
    // the coverage-select/alpha slots are part of immutable triangle state.
    // Hoist them out of the per-sample refine loop.
    const std::array<u32, 4> blendInputs{0, 0, fromRegister(blendColor_),
                                        fromRegister(fogColor_)};
    const bool coverageAlpha = (otherModes_ & 0x2000) != 0;
    std::atomic<bool> refined{false};
    const u64 before = statsPixelsDrawn_.load(std::memory_order_relaxed);
    const s32 yStart = std::max({yh >> 2, scissor_.minY, 0});
    const s32 yEnd = std::min({(yl + 3) >> 2, scissor_.maxY, s32(nativeFbHeight_)});
    if (yStart < yEnd) {
        const auto xmin = std::min({xh.at(yh - yBase), xh.at(yl - yBase),
                                    xm.at(yh - yBase), xm.at(ym - yBase), xl.x, xl.at(yl - ym)});
        const auto xmax = std::max({xh.at(yh - yBase), xh.at(yl - yBase),
                                    xm.at(yh - yBase), xm.at(ym - yBase), xl.x, xl.at(yl - ym)});
        const s32 width = std::min<s64>((xmax - xmin + 65535) >> 16, nativeFbWidth_);
        executeSpan(yStart, yEnd, refine ? width * resolutionScale_ : width,
                    cycle != 3 && (zbuffer || (otherModes_ & 4)) && (otherModes_ & 0x30),
                    [&, this](u32 yIn, unsigned) {
            const s32 y = static_cast<s32>(yIn);
            std::array<s64, 4> left{}, right{};
            s64 lo = nativeFbWidth_, hi = 0;
            s64 fullLo = 0, fullHi = nativeFbWidth_;
            for (s32 row = 0; row < 4; ++row) {
                const s32 subY = y * 4 + row;
                if (subY < yh || subY >= yl) {
                    fullHi = 0;
                    continue;
                }
                // XH/XM are latched at floor(YH); XL is loaded on the exact
                // quarter-row YM. Coverage itself still starts at YH.
                const s64 major = snap(xh.at(subY - yBase));
                const s64 minor = snap(subY < ym ? xm.at(subY - yBase) : xl.at(subY - ym));
                left[row] = (leftMajor ? major : minor);
                right[row] = (leftMajor ? minor : major);
                lo = std::min(lo, left[row] >> 16);
                hi = std::max(hi, (right[row] + 65535) >> 16);
                // All eight half-open tests are known true across the interior:
                // the first sample must clear the left edge and the last
                // sample must be strictly before the right edge on every row.
                const s64 offset = (row & 1) * 16384;
                fullLo = std::max(fullLo, (left[row] - offset + 65535) >> 16);
                fullHi = std::min(fullHi, (right[row] - offset - 32768 + 65535) >> 16);
            }
            const s32 xStart = static_cast<s32>(std::max({lo, s64{scissor_.minX}, s64{0}}));
            const s32 xEnd = static_cast<s32>(std::min({hi, s64{scissor_.maxX}, s64{nativeFbWidth_}}));
            const s32 dy = y - (yh >> 2);
            // The latch uses subrow 0 or 3, depending on slope and direction.
            // DE advances along XH; DY is the independent screen-Y gradient.
            // Their difference corrects the last-subrow latch, with hardware
            // truncation before adjusting for the fractional XH position.
            const bool doOffset = leftMajor == (xh.quarterStep < 0);
            const s64 majorX = xh.at(y * 4 - yBase + (doOffset ? 3 : 0));
            const auto coefficientAtStart = [&](const Coeff& coeff, s32 step) {
                const s64 de = coeff.de & ~511, screenY = coeff.dy & ~511;
                const s64 offset = doOffset ? de - (de >> 2) - screenY + (screenY >> 2) : 0;
                const s64 base = (coeff.base + s64{coeff.de} * dy) & ~s64{511};
                const s64 fraction = ((majorX >> 8) & 255) * ((coeff.dx >> 8) & ~1);
                return ((base + offset - fraction) & ~s64{1023}) +
                       s64{step} * (xStart - (majorX >> 16));
            };
            std::array<s64, 4> rgbaLine{};
            std::array<s64, 3> stwLine{};
            if (shade) for (u32 i = 0; i < 4; ++i) rgbaLine[i] = coefficientAtStart(rgba[i], rgba[i].dx & ~31);
            if (textured) for (u32 i = 0; i < 3; ++i) stwLine[i] = coefficientAtStart(stw[i], stw[i].dx & ~31);
            const auto zLine = zbuffer ? coefficientAtStart(depth, depth.dx) : 0;
            const bool persp = otherModes_ & (1ULL << 51);
            const bool filter0 = quad && (otherModes_ & (1ULL << 43));
            const bool filter1 = quad && (otherModes_ & (1ULL << 42));
            const auto sampleTexture = [&](s64 s, s64 t, s64 wFixed, u32& tex0, u32& tex1) {
                const s32 w = static_cast<s16>(wFixed >> 16);
                const auto divide = [&](s64 n) {
                    const s32 coord = static_cast<s32>(std::clamp<s64>(n >> 16, -32768, 32767));
                    return !persp ? coord : w <= 0 ? 32767 : std::clamp(coord * 32768 / w, -32768, 32767);
                };
                const auto sTex = divide(s), tTex = divide(t);
                tex0 = sampleTile(tiles_[tile], sTex, tTex, filter0);
                if (cycle == 1) tex1 = sampleTile(tiles_[(tile + 1) & 7], sTex, tTex, filter1);
            };
            const auto refineColor = [&](s32 x, s32 col) {
                // The center of each host sample lies in a regular SxS grid.
                // Evaluate the 16.16 screen-space planes in that grid without
                // re-running coverage, Z compares or any guest memory writes.
                std::array<s64, 4> shades{};
                std::array<s64, 3> texels{};
                if (shade) for (u32 i = 0; i < shades.size(); ++i)
                    shades[i] = rgbaLine[i] + s64{rgba[i].dx & ~31} * col;
                if (textured) for (u32 i = 0; i < texels.size(); ++i)
                    texels[i] = stwLine[i] + s64{stw[i].dx & ~31} * col;
                std::array<u32, 4> inputs = blendInputs;
                for (u32 sy = 0; sy < scale; ++sy) {
                    auto* dst = fb_.data() + (std::size_t{yIn} * scale + sy) * fbWidth_ + static_cast<u32>(x) * scale;
                    for (u32 sx = 0; sx < scale; ++sx) {
                        u32 v = 0, t0 = 0, t1 = 0;
                        if (shade) {
                            const auto& offset = shadeOffsets[sy * scale + sx];
                            for (u32 i = 0; i < 4; ++i) {
                                v |= static_cast<u32>(std::clamp<s64>((shades[i] + offset[i]) >> 16, 0, 255)) << (8 * i);
                            }
                        }
                        if (textured) {
                            const auto& offset = textureOffsets[sy * scale + sx];
                            sampleTexture(texels[0] + offset[0], texels[1] + offset[1], texels[2] + offset[2], t0, t1);
                        }
                        inputs[0] = combine(v, t0, t1, cycle == 1, 0, lodFraction).color;
                        if (cycle == 1) {
                            const std::array<u32, 4> a{coverageAlpha ? 255u : channel(inputs[0], 3),
                                                       fogColor_ & 255, channel(v, 3), 0};
                            const u32 av = a[firstBlend.a] >> 3;
                            const u32 bv = firstBlend.b == 0 ? 32 - av : firstBlend.b == 2 ? 32 : 1;
                            const u32 p = inputs[firstBlend.p], m = inputs[firstBlend.m];
                            inputs[0] = 0;
                            for (u32 i = 0; i < 3; ++i)
                                inputs[0] |= ((channel(p, i) * av + channel(m, i) * bv) >> 5 & 255) << (i * 8);
                        }
                        dst[sx] = (inputs[finalBlend.p] & 0x00FFFFFF) | 0xFF000000;
                    }
                }
            };
#if defined(__ARM_NEON) && defined(__aarch64__)
            const s32 rgbaStep[] = {rgba[0].dx & ~31, rgba[1].dx & ~31, rgba[2].dx & ~31, rgba[3].dx & ~31};
            const auto rg = vld1q_s64(rgbaLine.data()), ba = vld1q_s64(rgbaLine.data() + 2);
            const auto steps = vld1q_s32(rgbaStep);
#endif
            u32 pixels = 0;
            bool rowRefined = false;
            for (s32 x = xStart; x < xEnd; ++x) {
                const s64 px = s64{x} * 65536;
                // Eight subpixel samples: rows 0..3 at x+0 and x+1/2, with the
                // odd rows offset by 1/4 pixel. Bit 0 is the non-AA fill bit.
                u8 mask = 0xFF;
                if (x < fullLo || x >= fullHi) {
                    mask = 0;
                    for (u32 row = 0; row < 4; ++row) {
                        const s64 sample = px + ((row & 1) * 16384);
                        if (sample >= left[row] && sample < right[row]) mask |= 1u << (row * 2);
                        if (sample + 32768 >= left[row] && sample + 32768 < right[row])
                            mask |= 1u << (row * 2 + 1);
                    }
                }
                if (!mask || (cycle < 2 && !(otherModes_ & 8) && !(mask & 1))) continue;
                const u32 coverage = static_cast<u32>(std::popcount(mask));
                const bool firstSample = (mask & 1) != 0;
                const s32 col = x - xStart;
                u32 color = 0, tex0 = 0, tex1 = 0;
                if (shade) {
                    // Partial pixels use the leftmost sample in the first
                    // covered row for shade correction. TEX coordinates do
                    // not use this coverage offset.
                    if (mask != 0xFF) {
                        const u32 sample = std::countr_zero(static_cast<u32>(mask));
                        const s32 offY = static_cast<s32>(sample >> 1);
                        const s32 offX = (offY & 1) + 2 * static_cast<s32>(sample & 1);
                        const auto derivative = [](s32 d) { return static_cast<s32>(static_cast<u32>(d) << 5) >> 19; };
                        for (u32 i = 0; i < 4; ++i) {
                            const s64 value = rgbaLine[i] + s64{rgba[i].dx & ~31} * col;
                            const s64 component = ((value >> 14) * 4 + offX * derivative(rgba[i].dx) +
                                                   offY * derivative(rgba[i].dy)) >> 4;
                            color |= static_cast<u32>(std::clamp<s64>(component, 0, 255)) << (8 * i);
                        }
                    } else {
#if defined(__ARM_NEON) && defined(__aarch64__)
                        const auto upper = vshrq_n_s64(vmlal_n_s32(rg, vget_low_s32(steps), col), 16);
                        const auto lower = vshrq_n_s64(vmlal_high_n_s32(ba, steps, col), 16);
                        const auto channels = vcombine_s32(vqmovn_s64(upper), vqmovn_s64(lower));
                        const auto clamped = vqmovn_u16(vcombine_u16(vqmovun_s32(channels), vdup_n_u16(0)));
                        color = vget_lane_u32(vreinterpret_u32_u8(clamped), 0);
#else
                        for (u32 i = 0; i < 4; ++i) {
                            const s64 value = rgbaLine[i] + s64{rgba[i].dx & ~31} * col;
                            color |= static_cast<u32>(std::clamp<s64>(value >> 16, 0, 255)) << (8 * i);
                        }
#endif
                    }
                }
                if (textured && cycle < 2) {
                    const auto coord = [&](u32 i) { return stwLine[i] + s64{stw[i].dx & ~31} * col; };
                    sampleTexture(coord(0), coord(1), coord(2), tex0, tex1);
                }
                const s32 z = static_cast<s32>(std::clamp<s64>((zLine + s64{depth.dx} * col) >> 13, 0, 0x3FFFF));
                const bool fine = refine && mask == 0xFF;
                const bool written = drawFragment(x, y, color, tex0, tex1, coverage, firstSample, z, dz, zbuffer,
                                        noise ? fragmentNoise(x, y) : 0, nullptr, lodFraction, fine);
                pixels += written;
                if (written && fine) {
                    refineColor(x, col);
                    rowRefined = true;
                }
            }
            if (rowRefined) refined.store(true, std::memory_order_relaxed);
            statsPixelsDrawn_.fetch_add(pixels, std::memory_order_relaxed);
        });
    }
    if (refine && refined.load(std::memory_order_relaxed)) hasFineSamples_ = true;
    if (statsPixelsDrawn_.load(std::memory_order_relaxed) != before)
        statsTriangles_.fetch_add(1, std::memory_order_relaxed);
}

s32 Rdp::signExtend14(u32 v) noexcept { return static_cast<s32>((v & 0x2000) ? (v | 0xFFFFC000) : v); }

} // namespace n64forge
