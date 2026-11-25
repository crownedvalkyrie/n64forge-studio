# N64Forge Studio

Hey! I'm Skye, a high schooler who decided to build a Nintendo 64 emulator, static recompiler, and homebrew toolkit from scratch in C++20.

I wanted something that actually feels native on my Mac instead of being a clunky port, and I also thought it would be sick to have a built-in static recompiler (kind of like N64Recompiled) and a `libdragon` studio right inside the same app.

## What's Inside

- **From-Scratch N64 Core (`n64forge_core`):** Custom VR4300 CPU interpreter with native ARM64 and x64 JIT backends, LLE RSP vector core (with NEON fast paths), multi-threaded RDP rasterizer, and VI/AI audio/video timing.
- **Native GPU Presentation:** Metal on macOS, Direct3D 12 and Vulkan on Windows, plus optional Vulkan (MoltenVK / KosmicKrisp) and Software fallback backends. One `Auto` setting probes the native APIs in platform order.
- **Internal Resolution Scaling:** Run games at `1× Native`, `2×`, `3×`, or `4×` while keeping RDRAM readbacks accurate.
- **Cartridge Library & Cover Art:** Boots straight into a 3D/parallax N64 cartridge shelf that automatically pulls box art and logos from `N64ForgeStudio_CoverArt`.
- **Automatic Persistent Saves:** Detects EEPROM (4K/16K), SRAM, FlashRAM, and Controller Paks automatically and saves them cleanly without needing a config database.
- **Static Recompiler (`n64forge analyze` / `recompile`):** Decodes MIPS R4300i binaries, recovers functions and jump tables, and emits standalone C++20 code.
- **Discord Rich Presence:** Shows what game you're playing with cover art.

## Building Locally

### macOS (Apple Silicon)
Make sure you have Xcode Command Line Tools, CMake, Qt 6, and SDL3 installed (`brew install cmake qt sdl3`):

```bash
cmake --preset macos
cmake --build --preset macos-release -j8
ctest --preset macos-release-tests -LE hardware --output-on-failure
open build/macos/Release/n64forge_studio.app
```

### Windows (x64)
Install Visual Studio 2022 (C++ desktop workload), CMake, Qt 6, and SDL3. The
native Vulkan backend needs user-installed Vulkan headers at build time; the
loader and ICD stay user-installed at runtime. Then run:

```powershell
cmake --preset windows-msvc `
  -DN64FORGE_ENABLE_D3D12=ON `
  -DN64FORGE_ENABLE_WIN32_VULKAN=ON `
  -DN64FORGE_WIN32_VULKAN_INCLUDE_DIR:PATH=C:/VulkanSDK/<version>/Include
cmake --build --preset windows-debug
ctest --preset windows-tests
```

## Notes & Disclaimer

- **No ROMs included:** Bring your own legally dumped `.z64`, `.n64`, `.v64`, or `.zip` ROMs.
- **Work in progress:** This is a solo high school passion project! Most big 3D games run great, while cycle-level edge cases and Windows D3D12 polish are still being improved.
- **Credits:** Huge thanks to the N64brew wiki, `libdragon`, and `ares` docs for making hardware research possible.
