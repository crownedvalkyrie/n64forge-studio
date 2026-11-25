#pragma once

#include <QDateTime>
#include <QObject>
#include <QSet>
#include <QString>
#include <memory>
#include <optional>
#include <vector>

namespace n64forge::studio {

class GameDatabase;
class StudioSettings;

struct GameEntry {
    QString title;
    QString romPath;
    QString coverPath;   // local cached cover image path
    QString coverUrl;    // resolved HTTPS cover art URL (for Discord Rich Presence)
    QString format;
    QString videoRegion;
    QString cic;
    int countryCode = -1;
    int revision = -1;
    qint64 size = 0;
    QDateTime lastPlayed;
    int playCount = 0;
    bool manualCover = false;
};

class GameLibrary : public QObject {
    Q_OBJECT
public:
    explicit GameLibrary(StudioSettings* settings, QObject* parent = nullptr);
    explicit GameLibrary(StudioSettings* settings, GameDatabase* database, QObject* parent = nullptr);

    void addRom(const QString& path);
    void removeRom(const QString& path);
    void setCover(const QString& romPath, const QString& coverPath);
    void recordPlayed(const QString& romPath);
    void refreshArtwork();

    [[nodiscard]] const std::vector<GameEntry>& entries() const noexcept { return entries_; }
    [[nodiscard]] std::optional<GameEntry> find(const QString& romPath) const;
    [[nodiscard]] std::optional<GameEntry> inspectRom(const QString& romPath);

signals:
    void changed();
    void artworkResolved(const QString& romPath);

private:
    void load();
    void save();
    void queryDatabase(GameEntry& entry);

    StudioSettings* settings_ = nullptr;
    GameDatabase* database_ = nullptr;
    std::vector<GameEntry> entries_;
    QSet<QString> inspectedPaths_;
};

} // namespace n64forge::studio
