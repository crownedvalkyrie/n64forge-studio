# Native performance on Apple Silicon

Build and test on macOS:

```sh
cmake --preset macos
cmake --build --preset macos-debug
ctest --preset macos-tests
cmake --build --preset macos-release
ctest --preset macos-release-tests
```

The Xcode Release configuration uses `-O3` and ThinLTO. Keep both builds at
the same settings when comparing code changes. Unit-test assertions remain
active in Release. Windows was not tested; see [Windows build commands](ROADMAP.md#macos-native-boot-regression).

## Live diagnostics

In Studio, choose **View → Show Performance Overlay** (**F3**) or open
**View → Diagnostics** for the detailed readout. For headless monitoring:

```sh
build/macos/Release/n64forge.app/Contents/MacOS/n64forge \
  "$ROM" --steps 100 --frames 6000 --stats
```

- The sampler takes deltas over roughly one-second windows and smooths rates
  with a one-second EMA. Studio refreshes the text up to four times per second.
- **Speed** and **effective MHz** are based on nominal guest cycles at
  93.75 MHz. The reference CPU still charges one nominal cycle per step,
  not hardware-accurate instruction or stall timing. **MIPS** counts
  dispatched instructions; interrupt entries and failed fetches are excluded.
- **VI/s** counts guest field boundaries, including blanked fields. **Display
  FPS** counts native frame requests accepted for presentation or QPainter presents,
  including presentation-only blended frames. It is not a monitor-vsync counter.
  Diagnostics separately shows the measured host timer cadence and its 60/120 Hz
  target.
- **Field time** is wall time per guest VI field. CPU time samples batched
  and single-step work separately, and RSP time is sampled while running;
  RDP measures DPC processing, and sleep is measured on
  the worker. Other time includes device ticks, VI scanout, and frame handoff.
  Timing is opt-in and does not change guest scheduling.
- **Audio FIFO** is the host 48 kHz stereo ring. Underruns count transitions
  from primed playback into starvation; overruns count pushes that dropped
  frames. Guest AI rate uses the regional clock divided by `AI_DACRATE + 1`
  while AI is enabled. Headless runs have no host FIFO or display.
- RSP task rates are DMEM OSTask observations at SP unhalt, not hardware task
  counters. RDP triangles/s counts triangles that drew at least one pixel.
  RDP surface and VI output scale are reported separately. Cache hit rate
  covers eligible direct RDRAM instruction fetches; other fetches are excluded.

## Execution paths and attribution

`--cpu-mode jit|cached|interpreter` selects the guest CPU path. The opt-in
ARM64 JIT translates quiet RDRAM blocks on Apple Silicon, chains direct
branches, and compiles COP1 compares and BC1 with a CU1 guard. Aligned direct
KSEG0/KSEG1 loads and stores are native. MMIO, TLB accesses and stores to code
lines take the interpreter path. The JIT checks RDRAM line generations and
unlinks stale code on writes; timing and interrupts stay in the guest CPU.
`jitSteps` in `--stats` counts native instructions. Windows and `arm64e` hosts
fall back to cached execution. The JIT remains opt-in. See the comparison below.

`RspHle` is a display-list reference used by deterministic tests and tooling.
Native ROM execution still uses the real RSP interpreter. Consequently the
HLE texturing fix is validated by `n64forge_tests`, not by native OoT/MK64
probe counters or snapshots. Those probes come from the RSP/RDP hardware
model and show textured triangles with non-uniform pixels; Paper Mario and
Majora's Mask did not reach textured output in the sampled windows. See
[TESTING.md](TESTING.md#textured-triangle-probes) for the counts and caveats.

## Event batching and Studio verification (macOS Release)

The Studio viewport delivered non-black images for all three local probes.
The Studio samples were read five seconds after switching back to Fit Window
with interpolation off. Studio is paced against guest time; these snapshots
are not a guarantee of sustained gameplay throughput or compositor refresh.
Headless runs used `--steps 100 --frames 6000`.

| ROM | Studio speed sample | Headless time | Headless nominal speed | Final headless non-black pixels |
| --- | ---: | ---: | ---: | ---: |
| `mk64.z64` | 78.7% | 5.59 s | 114.5% | 13,628 |
| `oot.z64` | 99.6% | 4.60 s | 139.1% | 19,200 |
| `CPULW.N64` | 99.5% | 3.23 s | 198.1% | 33,273 |

`n64forge_tests --compare-event-batching "$ROM" 180` compared CPU state,
audio samples and every VI pixel against single-step ordering. Device ticks
fell from 338.5 M to 34.1 M for Mario Kart, 283.1 M to 7.0 M for Ocarina,
and 282.1 M to 1.23 M for CPULW. Mario Kart still falls below real time in
the sampled live scene, where RSP and RDP activity remains substantial.

The host timer test checks both 60 Hz and 120 Hz with the guest worker stopped,
independently of guest VI production. UI load can still cause missed deadlines.

## Sustained Metal viewport benchmark (macOS Release)

`n64forge_tests --studio-bench <rom> <scale> <seconds> [nearest|linear] [60|120] [jit|cached|interpreter] [metal|moltenvk|kosmickrisp]`
warms up for 3 s and then reports rolling minima over the measurement window.
All figures use `mk64.z64` on an Apple Silicon host with presentation-only
blending at the requested refresh target.

| Scale | Filter | Target | Min guest speed | VI fields/s | Display FPS | Worst GPU command time |
| ---: | --- | ---: | ---: | ---: | ---: | ---: |
| 0× (Fit) | nearest | 120 | 73–89% | 45.8–47.6 | 117.8–118.1 | 3.82–3.91 ms |
| 4× | nearest | 120 | 35.2% | 30.7 | 116.9 | 3.82 ms |
| 4× | linear | 120 | 81.3% | 46.0 | 117.4 | 3.73 ms |
| 4× | nearest | 60 | 90.3% | 47.6 | 59.0 | 3.19 ms |
| 4× | linear | 60 | 87.2% | 47.4 | 59.1 | 3.22 ms |

A separate 8 s native probe reported 99.5% speed, 93.3 MIPS, 49.7 VI fields/s,
95.1 FPS, 0.617 ms GPU command time, and one audio underrun over 114,539 host
FIFO pushes. A no-SDL build (CoreAudio fallback) reported 99.9% speed, 49.9
VI fields/s, 77.2 FPS, 0.365 ms GPU time, and the same 114,539 pushes. These
samples do **not** establish sustained 100%+ guest speed or `<0.2 ms` GPU time
under all settings; the 4×/120 Hz case drops below real time. The GPU timer
measures command submission and present, not total compositor cost.

A 2026-11-23 Cocoa recheck used the same 4×/linear/120 Hz/4 s command for
both local ROMs. OoT reported 99.83% minimum speed, 59.77 VI fields/s,
116.19 display FPS and 0.651 ms worst GPU command time. MK64 reported
92.49% minimum speed, 49.08 VI fields/s, 116.00 display FPS and 0.600 ms
worst GPU command time. Neither sample establishes sustained 100%+ playback
across games.

### Six-title Retina probe (2026-11-24)

These intermediate Release measurements were taken on an Apple M3 at
1440×900 logical pixels with the packed Metal path and linear filtering.
They precede the final line-coefficient and presentation-cadence changes.
The minimum is the lowest reported rolling speed in the 5-second window
(10 seconds for Mario Kart and OoT) after 3 seconds of warm-up, not a
per-frame guarantee. The list includes boot-limited and no-video runs.

| ROM | Fit / 60 Hz | Fit / 120 Hz | 4× / 60 Hz | 4× / 120 Hz |
| --- | ---: | ---: | ---: | ---: |
| Doom 64 | 100.01% | 100.04% | 97.96% | 97.32% |
| GoldenEye 007 (no scanout) | 25.35% | 38.33% | 23.24% | 38.50% |
| Majora’s Mask (no FlashRAM) | 99.89% | 99.91% | 63.63% | 98.72% |
| Paper Mario (no FlashRAM) | 99.99% | 60.67% | 99.67% | 88.71% |
| Mario Kart 64 | 85.62% | 67.61% | 95.32% | 95.86% |
| Ocarina of Time | 99.13% | 99.33% | 74.37% | 98.84% |

### Verified Retina baseline

The 6-second linear-filter recheck on the same M3 used a requested 1600×1050
logical window. Cocoa constrained it to a 3024×1834 drawable at 2×; both
settings consequently fitted the same centered 2445×1834 rectangle. Use a
larger desktop to measure the 4× cap independently of Fit Window.

| ROM | Fit / 60 Hz | Fit / 120 Hz | 4× / 60 Hz | 4× / 120 Hz | Fit/120 CPU / RSP / RDP / GPU ms |
| --- | ---: | ---: | ---: | ---: | ---: |
| Ocarina of Time | 99.35% | 99.32% | 98.95% | 99.77% | 7.13 / 2.15 / 2.46 / 1.55 |
| Mario Kart 64 | 99.22% | 99.08% | 99.02% | 98.56% | 8.87 / 6.46 / 2.67 / 1.77 |

The guest columns are rolling minimum speed. CPU/RSP/RDP times are averaged
per guest field; GPU time is the mean sampled command duration, not display
latency. Both runs drew 3D geometry, but neither verifies continuous
gameplay, sustained 100% minimum speed, or GPU commands under 0.2 ms.

### After the COP1 word-rounding fix

At the same 3024×1834 Retina drawable, 8-second Fit Window / linear / 120 Hz
Release probes measured a rolling minimum of **86.01%** for Ocarina of Time
and **77.16%** for Mario Kart 64. RDP averaged 4.33 and 4.00 ms per guest
field, respectively; the GPU command averages were 3.93 and 3.91 ms. Both
probes rendered more triangles per second than the previous shorter probe.
The windows do not necessarily cover identical guest scenes, so the numbers
are not an isolated CPU or renderer regression measurement.

A later Release recheck at the same 3024×1834 drawable still used Fit Window /
linear / 120 Hz. The rolling minimum was approximately **93.7%** for Ocarina
of Time and **74.1%** for Mario Kart 64. This remains below the 100%+ Fit
Window / 120 Hz target; no guest timing or presentation frame was advanced to
mask the shortfall.

## Historical block-batching benchmark (not current performance)

On an Apple M3, two-second `sample` captures at 1 ms intervals during the
headless ROM runs identified `Cpu::runBlock` and `Bus::read8`/`read32` as the
largest leaf costs on both games. VI, AI and DMA per-step dispatch are still
prominent. COP2 is a larger cost in Mario Kart, and the RDP fragment writer
is a larger cost in Ocarina of Time.

For each legally obtained local ROM:

```sh
/usr/bin/time -p build/macos/Release/n64forge.app/Contents/MacOS/n64forge \
  "$ROM" --steps 100 --frames 6000
```

`--frames` counts 100,000-step diagnostic batches. VI FPS below means actual
guest VI fields divided by wall time; it is not the game's render rate or the
Studio present rate. Real-time speed uses the core's nominal one-cycle-per-step
model: `(600000100 / 93750000) / wall_seconds * 100`. Both measurements include
boot, ROM loading and diagnostic scanout overhead.

| ROM | Wall time | VI FPS | Real time | Final VI pixels |
| --- | ---: | ---: | ---: | ---: |
| `mk64.z64` | 6.80 s | 46.91 | 94.1% | 36 |
| `oot.z64` | 5.32 s | 71.62 | 120.3% | 0 |
| `CPULW.N64` | 14.37 s | 26.58 | 44.5% | 33,273 |

Do not use these timings as a speedup or compatibility claim. The batched
CPU/device ordering regressed boot video in both commercial probes. The
Studio and diagnostic loops now interleave CPU and devices every nominal step.

## Scope

- Live and diagnostic loops batch event-free cached block prefixes. VI and AI
  clocks advance at their next line/sample deadline, or an earlier CPU
  barrier. COP0 Compare match cycles use single-step dispatch. MMIO writes,
  faultable accesses, active RSP, and in-flight PI/SI DMA retain reference
  ordering. SP DMA remains synchronous in the existing model. Read-only
  `VI_CURRENT` can be batched because it is stable until the next line event.
  These are scheduling changes to the nominal one-cycle-per-step model,
  not cycle-accurate VR4300/RCP timing.
- Aligned RDRAM loads/stores use the two fixed KSEG aliases; TLB, MMIO and
  other accesses retain their existing paths.
- ARM64 NEON handles COP2 element broadcast, fractional/high/mixed multiply
  and accumulate, saturating add/subtract, VABS and bitwise operations. The
  portable implementation remains the reference in the scalar test build.
- RDP spans cache their coefficients and fast-path fully covered interior
  pixels. ARM64 NEON interpolates and clamps shade channels without changing
  the pixel order. Fragments stay serial because they share noise state,
  guest color/depth storage and RDRAM write observers.
- Studio paints native VI images using **Fit Window** (4:3 by default) or
  fixed 1×/2×/4× viewport widths. The worker renders directly into four
  preallocated slots. Qt leases immutable slot pixels without copying them;
  unchanged RDRAM lines and identical output skip publication. On a supported
  unified-memory Metal device, the core copies active VI rows from mutable
  RDRAM into shared snapshot buffers and the GPU samples them through linear
  texture views. There is no second CPU copy or per-pixel CPU conversion in
  that path. The shader handles RGBA5551 unpacking, RGBA8888, nearest/bilinear
  filtering and blending. Metal uses a fitted physical-pixel viewport inside
  a full-size drawable. Software and non-unified-memory fallbacks remain.
  Higher internal RDP resolutions are disabled until true rerasterization
  exists. The CLI `--internal-resolution` option still scales only the debug
  surface.
- Studio runs the guest on a worker paced by cycles consumed, independent of
  the UI timer. Short overruns keep their deadline debt and fractional
  nanoseconds; the scheduler reanchors after a pause or a lag over 250 ms.
  The 60/120 interpolation targets blend adjacent VI images on Metal; block
  motion estimation only runs when the software path actually needs an
  intermediate image. The timer is not a display-vsync lock.
- Studio audio uses SDL3 `SDL_OpenAudioDeviceStream` with a native CoreAudio
  AudioQueue fallback when SDL3 is unavailable. A bounded, lock-free stereo FIFO
  feeds the host callback at 48 kHz; the guest AI clock is independent. A no-SDL
  build was verified to produce non-silent host telemetry. Device playback and
  glitch rate on other hosts are not established by headless runs.

## Fit Window / 120 Hz follow-up

On the M3, a fresh baseline OoT sample from `9878659` ran at a 77.1%
minimum (the earlier report observed 41.7%). A five-second macOS `sample`
trace found the guest worker in `Cpu::runQuietBlock` and the RDP fragment
path, particularly fill rectangles. `CAMetalLayer::nextDrawable` ran on
the UI thread, not the guest worker. The benchmark's GPU command time was
0.55 ms on average, compared to an estimated 8.9 ms CPU, 3.0 ms RSP and
5.3 ms RDP per guest field. Native JIT telemetry proves some prefixes ran
natively, not that all CPU work was recompiled.

## Final 3× Metal probe (2026-11-25)

These Release runs used the required 8-second measurement window after a
3-second warm-up, a 3024×1834 physical Retina drawable, 120 Hz target,
nearest filtering and `3×` fixed viewport scaling. Both `jit` and `cached`
were measured on each legally obtained local ROM with
`QT_QPA_PLATFORM=cocoa ./build/macos/Release/n64forge_tests --studio-bench "$ROM" 3 8 nearest 120 <jit|cached> metal`.
The presenter label is now derived from the Metal presenter's completed
frame, so these runs report `METAL` rather than a misleading `Software` or
waiting state.

| ROM | Mode | Min rolling speed | Final speed | CPU ms/field | RSP ms/field | RDP ms/field | GPU ms avg / max | VI/s | Display FPS |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `oot.z64` | `jit` | 98.78% | 100.0% | 4.59 | 3.09 | 2.41 | 0.59 / 4.78 | 59.6 | 60.2 |
| `oot.z64` | `cached` | 79.95% | 99.8% | 5.79 | 3.18 | 2.86 | 0.63 / 3.99 | 57.6 | 57.1 |
| `mk64.z64` | `jit` | 78.51% | 99.5% | 7.50 | 8.05 | 3.54 | 0.75 / 4.19 | 47.9 | 45.0 |
| `mk64.z64` | `cached` | 83.12% | 100.0% | 7.43 | 7.97 | 3.57 | 0.72 / 5.06 | 47.8 | 46.1 |

Repeated windows varied substantially: earlier runs in the same session
reported OoT cached at 98.99% minimum and MK64 `jit` at 98.85% minimum,
while the final table's `jit` MK64 run fell to 78.51%. The rolling minimum
is therefore not a stable proof of sustained throughput. **No required
8-second `3×` / 120 Hz run established `>= 100%` sustained speed for both
ROMs.** Where JIT lowered CPU time (notably OoT), RSP/RDP and host pacing
still dominated. No guest logic or audio was duplicated to raise these
numbers. Windows was not tested on this host.

The presenter now requests three drawables and acquires them away from
Qt and the guest; packed VI frames stay leased through GPU completion.
Studio defaults to the auto scanline pool, small primitives run inline,
and row-local telemetry avoids one shared atomic per pixel. Fill rectangles
reuse their color-combiner result where no per-pixel noise is selected.
The cached CPU path also collapses an already decoded taken self-branch
and NOP delay slot within the existing device/Compare budget, counting every
guest instruction and cycle without redispatching the same pair.
These changes do not overlap the emulated CPU and RCP scheduler.
The probe's exit status checks frame delivery and the requested backend,
not the real-time threshold. Compare the reported minimum guest speed and
the underruns accumulated during the sample; the UI timer target is not
the compositor's measured refresh rate.

### Final 8-second exclusive Metal recheck (2026-11-24)

These Release Cocoa runs used `n64forge_tests --studio-bench "$ROM" 0 8
nearest 120 jit metal` after the hardware-latch RDP change and the final
RDRAM coherency-coalescing change. Both used the packed shared VI path on
the same Apple M3 Retina drawable. The initial single min/max callback range
for deferred RDP writes invalidated the whole span between color and depth
surfaces and regressed the sample to 86.16% on OoT; deduplicating callbacks
by touched 64-byte line restored the cost.

| ROM | Min guest speed | VI fields/s | Display FPS | CPU / RSP / RDP ms per field | GPU present avg / max ms | Audio underruns |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| `oot.z64` | **99.0655%** | 59.6374 | 62.5273 | 4.856 / 3.182 / 1.951 | 0.615 / 3.596 | 1 |
| `mk64.z64` | **97.8826%** | 49.2896 | 47.4681 | 7.097 / 7.597 / 2.942 | 0.513 / 3.989 | 4 |

`minSpeed` is the lowest smoothed one-second `CoreStats` sample, not a p95
or a hard real-time guarantee. OoT remained under 100%; MK64 was further
limited by CPU/RSP work while SP/DP were active. The UI timer stayed near
120 Hz, but accepted present counts were 62.5 and 47.5 FPS, so neither run
demonstrates 120 Hz display cadence. Presentation-only interpolation never
advances guest logic or audio; no frame was fabricated to improve these
numbers. Windows/D3D12 was not tested on this macOS host.

### Internal-resolution matrix with real scaled sources (2026-11-24)

Release Cocoa runs used `--studio-bench "$ROM" 0 8 nearest 120 jit metal <scale>`.
Unlike the earlier matrix, these runs report the actual presented source size:
`320×237` at 1×, `640×474` at 2× and `1280×948` at 4×. The 2×/4× paths reuse
per-buffer native sidecars from recent RDP color images and handle VI origins
inside those buffers; guest RDRAM, color/depth and readback remain native.

| ROM | Scale | Min guest speed | VI fields/s | Display FPS | CPU / RSP / RDP ms per field | GPU present avg / max ms | Audio underruns |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `oot.z64` | 1× | 98.7864% | 59.6724 | 62.3974 | 4.875 / 3.114 / 2.055 | 0.587 / 4.669 | 1 |
| `oot.z64` | 2× | 98.9405% | 59.6425 | 64.1245 | 4.693 / 3.048 / 2.693 | 0.483 / 4.207 | 1 |
| `oot.z64` | 4× | 92.7022% | 58.3452 | 63.4427 | 4.523 / 2.858 / 4.808 | 0.408 / 3.423 | 29 |
| `mk64.z64` | 1× | 98.2012% | 49.4739 | 47.6139 | 7.138 / 7.211 / 3.077 | 0.750 / 3.525 | 3 |
| `mk64.z64` | 2× | 92.6703% | 47.6139 | 45.8045 | 7.155 / 7.298 / 3.969 | 0.437 / 3.442 | 11 |
| `mk64.z64` | 4× | 83.0085% | 44.8034 | 43.0589 | 7.282 / 7.415 / 5.560 | 0.535 / 3.767 | 17 |

These are honest regressions at higher refinement: the extra cost is in the
RDP subpixel interior pass, not GPU presentation. 4× is not a real-time setting
on this host. The selector still permits it for deterministic inspection, and
no frame interpolation is used to hide the missed guest fields.

A short API probe on the same host reported `METAL` for the Metal selector and
`VULKAN (MOLTENVK)` for MoltenVK. KosmicKrisp remains selectable but fell back
to `SOFTWARE` because no compatible presentation driver was found; this is a
driver-dependent fallback, not evidence of a KosmicKrisp pass. Windows/D3D12
and MetalFX were not tested here.

### 2× / 3× verification after refine-loop hoisting (2026-11-24)

Same Release Cocoa command shape. This run includes the refine-loop change that
hoists the constant combiner/coverage slots out of the per-sample loop; it is a
neutral-to-small improvement, within run-to-run noise. OoT 3× was repeated three
times because its minimum-speed sample is sensitive to host scheduling.

| ROM | Scale | Min guest speed | VI fields/s | Display FPS | CPU / RSP / RDP ms per field | GPU present avg / max ms |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| `oot.z64` | 2× | 99.0354% | 59.6460 | 63.3692 | 4.624 / 2.997 / 2.922 | 0.529 / 3.009 |
| `mk64.z64` | 2× | 92.1685% | 47.7405 | 45.9743 | 7.042 / 7.266 / 4.005 | 0.544 / 3.393 |
| `oot.z64` | 3× | 92.8096–95.0673% | 59.31–59.71 | 60.36–63.26 | 4.54–4.59 / 2.85–2.92 / 3.71–4.30 | 0.45–0.55 / 4.25–4.73 |
| `mk64.z64` | 3× | 86.6872% | 45.9074 | 44.1140 | 7.260 / 7.405 / 4.908 | 0.391 / 2.450 |

**The 3× real-time target is not met on this host.** OoT is close but still
misses 100% in every sample; MK64's guest CPU and RSP together exceed
16.67 ms per field, so resolution refinement cannot recover the gap. The 2×
OoT run is effectively real time; MK64 at 2× remains around 92% because of
guest-side work, not GPU presentation (average present cost is below 0.6 ms).
No frame interpolation, skipped guest work, or fabricated performance number is
used to hide this; the `minSpeed` figure is the lowest smoothed sample, not a
p95 or a guaranteed cadence. Wider native instruction coverage and reduced
RSP/RDP cost remain open.

### ARM64 JIT memory and dispatch follow-up (2026-11-24)

Native blocks cache recently written guest registers until a transfer or
fallback. A physical-word table replaces the hot dispatch hash lookup; virtual
PCs still distinguish KSEG0 and KSEG1. Invalidating a block only visits edges
linked to that target. Direct native blocks survive TLB rewrites, and `CACHE`
only invalidates the affected direct RDRAM line. Native data stores publish the
line generation without a C++ call; a store to translated code goes through the
interpreter before the store can invalidate any active block. Native indirect
jumps return their exact virtual target, and a faultable non-linking delay slot
can fall back at the branch so the interpreter retains precise BD/EPC.

Temporary profiling of the earlier native implementation found roughly 731k
whole-cache flushes on OoT and 353k on MK64 in the 8-second probe. Scoping
`CACHE` brought those counts to 103 and 63 in the corresponding runs, and
reduced write-protection transitions from 3.36M and 958k to 336k and 202k.
Those counters included startup and are counts per wall-clock run, not
normalized per field. The repro command for the side-by-side probe is
`QT_QPA_PLATFORM=cocoa build/macos/Release/n64forge_tests --studio-bench
"$ROM" 0 8 nearest 120 <jit|cached> metal 3`.

Release Cocoa, eight seconds per mode, Fit Window, 120 Hz presentation target,
3× internal resolution, exclusive Metal:

| ROM | CPU ms/field, Cached → JIT | CPU reduction | Min guest speed, Cached → JIT |
| --- | ---: | ---: | ---: |
| `oot.z64` | 5.62 → 5.18 | 7.7% | 63.39% → 61.07% |
| `mk64.z64` | 8.17 → 7.59 | 7.1% | 71.83% → 76.92% |

Build/test activity overlapped this pass. An earlier lower-contention pass
also favored JIT CPU time (4.53 → 4.32 ms and 8.09 → 7.73 ms), with JIT
minimum guest speeds of 97.84% and 80.26%. The CPU gain is repeatable in
these samples; the whole-emulator speed floor is not. Neither ROM has a
verified sustained 100% floor at 3×. Both ROMs matched the scalar interpreter
over 200 VI fields, comparing PC, GPRs, HI/LO, COP0, FPRs, FCSR, cycle and
instruction counts, all RDRAM bytes, and the scanned-out framebuffer.

The separate hardware corpus had 26 guest PASS verdicts, 18 visual-only
results and one inconclusive result (`nemu64-test` had no completion banner
by 25M cycles). No guest FAIL or host timeout was reported; strict CTest
still fails on the inconclusive result.
