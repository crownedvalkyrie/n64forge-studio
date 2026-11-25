#pragma once
#include "n64forge/input/controller.hpp"
#include "n64forge/core/controller_pak.hpp"
#ifdef N64FORGE_HAS_SDL3
#include "n64forge/input/sdl_input_manager.hpp"
#endif
#include <QObject>
#include <QSettings>
#include <QString>
#include <array>
#include <functional>
#include <unordered_set>
#include <vector>
#include <string>

namespace n64forge::studio {

class StudioSettings;

struct KeyMapping {
    int key = 0;
    input::Control control;
    int player = 0;
    bool operator==(const KeyMapping&) const = default;
};

class ControllerManager : public QObject {
    Q_OBJECT
public:
    explicit ControllerManager(QObject* parent = nullptr);

    void loadSettings();
    void saveSettings();

    void setDefaultMappings();
    void clearMappings(int player = -1);
    void bind(int player, input::Control control, int key);
    void unbind(int player, input::Control control);
    void setMappings(int player, std::vector<KeyMapping> bindings);

    bool handleKeyPress(int key, bool pressed);
    void resetAll();

    [[nodiscard]] n64forge::input::ControllerState state(int player = 0) const;
    [[nodiscard]] n64forge::input::ControllerState keyboardState(int player) const;
    [[nodiscard]] const std::vector<KeyMapping>& mappings(int player = 0) const;
    [[nodiscard]] QString keyName(int key) const;
    static std::vector<KeyMapping> defaults(int port);
    void pollGamepads();
    [[nodiscard]] bool connected(int port) const;
    [[nodiscard]] input::ControllerState combinedState(int port) const;

    // Device assignment
    [[nodiscard]] const std::string& assignedDevice(int port) const { return assignedDevices_[port]; }
    void setAssignedDevice(int port, const std::string& key);
    [[nodiscard]] PakType pakType(int port) const { return pakTypes_[port]; }
    [[nodiscard]] unsigned rumbleIntensity(int port) const { return intensities_[port]; }
    void setPakOptions(int port, PakType type, unsigned intensity);
    void syncRumble(u8 motorMask);
    // Diagnostic hook invoked with every motor mask before SDL dispatch. Used
    // by the Studio lifecycle test to verify vibration stops on pause/reset/close.
    void setRumbleObserver(std::function<void(u8)> observer) { rumbleObserver_ = std::move(observer); }

#ifdef N64FORGE_HAS_SDL3
    [[nodiscard]] input::SdlInputManager& gamepads() { return gamepads_; }
    [[nodiscard]] const input::PadProfile& profile(int port) const { return profiles_[port]; }
    void setProfile(int port, input::PadProfile profile) { profiles_[port] = std::move(profile); }
#endif

private:
    std::unordered_set<int> held_;
    std::array<std::vector<KeyMapping>, 4> mappings_;
    std::array<input::ControllerState, 4> padStates_{};
    std::array<bool, 4> padPresent_{};
    std::array<std::string, 4> assignedDevices_{};
    std::array<PakType, 4> pakTypes_{PakType::Controller, PakType::None, PakType::None, PakType::None};
    std::array<unsigned, 4> intensities_{100, 100, 100, 100};
    std::function<void(u8)> rumbleObserver_;
#ifdef N64FORGE_HAS_SDL3
    input::KeyboardStateCallback keyboardStateCallback_;
    input::SdlInputManager gamepads_;
    std::array<input::PadProfile, 4> profiles_;
    std::array<int, 4> padSlots_{-1, -1, -1, -1};
    std::array<unsigned, 4> lastRumble_{};
    std::chrono::steady_clock::time_point rumbleRefresh_{};
#endif
};

} // namespace n64forge::studio
