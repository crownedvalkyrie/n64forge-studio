#pragma once
#include "n64forge/core/types.hpp"

namespace n64forge {

// ---------------------------------------------------------------------------
// OSTask layout (libultra/OS.h) stored at DMEM 0x0FC0.
// All fields are big-endian 32-bit words.
// ---------------------------------------------------------------------------
struct OSTask {
    u32 type = 0;
    u32 flags = 0;
    u32 ucodeBoot = 0;
    u32 ucodeBootSize = 0;
    u32 ucode = 0;
    u32 ucodeSize = 0;
    u32 ucodeData = 0;
    u32 ucodeDataSize = 0;
    u32 dramStack = 0;
    u32 dramStackSize = 0;
    u32 outputBuff = 0;
    u32 outputBuffSize = 0;
    u32 dataPtr = 0;
    u32 dataSize = 0;
    u32 yieldDataPtr = 0;
    u32 yieldDataSize = 0;
};

[[nodiscard]] OSTask parseOSTask(const u8* dmem);

} // namespace n64forge
