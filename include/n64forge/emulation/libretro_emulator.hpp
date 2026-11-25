#pragma once

#include "n64forge/core/types.hpp"
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

// libretro header is in the project third_party directory.
#include "libretro.h"

namespace n64forge::emulation {

struct LibretroAvInfo {
    double fps = 0.0;
    double sampleRate = 0.0;
    unsigned width = 0;
    unsigned height = 0;
    retro_pixel_format pixelFormat = RETRO_PIXEL_FORMAT_0RGB1555;
};

using InputPollCallback = std::function<void()>;
using InputStateCallback = std::function<int16_t(unsigned port, unsigned device,
                                                   unsigned index, unsigned id)>;
using AudioSampleBatchCallback = std::function<void(const int16_t* data, size_t frames)>;

class LibretroEmulator {
public:
    LibretroEmulator();
    ~LibretroEmulator();

    // Non-copyable because it owns a dynamic library handle.
    LibretroEmulator(const LibretroEmulator&) = delete;
    LibretroEmulator& operator=(const LibretroEmulator&) = delete;

    [[nodiscard]] bool loadCore(const std::filesystem::path& corePath);
    void unloadCore();
    [[nodiscard]] bool loadGame(const std::filesystem::path& romPath);
    [[nodiscard]] bool reloadCore(const std::filesystem::path& corePath);
    void unload();
    void stopGame();
    void reset();
    void runFrame();

    [[nodiscard]] bool isCoreLoaded() const noexcept { return coreHandle_ != nullptr; }
    [[nodiscard]] bool isGameLoaded() const noexcept { return gameLoaded_; }
    [[nodiscard]] const LibretroAvInfo& avInfo() const noexcept { return avInfo_; }

    // Core options override. Call before loadGame for the option to take effect.
    void setVariable(const std::string& key, const std::string& value);
    void clearVariables();

    // Directories exposed to the core. These must be set before loadCore/loadGame
    // if the core requires them.
    void setSystemDirectory(const std::filesystem::path& path);
    void setSaveDirectory(const std::filesystem::path& path);
    void setCorePath(const std::filesystem::path& path);

    // Framebuffer access. The returned vector is RGBA8888, width*height*4 bytes.
    [[nodiscard]] std::vector<u8> framebuffer() const;
    [[nodiscard]] unsigned framebufferWidth() const noexcept { return fbWidth_; }
    [[nodiscard]] unsigned framebufferHeight() const noexcept { return fbHeight_; }

    void setInputPollCallback(InputPollCallback cb) { inputPoll_ = std::move(cb); }
    void setInputStateCallback(InputStateCallback cb) { inputState_ = std::move(cb); }
    void setAudioSampleBatchCallback(AudioSampleBatchCallback cb) { audioBatch_ = std::move(cb); }

    [[nodiscard]] std::string lastError() const { return lastError_; }

private:
    // libretro.h defines the callback typedefs (retro_environment_t etc.) but
    // does not provide typedefs for the core entry points on all revisions, so
    // we define our own.
    using InitFn = void (*)(void);
    using DeinitFn = void (*)(void);
    using ApiVersionFn = unsigned (*)(void);
    using GetSystemInfoFn = void (*)(retro_system_info*);
    using GetSystemAvInfoFn = void (*)(retro_system_av_info*);
    using SetControllerPortDeviceFn = void (*)(unsigned port, unsigned device);
    using ResetFn = void (*)(void);
    using RunFn = void (*)(void);
    using LoadGameFn = bool (*)(const retro_game_info* info);
    using UnloadGameFn = void (*)(void);

    using SetEnvironmentFn = void (*)(retro_environment_t cb);
    using SetVideoRefreshFn = void (*)(retro_video_refresh_t cb);
    using SetAudioSampleFn = void (*)(retro_audio_sample_t cb);
    using SetAudioSampleBatchFn = void (*)(retro_audio_sample_batch_t cb);
    using SetInputPollFn = void (*)(retro_input_poll_t cb);
    using SetInputStateFn = void (*)(retro_input_state_t cb);

    bool bindSymbols();
    void unloadGame();
    static bool environmentCallback(unsigned cmd, void* data);
    static void videoRefreshCallback(const void* data, unsigned width, unsigned height, size_t pitch);
    static void audioSampleCallback(int16_t left, int16_t right);
    static size_t audioSampleBatchCallback(const int16_t* data, size_t frames);
    static void inputPollCallback();
    static int16_t inputStateCallback(unsigned port, unsigned device, unsigned index, unsigned id);

    void convertFramebuffer(const void* data, unsigned width, unsigned height, size_t pitch);
    bool handleSetVariables(const retro_variable* vars);

    void* coreHandle_ = nullptr;

    SetEnvironmentFn retro_set_environment_ = nullptr;
    SetVideoRefreshFn retro_set_video_refresh_ = nullptr;
    SetAudioSampleFn retro_set_audio_sample_ = nullptr;
    SetAudioSampleBatchFn retro_set_audio_sample_batch_ = nullptr;
    SetInputPollFn retro_set_input_poll_ = nullptr;
    SetInputStateFn retro_set_input_state_ = nullptr;

    InitFn retro_init_ = nullptr;
    DeinitFn retro_deinit_ = nullptr;
    ApiVersionFn retro_api_version_ = nullptr;
    GetSystemInfoFn retro_get_system_info_ = nullptr;
    GetSystemAvInfoFn retro_get_system_av_info_ = nullptr;
    SetControllerPortDeviceFn retro_set_controller_port_device_ = nullptr;
    ResetFn retro_reset_ = nullptr;
    RunFn retro_run_ = nullptr;
    LoadGameFn retro_load_game_ = nullptr;
    UnloadGameFn retro_unload_game_ = nullptr;

    bool coreInitialized_ = false;
    bool gameLoaded_ = false;
    std::string lastError_;
    LibretroAvInfo avInfo_{};

    mutable std::mutex fbMutex_;
    std::vector<u8> framebuffer_;
    unsigned fbWidth_ = 0;
    unsigned fbHeight_ = 0;
    retro_pixel_format pixelFormat_ = RETRO_PIXEL_FORMAT_0RGB1555;

    InputPollCallback inputPoll_;
    InputStateCallback inputState_;
    AudioSampleBatchCallback audioBatch_;

    // Core option overrides declared by the core via SET_VARIABLES.
    std::vector<retro_variable> declaredVariables_;
    std::unordered_map<std::string, std::string> variableValues_;
    bool variablesUpdated_ = false;

    std::string systemDirectory_;
    std::string saveDirectory_;
    std::string corePathString_;

    static thread_local LibretroEmulator* current_;
};

} // namespace n64forge::emulation
