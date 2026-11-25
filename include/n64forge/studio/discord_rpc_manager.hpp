#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QTimer>

namespace n64forge::studio {

class DiscordRpcManager : public QObject {
    Q_OBJECT
public:
    explicit DiscordRpcManager(QObject* parent = nullptr);
    ~DiscordRpcManager() override;

    void setEnabled(bool enabled);
    void setClientId(const QString& clientId);

    void setActivity(const QString& state, const QString& details,
                     const QString& largeImageKey = {}, const QString& largeImageText = {},
                     int partySize = 0, int partyMax = 0);
    void clearActivity();

private slots:
    void tryConnect();
    void onUpdateTimer();

private:
    void disconnect();
    [[nodiscard]] bool isConnected() const noexcept;
    [[nodiscard]] bool sendFrame(quint32 op, const QByteArray& payload);
    void sendHandshake();
    void sendSetActivity();

    bool enabled_ = false;
    QString clientId_;
    QString state_;
    QString details_;
    QString largeImageKey_;
    QString largeImageText_;
    int partySize_ = 0;
    int partyMax_ = 0;
    bool activityDirty_ = false;
    bool activitySet_ = false;

    QTimer* reconnectTimer_ = nullptr;
    QTimer* updateTimer_ = nullptr;

#ifdef _WIN32
    void* pipeHandle_ = nullptr;
#else
    int socketFd_ = -1;
#endif
};

} // namespace n64forge::studio
