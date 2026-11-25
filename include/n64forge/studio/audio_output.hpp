#pragma once

#include "n64forge/emulation/audio_ring.hpp"
#include <memory>

namespace n64forge::studio {

// UI thread owns device lifetime; the guest worker only pushes to the FIFO.
class AudioOutput {
public:
    static constexpr int SampleRate = 48000;
    AudioOutput();
    ~AudioOutput();
    [[nodiscard]] bool start();
    void writeSamples(const std::int16_t* samples, std::size_t frames) noexcept;
    [[nodiscard]] auto fifoStats() const noexcept { return buffer_.stats(); }
    // Counts non-silent frames that came from the guest, not synth fade/tail.
    [[nodiscard]] std::uint64_t nonSilentFrames() const noexcept {
        return nonSilentFrames_.load(std::memory_order_relaxed);
    }
    // Frames actually accepted by the FIFO; rejected frames are overruns.
    [[nodiscard]] std::uint64_t writtenFrames() const noexcept {
        return writtenFrames_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] int peakSample() const noexcept {
        return peakSample_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] const char* backend() const noexcept;

private:
    struct Device;
    std::unique_ptr<Device> device_;
    AudioRing<8192> buffer_;
    std::atomic<std::uint64_t> nonSilentFrames_{0};
    std::atomic<std::uint64_t> writtenFrames_{0};
    std::atomic<int> peakSample_{0};
    void render(std::int16_t* output, std::size_t frames) noexcept;
};

} // namespace n64forge::studio
