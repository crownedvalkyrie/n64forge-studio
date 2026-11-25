// Standalone deterministic shutdown tests for DiscordRpc. Not registered in
// CMake; call run_discord_tests() from a test harness when desired.

#include "n64forge/studio/discord_rpc.hpp"

#include <cassert>
#include <chrono>
#include <iostream>
#include <thread>

namespace {

using n64forge::studio::DiscordRpc;

void testDisabledQueueDrainShutdown() {
    const auto start = std::chrono::steady_clock::now();
    {
        DiscordRpc rpc; // Disabled by default; queued commands must be discarded.
        for (int i = 0; i < 64; ++i) {
            rpc.updatePresence("state", "details");
            rpc.setClientId(std::to_string(i));
        }
        rpc.clearPresence();
        rpc.shutdown();
    } // Destructor joins the worker.
    const auto elapsed = std::chrono::steady_clock::now() - start;
    assert(elapsed < std::chrono::seconds(1));
}

void testReconnectBackoffIsInterruptible() {
    const auto start = std::chrono::steady_clock::now();
    {
        DiscordRpc rpc;
        rpc.setEnabled(true);
        rpc.updatePresence("Playing", "ROM");
        // No Discord IPC socket exists in tests, so the worker enters the
        // reconnect backoff. Disabling must wake it before the 5 s delay.
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        rpc.setEnabled(false);
        rpc.shutdown();
    }
    const auto elapsed = std::chrono::steady_clock::now() - start;
    assert(elapsed < std::chrono::milliseconds(1500));
}

} // namespace

int run_discord_tests() {
    testDisabledQueueDrainShutdown();
    testReconnectBackoffIsInterruptible();
    std::cout << "DiscordRpc shutdown tests passed\n";
    return 0;
}
