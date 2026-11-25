#include "n64forge/emulation/libretro_emulator.hpp"
#include "n64forge/core/cartridge.hpp"
#include <algorithm>
#include <cstdarg>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace n64forge::emulation {

thread_local LibretroEmulator* LibretroEmulator::current_ = nullptr;

namespace {

void* loadSymbol(void* handle, const char* name) {
#ifdef _WIN32
    return GetProcAddress(static_cast<HMODULE>(handle), name);
#else
    return dlsym(handle, name);
#endif
}

std::string dlError() {
#ifdef _WIN32
    return std::to_string(GetLastError());
#else
    const char* err = dlerror();
    return err ? err : "unknown";
#endif
}

void retroLogCallback(enum retro_log_level level, const char* fmt, ...) {
    (void)level;
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
}

std::vector<u8> readFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) return {};
    const auto size = file.tellg();
    file.seekg(0, std::ios::beg);
    std::vector<u8> data(static_cast<size_t>(size));
    if (size > 0) {
        file.read(reinterpret_cast<char*>(data.data()), size);
    }
    return data;
}

// Parses "description; value1|value2|..." and returns the first value.
std::string parseVariableDefault(const char* value) {
    if (!value) return {};
    const char* semi = std::strchr(value, ';');
    if (!semi) return {};
    const char* start = semi + 1;
    while (*start == ' ') ++start;
    const char* pipe = std::strchr(start, '|');
    if (pipe) {
        std::string def(start, static_cast<size_t>(pipe - start));
        // Trim trailing spaces.
        while (!def.empty() && def.back() == ' ') def.pop_back();
        return def;
    }
    return std::string(start);
}

} // namespace

LibretroEmulator::LibretroEmulator() {
    // Provide safe defaults so cores that copy these strings immediately do not
    // dereference nullptr. Frontends can override with explicit directories.
    systemDirectory_ = (std::filesystem::current_path() / "system").string();
    saveDirectory_ = (std::filesystem::current_path() / "save").string();
}

LibretroEmulator::~LibretroEmulator() {
    unload();
}

void LibretroEmulator::setVariable(const std::string& key, const std::string& value) {
    variableValues_[key] = value;
}

void LibretroEmulator::clearVariables() {
    variableValues_.clear();
    declaredVariables_.clear();
}

void LibretroEmulator::setSystemDirectory(const std::filesystem::path& path) {
    systemDirectory_ = path.string();
}

void LibretroEmulator::setSaveDirectory(const std::filesystem::path& path) {
    saveDirectory_ = path.string();
}

void LibretroEmulator::setCorePath(const std::filesystem::path& path) {
    corePathString_ = path.string();
}

bool LibretroEmulator::loadCore(const std::filesystem::path& corePath) {
    unload();

#ifdef _WIN32
    coreHandle_ = LoadLibraryW(corePath.c_str());
#else
    coreHandle_ = dlopen(corePath.c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
    if (!coreHandle_) {
        lastError_ = "Failed to load core: " + dlError();
        return false;
    }

    if (!bindSymbols()) {
        unload();
        return false;
    }

    if (retro_api_version_) {
        const unsigned apiVersion = retro_api_version_();
        if (apiVersion != RETRO_API_VERSION) {
            lastError_ = "Unsupported libretro API version: " + std::to_string(apiVersion);
            unload();
            return false;
        }
    }

    // Prepare callbacks. retro_set_environment must be called before retro_init.
    current_ = this;
    if (retro_set_environment_) {
        retro_set_environment_(environmentCallback);
    }

    if (retro_set_video_refresh_) retro_set_video_refresh_(videoRefreshCallback);
    if (retro_set_audio_sample_) retro_set_audio_sample_(audioSampleCallback);
    if (retro_set_audio_sample_batch_) retro_set_audio_sample_batch_(audioSampleBatchCallback);
    if (retro_set_input_poll_) retro_set_input_poll_(inputPollCallback);
    if (retro_set_input_state_) retro_set_input_state_(inputStateCallback);

    if (retro_init_) {
        retro_init_();
        coreInitialized_ = true;
    }
    current_ = nullptr;

    return true;
}

bool LibretroEmulator::bindSymbols() {
    retro_set_environment_ = reinterpret_cast<SetEnvironmentFn>(loadSymbol(coreHandle_, "retro_set_environment"));
    retro_set_video_refresh_ = reinterpret_cast<SetVideoRefreshFn>(loadSymbol(coreHandle_, "retro_set_video_refresh"));
    retro_set_audio_sample_ = reinterpret_cast<SetAudioSampleFn>(loadSymbol(coreHandle_, "retro_set_audio_sample"));
    retro_set_audio_sample_batch_ = reinterpret_cast<SetAudioSampleBatchFn>(loadSymbol(coreHandle_, "retro_set_audio_sample_batch"));
    retro_set_input_poll_ = reinterpret_cast<SetInputPollFn>(loadSymbol(coreHandle_, "retro_set_input_poll"));
    retro_set_input_state_ = reinterpret_cast<SetInputStateFn>(loadSymbol(coreHandle_, "retro_set_input_state"));

    retro_init_ = reinterpret_cast<InitFn>(loadSymbol(coreHandle_, "retro_init"));
    retro_deinit_ = reinterpret_cast<DeinitFn>(loadSymbol(coreHandle_, "retro_deinit"));
    retro_api_version_ = reinterpret_cast<ApiVersionFn>(loadSymbol(coreHandle_, "retro_api_version"));
    retro_get_system_info_ = reinterpret_cast<GetSystemInfoFn>(loadSymbol(coreHandle_, "retro_get_system_info"));
    retro_get_system_av_info_ = reinterpret_cast<GetSystemAvInfoFn>(loadSymbol(coreHandle_, "retro_get_system_av_info"));
    retro_set_controller_port_device_ = reinterpret_cast<SetControllerPortDeviceFn>(loadSymbol(coreHandle_, "retro_set_controller_port_device"));
    retro_reset_ = reinterpret_cast<ResetFn>(loadSymbol(coreHandle_, "retro_reset"));
    retro_run_ = reinterpret_cast<RunFn>(loadSymbol(coreHandle_, "retro_run"));
    retro_load_game_ = reinterpret_cast<LoadGameFn>(loadSymbol(coreHandle_, "retro_load_game"));
    retro_unload_game_ = reinterpret_cast<UnloadGameFn>(loadSymbol(coreHandle_, "retro_unload_game"));

    if (!retro_init_ || !retro_deinit_ || !retro_run_ || !retro_load_game_ ||
        !retro_unload_game_ || !retro_get_system_av_info_) {
        lastError_ = "Core is missing required libretro entry points.";
        return false;
    }
    return true;
}

bool LibretroEmulator::loadGame(const std::filesystem::path& romPath) {
    if (!isCoreLoaded()) return false;
    unloadGame();

    // Use the core's byte-order-normalized view when it is a ROM we understand.
    // This also transparently handles a single-ROM ZIP without writing a temp file.
    Cartridge cartridge;
    std::vector<u8> romData = cartridge.load(romPath) ? cartridge.rom() : readFile(romPath);
    retro_game_info info{};
    std::string pathStr = romPath.string();
    info.path = pathStr.c_str();
    if (!romData.empty()) {
        info.data = romData.data();
        info.size = romData.size();
    }

    current_ = this;
    const bool ok = retro_load_game_(&info);
    if (ok) {
        gameLoaded_ = true;
        retro_system_av_info av{};
        retro_get_system_av_info_(&av);
        avInfo_.fps = av.timing.fps;
        avInfo_.sampleRate = av.timing.sample_rate;
        avInfo_.width = av.geometry.base_width;
        avInfo_.height = av.geometry.base_height;
        avInfo_.pixelFormat = pixelFormat_;
        if (retro_set_controller_port_device_) {
            retro_set_controller_port_device_(0, RETRO_DEVICE_JOYPAD);
        }
    }
    current_ = nullptr;
    return ok;
}

void LibretroEmulator::unloadCore() {
    // Full core teardown: release the running game, deinitialize the core and
    // unload the shared library. Used before loading a different ROM so that no
    // state from the previous game can leak into the next load.
    unloadGame();
    if (coreInitialized_ && retro_deinit_) {
        current_ = this;
        retro_deinit_();
        current_ = nullptr;
        coreInitialized_ = false;
    }
    if (coreHandle_) {
#ifdef _WIN32
        FreeLibrary(static_cast<HMODULE>(coreHandle_));
#else
        dlclose(coreHandle_);
#endif
        coreHandle_ = nullptr;
    }
    variableValues_.clear();
    declaredVariables_.clear();
    variablesUpdated_ = false;
}

bool LibretroEmulator::reloadCore(const std::filesystem::path& corePath) {
    unloadCore();
    return loadCore(corePath);
}

void LibretroEmulator::unload() {
    unloadGame();
    if (coreInitialized_ && retro_deinit_) {
        current_ = this;
        retro_deinit_();
        current_ = nullptr;
        coreInitialized_ = false;
    }
    if (coreHandle_) {
#ifdef _WIN32
        FreeLibrary(static_cast<HMODULE>(coreHandle_));
#else
        dlclose(coreHandle_);
#endif
        coreHandle_ = nullptr;
    }
}

void LibretroEmulator::unloadGame() {
    if (gameLoaded_ && retro_unload_game_) {
        current_ = this;
        retro_unload_game_();
        current_ = nullptr;
        gameLoaded_ = false;
    }
}

void LibretroEmulator::stopGame() {
    unloadGame();
}

void LibretroEmulator::reset() {
    if (gameLoaded_ && retro_reset_) {
        current_ = this;
        retro_reset_();
        current_ = nullptr;
    }
}

void LibretroEmulator::runFrame() {
    if (!gameLoaded_ || !retro_run_) return;
    current_ = this;
    retro_run_();
    current_ = nullptr;
}

std::vector<u8> LibretroEmulator::framebuffer() const {
    std::lock_guard<std::mutex> lock(fbMutex_);
    return framebuffer_;
}

void LibretroEmulator::convertFramebuffer(const void* data, unsigned width, unsigned height, size_t pitch) {
    if (!data || width == 0 || height == 0) return;
    std::lock_guard<std::mutex> lock(fbMutex_);
    fbWidth_ = width;
    fbHeight_ = height;
    framebuffer_.resize(static_cast<size_t>(width) * height * 4);

    const u8* src = static_cast<const u8*>(data);
    u8* dst = framebuffer_.data();

    for (unsigned y = 0; y < height; ++y) {
        for (unsigned x = 0; x < width; ++x) {
            u8 r = 0, g = 0, b = 0, a = 255;
            if (pixelFormat_ == RETRO_PIXEL_FORMAT_XRGB8888) {
                b = src[y * pitch + x * 4 + 0];
                g = src[y * pitch + x * 4 + 1];
                r = src[y * pitch + x * 4 + 2];
            } else if (pixelFormat_ == RETRO_PIXEL_FORMAT_RGB565) {
                const u16 p = src[y * pitch + x * 2 + 0] |
                              (static_cast<u16>(src[y * pitch + x * 2 + 1]) << 8);
                r = static_cast<u8>(((p >> 11) & 0x1F) << 3);
                g = static_cast<u8>(((p >> 5) & 0x3F) << 2);
                b = static_cast<u8>((p & 0x1F) << 3);
            } else { // RETRO_PIXEL_FORMAT_0RGB1555
                const u16 p = src[y * pitch + x * 2 + 0] |
                              (static_cast<u16>(src[y * pitch + x * 2 + 1]) << 8);
                r = static_cast<u8>(((p >> 10) & 0x1F) << 3);
                g = static_cast<u8>(((p >> 5) & 0x1F) << 3);
                b = static_cast<u8>((p & 0x1F) << 3);
            }
            dst[(y * width + x) * 4 + 0] = r;
            dst[(y * width + x) * 4 + 1] = g;
            dst[(y * width + x) * 4 + 2] = b;
            dst[(y * width + x) * 4 + 3] = a;
        }
    }
}

bool LibretroEmulator::handleSetVariables(const retro_variable* vars) {
    declaredVariables_.clear();
    if (!vars) return true;
    for (const retro_variable* v = vars; v && v->key; ++v) {
        declaredVariables_.push_back(*v);
        const std::string key(v->key);
        if (variableValues_.find(key) == variableValues_.end()) {
            variableValues_[key] = parseVariableDefault(v->value);
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// libretro callbacks
// ---------------------------------------------------------------------------

bool LibretroEmulator::environmentCallback(unsigned cmd, void* data) {
    if (!current_) return false;
    switch (cmd) {
        case RETRO_ENVIRONMENT_SET_ROTATION:
            return true;
        case RETRO_ENVIRONMENT_GET_OVERSCAN: {
            bool* overscan = static_cast<bool*>(data);
            *overscan = false;
            return true;
        }
        case RETRO_ENVIRONMENT_GET_CAN_DUPE: {
            bool* canDupe = static_cast<bool*>(data);
            *canDupe = true;
            return true;
        }
        case RETRO_ENVIRONMENT_SET_MESSAGE: {
            const auto* msg = static_cast<retro_message*>(data);
            if (msg && msg->msg) {
                std::cerr << "[libretro core] " << msg->msg << '\n';
            }
            return true;
        }
        case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: {
            const auto* fmt = static_cast<retro_pixel_format*>(data);
            if (*fmt == RETRO_PIXEL_FORMAT_0RGB1555 ||
                *fmt == RETRO_PIXEL_FORMAT_RGB565 ||
                *fmt == RETRO_PIXEL_FORMAT_XRGB8888) {
                current_->pixelFormat_ = *fmt;
                return true;
            }
            return false;
        }
        case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY: {
            const char** dir = static_cast<const char**>(data);
            *dir = current_->systemDirectory_.c_str();
            return true;
        }
        case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY: {
            const char** dir = static_cast<const char**>(data);
            *dir = current_->saveDirectory_.c_str();
            return true;
        }
        case RETRO_ENVIRONMENT_GET_LOG_INTERFACE: {
            auto* cb = static_cast<retro_log_callback*>(data);
            cb->log = retroLogCallback;
            return true;
        }
        case RETRO_ENVIRONMENT_GET_VARIABLE: {
            auto* var = static_cast<retro_variable*>(data);
            if (!var || !var->key) return false;
            const auto it = current_->variableValues_.find(var->key);
            if (it != current_->variableValues_.end()) {
                var->value = it->second.c_str();
                return true;
            }
            var->value = nullptr;
            return false;
        }
        case RETRO_ENVIRONMENT_SET_VARIABLES: {
            const auto* vars = static_cast<const retro_variable*>(data);
            return current_->handleSetVariables(vars);
        }
        case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE: {
            bool* updated = static_cast<bool*>(data);
            *updated = current_->variablesUpdated_;
            current_->variablesUpdated_ = false;
            return true;
        }
        case RETRO_ENVIRONMENT_GET_INPUT_MAX_USERS: {
            unsigned* maxUsers = static_cast<unsigned*>(data);
            *maxUsers = 4;
            return true;
        }
        case RETRO_ENVIRONMENT_GET_LANGUAGE: {
            unsigned* language = static_cast<unsigned*>(data);
            *language = RETRO_LANGUAGE_ENGLISH;
            return true;
        }
        case RETRO_ENVIRONMENT_GET_LIBRETRO_PATH: {
            const char** path = static_cast<const char**>(data);
            if (current_->corePathString_.empty()) return false;
            *path = current_->corePathString_.c_str();
            return true;
        }
        case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO:
        case RETRO_ENVIRONMENT_SET_PERFORMANCE_LEVEL:
        case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
        case RETRO_ENVIRONMENT_SET_KEYBOARD_CALLBACK:
        case RETRO_ENVIRONMENT_SET_DISK_CONTROL_INTERFACE:
        case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
        case RETRO_ENVIRONMENT_SET_FRAME_TIME_CALLBACK:
        case RETRO_ENVIRONMENT_SET_AUDIO_CALLBACK:
        case RETRO_ENVIRONMENT_GET_PERF_INTERFACE:
        case RETRO_ENVIRONMENT_GET_LOCATION_INTERFACE:
        case RETRO_ENVIRONMENT_GET_CAMERA_INTERFACE:
        case RETRO_ENVIRONMENT_GET_SENSOR_INTERFACE:
        case RETRO_ENVIRONMENT_GET_RUMBLE_INTERFACE:
        case RETRO_ENVIRONMENT_GET_INPUT_DEVICE_CAPABILITIES:
        case RETRO_ENVIRONMENT_GET_CORE_ASSETS_DIRECTORY:
        case RETRO_ENVIRONMENT_SET_SUBSYSTEM_INFO:
        case RETRO_ENVIRONMENT_SET_MEMORY_MAPS:
        case RETRO_ENVIRONMENT_SET_GEOMETRY:
        case RETRO_ENVIRONMENT_GET_USERNAME:
        case RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE:
        case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO:
        case RETRO_ENVIRONMENT_SET_PROC_ADDRESS_CALLBACK:
        case RETRO_ENVIRONMENT_SET_HW_RENDER:
        default:
            return false;
    }
}

void LibretroEmulator::videoRefreshCallback(const void* data, unsigned width, unsigned height, size_t pitch) {
    if (current_) {
        current_->convertFramebuffer(data, width, height, pitch);
    }
}

void LibretroEmulator::audioSampleCallback(int16_t left, int16_t right) {
    if (current_ && current_->audioBatch_) {
        const int16_t frame[2] = {left, right};
        current_->audioBatch_(frame, 1);
    }
}

size_t LibretroEmulator::audioSampleBatchCallback(const int16_t* data, size_t frames) {
    if (current_ && current_->audioBatch_) {
        current_->audioBatch_(data, frames);
    }
    return frames;
}

void LibretroEmulator::inputPollCallback() {
    if (current_ && current_->inputPoll_) {
        current_->inputPoll_();
    }
}

int16_t LibretroEmulator::inputStateCallback(unsigned port, unsigned device, unsigned index, unsigned id) {
    if (current_ && current_->inputState_) {
        return current_->inputState_(port, device, index, id);
    }
    return 0;
}

} // namespace n64forge::emulation
