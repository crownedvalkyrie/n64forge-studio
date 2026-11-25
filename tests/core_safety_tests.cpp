// Deterministic regressions for the audited CPU, memory and JIT boundaries.

#include "n64forge/core/bus.hpp"
#include "n64forge/core/cpu.hpp"
#include "n64forge/core/memory.hpp"
#include "n64forge/core/wide_product.hpp"

#include <cassert>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

using namespace n64forge;

// Defined in src/core/jit_arm64.cpp on every host (the native JIT itself is
// still guarded by the Apple arm64 build check).
namespace n64forge {
[[nodiscard]] u32 jitTranslationWindow(u32 pc) noexcept;
}

namespace {

constexpr u64 Kseg0 = 0xFFFFFFFF80000000ULL;
constexpr u64 ExceptionVector = Kseg0 + 0x180;

[[nodiscard]] u32 encodeR(u32 funct, u32 rs, u32 rt, u32 rd, u32 shamt = 0) {
    return (rs << 21) | (rt << 16) | (rd << 11) | (shamt << 6) | funct;
}
[[nodiscard]] u32 encodeI(u32 op, u32 rs, u32 rt, s16 imm) {
    return (op << 26) | (rs << 21) | (rt << 16) | static_cast<u16>(imm);
}
[[nodiscard]] u32 lui(u32 rt, u16 imm) { return encodeI(0x0F, 0, rt, static_cast<s16>(imm)); }
[[nodiscard]] u32 ori(u32 rt, u32 rs, u16 imm) { return encodeI(0x0D, rs, rt, static_cast<s16>(imm)); }
[[nodiscard]] u32 addiu(u32 rt, u32 rs, s16 imm) { return encodeI(0x09, rs, rt, imm); }
[[nodiscard]] u32 daddiu(u32 rt, u32 rs, s16 imm) { return encodeI(0x19, rs, rt, imm); }
[[nodiscard]] u32 dsll32(u32 rd, u32 rt, u32 shamt) { return encodeR(0x3C, 0, rt, rd, shamt); }
[[nodiscard]] u32 add(u32 rd, u32 rs, u32 rt) { return encodeR(0x20, rs, rt, rd); }
[[nodiscard]] u32 dadd(u32 rd, u32 rs, u32 rt) { return encodeR(0x2C, rs, rt, rd); }
[[nodiscard]] u32 encodeCop1(u32 rs, u32 rt, u32 fdField, u32 funct = 0) {
    return (0x11u << 26) | (rs << 21) | (rt << 16) | (fdField << 6) | funct;
}
[[nodiscard]] u32 mtc1(u32 rt, u32 fs) { return encodeCop1(0x04, rt, fs << 5); }
[[nodiscard]] u32 dmtc1(u32 rt, u32 fs) { return encodeCop1(0x05, rt, fs << 5); }
[[nodiscard]] u32 cvtLS(u32 fd, u32 fs) { return encodeCop1(0x10, 0, (fs << 5) | fd, 0x25); }
[[nodiscard]] u32 cvtLD(u32 fd, u32 fs) { return encodeCop1(0x11, 0, (fs << 5) | fd, 0x25); }

void writeProgram(Bus& bus, u32 physAddr, const std::vector<u32>& code) {
    for (std::size_t i = 0; i < code.size(); ++i)
        bus.write32(physAddr + static_cast<u32>(i) * 4, code[i]);
}

void checkSyncAndDelaySlot() {
    constexpr u64 start = Kseg0 + 0x200;
    for (const auto mode : {CpuExecutionMode::Interpreter, CpuExecutionMode::Cached,
                            CpuExecutionMode::Jit}) {
        Bus bus;
        Cpu cpu;
        cpu.setPc(start);
        if (mode == CpuExecutionMode::Jit) static_cast<void>(cpu.setJitEnabled(true));
        writeProgram(bus, 0x200, {
            addiu(1, 0, 7),
            0x0000000F, // SYNC is ordered, not a reserved instruction
            encodeI(0x04, 0, 1, 3), // untaken BEQ
            0x0000000D, // BREAK in its delay slot
        });
        const auto step = [&] {
            if (mode == CpuExecutionMode::Interpreter) cpu.step(bus);
            else cpu.stepCached(bus);
        };
        if (mode == CpuExecutionMode::Interpreter) step();
        else assert(cpu.runQuietBlock(bus, 8) == 1);
        assert(cpu.gpr()[1] == 7 && cpu.pc() == start + 4);
        step();
        assert(cpu.pc() == start + 8 && cpu.exceptionTrace().counts[10] == 0);
        if (mode == CpuExecutionMode::Interpreter) step();
        else assert(cpu.runQuietBlock(bus, 8) == 1);
        assert(cpu.pc() == start + 12);
        step();
        assert(cpu.pc() == ExceptionVector && cpu.exceptionTrace().counts[9] == 1);
        assert(cpu.cop0()[14] == start + 8 && (cpu.cop0()[13] & (1ULL << 31)));
    }
}

void checkWideMultiply() {
    constexpr u64 max = ~u64{0}, minSigned = u64{1} << 63;
    static_assert(multiplyWideUnsigned(max, max).hi == max - 1);
    static_assert(multiplyWideUnsigned(max, max).lo == 1);
    static_assert(multiplyWideSigned(minSigned, minSigned).hi == (u64{1} << 62));
    static_assert(multiplyWideSigned(minSigned, max).hi == 0);
    static_assert(multiplyWideSigned(minSigned, max).lo == minSigned);
    Bus bus;
    Cpu cpu;
    cpu.setPc(Kseg0 + 0x200);
    writeProgram(bus, 0x200, {
        daddiu(1, 0, -1), daddiu(2, 0, -1),
        encodeR(0x1C, 1, 2, 0), encodeR(0x10, 0, 0, 3), encodeR(0x12, 0, 0, 4),
        encodeR(0x1D, 1, 2, 0), encodeR(0x10, 0, 0, 5), encodeR(0x12, 0, 0, 6),
    });
    for (int i = 0; i < 8; ++i) cpu.stepCached(bus);
    assert(cpu.gpr()[3] == 0 && cpu.gpr()[4] == 1);
    assert(cpu.gpr()[5] == max - 1 && cpu.gpr()[6] == 1);
}

template<class F> void expectOutOfRange(F&& action) {
    bool threw = false;
    try {
        action();
    } catch (const std::out_of_range&) {
        threw = true;
    }
    assert(threw);
}

void checkAddOverflow(bool cached) {
    Bus bus;
    Cpu cpu;
    cpu.setPc(Kseg0 + 0x200);
    writeProgram(bus, 0x200, {
        lui(1, 0x7FFF), ori(1, 1, 0xFFFF), // INT32_MAX
        addiu(2, 0, 1),
        add(3, 1, 2),                      // traps: INT32_MAX + 1
    });
    for (int i = 0; i < 4; ++i) {
        if (cached) cpu.stepCached(bus);
        else cpu.step(bus);
    }
    assert(cpu.exceptionTrace().counts[12] == 1);
    assert(((cpu.cop0()[13] >> 2) & 31) == 12);
    assert(cpu.gpr()[3] == 0 && cpu.pc() == ExceptionVector);
}

void checkDaddOverflow(bool cached) {
    Bus bus;
    Cpu cpu;
    cpu.setPc(Kseg0 + 0x200);
    writeProgram(bus, 0x200, {
        lui(1, 0x8000), dsll32(1, 1, 0), daddiu(1, 1, -1), // INT64_MAX
        addiu(2, 0, 1),
        dadd(3, 1, 2),                     // traps: INT64_MAX + 1
    });
    for (int i = 0; i < 5; ++i) {
        if (cached) cpu.stepCached(bus);
        else cpu.step(bus);
    }
    assert(cpu.exceptionTrace().counts[12] == 1);
    assert(((cpu.cop0()[13] >> 2) & 31) == 12);
    assert(cpu.gpr()[3] == 0 && cpu.pc() == ExceptionVector);
}

void checkCvtLongInvalid(u32 fmt, u32 fd, const std::vector<u32>& setup) {
    Bus bus;
    Cpu cpu;
    cpu.setPc(Kseg0 + 0x400);
    cpu.setIplHandoffRegisters(1, 1, 0, 0); // CU1 and FR enabled
    auto program = setup;
    program.push_back(fmt == 0x10 ? cvtLS(fd, 2) : cvtLD(fd, 2));
    writeProgram(bus, 0x400, program);
    for (std::size_t i = 0; i < program.size(); ++i) cpu.step(bus);

    constexpr u64 IndefiniteInteger = 0x8000000000000000ULL;
    assert(cpu.fpr()[fd] == IndefiniteInteger);
    assert((cpu.fcr31() & 0x10000) != 0); // FCSR cause: invalid operation
    assert((cpu.fcr31() & 0x40) != 0);    // sticky invalid flag
    assert(cpu.exceptionTrace().counts[15] == 1);
    assert(((cpu.cop0()[13] >> 2) & 31) == 15);
    assert(cpu.pc() == ExceptionVector);
}

} // namespace

int run_core_safety_tests() {
    checkSyncAndDelaySlot();
    checkWideMultiply();
    // 1. Overflowing ADD / DADD trap in both the legacy and decoded paths.
    checkAddOverflow(false);
    checkAddOverflow(true);
    checkDaddOverflow(false);
    checkDaddOverflow(true);

    // 2. A 16-bit access at the last 32-bit address must not wrap to zero.
    Memory memory;
    expectOutOfRange([&] { memory.write16(0xFFFFFFFFu, 0x1234); });
    Bus bus;
    expectOutOfRange([&] { bus.write16(0xFFFFFFFFu, 0x1234); });

    // 3. Multi-byte accesses must fit wholly inside RDRAM, checked in size_t.
    const u32 size = Memory::RdramSize;
    expectOutOfRange([&] { memory.read16(size - 1); });
    expectOutOfRange([&] { memory.write16(size - 1, 0); });
    expectOutOfRange([&] { memory.read32(size - 1); });
    expectOutOfRange([&] { memory.read32(size - 3); });
    expectOutOfRange([&] { memory.write32(size - 3, 0); });
    expectOutOfRange([&] { memory.read64(size - 1); });
    expectOutOfRange([&] { memory.read64(size - 7); });
    expectOutOfRange([&] { memory.write64(size - 7, 0); });
    expectOutOfRange([&] { memory.read32(0xFFFFFFFFu); });
    expectOutOfRange([&] { memory.write64(0xFFFFFFFFu, 0); });
    memory.write16(size - 2, 0xABCD);
    assert(memory.read16(size - 2) == 0xABCD);
    memory.write32(size - 4, 0x11223344);
    assert(memory.read32(size - 4) == 0x11223344);
    memory.write64(size - 8, 0x0102030405060708ULL);
    assert(memory.read64(size - 8) == 0x0102030405060708ULL);

    // 4. The JIT translation window stops at the RDRAM end instead of
    //    fetching unchecked pc + 4 * steps words.
    assert(jitTranslationWindow(0) == 16);
    assert(jitTranslationWindow(0x1000) == 16);
    assert(jitTranslationWindow(size - 64) == 16);
    assert(jitTranslationWindow(size - 8) == 2);
    assert(jitTranslationWindow(size - 4) == 1);
    assert(jitTranslationWindow(size) == 0);
    assert(jitTranslationWindow(size + 4) == 0);

    // 5. NaN / infinity CVT.L must raise FPU invalid and produce one
    //    deterministic destination pattern rather than converting UB.
    checkCvtLongInvalid(0x10, 3, {lui(1, 0x7FC0), ori(1, 1, 0), mtc1(1, 2)});
    checkCvtLongInvalid(0x11, 4, {lui(1, 0x7FF0), dsll32(1, 1, 0), dmtc1(1, 2)});

    std::cout << "core safety boundary tests passed\n";
    return 0;
}
