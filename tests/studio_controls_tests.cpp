#include "n64forge/studio/controller_manager.hpp"
#include "n64forge/input/controller.hpp"
#include "n64forge/studio/settings.hpp"
#include <QCoreApplication>
#include <QSettings>
#include <iostream>

using namespace n64forge::studio;

void testKeyboardBindings() {
    ControllerManager manager;
    manager.loadSettings();

    // Test default bindings for port 0
    const auto defaults = ControllerManager::defaults(0);
    assert(defaults.size() == static_cast<size_t>(n64forge::input::ControlCount));

    // Test binding a key
    manager.bind(0, n64forge::input::Control::A, Qt::Key_X);
    auto mappings = manager.mappings(0);
    bool found = false;
    for (const auto& km : mappings) {
        if (km.control == n64forge::input::Control::A && km.key == Qt::Key_X) {
            found = true;
            break;
        }
    }
    assert(found);

    // Test unbinding
    manager.unbind(0, n64forge::input::Control::A);
    mappings = manager.mappings(0);
    found = false;
    for (const auto& km : mappings) {
        if (km.control == n64forge::input::Control::A) {
            found = true;
            break;
        }
    }
    assert(!found);

    // Test key name
    QString keyName = manager.keyName(Qt::Key_X);
    assert(!keyName.isEmpty());
}

void testKeyboardState() {
    ControllerManager manager;
    manager.loadSettings();

    // Test that keyboard state is initially empty
    auto state = manager.keyboardState(0);
    assert(state.buttons == 0);
    assert(state.stickX == 0);
    assert(state.stickY == 0);

    // Test that held keys are tracked
    manager.handleKeyPress(Qt::Key_X, true);
    state = manager.keyboardState(0);
    assert(state.buttons != 0);

    manager.handleKeyPress(Qt::Key_X, false);
    state = manager.keyboardState(0);
    assert(state.buttons == 0);
}

void testControllerState() {
    ControllerManager manager;
    manager.loadSettings();

    // Test that controller state is initially empty
    auto state = manager.state(0);
    assert(state.buttons == 0);
    assert(state.stickX == 0);
    assert(state.stickY == 0);

    // Test that controller state includes keyboard input
    manager.handleKeyPress(Qt::Key_X, true);
    state = manager.state(0);
    assert(state.buttons != 0);
}

void testDefaultMappings() {
    const auto defaults = ControllerManager::defaults(0);
    const auto defaults1 = ControllerManager::defaults(1);
    const auto defaults2 = ControllerManager::defaults(2);
    const auto defaults3 = ControllerManager::defaults(3);

    // All ports should have the same number of mappings
    assert(defaults.size() == defaults1.size());
    assert(defaults.size() == defaults2.size());
    assert(defaults.size() == defaults3.size());

    // All ports should have the same total count
    assert(defaults.size() == static_cast<size_t>(n64forge::input::ControlCount));
}

void testResetAll() {
    ControllerManager manager;
    manager.loadSettings();

    // Bind some keys
    manager.bind(0, n64forge::input::Control::A, Qt::Key_X);
    manager.bind(0, n64forge::input::Control::B, Qt::Key_Z);

    // Reset all
    manager.resetAll();

    // Verify bindings are cleared
    auto mappings = manager.mappings(0);
    bool hasBindings = false;
    for (const auto& km : mappings) {
        if (km.key != 0) {
            hasBindings = true;
            break;
        }
    }
    assert(!hasBindings);
}

void testPersistence() {
    QSettings settings;
    settings.clear();

    ControllerManager manager;
    manager.bind(0, n64forge::input::Control::A, Qt::Key_X);
    manager.bind(0, n64forge::input::Control::B, Qt::Key_Z);
    manager.saveSettings();

    ControllerManager manager2;
    manager2.loadSettings();

    auto mappings = manager2.mappings(0);
    bool foundA = false;
    bool foundB = false;
    for (const auto& km : mappings) {
        if (km.control == n64forge::input::Control::A && km.key == Qt::Key_X) {
            foundA = true;
        }
        if (km.control == n64forge::input::Control::B && km.key == Qt::Key_Z) {
            foundB = true;
        }
    }

    assert(foundA);
    assert(foundB);
}

#ifdef N64FORGE_HAS_SDL3
void testGamepadDetection() {
    ControllerManager manager;
    manager.loadSettings();

    // Test that gamepads can be polled
    manager.pollGamepads();

    // Test that connected status works
    bool hasGamepad = manager.connected(0);
    // This will be false if no gamepad is connected, which is expected
    assert(!hasGamepad);
}
#endif

#ifndef N64FORGE_STUDIO_CONTROLS_TESTS
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);

    testKeyboardBindings();
    testKeyboardState();
    testControllerState();
    testDefaultMappings();
    testResetAll();
    testPersistence();

#ifdef N64FORGE_HAS_SDL3
    testGamepadDetection();
#endif
}
#else
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);

    testKeyboardBindings();
    testKeyboardState();
    testControllerState();
    testDefaultMappings();
    testResetAll();
    testPersistence();

#ifdef N64FORGE_HAS_SDL3
    testGamepadDetection();
#endif

    std::cout << "All Studio Controls tests passed!\n";
    return 0;
}
#endif
