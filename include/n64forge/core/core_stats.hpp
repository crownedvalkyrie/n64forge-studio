#pragma once

#include "n64forge/core/types.hpp"
#include <algorithm>
#include <cmath>

namespace n64forge {

inline constexpr u32 NominalCpuHz = 93'750'000;

struct CoreCounters {
    u64 cycles = 0, instructions = 0, viFields = 0, paints = 0, presentationTicks = 0;
    u64 cacheLookups = 0, cacheHits = 0;
    u64 jitSteps = 0, jitInvocations = 0;
    u64 gfx = 0, audio = 0, other = 0, triangles = 0;
    u64 sampledSteps = 0, cpuNs = 0, rspNs = 0, rdpNs = 0, sleepNs = 0;
    u64 deviceTicks = 0, batchedCycles = 0;
    u64 sampledBatchCycles = 0, batchCpuNs = 0;
    double dacHz = 0;
    u32 rdpScale = 1, rdpWidth = 0, rdpHeight = 0;
    u32 viWidth = 0, viHeight = 0;
};

struct CoreStats {
    bool ready = false, hasCacheRate = false;
    double speed = 0, mips = 0, mhz = 0, viFps = 0, displayFps = 0, cacheHit = 0;
    double presentationHz = 0;
    double frameMs = 0, cpuMs = 0, rspMs = 0, rdpMs = 0, sleepMs = 0, otherMs = 0;
    double gfxPerSec = 0, audioPerSec = 0, otherPerSec = 0, trianglesPerSec = 0;
    CoreCounters counters;
};

// Clock and host paint counts are injected by the frontend; never used to
// advance the guest. Call after a batch, not from the instruction loop.
class CoreStatsSampler {
public:
    void reset(u64 nowNs, const CoreCounters& counters) noexcept {
        startNs_ = nowNs;
        start_ = counters;
        stats_ = {};
    }

    bool update(u64 nowNs, const CoreCounters& c) noexcept {
        if (nowNs <= startNs_ || nowNs - startNs_ < 1'000'000'000) return false;
        const double seconds = (nowNs - startNs_) * 1e-9;
        const double alpha = stats_.ready ? -std::expm1(-seconds) : 1.0;
        const auto set = [alpha](double& dest, double value) { dest += alpha * (value - dest); };
        const u64 cycles = c.cycles - start_.cycles;
        const u64 fields = c.viFields - start_.viFields;
        const u64 lookups = c.cacheLookups - start_.cacheLookups;
        set(stats_.mhz, cycles / seconds / 1e6);
        set(stats_.speed, cycles / seconds / NominalCpuHz * 100);
        set(stats_.mips, (c.instructions - start_.instructions) / seconds / 1e6);
        set(stats_.viFps, fields / seconds);
        set(stats_.displayFps, (c.paints - start_.paints) / seconds);
        set(stats_.presentationHz, (c.presentationTicks - start_.presentationTicks) / seconds);
        if (lookups) {
            const double rate = 100.0 * (c.cacheHits - start_.cacheHits) / lookups;
            if (stats_.hasCacheRate) set(stats_.cacheHit, rate);
            else stats_.cacheHit = rate;
        }
        stats_.hasCacheRate = lookups != 0;
        const u64 batched = c.batchedCycles - start_.batchedCycles;
        const u64 samples = c.sampledSteps - start_.sampledSteps;
        const u64 batchSamples = c.sampledBatchCycles - start_.sampledBatchCycles;
        const u64 scalarSamples = samples - batchSamples;
        const double scalarWeight = scalarSamples ? double(cycles - batched) / scalarSamples : 0;
        const double batchWeight = batchSamples ? double(batched) / batchSamples : 0;
        const double sleep = std::min(seconds, (c.sleepNs - start_.sleepNs) * 1e-9);
        const double rdp = std::min(seconds - sleep, (c.rdpNs - start_.rdpNs) * 1e-9);
        const double cpu = ((c.cpuNs - start_.cpuNs) * scalarWeight +
                            (c.batchCpuNs - start_.batchCpuNs) * batchWeight) * 1e-9;
        const double rsp = (c.rspNs - start_.rspNs) * 1e-9 * scalarWeight;
        const double remaining = seconds - sleep - rdp;
        const double weight = cpu + rsp > remaining ? remaining / (cpu + rsp) : 1;
        const double msPerField = fields ? 1000.0 / fields : 0;
        set(stats_.frameMs, seconds * msPerField);
        set(stats_.cpuMs, cpu * weight * msPerField);
        set(stats_.rspMs, rsp * weight * msPerField);
        set(stats_.rdpMs, rdp * msPerField);
        set(stats_.sleepMs, sleep * msPerField);
        set(stats_.otherMs, std::max(0.0, remaining - (cpu + rsp) * weight) * msPerField);
        set(stats_.gfxPerSec, (c.gfx - start_.gfx) / seconds);
        set(stats_.audioPerSec, (c.audio - start_.audio) / seconds);
        set(stats_.otherPerSec, (c.other - start_.other) / seconds);
        set(stats_.trianglesPerSec, (c.triangles - start_.triangles) / seconds);
        stats_.counters = c;
        stats_.ready = true;
        start_ = c;
        startNs_ = nowNs;
        return true;
    }

    [[nodiscard]] const CoreStats& snapshot() const noexcept { return stats_; }

private:
    u64 startNs_ = 0;
    CoreCounters start_;
    CoreStats stats_;
};
}
