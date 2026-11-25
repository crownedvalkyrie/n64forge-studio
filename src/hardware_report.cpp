#include "hardware_report.hpp"
#include "n64forge/core/emulator.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <fstream>
#include <iostream>
#include <numeric>
#include <string_view>

namespace {
using namespace n64forge;

bool writeSnapshot(const Emulator& emulator, const char* path) {
    std::ofstream output(std::filesystem::path(path), std::ios::binary);
    output << "P6\n" << emulator.framebufferWidth() << ' ' << emulator.framebufferHeight() << "\n255\n";
    for (const u32 pixel : emulator.framebuffer()) {
        const char rgb[]{static_cast<char>(pixel), static_cast<char>(pixel >> 8),
                         static_cast<char>(pixel >> 16)};
        output.write(rgb, sizeof(rgb));
    }
    return static_cast<bool>(output);
}

bool parseRdpThreads(std::string_view value, unsigned& count) {
    if (value == "auto") {
        count = 0;
        return true;
    }
    if (value == "1" || value == "2" || value == "4" || value == "8") {
        count = static_cast<unsigned>(value.front() - '0');
        return true;
    }
    return false;
}
}

int runHardwareReport(int argc, char* argv[]) {
    if (argc != 4 && argc != 5) {
        std::cerr << "Usage: n64forge --hw-test-report <rom> <cycle-budget> <r30|visual> "
                     "<snapshot.ppm> [flashram]\n";
        return 2;
    }
    u64 budget = 0;
    const std::string_view cycles = argv[1], protocol = argv[2];
    const auto parsed = std::from_chars(cycles.data(), cycles.data() + cycles.size(), budget);
    if (parsed.ec != std::errc{} || parsed.ptr != cycles.data() + cycles.size() || !budget ||
        (protocol != "r30" && protocol != "visual")) {
        std::cerr << "Expected a positive cycle budget and r30 or visual protocol.\n";
        return 2;
    }

    Emulator emulator;
    const char* stop = "budget";
    try {
        if (!emulator.loadCartridge(argv[0])) stop = "load_error";
        else {
            emulator.bus().flash().attach(argc == 5 && std::string_view(argv[4]) == "flashram");
            for (u64 step = 0; step < budget; ++step) {
                emulator.runOneStep();
                if (protocol == "r30" && emulator.cpu().gpr()[30] != 0) {
                    stop = "r30";
                    break;
                }
            }
            emulator.scanout();
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        stop = "execution_error";
    }

    u64 nonBlack = 0;
    for (const auto pixel : emulator.framebuffer()) nonBlack += (pixel & 0xFFFFFF) != 0;
    bool snapshot = false;
    if (!emulator.framebuffer().empty()) {
        snapshot = writeSnapshot(emulator, argv[3]);
        if (!snapshot) std::cerr << "Could not write VI snapshot\n";
    }
    const auto& cpu = emulator.cpu();
    const auto& cop0 = cpu.cop0();
    const auto& trace = cpu.exceptionTrace();
    const auto rdp = emulator.rdp().stats();
    const auto counters = emulator.counters();
    const auto hex = [](u64 value) { std::cout << "\"0x" << std::hex << value << std::dec << '"'; };
    std::cout << "{\"stop\":\"" << stop << "\",\"cycles\":" << counters.cycles
              << ",\"instructions\":" << counters.instructions << ",\"pc\":";
    hex(cpu.pc());
    std::cout << ",\"r30\":" << static_cast<s64>(cpu.gpr()[30]) << ",\"cause\":";
    hex(cop0[13]);
    std::cout << ",\"epc\":";
    hex(cop0[14]);
    std::cout << ",\"bad_vaddr\":";
    hex(cop0[8]);
    std::cout << ",\"exceptions\":" << std::accumulate(trace.counts.begin(), trace.counts.end(), u64{0})
              << ",\"exception_counts\":[";
    for (unsigned i = 0; i < trace.counts.size(); ++i) std::cout << (i ? "," : "") << trace.counts[i];
    std::cout << "],\"first_exception\":";
    if (trace.observed) {
        std::cout << "{\"code\":" << trace.firstCode << ",\"pc\":";
        hex(trace.firstPc);
        std::cout << "},\"last_exception\":{\"code\":" << trace.lastCode << ",\"pc\":";
        hex(trace.lastPc);
        std::cout << '}';
    } else std::cout << "null,\"last_exception\":null";
    std::cout << ",\"rsp_launches\":" << emulator.rsp().taskLaunches()
              << ",\"rsp_pc\":" << emulator.rsp().pc()
              << ",\"sp_status\":" << emulator.bus().hw().sp().status
              << ",\"rdp_commands\":" << rdp.commandsProcessed << ",\"rdp_syncs\":" << rdp.fullSyncs
              << ",\"vi_fields\":" << counters.viFields << ",\"non_black\":" << nonBlack
              << ",\"snapshot\":" << (snapshot ? "true" : "false") << "}\n";
    return 0;
}

int runStallProbe(int argc, char* argv[]) {
    if (argc < 2) return 2;
    u32 budget = 0;
    const std::string_view slices = argv[1];
    const auto parsed = std::from_chars(slices.data(), slices.data() + slices.size(), budget);
    if (parsed.ec != std::errc{} || parsed.ptr != slices.data() + slices.size() || !budget) return 2;

    unsigned rdpThreads = 0;
    const char* mediaArg = nullptr;
    const char* snapshotArg = nullptr;
    for (int i = 2; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        if (arg == "--rdp-threads") {
            if (i + 1 >= argc || !parseRdpThreads(argv[++i], rdpThreads)) {
                std::cerr << "--rdp-threads expects auto, 1, 2, 4, or 8\n";
                return 2;
            }
        } else if (arg.starts_with("--")) {
            if (mediaArg) return 2;
            mediaArg = argv[i];
        } else {
            if (snapshotArg) return 2;
            snapshotArg = argv[i];
        }
    }

    Emulator emulator;
    emulator.rdp().setThreadCount(rdpThreads);
    std::cout << "RDP threads: " << emulator.rdp().effectiveThreadCount()
              << (rdpThreads == 0 ? " (auto)" : " (explicit)") << '\n';
    if (!emulator.loadCartridge(argv[0])) return 1;
    auto& bus = emulator.bus();
    using Media = Bus::PiSaveMedia;
    if (mediaArg) {
        const std::string_view media = mediaArg;
        if (media == "--flashram") {
            bus.flash().attach(true);
            bus.setPiSaveMedia(Media::FlashRam);
        } else if (media == "--sram") {
            bus.sram().attach(true);
            bus.setPiSaveMedia(Media::Sram);
        } else if (media == "--eeprom4k") bus.eeprom().attach(Eeprom::Type::Eeprom4K);
        else if (media == "--eeprom16k") bus.eeprom().attach(Eeprom::Type::Eeprom16K);
        else return 2;
    }
    bus.setIsViewerLogCallback([](std::string_view text) { std::cout << "IS-Viewer: " << text << '\n'; });
    const auto& cpu = emulator.cpu();
    auto& hw = bus.hw();
    std::cout << "CIC: " << cicName(emulator.cicProfile().variant) << '\n';
    for (u32 slice = 0; slice < budget; ++slice) {
        u16 buttons = 0;
        const u64 field = hw.videoFields();
        // Ordinary port-1 Joybus input, not injected guest calls or PC hooks.
        // Alternate Start/A after two seconds of NTSC-rate fields.
        if (field >= 120 && (field - 120) % 45 < 6) {
            buttons = static_cast<u16>(((field - 120) / 45) & 1
                                      ? input::N64Button::A : input::N64Button::Start);
        }
        hw.setController(0, {buttons, 0, 0}, true);
        emulator.runVideoFrame(false);
        if ((slice + 1) % 60 && slice + 1 != budget) continue;
        std::array<u64, 128> pcs{};
        for (auto& pc : pcs) {
            pc = cpu.pc();
            emulator.runOneStep();
        }
        std::sort(pcs.begin(), pcs.end());
        const auto lastPc = pcs.back();
        const auto unique = std::unique(pcs.begin(), pcs.end()) - pcs.begin();
        emulator.scanout();
        const auto& cop0 = cpu.cop0();
        const auto& trace = cpu.exceptionTrace();
        const auto stats = emulator.rdp().stats();
        u64 nonBlack = 0, fingerprint = 14695981039346656037ull;
        for (const u32 pixel : emulator.framebuffer()) {
            nonBlack += (pixel & 0xFFFFFF) != 0;
            fingerprint = (fingerprint ^ pixel) * 1099511628211ull;
        }
        std::cout << "slice=" << slice + 1 << " vi=" << hw.videoFields()
                  << " pixels=" << nonBlack << " syncs=" << stats.fullSyncs
                  << " sp_launches=" << emulator.rsp().taskLaunches()
                  << " tris=" << stats.triangleCommands
                  << " tex=" << stats.texturedTriangles
                  << " shade=" << stats.shadedTriangles << std::hex
                  << " rgba=" << fingerprint << " pc=" << cpu.pc()
                  << " ra=" << cpu.gpr()[31] << " sp=" << cpu.gpr()[29]
                  << " pc_range=[" << pcs.front() << ',' << lastPc << ']' << std::dec
                  << " unique_pc=" << unique << std::hex
                  << " status=" << cop0[12] << " cause=" << cop0[13] << " epc=" << cop0[14]
                  << " badvaddr=" << cop0[8] << " entryhi=" << cop0[10]
                  << " mi=" << hw.mi().interrupt << '/' << hw.mi().mask
                  << " sp_status=" << hw.sp().status << " dp_status=" << hw.dp().status
                  << " pi_status=" << hw.read(0x04600010)
                  << " si_status=" << hw.read(0x04800018) << std::dec << " exceptions=";
        for (u32 code = 0; code < trace.counts.size(); ++code)
            if (trace.counts[code]) {
                const auto pc = trace.lastPcByCode[code];
                std::cout << code << ':' << trace.counts[code] << '@' << std::hex << pc;
                // Inspect only direct RDRAM: never issue a diagnostic read
                // through a device or retake a TLB exception while logging.
                const u32 low = static_cast<u32>(pc);
                const u32 phys = low & 0x1FFFFFFF;
                if (code && low >= 0x80000000 && low < 0xC0000000 && phys <= Memory::RdramSize - 4)
                    std::cout << '/' << bus.rdram().loadRdram<u32>(phys);
                std::cout << std::dec << ',';
            }
        if (trace.observed) std::cout << " first=" << trace.firstCode << '@' << std::hex << trace.firstPc
                                      << std::dec << " last=" << trace.lastCode << '@' << std::hex << trace.lastPc;
        std::cout << std::dec << '\n';
    }
    if (snapshotArg) {
        emulator.scanout();
        if (!writeSnapshot(emulator, snapshotArg)) {
            std::cerr << "Could not write VI snapshot\n";
            return 1;
        }
    }
    return 0;
}
