#pragma once

#include "n64forge/core/types.hpp"
#include "n64forge/input/controller.hpp"
#include <SDL3/SDL.h>
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <chrono>
#include <vector>

namespace n64forge::input {

using KeyboardStateCallback = std::function<ControllerState(int player)>;

struct PadBinding {
    enum Kind { None, Button, PositiveAxis, NegativeAxis };
    Kind kind = None;
    int index = 0;
    bool operator==(const PadBinding&) const = default;
};
struct PadProfile {
    std::array<PadBinding, ControlCount> bindings;
    double deadzone = 0.14, sensitivity = 1.0;
    std::string device; // empty: automatic; "-": no gamepad
    static PadProfile defaults();
};
struct PadSnapshot {
    std::array<bool, SDL_GAMEPAD_BUTTON_COUNT> buttons{};
    std::array<s16, SDL_GAMEPAD_AXIS_COUNT> axes{};
    [[nodiscard]] double value(PadBinding binding) const noexcept;
    [[nodiscard]] ControllerState mapped(const PadProfile& profile) const;
    [[nodiscard]] std::optional<PadBinding> activeBinding() const;
};
struct PadDevice { unsigned slot; std::string name, key; };

class SdlInputManager {
public:
    explicit SdlInputManager(KeyboardStateCallback keyboardState = {});
    ~SdlInputManager();

    SdlInputManager(const SdlInputManager&) = delete;
    SdlInputManager& operator=(const SdlInputManager&) = delete;

    [[nodiscard]] bool initialized() const noexcept { return initialized_; }
    // SDL uses a full-scale 16-bit motor amplitude; keep the conversion here so
    // the configured Studio percentage can be tested without a gamepad.
    [[nodiscard]] static constexpr u16 rumbleStrength(unsigned percent) noexcept {
        const unsigned clamped = percent > 100 ? 100 : percent;
        return static_cast<u16>(clamped * 65535u / 100u);
    }
    void update();
    [[nodiscard]] std::vector<PadDevice> devices() const;
    [[nodiscard]] const PadSnapshot* pad(unsigned slot) const;
    [[nodiscard]] std::string_view deviceKey(unsigned slot) const;
    void rumble(unsigned slot, unsigned percent);

    [[nodiscard]] int16_t state(unsigned port, unsigned device, unsigned index, unsigned id) const;

private:
    void openGamepads();
    void closeGamepads();

    KeyboardStateCallback keyboardState_;
    bool initialized_ = false;

    struct GamepadState {
        SDL_Gamepad* handle = nullptr;
        SDL_JoystickID id = 0;
        std::string key;
        PadSnapshot snapshot;
        std::array<int16_t, 16> buttons{};
        std::array<int16_t, 4> analog{}; // LX, LY, RX, RY
    };
    std::array<GamepadState, 4> gamepads_{};
    std::chrono::steady_clock::time_point nextDiscovery_{};
};

} // namespace n64forge::input
