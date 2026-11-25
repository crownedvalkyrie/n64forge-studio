#include "n64forge/studio/discord_rpc.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <iomanip>
#include <sstream>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace n64forge::studio {

namespace {

constexpr auto kReconnectDelay = std::chrono::milliseconds(5000);
constexpr auto kUpdateInterval = std::chrono::milliseconds(5000);
constexpr auto kReadTimeoutMs = 100;

struct FrameHeader {
    uint32_t opcode = 0;
    uint32_t length = 0;
};

[[nodiscard]] std::vector<std::string> ipcPaths() {
    std::vector<std::string> paths;
    paths.reserve(20);

#ifdef _WIN32
    for (int i = 0; i < 10; ++i) {
        paths.push_back("\\\\.\\pipe\\discord-ipc-" + std::to_string(i));
    }
#else
    const char* tmpdir = std::getenv("TMPDIR");
    if (tmpdir && tmpdir[0]) {
        std::string base(tmpdir);
        if (base.back() != '/') base += '/';
        for (int i = 0; i < 10; ++i) {
            paths.push_back(base + "discord-ipc-" + std::to_string(i));
        }
    }
    for (int i = 0; i < 10; ++i) {
        paths.push_back("/tmp/discord-ipc-" + std::to_string(i));
    }
#endif
    return paths;
}

} // namespace

DiscordRpc::DiscordRpc() {
    {
        std::lock_guard<std::mutex> lock(commandMutex_);
        clientId_ = kDefaultDiscordClientId;
    }
    worker_ = std::thread(&DiscordRpc::workerLoop, this);
}

DiscordRpc::~DiscordRpc() {
    shutdown();
    if (worker_.joinable()) {
        worker_.join();
    }
}

void DiscordRpc::setEnabled(bool enabled) {
    enabled_.store(enabled, std::memory_order_release);
    commandCv_.notify_all();
}

void DiscordRpc::setClientId(const std::string& clientId) {
    {
        std::lock_guard<std::mutex> lock(commandMutex_);
        clientId_ = clientId.empty() ? std::string(kDefaultDiscordClientId) : clientId;
    }
    // Force a reconnect so the new client ID is used immediately.
    if (enabled_.load(std::memory_order_acquire)) {
        pushCommand(ClearPresenceCommand{});
    }
    commandCv_.notify_all();
}

void DiscordRpc::updatePresence(const std::string& state,
                                const std::string& details,
                                const std::string& largeImage,
                                const std::string& largeText,
                                const std::string& smallImage,
                                const std::string& smallText,
                                bool preserveStartTimestamp) {
    std::int64_t startTimestamp = 0;
    {
        std::lock_guard<std::mutex> lock(commandMutex_);
        if (preserveStartTimestamp && lastPresence_.startTimestamp != 0) {
            startTimestamp = lastPresence_.startTimestamp;
        }
    }
    if (startTimestamp == 0) {
        const auto now = std::chrono::system_clock::now().time_since_epoch();
        startTimestamp = std::chrono::duration_cast<std::chrono::seconds>(now).count();
    }

    SetPresenceCommand cmd{state, details, largeImage, largeText,
                           smallImage, smallText, startTimestamp};
    {
        std::lock_guard<std::mutex> lock(commandMutex_);
        lastPresence_ = cmd;
        activitySet_ = true;
    }
    pushCommand(std::move(cmd));
}

void DiscordRpc::clearPresence() {
    {
        std::lock_guard<std::mutex> lock(commandMutex_);
        activitySet_ = false;
    }
    pushCommand(ClearPresenceCommand{});
}

void DiscordRpc::shutdown() {
    shutdownRequested_.store(true, std::memory_order_release);
    pushCommand(ShutdownCommand{});
}

void DiscordRpc::pushCommand(Command command) {
    {
        std::lock_guard<std::mutex> lock(commandMutex_);
        commandQueue_.push(std::move(command));
    }
    commandCv_.notify_all();
}

std::optional<DiscordRpc::Command> DiscordRpc::popCommand() {
    std::lock_guard<std::mutex> lock(commandMutex_);
    if (commandQueue_.empty()) {
        return std::nullopt;
    }
    Command cmd = std::move(commandQueue_.front());
    commandQueue_.pop();
    return cmd;
}

DiscordRpc::PresenceSnapshot DiscordRpc::presenceSnapshot() const {
    std::lock_guard<std::mutex> lock(commandMutex_);
    return PresenceSnapshot{activitySet_, lastPresence_};
}

std::string DiscordRpc::clientIdSnapshot() const {
    std::lock_guard<std::mutex> lock(commandMutex_);
    return clientId_;
}

void DiscordRpc::workerLoop() {
    auto lastUpdate = std::chrono::steady_clock::now();
    bool pendingReconnect = false;

    while (!shutdownRequested_.load(std::memory_order_acquire)) {
        const bool enabled = enabled_.load(std::memory_order_acquire);

        if (!enabled) {
            pendingReconnect = false;
            if (isConnected()) {
                (void)sendFrame(Opcode::Frame, buildClearActivity());
                disconnect();
            }
            // Discard commands queued while Rich Presence was off. Without this
            // drain, the wake predicate stays true and the worker busy-spins.
            while (popCommand()) {
            }
            std::unique_lock<std::mutex> lock(commandMutex_);
            commandCv_.wait(lock, [this] {
                return shutdownRequested_.load(std::memory_order_acquire) ||
                       enabled_.load(std::memory_order_acquire) ||
                       !commandQueue_.empty();
            });
            continue;
        }

        if (!isConnected()) {
            if (pendingReconnect) {
                // Interruptible backoff: shutdown, disable or a new command
                // must not wait out the full reconnect delay.
                std::unique_lock<std::mutex> lock(commandMutex_);
                commandCv_.wait_until(
                    lock,
                    std::chrono::steady_clock::now() + kReconnectDelay,
                    [this] {
                        return shutdownRequested_.load(std::memory_order_acquire) ||
                               !enabled_.load(std::memory_order_acquire) ||
                               !commandQueue_.empty();
                    });
                lock.unlock();
                pendingReconnect = false;
                if (shutdownRequested_.load(std::memory_order_acquire) ||
                    !enabled_.load(std::memory_order_acquire)) {
                    continue;
                }
            }
            if (!connect()) {
                pendingReconnect = true;
                continue;
            }
            if (!sendFrame(Opcode::Handshake, buildHandshake())) {
                disconnect();
                pendingReconnect = true;
                continue;
            }
            const auto presence = presenceSnapshot();
            if (presence.activitySet) {
                if (!sendFrame(Opcode::Frame, buildSetActivity(presence.lastPresence))) {
                    disconnect();
                    pendingReconnect = true;
                    continue;
                }
            } else {
                (void)sendFrame(Opcode::Frame, buildClearActivity());
            }
        }

        // Process queued commands.
        while (auto cmd = popCommand()) {
            if (std::holds_alternative<ShutdownCommand>(*cmd)) {
                break;
            }
            if (std::holds_alternative<ClearPresenceCommand>(*cmd)) {
                if (!sendFrame(Opcode::Frame, buildClearActivity())) {
                    disconnect();
                    pendingReconnect = true;
                    break;
                }
            } else if (auto* set = std::get_if<SetPresenceCommand>(&*cmd)) {
                if (!sendFrame(Opcode::Frame, buildSetActivity(*set))) {
                    disconnect();
                    pendingReconnect = true;
                    break;
                }
            }
        }

        if (shutdownRequested_.load(std::memory_order_acquire)) break;
        if (!isConnected()) continue;

        // Periodic re-send of the current activity to keep it alive.
        const auto now = std::chrono::steady_clock::now();
        if (now - lastUpdate >= kUpdateInterval) {
            lastUpdate = now;
            const auto presence = presenceSnapshot();
            if (presence.activitySet) {
                if (!sendFrame(Opcode::Frame, buildSetActivity(presence.lastPresence))) {
                    disconnect();
                    pendingReconnect = true;
                    continue;
                }
            }
        }

        // Read any incoming frames (pings, pongs, close) without blocking forever.
        Opcode op;
        std::string payload;
        if (readFrame(op, payload)) {
            if (op == Opcode::Close) {
                disconnect();
                pendingReconnect = true;
            } else if (op == Opcode::Ping) {
                (void)sendFrame(Opcode::Pong, payload);
            }
            // Pong and handshake responses are ignored for now.
        }

        // Avoid busy-spinning when no commands are pending. Re-check enablement
        // so disabling wakes the worker immediately.
        std::unique_lock<std::mutex> lock(commandMutex_);
        commandCv_.wait_for(lock, std::chrono::milliseconds(kReadTimeoutMs), [this] {
            return shutdownRequested_.load(std::memory_order_acquire) ||
                   !enabled_.load(std::memory_order_acquire) ||
                   !commandQueue_.empty();
        });
    }

    if (isConnected()) {
        (void)sendFrame(Opcode::Close, std::string{});
    }
    disconnect();
}

bool DiscordRpc::connect() {
    disconnect();

    for (const std::string& path : ipcPaths()) {
#ifdef _WIN32
        HANDLE h = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                               nullptr, OPEN_EXISTING, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            pipeHandle_ = h;
            return true;
        }
#else
        int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) continue;

        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        if (path.size() >= sizeof(addr.sun_path)) {
            ::close(fd);
            continue;
        }
        std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);

        if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
            socketFd_ = fd;
            return true;
        }
        ::close(fd);
#endif
    }
    return false;
}

void DiscordRpc::disconnect() {
#ifdef _WIN32
    if (pipeHandle_) {
        CloseHandle(static_cast<HANDLE>(pipeHandle_));
        pipeHandle_ = nullptr;
    }
#else
    if (socketFd_ >= 0) {
        ::close(socketFd_);
        socketFd_ = -1;
    }
#endif
}

bool DiscordRpc::isConnected() const noexcept {
#ifdef _WIN32
    return pipeHandle_ != nullptr;
#else
    return socketFd_ >= 0;
#endif
}

bool DiscordRpc::sendFrame(Opcode op, const std::string& payload) {
    if (!isConnected()) return false;

    FrameHeader header;
    header.opcode = static_cast<uint32_t>(op);
    header.length = static_cast<uint32_t>(payload.size());

    std::vector<uint8_t> frame(sizeof(header) + payload.size());
    std::memcpy(frame.data(), &header, sizeof(header));
    if (!payload.empty()) {
        std::memcpy(frame.data() + sizeof(header), payload.data(), payload.size());
    }

    return sendRaw(frame.data(), frame.size());
}

bool DiscordRpc::sendRaw(const uint8_t* data, std::size_t length) {
    if (!isConnected()) return false;

#ifdef _WIN32
    DWORD written = 0;
    while (written < length) {
        DWORD chunk = 0;
        BOOL ok = WriteFile(static_cast<HANDLE>(pipeHandle_), data + written,
                            static_cast<DWORD>(length - written), &chunk, nullptr);
        if (!ok || chunk == 0) {
            disconnect();
            return false;
        }
        written += chunk;
    }
    return true;
#else
    std::size_t total = 0;
    while (total < length) {
        ssize_t n = ::write(socketFd_, data + total, length - total);
        if (n < 0) {
            if (errno == EINTR) continue;
            disconnect();
            return false;
        }
        if (n == 0) {
            disconnect();
            return false;
        }
        total += static_cast<std::size_t>(n);
    }
    return true;
#endif
}

bool DiscordRpc::readFrame(Opcode& outOp, std::string& outPayload) {
    if (!isConnected()) return false;

    FrameHeader header;
    if (!readExact(reinterpret_cast<uint8_t*>(&header), sizeof(header))) {
        return false;
    }

    outOp = static_cast<Opcode>(header.opcode);
    if (header.length == 0) {
        outPayload.clear();
        return true;
    }

    outPayload.resize(header.length);
    return readExact(reinterpret_cast<uint8_t*>(outPayload.data()), header.length);
}

bool DiscordRpc::readExact(uint8_t* buffer, std::size_t length) {
    if (!isConnected()) return false;

#ifdef _WIN32
    DWORD total = 0;
    while (total < length) {
        DWORD available = 0;
        DWORD read = 0;
        if (!PeekNamedPipe(static_cast<HANDLE>(pipeHandle_), nullptr, 0, nullptr, &available, nullptr)) {
            disconnect();
            return false;
        }
        if (available == 0) {
            // No data available yet; treat as timeout rather than error.
            return false;
        }
        DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(length - total, available));
        if (!ReadFile(static_cast<HANDLE>(pipeHandle_), buffer + total, chunk, &read, nullptr)) {
            disconnect();
            return false;
        }
        total += read;
    }
    return true;
#else
    // Use non-blocking peek to implement a timeout semantics without select().
    int flags = ::fcntl(socketFd_, F_GETFL, 0);
    (void)::fcntl(socketFd_, F_SETFL, flags | O_NONBLOCK);

    std::size_t total = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kReadTimeoutMs);
    while (total < length) {
        ssize_t n = ::read(socketFd_, buffer + total, length - total);
        if (n > 0) {
            total += static_cast<std::size_t>(n);
        } else if (n == 0) {
            disconnect();
            ::fcntl(socketFd_, F_SETFL, flags);
            return false;
        } else if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                if (std::chrono::steady_clock::now() >= deadline) {
                    ::fcntl(socketFd_, F_SETFL, flags);
                    return false;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }
            disconnect();
            ::fcntl(socketFd_, F_SETFL, flags);
            return false;
        }
    }
    ::fcntl(socketFd_, F_SETFL, flags);
    return true;
#endif
}

std::string DiscordRpc::buildHandshake() const {
    return std::string(R"({"v":1,"client_id":")") + jsonEscape(clientIdSnapshot()) + R"("})";
}

std::string DiscordRpc::buildSetActivity(const SetPresenceCommand& cmd) const {
    std::ostringstream json;
    json << R"({"cmd":"SET_ACTIVITY","args":{"pid":)" << currentProcessId()
         << R"(,"activity":{)";

    bool first = true;
    auto addField = [&](const char* key, const std::string& value) {
        if (value.empty()) return;
        if (!first) json << ",";
        first = false;
        json << "\"" << key << "\":\"" << jsonEscape(value) << "\"";
    };

    addField("state", cmd.state);
    addField("details", cmd.details);

    if (cmd.startTimestamp != 0) {
        if (!first) json << ",";
        first = false;
        json << "\"timestamps\":{\"start\":" << cmd.startTimestamp << "}";
    }

    if (!cmd.largeImage.empty() || !cmd.largeText.empty() ||
        !cmd.smallImage.empty() || !cmd.smallText.empty()) {
        if (!first) json << ",";
        first = false;
        json << "\"assets\":{";
        bool assetFirst = true;
        auto addAsset = [&](const char* key, const std::string& value) {
            if (value.empty()) return;
            if (!assetFirst) json << ",";
            assetFirst = false;
            json << "\"" << key << "\":\"" << jsonEscape(value) << "\"";
        };
        addAsset("large_image", cmd.largeImage);
        addAsset("large_text", cmd.largeText);
        addAsset("small_image", cmd.smallImage);
        addAsset("small_text", cmd.smallText);
        json << "}";
    }

    static uint64_t nonceCounter = 0;
    json << "}},\"nonce\":\"" << ++nonceCounter << "\"}";
    return json.str();
}

std::string DiscordRpc::buildClearActivity() const {
    std::ostringstream json;
    static uint64_t nonceCounter = 0;
    json << R"({"cmd":"SET_ACTIVITY","args":{"pid":)" << currentProcessId()
         << R"(,"activity":null},"nonce":"clear_)" << ++nonceCounter << R"("})";
    return json.str();
}

std::string DiscordRpc::jsonEscape(const std::string& value) const {
    std::string out;
    out.reserve(value.size());
    for (char c : value) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    std::ostringstream hex;
                    hex << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                        << static_cast<int>(static_cast<unsigned char>(c));
                    out += hex.str();
                } else {
                    out += c;
                }
                break;
        }
    }
    return out;
}

long long DiscordRpc::currentProcessId() const noexcept {
#ifdef _WIN32
    return static_cast<long long>(GetCurrentProcessId());
#else
    return static_cast<long long>(getpid());
#endif
}

} // namespace n64forge::studio
