#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <queue>
#include <string>
#include <thread>
#include <variant>
#include <vector>

namespace n64forge::studio {

// Default public Discord Application ID for N64Forge Studio. Users may override
// this through Settings; when the override is empty this ID is used automatically.
inline constexpr const char* kDefaultDiscordClientId = "1573978856262598697";

// Zero-dependency cross-platform Discord Rich Presence client.
//
// Transport:
//   - macOS / POSIX: Unix domain sockets in $TMPDIR/discord-ipc-0..9,
//     falling back to /tmp/discord-ipc-0..9.
//   - Windows: named pipes \\.\pipe\discord-ipc-0..9.
//
// IPC uses Discord's binary framing (8-byte little-endian header + UTF-8 JSON).
// All network I/O and reconnects run on a dedicated std::jthread so updating
// presence never blocks the UI or emulation threads.
class DiscordRpc {
public:
    DiscordRpc();
    ~DiscordRpc();

    // Non-copyable because of the worker thread and file handles.
    DiscordRpc(const DiscordRpc&) = delete;
    DiscordRpc& operator=(const DiscordRpc&) = delete;

    void setEnabled(bool enabled);
    void setClientId(const std::string& clientId);

    // Update the rich presence activity. `largeImage` may be a Discord asset
    // key or an external http(s) URL. `smallImage`/`smallText` describe the
    // active mode. By default the session start timestamp is refreshed; pass
    // `preserveStartTimestamp = true` to keep the timestamp from the previous
    // activity (useful for pause/resume updates).
    void updatePresence(const std::string& state,
                        const std::string& details,
                        const std::string& largeImage = {},
                        const std::string& largeText = {},
                        const std::string& smallImage = {},
                        const std::string& smallText = {},
                        bool preserveStartTimestamp = false);

    // Clear the active presence and optionally disconnect from the IPC pipe.
    void clearPresence();

    // Request clean shutdown; the worker thread will close the connection and
    // exit. Called automatically by the destructor.
    void shutdown();

private:
    enum class Opcode : uint32_t {
        Handshake = 0,
        Frame = 1,
        Close = 2,
        Ping = 3,
        Pong = 4,
    };

    struct SetPresenceCommand {
        std::string state;
        std::string details;
        std::string largeImage;
        std::string largeText;
        std::string smallImage;
        std::string smallText;
        std::int64_t startTimestamp = 0;
    };

    struct ClearPresenceCommand {};

    struct ShutdownCommand {};

    using Command = std::variant<SetPresenceCommand, ClearPresenceCommand, ShutdownCommand>;

    struct PresenceSnapshot {
        bool activitySet = false;
        SetPresenceCommand lastPresence;
    };

    void workerLoop();
    [[nodiscard]] bool connect();
    void disconnect();
    [[nodiscard]] bool isConnected() const noexcept;
    [[nodiscard]] bool sendFrame(Opcode op, const std::string& payload);
    [[nodiscard]] bool sendRaw(const uint8_t* data, std::size_t length);
    [[nodiscard]] bool readFrame(Opcode& outOp, std::string& outPayload);
    [[nodiscard]] bool readExact(uint8_t* buffer, std::size_t length);

    void pushCommand(Command command);
    [[nodiscard]] std::optional<Command> popCommand();

    // Copies guarded state under commandMutex_ so the worker never reads fields
    // that another thread may be updating.
    [[nodiscard]] PresenceSnapshot presenceSnapshot() const;
    [[nodiscard]] std::string clientIdSnapshot() const;

    [[nodiscard]] std::string buildHandshake() const;
    [[nodiscard]] std::string buildSetActivity(const SetPresenceCommand& cmd) const;
    [[nodiscard]] std::string buildClearActivity() const;
    [[nodiscard]] std::string jsonEscape(const std::string& value) const;
    [[nodiscard]] long long currentProcessId() const noexcept;

    std::string clientId_;
    std::atomic<bool> enabled_{false};
    std::atomic<bool> shutdownRequested_{false};

    // Mutable so const snapshot helpers can lock it.
    mutable std::mutex commandMutex_;
    std::condition_variable commandCv_;
    std::queue<Command> commandQueue_;

    std::thread worker_;

    bool activitySet_ = false;
    SetPresenceCommand lastPresence_;

#ifdef _WIN32
    void* pipeHandle_ = nullptr;
#else
    int socketFd_ = -1;
#endif
};

} // namespace n64forge::studio
