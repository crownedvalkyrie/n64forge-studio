#include "n64forge/studio/discord_rpc_manager.hpp"
#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QtGlobal>
#include <QUuid>
#include <array>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace n64forge::studio {

namespace {

constexpr int RECONNECT_MS = 5000;
constexpr int UPDATE_MS = 5000;

[[nodiscard]] QStringList ipcPaths() {
    QStringList paths;
    paths << QStringLiteral("/tmp/discord-ipc-0");
    const char* tmpdir = getenv("TMPDIR");
    if (tmpdir && tmpdir[0]) {
        paths << QString::fromLocal8Bit(tmpdir) + QStringLiteral("discord-ipc-0");
    }
    const char* tmp = getenv("TMP");
    if (tmp && tmp[0]) {
        paths << QString::fromLocal8Bit(tmp) + QStringLiteral("/discord-ipc-0");
    }
    const char* temp = getenv("TEMP");
    if (temp && temp[0]) {
        paths << QString::fromLocal8Bit(temp) + QStringLiteral("/discord-ipc-0");
    }
    return paths;
}

[[nodiscard]] QByteArray makeHandshake(const QString& clientId) {
    QJsonObject obj;
    obj[QStringLiteral("v")] = 1;
    obj[QStringLiteral("client_id")] = clientId;
    return QJsonDocument(obj).toJson(QJsonDocument::Compact);
}

[[nodiscard]] QByteArray makeSetActivity(const QString& clientId,
                                          const QString& state,
                                          const QString& details,
                                          const QString& largeImageKey,
                                          const QString& largeImageText,
                                          int partySize,
                                          int partyMax,
                                          qint64 pid) {
    QJsonObject activity;
    if (!state.isEmpty()) activity[QStringLiteral("state")] = state;
    if (!details.isEmpty()) activity[QStringLiteral("details")] = details;

    if (!largeImageKey.isEmpty() || !largeImageText.isEmpty()) {
        QJsonObject assets;
        if (!largeImageKey.isEmpty()) assets[QStringLiteral("large_image")] = largeImageKey;
        if (!largeImageText.isEmpty()) assets[QStringLiteral("large_text")] = largeImageText;
        activity[QStringLiteral("assets")] = assets;
    }

    if (partySize > 0 || partyMax > 0) {
        QJsonObject party;
        party[QStringLiteral("size")] = QJsonArray{partySize, qMax(partyMax, partySize)};
        activity[QStringLiteral("party")] = party;
    }

    QJsonObject args;
    args[QStringLiteral("activity")] = activity;
    args[QStringLiteral("pid")] = static_cast<qint64>(pid);

    QJsonObject obj;
    obj[QStringLiteral("cmd")] = QStringLiteral("SET_ACTIVITY");
    obj[QStringLiteral("args")] = args;
    obj[QStringLiteral("nonce")] = QUuid::createUuid().toString(QUuid::WithoutBraces);

    return QJsonDocument(obj).toJson(QJsonDocument::Compact);
}

[[nodiscard]] QByteArray makeClearActivity(qint64 pid) {
    QJsonObject args;
    args[QStringLiteral("activity")] = QJsonValue::Null;
    args[QStringLiteral("pid")] = static_cast<qint64>(pid);

    QJsonObject obj;
    obj[QStringLiteral("cmd")] = QStringLiteral("SET_ACTIVITY");
    obj[QStringLiteral("args")] = args;
    obj[QStringLiteral("nonce")] = QUuid::createUuid().toString(QUuid::WithoutBraces);

    return QJsonDocument(obj).toJson(QJsonDocument::Compact);
}

} // namespace

DiscordRpcManager::DiscordRpcManager(QObject* parent)
    : QObject(parent)
    , reconnectTimer_(new QTimer(this))
    , updateTimer_(new QTimer(this)) {
    reconnectTimer_->setInterval(RECONNECT_MS);
    connect(reconnectTimer_, &QTimer::timeout, this, &DiscordRpcManager::tryConnect);

    updateTimer_->setInterval(UPDATE_MS);
    connect(updateTimer_, &QTimer::timeout, this, &DiscordRpcManager::onUpdateTimer);
}

DiscordRpcManager::~DiscordRpcManager() {
    if (isConnected()) {
        (void)sendFrame(1, makeClearActivity(QCoreApplication::applicationPid()));
    }
    disconnect();
}

void DiscordRpcManager::setEnabled(bool enabled) {
    if (enabled_ == enabled) return;
    enabled_ = enabled;
    if (enabled_) {
        tryConnect();
        reconnectTimer_->start();
        updateTimer_->start();
    } else {
        reconnectTimer_->stop();
        updateTimer_->stop();
        if (isConnected()) {
            (void)sendFrame(1, makeClearActivity(QCoreApplication::applicationPid()));
        }
        disconnect();
    }
}

void DiscordRpcManager::setClientId(const QString& clientId) {
    if (clientId_ == clientId) return;
    clientId_ = clientId;
    if (enabled_) {
        disconnect();
        tryConnect();
    }
}

void DiscordRpcManager::setActivity(const QString& state, const QString& details,
                                    const QString& largeImageKey, const QString& largeImageText,
                                    int partySize, int partyMax) {
    state_ = state;
    details_ = details;
    largeImageKey_ = largeImageKey;
    largeImageText_ = largeImageText;
    partySize_ = partySize;
    partyMax_ = partyMax;
    activityDirty_ = true;
    activitySet_ = true;
    if (enabled_ && isConnected()) {
        sendSetActivity();
    }
}

void DiscordRpcManager::clearActivity() {
    state_.clear();
    details_.clear();
    largeImageKey_.clear();
    largeImageText_.clear();
    partySize_ = 0;
    partyMax_ = 0;
    activitySet_ = false;
    activityDirty_ = false;
    if (enabled_ && isConnected()) {
        (void)sendFrame(1, makeClearActivity(QCoreApplication::applicationPid()));
    }
}

void DiscordRpcManager::tryConnect() {
    if (!enabled_ || isConnected() || clientId_.isEmpty()) return;

#ifdef _WIN32
    const QString pipeName = QStringLiteral("\\\\?\\pipe\\discord-ipc-0");
    HANDLE h = CreateFileW(pipeName.toStdWString().c_str(), GENERIC_READ | GENERIC_WRITE,
                           0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return;
    }
    pipeHandle_ = h;
#else
    int fd = -1;
    for (const QString& path : ipcPaths()) {
        fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) continue;
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        const QByteArray utf8 = path.toUtf8();
        if (utf8.size() >= static_cast<int>(sizeof(addr.sun_path))) continue;
        (void)std::strncpy(addr.sun_path, utf8.constData(), sizeof(addr.sun_path) - 1);
        if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
            break;
        }
        ::close(fd);
        fd = -1;
    }
    if (fd < 0) return;
    socketFd_ = fd;
#endif

    (void)sendHandshake();
    if (activitySet_) {
        sendSetActivity();
    } else {
        (void)sendFrame(1, makeClearActivity(QCoreApplication::applicationPid()));
    }
}

void DiscordRpcManager::onUpdateTimer() {
    if (!enabled_ || !isConnected()) return;
    if (activityDirty_) {
        sendSetActivity();
    }
}

void DiscordRpcManager::disconnect() {
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

bool DiscordRpcManager::isConnected() const noexcept {
#ifdef _WIN32
    return pipeHandle_ != nullptr;
#else
    return socketFd_ >= 0;
#endif
}

bool DiscordRpcManager::sendFrame(quint32 op, const QByteArray& payload) {
    if (!isConnected()) return false;

    QByteArray frame;
    frame.reserve(8 + payload.size());
    frame.append(reinterpret_cast<const char*>(&op), sizeof(op));
    quint32 len = static_cast<quint32>(payload.size());
    frame.append(reinterpret_cast<const char*>(&len), sizeof(len));
    frame.append(payload);

#ifdef _WIN32
    DWORD written = 0;
    BOOL ok = WriteFile(static_cast<HANDLE>(pipeHandle_), frame.constData(),
                        static_cast<DWORD>(frame.size()), &written, nullptr);
    return ok && written == static_cast<DWORD>(frame.size());
#else
    ssize_t total = 0;
    while (total < frame.size()) {
        ssize_t n = ::write(socketFd_, frame.constData() + total, frame.size() - total);
        if (n < 0) {
            if (errno == EINTR) continue;
            disconnect();
            return false;
        }
        total += n;
    }
    return total == frame.size();
#endif
}

void DiscordRpcManager::sendHandshake() {
    (void)sendFrame(0, makeHandshake(clientId_));
}

void DiscordRpcManager::sendSetActivity() {
    if (!isConnected()) return;
    activityDirty_ = false;
    (void)sendFrame(1, makeSetActivity(clientId_, state_, details_, largeImageKey_,
                                 largeImageText_, partySize_, partyMax_,
                                 QCoreApplication::applicationPid()));
}

} // namespace n64forge::studio
