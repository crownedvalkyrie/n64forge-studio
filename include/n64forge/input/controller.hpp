#pragma once
#include "n64forge/core/types.hpp"
#include <array>
#include <cstdint>
#include <algorithm>
#include <cmath>

namespace n64forge::input {

enum class N64Button : u16 {
    A       = 0x8000,
    B       = 0x4000,
    Z       = 0x2000,
    Start   = 0x1000,
    DUp     = 0x0800,
    DDown   = 0x0400,
    DLeft   = 0x0200,
    DRight  = 0x0100,
    L       = 0x0020,
    R       = 0x0010,
    CUp     = 0x0008,
    CDown   = 0x0004,
    CLeft   = 0x0002,
    CRight  = 0x0001,
};

struct ControllerState {
    u16 buttons = 0;
    s8 stickX = 0;
    s8 stickY = 0;
};

enum class Control : u8 {
    A, B, Z, Start, DUp, DDown, DLeft, DRight, L, R, CUp, CDown, CLeft, CRight,
    StickUp, StickDown, StickLeft, StickRight, Count
};
inline constexpr unsigned ControlCount = static_cast<unsigned>(Control::Count);
inline constexpr unsigned DigitalControls = static_cast<unsigned>(Control::StickUp);
inline constexpr std::array ControlButtons{
    N64Button::A, N64Button::B, N64Button::Z, N64Button::Start,
    N64Button::DUp, N64Button::DDown, N64Button::DLeft, N64Button::DRight,
    N64Button::L, N64Button::R, N64Button::CUp, N64Button::CDown, N64Button::CLeft, N64Button::CRight
};
inline constexpr s8 StickLimit = 80;

inline double normalizeStick(s16 value) noexcept {
    return value < 0 ? value / 32768.0 : value / 32767.0;
}

// Radial inner deadzone with remapping of the remaining throw. Travel is
// bounded to a nominal 80-unit stick, including diagonal movement.
inline std::array<s8, 2> scaleStick(double x, double y, double deadzone, double sensitivity) {
    const double length = std::hypot(x, y);
    deadzone = std::clamp(deadzone, 0.0, 0.95);
    if (length <= deadzone) return {};
    const double scale = std::clamp((length - deadzone) * sensitivity / (1 - deadzone), 0.0, 1.0)
                         * StickLimit / length;
    return {static_cast<s8>(std::lround(x * scale)), static_cast<s8>(std::lround(y * scale))};
}

inline ControllerState mergeControllers(ControllerState keyboard, const ControllerState& gamepad) {
    keyboard.buttons |= gamepad.buttons;
    if (!keyboard.stickX && !keyboard.stickY) {
        keyboard.stickX = gamepad.stickX;
        keyboard.stickY = gamepad.stickY;
    }
    return keyboard;
}

class Controller {
public:
    void setButton(N64Button button, bool pressed);
    void setStick(s8 x, s8 y) { state_.stickX = x; state_.stickY = y; }
    [[nodiscard]] const ControllerState& state() const noexcept { return state_; }
    void reset() { state_ = {}; }

private:
    ControllerState state_;
};

[[nodiscard]] inline constexpr const char* buttonName(N64Button button) {
    switch (button) {
        case N64Button::A: return "A";
        case N64Button::B: return "B";
        case N64Button::Z: return "Z";
        case N64Button::Start: return "Start";
        case N64Button::DUp: return "D-Up";
        case N64Button::DDown: return "D-Down";
        case N64Button::DLeft: return "D-Left";
        case N64Button::DRight: return "D-Right";
        case N64Button::L: return "L";
        case N64Button::R: return "R";
        case N64Button::CUp: return "C-Up";
        case N64Button::CDown: return "C-Down";
        case N64Button::CLeft: return "C-Left";
        case N64Button::CRight: return "C-Right";
    }
    return "";
}

inline constexpr const char* controlName(Control control) {
    const auto i = static_cast<unsigned>(control);
    constexpr std::array names{"Stick Up", "Stick Down", "Stick Left", "Stick Right"};
    if (i < DigitalControls) return buttonName(ControlButtons[i]);
    return i < ControlCount ? names[i - DigitalControls] : "";
}

} // namespace n64forge::input
