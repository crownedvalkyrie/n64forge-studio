# Development roadmap

N64Forge Studio is transitioning from a libretro-core runtime to its own native `n64forge_core` hardware model. The generic libretro loader remains available for optional external-core comparison, but no core is bundled. Native ROM execution uses the in-house R4300i, RSP and RDP, without dispatching OSTasks to HLE.

## Milestone 0 — Build foundation
- [x] C++20 and CMake
- [x] Windows/MSVC and macOS/Apple Clang presets
- [x] Platform-specific renderer source selection
- [x] Basic memory unit test
- [x] CI on Windows and macOS (workflow added; macOS verified locally)

> **Current status (macOS):** Qt 6.12.0 and SDL3 installed via Homebrew. The `macos` preset
> configures and builds `n64forge`, `n64forge_core`, `n64forge_video`,
> `n64forge_tests`, `n64forge_libretro_smoke`, and the Qt 6 `n64forge_studio` GUI shell
> successfully with Apple Clang 21 / Xcode 27.1 in both Debug and Release. All
> non-hardware CTest tests pass in both configurations. No libretro core is
> bundled; `n64forge_libretro_smoke` skips cleanly when no external core is supplied.
> The Studio Emulator tab runs the native `n64forge::Emulator` (RDRAM +
> R4300i CPU + RSP + software RDP reference renderer) and displays the
> VI scanout from guest RDRAM. The libretro loader remains only for optional external
> comparison. Automatic save detection, ZIP/single-ROM loading, the opt-in ARM64
> JIT and the static recompiler indirect-dispatch fallback now have deterministic
> tests; full IPL execution, cycle-accurate scheduling and sustained 3× real-time
> playback remain incomplete.
> Windows has not been tested on this host. A GitHub Actions workflow for both
> platforms has been added but not yet run.

## Phase 1 — Studio polish: thumbnails and Discord Rich Presence
- [x] Configurable GitHub artwork repository with box-art/title fallback and title, logo, and screenshot categories.
- [x] Asynchronous PNG validation and atomic downloads into per-repository caches, with an opt-in bulk art pack download.
- [x] Persistent cover cache keyed by canonical title and ROM fallback filename
- [x] Discord RPC preserves session start timestamp across pause/resume/reset updates; verified cover URLs passed as `large_image`
- [x] Build and tests pass on macOS

Studio uses `https://github.com/crownedvalkyrie/N64ForgeStudio_CoverArt.git`
by default. Change it in **Settings → Discord & Paths → Artwork repository…**
or launch the Studio executable with `--art-repo-url https://github.com/OWNER/REPO.git`
for a session-only override. Repositories use the `main` branch and four
folders: `Named_Boxarts`, `Named_Titles`, `Named_Logos`, and `Named_Snaps`.
The cache lives under `~/Library/Caches/n64forge/covers` on macOS and
`~/.cache/n64forge/covers` on Windows. **Download Artwork Pack…** shallow-clones
into this cache using installed Git; it does not overwrite an existing pack.
The launcher prefers title art for the splash; Discord receives the verified
raw box-art URL, then the title URL, or the local app's Discord logo key.

## Phase 2 — OSTask tooling and retired HLE dispatch
- [x] OSTask parsing and the experimental display-list interpreter remain isolated in tests/tooling. The emulator no longer dispatches them, injects synthetic DP tasks, or synthesizes task-completion signals.
- [x] Diagnostic counters observe the DMEM task type at SP unhalt and count actual RSP `BREAK` instructions with SIG2 set. They are not used to control execution.

### Boot and I/O conformance
- [x] Conventional post-IPL3 payload copied from ROM into RDRAM at the header entry point; no per-game address overrides.
- [x] PIF RAM mapping and mirroring, bidirectional SI DMA, and SI interrupt acknowledgement through MI; tested through CPU COP0 interrupt delivery.
- [x] Standard controller on Joybus port 0: Info/Reset, neutral input, no-Pak CRC responses, and no-response flags for disconnected devices. SI writes configure transactions; SI DMA reads execute them.
- [x] CIC variants (6101, 6102/7101, 7102, 6103/7103, 6105/7105, 6106/7106, 5101) are derived from the IPL2 checksum authenticated against published PIF secrets, never from game identity. The model applies each variant's documented CIC id, seed/return register, entry-point adjustment and CIC-6105 challenge flag. Custom/nonstandard IPL3 payloads use a generic handoff fallback, never a
      per-ROM override. Full IPL1/IPL2/IPL3 execution and the CIC challenge
      exchange remain out of scope.
- [x] PI/RI register behavior: 24-bit/even PI addresses, count-minus-one DMA lengths, per-domain BSD latches, PI busy/error/interrupt status, and PI/RI register-block mirroring are covered by deterministic tests. PI `ioBusy` and real DMA latency remain unmodeled.
- [x] VI register timing from nominal CPU cycles and regional video clocks, including MI delivery and acknowledgement.
- [x] R4300i branch-likely delay-slot annulment (including REGIMM forms), COP1
      word conversions (`ROUND`/`TRUNC`/`CEIL`/`FLOOR`/`CVT.W.S/D`) with
      rounding mode, inexact and range tests, and the COP0 Count/Compare
      interrupt. Other COP1 flag and rounding paths are not fully modeled.
- [x] VR4300 big-endian word and doubleword left/right memory merges. Each instruction accesses only its aligned word or doubleword and stores preserve unselected bytes; tests cover all address offsets and packed records that cross a word boundary.
- [ ] Full IPL execution/verification and PIF reset timeout, accurate CPU/DMA scheduling, and VI filtering. The handoff still skips boot firmware; the CPU interpreter charges one nominal cycle per step. Save-device access is explicit and deterministic, but its command timing and DMA latency are approximations.
- [ ] External renderer integration. Keep ultrawide output, game-specific projection enhancements, and presentation-only frame interpolation separate from native emulation timing.

See the native graphics milestone below for boot-probe results.

## Milestone 1 — libretro runtime integration (optional / external comparison)
- [x] libretro API wrapper (`third_party/libretro/libretro.h`)
- [x] Dynamic core loading, environment callbacks, variables and directory handling
- [x] SDL3 gamepad + Qt keyboard controller input
- [x] Software-rendered framebuffer presentation in the Studio Emulator tab
- [~] "Run in Emulator" action routed to native core; libretro path retained for external comparison
- [x] Audio routing from core to host output (Qt Multimedia, optional at configure time)
- [x] Presentation-only frame interpolation and nearest/smooth/integer scaling options
- [x] Frame rate upscaling / duplication to 60, 120 and 180 fps (presentation only)
- [x] Ultrawide (21:9) aspect-ratio option and integer-scaling rendering fix
- [x] In-game splash/cover art download and display for the running ROM
- [x] Game metadata lookup (libretro-database DAT) and automatic box-art download/cache
- [ ] Windows libretro core path discovery and CI testing

## Milestone 2 — Hardware model (long-term reference / tooling)
- [x] Cartridge header parsing and byte-order detection (`.z64`/`.v64`/`.n64`),
      arbitrary ROM lengths up to the mapped PI window, open bus past EOF, and
      optional single-ROM `.zip` containers using the platform zlib. ZIP64,
      encrypted, multi-ROM and unsupported-method archives are rejected without
      guessing an image. The core still builds when zlib is absent.
- [x] Physical memory map and bus access rules (RDRAM, SP DMEM/IMEM, PIF RAM, cartridge ROM)
- [x] MI, VI, AI, PI, SI, RI, SP and DP register blocks (`HwRegisters`)
- [x] Interrupt controller (MI mask/pending aggregation) and PI DMA tick engine
- [x] R4300i CPU interpreter (basic integer ALU, branches, load/store, delay slots)
- [x] Exceptions, COP0 (MFC0/MTC0, ERET, TLBWI/TLBR/TLBP) and TLB translation
- [x] 64-bit integer ops (DADD/DADDU/DSUB/DSUBU/DSLL/DSRL/DSRA and 32-bit variants)
- [x] COP1 floating-point (MFC1/MTC1/DMFC1/DMTC1, ADD.S/ADD.D, BC1F/BC1T, CFC1/CTC1, compare/convert)
- [x] Deterministic CPU/TLB/COP0/COP1 unit tests

## Phase 1 — Native RDP Command Processor + Deterministic Reference Renderer
- [x] RDP command FIFO parser (`Rdp::process`)
- [x] Set_Color_Image, Set_Scissor, Set_Fill_Color, Fill_Rectangle, Sync_Full, Shade_Triangle
- [x] Fixed-point edge-walker triangle rasterizer with half-open pixel intervals
- [x] Internal RGBA8888 framebuffer and scissor clipping
- [x] Native `n64forge::Emulator` owns Bus/Cpu/Rsp/Rdp/HwRegisters and `runFrame()` drives RDP processing
- [x] Studio `EmuWidget` displays VI scanout via `Emulator::framebuffer()`
- [x] RSP COP0 submission to DPC, including RDRAM and DMEM/XBUS sources, split commands, and `SYNC_FULL` -> MI interrupt.

## Phase 2 — Native CPU: 64-bit / COP0 / COP1 / TLB
- [x] 64-bit integer ops (DADD/DADDU/DSUB/DSUBU/DSLL/DSRL/DSRA, 32-bit variants, DSLL/DM etc.)
- [x] COP0 (MFC0/DMFC0, MTC0/DMTC0, ERET, TLBWI/TLBR/TLBP, COP1 enable)
- [x] COP1 (MFC1/DMFC1, MTC1/DMTC1, ADD.S/ADD.D, BC1F/BC1T, CFC1/CTC1, compare/convert)
- [x] TLB page translation with EntryHi/EntryLo/Index/PageMask and ASID
- [x] Deterministic CPU/TLB/COP0/COP1 unit tests (`test_cpu_64_cop_tlb`)
- [x] TLB-miss / invalid / modified exceptions routed through the Coprocessor exception state machine (no host crash; `test_cpu_tlb_exception_routing`)

## Phase 3 — RSP vector unit + task engine
- [x] Scalar interpreter with wrapped IMEM PC and unaligned, wrapped DMEM accesses.
- [x] COP2 interpreter implements eight lanes, broadcast, flags, 48-bit accumulator, integer divider lookup, and vector memory instructions including LTV/STV.
- [x] SP and DP COP0 MMIO; `BREAK` updates HALT/BROKE and raises MI SP if enabled. Only guest software writes the signal bits.
- [ ] Hardware conformance and timing: validate the full ISA on hardware vectors; add RSP issue timing, asynchronous SP DMA and busy/full behavior, and DPC double buffering/counters. The current scheduler steps RSP at most once per nominal CPU cycle and SP DMA is synchronous.

## Milestone 2A — Static recompiler (Track A, tooling / analysis)
- [x] MIPS decoder with canonical opcode tables and reserved-instruction detection
- [x] Function discovery, control-flow graph, branch-likely and delay-slot handling
- [x] Jump-table resolution and function signature hashing. Unresolved static
      targets are warnings; dynamic `JR`/`JALR` and jump-table defaults route
      through `dispatchIndirect(state, target_pc)` after KSEG0/KSEG1
      canonicalization. Analyzer output no longer aborts on an unknown target.
- [x] Big-endian recompiler runtime (`RecompContext`) with LWL/LWR/SWL/SWR helpers
- [x] C++20 emitter lowering MIPS functions to compilable translation units
- [x] `n64forge recompile <rom>` CLI command
- [x] Studio Toolkit buttons: **Analyze ROM**, **Recompile to C++**, **Build Native Executable**
- [~] libultra HLE shims (threads, message queues, timers, DMA, gSPMatrix/gSPViewport hooks)
- [x] End-to-end execution for a supported synthetic cartridge on macOS. The
      generated executable links the in-repository runtime and consumes the
      cartridge at launch; arbitrary MMIO-dependent ROMs are not supported.

## Milestone 3 — RSP and graphics
- [x] Run guest-uploaded audio, JPEG and graphics microcode through the same RSP interpreter.
- [x] DPC command DMA and `SYNC_FULL`, RGBA16/RGBA32 fill output to RDRAM, RGBA16 copy-cycle texture rectangles, and `SETTIMG` / `SETTILE` / `LOADTILE` / `LOADBLOCK` for the supported format.
- [x] Native triangle edge/attribute decoding, quarter-scanline spans and eight coverage samples. The software path uses TMEM point or RDP three-point filtering, RGBA/CI/IA/I texels and RGBA/IA palettes, one-/two-cycle combining, alpha compare, coverage and blending, and compressed Z/DZ comparisons and writes in RDRAM. Tests drive the FIFO and verify actual framebuffer/depth values, including near/far rejection, CI4 palette banks, filtering and alpha compare.
- [x] `LOADBLOCK` applies the programmed destination line stride when DXT
      advances T, in addition to odd-row bank swapping. A perspective triangle
      exercises this with RGBA16 and 16-bit-aliased CI4/CI8 uploads and two-cycle
      environment modulation.
- [x] Native RGBA32 `LOADTILE`/TEX_RECT split-bank addressing and geometric
       coverage alpha comparison in one- and two-cycle modes have pixel-level
       regressions. This does not resolve all visible seams or decals.
- [x] Triangle XH/XM now start at `floor(YH)`, while XL starts at exact `YM`.
      Shaded, textured and depth attributes latch at `(XH, floor(YH))`, with
      the reference `do_offset` rule for a negative-slope left-major edge.
      Regressions check the shared edge, fractional gradient and offset latch.
      The sampled OoT title scene has not been visually re-verified after this
      change; no claim of a seam-free commercial scene is made.
- [x] The blender follows the documented cycle-type encoding: type 0 runs only
      stage 0, type 1 runs stage 0 followed by stage 1, and `COLOR_ON_CVG`
      substitutes the active stage's M input without an extra equation. This
      removes black single-sample holes on shared edges when
      `ALPHA_CVG_SEL`/`CVG_X_ALPHA` are active and blending is not forced.
      Adjacent-triangle regressions check both sides of a shared diagonal,
      explicit `COLOR_ON_CVG` substitution/prewrap suppression, and a distinct
      stage-0/stage-1 MUX case for both cycle types. An earlier edit inverted
      the cycle-type predicate and changed real ROM captures; the current
      regression would fail on that variant.
- [x] `SETTILE` S/T shifts wrap through a signed 16-bit coordinate before the
      later right shift, matching the reference tile-coordinate path for
      shifts 11 through 15. The left shift uses an unsigned intermediate. A
      deterministic regression uses hand-derived coordinates that cross the
      wrap and checks every texel for `0x0E` and `0x0F` textured triangles
      across all 256 S/T shift pairs. Non-quarter-aligned tile bounds and
      `LOADTILE` source coordinates are not covered; a final gameplay capture
      of Hyrule Field has not been produced, so the flat tan ground observation
      is not fully re-verified.
- [x] The triangle latch uses independent `DE` and `DY` coefficients and
       partial coverage now shifts SHADE to the first covered sample. The
       three-point filter clamps its base before selecting neighboring taps,
       and the first two-cycle blend equation runs even when the final mix
       is bypassed. Raw FIFO tests check the lighting and texture gradients
       independently in the same primitive, including alpha compare with
       the level-zero `LOD_FRAC` combiner input.
- [~] The RDP is a reference model, not a bit-exact implementation. Remaining
       gaps include subpixel scissor and Z sample correction, exact shade
       overflow and blender division, perspective reciprocal precision,
       derivative-based texture LOD and YUV/keying, dither and exact noise, cycle-to-cycle and
       neighbor-pixel timing, unusual TMEM loads, point-sampled fractional tile
       bounds, `LOADTILE` source addressing, coherent hidden bits for writes
       outside the RDP, and visual confirmation of commercial-scene seams.
       Color image support is RGBA16/RGBA32.
- [x] Unfiltered VI snapshot of RGBA5551/RGBA8888 from guest RDRAM. VI retrace
      continues while blanked. The active window honors the whole-pixel X
      offset and ignores the unused high bits of the fractional Y offset.
      Fractional resampling, field weaving, analog filtering and mid-field
      updates remain unmodeled.
- [ ] Verify continuous 3D gameplay and live window output for both commercial probes.

### RSP HLE texturing reference path

- [x] The HLE display-list interpreter forwards `SETTIMG`, `SETTILE`, `LOADBLOCK`,
      `LOADTILE`, `LOADTLUT`, `SETTILESIZE`, `SETCOMBINE` and `SETOTHERMODE` into the
      RDP state machine before submitting triangles. It keeps `G_TEXTURE` tile,
      level and signed 16.16 scales, scales raw `S10.5` `G_VTX` coordinates before
      perspective-correct coefficient fitting, and emits complete shaded/textured/Z
      coefficient blocks. Legacy `G_SETOTHERMODE_L/H` and F3DEX2 `G_SETOTHERMODE_L/H`
      use their distinct encodings, and F3DEX2 `G_QUAD` emits both triangles.
- [x] Deterministic tests submit real F3DEX2 display lists through
      `RspHle::runGraphicsTask()`, assert textured RDP triangle submission, and check
      that multiple decoded texels reach the framebuffer. Separate regressions cover
      both SETOTHERMODE encodings and F3DEX2 `G_QUAD` decoding.
- [~] Native ROM execution still uses the real RSP interpreter, not `RspHle`. Native
      OoT and MK64 probes already reported textured triangles and produced VI
      snapshots with non-uniform pixel variance; the HLE changes are attributable
      only to the HLE/tooling path. Paper Mario and Majora's Mask local probes did
      not reach textured output within the sampled window, so their 3D output is
      unverified. No ROM identity, CRC or game-specific PC is used to select behavior.
- [ ] Fast3D-specific `G_VTX` nibble encoding is not selected automatically because
      legacy display lists alone cannot reliably distinguish Fast3D from F3DEX.

### macOS native boot regression

Run `cmake --preset macos`, `cmake --build --preset macos-debug`, and
`ctest --preset macos-tests`. The libretro smoke test skips unless an external
core is supplied. With Qt available, CTest also boots a generated ROM through
the actual Studio viewport in offscreen mode and inspects the painted RGBA
image. Windows was not tested on this host;
use `cmake --preset windows-msvc`, `cmake --build --preset windows-debug`,
and `ctest --preset windows-tests` from a Visual Studio 2022 environment.

To repeat the live macOS Studio viewport probe with a legally obtained local
ROM, run `build/macos/Release/n64forge_tests --studio-video "$ROM" [minimum-rdp-full-syncs]`.
This opens an actual Qt widget and checks painted output with **Fit Window**
and interpolation off, then fixed 2×/4× viewport modes with 60/120 FPS
interpolation selected. Earlier Release probes observed 9,209 native non-black
pixels after 20 RDP full syncs for `mk64.z64`, 3,416 after 30 syncs for
`oot.z64`, and 33,273 CPU-written pixels for `CPULW.N64` with no RDP syncs.
These are frame-delivery checks, not frame-pacing or continuous-gameplay claims.

Headless command for each legally obtained local ROM:
`build/macos/Debug/n64forge.app/Contents/MacOS/n64forge "$ROM" --steps 100 --frames "$BATCHES"`.
`--frames` here counts diagnostic batches of 100,000 CPU steps, not VI fields.

| ROM | Batches | Native execution observed | VI result |
| --- | ---: | --- | --- |
| `mk64.z64` | 1,600 | 73 audio and 17 graphics `BREAK`s with SIG2 set; 17 RDP full syncs; 4,400 of 8,941 triangle commands wrote at least one pixel | `0x36AA00`, 9,282 non-black pixels in the final snapshot |
| `oot.z64` | 1,600 | One type-4 and 73 type-1 `BREAK`s with SIG2 set; 73 RDP full syncs; 2,806 of 2,853 triangle commands wrote at least one pixel | `0x3DAA80`, 3,400 non-black pixels in the final snapshot |
| `CPULW.N64` | 160 | CPU reaches `0x80008DF0` with no OSTask | `0x100000`, 33,273 non-black pixels from guest CPU writes |

The OoT trace showed Count/Compare and SI interrupts arriving. The controller
thread acquired the pad-data lock and took the invalid-response path after an
unaligned store pair corrupted the poll packet. Fixing VR4300 merge semantics
restored the packet and let both threads progress; no guest or ROM-specific behavior
was added. Both commercial probes produced changing non-black VI snapshots.
These probes do not establish pixel accuracy, continuous gameplay, audio
playback, RSP arithmetic conformance, or a test-ROM pass verdict. After the
x64 JIT wiring, the same headless progression check was repeated with native
JIT enabled: MK64 reached 84 VI fields with 17 RDP full syncs and 13,886
non-black pixels, OoT reached 100 fields with 73 syncs and 3,741 non-black
pixels, and `CPULW.N64` reached `0x80008DF0` with 33,273 CPU-written pixels.
Full numbers and the Rosetta x64 result are in `docs/TESTING.md`.

## Milestone 4 — Native graphics backends
- [~] D3D12 backend on Windows: native swapchain, shader unpacking/filtering,
      triple-buffered uploads, resource-state barriers and attach-time fallback
      are implemented. Windows x64 CI compiles and links it; no Windows host
      was available locally for a visual attach/present check.
- [x] Metal backend on macOS: Retina-aware 4:3 Fit Window and fixed 1×/2×/4×
       viewports, shared packed VI textures on unified-memory GPUs, shader
      filtering and presentation-only 60/120 Hz blending, GPU command timing,
      and three drawables with bounded off-main-thread acquisition. QPainter
      fallback is retained.
      See [performance notes](PERFORMANCE.md#sustained-metal-viewport-benchmark-macos-release).
- [ ] Shared renderer resource and command abstractions
- [ ] Shader translation / microcode strategy documented
- [ ] Pixel comparison tests against reference renderer
- [~] Optional Cocoa Vulkan swapchain presenter. Build with recent Vulkan
      headers; no Vulkan library is linked. The adapter can open MoltenVK
      directly or use the Vulkan loader with a selected ICD manifest. It
      uploads native VI pixels and performs scaling/letterboxing on Vulkan.
      MoltenVK was tested locally; KosmicKrisp presentation is not verified.
      Incompatible or absent drivers use Software.
- [~] Native Windows Vulkan swapchain presenter. It opens `vulkan-1.dll` at
      runtime, uses only Khronos headers at build time, uploads native VI pixels
      and blits/scales them into a `VK_KHR_win32_surface` swapchain. Windows x64
      CI compiles it with pinned Vulkan headers; no Windows host was available
      locally for an attach/present check.
- [x] One persisted Graphics API setting with appended `Auto`, `D3D12` and
      `Vulkan` values. Windows defaults to `Auto` and probes D3D12 → Vulkan →
      Software; Apple keeps its historical Metal default and probes
      Metal → MoltenVK → KosmicKrisp → Software. Explicit hardware choices fall
      back only to Software, and attach failures fall through only within the
      `Auto` chain. Deterministic resolver tests cover every combination.

## Milestone 5 — Accuracy and compatibility
- [x] IS-Viewer 64 debug cartridge at PI `0x13FF0000`, including libultra and
      libdragon flush conventions and the CLI `--isviewer` output flag. Local
      OoT and MK64 probes produced no guest log lines.
- [x] Opt-in external CPU, COP0/COP1, TLB, RSP, RDP and DMA diagnostic corpus and artifact reporting. See [testing workflow](TESTING.md); strict corpus remains failed because `nemu64-test` has no completion banner.
- [x] VR4300 CPU-side COP2 transfer latch and CU2/RI precedence. Unaligned
      scalar loads/stores and instruction fetches raise AdEL/AdES before
      touching memory and update BadVAddr, Context and XContext. The local
      system test still reports failures in other CPU behaviors.
- [ ] Save states with versioned serialization
- [x] Controller Pak and Rumble Pak on all four ports with Joybus checksums,
      initialized 32 KiB `.mpk` files, per-port accessory selection, persistent
      0–100% rumble intensity, and vibration stopped on pause/reset/close.
- [x] 1 Mbit FlashRAM with status/silicon ID, page program, sector/chip erase,
      busy timing and PI DMA; 4/16 Kbit Serial EEPROM on Joybus channel 4; and
      32 KiB battery-backed SRAM at PI domain `0x08000000`. The active SRAM vs
      FlashRAM device is selected explicitly, never inferred from ROM identity.
      CRC-named `.fla`, `.eep` and `.sra` files are written atomically.
- [x] Studio **Emulator → Cartridge Save Media** selects Auto, None, FlashRAM
      (128 KiB), SRAM (32 KiB), EEPROM 4 Kbit or EEPROM 16 Kbit. Auto reuses a
      single matching, readable save and does not create a new one. EEPROM
      4K/16K is selected from Joybus channel-4 traffic with safe expansion to
      16K when a guest accesses a high block; SRAM/FlashRAM from real PI-domain
      commands and writes; Controller Paks stay independent. Only the detected
      device is created and persisted, atomically, with its status shown in
      Diagnostics. Ambiguous cartridge images disable all cartridge devices
      without touching existing files. Use an explicit medium for a new game;
      settings take effect on the next load or reset. Wrong-size and unreadable
      existing files are preserved.
- [x] Two-buffer AI DMA, region-clock DAC rate, IRQ on buffer start and 48 kHz
      host sample-and-hold through SDL3 (CoreAudio fallback). Host underrun/idle
      decay and audio hardware timing are not cycle accurate.
- [ ] Per-game compatibility tracking

## Milestone 6 — Optional graphics enhancements
- [x] Release profiling, direct VR4300 RDRAM accesses, selected ARM64 RSP SIMD
      operations, and predecoded RDP combiner/blender state. The NEON/scalar
      regression includes all 43 original snapshots and a mixed-arithmetic
      stream covering all 16 element selections. See [performance notes](PERFORMANCE.md).
- [x] Resume decoded CPU blocks between device ticks, validating each instruction's RDRAM line. Verified boot video in the Studio viewport and cache coherency after writes and SP DMA.
- [x] Batch event-free CPU prefixes up to VI/AI deadlines, with precise COP0 Compare dispatch and scalar fallback for busy devices and CPU barriers. Compare the Release per-field results with `n64forge_tests --compare-event-batching "$ROM" 180`.
- [x] Cache RSP scalar and COP2 decode per IMEM word; invalidate on CPU writes and SP DMA, including bank wrap.
- [x] Chain cached static CPU branches within an RDRAM page when the device budget and delay slot allow; validate the destination code line after CPU writes or DMA.
- [x] Count repeated taken self-branches with a NOP delay slot in pairs
      within the existing device/Compare horizon. Tests compare retired
      instructions and COP0 against scalar execution and replace the slot
      to verify RDRAM write invalidation. No ROM address selects the shortcut.
- [x] KUSEG TLB lookup outside ERL, with ASID-aware mappings and a 4 KiB RDRAM fast path for aligned accesses. Mapped cache entries retain offsets rather than raw pointers; remaps and COP0 changes invalidate them, and writes still notify the per-line generation observer. KSEG0/KSEG1 retain the direct path.
- [x] COP0 EntryHi retains the region and the extended VPN on word-form
      writes. The mapped RDRAM data cache covers canonical upper-kernel
      aliases and is cleared by `TLBWI`/`TLBWR`. Instruction fetch from these
      addresses translates on each fetch; decoded blocks are physical. An
      opt-in ARM64 native JIT handles quiet RDRAM prefixes on Apple Silicon;
      other hosts fall back to cached execution.
- [x] Opt-in ARM64 native JIT for quiet direct RDRAM blocks on Apple Silicon:
      indexed dispatch, static chaining, native `JR $ra` returns, pinned GPR
      self-loops, COP1 loads/stores/transfers and S/D arithmetic, 64-bit shifts,
      aligned RDRAM loads/stores, and line-scoped coherency. GPR values are
      flushed before C++ fallbacks, chain exits and epilogues. RSP NEON handles
      only aligned `LQV`/`SQV` with zero offset/element and falls back otherwise.
      Timing and interrupt checks remain in the guest scheduler; `arm64e`
      falls back to cached execution.
- [~] Native x64 JIT with the same contract as the ARM64 backend: host-owned
      budget, exact continuation PC on bails, generation-checked invalidation,
      static branch chaining and interpreter fallback for unsupported code.
      Windows x64 is the default CPU mode; Apple x86_64/Rosetta uses the same
      path. The full parity suite passes under Rosetta; Windows x64 CI asserts
      that the backend is available and compiles/runs the same tests. No
      Windows host was available locally for performance or visual verification.
- [x] Deterministic JIT parity tests cover tight `BEQ`/`BNE` and `BC1T`/`BC1F`
      loops, budget exits, pinned GPR loops, COP1 compare delay slots, 64-bit
      operations, nested `JAL`/`JR $ra`, and self-modifying-code invalidation.
      Hot block PCs are diagnostics only, never codegen conditions.
- [ ] Make the ARM64 JIT the default after broader conformance and sustained
      performance testing. Expand native coverage and reduce RSP/RDP cost
      before claiming real-time 3× gameplay. Final 8-second 3×/120 Hz Metal
      windows still fell below a 100% rolling minimum on at least one ROM;
      see [performance notes](PERFORMANCE.md#final-3-metal-probe-2026-11-25).
- [ ] Asynchronous RCP task execution. The current device scheduler owns
      RSP/RDP side effects and interrupt ordering; no asynchronous toggle is
      exposed. Safe overlap needs guest-time fences for RDRAM/DMEM/DPC access,
      DMA, VI scanout and interrupt observation before work can move to a host thread.
- [~] Optimize the RDP without changing pixel order. Proven interior pixels
      bypass eight edge comparisons; the rasterizer caches line coefficients
      and uses ARM64 NEON for shade interpolation/clamping. Studio and CLI
      default to auto scanline workers for larger non-aliasing primitives;
      small primitives stay inline, and aliased depth/color ranges stay serial.
      RDRAM stores remain immediate, while coherency callbacks are coalesced
      per touched 64-byte line and flushed before guest execution. Sustained
      real-time gameplay remains open.
- [ ] Fit Window / 120 Hz at or above 100% guest speed for both commercial
      probes. Repeated Release windows still vary around the target; the final
      3×/120 Hz samples are in
      [performance notes](PERFORMANCE.md#final-3-metal-probe-2026-11-25).
      No guest logic or audio frame was duplicated to improve them.
- [x] Studio guest worker and host-only VI motion interpolation with Off/60/120 targets; bounded SDL3/CoreAudio stereo FIFO with drift correction. Metal blend selection is covered by deterministic tests and a Cocoa GPU probe; the guest timestamp is never advanced for a blended frame. Device playback was probed on macOS; other hosts are untested. Display-vsync locking remains open.
- [x] 4:3 viewport Auto-Fit and fixed viewport sizes. Preallocated frame slots deliver native VI pixels to Qt without a full-frame copy on the worker.
- [x] Opt-in Studio performance HUD and diagnostics panel plus headless `--stats`; rolling rates, sampled phase timings, and host audio FIFO counters. See [metric definitions](PERFORMANCE.md#live-diagnostics).
- [ ] True higher-resolution RDP rasterization with native guest readback and VI-coherent selection.
- [ ] Texture filtering and widescreen options, explicitly marked as hacks
- [x] Metal sub-frame interpolation prototype: triple-buffered FrameRingBuffer
      (emulation thread producer spins/yields, Metal consumer blocks on condition
      variable), `DrawCallSnapshot` + `InterpolateMvp` (simd quaternion slerp via
      `simd_slerp`, lerp via `simd_mix`, translation/scale lerp, L-infinity
      translation + quaternion angle camera-cut detection with tunable thresholds,
      2D UI bypass) and CADisplayLink render loop with timestamp-derived alpha
      (alpha = (hostTime - frameOrigin) / sourcePeriod)
- [ ] MetalFX spatial/temporal scaling where supported
- [ ] Optional MetalFX frame interpolation on supported macOS versions
- [ ] Optional Windows frame-generation integration, behind a capability interface
- [ ] Frame pacing, latency controls, UI/HUD ghosting mitigation
- [ ] Sustain real time across the title/viewport matrix. Monotonic pacing
      retains fractional cycle time and catches up short overruns, but cannot
      recover time the host lacks the throughput to execute.

## Non-negotiable accuracy rules
- Never change emulated CPU/VI timing to fake presentation FPS.
- Keep emulated frame rate, host presentation rate and generated-frame rate separate.
- Never assume all N64 games render at one resolution or use one microcode.
- Treat enhancements as opt-in per-game settings.
- Preserve a deterministic mode for tests and debugging.


## N64Forge Studio product roadmap

### Studio shell
- [x] Optional Qt 6 desktop shell scaffold (builds and runs)
- [x] Emulator and Toolkit workspace tabs
- [x] Persistent preferences and recent projects/ROMs
- [x] Native app packaging and generated Valkyrie app/window icon (macOS bundle produced)
- [x] Keyboard and SDL gamepad controller configuration, including per-port
      Controller/Rumble Pak selection and persisted rumble intensity
- [x] Metal viewport presenter on macOS with persistent previous/current VI
      textures and presentation-only blending; 4:3 Fit Window, fixed 1×/2×/4×
      scaling, nearest/bilinear filtering and 60/120 Hz targets
- [x] Studio publishes keyboard and SDL3 controller state to the native SI ports; tests cover keybinding/profile persistence and stick limits.
- [x] Toggleable boot telemetry overlay with a 1.2 s minimum sweep and host-only fade after active VI output; headless and test invocations bypass it.
- [x] "Run in Emulator" action loads and runs project or selected ROMs
- [x] Full emulation lifecycle controls (Pause/Resume, Reset, Stop, Full Screen, Core selection)
- [x] Toolbar with quick-access actions, search/filter bar in Library, and active core indicator
- [x] Smart focus and tab-switch auto-pause/resume handling
- Frame rate upscaling (60/120/180 fps), ultrawide aspect mode, integer-scaling fix
- [x] Frame interpolation rewritten as temporal interpolation (fractional-frame blending between consecutive source frames); display pipeline hardened with frame dirty-flag and clamped frame ratios; live FPS readout added to the Emulator view
- [x] Audio output buffer hardening (bounds-safe underrun handling, whole-frame drop alignment)
- [x] In-game splash/cover art for the running ROM via local cache
- [x] Native audio output path: SDL3 `SDL_OpenAudioDeviceStream` with CoreAudio fallback. AI stereo DMA samples are converted to 48 kHz host stereo and drained through a bounded nonblocking FIFO with underrun telemetry.
- [x] Recent ROMs library view with card/list layouts and search filtering
- [x] Atomic Smoke hardware deck: beveled cartridge cards, transport controls,
      live diagnostics styling, and a cartridge inspector backed by header
      region and IPL2 CIC detection. The one-click video baseline leaves save
      media alone; Expansion Pak and save requirements are not inferred.
- [x] Optional Library parallax: cursor-driven cartridge layers and a
      crossfading artwork banner. The appearance switch persists and stops
      both animations immediately when turned off.
- [x] Library-first launch, shared transport and menu actions, and scrollable
      Settings tabs. Fit Window stays 4:3 even if fixed-size ultrawide is enabled.
- [x] Native single-row header: Library · Emulator · Toolkit · Settings on the
      left, Open ROM / Pause / Reset / Stop in the center, Full Screen on the
      right. Studio opens on the Library and Stop returns there. The tab bar is
      hidden so there is only one navigation surface, and no bottom duplicate
      transport remains.
- [x] Settings is the only settings surface. It has five spaced, scrollable
      tabs and one named control for each setting; CPU/save/video menu toggles,
      the Discord popover, and the duplicate toolbar video strip were removed.
      Rumble intensity now lives in Settings → Audio & Controls rather than the
      controller dialog. Discord and artwork repository paths live in Settings →
      Discord & Paths rather than the View menu.
- [x] Dark platform palette plus stylesheet: off-white `#F1F5F9` primary text on
      charcoal surfaces, slate `#CBD5E1` subtitles, explicit focus/hover/press
      states. A unit check enforces a WCAG-AAA 7:1 contrast ratio for primary
      text, so macOS never falls back to black text on a grey window.
- [x] Credits is a separate read-only workspace tab (not a Settings control):
      `Devs`, `Supporters` and `Testers` pages loaded from
      `resources/credits/credits.json` (bundled as `:/credits/credits.json`),
      with a 44×44 circular PFP, role pill, contribution line and a
      deterministic initials/accent fallback when an image is missing. The
      on-screen collapsible "How to Add People & PFPs" guide and
      `docs/CREDITS_GUIDE.md` document the entry format.
- [~] Async RCP and Expansion Pak switches are deliberately absent: the
      reference model exposes RDP worker concurrency but no independent async
      RCP mode, and 4/8 MiB memory is selected from the authenticated CIC
      profile. No inert or misleading controls were added for them.
- [x] Internal resolution 1×–4× refines opaque, fully covered native triangle
      interiors in a host sidecar while guest RDRAM, color/depth buffers and
      readback stay native. Recent color buffers keep independent sidecars, and
      VI sub-rectangle origins are handled, so double-buffered ROMs present
      `640×474` at 2× and `1280×948` at 4× from a 320×237 VI window. Mixed,
      CPU-written or unrefined pixels fall back to native per pixel. This is
      not independent high-resolution geometry, depth or universal supersampling.
- [x] Settings → Graphics offers one persisted selector: Auto on Windows with
      Direct3D 12 and Vulkan entries, and the historical Metal/MoltenVK/
      KosmicKrisp entries on Apple, plus Software on both. The active presenter
      label reports the actual GPU presenter even while another workspace tab is
      frontmost; Metal/Vulkan/D3D12 backends expose their API and asynchronous
      completion state so the label no longer falls back to `Software` while a
      GPU frame is in flight. Unavailable drivers still fall back to Software.
- [x] Per-ROM cover art (manual image selection + automatic N64Forge artwork download)
- [x] Discord Rich Presence integration (manual Client ID)
- [x] Animated splash intro: embedded `n64musplashlogo.gif` (QMovie) with staggered "N64" letter pop-in, glow, gold ring, tagline and progress bar; drawn with QPainter
- [x] macOS (Apple Silicon, Xcode/SDK 27.0, Qt 6.12) build fixes: AppKit framework for Metal frame interpolator, valid NSScreen display-link target, std::string conversions at Discord presence call sites
- [x] Robust ROM switching: full libretro core teardown + reload on ROM load when a game was previously loaded (fixes failing loads when switching ROMs)
- [x] Dynamic N64 box art: libretro CDN cover resolution with No-Intro title sanitization, ROM hash identification, async fetch + CRC32-keyed local cache, splash/cover display, and Discord RPC `large_image` wiring

### N64 Toolkit
- [x] Starter homebrew project template placeholder
- [x] Project wizard and project metadata
- [x] libdragon SDK path/version detection
- [x] Build command integration with explicit user approval
- [x] Build log, error navigation and output ROM validation
- [x] Launch generated ROM in the built-in emulator (native `n64forge::Emulator` now backs the Emulator tab)
- [x] libdragon dual-target build template aligned with official libdragon Makefile flow (n64.mk-driven, no hand-rolled rules)
- [ ] PNG/WAV asset import and libdragon asset-tool integration
- [ ] 3D model pipeline after stable SDK/version selection

### Native desktop companion
- [ ] Optional Windows `.exe` and macOS `.app` target for portable game logic
- [ ] Shared gameplay layer with separate N64 and desktop platform adapters
- [ ] Document that N64-specific code and hardware calls cannot be automatically converted into desktop code
