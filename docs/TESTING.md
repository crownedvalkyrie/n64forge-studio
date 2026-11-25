# Emulator tests

## Fast tests (no external ROMs required)

```sh
cmake --preset macos
cmake --build --preset macos-debug
ctest --preset macos-tests -LE hardware --output-on-failure
cmake --build --preset macos-release
ctest --preset macos-release-tests -LE hardware --output-on-failure
```

The normal configure presets don't register external ROM tests. The libretro
smoke test skips without a user-provided core. The fast suite also covers the
IPL2-derived CIC profiles and handoff registers (including 8 MB RDRAM and RI
setup), PI/RI register masks and mirroring, EEPROM/SRAM/FlashRAM command and
persistence paths, IS-Viewer flushes, Controller Pak validation edges, COP2
NEON/scalar golden equivalence, Studio save-media settings, SDL rumble strength
scaling, Metal interpolation blend selection, and exact monotonic cycle-to-time
conversion across fractional deadlines. RSP HLE display-list tests submit real
F3DEX2 commands through the RSP task pipeline and check textured RDP triangles,
multiple sampled texels, both SETOTHERMODE encodings and `G_QUAD`. Studio tests
use an offscreen Qt platform and isolated temporary settings/save directories. Auto selection,
ambiguous matches, and replacement of an active save with a wrong-size file
are exercised without touching user ROMs or saves.

The viewport tests check 1× and 2× drawable geometry and, on a supported
Metal device, read back the production shader's letterbox bars and four image
corners. The readback covers packed RGBA5551 and RGBA8888 textures, nearest
sampling, bilinear filtering and blending. A core test checks packed VI byte
order, H/V window and scale offsets, and padded rows.

The offscreen Studio video suite also paints the cartridge deck with a
synthetic PAL header, checks persisted hardware facts and the unknown CIC
state, and verifies that applying the recommended video baseline leaves the
selected save medium unchanged.
It also checks the parallax preference across fresh settings instances and
checks the cursor offset bounds and exact neutral values when disabled, centered
or outside the card.

The Studio UI contract is one visible control per setting. Settings → General,
Graphics, Performance, Audio & Controls and Discord & Paths owns the controls
named `uiParallaxSetting`, `pauseOnFocusLossSetting`, `bootAnimationSetting`,
`saveMediaSetting`, `frameInterpolationSetting`, `graphicsApiSetting`,
`scalingFilterSetting`, `internalResolutionSetting`, `aspectScalingSetting`,
`aspectRatioSetting`, `cpuModeSetting`, `rdpThreadsSetting`, `rumblePortSetting`,
`rumbleIntensitySetting`, `discordEnabledSetting`, `discordClientIdSetting` and
`artworkRepositorySetting`. The hidden `applyRecommendedSettings` and
`cartridgeHardwareDetails` hooks remain for existing video tests only. The
Library art widget is `N64ForgeStudio_CoverArt`. No async-RCP or Expansion-Pak
toggle is exposed because neither has an independent implementation in the
reference model; RDP worker count and CIC-derived memory size remain the single
sources of truth.

`test_rdp_internal_scale_double_buffer_presentation` covers the universal
internal-resolution path: a refined buffer is retired by `SET_COLOR_IMAGE`,
its sidecar survives while VI scans it out, CPU writes to that buffer still win,
a VI origin one row into the buffer selects the same sidecar, and an unrefined
buffer falls back to native. The bench diagnostic prints VI layout, per-buffer
sidecars, eligibility and the presented source size; OoT at 2×/4× reports
`640×474`/`1280×948`. The active presenter label follows the real GPU presenter
rather than the visibility of the Emulator tab. A Cocoa probe reported `METAL`
and `VULKAN (MOLTENVK)` when those selectors were chosen; KosmicKrisp fell back
to `SOFTWARE` on this host.

The same suite checks the Credits and platform-theme contract without a display:
bundled JSON parses into `Devs`/`Supporters`/`Testers`, malformed documents are
reported instead of crashing, nameless entries are skipped, initials/accent
colours are deterministic, a missing PFP renders a 44×44 initials badge, and a
temporary override file reloads. The dark `QPalette` is checked for a WCAG-AAA
7:1 primary-text contrast ratio. `test_rdp_internal_scale_native_writeback`
already exercises 2×/3×/4× rendering with one thread and with `auto`, asserting
byte-identical native writeback and presentation, so 3× determinism is covered
in both Debug and Release.

When Python 3 is available on macOS, `n64forge_recomp_native` emits and
compiles a firmware-free synthetic cartridge in an isolated temporary
directory. Its native process must return `v0=0xF` after a call, a branch
with a register-changing delay slot and a load from the boot payload. It
also verifies that the CLI refuses to overwrite a nonempty output project.

## Opt-in hardware conformance corpus

```sh
scripts/fetch_hardware_tests.sh
cmake --preset macos-hardware
cmake --build --preset macos-hardware-release --target n64forge
ctest --preset macos-hardware-tests -L hardware --output-on-failure
```

The opt-in configure preset uses a separate build directory. To build and run
fast tests in that same directory, build all targets and use
`ctest --test-dir build/macos-hardware -C Release -LE hardware`.
On Windows, configure with `cmake --preset windows-msvc -DN64FORGE_ENABLE_HARDWARE_TESTS=ON`,
build with `cmake --build --preset windows-debug`, and use
`ctest --test-dir build/windows -C Debug -L hardware --output-on-failure`.
The fetch and run Python entry points can be invoked directly without Bash.
The Windows workflow has not been tested on this host.

The fetcher needs Python 3, network access, and Git plus Rust/Cargo with
rustup for the source-built suite. `--suite nemu|dillon|peter` limits which
suite is fetched. The script uses the upstream pinned nightly (rustup may
install it), builds the pinned `nust64` locally, and stores sources and build
files under the ignored `tests/hardware_corpus/.build/`. It prints installed ROM
SHA-256 hashes and refuses to replace a different existing ROM.

| Suite | Source and scope | Verdict |
|---|---|---|
| `nemu/system` | [`nemu64-test`](https://github.com/thelemmy/nemu64-test), successor to `n64-systemtest`; pinned source with `cycle,timing` enabled because there is no published release ROM. Covers VR4300, COP0, COP1, TLB, RSP, RDP and timing. | OCR of the guest's `Done! Tests: … Failed: …` line; no line means `INCONCLUSIVE`. |
| `dillon/cpu` | [Dillonb/n64-tests](https://github.com/Dillonb/n64-tests) simpleboot release asset. All published ROMs are CPU tests; upstream does not ship separate FPU or RSP ROMs. | First nonzero `r30` sampled after each step: `-1` pass, positive failing subtest index. |
| `peter/{cpu,cop1,rsp,rdp,dma}` | [Peter Lemon's bare-metal tests](https://github.com/PeterLemon/N64), pinned selection: 64-bit ALU, mult/div, `CPULW`, atomics/CPU timing, `COP1FullMode`, RSP vector/DMA, PI alignment and RDP fill/CI4/CI8 TLUT. | OCR failure markers are reported when recognized; otherwise a non-black frame is `REVIEW`, **not** a conformance pass. |

Only public diagnostics are fetched. The Peter Lemon copies retain the header
and test payload but zero IPL3 before writing to disk: these are **post-IPL
diagnostic copies**, not hardware-bootable cartridges. Neither this workflow
nor the native core verifies authentic boot firmware or produces cycle-accurate
timing. Commercial ROMs are never fetched. Both `roms/` and the external
corpus are ignored; place additional legally obtained diagnostics under
`tests/hardware_corpus/local/<subsystem>/` for visual inspection.

## Reports and triage

```sh
scripts/run_hardware_tests.sh \
  --cli build/macos-hardware/Release/n64forge.app/Contents/MacOS/n64forge \
  --cycles 25000000
```

The script defaults to report-only; use `--strict` for the same verdict policy
as CTest. Strict mode fails for `FAIL`, `ERROR`, and `INCONCLUSIVE`; `REVIEW`
is left ungraded. Each run has its own directory containing `report.md`,
`report.json`, and per-ROM `telemetry.json`, `stderr.txt` and `vi.png` if VI
was enabled. PNGs preserve the final guest scanout. OCR is best-effort
(macOS Vision via Xcode, or Tesseract if already installed); original text
and screenshots are retained when available. Always consult the PNG when
OCR confidence is poor. No dark framebuffer, host timeout, or absent OCR
counts as a guest pass or a proven guest failure.

For a shorter iteration, pass `--corpus tests/hardware_corpus/peter/rdp` or
point the runner at a directory with just the test(s) under investigation.
Keep suite/subsystem folders under the passed corpus root to preserve verdict
classification (`dillon/cpu/<rom>`, for example). For one exact probe:

```sh
build/macos-hardware/Release/n64forge.app/Contents/MacOS/n64forge \
  --hw-test-report tests/hardware_corpus/dillon/cpu/basic_simpleboot.z64 \
  25000000 r30 build/basic-vi.ppm
```

The CLI prints JSON including the final PC, Cause, EPC, BadVAddr, exception
histogram and first/last fault locations, VI fields, SP launches, and DP
commands/full syncs. The hardware runner converts the CLI's PPM to PNG with
Python's standard library, so neither Qt nor a PNG dependency is required.
The budget counts nominal CPU steps; it is not an assertion about real time
or emulated hardware latency. Increase it when a test makes progress but
does not reach its completion signal. Never add per-ROM core behavior to
satisfy a test.

### Initial Apple Silicon baseline

Release, 25 million nominal cycles per ROM: **24 pass, 3 fail, 17 visual
review, 1 inconclusive** out of 45 ROMs. CTest correctly reported the corpus
as failed. Dillon `sra` reported subtest **1501** and `srav` subtest **201**.
Peter Lemon `CPUDIV` rendered `FAIL`. `nemu64-test` produced no visible
completion banner; its first exception was code 4 at `0xffffffffa4000600`.
The Peter Lemon `CPULW` scanout had 33,273 non-black pixels, and the fill,
CI4, and CI8 RDP probes each reached a DP full sync. None of those visual
observations is a conformance verdict.

The local `roms/CPULW.N64` also matched scalar execution over 90 VI fields
(CPU state, audio, RGBA hash `776ec1f066b1cb25`). One Release sample measured
267.2% nominal headless throughput and 100.4% in the offscreen Studio probe;
a Studio sample under concurrent load fell to 85.4%. The local 18-byte
`cpu-cop0.z64` was rejected before executing. `oot.z64` and `mk64.z64` were
not present. These results cannot establish sustained or universal real time.

### VR4300 follow-up

The 45-ROM rerun returned **26 pass, 18 visual review, 1 inconclusive**.
Dillon `sra` and `srav` now report `r30 = -1`. Hardware shifts take the
full 64-bit source and sign-extend the resulting word, which matters for
non-sign-extended inputs. Division tests cover zero, signed overflow, and
word HI/LO sign extension. All visible `CPUDIV` result lines show `PASS`;
the runner still labels this visual-only ROM `REVIEW`.

`nemu64-test` passed its first RDRAM broadcast Delay write but still faults
in the IPL3 RDRAM register path (`0xffffffffa4000460`). A 200-million-step
probe hit the same first fault and produced zero VI fields. It has not
reached the guest CPU/TLB test runner. The RDRAM Delay register latch is
modeled for four modules; the full device initialization and timing model
is not.

### Imported baseline (2026-11-23)

Both fast presets passed after the CIC, PI/RI, save-hardware, haptics and
interpolation changes: Release **5/5** (core 1.86 s, Studio 2.05 s) and Debug
**5/5** (core 65.85 s, Studio 3.95 s); `n64forge_libretro_smoke` was skipped
without a user-provided core. A fresh hardware-corpus run returned **26 pass,
18 visual review, 1 inconclusive**; the CTest hardware label remains red.
`nemu64-test` produced no completion banner in 25 million steps and still has
not reached its guest test runner. The remaining visual verdicts are not
conformance passes.

Headless CLI probes with 100 steps:

```sh
CLI=build/macos/Release/n64forge.app/Contents/MacOS/n64forge
"$CLI" /Users/zy/Downloads/oot/oot.z64 --steps 100 --isviewer
"$CLI" /Users/zy/Downloads/mk64/mk64.z64 --steps 100 --isviewer
```

OoT reported `CIC-NUS-6105/7105 (osCicId 6105)` and MK64 reported
`CIC-NUS-6102/7101 (osCicId 7101)`; both reached PC `0x80000414`. Neither
guest emitted an IS-Viewer line in that window. Offscreen Studio probes with
`--studio-video <rom> 1 1500` showed 2,509 non-black pixels for OoT
(320x237, 5 RDP full syncs, 100.1% speed, 59.8 VI fields/s) and 5,910 for
MK64 (320x237, 6 syncs, 99.5% speed, 49.7 VI fields/s). These are sampled
visual/boot results, not gameplay or compatibility verdicts.

### Multi-title boot checks (2026-11-24)

Use `--steps 100 --frames 6000 --isviewer` for the headless checks (600
million nominal CPU steps) and
`QT_QPA_PLATFORM=offscreen build/macos/Release/n64forge_tests --studio-video "$ROM" 0 1500`
for the software viewport check. The latter reports an early non-black image
and a separate rolling speed sample; it does not use Metal or host audio.

| Local title | CIC family (osCicId) | Headless VI fields | Final non-black pixels | Offscreen pixels / sampled speed |
| --- | --- | ---: | ---: | --- |
| Doom 64 | 6102/7101 (6102) | 382 | 8,680 | 8,885 / 100.0% |
| GoldenEye 007 | 6102/7101 (6102) | 379 | 0 (no scanout) | Timed out |
| Majora’s Mask | 6105/7105 (6105) | 382 | 8 | 8 / 100.3% |
| Paper Mario | 6103/7103 (6103) | 382 | 9,513 | 9,333 / 100.2% |
| Mario Kart 64 | 6102/7101 (7101) | 319 | 13,804 | 5,910 / 99.8% |
| Ocarina of Time | 6105/7105 (6105) | 381 | 19,200 | 2,521 / 100.3% |

No IS-Viewer text was captured. In separate headless probes with an explicit
in-memory `--flashram` device, Majora’s Mask went from 0 to 185 RDP full
syncs (267 final non-black pixels) and Paper Mario from 1 to 344 (75,840
pixels), in the same cycle budget. GoldenEye still had no scanout with
`--eeprom4k`. These do not establish gameplay compatibility.

Both final macOS builds passed all four enabled fast tests; the optional
libretro smoke test skipped. The hardware corpus remained at **26 guest
passes, 18 visual reviews, 1 inconclusive**: strict CTest returns failure
because `nemu64-test` produces no completion banner.

The follow-up `LOADBLOCK` stride regression renders CI4, CI8 and RGBA16
perspective triangles into guest RAM. It varies W by row and checks every
pixel after two-cycle environment-color modulation. Both fast presets
passed. Six headless boot probes reproduced the same final pixel/VI counts
as above; textured gameplay past the intro was not established.

### Input-driven stall diagnosis

```sh
CLI=build/macos/Release/n64forge.app/Contents/MacOS/n64forge
"$CLI" --stall-probe "$ROM" 1200 --flashram
```

The last argument is optional; supported explicit in-memory devices are
`--flashram`, `--sram`, `--eeprom4k` and `--eeprom16k`. The probe leaves
save files alone and alternates six-field Start and A pulses on port 1
through ordinary Joybus input. It logs every 60 field slices and at exit:
PC/RA/SP, the PC range of 128 CPU steps, COP0 and RCP state, cumulative
exception counts and last PC by cause, a VI fingerprint, and IS-Viewer
text if present. A two-PC idle sample alone does not establish a stalled
guest OS.

| Title (save device) | VI fields | DP full syncs, before → after | SP launches, before → after | Final non-black pixels, before → after |
| --- | ---: | ---: | ---: | ---: |
| Ocarina of Time (SRAM) | 1,199 | 1,027 → 1,027 | 2,746 → 2,746 | 19,761 → 75,618 |
| Mario Kart 64 (EEPROM 4 Kbit) | 1,200 | 213 → 429 | 1,394 → 1,603 | 72,128 → 74,144 |
| Paper Mario (FlashRAM) | 1,200 | 1,137 → 1,137 | 2,308 → 2,308 | 75,649 → 75,649 |
| Majora’s Mask (FlashRAM) | 1,200 | 512 → 638 | 2,186 → 2,436 | 8 → 2,669 |

The TLB fix alone did not change the baseline totals. No probe emitted IS-Viewer text
or reproduced the reported Paper Mario `E000E000` load fault. A synthetic
overlay test exercises instruction fetch and data loads/stores through
`0xFFFFFFFFE0000000`, remaps the same address via `TLBWI` and `TLBWR`,
checks an unrelated region cannot match even a global entry, and
returns via `ERET`.

In the Majora’s Mask baseline, DP stopped at 512 full syncs while VI and
SP continued. The new exception trace isolated cause 10 at `0xFFFFFFFF80086D24`.
The word at that address was `0x4620630C` (`ROUND.W.D`), which previously
raised reserved instruction. The general COP1 word-rounding fix removed
the exception: the repeat had 638 DP full syncs, 2,436 SP launches, and
2,669 final non-black pixels. These are progress measurements, not
proof of interactive gameplay or that all MI edges were delivered.

### Textured triangle probes

`--stall-probe` now reports cumulative triangle-command, textured-triangle and
shaded-triangle counts (`tris`, `tex`, `shade`). An optional in-memory save
device and PPM path write the final VI framebuffer without touching user save
files:

```sh
CLI=build/macos/Release/n64forge.app/Contents/MacOS/n64forge
"$CLI" --stall-probe "$HOME/Downloads/oot/oot.z64" 1200 --flashram oot.ppm
"$CLI" --stall-probe "$HOME/Downloads/mk64/mk64.z64" 1500 --eeprom4k mk64.ppm
```

OoT reported `tris=188345 tex=188345 shade=188215`; its 320×237 snapshot had
75,618 non-black pixels, 775 unique colors, and 264 of 300 sampled tiles with
non-uniform pixels. MK64 reported `tris=280740 tex=268573 shade=280740`; its
snapshot had 73,935 non-black pixels, 2,333 unique colors, and 189 of 300
sampled tiles with non-uniform pixels. These counts come from the native RSP
and RDP path. `RspHle` is a separate deterministic reference path used by the
core tests, so the HLE texturing changes are **not** responsible for these
native ROM results.

The same probe parameters did not establish textured 3D for the other two
local ROMs: Paper Mario reported `tris=0 tex=0 shade=0`, one full sync, and
9,513 uniform-gray non-black pixels; Majora's Mask reported
`tris=0 tex=0 shade=0`, no full syncs, and 8 non-black pixels. No ROM identity, CRC or
game-specific PC selects behavior; these are reported outcomes of the shared
hardware model.

### Native RDP and Studio follow-up

Both macOS configurations passed all six runnable non-hardware tests; the
optional libretro smoke test skipped. The strict hardware corpus remained at
**26 guest passes, 18 visual reviews, 1 inconclusive**. The separately supplied
local `n64-systemtest.z64` now reaches individual tests over IS-Viewer after
`SYNC` was implemented, but reports guest diagnostic failures and still has
no visible diagnostic output. The pinned `nemu64-test` corpus image has no
completion banner, so there is no hardware boot/conformance pass to report.

| Native headless probe | Slices | Full syncs | Textured triangles | Final non-black pixels |
| --- | ---: | ---: | ---: | ---: |
| Ocarina of Time (SRAM) | 420 | 281 | 18,213 | 75,580 |
| Mario Kart 64 (EEPROM 4 Kbit) | 1,200 | 429 | 149,013 | 74,159 |
| Doom 64 | 400 | 185 | 0 | 8,680 |

OoT and MK64 had byte-identical final PPMs with `--rdp-threads 1` and `auto`.
The OoT title scene still showed diagonal artifacts. These probes establish
native texture activity, not universal seam-free rendering.

On the Apple M3, the eight-second Cocoa `--studio-bench` probes
(`0 8 nearest 120 jit`) used the 2× Metal drawable and recorded minimum
guest speeds of **41.7% for OoT** and **31.8% for MK64** after warmup. These
are not 120 Hz or real-time passes; host audio underruns and physical
controller haptics were not verified in those probes. The host's Homebrew
Qt/SDL binaries have newer minimum macOS versions than the project target;
the local deployment-target linker warnings do not establish macOS 13
compatibility for those dependencies.

### Fractional edges, VR4300 exceptions and Vulkan viewport

The raw RDP regression renders the shared diagonal as `XL` in one command
and `XH` in the other. `YM` and `YH` have fractional scanlines and use
different X stepping origins. The test checks all 64 interior pixels and
the exact write count. Shaded/textured/depth attributes are checked at the
`(XH, floor(YH))` latch and at the negative-slope `do_offset` latch; HLE
coefficient bases include the `YH` origin. A separate large patterned
primitive compares RDRAM, the debug framebuffer, and pixel counts across
one thread, four threads, and auto. The memory regression checks that RDP
data is visible immediately while coherency callbacks are deduplicated per
touched 64-byte line, with no coarse range across gaps.

Native boot probes at 420 slices for OoT (SRAM), 1,200 for MK64 (EEPROM 4 Kbit),
and 400 for Doom 64 reported 281, 429 and 185 DP full syncs, respectively.
OoT and MK64 snapshots were byte-identical between `--rdp-threads 1` and
`auto`. At 180 field slices, `--compare-event-batching` matched scalar CPU
state, audio and every VI pixel for both titles (179 OoT and 180 MK64
fields). The new interpolation changed the triangle-heavy VI fingerprints to
`b108eb97c8b566b6` (OoT) and `bacffecbc1b9dfed` (MK64); the Doom fingerprint,
which submitted no triangles, stayed `66a431558f6edd3d`. Final PPM SHA-256
values were `7a510465…d7d32b7ec35` for OoT, `5c7095c2…603ae3612` for MK64 and
`c26ba5c7…b4842d7b9a92eae` for Doom, with 1-thread and auto matching for the
first two. The OoT title snapshot differs from the previous
midpoint-interpolation capture at 2,354 of 75,840 pixels (90 pixels with a
channel delta above 24), but a seam-free commercial scene has not been
visually re-verified. The strict hardware corpus returned **26 guest passes,
18 visual reviews, one inconclusive** (the pinned `nemu64-test` produced no
completion banner).

With the separate local `n64-systemtest.z64`, `--steps 100 --frames 1800
--isviewer` no longer reports failures for the sampled unaligned LW, SW,
LWC1 and jump tests, or the prior COP2 preset failure and exception storm.
It proceeds into later diagnostics, still reports other guest failures
(including CACHE and non-sign-extended effective addresses), and does not
produce a completion verdict or visible image in that run.

The optional Cocoa Vulkan backend is compiled when CMake finds recent
Vulkan headers. If needed, configure with
`cmake --preset macos -DN64FORGE_VULKAN_INCLUDE_DIR=/path/to/sdk/include`.
No Vulkan library is linked. Install MoltenVK or install KosmicKrisp with
the Vulkan loader and its ICD manifest; `VULKAN_SDK` can specify the SDK
root and `VK_DRIVER_FILES` can point to the manifest. A live synthetic smoke
probe uses a generated ROM, checks red and green VI source pixels, and
requires a successful swapchain present rather than a Software fallback:

```sh
QT_QPA_PLATFORM=cocoa build/macos/Release/n64forge_tests \
  --studio-bench --synthetic 0 2 nearest 60 jit moltenvk
```

This passed with MoltenVK 1.4.2 both directly and through Vulkan Loader
1.4.363 (the loader run used linear filtering). The final RDP/coherency
rebuild was rechecked in both modes: direct MoltenVK reported 99.82% minimum
guest speed and the loader path 100.55%, both with the presenter identified
as `VULKAN (MOLTENVK)`. These synthetic checks cover source pixel values and
swapchain submission, not Vulkan pixel readback. KosmicKrisp was not installed
and could not be verified. SDL3 reported success starting
a 250 ms rumble pulse and stopping it on a connected Nintendo Switch Pro
Controller; that verifies device/API acceptance, not tactile output.

### Blender cycle-type correction and RDP edge regressions (2026-11-24)

Four deterministic raw-RDP regressions now cover this area. The shared-edge
test draws two adjacent shaded triangles whose common diagonal is `XH` on one
side and `XL` on the other, with `ALPHA_CVG_SEL`, `CVG_X_ALPHA`, no forced
blend and single-sample edge pixels. It checks all 64 interior pixels and
fails when a second blender equation replaces a partially covered edge with
black. A second test sets `COLOR_ON_CVG` with an explicit stage-0
A=shade-alpha and M=fog selection: partial diagonal pixels must select fog
alone, prewrapped pixels must keep the ordinary equation. A third test uses
distinct stage-0 (memory green) and stage-1 (fog blue) M inputs for cycle type
0 and cycle type 1. It asserts that one-cycle output is the stage-0 result,
two-cycle output is the stage-1 result fed by stage 0, and `COLOR_ON_CVG`
substitutes the active stage's M only on non-prewrapped pixels. This test
fails if the cycle-type predicate is inverted. The tile test drives `0x0E`
and `0x0F` textured triangles for all 256 S/T `SETTILE` shift pairs from 0
through 15, using a hand-derived coordinate table that crosses signed-16-bit
wrap for shifts 11 through 15. The implementation uses an unsigned
intermediate for the left shift. Non-quarter-aligned tile bounds and
`LOADTILE` source coordinates remain outside this regression.

The earlier blender edit used `1 << 52` as "one cycle" in its test commands.
That field is the RDP cycle type, where 0 = 1-cycle, 1 = 2-cycle, 2 = copy and
3 = fill. The code branch had been inverted: the one-cycle path ran two
equations and the two-cycle path ran one. That changed real ROM captures and
matches the reported texture regression. The corrected code keeps the separate
cycle paths and active-stage `COLOR_ON_CVG` substitution while restoring the
documented cycle-type mapping; no ROM identity or game-specific command value
selects behavior.

After the correction, the full non-hardware suite passed in both
configurations: Release **6 passed, 1 skipped** (17.31 s total; core tests
3.47 s) and Debug **6 passed, 1 skipped** (231.06 s total; core tests
208.53 s). The optional libretro smoke test skipped without an external core.
Parity was unchanged: final 420-slice OoT and MK64 captures were
byte-identical between `--rdp-threads 1` and `auto`, with SHA-256
`7a5104655aeaa67089c8e241292b68d35c8066db002e937012015d7d32b7ec35` (OoT) and
`457c4781f4f25592ec48053fe4b1736910c34c242d69f8c5d5e1806317ca9ebf` (MK64).
These match the pre-blender-edit baseline captures, while the inverted-cycle
variant hashes were different. `--compare-event-batching` matched scalar CPU
state, audio and every VI pixel for 419 OoT and 420 MK64 fields, with RGBA
hashes `b5c355235d43a61e` and `b18e3079278b77ba`.

The OoT title capture still contains green and tan textured regions. An
isolated-pixel scan is not a universal proof of seam-free textured triangles,
and a headless A-only probe reached the file-select screen but not Hyrule
Field within 3,000 field slices, so the flat tan ground observation is **not
re-verified** by a final gameplay capture. Hardware-conformance status is
unchanged; none of these probes is a guest pass.

### Gouraud shade, three-point filtering, and two-cycle fog follow-up

The raw `0x0E` regression sends both attribute blocks through the FIFO,
checks signed/fractional coefficient stepping and partial-coverage shade
correction, and verifies exact pixels in point and three-point filter modes.
It exercises `SAMPLE_TYPE` (bit 45) and `BI_LERP_0` (bit 43) independently
and checks the two triangles of the filter kernel and a fractional S clamp
boundary. `BI_LERP_1` is bit 42; bits 51:50 are perspective and detail, not
filter selectors. The render still uses the RDP's programmed modes; it
doesn't force smoothing on flat-shaded geometry.

For textured triangles with max level zero and neither detail nor sharpen,
the combiner now receives `LOD_FRAC=0xff`, even without mip tile selection.
The raw test uses it as the texture-alpha multiplier and checks that the
triangle survives alpha compare. Peter Lemon's RGBA16 texture-triangle ROM
previously drew only its yellow background, despite submitting 45 RDP
commands. The revised run shows all nine textured shapes; 4,614 pixels
differ from the background, also the count in the suite's reference image.
Pixel values and placement are not identical to that reference.
Derivative-based LOD remains unmodeled.

A separate raw test verifies that the two-cycle blender retains its first
equation (used for fog) even when its final blend is disabled. CLAMP, WRAP,
ZAP, SAVE and `COLOR_ON_CVG` were cross-checked against the reference coverage
path; this does not model exact blender divider or hidden-bit timing.
Native lighting still runs in guest RSP microcode. The LLE vector instruction
paths were reviewed; the available tests did not isolate a lighting defect
there, so no LLE vector arithmetic was changed. The separate HLE test/tool
producer now emits `DY` to match the decoder.

The pinned hardware corpus returned **26 guest passes, 18 visual reviews,
1 inconclusive** (the nemu64-test cartridge did not report completion).
A separate 70-ROM run of selected Peter Lemon RDP/RSP tests, the Project64
mirror, and a local n64-systemtest build returned **69 visual reviews,
1 inconclusive**. The local system test reached individual guest failures
over IS-Viewer with a longer budget, but did not report overall completion.
Peter Lemon's RGBA16 and RGBA32 Gouraud diagnostics displayed gradients in
all eight orientations. Peter Lemon's VMULF, VMACF, VMUDN, VLT and VRCP screens each
showed per-vector `PASS` markers on visual inspection; this is not an
end-to-end microcode lighting verdict. The `n64triangle` 3.0 release rendered
its default triangle and edge guides; that is a visual check, not a guest verdict.

Headless local ROM captures below use the same inputs before and after the
RDP change. They do not establish continuous gameplay or hardware equivalence.

| ROM | Slices | Textured triangles | Final observation |
| --- | ---: | ---: | --- |
| Ocarina of Time | 420 | 18,213 | Title-scene ground texture detail became visible; gameplay not verified |
| Mario Kart 64 | 1,200 | 149,013 | Reached the starting grid; no full race verified |
| Super Mario 64 | 1,200 | 180,650 | Reached Peach's letter; the earlier title snapshot showed gradients on Mario's head before and after |
| GoldenEye 007 | 420 | 15,389 | Opening logo; gameplay not verified |
| Paper Mario | 1,200 | 19,298 | File select; pixels changed |
| Majora's Mask | 1,200 | 111,080 | Opening narration; no gameplay lighting comparison |
| Doom 64 | 420 | 0 | Copyright screen unchanged; no triangle coverage |

One-thread and automatic RDP worker captures matched for OoT and MK64.
Event batching matched unbatched CPU state, audio and VI pixels for 179 OoT
and 180 MK64 fields (RGBA hashes `3279f5929123e590`, `5fab45105a8e47e3`).
The offscreen Studio viewport also presented both ROMs. Release and Debug
each passed all six runnable non-hardware tests; the optional libretro smoke
test skipped. No test or commercial ROM binaries were added to Git.

### Windows native presenters and x64 JIT (2026-11-25)

The single persisted Graphics API setting gained appended `Auto`, `D3D12` and
`Vulkan` values; `Metal=0`, `MoltenVK=1`, `KosmicKrisp=2` and `Software=3`
were not renumbered. Windows uses `Auto` by default and probes
D3D12 → Vulkan → Software. Apple keeps the historical Metal default and probes
Metal → MoltenVK → KosmicKrisp → Software. An explicit hardware choice falls
back only to Software, never silently to another hardware API. If an `Auto`
candidate probes successfully but cannot attach to the native view, the next
`Auto` candidate is tried and Software remains the final fallback. These rules
are covered by deterministic resolver tests that do not need a host GPU.

The Windows D3D12 presenter uses a DXGI flip-discard swapchain, three fenced
upload slots, point/linear samplers and two shaders (native RGBA8888 sampling
and RGBA5551 unpacking). Swapchain buffers transition PRESENT → RENDER_TARGET
→ PRESENT inside each command list. The Windows Vulkan presenter opens
`vulkan-1.dll` at runtime, uses `VK_KHR_win32_surface`, and blits/scales native
VI pixels into the swapchain; neither loader nor ICD binaries are bundled.
Both Windows and Vulkan SDK headers stay inside their platform backend files;
the renderer-neutral `ViewportPresenter` interface has no HWND, COM or Vulkan
types.

Windows x64 CI configures both native backends explicitly, fetches only pinned
Khronos Vulkan headers at build time, and asserts that the generated compile
commands include `d3d12_viewport.cpp`, `win32_vulkan_viewport.cpp` and
`jit_x64.cpp`. The native x64 JIT is the Windows x64 default CPU mode and
must report available during `n64forge_tests`, which runs the same parity
suite as the ARM64 backend. No Windows host was available locally, so Windows
attach/present behavior and performance were **not** verified on this host.

Exact Windows x64 commands for a user-installed header location:

```sh
cmake --preset windows-msvc ^
  -DN64FORGE_ENABLE_D3D12=ON ^
  -DN64FORGE_ENABLE_WIN32_VULKAN=ON ^
  -DN64FORGE_WIN32_VULKAN_INCLUDE_DIR:PATH=C:/VulkanSDK/<version>/Include
cmake --build --preset windows-debug
ctest --preset windows-tests
```

### Final host verification (2026-11-25)

A clean Xcode configure, Debug build, Debug test run, Release build and Release
test run used a fresh `build/macos-verify` directory. `n64forge_core`,
`n64forge_video` (Metal and optional Vulkan where headers were present),
`n64forge_recomp`, the `n64forge` CLI, the Qt 6 Studio app and `n64forge_tests`
built in both configurations. Each CTest run had six passing tests and one
skipped test (`n64forge_libretro_smoke`, no external core supplied): Debug took
256.58 s and Release took 20.71 s. The x86_64 `n64forge_tests` target was also
rebuilt and passed under Rosetta, exercising the native x64 JIT parity suite on
this Apple host.

Headless boot progression was repeated after the CPU/JIT edits with:

```sh
build/macos-verify/Release/n64forge.app/Contents/MacOS/n64forge "$ROM" \
  --cpu-mode jit --steps 100 --frames "$BATCHES" --isviewer
```

`--frames` counts 100,000-step diagnostic batches, not VI fields.

| Local title | Batches | Native execution observed | VI result |
| --- | ---: | --- | --- |
| `mk64.z64` | 1,600 | 150,117,424 native JIT steps; 17 RDP full syncs; 7,563/9,125 triangle commands rasterized pixels | 84 VI fields, `0x80000598`, 13,886 non-black pixels |
| `oot.z64` | 1,600 | 156,433,305 native JIT steps; 73 RDP full syncs; 3,447/3,454 triangle commands rasterized pixels | 100 VI fields, `0x80000814`, 3,741 non-black pixels |
| `CPULW.N64` | 160 | 8,827 native JIT steps with no OSTask | 10 VI fields, `0x80008DF0`, 33,273 CPU-written non-black pixels |

All three exited without an exception. A shorter 100-batch JIT smoke also
completed without an exception for OoT, MK64, Paper Mario, Doom 64 and Majora's
Mask. The local `roms/cpu-cop0.z64` file was rejected by the cartridge loader,
so it contributed no boot result. These are progression measurements, not guest
pass verdicts, pixel-accuracy claims or continuous-gameplay evidence. Windows
presenter attach/present behavior and Windows x64 CI results were not available
on this host.
