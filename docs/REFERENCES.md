# Primary technical references

Use these as the source of truth. Prefer primary vendor and project documentation over blogs.

1. N64brew — Nintendo 64 hardware documentation: https://n64brew.dev/wiki/Main_Page
2. Libretro n64emu core notes — hardware components, timing and test approach: https://docs.libretro.com/library/n64emu/
3. N64 System Test — CPU, memory and RSP test coverage: https://github.com/lemmy-64/n64-systemtest
4. Apple Metal documentation: https://developer.apple.com/metal/
5. Apple MetalFX documentation: https://developer.apple.com/documentation/metalfx
6. Apple Game Porting Toolkit MetalFX frame interpolation guide: https://github.com/apple/game-porting-toolkit/tree/main/game-porting-skills/skills/using-metalfx-frame-interpolation
7. Microsoft Direct3D 12 programming guide: https://learn.microsoft.com/windows/win32/direct3d12/
8. CMake tutorial: https://cmake.org/cmake/help/latest/guide/tutorial/
9. SDL3 documentation and CMake guide: https://wiki.libsdl.org/SDL3/README-cmake
10. Kimi Code MCP documentation: https://www.kimi.com/code/docs/en/kimi-code-cli/customization/mcp

Additional useful references:
- MIPS architecture overview (R4000 family): https://www.cs.cmu.edu/afs/cs/academic/class/15740-f97/public/doc/mips-isa.pdf
- C++ reference: https://en.cppreference.com/
- Model Context Protocol: https://modelcontextprotocol.io/
- Microsoft DirectX Graphics samples: https://github.com/microsoft/DirectX-Graphics-Samples


## OpenCode and N64 homebrew toolkit
11. OpenCode project configuration: https://opencode.ai/docs/config
12. OpenCode MCP servers: https://opencode.ai/docs/mcp-servers
13. libdragon SDK overview: https://libdragon.dev/
14. libdragon API/build documentation: https://libdragon.dev/ref/
15. libdragon source and examples: https://github.com/DragonMinded/libdragon

## Emulation runtime and metadata
16. libretro API: https://github.com/libretro/libretro-common/blob/master/include/libretro.h
17. libretro-database (No-Intro DAT files): https://github.com/libretro/libretro-database
18. N64Forge Studio cover-art repository: https://github.com/crownedvalkyrie/N64ForgeStudio_CoverArt
19. Qt 6 Multimedia (QAudioSink): https://doc.qt.io/qt-6/qaudiosink.html

## libultra HLE and graphics microcode
21. N64brew libultra overview: https://n64brew.dev/wiki/Libultra
22. Standard Microcode (RSP/RDP command reference): https://n64brew.dev/wiki/RSP_Commands
23. N64 Graphics HLE reference (Fast3D/F3DEX): https://n64brew.dev/wiki/F3DZEX
24. N64 memory map and post-IPL3 boot handoff: https://n64brew.dev/wiki/Memory_map and https://n64brew.dev/wiki/Initial_Program_Load
25. PIF RAM, SI DMA and interrupt behavior: https://n64brew.dev/wiki/PIF-NUS and https://n64brew.dev/wiki/Serial_Interface
26. PI DMA and open bus: https://n64brew.dev/wiki/Parallel_Interface
26a. PI/RI register masks, mirroring and status semantics cross-checked against [ares PI io](https://github.com/ares-emulator/ares/blob/6f6786e0/ares/n64/pi/io.cpp) and [ares RI io](https://github.com/ares-emulator/ares/blob/6f6786e0/ares/n64/ri/io.cpp). The ares notice below applies.
27. Joybus commands and VI timing: https://n64brew.dev/wiki/Joybus_Protocol and https://n64brew.dev/wiki/Video_Interface
28. IPL register handoff and CIC seeds: https://github.com/decompals/N64-IPL and https://github.com/Decompollaborate/ipl3checksum/blob/1.x/docs/reference_implementation.md
29. CIC-6105 challenge transform: [ares CIC commands](https://github.com/ares-emulator/ares/blob/master/ares/n64/cic/commands.cpp).
29a. IPL2 checksum constants and algorithm: [N64brew PIF-NUS](https://n64brew.dev/wiki/PIF-NUS), [PifChecksum](https://github.com/jago85/PifChecksum), and cross-checks against [ares IPL2 checksum](https://github.com/ares-emulator/ares/blob/master/mia/medium/nintendo-64.cpp). PifChecksum is MIT-licensed; its notice appears below.

30. [N64brew RSP scalar/vector ISA](https://n64brew.dev/wiki/Reality_Signal_Processor/CPU_Core), [ares SISD RSP](https://github.com/ares-emulator/ares/blob/master/ares/n64/rsp/interpreter-vpu.cpp), and [N64brew DPC interface](https://n64brew.dev/wiki/Reality_Display_Processor/Interface).
31. [N64brew RDP command formats](https://n64brew.dev/wiki/Reality_Display_Processor/Commands) and [AI DMA, DAC rate and delayed address carry](https://n64brew.dev/wiki/Audio_Interface).
32. [MIPS R4000 User's Manual](https://www.cs.cmu.edu/afs/cs/academic/class/15740-f97/public/doc/mips-isa.pdf), unaligned left/right load and store instructions; cross-checked against [ares VR4300 integer instructions](https://github.com/ares-emulator/ares/blob/master/ares/n64/cpu/interpreter-ipu.cpp).
33. [paraLLEl-RDP depth compression](https://github.com/Themaister/parallel-rdp/blob/master/parallel-rdp/shaders/z_encode.h), [depth comparison](https://github.com/Themaister/parallel-rdp/blob/master/parallel-rdp/shaders/depth_test.h), and [perspective divider](https://github.com/Themaister/parallel-rdp/blob/master/parallel-rdp/shaders/perspective.h) as hardware-behavior cross-references.
34. [Qt QSaveFile](https://doc.qt.io/qt-6/qsavefile.html) for temporary-file commit and atomic replacement without direct-write fallback; [SDL3 rumble](https://wiki.libsdl.org/SDL3/SDL_RumbleGamepad) for the 16-bit motor amplitude and stop behavior.
35. [CAMetalLayer drawable size](https://developer.apple.com/documentation/quartzcore/cametallayer/drawablesize) and [Metal linear textures backed by shared buffers](https://developer.apple.com/documentation/metal/mtlbuffer/maketexture(descriptor:offset:bytesperrow:)) for the macOS viewport and packed VI presentation path.
36. [Qt high-DPI coordinate systems](https://doc.qt.io/qt-6/highdpi.html) and [VI X/Y scale and offset register layouts](https://n64brew.dev/wiki/Video_Interface#0x0440_0030_-_VI_X_SCALE).
37. [paraLLEl-RDP upload model](https://github.com/Themaister/parallel-rdp/blob/master/parallel-rdp/rdp_renderer.cpp) cross-reference for `LOADBLOCK` destination stride and DXT.
38. [ares VR4300 system coprocessor instructions](https://github.com/ares-emulator/ares/blob/master/ares/n64/cpu/interpreter-scc.cpp) and [TLB matching](https://github.com/ares-emulator/ares/blob/master/ares/n64/cpu/tlb.cpp), cross-references for MTC0 EntryHi/EPC source width and TLB region matching.
39. [ares VR4300 COP1](https://github.com/ares-emulator/ares/blob/master/ares/n64/cpu/interpreter-fpu.cpp) and the R4000 User’s Manual above, for fixed word rounding and conversion exception behavior.
40. [libdragon source](https://github.com/DragonMinded/libdragon), specifically its
     `gbi.h` display-list macros, as a cross-reference for Fast3D/F3DEX/F3DEX2
     `G_VTX`, `G_TEXTURE`, `G_SETOTHERMODE_L/H`, `G_TRI2` and `G_QUAD` encodings.
41. [paraLLEl-RDP texture sampling](https://github.com/Themaister/parallel-rdp/blob/master/parallel-rdp/shaders/texture.h)
    and [combiner alpha handling](https://github.com/Themaister/parallel-rdp/blob/master/parallel-rdp/shaders/combiner.h)
    for RGBA32 split-bank addressing and geometric-coverage alpha comparison.
42. [Vulkan Loader driver discovery](https://github.com/KhronosGroup/Vulkan-Loader/blob/main/docs/LoaderDriverInterface.md)
     for ICD manifest locations and override variables. Finding a manifest does not
     establish that a driver can present a Studio viewport.
43. [Vulkan Metal surfaces](https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_metal_surface.html),
    [swapchain semaphore reuse](https://docs.vulkan.org/guide/latest/swapchain_semaphore_reuse.html)
    and the [MoltenVK runtime guide](https://github.com/KhronosGroup/MoltenVK/blob/main/Docs/MoltenVK_Runtime_UserGuide.md)
    for the optional macOS presenter.
44. [paraLLEl-RDP triangle setup](https://github.com/Themaister/parallel-rdp/blob/master/parallel-rdp/rdp_device.cpp)
    and [subpixel span setup](https://github.com/Themaister/parallel-rdp/blob/master/parallel-rdp/shaders/span_setup.comp)
    cross-check the RDP command edge anchors, sticky edge quantization,
    `(XH, floor(YH))` attribute latch, and negative-slope `DO_OFFSET`
    correction described in reference 31.
45. [ares VR4300 COP2 transfers](https://github.com/ares-emulator/ares/blob/master/ares/n64/cpu/interpreter-cop2.cpp)
    and the hardware-derived [n64-systemtest address-error checks](https://github.com/thelemmy/nemu64-test/tree/9a8b9f7d94ee2f6f57d7feed70c98c22cdc30e6c/src/tests/address_error_exception)
    cross-check CU2, Cause.CE, and fault-address latches.
46. [Angrylion RDP Plus](https://github.com/ata4/angrylion-rdp-plus) is a
    low-level reference cross-check for one-cycle blender selection and
    `COLOR_ON_CVG` (the active stage's M input), including prewrap suppression,
    and for `tcshift_cycle()`'s signed-16-bit coordinate wrap before the
    tile-coordinate right shift.
47. [N64brew RDP color/coverage modes](https://n64brew.dev/wiki/Reality_Display_Processor/Commands)
    cross-check the `ALPHA_CVG_SEL`, `CVG_X_ALPHA`, `FORCE_BLEND` and coverage
    destination bit layout used by the shared-edge regression.
48. [libultra `gDPSetCycleType`](https://ultra64.ca/files/documentation/online-manuals/man/n64man/gdp/gDPSetCycleType.html)
    and [Angrylion RDP Plus `CYCLE_TYPE_*`](https://github.com/ata4/angrylion-rdp-plus)
    cross-check the cycle-type field: 0 = 1-cycle, 1 = 2-cycle, 2 = copy,
    3 = fill. The one-cycle pipeline executes stage 0 only; the two-cycle
    pipeline executes stage 0 then stage 1.
49. [N64 Programming Manual: texture mapping](https://ultra64.ca/files/documentation/online-manuals/man/pro-man/pro13/index.html),
    [blender](https://ultra64.ca/files/documentation/online-manuals/man/pro-man/pro15/index15.5.html),
    [coverage breakdown](https://offtkp.github.io/RDP/), and
    [n64triangle](https://github.com/sp1187/n64triangle). Angrylion's
    `ADJUST_ATTR_PRIM`, `rgba_correct`, `texture_pipeline_cycle`, and
    `blender_2cycle_cycle0` cross-check the fixed-point shade latch, partial
    coverage sample selection, filter tap addressing, and first blender equation.
50. [Microsoft D3D12 programming guide](https://learn.microsoft.com/windows/win32/direct3d12/),
    [D3D12 resource states](https://learn.microsoft.com/windows/win32/direct3d12/resource-state-transitions),
    and [DirectX Graphics samples](https://github.com/microsoft/DirectX-Graphics-Samples)
    for the Windows viewport's swapchain barriers, descriptor tables and fence use.
51. [Microsoft Win32 surface creation](https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_win32_surface.html)
    and [Vulkan swapchain](https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_swapchain.html)
    references for the runtime-loaded `vulkan-1.dll` presenter. Only Khronos
    headers are used at build time; no loader or ICD is bundled.
52. [Khronos Vulkan-Headers](https://github.com/KhronosGroup/Vulkan-Headers)
    provides the pinned build-time headers used by Windows x64 CI.
53. [Microsoft x64 calling convention](https://learn.microsoft.com/cpp/build/x64-calling-convention)
    and [System V AMD64 ABI](https://gitlab.com/x86-psABIs/x86-64-ABI)
    cross-check the x64 JIT's shadow space, argument registers and callee-saved
    register allocation.

### PifChecksum attribution (IPL2 checksum)

Copyright (c) 2019 jago85

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

### ares attribution (CIC-6105 transform, RSP vector model, PI/RI register model)

Copyright (c) 2004-2025 ares team, Near et al

Permission to use, copy, modify, and/or distribute this software for any
purpose with or without fee is hereby granted, provided that the above
copyright notice and this permission notice appear in all copies.

THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
