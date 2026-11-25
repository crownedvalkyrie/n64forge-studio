#include "n64forge/input/sdl_input_manager.hpp"
#include "libretro.h"
#include <algorithm>
#include <cmath>
#include <iostream>

namespace n64forge::input {

namespace {

constexpr int16_t AXIS_MAX = 0x7fff;

[[nodiscard]] int16_t normalizeAxis(Sint16 value) {
    // SDL axis range is -32768..32767. Scale to libretro's -0x7fff..0x7fff.
    return std::max<int>(value, -AXIS_MAX);
}

} // namespace

PadProfile PadProfile::defaults() {
    using B = PadBinding;
    return {{{
        {B::Button, SDL_GAMEPAD_BUTTON_SOUTH}, {B::Button, SDL_GAMEPAD_BUTTON_WEST},
        {B::PositiveAxis, SDL_GAMEPAD_AXIS_LEFT_TRIGGER}, {B::Button, SDL_GAMEPAD_BUTTON_START},
        {B::Button, SDL_GAMEPAD_BUTTON_DPAD_UP}, {B::Button, SDL_GAMEPAD_BUTTON_DPAD_DOWN},
        {B::Button, SDL_GAMEPAD_BUTTON_DPAD_LEFT}, {B::Button, SDL_GAMEPAD_BUTTON_DPAD_RIGHT},
        {B::Button, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER}, {B::Button, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER},
        {B::NegativeAxis, SDL_GAMEPAD_AXIS_RIGHTY}, {B::PositiveAxis, SDL_GAMEPAD_AXIS_RIGHTY},
        {B::NegativeAxis, SDL_GAMEPAD_AXIS_RIGHTX}, {B::PositiveAxis, SDL_GAMEPAD_AXIS_RIGHTX},
        {B::NegativeAxis, SDL_GAMEPAD_AXIS_LEFTY}, {B::PositiveAxis, SDL_GAMEPAD_AXIS_LEFTY},
        {B::NegativeAxis, SDL_GAMEPAD_AXIS_LEFTX}, {B::PositiveAxis, SDL_GAMEPAD_AXIS_LEFTX},
    }}, 0.14, 1.0, {}};
}

double PadSnapshot::value(PadBinding b) const noexcept {
    if (b.index < 0) return 0;
    const auto i = static_cast<unsigned>(b.index);
    if (b.kind == PadBinding::Button) return i < buttons.size() && buttons[i] ? 1 : 0;
    if (i >= axes.size()) return 0;
    if (b.kind == PadBinding::PositiveAxis) return std::max(0.0, normalizeStick(axes[i]));
    if (b.kind == PadBinding::NegativeAxis) return std::max(0.0, -normalizeStick(axes[i]));
    return 0;
}

ControllerState PadSnapshot::mapped(const PadProfile& profile) const {
    ControllerState state;
    for (unsigned i = 0; i < DigitalControls; ++i)
        if (value(profile.bindings[i]) >= 0.55) state.buttons |= static_cast<u16>(ControlButtons[i]);
    const auto get = [&](Control c) { return value(profile.bindings[static_cast<unsigned>(c)]); };
    const auto [x, y] = scaleStick(get(Control::StickRight) - get(Control::StickLeft),
                                   get(Control::StickUp) - get(Control::StickDown),
                                   profile.deadzone, profile.sensitivity);
    state.stickX = x;
    state.stickY = y;
    return state;
}

std::optional<PadBinding> PadSnapshot::activeBinding() const {
    for (unsigned i = 0; i < buttons.size(); ++i)
        if (buttons[i]) return PadBinding{PadBinding::Button, static_cast<int>(i)};
    std::optional<PadBinding> result;
    double threshold = 0.65;
    for (unsigned i = 0; i < axes.size(); ++i) {
        const auto v = normalizeStick(axes[i]);
        if (std::abs(v) > threshold) {
            threshold = std::abs(v);
            result = PadBinding{v < 0 ? PadBinding::NegativeAxis : PadBinding::PositiveAxis, static_cast<int>(i)};
        }
    }
    return result;
}

SdlInputManager::SdlInputManager(KeyboardStateCallback keyboardState)
    : keyboardState_(std::move(keyboardState)) {
    const bool ok = SDL_InitSubSystem(SDL_INIT_GAMEPAD);
    if (!ok) {
        std::cerr << "SDL_Init(SDL_INIT_GAMEPAD) failed: " << SDL_GetError() << '\n';
        return;
    }
    initialized_ = true;
    openGamepads();
}

SdlInputManager::~SdlInputManager() {
    closeGamepads();
    if (initialized_) {
        SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
    }
}

void SdlInputManager::rumble(unsigned slot, unsigned percent) {
    if (slot >= gamepads_.size() || !gamepads_[slot].handle) return;
    const auto strength = static_cast<Uint16>(rumbleStrength(percent));
    SDL_RumbleGamepad(gamepads_[slot].handle, strength, strength, strength ? 250 : 0);
}

void SdlInputManager::openGamepads() {
    if (!initialized_) return;
    int count = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&count);
    if (!ids) return;
    for (int i = 0; i < count; ++i) {
        const auto id = ids[i];
        if (std::any_of(gamepads_.begin(), gamepads_.end(), [id](const auto& p) { return p.id == id; }))
            continue;
        const auto slot = std::find_if(gamepads_.begin(), gamepads_.end(), [](const auto& p) { return !p.handle; });
        if (slot == gamepads_.end()) break;
        if (auto* pad = SDL_OpenGamepad(id)) {
            slot->handle = pad;
            slot->id = id;
            char guid[33]{};
            SDL_GUIDToString(SDL_GetJoystickGUIDForID(id), guid, sizeof(guid));
            slot->key = guid;
            if (const auto* serial = SDL_GetGamepadSerial(pad); serial && *serial)
                slot->key += std::string("/") + serial;
        }
    }
    SDL_free(ids);
}

void SdlInputManager::closeGamepads() {
    for (auto& pad : gamepads_) {
        if (pad.handle) {
            SDL_RumbleGamepad(pad.handle, 0, 0, 0);
            SDL_CloseGamepad(pad.handle);
            pad.handle = nullptr;
        }
        pad.buttons.fill(0);
        pad.analog.fill(0);
    }
}

void SdlInputManager::update() {
    if (!initialized_) return;
    SDL_UpdateGamepads();

    for (auto& pad : gamepads_) {
        if (pad.handle && !SDL_GamepadConnected(pad.handle)) {
            SDL_CloseGamepad(pad.handle);
            pad = {};
        }
    }
    const auto now = std::chrono::steady_clock::now();
    if (now >= nextDiscovery_) {
        nextDiscovery_ = now + std::chrono::milliseconds(250);
        openGamepads();
    }

    for (auto& pad : gamepads_) {
        if (!pad.handle) continue;
        for (unsigned i = 0; i < pad.snapshot.buttons.size(); ++i)
            pad.snapshot.buttons[i] = SDL_GetGamepadButton(pad.handle, static_cast<SDL_GamepadButton>(i));
        for (unsigned i = 0; i < pad.snapshot.axes.size(); ++i)
            pad.snapshot.axes[i] = SDL_GetGamepadAxis(pad.handle, static_cast<SDL_GamepadAxis>(i));
        pad.buttons.fill(0);
        pad.analog.fill(0);

        // Face buttons
        pad.buttons[8] = SDL_GetGamepadButton(pad.handle, SDL_GAMEPAD_BUTTON_SOUTH) ? 1 : 0; // A
        pad.buttons[0] = SDL_GetGamepadButton(pad.handle, SDL_GAMEPAD_BUTTON_EAST) ? 1 : 0;  // B
        pad.buttons[3] = SDL_GetGamepadButton(pad.handle, SDL_GAMEPAD_BUTTON_START) ? 1 : 0; // Start
        // D-pad
        pad.buttons[4] = SDL_GetGamepadButton(pad.handle, SDL_GAMEPAD_BUTTON_DPAD_UP) ? 1 : 0;
        pad.buttons[5] = SDL_GetGamepadButton(pad.handle, SDL_GAMEPAD_BUTTON_DPAD_DOWN) ? 1 : 0;
        pad.buttons[6] = SDL_GetGamepadButton(pad.handle, SDL_GAMEPAD_BUTTON_DPAD_LEFT) ? 1 : 0;
        pad.buttons[7] = SDL_GetGamepadButton(pad.handle, SDL_GAMEPAD_BUTTON_DPAD_RIGHT) ? 1 : 0;
        // Shoulders
        pad.buttons[10] = SDL_GetGamepadButton(pad.handle, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER) ? 1 : 0; // L
        pad.buttons[11] = SDL_GetGamepadButton(pad.handle, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER) ? 1 : 0; // R
        // Z mapped to left trigger
        pad.buttons[12] = (SDL_GetGamepadAxis(pad.handle, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 0) ? 1 : 0;

        // Analog sticks
        pad.analog[0] = normalizeAxis(SDL_GetGamepadAxis(pad.handle, SDL_GAMEPAD_AXIS_LEFTX));
        pad.analog[1] = normalizeAxis(SDL_GetGamepadAxis(pad.handle, SDL_GAMEPAD_AXIS_LEFTY));
        pad.analog[2] = normalizeAxis(SDL_GetGamepadAxis(pad.handle, SDL_GAMEPAD_AXIS_RIGHTX));
        pad.analog[3] = normalizeAxis(SDL_GetGamepadAxis(pad.handle, SDL_GAMEPAD_AXIS_RIGHTY));
    }
}

std::vector<PadDevice> SdlInputManager::devices() const {
    std::vector<PadDevice> result;
    for (unsigned i = 0; i < gamepads_.size(); ++i) {
        const auto& p = gamepads_[i];
        if (p.handle) {
            const auto* name = SDL_GetGamepadName(p.handle);
            result.push_back({i, name ? name : "SDL Gamepad", p.key});
        }
    }
    return result;
}

const PadSnapshot* SdlInputManager::pad(unsigned slot) const {
    return slot < gamepads_.size() && gamepads_[slot].handle ? &gamepads_[slot].snapshot : nullptr;
}

std::string_view SdlInputManager::deviceKey(unsigned slot) const {
    return slot < gamepads_.size() ? std::string_view(gamepads_[slot].key) : std::string_view{};
}

int16_t SdlInputManager::state(unsigned port, unsigned device, unsigned index, unsigned id) const {
    if (port >= 4) return 0;

    int16_t result = 0;

    // Merge SDL gamepad state.
    if (device == RETRO_DEVICE_JOYPAD) {
        if (id == RETRO_DEVICE_ID_JOYPAD_MASK) {
            int16_t mask = 0;
            for (int i = 0; i < 16; ++i) {
                mask |= static_cast<int16_t>(gamepads_[port].buttons[i] << i);
            }
            result |= mask;
        } else if (id < 16) {
            result |= gamepads_[port].buttons[id];
        }
    } else if (device == RETRO_DEVICE_ANALOG) {
        if (index == RETRO_DEVICE_INDEX_ANALOG_LEFT) {
            if (id == RETRO_DEVICE_ID_ANALOG_X) result = gamepads_[port].analog[0];
            if (id == RETRO_DEVICE_ID_ANALOG_Y) result = gamepads_[port].analog[1];
        } else if (index == RETRO_DEVICE_INDEX_ANALOG_RIGHT) {
            if (id == RETRO_DEVICE_ID_ANALOG_X) result = gamepads_[port].analog[2];
            if (id == RETRO_DEVICE_ID_ANALOG_Y) result = gamepads_[port].analog[3];
        }
    }

    // Merge keyboard mappings.
    if (keyboardState_) {
        using Btn = n64forge::input::N64Button;
        const auto kbd = keyboardState_(static_cast<int>(port));
        const auto pressed = [kbd](Btn b) { return (kbd.buttons & static_cast<u16>(b)) != 0; };

        if (device == RETRO_DEVICE_JOYPAD) {
            if (id == RETRO_DEVICE_ID_JOYPAD_MASK) {
                int16_t mask = 0;
                if (pressed(Btn::A)) mask |= (1 << 8);
                if (pressed(Btn::B)) mask |= (1 << 0);
                if (pressed(Btn::Start)) mask |= (1 << 3);
                if (pressed(Btn::DUp)) mask |= (1 << 4);
                if (pressed(Btn::DDown)) mask |= (1 << 5);
                if (pressed(Btn::DLeft)) mask |= (1 << 6);
                if (pressed(Btn::DRight)) mask |= (1 << 7);
                if (pressed(Btn::L)) mask |= (1 << 10);
                if (pressed(Btn::R)) mask |= (1 << 11);
                if (pressed(Btn::Z)) mask |= (1 << 12);
                result |= mask;
            } else {
                if (id == 8 && pressed(Btn::A)) result = 1;
                else if (id == 0 && pressed(Btn::B)) result = 1;
                else if (id == 3 && pressed(Btn::Start)) result = 1;
                else if (id == 4 && pressed(Btn::DUp)) result = 1;
                else if (id == 5 && pressed(Btn::DDown)) result = 1;
                else if (id == 6 && pressed(Btn::DLeft)) result = 1;
                else if (id == 7 && pressed(Btn::DRight)) result = 1;
                else if (id == 10 && pressed(Btn::L)) result = 1;
                else if (id == 11 && pressed(Btn::R)) result = 1;
                else if (id == 12 && pressed(Btn::Z)) result = 1;
            }
        } else if (device == RETRO_DEVICE_ANALOG && index == RETRO_DEVICE_INDEX_ANALOG_RIGHT) {
            // Map keyboard C buttons to the right analog stick.
            int16_t x = 0, y = 0;
            if (pressed(Btn::CLeft)) x -= AXIS_MAX;
            if (pressed(Btn::CRight)) x += AXIS_MAX;
            if (pressed(Btn::CUp)) y -= AXIS_MAX;
            if (pressed(Btn::CDown)) y += AXIS_MAX;
            if (id == RETRO_DEVICE_ID_ANALOG_X) result = x;
            if (id == RETRO_DEVICE_ID_ANALOG_Y) result = y;
        }
    }

    return result;
}

} // namespace n64forge::input
