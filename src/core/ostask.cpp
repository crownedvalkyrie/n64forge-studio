#include "n64forge/core/ostask.hpp"

namespace n64forge {

namespace {

constexpr u32 OSTASK_BASE = 0x0FC0;

[[nodiscard]] u32 readU32Be(const u8* p) {
    return (static_cast<u32>(p[0]) << 24) |
           (static_cast<u32>(p[1]) << 16) |
           (static_cast<u32>(p[2]) << 8) |
           static_cast<u32>(p[3]);
}

} // namespace

OSTask parseOSTask(const u8* dmem) {
    OSTask t;
    const u32 base = OSTASK_BASE;
    t.type = readU32Be(dmem + base + 0x00);
    t.flags = readU32Be(dmem + base + 0x04);
    t.ucodeBoot = readU32Be(dmem + base + 0x08);
    t.ucodeBootSize = readU32Be(dmem + base + 0x0C);
    t.ucode = readU32Be(dmem + base + 0x10);
    t.ucodeSize = readU32Be(dmem + base + 0x14);
    t.ucodeData = readU32Be(dmem + base + 0x18);
    t.ucodeDataSize = readU32Be(dmem + base + 0x1C);
    t.dramStack = readU32Be(dmem + base + 0x20);
    t.dramStackSize = readU32Be(dmem + base + 0x24);
    t.outputBuff = readU32Be(dmem + base + 0x28);
    t.outputBuffSize = readU32Be(dmem + base + 0x2C);
    t.dataPtr = readU32Be(dmem + base + 0x30);
    t.dataSize = readU32Be(dmem + base + 0x34);
    t.yieldDataPtr = readU32Be(dmem + base + 0x38);
    t.yieldDataSize = readU32Be(dmem + base + 0x3C);
    return t;
}

} // namespace n64forge
