#include "n64forge/core/bus.hpp"
#include "n64forge/core/hw_registers.hpp"
#include "n64forge/core/rdp.hpp"

#include <atomic>
#include <cassert>
#include <cstdint>
#include <limits>
#include <thread>
#include <vector>

using namespace n64forge;

namespace {

[[nodiscard]] u64 safetySyncFull() { return 0x29ULL << 56; }

void prepareCycle3Fill(Rdp& rdp) {
    rdp.setOtherModes(3ULL << 52); // Copy/fill cycle: drawFragment writes fillColor directly.
}

// Huge native dimensions must be rejected without changing the live surface.
void test_rdp_safety_framebuffer_rejects_overflow() {
    Rdp rdp;
    rdp.setFramebufferSize(8, 8);
    rdp.setFramebufferSize(std::numeric_limits<u32>::max(), std::numeric_limits<u32>::max());
    assert(rdp.width() == 8 && rdp.height() == 8);
    assert(rdp.framebuffer().size() == 64);

    // 0x40000000 native pixels becomes 2^32 internal pixels at 4x, which
    // cannot be represented in fbWidth_ even before the pixel-count cap.
    rdp.setResolutionScale(4);
    assert(rdp.width() == 32 && rdp.height() == 32);
    rdp.setFramebufferSize(0x40000000u, 2);
    assert(rdp.width() == 32 && rdp.height() == 32);
    assert(rdp.framebuffer().size() == 1024);

    // A rejected allocation must not leave the rasterizer pointing past fb_.
    rdp.setResolutionScale(1);
    prepareCycle3Fill(rdp);
    rdp.setScissor(0, 0, 8, 8);
    rdp.setFillColor(0xAABBCCDDu);
    rdp.fillRectangle(0, 0, 8, 8);
    assert(rdp.pixel(7, 7) == 0xAABBCCDDu);
}

// DP_CURRENT + 8 must be compared in u64; a u32 wrap used to read address 0.
void test_rdp_safety_ring_does_not_wrap_at_u32_max() {
    Bus bus;
    HwRegisters hw;
    Rdp rdp;
    rdp.setBus(&bus);
    rdp.setHw(&hw);
    rdp.setFramebufferSize(16, 8);

    hw.dp().status = 0;
    hw.dp().current = 0xFFFFFFF8u;
    hw.dp().end = 0xFFFFFFFFu;
    bus.write64(0, safetySyncFull());

    rdp.process();

    assert(hw.dp().current == 0xFFFFFFF8u);
    assert(rdp.stats().commandsProcessed.load() == 0);
    assert(rdp.stats().fullSyncs.load() == 0);
}

// Direct command submission is usable before an HwRegisters instance is attached.
void test_rdp_safety_sync_full_without_hw() {
    Rdp rdp;
    const u64 command = safetySyncFull();
    rdp.submitCommands(&command, 1);
    rdp.process();
    assert(rdp.stats().fullSyncs.load() == 1);
}

struct RenderResult {
    std::vector<u32> framebuffer;
    u64 pixels = 0;
};

[[nodiscard]] RenderResult renderWithScissorMinusOne(unsigned threads) {
    Rdp rdp;
    rdp.setFramebufferSize(16, 8);
    rdp.setThreadCount(threads);
    rdp.setScissor(-1, -1, 16, 8);
    prepareCycle3Fill(rdp);
    rdp.setFillColor(0x11223344u);
    rdp.fillRectangle(-2, -1, 15, 4);
    const auto stats = rdp.stats();
    return {rdp.framebuffer(), stats.pixelsDrawn.load()};
}

// Negative scissor bounds are clamped in s64 before thread partitioning.
void test_rdp_safety_threaded_negative_scissor_parity() {
    const RenderResult serial = renderWithScissorMinusOne(1);
    const RenderResult threaded = renderWithScissorMinusOne(4);
    assert(serial.pixels == 15u * 4u);
    assert(threaded.pixels == serial.pixels);
    assert(threaded.framebuffer == serial.framebuffer);
}

void paintRepeatedly(Rdp& rdp) {
    prepareCycle3Fill(rdp);
    rdp.setScissor(0, 0, 16, 8);
    for (u32 i = 0; i < 512; ++i) {
        rdp.setFillColor(0xFF000000u | (i << 8));
        rdp.fillRectangle(0, 0, 16, 8);
    }
}

// Reconfiguring the pool while spans are in flight must keep the shared_ptr
// snapshot alive and still produce serial output.
void test_rdp_safety_thread_count_reconfiguration_parity() {
    Rdp baseline;
    baseline.setFramebufferSize(16, 8);
    baseline.setThreadCount(1);
    paintRepeatedly(baseline);

    Rdp dynamic;
    dynamic.setFramebufferSize(16, 8);
    dynamic.setThreadCount(1);

    std::atomic<bool> done{false};
    std::thread painter([&] {
        paintRepeatedly(dynamic);
        done.store(true, std::memory_order_release);
    });

    unsigned toggle = 0;
    while (!done.load(std::memory_order_acquire)) {
        dynamic.setThreadCount((toggle++ & 1u) ? 4u : 1u);
        std::this_thread::yield();
    }
    painter.join();

    assert(dynamic.framebuffer() == baseline.framebuffer());
    assert(dynamic.stats().pixelsDrawn.load() == baseline.stats().pixelsDrawn.load());
    assert(dynamic.threadCount() <= 8u);
}

// INT_MIN..INT_MAX triangle deltas must not overflow s32 edge arithmetic or
// iterate an unclipped bounding box.
void test_rdp_safety_extreme_triangle_bounds() {
    Rdp rdp;
    rdp.setFramebufferSize(16, 8);
    prepareCycle3Fill(rdp);
    rdp.setScissor(0, 0, 16, 8);
    rdp.setFillColor(0x55667788u);

    constexpr s32 lo = std::numeric_limits<s32>::min();
    constexpr s32 hi = std::numeric_limits<s32>::max();
    rdp.drawFlatTriangle(lo, hi, hi, hi, hi, lo, 0x55667788u);

    assert(rdp.stats().triangles.load() == 1);
    assert(rdp.pixel(0, 0) == 0x55667788u);
    assert(rdp.pixel(15, 7) == 0x55667788u);
}

} // namespace

int run_rdp_safety_tests() {
    test_rdp_safety_framebuffer_rejects_overflow();
    test_rdp_safety_ring_does_not_wrap_at_u32_max();
    test_rdp_safety_sync_full_without_hw();
    test_rdp_safety_threaded_negative_scissor_parity();
    test_rdp_safety_thread_count_reconfiguration_parity();
    test_rdp_safety_extreme_triangle_bounds();
    return 0;
}
