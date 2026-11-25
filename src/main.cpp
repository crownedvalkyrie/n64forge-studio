#include "n64forge/core/emulator.hpp"
#include "n64forge/video/graphics_api.hpp"
#include "n64forge/recomp/analyzer.hpp"
#include "n64forge/video/renderer.hpp"
#include "hardware_report.hpp"
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <iomanip>
#include <stdexcept>
#include <string_view>

namespace {
using Clock = std::chrono::steady_clock;
n64forge::u64 nowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
}

void printStats(const n64forge::CoreStats& s) {
    const auto& c = s.counters;
    std::cout << std::fixed << std::setprecision(2)
              << "Stats: " << s.speed << "% | CPU " << s.mips << " MIPS / " << s.mhz
              << " MHz | VI " << s.viFps << "/s | Display N/A | Cache ";
    if (s.hasCacheRate) std::cout << s.cacheHit << '%';
    else std::cout << "N/A (inactive)";
    std::cout << " | Field " << s.frameMs << " ms (CPU ~" << s.cpuMs << ", RSP ~" << s.rspMs
              << ", RDP " << s.rdpMs << ", sleep " << s.sleepMs << ", other " << s.otherMs << ')'
              << " | AI " << c.dacHz << " Hz, host FIFO N/A"
              << " | RSP/s GFX " << s.gfxPerSec << ", AUD " << s.audioPerSec << ", OTHER " << s.otherPerSec
              << " | RDP triangles/s " << s.trianglesPerSec << " | RDP " << c.rdpScale << "x "
              << c.rdpWidth << 'x' << c.rdpHeight << " | VI " << c.viWidth << 'x' << c.viHeight
              << '\n' << std::defaultfloat << std::flush;
}
}

int main(int argc, char* argv[]) {
    namespace video = n64forge::video;
    const auto graphicsOption = [&](int i) {
        auto api = i < argc ? video::parseGraphicsApi(argv[i]) : std::nullopt;
        if (!api) std::cerr << "--graphics-api expects auto, metal, moltenvk, kosmickrisp, software, d3d12, or vulkan\n";
        return api;
    };
    const auto describeGraphics = [](video::GraphicsApi api) {
        const auto info = video::probeGraphicsApi(api);
        std::cout << "Graphics requested: " << video::graphicsApiName(api)
                  << "; viewport candidate: " << video::graphicsApiName(info.fallback)
                  << " (" << info.detail << "); CLI remains headless\n";
    };
    if (argc > 1 && std::strcmp(argv[1], "--graphics-api") == 0) {
        const auto api = graphicsOption(2);
        if (!api || argc != 3) return 2;
        describeGraphics(*api);
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "--hw-test-report") == 0) {
        return runHardwareReport(argc - 2, argv + 2);
    }
    if (argc > 1 && std::strcmp(argv[1], "--stall-probe") == 0) {
        return runStallProbe(argc - 2, argv + 2);
    }
    if (argc > 1 && std::strcmp(argv[1], "analyze") == 0) {
        return n64forge::recomp::runAnalyzeCommand(argc - 2, argv + 2);
    }
    if (argc > 1 && std::strcmp(argv[1], "recompile") == 0) {
        return n64forge::recomp::runRecompileCommand(argc - 2, argv + 2);
    }
    n64forge::Emulator emulator;

    if (argc > 1) {
        const char* romPath = argv[1];
        int frames = 0;
        int steps = 100;
        bool showStats = false;
        bool flashRam = false;
        bool sram = false;
        int eepromKbit = 0;
        bool isViewer = false;
        auto cpuMode = n64forge::defaultDesktopCpuMode();
        unsigned int internalResolution = 1;
        unsigned int rdpThreads = 0; // 0 = auto
        for (int i = 2; i < argc; ++i) {
            if (std::strcmp(argv[i], "--cpu-mode") == 0) {
                const auto mode = i + 1 < argc ? n64forge::parseCpuMode(argv[++i]) : std::nullopt;
                if (!mode) {
                    std::cerr << "--cpu-mode expects jit, cached, or interpreter\n";
                    return 2;
                }
                cpuMode = *mode;
            } else if (std::strcmp(argv[i], "--graphics-api") == 0) {
                const auto api = graphicsOption(++i);
                if (!api) return 2;
                describeGraphics(*api);
            } else if (std::strcmp(argv[i], "--stats") == 0) {
                showStats = true;
            } else if (std::strcmp(argv[i], "--flashram") == 0) {
                flashRam = true;
            } else if (std::strcmp(argv[i], "--sram") == 0) {
                sram = true;
            } else if (std::strcmp(argv[i], "--eeprom4k") == 0) {
                eepromKbit = 4;
            } else if (std::strcmp(argv[i], "--eeprom16k") == 0) {
                eepromKbit = 16;
            } else if (std::strcmp(argv[i], "--isviewer") == 0) {
                isViewer = true;
            } else if (std::strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
                frames = std::atoi(argv[i + 1]);
                ++i;
            } else if (std::strcmp(argv[i], "--steps") == 0 && i + 1 < argc) {
                steps = std::atoi(argv[i + 1]);
                ++i;
            } else if (std::strcmp(argv[i], "--internal-scale") == 0 ||
                       std::strcmp(argv[i], "--internal-resolution") == 0) {
                if (i + 1 >= argc || std::strlen(argv[i + 1]) != 1 ||
                    argv[i + 1][0] < '1' || argv[i + 1][0] > '4') {
                    std::cerr << "--internal-scale expects 1, 2, 3, or 4\n";
                    return 2;
                }
                internalResolution = static_cast<unsigned>(argv[++i][0] - '0');
            } else if (std::strcmp(argv[i], "--rdp-threads") == 0 && i + 1 < argc) {
                const std::string_view value(argv[++i]);
                if (value == "auto") {
                    rdpThreads = 0;
                } else if (value == "1" || value == "2" || value == "4" || value == "8") {
                    rdpThreads = static_cast<unsigned int>(value.front() - '0');
                } else {
                    std::cerr << "--rdp-threads expects auto, 1, 2, 4, or 8\n";
                    return 2;
                }
            }
        }
        emulator.setInternalResolution(internalResolution);
        emulator.rdp().setThreadCount(rdpThreads);
        std::cout << "RDP threads: " << emulator.rdp().effectiveThreadCount()
                  << (rdpThreads == 0 ? " (auto)" : " (explicit)") << '\n';
        if (emulator.setCpuExecutionMode(cpuMode) != cpuMode)
            std::cerr << "ARM64 JIT unavailable; using cached interpreter\n";

        std::cout << "Loading ROM: " << romPath << '\n';
        if (!emulator.loadCartridge(romPath)) {
            std::cerr << "Failed to load ROM.\n";
            return 1;
        }
        emulator.bus().flash().attach(flashRam);
        emulator.bus().sram().attach(sram);
        if (flashRam) emulator.bus().setPiSaveMedia(n64forge::Bus::PiSaveMedia::FlashRam);
        else if (sram) emulator.bus().setPiSaveMedia(n64forge::Bus::PiSaveMedia::Sram);
        if (eepromKbit == 4)
            emulator.bus().eeprom().attach(n64forge::Eeprom::Type::Eeprom4K);
        else if (eepromKbit == 16)
            emulator.bus().eeprom().attach(n64forge::Eeprom::Type::Eeprom16K);
        if (isViewer)
            emulator.bus().setIsViewerLogCallback([](std::string_view line) {
                std::cout << "IS-Viewer: " << line << '\n';
            });
        const auto& header = emulator.cartridge().header();
        std::cout << "ROM loaded: " << header.name << '\n';
        std::cout << "Size: " << emulator.cartridge().size() << " bytes\n";
        std::cout << "Format: " << emulator.cartridge().formatName() << '\n';
        std::cout << "Boot PC: 0x" << std::hex << header.bootPc << std::dec << '\n';
        std::cout << "CIC: " << n64forge::cicName(emulator.cicProfile().variant)
                  << " (osCicId " << emulator.bus().read32(0x310) << ")\n";

        std::cout << "Running " << steps << " CPU steps (emulation is partial):\n";
        try {
            n64forge::CoreStatsSampler sampler;
            if (showStats) {
                emulator.setTimingEnabled(true);
                sampler.reset(nowNs(), emulator.counters());
            }
            const auto report = [&] {
                if (showStats && sampler.update(nowNs(), emulator.counters())) printStats(sampler.snapshot());
            };
            for (int i = 0; i < steps; ++i) {
                emulator.runOneStep();
                if (showStats && (i & 0xFFFF) == 0xFFFF) report();
            }
            report();
            std::cout << "CPU PC after steps: 0x" << std::hex << emulator.cpu().pc() << std::dec << '\n';

            if (frames > 0) {
                std::cout << "Running " << frames << " diagnostic batches (100,000 steps each)...\n";
                for (int f = 0; f < frames; ++f) {
                    emulator.runFrame();
                    report();
                }
                const auto& rdpStats = emulator.rdp().stats();
                std::cout << "CPU PC after frames: 0x" << std::hex << emulator.cpu().pc() << std::dec << '\n';
                std::cout << "SP status: 0x" << std::hex << emulator.bus().hw().sp().status << std::dec << '\n';
                std::cout << "OSTask type in DMEM: " << emulator.rsp().readMem32(0xFC0) << '\n';
                std::cout << "RSP PC: 0x" << std::hex << emulator.rsp().pc() << std::dec << '\n';
                const auto& trace = emulator.rsp().taskTrace();
                for (std::size_t type = 1; type < trace.size(); ++type) {
                    std::cout << "OSTask " << type << ": " << trace[type].launches << " launches, "
                              << trace[type].breaks << " BREAKs, " << trace[type].doneAtBreak
                              << " BREAKs with SIG2\n";
                }
                std::cout << "RDP commands processed: " << rdpStats.commandsProcessed << '\n';
                std::cout << "RDP fill rectangles: " << rdpStats.fillRectangles << '\n';
                std::cout << "RDP triangles rasterized/submitted: " << rdpStats.triangles
                          << "/" << rdpStats.triangleCommands << '\n';
                std::cout << "RDP pixels drawn: " << rdpStats.pixelsDrawn << '\n';
                std::cout << "RDP full syncs: " << rdpStats.fullSyncs << '\n';
                const auto& hw = emulator.bus().hw();
                std::cout << "VI_ORIGIN: 0x" << std::hex << hw.vi().origin
                          << ", VI_CONTROL: 0x" << hw.vi().status << ", MI pending/mask: 0x"
                          << hw.mi().interrupt << "/0x" << hw.mi().mask << std::dec << '\n';
                std::cout << "VI fields: " << hw.videoFields() << '\n';
                if (cpuMode == n64forge::CpuExecutionMode::Jit)
                    std::cout << "Native JIT steps: " << emulator.cpu().jitSteps() << '\n';
                std::size_t audioFrames = 0;
                const auto* audio = emulator.audioSamples(audioFrames);
                int peak = 0;
                for (std::size_t i = 0; i < audioFrames * 2; ++i)
                    peak = std::max(peak, std::abs(static_cast<int>(audio[i])));
                std::cout << "AI samples drained/nonzero: " << hw.ai().samplesDrained << "/"
                          << hw.ai().nonZeroSamples << ", last batch peak " << peak << '\n';
                const auto& fb = emulator.framebuffer();
                std::uint32_t nonBlack = 0;
                for (std::uint32_t pixel : fb) {
                    if ((pixel & 0xFFFFFF) != 0) ++nonBlack;
                }
                std::cout << "Framebuffer non-black pixels: " << nonBlack << " / " << fb.size() << '\n';
            }
            if (showStats && !sampler.snapshot().ready)
                std::cout << "Stats: run completed before the first one-second sample.\n";
        } catch (const std::exception& e) {
            std::cerr << "Execution stopped: " << e.what() << '\n';
            std::cerr << "PC: 0x" << std::hex << emulator.cpu().pc() << std::dec << '\n';
            return 2;
        }
        return 0;
    }

    auto renderer = n64forge::createRenderer();
    std::cout << "       n64forge --graphics-api auto|metal|moltenvk|kosmickrisp|software|d3d12|vulkan (probe)\n";
    std::cout << "N64Forge 0.1.0\n";
    std::cout << "Usage: " << (argc > 0 ? argv[0] : "n64forge") << " <rom.z64|rom.n64|rom.v64|rom.zip> [--cpu-mode jit|cached|interpreter] [--steps N] [--frames N] [--internal-scale 1|2|3|4] [--rdp-threads auto|1|2|4|8] [--flashram|--sram] [--eeprom4k|--eeprom16k] [--isviewer] [--stats]\n";
    std::cout << "       n64forge --stall-probe <rom> <field-slices> [--rdp-threads auto|1|2|4|8] [--flashram|--sram|--eeprom4k|--eeprom16k] [snapshot.ppm]\n";
    std::cout << "Hardware diagnostics: n64forge --hw-test-report <rom> <cycle-budget> <r30|visual> <snapshot.ppm>\n";
    std::cout << "CPU reset PC: 0x" << std::hex << emulator.cpu().pc() << '\n';
    std::cout << "Renderer: " << (renderer ? renderer->backendName() : "not available") << '\n';
    std::cout << "Emulation core is partially implemented; pass a ROM path to exercise it.\n";
    return 0;
}
