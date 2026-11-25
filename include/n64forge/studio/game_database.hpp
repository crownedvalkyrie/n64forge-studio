#pragma once

#ifdef N64FORGE_HAS_QT_NETWORK

#include "n64forge/studio/art_repository.hpp"
#include <QHash>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QObject>
#include <QPixmap>
#include <QString>
#include <functional>
#include <optional>
#include <unordered_map>
#include <vector>

namespace n64forge::studio {

struct GameInfo {
    QString title;              // canonical No-Intro / database title
    QString cartridgeId;        // 2-byte cartridge ID (e.g. "NSME")
    QString romMd5;             // big-endian ROM MD5 (hex, lower-case)
    QString romCrc;             // big-endian ROM CRC32 (hex, lower-case)
};

struct Artwork {
    QPixmap image;
    QString url;
    QString path;
};

class GameDatabase : public QObject {
    Q_OBJECT
public:
    explicit GameDatabase(QObject* parent = nullptr, ArtRepository art = ArtRepository());

    // Look up a game by MD5 (preferred) or CRC32 hex string.
    [[nodiscard]] std::optional<GameInfo> lookup(const QString& md5, const QString& crc) const;

    // Fallback lookup by canonical title or 2-byte cartridge ID.
    [[nodiscard]] std::optional<GameInfo> lookupByTitleOrId(const QString& title,
                                                            const QString& cartridgeId) const;

    [[nodiscard]] const ArtRepository& artRepository() const { return art_; }
    void setArtRepository(const QString& url);

    // Ensure the database is loaded, fetching the public No-Intro DAT if needed.
    void ensureLoaded();

    using ArtCallback = std::function<void(const Artwork&)>;
    [[nodiscard]] Artwork cachedArtwork(const QString& title, ArtKind kind) const;
    void fetchArtwork(const QString& title, ArtKind kind, ArtCallback callback);
    // Returns a verified box-art URL (or title fallback), never a guessed URL.
    void fetchCover(const QString& title, ArtCallback callback);

    // Look up a ROM by canonical hash, header title/cartridge ID, or filename.
    [[nodiscard]] std::optional<GameInfo> lookupForRom(const QString& romPath) const;

signals:
    void loaded();
    void artRepositoryChanged();

private:
    void parseDat(const QByteArray& data);
    void saveDatCache(const QByteArray& data);

    [[nodiscard]] QString datCachePath() const;

    QNetworkAccessManager network_;
    ArtRepository art_;
    QHash<QString, std::vector<ArtCallback>> pendingArt_;
    bool loading_ = false;
    bool loaded_ = false;
    std::unordered_map<QString, GameInfo> byMd5_;
    std::unordered_map<QString, GameInfo> byCrc_;
    std::unordered_map<QString, GameInfo> byTitle_;
    std::unordered_map<QString, GameInfo> byCartridgeId_;
};

} // namespace n64forge::studio

#endif // N64FORGE_HAS_QT_NETWORK
