#include "n64forge/studio/audio_output.hpp"
#include <algorithm>
#include <cstdlib>
#ifdef N64FORGE_HAS_SDL3
#include <SDL3/SDL.h>
#endif
#ifdef __APPLE__
#include <AudioToolbox/AudioToolbox.h>
#endif

namespace n64forge::studio {
struct AudioOutput::Device {
#ifdef N64FORGE_HAS_SDL3
    SDL_AudioStream* stream = nullptr;
    bool initialized = false;
    static void SDLCALL supply(void* data, SDL_AudioStream* stream, int bytes, int) {
        auto& output = *static_cast<AudioOutput*>(data);
        std::int16_t samples[1024];
        int frames = bytes > 0 ? 1 + (bytes - 1) / 4 : 0;
        bool silent = false;
        while (frames > 0) {
            const int count = std::min(frames, 512);
            if (!silent) {
                output.render(samples, count);
                if (SDL_PutAudioStreamData(stream, samples, count * 4)) {
                    frames -= count;
                    continue;
                }
                // Retry this chunk as silence and keep every later chunk
                // silent instead of leaving the requested bytes unfilled.
                silent = true;
            }
            std::fill_n(samples, static_cast<std::size_t>(count) * 2, std::int16_t{0});
            static_cast<void>(SDL_PutAudioStreamData(stream, samples, count * 4));
            frames -= count;
        }
    }
#endif
#ifdef __APPLE__
    AudioQueueRef queue = nullptr;
    static void supplyCoreAudio(void* data, AudioQueueRef queue, AudioQueueBufferRef buffer) {
        auto& output = *static_cast<AudioOutput*>(data);
        output.render(static_cast<std::int16_t*>(buffer->mAudioData), buffer->mAudioDataBytesCapacity / 4);
        buffer->mAudioDataByteSize = buffer->mAudioDataBytesCapacity;
        AudioQueueEnqueueBuffer(queue, buffer, 0, nullptr);
    }
#endif
    ~Device() {
#ifdef N64FORGE_HAS_SDL3
        if (stream) SDL_DestroyAudioStream(stream);
        if (initialized) SDL_QuitSubSystem(SDL_INIT_AUDIO);
#endif
#ifdef __APPLE__
        if (queue) AudioQueueDispose(queue, true);
#endif
    }
};

AudioOutput::AudioOutput() = default;
AudioOutput::~AudioOutput() { device_.reset(); } // stop callbacks before destroying the FIFO

bool AudioOutput::start() {
    if (device_) return true;
    auto device = std::make_unique<Device>();
#ifdef N64FORGE_HAS_SDL3
    if ((device->initialized = SDL_InitSubSystem(SDL_INIT_AUDIO))) {
        const SDL_AudioSpec format{SDL_AUDIO_S16, 2, SampleRate};
        device->stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &format,
                                                  &Device::supply, this);
        if (device->stream && SDL_ResumeAudioStreamDevice(device->stream)) {
            device_ = std::move(device);
            return true;
        }
        if (device->stream) SDL_DestroyAudioStream(device->stream);
        device->stream = nullptr;
    }
#endif
#ifdef __APPLE__
    AudioStreamBasicDescription format{};
    format.mSampleRate = SampleRate;
    format.mFormatID = kAudioFormatLinearPCM;
    format.mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked |
                          kAudioFormatFlagsNativeEndian;
    format.mFramesPerPacket = 1;
    format.mBytesPerFrame = format.mBytesPerPacket = 4;
    format.mChannelsPerFrame = 2;
    format.mBitsPerChannel = 16;
    if (AudioQueueNewOutput(&format, &Device::supplyCoreAudio, this, nullptr, nullptr, 0,
                            &device->queue) != noErr) return false;
    for (int i = 0; i < 3; ++i) {
        AudioQueueBufferRef buffer = nullptr;
        if (AudioQueueAllocateBuffer(device->queue, 512 * 4, &buffer) != noErr) return false;
        std::fill_n(static_cast<std::int16_t*>(buffer->mAudioData), 1024, 0);
        buffer->mAudioDataByteSize = 512 * 4;
        if (AudioQueueEnqueueBuffer(device->queue, buffer, 0, nullptr) != noErr) return false;
    }
    if (AudioQueueStart(device->queue, nullptr) != noErr) return false;
    device_ = std::move(device);
    return true;
#else
    return false;
#endif
}

void AudioOutput::writeSamples(const std::int16_t* samples, std::size_t frames) noexcept {
    if (!samples || !frames) return;
    const auto accepted = buffer_.push(samples, frames); // overruns stay in fifoStats()
    if (accepted) writtenFrames_.fetch_add(accepted, std::memory_order_relaxed);
    int peak = 0;
    for (std::size_t i = 0; i < frames * 2; ++i)
        peak = std::max(peak, std::abs(static_cast<int>(samples[i])));
    int observed = peakSample_.load(std::memory_order_relaxed);
    while (peak > observed &&
           !peakSample_.compare_exchange_weak(observed, peak, std::memory_order_relaxed)) {
    }
}

void AudioOutput::render(std::int16_t* output, std::size_t frames) noexcept {
    std::uint64_t nonSilent = 0;
    buffer_.render(frames, [&](const auto& frame) {
        *output++ = frame[0];
        *output++ = frame[1];
        if (frame.fromGuest && (frame[0] || frame[1])) ++nonSilent;
    });
    nonSilentFrames_.fetch_add(nonSilent, std::memory_order_relaxed);
}

const char* AudioOutput::backend() const noexcept {
    if (!device_) return "unavailable";
#ifdef N64FORGE_HAS_SDL3
    if (device_->stream) return "SDL3";
#endif
    return "CoreAudio";
}
} // namespace n64forge::studio
