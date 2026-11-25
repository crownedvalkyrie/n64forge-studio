#include "n64forge/studio/controller_manager.hpp"
#include "n64forge/input/controller.hpp"
#ifdef N64FORGE_HAS_SDL3
#include "n64forge/input/sdl_input_manager.hpp"
#endif
#include <algorithm>
#include <QCoreApplication>
#include <QSettings>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QStandardPaths>

namespace n64forge::studio {

static const char* settingsGroup = "Input";
static const char* keyMappingsKey = "keyMappings";
static const char* padProfilesKey = "padProfiles";
static const char* padDevicesKey = "padDevices";

ControllerManager::ControllerManager(QObject* parent)
    : QObject(parent)
#ifdef N64FORGE_HAS_SDL3
    , keyboardStateCallback_([this](int player) { return keyboardState(player); })
    , gamepads_(keyboardStateCallback_)
#endif
{
    loadSettings();
}



void ControllerManager::loadSettings() {
    QSettings settings;
    settings.beginGroup(settingsGroup);
    for (auto& bindings : mappings_) bindings.clear();
    assignedDevices_ = {};

    // Load key mappings
    const auto mappingsData = settings.value(keyMappingsKey).toByteArray();
    if (!mappingsData.isEmpty()) {
        const auto doc = QJsonDocument::fromJson(mappingsData);
        if (doc.isArray()) {
            const auto array = doc.array();
            for (const auto& value : array) {
                const auto obj = value.toObject();
                KeyMapping km;
                km.control = static_cast<input::Control>(obj["control"].toInt());
                km.key = obj["key"].toInt();
                km.player = obj["player"].toInt();
                if (km.player >= 0 && km.player < 4 &&
                    static_cast<unsigned>(km.control) < input::ControlCount && km.key > 0) {
                    mappings_[km.player].push_back(km);
                }
            }
        }
    } else {
        for (int i = 0; i < 4; ++i) {
            mappings_[i] = ControllerManager::defaults(i);
        }
    }

    // Load pad profiles
#ifdef N64FORGE_HAS_SDL3
    profiles_.fill(input::PadProfile::defaults());
    const auto profilesData = settings.value(padProfilesKey).toByteArray();
    if (!profilesData.isEmpty()) {
        const auto doc = QJsonDocument::fromJson(profilesData);
        if (doc.isArray()) {
            const auto array = doc.array();
            for (int i = 0; i < 4 && i < array.size(); ++i) {
                const auto obj = array[i].toObject();
                auto profile = input::PadProfile::defaults();
                profile.deadzone = obj["deadzone"].toDouble(0.14);
                profile.sensitivity = obj["sensitivity"].toDouble(1.0);
                profile.device = obj["device"].toString().toStdString();

                const auto bindingsArray = obj["bindings"].toArray();
                for (unsigned j = 0; j < input::ControlCount && j < bindingsArray.size(); ++j) {
                    const auto bObj = bindingsArray[j].toObject();
                    input::PadBinding binding;
                    binding.kind = static_cast<input::PadBinding::Kind>(bObj["kind"].toInt());
                    binding.index = bObj["index"].toInt();
                    profile.bindings[j] = binding;
                }
                profiles_[i] = profile;
            }
        }
    }
#endif

    // Load pad device assignments
    const auto devicesData = settings.value(padDevicesKey).toStringList();
    for (int i = 0; i < 4 && i < devicesData.size(); ++i) {
        assignedDevices_[i] = devicesData[i].toStdString();
    }
    for (int i = 0; i < 4; ++i) {
        const auto group = QStringLiteral("port%1/").arg(i);
        const int type = settings.value(group + "pak", i == 0 ? 1 : 0).toInt();
        pakTypes_[i] = static_cast<PakType>(std::clamp(type, 0, 2));
        intensities_[i] = std::min(100u, settings.value(group + "rumble", 100).toUInt());
    }

    settings.endGroup();
}

void ControllerManager::saveSettings() {
    QSettings settings;
    settings.beginGroup(settingsGroup);
    for (int i = 0; i < 4; ++i) {
        const auto group = QStringLiteral("port%1/").arg(i);
        settings.setValue(group + "pak", static_cast<int>(pakTypes_[i]));
        settings.setValue(group + "rumble", intensities_[i]);
    }

    // Save key mappings
    QJsonArray mappingsArray;
    for (int i = 0; i < 4; ++i) {
        for (const auto& km : mappings_[i]) {
            QJsonObject obj;
            obj["control"] = static_cast<int>(km.control);
            obj["key"] = km.key;
            obj["player"] = km.player;
            mappingsArray.append(obj);
        }
    }
    settings.setValue(keyMappingsKey, QJsonDocument(mappingsArray).toJson());

#ifdef N64FORGE_HAS_SDL3
    // Save pad profiles
    QJsonArray profilesArray;
    for (int i = 0; i < 4; ++i) {
        QJsonObject obj;
        obj["deadzone"] = profiles_[i].deadzone;
        obj["sensitivity"] = profiles_[i].sensitivity;
        obj["device"] = QString::fromStdString(profiles_[i].device);
        QJsonArray bindingsArray;
        for (unsigned j = 0; j < input::ControlCount; ++j) {
            QJsonObject bObj;
            bObj["kind"] = static_cast<int>(profiles_[i].bindings[j].kind);
            bObj["index"] = profiles_[i].bindings[j].index;
            bindingsArray.append(bObj);
        }
        obj["bindings"] = bindingsArray;
        profilesArray.append(obj);
    }
    settings.setValue(padProfilesKey, QJsonDocument(profilesArray).toJson());

    // Save pad device assignments
    QStringList devicesList;
    for (int i = 0; i < 4; ++i) {
        devicesList.append(QString::fromStdString(assignedDevices_[i]));
    }
    settings.setValue(padDevicesKey, devicesList);
#endif

    settings.endGroup();
}

void ControllerManager::bind(int player, input::Control control, int key) {
    if (player < 0 || player >= 4 || key == 0) return;
    unbind(player, control);
    KeyMapping km;
    km.key = key;
    km.control = control;
    km.player = player;
    mappings_[player].push_back(km);
    saveSettings();
}

void ControllerManager::unbind(int player, input::Control control) {
    if (player < 0 || player >= 4) return;
    auto& vec = mappings_[player];
    vec.erase(std::remove_if(vec.begin(), vec.end(),
        [control](const KeyMapping& km) { return km.control == control; }), vec.end());
    saveSettings();
}

void ControllerManager::setMappings(int player, std::vector<KeyMapping> bindings) {
    if (player < 0 || player >= 4) return;
    mappings_[player] = std::move(bindings);
    saveSettings();
}

std::vector<KeyMapping> ControllerManager::defaults(int port) {
    if (port != 0) return {};
    using C = input::Control;
    const std::array<int, input::ControlCount> keys = {
        Qt::Key_X,          // A
        Qt::Key_Z,          // B
        Qt::Key_Q,          // Z
        Qt::Key_Return,     // Start
        Qt::Key_Up,         // DUp
        Qt::Key_Down,       // DDown
        Qt::Key_Left,       // DLeft
        Qt::Key_Right,      // DRight
        Qt::Key_1,          // L
        Qt::Key_3,          // R
        Qt::Key_I,          // CUp
        Qt::Key_K,          // CDown
        Qt::Key_J,          // CLeft
        Qt::Key_L,          // CRight
        Qt::Key_W,          // StickUp
        Qt::Key_S,          // StickDown
        Qt::Key_A,          // StickLeft
        Qt::Key_D           // StickRight
    };
    std::vector<KeyMapping> result;
    for (unsigned i = 0; i < input::ControlCount; ++i) {
        result.push_back(KeyMapping{keys[i], static_cast<C>(i), port});
    }
    result.push_back({Qt::Key_Space, C::A, port});
    result.push_back({Qt::Key_Shift, C::B, port});
    result.push_back({Qt::Key_C, C::Z, port});
    return result;
}

input::ControllerState ControllerManager::keyboardState(int player) const {
    input::ControllerState state;
    if (player < 0 || player >= 4) return state;
    for (const auto& km : mappings_[player]) {
        if (held_.contains(km.key)) {
            if (static_cast<unsigned>(km.control) < input::DigitalControls) {
                state.buttons |= static_cast<u16>(input::ControlButtons[static_cast<unsigned>(km.control)]);
            } else {
                // Handle stick axes
                switch (km.control) {
                    case input::Control::StickUp:    state.stickY = std::max<s8>(state.stickY, input::StickLimit); break;
                    case input::Control::StickDown:  state.stickY = std::min<s8>(state.stickY, -input::StickLimit); break;
                    case input::Control::StickLeft:  state.stickX = std::min<s8>(state.stickX, -input::StickLimit); break;
                    case input::Control::StickRight: state.stickX = std::max<s8>(state.stickX, input::StickLimit); break;
                    default: break;
                }
            }
        }
    }
    return state;
}

bool ControllerManager::handleKeyPress(int key, bool pressed) {
    if (key == 0 || std::none_of(mappings_.begin(), mappings_.end(), [key](const auto& port) {
            return std::any_of(port.begin(), port.end(), [key](auto m) { return m.key == key; });
        })) return false;
    if (pressed) {
        held_.insert(key);
    } else {
        held_.erase(key);
    }
    return true;
}

void ControllerManager::pollGamepads() {
#ifdef N64FORGE_HAS_SDL3
    gamepads_.update();
    const auto devices = gamepads_.devices();
    padPresent_.fill(false);
    padStates_.fill({});
    padSlots_.fill(-1);
    unsigned usedSlots = 0;
    // Explicit assignments first so Auto never steals an assigned slot.
    for (bool automatic : {false, true}) {
        for (int i = 0; i < 4; ++i) {
            const auto& assignment = assignedDevices_[i];
            if (assignment == "-" || assignment.empty() != automatic) continue;
            for (const auto& dev : devices) {
                const unsigned slot = 1u << dev.slot;
                if ((!automatic && assignment != dev.key) || (usedSlots & slot)) continue;
                if (const auto* pad = gamepads_.pad(dev.slot)) {
                    padPresent_[i] = true;
                    padStates_[i] = pad->mapped(profiles_[i]);
                    padSlots_[i] = static_cast<int>(dev.slot);
                    usedSlots |= slot;
                    break;
                }
            }
        }
    }
#endif
}

void ControllerManager::setPakOptions(int port, PakType type, unsigned intensity) {
    if (port < 0 || port >= 4) return;
    pakTypes_[port] = type;
    intensities_[port] = std::min(100u, intensity);
}

void ControllerManager::syncRumble(u8 motorMask) {
    if (rumbleObserver_) rumbleObserver_(motorMask);
#ifdef N64FORGE_HAS_SDL3
    std::array<unsigned, 4> targets{};
    for (int port = 0; port < 4; ++port)
        if ((motorMask & (1u << port)) && padSlots_[port] >= 0)
            targets[padSlots_[port]] = intensities_[port];
    const auto now = std::chrono::steady_clock::now();
    for (unsigned slot = 0; slot < targets.size(); ++slot)
        if (targets[slot] != lastRumble_[slot] || (targets[slot] && now >= rumbleRefresh_))
            gamepads_.rumble(slot, targets[slot]);
    if (now >= rumbleRefresh_) rumbleRefresh_ = now + std::chrono::milliseconds(100);
    lastRumble_ = targets;
#else
    Q_UNUSED(motorMask)
#endif
}

bool ControllerManager::connected(int port) const {
    return port >= 0 && port < 4 && padPresent_[port];
}

input::ControllerState ControllerManager::combinedState(int port) const {
    input::ControllerState state = keyboardState(port);
    if (port >= 0 && port < 4 && padPresent_[port]) {
        state = input::mergeControllers(state, padStates_[port]);
    }
    return state;
}

void ControllerManager::setAssignedDevice(int port, const std::string& key) {
    if (port < 0 || port >= 4) return;
    assignedDevices_[port] = key;
    saveSettings();
}

void ControllerManager::resetAll() {
    syncRumble(0);
    held_.clear();
    for (int i = 0; i < 4; ++i) {
        padPresent_[i] = false;
        padStates_[i] = input::ControllerState{};
    }
}

n64forge::input::ControllerState ControllerManager::state(int player) const {
    return combinedState(player);
}

const std::vector<KeyMapping>& ControllerManager::mappings(int player) const {
    if (player < 0 || player >= 4) return mappings_[0];
    return mappings_[player];
}

QString ControllerManager::keyName(int key) const {
    const char* name = "";
    switch (key) {
        case Qt::Key_Escape: name = "Escape"; break;
        case Qt::Key_Tab: name = "Tab"; break;
        case Qt::Key_Backtab: name = "Backtab"; break;
        case Qt::Key_Backslash: name = "\\"; break;
        case Qt::Key_Asterisk: name = "*"; break;
        case Qt::Key_Plus: name = "+"; break;
        case Qt::Key_Comma: name = ","; break;
        case Qt::Key_Minus: name = "-"; break;
        case Qt::Key_Period: name = "."; break;
        case Qt::Key_Slash: name = "/"; break;
        case Qt::Key_0: name = "0"; break;
        case Qt::Key_1: name = "1"; break;
        case Qt::Key_2: name = "2"; break;
        case Qt::Key_3: name = "3"; break;
        case Qt::Key_4: name = "4"; break;
        case Qt::Key_5: name = "5"; break;
        case Qt::Key_6: name = "6"; break;
        case Qt::Key_7: name = "7"; break;
        case Qt::Key_8: name = "8"; break;
        case Qt::Key_9: name = "9"; break;
        case Qt::Key_A: name = "A"; break;
        case Qt::Key_B: name = "B"; break;
        case Qt::Key_C: name = "C"; break;
        case Qt::Key_D: name = "D"; break;
        case Qt::Key_E: name = "E"; break;
        case Qt::Key_F: name = "F"; break;
        case Qt::Key_G: name = "G"; break;
        case Qt::Key_H: name = "H"; break;
        case Qt::Key_I: name = "I"; break;
        case Qt::Key_J: name = "J"; break;
        case Qt::Key_K: name = "K"; break;
        case Qt::Key_L: name = "L"; break;
        case Qt::Key_M: name = "M"; break;
        case Qt::Key_N: name = "N"; break;
        case Qt::Key_O: name = "O"; break;
        case Qt::Key_P: name = "P"; break;
        case Qt::Key_Q: name = "Q"; break;
        case Qt::Key_R: name = "R"; break;
        case Qt::Key_S: name = "S"; break;
        case Qt::Key_T: name = "T"; break;
        case Qt::Key_U: name = "U"; break;
        case Qt::Key_V: name = "V"; break;
        case Qt::Key_W: name = "W"; break;
        case Qt::Key_X: name = "X"; break;
        case Qt::Key_Y: name = "Y"; break;
        case Qt::Key_Z: name = "Z"; break;
        case Qt::Key_Ampersand: name = "&"; break;
        case Qt::Key_AsciiTilde: name = "~"; break;
        case Qt::Key_BracketLeft: name = "["; break;
        case Qt::Key_BracketRight: name = "]"; break;
        case Qt::Key_Bar: name = "|"; break;
        case Qt::Key_Exclam: name = "!"; break;
        case Qt::Key_ParenLeft: name = "("; break;
        case Qt::Key_ParenRight: name = ")"; break;
        case Qt::Key_Colon: name = ":"; break;
        case Qt::Key_Semicolon: name = ";"; break;
        case Qt::Key_QuoteDbl: name = "\""; break;
        case Qt::Key_QuoteLeft: name = "`"; break;
        case Qt::Key_Space: name = "Space"; break;
        case Qt::Key_Return: name = "Return"; break;
        case Qt::Key_Enter: name = "Enter"; break;
        case Qt::Key_Backspace: name = "Backspace"; break;
        case Qt::Key_Up: name = "Up"; break;
        case Qt::Key_Down: name = "Down"; break;
        case Qt::Key_Left: name = "Left"; break;
        case Qt::Key_Right: name = "Right"; break;
        case Qt::Key_Home: name = "Home"; break;
        case Qt::Key_End: name = "End"; break;
        case Qt::Key_PageUp: name = "PageUp"; break;
        case Qt::Key_PageDown: name = "PageDown"; break;
        case Qt::Key_Insert: name = "Insert"; break;
        case Qt::Key_Delete: name = "Delete"; break;
        case Qt::Key_Pause: name = "Pause"; break;
        case Qt::Key_ScrollLock: name = "ScrollLock"; break;
        case Qt::Key_Print: name = "Print"; break;
        case Qt::Key_Clear: name = "Clear"; break;
        case Qt::Key_VolumeMute: name = "VolumeMute"; break;
        case Qt::Key_VolumeDown: name = "VolumeDown"; break;
        case Qt::Key_VolumeUp: name = "VolumeUp"; break;
        case Qt::Key_MediaPlay: name = "MediaPlay"; break;
        case Qt::Key_MediaStop: name = "MediaStop"; break;
        case Qt::Key_MediaNext: name = "MediaNext"; break;
        case Qt::Key_MediaPrevious: name = "MediaPrevious"; break;
        default: name = ""; break;
    }
    return QCoreApplication::translate("Controller", name);
}

} // namespace n64forge::studio
