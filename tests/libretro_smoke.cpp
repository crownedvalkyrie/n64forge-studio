#include "n64forge/emulation/libretro_emulator.hpp"
#include "n64forge/input/controller.hpp"
#include "n64forge/input/sdl_input_manager.hpp"
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

using n64forge::emulation::LibretroEmulator;
using n64forge::input::ControllerState;
using n64forge::input::SdlInputManager;

int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::cout << "No external libretro core/ROM path supplied; skipping smoke test.\n";
        return 77; // CTest SKIP_RETURN_CODE
    }

    const std::string corePath = argv[1];
    const std::string romPath = argv[2];

    const auto systemDir = std::filesystem::temp_directory_path() / "n64forge_system";
    const auto saveDir = std::filesystem::temp_directory_path() / "n64forge_save";
    std::filesystem::create_directories(systemDir);
    std::filesystem::create_directories(saveDir);

    SdlInputManager input([](int) { return ControllerState{}; });
    if (!input.initialized()) {
        std::cout << "SDL gamepad not initialized; continuing with keyboard stub.\n";
    }

    LibretroEmulator emu;
    emu.setSystemDirectory(systemDir);
    emu.setSaveDirectory(saveDir);
    emu.setCorePath(std::filesystem::absolute(corePath));
    // Core-specific variables can be set here; no specific-core defaults are required.

    emu.setInputPollCallback([&input]() { input.update(); });
    emu.setInputStateCallback([&input](unsigned port, unsigned device, unsigned index, unsigned id) {
        return input.state(port, device, index, id);
    });

    if (!emu.loadCore(corePath)) {
        std::cerr << "loadCore failed: " << emu.lastError() << '\n';
        return 2;
    }

    if (!emu.loadGame(romPath)) {
        std::cerr << "loadGame failed: " << emu.lastError() << '\n';
        return 3;
    }

    const auto& av = emu.avInfo();
    std::cout << "AV info: " << av.width << 'x' << av.height << " @ " << av.fps << " fps\n";

    for (int i = 0; i < 30; ++i) {
        input.update();
        emu.runFrame();
    }

    const auto fb = emu.framebuffer();
    std::cout << "Framebuffer after 30 frames: " << emu.framebufferWidth() << 'x'
              << emu.framebufferHeight() << " (" << fb.size() << " bytes)\n";

    if (emu.framebufferWidth() == 0 || emu.framebufferHeight() == 0) {
        std::cerr << "No framebuffer produced.\n";
        return 4;
    }

    std::cout << "libretro smoke test passed.\n";
    return 0;
}
