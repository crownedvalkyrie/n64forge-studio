#pragma once
// High-level emulation (HLE) shims for the libultra operating system services.
// This is intentionally not a full OS implementation; it provides the minimal
// state and hooks used by the in-house CPU interpreter and the static
// recompiler runtime to avoid low-level libultra boot/game microcode.
#include "n64forge/core/types.hpp"

#include <array>
#include <cstddef>
#include <deque>
#include <unordered_map>
#include <vector>

namespace n64forge {

class Bus;

// gSPMatrix parameter bits (subset used by HLE graphics hooks).
inline constexpr u32 HLE_MTX_MODELVIEW  = 0x00;
inline constexpr u32 HLE_MTX_PROJECTION = 0x01;
inline constexpr u32 HLE_MTX_MUL        = 0x00;
inline constexpr u32 HLE_MTX_LOAD       = 0x02;
inline constexpr u32 HLE_MTX_PUSH       = 0x04;
inline constexpr u32 HLE_MTX_NOPUSH     = 0x00;

// Message-queue flags.
inline constexpr u32 HLE_MESG_NOBLOCK = 0x01;
inline constexpr u32 HLE_MESG_BLOCK   = 0x00;

struct HleThread {
    u32 id = 0;
    u32 pc = 0;
    u32 sp = 0;
    s32 priority = 0;
    u32 state = 0; // 0 = stopped, 1 = running, 2 = waiting
};

struct HleMesgQueue {
    u32 capacity = 0;
    std::deque<u32> messages;
};

struct HleTimer {
    u64 expireCount = 0;
    u32 queueAddr = 0;
    u32 message = 0;
};

struct HleViewport {
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
    float near = 0.0f;
    float far = 0.0f;
};

// ---------------------------------------------------------------------------
// libultra HLE service container.
// ---------------------------------------------------------------------------
class HleLibultra {
public:
    static constexpr std::size_t MatrixStackDepth = 16;

    HleLibultra();

    void reset();

    // Threads
    [[nodiscard]] u32 createThread(u32 entryPc, u32 sp, s32 priority, u32 flag);
    void startThread(u32 id);
    void stopThread(u32 id);
    void yieldThread();
    [[nodiscard]] const HleThread* currentThread() const noexcept;
    [[nodiscard]] const std::unordered_map<u32, HleThread>& threads() const noexcept { return threads_; }

    // Message queues
    void createMesgQueue(u32 queueAddr, u32 capacity);
    [[nodiscard]] bool sendMesg(u32 queueAddr, u32 message, u32 flags);
    [[nodiscard]] bool recvMesg(u32 queueAddr, u32& outMessage, u32 flags);

    // Timers
    void createTimer(u32 timerAddr, u64 expireCount, u32 queueAddr, u32 message);
    void tick(u64 now);

    // DMA (cartridge <-> RDRAM)
    void piDmaRead(u32 cartAddr, u32 rdramAddr, u32 length, Bus& bus);
    void piDmaWrite(u32 rdramAddr, u32 cartAddr, u32 length, Bus& bus);

    // Graphics HLE hooks (gSPMatrix / gSPViewport)
    void matrix(const float* m, u32 param);
    void viewport(const HleViewport& vp) noexcept;
    [[nodiscard]] const std::array<float, 16>& currentMatrix() const noexcept { return matrixStack_[matrixTop_]; }
    [[nodiscard]] const HleViewport& currentViewport() const noexcept { return viewport_; }

private:
    std::unordered_map<u32, HleThread> threads_;
    u32 nextThreadId_ = 1;
    u32 currentThreadId_ = 0;

    std::unordered_map<u32, HleMesgQueue> mesgQueues_;
    std::vector<HleTimer> timers_;

    std::array<std::array<float, 16>, MatrixStackDepth> matrixStack_{};
    std::size_t matrixTop_ = 0;
    HleViewport viewport_;

    static void identityMatrix(std::array<float, 16>& out) noexcept;
    static void multiplyMatrix(std::array<float, 16>& out, const std::array<float, 16>& a,
                               const std::array<float, 16>& b) noexcept;
};

} // namespace n64forge
