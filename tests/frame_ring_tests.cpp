// Standalone deterministic tests for the frame ring and the software
// interpolation gate. Not registered in CMake; call run_frame_ring_tests()
// from a test harness when desired.

#include "n64forge/emulation/frame_ring_buffer.hpp"
#include "n64forge/studio/emu_widget.hpp"

#include <cassert>
#include <chrono>
#include <future>
#include <iostream>
#include <optional>
#include <thread>

namespace {

using n64forge::emulation::DrawCallSnapshot;
using n64forge::emulation::FrameRingBuffer;
using n64forge::emulation::FrameSnapshot;

FrameSnapshot makeFrame(uint64_t ordinal, uint64_t key) {
    FrameSnapshot frame;
    frame.frameOrdinal = ordinal;
    DrawCallSnapshot draw;
    draw.matchKey = key;
    frame.draws.push_back(draw);
    return frame;
}

void testEmptyRingFallback() {
    FrameRingBuffer ring;
    const auto start = std::chrono::steady_clock::now();
    const auto frame = ring.waitFrame(1);
    const auto elapsed = std::chrono::steady_clock::now() - start;

    // No producer has ever published: the wall-clock interpolator must not
    // block waiting for its first ordinal.
    assert(frame && frame->frameOrdinal == 1 && frame->draws.empty());
    assert(elapsed < std::chrono::milliseconds(50));
}

void testPublishReclaimAndStaleDrop() {
    FrameRingBuffer ring;
    ring.publish(makeFrame(0, 10));
    ring.publish(makeFrame(1, 11));
    ring.publish(makeFrame(2, 12));
    assert(ring.head() == 3);

    // Ordinal 3 maps back to slot 0, which the consumer has not reclaimed.
    // The newest frame is dropped instead of spinning the guest thread.
    ring.publish(makeFrame(3, 13));
    assert(ring.head() == 3);

    const auto first = ring.waitFrame(0);
    assert(first && first->frameOrdinal == 0 && first->draws[0].matchKey == 10);

    ring.publish(makeFrame(3, 13));
    assert(ring.head() == 4);
    const auto third = ring.waitFrame(3);
    assert(third && third->frameOrdinal == 3 && third->draws[0].matchKey == 13);

    ring.publish(makeFrame(1, 99));
    assert(ring.head() == 4);
}

void testSkippedOrdinalFallback() {
    FrameRingBuffer ring;
    ring.publish(makeFrame(5, 55));
    assert(ring.head() == 6);

    const auto skipped = ring.waitFrame(3);
    assert(skipped && skipped->frameOrdinal == 3 && skipped->draws.empty());

    const auto present = ring.waitFrame(5);
    assert(present && present->frameOrdinal == 5 && present->draws[0].matchKey == 55);
}

void testCloseUnblocksWaiter() {
    FrameRingBuffer ring;
    ring.publish(makeFrame(0, 1));

    std::promise<std::optional<FrameSnapshot>> result;
    auto future = result.get_future();
    std::thread waiter([&] { result.set_value(ring.waitFrame(1000)); });

    const auto start = std::chrono::steady_clock::now();
    ring.close();
    assert(future.wait_for(std::chrono::milliseconds(50)) == std::future_status::ready);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    assert(elapsed < std::chrono::milliseconds(50));

    const auto frame = future.get();
    waiter.join();
    assert(frame && frame->frameOrdinal == 1000 && frame->draws.empty());
    assert(ring.closed());

    ring.publish(makeFrame(1, 2));
    assert(ring.head() == 1);
}

} // namespace

namespace n64forge::studio {

struct SoftwareBlendProbe {
    static bool blend(qint64 timestamp, qint64 lastShownTimestamp,
                      bool priorEndpointShown, bool enabled) {
        return EmuWidget::shouldBlendSoftwareFrame(timestamp, lastShownTimestamp,
                                                   priorEndpointShown, enabled);
    }
};

} // namespace n64forge::studio

namespace {

void testSoftwareBlendGate() {
    using n64forge::studio::SoftwareBlendProbe;

    assert(!SoftwareBlendProbe::blend(1, 0, true, true));  // first published frame
    assert(SoftwareBlendProbe::blend(2, 1, true, true));   // new frame after endpoint
    assert(!SoftwareBlendProbe::blend(2, 1, false, true)); // midpoint already shown
    assert(!SoftwareBlendProbe::blend(2, 2, true, true));  // same ordinal again
    assert(!SoftwareBlendProbe::blend(2, 1, true, false)); // interpolation disabled
}

} // namespace

int run_frame_ring_tests() {
    testEmptyRingFallback();
    testPublishReclaimAndStaleDrop();
    testSkippedOrdinalFallback();
    testCloseUnblocksWaiter();
    testSoftwareBlendGate();
    std::cout << "FrameRingBuffer and software interpolation gate tests passed\n";
    return 0;
}
