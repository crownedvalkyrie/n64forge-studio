#include "n64forge/core/hle_libultra.hpp"
#include "n64forge/core/bus.hpp"
#include <algorithm>
#include <cstring>
#include <limits>

namespace n64forge {

HleLibultra::HleLibultra() {
    reset();
}

void HleLibultra::reset() {
    threads_.clear();
    nextThreadId_ = 1;
    currentThreadId_ = 0;
    mesgQueues_.clear();
    timers_.clear();
    matrixTop_ = 0;
    for (auto& m : matrixStack_) identityMatrix(m);
    viewport_ = HleViewport{};
}

u32 HleLibultra::createThread(u32 entryPc, u32 sp, s32 priority, u32 /*flag*/) {
    const u32 id = nextThreadId_++;
    HleThread t;
    t.id = id;
    t.pc = entryPc;
    t.sp = sp;
    t.priority = priority;
    t.state = 0;
    threads_.emplace(id, t);
    return id;
}

void HleLibultra::startThread(u32 id) {
    auto it = threads_.find(id);
    if (it == threads_.end()) return;
    it->second.state = 1;
    currentThreadId_ = id;
}

void HleLibultra::stopThread(u32 id) {
    auto it = threads_.find(id);
    if (it == threads_.end()) return;
    it->second.state = 0;
    if (currentThreadId_ == id) {
        currentThreadId_ = 0;
        yieldThread();
    }
}

void HleLibultra::yieldThread() {
    s32 bestPri = std::numeric_limits<s32>::min();
    u32 bestId = 0;
    for (const auto& [id, t] : threads_) {
        if (t.state == 1 && t.priority > bestPri) {
            bestPri = t.priority;
            bestId = id;
        }
    }
    currentThreadId_ = bestId;
}

const HleThread* HleLibultra::currentThread() const noexcept {
    if (currentThreadId_ == 0) return nullptr;
    const auto it = threads_.find(currentThreadId_);
    if (it == threads_.end()) return nullptr;
    return &it->second;
}

void HleLibultra::createMesgQueue(u32 queueAddr, u32 capacity) {
    HleMesgQueue q;
    q.capacity = capacity;
    mesgQueues_[queueAddr] = std::move(q);
}

bool HleLibultra::sendMesg(u32 queueAddr, u32 message, u32 flags) {
    auto it = mesgQueues_.find(queueAddr);
    if (it == mesgQueues_.end()) return false;
    HleMesgQueue& q = it->second;
    if (q.messages.size() >= q.capacity) {
        if (flags & HLE_MESG_NOBLOCK) return false;
        // Blocking would require scheduler integration; treat as failure in HLE shim.
        return false;
    }
    q.messages.push_back(message);
    return true;
}

bool HleLibultra::recvMesg(u32 queueAddr, u32& outMessage, u32 flags) {
    auto it = mesgQueues_.find(queueAddr);
    if (it == mesgQueues_.end()) return false;
    HleMesgQueue& q = it->second;
    if (q.messages.empty()) {
        if (flags & HLE_MESG_NOBLOCK) return false;
        return false;
    }
    outMessage = q.messages.front();
    q.messages.pop_front();
    return true;
}

void HleLibultra::createTimer(u32 timerAddr, u64 expireCount, u32 queueAddr, u32 message) {
    HleTimer t;
    t.expireCount = expireCount;
    t.queueAddr = queueAddr;
    t.message = message;
    timers_.push_back(t);
    // timerAddr is kept for future introspection; the timer is tracked by value.
    (void)timerAddr;
}

void HleLibultra::tick(u64 now) {
    for (auto it = timers_.begin(); it != timers_.end(); ) {
        if (it->expireCount <= now) {
            if (it->queueAddr != 0) {
                sendMesg(it->queueAddr, it->message, HLE_MESG_NOBLOCK);
            }
            it = timers_.erase(it);
        } else {
            ++it;
        }
    }
}

void HleLibultra::piDmaRead(u32 cartAddr, u32 rdramAddr, u32 length, Bus& bus) {
    for (u32 i = 0; i < length; ++i) {
        bus.write8(rdramAddr + i, bus.read8(cartAddr + i));
    }
}

void HleLibultra::piDmaWrite(u32 rdramAddr, u32 cartAddr, u32 length, Bus& bus) {
    for (u32 i = 0; i < length; ++i) {
        bus.write8(cartAddr + i, bus.read8(rdramAddr + i));
    }
}

void HleLibultra::matrix(const float* m, u32 param) {
    if (matrixTop_ + 1 >= MatrixStackDepth && (param & HLE_MTX_PUSH)) {
        // Stack overflow: ignore push.
        return;
    }

    if (param & HLE_MTX_PUSH) {
        matrixStack_[matrixTop_ + 1] = matrixStack_[matrixTop_];
        ++matrixTop_;
    }

    std::array<float, 16> incoming;
    std::memcpy(incoming.data(), m, sizeof(incoming));

    if (param & HLE_MTX_LOAD) {
        matrixStack_[matrixTop_] = incoming;
    } else {
        std::array<float, 16> result;
        multiplyMatrix(result, matrixStack_[matrixTop_], incoming);
        matrixStack_[matrixTop_] = result;
    }
}

void HleLibultra::viewport(const HleViewport& vp) noexcept {
    viewport_ = vp;
}

void HleLibultra::identityMatrix(std::array<float, 16>& out) noexcept {
    out.fill(0.0f);
    out[0]  = 1.0f;
    out[5]  = 1.0f;
    out[10] = 1.0f;
    out[15] = 1.0f;
}

void HleLibultra::multiplyMatrix(std::array<float, 16>& out, const std::array<float, 16>& a,
                                  const std::array<float, 16>& b) noexcept {
    std::array<float, 16> tmp{};
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k) {
                sum += a[i * 4 + k] * b[k * 4 + j];
            }
            tmp[i * 4 + j] = sum;
        }
    }
    out = tmp;
}

} // namespace n64forge
