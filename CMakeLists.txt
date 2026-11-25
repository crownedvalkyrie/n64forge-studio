cmake_minimum_required(VERSION 3.24)
project(N64Forge VERSION 0.2.0 LANGUAGES CXX)

if(NOT WIN32 AND NOT APPLE)
    message(FATAL_ERROR "N64Forge supports Windows and macOS only.")
endif()

option(N64FORGE_BUILD_TESTS "Build unit tests" ON)
option(N64FORGE_ENABLE_HARDWARE_TESTS "Register the opt-in external ROM corpus test" OFF)
option(N64FORGE_BUILD_STUDIO "Build the Qt 6 Studio GUI shell when available" ON)
option(N64FORGE_ENABLE_D3D12 "Build the Windows Direct3D 12 backend" ON)
option(N64FORGE_ENABLE_WIN32_VULKAN "Build the Windows Vulkan backend" ON)
option(N64FORGE_ENABLE_METAL "Build the macOS Metal backend" ON)
option(N64FORGE_FORCE_SCALAR_COP2 "Force portable scalar COP2 paths on ARM64 (for NEON equivalence testing)" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(APPLE AND NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "13.0" CACHE STRING "Minimum macOS version" FORCE)
endif()

if(APPLE)
    # Xcode's Release settings apply to core libraries and their consumers.
    set(CMAKE_XCODE_ATTRIBUTE_GCC_OPTIMIZATION_LEVEL[variant=Release] 3)
    set(CMAKE_XCODE_ATTRIBUTE_LLVM_LTO[variant=Release] YES_THIN)
endif()

find_package(SDL3 QUIET CONFIG)
if(SDL3_FOUND)
    message(STATUS "SDL3 found: ${SDL3_VERSION}")
else()
    message(STATUS "SDL3 not found; gamepad support will be disabled.")
endif()

add_library(n64forge_core
    src/core/memory.cpp
    src/core/bus.cpp
    src/core/cartridge.cpp
    src/core/r4300_decode.cpp
    src/core/cpu.cpp
    src/core/jit_arm64.cpp
    src/core/jit_x64.cpp
    src/core/rsp.cpp
    src/core/rsp_vector.cpp
    src/core/ostask.cpp
    src/core/rsp_hle.cpp
    src/core/controller.cpp
    src/core/controller_pak.cpp
    src/core/eeprom.cpp
    src/core/flash_ram.cpp
    src/core/sram.cpp
    src/core/emulator.cpp
    src/core/hw_registers.cpp
    src/core/ipl.cpp
    src/core/is_viewer.cpp
    src/core/hle_libultra.cpp
    src/core/rdp.cpp
    src/emulation/libretro_emulator.cpp
    src/emulation/frame_ring_buffer.cpp
)
target_include_directories(n64forge_core PUBLIC include third_party/libretro)
target_compile_features(n64forge_core PUBLIC cxx_std_20)
# ZIP containers use the platform zlib. It is present in the macOS SDK; a
# Windows build without zlib still compiles all other ROM formats.
find_package(ZLIB QUIET)
if(ZLIB_FOUND)
    target_compile_definitions(n64forge_core PRIVATE N64FORGE_HAS_ZLIB=1)
    target_link_libraries(n64forge_core PRIVATE ZLIB::ZLIB)
endif()
if(SDL3_FOUND)
    target_sources(n64forge_core PRIVATE src/input/sdl_input_manager.cpp)
    target_compile_definitions(n64forge_core PUBLIC N64FORGE_HAS_SDL3=1)
    target_link_libraries(n64forge_core PUBLIC SDL3::SDL3)
endif()
if(N64FORGE_FORCE_SCALAR_COP2)
    target_compile_definitions(n64forge_core PRIVATE N64FORGE_FORCE_SCALAR_COP2=1)
endif()

if(MSVC)
    target_compile_options(n64forge_core PRIVATE /W4 /permissive- /EHsc)
else()
    target_compile_options(n64forge_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

set(RENDERER_SOURCES
    src/video/graphics_api.cpp
    src/video/renderer_factory.cpp
    src/video/interpolation_math.cpp
    src/video/frame_interpolation.cpp
    src/video/vi_interpolation.cpp
    src/video/viewport_presenter.cpp
)
if(WIN32 AND N64FORGE_ENABLE_D3D12)
    list(APPEND RENDERER_SOURCES
        src/video/d3d12/d3d12_renderer.cpp
        src/video/d3d12/d3d12_frame_interpolator.cpp
        src/video/d3d12/d3d12_viewport.cpp
    )
elseif(APPLE AND N64FORGE_ENABLE_METAL)
    enable_language(OBJCXX)
    list(APPEND RENDERER_SOURCES
        src/video/metal/metal_renderer.mm
        src/video/metal/metal_frame_interpolator.mm
        src/video/metal/metal_viewport.mm
    )
    set_source_files_properties(src/video/metal/metal_renderer.mm PROPERTIES COMPILE_FLAGS "-fobjc-arc")
    set_source_files_properties(src/video/metal/metal_frame_interpolator.mm PROPERTIES COMPILE_FLAGS "-fobjc-arc")
    set_source_files_properties(src/video/metal/metal_viewport.mm PROPERTIES COMPILE_FLAGS "-fobjc-arc")
endif()

if(WIN32 AND N64FORGE_ENABLE_WIN32_VULKAN)
    # The loader is opened at runtime; only the platform headers are used at
    # build time. No Vulkan SDK, loader or ICD is vendored or downloaded.
    find_path(N64FORGE_WIN32_VULKAN_INCLUDE_DIR vulkan/vulkan.h
        HINTS "$ENV{VULKAN_SDK}/include" "$ENV{VK_SDK_ROOT}/Include")
    if(NOT N64FORGE_WIN32_VULKAN_INCLUDE_DIR OR
       NOT EXISTS "${N64FORGE_WIN32_VULKAN_INCLUDE_DIR}/vulkan/vulkan_win32.h")
        message(FATAL_ERROR
            "N64FORGE_ENABLE_WIN32_VULKAN requires Vulkan headers (vulkan/vulkan.h and "
            "vulkan/vulkan_win32.h). Install the Vulkan SDK headers or configure with "
            "-DN64FORGE_ENABLE_WIN32_VULKAN=OFF.")
    endif()
    list(APPEND RENDERER_SOURCES src/video/vulkan/win32_vulkan_viewport.cpp)
endif()

if(APPLE)
    # Only headers are needed at build time. Drivers and the loader are
    # user-installed and opened by the Cocoa adapter at runtime.
    find_path(N64FORGE_VULKAN_INCLUDE_DIR vulkan/vulkan.h
        HINTS "$ENV{VULKAN_SDK}/include" /opt/homebrew/include /opt/local/include)
    if(EXISTS "${N64FORGE_VULKAN_INCLUDE_DIR}/vulkan/vulkan_core.h")
        file(STRINGS "${N64FORGE_VULKAN_INCLUDE_DIR}/vulkan/vulkan_core.h" N64FORGE_PORTABILITY_HEADER
            REGEX "^#define VK_KHR_portability_enumeration 1$")
    endif()
    if(N64FORGE_PORTABILITY_HEADER)
        enable_language(OBJCXX)
        list(APPEND RENDERER_SOURCES src/video/vulkan/vulkan_viewport.mm)
        set_source_files_properties(src/video/vulkan/vulkan_viewport.mm
            PROPERTIES COMPILE_FLAGS "-fobjc-arc")
    endif()
endif()

add_library(n64forge_video ${RENDERER_SOURCES})
target_include_directories(n64forge_video PUBLIC include)
target_link_libraries(n64forge_video PUBLIC n64forge_core)

if(APPLE AND N64FORGE_PORTABILITY_HEADER)
    target_compile_definitions(n64forge_video PRIVATE N64FORGE_HAS_VULKAN_VIEWPORT=1)
    target_include_directories(n64forge_video PRIVATE "${N64FORGE_VULKAN_INCLUDE_DIR}")
    target_link_libraries(n64forge_video PRIVATE "-framework AppKit" "-framework QuartzCore")
endif()

if(WIN32 AND N64FORGE_ENABLE_D3D12)
    target_compile_definitions(n64forge_video PRIVATE N64FORGE_HAS_D3D12=1)
    target_link_libraries(n64forge_video PRIVATE d3d12 d3dcompiler dxgi dxguid)
endif()
if(WIN32 AND N64FORGE_ENABLE_WIN32_VULKAN)
    target_compile_definitions(n64forge_video PRIVATE N64FORGE_HAS_WIN32_VULKAN=1)
    target_include_directories(n64forge_video PRIVATE "${N64FORGE_WIN32_VULKAN_INCLUDE_DIR}")
endif()
if(APPLE AND N64FORGE_ENABLE_METAL)
    target_compile_definitions(n64forge_video PRIVATE N64FORGE_HAS_METAL=1)
    target_link_libraries(n64forge_video PRIVATE
        "-framework Foundation" "-framework Metal" "-framework QuartzCore" "-framework AppKit")
endif()

# Static recompiler front-end (Track A): analyzer, CFG, jump tables, signatures, emitter, runtime.
add_library(n64forge_recomp
    src/recomp/analyzer.cpp
    src/recomp/analyze_cli.cpp
    src/recomp/emitter.cpp
    src/recomp/recomp_rt.cpp
)
target_include_directories(n64forge_recomp PUBLIC include)
target_compile_definitions(n64forge_recomp PRIVATE N64FORGE_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(n64forge_recomp PUBLIC n64forge_core)
if(MSVC)
    target_compile_options(n64forge_recomp PRIVATE /W4 /permissive- /EHsc)
else()
    target_compile_options(n64forge_recomp PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(n64forge src/main.cpp src/hardware_report.cpp)
target_link_libraries(n64forge PRIVATE n64forge_core n64forge_video n64forge_recomp)

if(N64FORGE_BUILD_TESTS)
    enable_testing()
    add_executable(n64forge_tests
        tests/core_tests.cpp tests/recomp_tests.cpp
        tests/graphics_api_tests.cpp
        tests/core_safety_tests.cpp tests/rdp_safety_tests.cpp
        tests/discord_tests.cpp src/studio/discord_rpc.cpp
    )
    if(MSVC)
        target_compile_options(n64forge_tests PRIVATE /UNDEBUG)
    else()
        target_compile_options(n64forge_tests PRIVATE -UNDEBUG)
    endif()
    target_link_libraries(n64forge_tests PRIVATE n64forge_core n64forge_recomp n64forge_video)
    if(ZLIB_FOUND)
        target_compile_definitions(n64forge_tests PRIVATE N64FORGE_HAS_ZLIB=1)
        target_include_directories(n64forge_tests PRIVATE ${ZLIB_INCLUDE_DIRS})
        target_link_libraries(n64forge_tests PRIVATE ZLIB::ZLIB)
    endif()
    add_test(NAME n64forge_core_tests COMMAND n64forge_tests)
    if(APPLE)
        find_package(Python3 QUIET COMPONENTS Interpreter)
        if(Python3_Interpreter_FOUND)
            add_test(NAME n64forge_recomp_native
                COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/recomp_native_smoke.py"
                    $<TARGET_FILE:n64forge> "${CMAKE_COMMAND}" "${CMAKE_GENERATOR}")
            set_tests_properties(n64forge_recomp_native PROPERTIES TIMEOUT 180)
        endif()
    endif()

    # COP2 golden equivalence: the normal core uses NEON on Apple Silicon, while
    # n64forge_core_scalar always uses the portable scalar paths. Both run the
    # identical instruction stream and must match embedded golden snapshots.
    add_library(n64forge_core_scalar
        src/core/memory.cpp
        src/core/bus.cpp
        src/core/cartridge.cpp
        src/core/r4300_decode.cpp
        src/core/cpu.cpp
        src/core/jit_arm64.cpp
        src/core/jit_x64.cpp
        src/core/rsp.cpp
        src/core/rsp_vector.cpp
        src/core/ostask.cpp
        src/core/rsp_hle.cpp
        src/core/controller.cpp
        src/core/controller_pak.cpp
        src/core/eeprom.cpp
        src/core/flash_ram.cpp
        src/core/sram.cpp
        src/core/emulator.cpp
        src/core/hw_registers.cpp
        src/core/is_viewer.cpp
        src/core/hle_libultra.cpp
        src/core/rdp.cpp
        src/emulation/libretro_emulator.cpp
        src/emulation/frame_ring_buffer.cpp
    )
    target_include_directories(n64forge_core_scalar PUBLIC include third_party/libretro)
    target_compile_features(n64forge_core_scalar PUBLIC cxx_std_20)
    target_compile_definitions(n64forge_core_scalar PRIVATE N64FORGE_FORCE_SCALAR_COP2=1)
    if(SDL3_FOUND)
        target_sources(n64forge_core_scalar PRIVATE src/input/sdl_input_manager.cpp)
        target_compile_definitions(n64forge_core_scalar PUBLIC N64FORGE_HAS_SDL3=1)
        target_link_libraries(n64forge_core_scalar PUBLIC SDL3::SDL3)
    endif()

    add_executable(n64forge_cop2_golden tests/rsp_cop2_golden.cpp)
    target_link_libraries(n64forge_cop2_golden PRIVATE n64forge_core)
    add_test(NAME n64forge_cop2_golden COMMAND n64forge_cop2_golden)

    add_executable(n64forge_cop2_golden_scalar tests/rsp_cop2_golden.cpp)
    target_link_libraries(n64forge_cop2_golden_scalar PRIVATE n64forge_core_scalar)
    add_test(NAME n64forge_cop2_golden_scalar COMMAND n64forge_cop2_golden_scalar)

    add_executable(n64forge_cop2_golden_emit tests/rsp_cop2_golden.cpp)
    target_compile_definitions(n64forge_cop2_golden_emit PRIVATE N64FORGE_EMIT_COP2_GOLDEN=1)
    target_link_libraries(n64forge_cop2_golden_emit PRIVATE n64forge_core_scalar)
    if(N64FORGE_ENABLE_HARDWARE_TESTS)
        find_package(Python3 REQUIRED COMPONENTS Interpreter)
        add_test(NAME n64forge_hardware_corpus
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/scripts/run_hardware_tests.py"
                --cli $<TARGET_FILE:n64forge>
                --output "${CMAKE_BINARY_DIR}/hardware-report/$<CONFIG>"
                --strict)
        set_tests_properties(n64forge_hardware_corpus PROPERTIES LABELS hardware TIMEOUT 3600)
    endif()

    if(SDL3_FOUND)
        add_executable(n64forge_libretro_smoke tests/libretro_smoke.cpp)
        target_link_libraries(n64forge_libretro_smoke PRIVATE n64forge_core)
        add_test(NAME n64forge_libretro_smoke COMMAND n64forge_libretro_smoke)
        set_tests_properties(n64forge_libretro_smoke PROPERTIES
            SKIP_RETURN_CODE 77
            ENVIRONMENT "DYLD_LIBRARY_PATH=/opt/homebrew/opt/sdl3/lib")
    endif()
endif()

if(APPLE)
    set_target_properties(n64forge PROPERTIES MACOSX_BUNDLE TRUE)
endif()

# Optional desktop Studio shell. The emulator core and CLI remain buildable
# without Qt 6; the GUI is enabled automatically when Qt 6 Widgets is found.
if(N64FORGE_BUILD_STUDIO)
    find_package(Qt6 QUIET COMPONENTS Widgets)
    if(Qt6_FOUND)
        find_package(Qt6 QUIET COMPONENTS Network)
        find_package(Qt6 QUIET COMPONENTS Multimedia)
        set(CMAKE_AUTOMOC ON)
        set(CMAKE_AUTORCC ON)
        set(CMAKE_AUTOUIC ON)
        add_library(n64forge_studio_viewport STATIC
            src/studio/emu_widget.cpp
            src/studio/settings.cpp
            src/studio/audio_output.cpp
            src/studio/controller_manager.cpp
            src/studio/game_library.cpp
            src/studio/library_widget.cpp
            src/studio/credits_widget.cpp
            src/studio/style.cpp
            include/n64forge/studio/emu_widget.hpp
            include/n64forge/studio/audio_output.hpp
            include/n64forge/studio/controller_manager.hpp
            include/n64forge/studio/game_library.hpp
            include/n64forge/studio/library_widget.hpp
            include/n64forge/studio/credits_widget.hpp
        )
        qt_add_resources(n64forge_studio_viewport "credits_data"
            PREFIX "/credits"
            BASE "resources/credits"
            FILES
                resources/credits/credits.json
                resources/credits/avatars/skye.jpg
                resources/credits/avatars/community.png
                resources/credits/avatars/contributor.png
                resources/credits/avatars/supporters.png
                resources/credits/avatars/tester.png
        )
        target_link_libraries(n64forge_studio_viewport PUBLIC Qt6::Widgets n64forge_video)
        if(APPLE)
            target_link_libraries(n64forge_studio_viewport PRIVATE "-framework AudioToolbox")
        endif()
        set(N64FORGE_APP_ICON "${CMAKE_CURRENT_SOURCE_DIR}/assets/n64forge.icns")
        set_source_files_properties(${N64FORGE_APP_ICON} PROPERTIES MACOSX_PACKAGE_LOCATION "Resources")
        add_executable(n64forge_studio
            ${N64FORGE_APP_ICON}
            assets/splash.qrc
            src/studio/main.cpp
            src/studio/controls_dialog.cpp
            src/studio/discord_rpc.cpp
            src/studio/libdragon_sdk.cpp
            src/studio/project_wizard.cpp
            src/studio/splash_dialog.cpp
            src/studio/build_runner.cpp
            include/n64forge/studio/settings.hpp
            include/n64forge/studio/style.hpp
            include/n64forge/studio/controls_dialog.hpp
            include/n64forge/studio/discord_rpc.hpp
            include/n64forge/studio/libdragon_sdk.hpp
            include/n64forge/studio/project_wizard.hpp
            include/n64forge/studio/splash_dialog.hpp
            include/n64forge/studio/build_runner.hpp
        )
        target_link_libraries(n64forge_studio PRIVATE n64forge_studio_viewport)
        if(Qt6Network_FOUND)
            target_sources(n64forge_studio_viewport PRIVATE
                src/studio/game_database.cpp include/n64forge/studio/game_database.hpp)
            target_link_libraries(n64forge_studio_viewport PUBLIC Qt6::Network)
            target_compile_definitions(n64forge_studio_viewport PUBLIC N64FORGE_HAS_QT_NETWORK=1)
        endif()
        if(Qt6Multimedia_FOUND)
            target_link_libraries(n64forge_studio_viewport PUBLIC Qt6::Multimedia)
            target_compile_definitions(n64forge_studio_viewport PUBLIC N64FORGE_HAS_QT_MULTIMEDIA=1)
        endif()
        if(TARGET n64forge_tests)
            target_sources(n64forge_tests PRIVATE tests/studio_video_tests.cpp tests/frame_ring_tests.cpp)
            target_link_libraries(n64forge_tests PRIVATE n64forge_studio_viewport)
            add_executable(n64forge_studio_command_tests
                tests/studio_command_tests.cpp
                src/studio/build_runner.cpp
                src/studio/project_wizard.cpp
                src/studio/libdragon_sdk.cpp
                include/n64forge/studio/build_runner.hpp
                include/n64forge/studio/project_wizard.hpp
            )
            set_target_properties(n64forge_studio_command_tests PROPERTIES AUTOMOC ON)
            target_link_libraries(n64forge_studio_command_tests PRIVATE Qt6::Widgets n64forge_core)
            add_test(NAME n64forge_studio_command COMMAND n64forge_studio_command_tests)
            set_tests_properties(n64forge_studio_command PROPERTIES
                ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 30)
            add_library(studio_controls_tests STATIC tests/studio_controls_tests.cpp)
            target_link_libraries(studio_controls_tests PRIVATE n64forge_studio_viewport)
            target_compile_definitions(studio_controls_tests PRIVATE N64FORGE_STUDIO_CONTROLS_TESTS=1)
            target_include_directories(studio_controls_tests PUBLIC include/n64forge/studio include/n64forge/input)
            target_compile_definitions(n64forge_tests PRIVATE N64FORGE_TEST_STUDIO_VIDEO=1)
            add_test(NAME n64forge_studio_video COMMAND n64forge_tests --studio-video)
            set_tests_properties(n64forge_studio_video PROPERTIES
                ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 45)
        endif()
        target_include_directories(n64forge_studio PRIVATE include)
        target_compile_definitions(n64forge_studio PRIVATE
            N64FORGE_STUDIO_VERSION="${PROJECT_VERSION}")
        if(APPLE)
            set_target_properties(n64forge_studio PROPERTIES MACOSX_BUNDLE TRUE)
        elseif(WIN32)
            set_target_properties(n64forge_studio PROPERTIES WIN32_EXECUTABLE TRUE)
        endif()
    else()
        message(STATUS "Qt 6 Widgets not found; n64forge_studio will not be built.")
    endif()
endif()

if(TARGET n64forge_studio)
    set_target_properties(n64forge_studio PROPERTIES MACOSX_BUNDLE_ICON_FILE "n64forge.icns")
endif()
