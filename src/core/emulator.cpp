#include "n64forge/core/emulator.hpp"
#include "n64forge/core/hw_registers.hpp"
#include "n64forge/core/ipl.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <utility>

namespace n64forge {
namespace {

using Clock = std::chrono::steady_clock;
u64 elapsedNs(Clock::time_point since) noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - since).count();
}

constexpr u32 PayloadOffset = 0x1000;
constexpr u32 InitialLoadSize = 0x100000;

u32 tvType(u8 country) {
    switch (country) {
        case 'B': return 2; // MPAL
        case 'D': case 'F': case 'H': case 'I': case 'L': case 'P':
        case 'S': case 'U': case 'W': case 'X': case 'Y': return 0; // PAL
        default: return 1; // NTSC
    }
}

} // namespace

Emulator::Emulator() {
    audio_.reserve(96000);
    bus_.attachRsp(&rsp_);
    bus_.attachCpu(&cpu_);
    rdp_.setBus(&bus_);
    rdp_.setHw(&bus_.hw());
    reset();
}

void Emulator::reset() {
    counters_ = {};
    timingCountdown_ = 0;
    cpu_.reset();
    bus_.reset();
    cpu_.attachToBus(bus_);
    rsp_.reset();
    rsp_.setBus(&bus_);
    rdp_.reset();
    rdp_.setBus(&bus_);
    rdp_.setHw(&bus_.hw());
    rdp_.setFramebufferSize(320, 240);
    audio_.clear();
    audioResamplePhase_ = 0;
    scanout_.clear();
    scanoutWidth_ = scanoutHeight_ = 0;
    if (!cartridge_.loaded()) return;

    // The CIC profile comes only from the IPL2 checksum authenticated by the
    // PIF. Full IPL execution is still outside the reference model.
    applyBootProfile(detectCic(cartridge_.rom()));
}

void Emulator::applyBootProfile(const CicProfile& profile) {
    cicProfile_ = profile;
    const auto& rom = cartridge_.rom();
    const u32 tv = tvType(cartridge_.header().countryCode);
    const u32 version = tv == 0 ? 7 : tv == 2 ? 5 : 1;
    const u32 entry = profile.fixedEntry ? profile.fixedEntryAddress
                                         : cartridge_.header().bootPc - profile.entryBias;
    const u32 target = entry & 0x1FFFFFFF;
    const auto size = std::min<std::size_t>(InitialLoadSize, rom.size() - PayloadOffset);
    for (u32 i = 0; i < size; ++i) bus_.write8(target + i, rom[PayloadOffset + i]);
    bus_.write32(0x300, tv);
    bus_.write32(0x304, 0); // cartridge
    bus_.write32(0x308, 0xB0000000);
    bus_.write32(0x30C, 0); // cold reset
    bus_.write32(0x310, profile.id(tv == 0));
    bus_.write32(0x314, version);
    bus_.write32(0x318, Memory::RdramSize);
    // IPL3 leaves the RI configured for the attached RDRAM. No RAM-size
    // hint is derived from cartridge identity.
    bus_.write32(0x0470000C, 0x14);
    if (profile.challenge) bus_.write32(0x3F0, Memory::RdramSize);
    bus_.hw().si().cic6105 = profile.challenge; // PIF RAM is cleared before IPL3
    bus_.hw().setVideoClock(tv == 0 ? HwRegisters::PalVideoClockHz :
                            tv == 2 ? HwRegisters::MpalVideoClockHz : HwRegisters::NtscVideoClockHz);
    // X105 repurposes s6 while calculating the cartridge checksum.
    cpu_.setIplHandoffRegisters(tv, version,
                                profile.challenge ? 0xFFFFFFFFA0000200ULL : profile.seed,
                                tv == 1 ? 0xFFFFFFFFA4001550ULL : 0xFFFFFFFFA4001554ULL);
    cpu_.setPc(static_cast<s64>(static_cast<s32>(entry)));
}

bool Emulator::loadCartridge(const std::filesystem::path& path) {
    Cartridge cart;
    if (!cart.load(path)) return false;
    return loadCartridge(std::move(cart));
}

bool Emulator::loadCartridge(Cartridge cart) {
    if (cart.size() <= PayloadOffset) return false;
    const auto profile = detectCic(cart.rom());
    const u32 entry = profile.fixedEntry ? profile.fixedEntryAddress
                                         : cart.header().bootPc - profile.entryBias;
    const u32 target = entry & 0x1FFFFFFF;
    if (entry < 0x80000000 || entry >= 0xC0000000 ||
        (entry & 3) != 0 || target >= Memory::RdramSize ||
        std::min<std::size_t>(InitialLoadSize, cart.size() - PayloadOffset) > Memory::RdramSize - target) {
        return false;
    }
    cartridge_ = std::move(cart);
    bus_.mountCartridge(&cartridge_);
    reset();
    return true;
}

void Emulator::tickHardware(bool sample, u32 cycles) {
    ++counters_.deviceTicks;
    bus_.flash().advance(cycles);
    bus_.eeprom().advance(cycles);
    if (cycles == 1) bus_.hw().tickDma(bus_);
    bus_.hw().tickVideo(cycles); // one nominal cycle per interpreter step
    if (const auto sample = bus_.hw().tickAudio(bus_, cycles)) {
        // Convert the emulated DAC stream to the host sink's 48 kHz using
        // sample-and-hold. This does not affect hardware progress.
        audioResamplePhase_ += u64{48000} * (bus_.hw().ai().dacrate + 1);
        const auto clock = bus_.hw().videoClockHz();
        while (audioResamplePhase_ >= clock) {
            audioResamplePhase_ -= clock;
            if (audio_.size() < 96000) audio_.insert(audio_.end(), sample->begin(), sample->end());
        }
    }
    if (cycles != 1) return; // The batched interval was idle for the other devices.
    if (!(bus_.hw().sp().status & 1)) {
        if (sample) {
            const auto start = Clock::now();
            rsp_.step();
            counters_.rspNs += elapsedNs(start);
        } else {
            rsp_.step();
        }
    }
    const auto& dp = bus_.hw().dp();
    if (dp.current < dp.end || (dp.status & 4)) {
        if (timingEnabled_) {
            const auto start = Clock::now();
            rdp_.process();
            counters_.rdpNs += elapsedNs(start);
        } else {
            rdp_.process();
        }
    }
}

void Emulator::runOneStep() {
    ++counters_.cycles;
    const bool sample = timingEnabled_ && timingCountdown_-- == 0;
    if (sample) {
        // Vary the interval to avoid repeatedly sampling the same guest loop slot.
        timingCountdown_ = 768 + ((++counters_.sampledSteps * 137) & 511);
        const auto start = Clock::now();
        if (blockCacheEnabled_) cpu_.stepCached(bus_);
        else cpu_.step(bus_);
        counters_.cpuNs += elapsedNs(start);
    } else {
        if (blockCacheEnabled_) cpu_.stepCached(bus_);
        else cpu_.step(bus_);
    }
    tickHardware(sample);
}

CoreCounters Emulator::counters() const noexcept {
    auto c = counters_;
    c.instructions = cpu_.instructionsDispatched();
    c.viFields = bus_.hw().videoFields();
    c.cacheLookups = cpu_.cacheStats().lookups;
    c.cacheHits = cpu_.cacheStats().hits;
    c.jitSteps = cpu_.jitSteps();
    c.jitInvocations = cpu_.jitInvocations();
    c.gfx = rsp_.taskTrace()[1].launches;
    c.audio = rsp_.taskTrace()[2].launches;
    c.other = rsp_.taskLaunches() - c.gfx - c.audio;
    c.triangles = rdp_.stats().triangles;
    if (bus_.hw().ai().control)
        c.dacHz = double(bus_.hw().videoClockHz()) / (bus_.hw().ai().dacrate + 1.0);
    c.rdpScale = rdp_.resolutionScale();
    c.rdpWidth = rdp_.width();
    c.rdpHeight = rdp_.height();
    c.viWidth = scanoutWidth_;
    c.viHeight = scanoutHeight_;
    return c;
}

size_t Emulator::audioFrameCount() const { return audio_.size() / 2; }

const int16_t* Emulator::audioSamples(size_t& frames) const {
    frames = audioFrameCount();
    if (frames == 0) return nullptr;
    return audio_.data();
}

void Emulator::runFrame() {
    constexpr int kInstructionsPerBatch = 100000;
    audio_.clear();
    runCycles(kInstructionsPerBatch, false);
    scanout();
}

void Emulator::setInternalResolution(u32 scale) {
    rdp_.setResolutionScale(scale);
}

CpuExecutionMode Emulator::setCpuExecutionMode(CpuExecutionMode requested) {
    blockCacheEnabled_ = eventBatchingEnabled_ = requested != CpuExecutionMode::Interpreter;
    if (!cpu_.setJitEnabled(requested == CpuExecutionMode::Jit)) return CpuExecutionMode::Cached;
    return requested;
}

u32 Emulator::runVideoFrame(bool capture) {
    audio_.clear();
    const auto field = bus_.hw().videoFields();
    constexpr u32 maxCycles = HwRegisters::CpuClockHz / 40;
    const u32 cycles = runCycles(maxCycles, true);
    if (capture && bus_.hw().videoFields() != field) scanout();
    return cycles;
}

u32 Emulator::runCycles(u32 limit, bool stopAtField) {
    const auto field = bus_.hw().videoFields();
    u32 consumed = 0;
    while (consumed < limit && (!stopAtField || bus_.hw().videoFields() == field)) {
        // An active RSP requires one device tick per instruction. Keep the
        // same CPU/RSP order without calling the idle-horizon code each time.
        if (eventBatchingEnabled_ && blockCacheEnabled_ && (bus_.hw().sp().status & 1)) {
            const auto horizon = bus_.hw().cyclesToDeviceEvent(limit - consumed);
            if (horizon > 1) {
                const bool sample = timingEnabled_ && timingCountdown_ == 0;
                const auto start = sample ? Clock::now() : Clock::time_point{};
                u32 elapsed = 0;
                while (elapsed < horizon) {
                    const auto n = cpu_.runQuietBlock(bus_, horizon - elapsed);
                    if (!n) break;
                    elapsed += n;
                }
                if (elapsed) {
                    if (sample) {
                        counters_.sampledSteps += elapsed;
                        counters_.sampledBatchCycles += elapsed;
                        counters_.batchCpuNs += elapsedNs(start);
                        timingCountdown_ = 768 + ((counters_.sampledSteps * 137) & 511);
                    } else if (timingEnabled_) {
                        timingCountdown_ -= std::min(timingCountdown_, elapsed);
                    }
                    counters_.cycles += elapsed;
                    counters_.batchedCycles += elapsed;
                    consumed += elapsed;
                    tickHardware(false, elapsed);
                    if (elapsed == horizon) continue;
                    // A CPU barrier stopped us early; flush virtual time first,
                    // then execute the barrier with the regular device order.
                }
            }
        }
        runOneStep();
        ++consumed;
    }
    return consumed;
}

Emulator::ScanoutLayout Emulator::scanoutLayout() const noexcept {
    const auto& vi = bus_.hw().vi();
    const u32 format = vi.status & 3;
    const u32 hBegin = (vi.hStart >> 16) & 1023, hEnd = vi.hStart & 1023;
    const u32 vBegin = (vi.vStart >> 16) & 1023, vEnd = vi.vStart & 1023;
    const u32 xs = vi.xScale & 4095, ys = vi.yScale & 4095;
    if (format < 2 || !vi.width || hEnd <= hBegin || vEnd <= vBegin || !xs || !ys) return {};
    // Unfiltered native-resolution view of the active window. This drops
    // analog blanking and field weaving, rather than simulating a CRT raster.
    const u32 width = std::min(vi.width, (hEnd - hBegin) * xs / 1024);
    const u32 height = std::min(1024u, (vEnd - vBegin) * ys / 2048);
    if (!width || !height) return {};
    const u32 xOffset = ((vi.xScale >> 16) & 4095) >> 10;
    // Y_OFFSET is 0.10, not 2.10 like X_OFFSET. Its upper two latch bits
    // are ignored by VI; they must not become a whole-row crop. This
    // unfiltered snapshot doesn't reproduce fractional phase or field weaving.
    const u32 bytes = format == 2 ? 2 : 4;
    return {width, height, vi.width, xOffset < vi.width ? std::min(width, vi.width - xOffset) : 0u,
            vi.origin + xOffset * bytes, bytes};
}

void Emulator::renderScanout(const ScanoutLayout& layout, std::span<u32> destination) const {
    for (u32 y = 0; y < layout.height; ++y) {
        const u32 address = layout.start + y * layout.stride * layout.bytes;
        const u32 valid = address < Memory::RdramSize
            ? std::min(layout.rowWidth, (Memory::RdramSize - address) / layout.bytes) : 0;
        auto* row = destination.data() + y * layout.width;
        if (layout.bytes == 2) {
            const auto expand = [](u32 n) { return (n << 3) | (n >> 2); };
            for (u32 x = 0; x < valid; ++x) {
                const u16 raw = bus_.rdram().loadRdram<u16>(address + x * 2);
                row[x] = 0xFF000000 | expand(raw >> 11) | (expand((raw >> 6) & 31) << 8) |
                         (expand((raw >> 1) & 31) << 16);
            }
        } else {
            for (u32 x = 0; x < valid; ++x) {
                const u32 raw = bus_.rdram().loadRdram<u32>(address + x * 4);
                row[x] = 0xFF000000 | (raw >> 24) | ((raw >> 8) & 0xFF00) | ((raw << 8) & 0xFF0000);
            }
        }
        std::fill(row + valid, row + layout.width, 0xFF000000);
    }
}

void Emulator::scanout() {
    const auto layout = scanoutLayout();
    scanoutWidth_ = layout.width;
    scanoutHeight_ = layout.height;
    scanout_.resize(layout.width * layout.height);
    renderScanout(layout, scanout_);
}

bool Emulator::scanoutForPresentation(std::span<u32> output, std::span<const u32> previous,
                                      ScanoutHistory& history, u32 packedRowAlignment) {
    const auto layout = scanoutLayout();
    if (layout.rowWidth == layout.width &&
        rdp_.canPresentScaled(layout.start, layout.stride, layout.width, layout.height, layout.bytes)) {
        const auto scale = rdp_.resolutionScale();
        const auto w = layout.width * scale, h = layout.height * scale;
        const u32 rowBytes = w * sizeof(u32);
        const u32 aligned = packedRowAlignment
            ? (rowBytes + packedRowAlignment - 1) / packedRowAlignment * packedRowAlignment : rowBytes;
        if (aligned % sizeof(u32) == 0 && u64{aligned} * h <= output.size_bytes()) {
            const auto words = std::size_t{aligned / sizeof(u32)} * h;
            rdp_.copyScaledColor(output.first(words), aligned / sizeof(u32), layout.start,
                                 layout.stride, layout.width, layout.height, layout.bytes);
            scanoutWidth_ = w;
            scanoutHeight_ = h;
            scanoutBytesPerPixel_ = sizeof(u32);
            scanoutPitch_ = aligned;
            const bool changed = !history.enhanced || !(history.layout == layout) ||
                history.pitch != aligned || previous.size() != words ||
                !std::equal(output.begin(), output.begin() + words, previous.begin());
            history.layout = layout;
            history.pitch = aligned;
            history.enhanced = true;
            return changed;
        }
    }
    const u32 pixels = layout.width * layout.height;
    const u32 pitch = packedRowAlignment
        ? (layout.width * layout.bytes + packedRowAlignment - 1) / packedRowAlignment * packedRowAlignment : 0;
    if (output.size() < pixels || output.size_bytes() < u64{pitch} * layout.height) return false;
    scanoutWidth_ = layout.width;
    scanoutHeight_ = layout.height;
    scanoutBytesPerPixel_ = packedRowAlignment ? layout.bytes : 4;
    scanoutPitch_ = pitch ? pitch : layout.width * 4;
    const u64 expectedBytes = packedRowAlignment ? u64{pitch} * layout.height : u64{pixels} * sizeof(u32);
    const bool layoutChanged = history.enhanced || !(history.layout == layout) || previous.size_bytes() != expectedBytes ||
                               history.pitch != pitch;
    bool dirty = layoutChanged || history.epoch != cpu_.rdramGenerationEpoch();
    history.layout = layout;
    history.epoch = cpu_.rdramGenerationEpoch();
    history.pitch = pitch;
    history.enhanced = false;
    if (!pixels) return !previous.empty();
    if (layout.rowWidth && layout.start < Memory::RdramSize) {
        const u32 end = std::min(Memory::RdramSize, layout.start + layout.bytes *
            ((layout.height - 1) * layout.stride + layout.rowWidth));
        // The line range may include padding between rows. A dirty padding
        // line can cause a recheck but cannot change the published pixels.
        for (u32 line = layout.start >> 6; line <= ((end - 1) >> 6); ++line) {
            const auto gen = cpu_.rdramLineGeneration(line);
            dirty |= history.lines[line] != gen;
            history.lines[line] = gen;
        }
    }
    if (!dirty) return false;
    if (!packedRowAlignment) {
        renderScanout(layout, output);
        return layoutChanged || !std::equal(output.begin(), output.begin() + pixels, previous.begin());
    }
    auto* dst = reinterpret_cast<u8*>(output.data());
    for (u32 y = 0; y < layout.height; ++y) {
        const u32 address = layout.start + y * layout.stride * layout.bytes;
        const u32 valid = address < Memory::RdramSize
            ? std::min(layout.rowWidth, (Memory::RdramSize - address) / layout.bytes) * layout.bytes : 0;
        if (valid) bus_.rdram().copyRdram(address, {dst + y * pitch, valid});
        std::fill_n(dst + y * pitch + valid, pitch - valid, u8{0});
    }
    return layoutChanged || std::memcmp(dst, previous.data(), pitch * layout.height) != 0;
}

} // namespace n64forge
