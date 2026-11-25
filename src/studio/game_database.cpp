#include "n64forge/studio/game_database.hpp"

#ifdef N64FORGE_HAS_QT_NETWORK

#include "n64forge/core/cartridge.hpp"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUrl>
#include <QXmlStreamReader>
#include <QtGlobal>

#include <algorithm>
#include <array>

namespace n64forge::studio {

namespace {

constexpr const char* DAT_URL =
    "https://raw.githubusercontent.com/libretro/libretro-database/master/metadat/no-intro/"
    "Nintendo%20-%20Nintendo%2064.dat";

[[nodiscard]] QString md5Key(QString md5) { return md5.toLower(); }
[[nodiscard]] QString crcKey(QString crc) { return crc.toLower(); }

[[nodiscard]] QString quotedValue(const QString& line, const QString& key) {
    const QRegularExpression re(QStringLiteral("\\b%1\\s+\"([^\"]*)\"").arg(QRegularExpression::escape(key)));
    const auto match = re.match(line);
    return match.hasMatch() ? match.captured(1) : QString{};
}

[[nodiscard]] QString bareValue(const QString& line, const QString& key) {
    const QRegularExpression re(
        QStringLiteral("\\b%1\\s+\"?([0-9A-Fa-f]+)\"?").arg(QRegularExpression::escape(key)));
    const auto match = re.match(line);
    return match.hasMatch() ? match.captured(1) : QString{};
}

[[nodiscard]] const std::array<quint32, 256>& crc32Table() {
    static const std::array<quint32, 256> table = []() {
        std::array<quint32, 256> t{};
        for (int i = 0; i < 256; ++i) {
            quint32 crc = static_cast<quint32>(i);
            for (int j = 0; j < 8; ++j) {
                crc = (crc >> 1) ^ (0xEDB88320u & ~(crc & 1 ? 0u : ~0u));
            }
            t[i] = crc;
        }
        return t;
    }();
    return table;
}

[[nodiscard]] QByteArray fileMd5(const QByteArray& data) {
    return QCryptographicHash::hash(data, QCryptographicHash::Md5).toHex();
}

[[nodiscard]] QString fileCrc32(const QByteArray& data) {
    quint32 crc = 0xFFFFFFFFu;
    for (char c : data) {
        crc = (crc >> 8) ^ crc32Table()[(crc ^ static_cast<quint8>(c)) & 0xFFu];
    }
    crc ^= 0xFFFFFFFFu;
    return QStringLiteral("%1").arg(crc, 8, 16, QLatin1Char('0'));
}

[[nodiscard]] QString normalizeTitle(QString s) {
    while (!s.isEmpty() && (s.back().isSpace() || s.back() == QChar::Null)) {
        s.chop(1);
    }
    return s;
}

[[nodiscard]] QString cartIdString(u16 id) {
    return QStringLiteral("%1").arg(id, 4, 16, QLatin1Char('0')).toUpper();
}

} // namespace

GameDatabase::GameDatabase(QObject* parent, ArtRepository art)
    : QObject(parent), art_(std::move(art)) {
}

void GameDatabase::ensureLoaded() {
    if (loaded_ || loading_) return;

    const QString cache = datCachePath();
    if (QFile::exists(cache)) {
        QFile file(cache);
        if (file.open(QIODevice::ReadOnly)) {
            parseDat(file.readAll());
            loaded_ = true;
            emit loaded();
            return;
        }
    }

    loading_ = true;
    QNetworkReply* reply = network_.get(QNetworkRequest(QUrl(QString::fromLatin1(DAT_URL))));
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        loading_ = false;
        if (reply->error() == QNetworkReply::NoError) {
            const QByteArray data = reply->readAll();
            saveDatCache(data);
            parseDat(data);
        }
        loaded_ = true;
        emit loaded();
    });
}

std::optional<GameInfo> GameDatabase::lookup(const QString& md5, const QString& crc) const {
    if (!md5.isEmpty()) {
        auto it = byMd5_.find(md5Key(md5));
        if (it != byMd5_.end()) return it->second;
    }
    if (!crc.isEmpty()) {
        auto it = byCrc_.find(crcKey(crc));
        if (it != byCrc_.end()) return it->second;
    }
    return std::nullopt;
}

std::optional<GameInfo> GameDatabase::lookupByTitleOrId(const QString& title,
                                                        const QString& cartridgeId) const {
    if (!title.isEmpty()) {
        const QString nt = normalizeTitle(title);
        auto it = byTitle_.find(nt);
        if (it != byTitle_.end()) return it->second;
    }
    if (!cartridgeId.isEmpty()) {
        const QString cid = cartridgeId.toUpper();
        auto it = byCartridgeId_.find(cid);
        if (it != byCartridgeId_.end()) return it->second;
    }
    return std::nullopt;
}

void GameDatabase::setArtRepository(const QString& url) {
    const ArtRepository repo(url);
    if (repo.url() == art_.url()) return;
    art_ = repo;
    emit artRepositoryChanged();
}

QString GameDatabase::datCachePath() const {
    const QString base = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    QDir().mkpath(base + QStringLiteral("/database"));
    return base + QStringLiteral("/database/n64.dat");
}

Artwork GameDatabase::cachedArtwork(const QString& title, ArtKind kind) const {
    const auto url = art_.imageUrl(kind, title);
    if (url.isEmpty()) return {};
    for (const auto& path : {art_.imagePath(kind, title), art_.packedImagePath(kind, title)}) {
        if (!QFileInfo::exists(path)) continue;
        const QPixmap image(path, "PNG");
        if (!image.isNull()) return {image, url, path};
    }
    return {};
}

void GameDatabase::fetchArtwork(const QString& title, ArtKind kind, ArtCallback callback) {
    const auto url = art_.imageUrl(kind, title);
    if (url.isEmpty()) {
        callback({});
        return;
    }
    if (auto cached = cachedArtwork(title, kind); !cached.url.isEmpty()) {
        callback(cached);
        return;
    }
    const auto cache = art_.imagePath(kind, title);
    auto& callbacks = pendingArt_[url];
    callbacks.push_back(std::move(callback));
    if (callbacks.size() > 1) return;
    QNetworkRequest request(QUrl::fromEncoded(url.toLatin1()));
    request.setTransferTimeout(10'000);
    auto* reply = network_.get(request);
    constexpr qint64 maxBytes = 8 * 1024 * 1024;
    connect(reply, &QNetworkReply::readyRead, reply, [reply] {
        if (reply->bytesAvailable() > maxBytes) reply->abort();
    });
    const auto repository = art_.url();
    connect(reply, &QNetworkReply::finished, this, [this, reply, url, cache, repository] {
        reply->deleteLater();
        auto callbacks = pendingArt_.take(url);
        if (repository != art_.url()) return;
        Artwork result;
        if (reply->error() == QNetworkReply::NoError &&
            reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 200 &&
            reply->bytesAvailable() <= maxBytes) {
            const auto data = reply->readAll();
            if (result.image.loadFromData(data, "PNG")) {
                result.url = url;
                QDir().mkpath(QFileInfo(cache).absolutePath());
                QSaveFile file(cache);
                if (file.open(QIODevice::WriteOnly) && file.write(data) == data.size() && file.commit())
                    result.path = cache;
            }
        }
        for (const auto& done : callbacks) done(result);
    });
}

void GameDatabase::fetchCover(const QString& title, ArtCallback callback) {
    fetchArtwork(title, ArtKind::Cover, [this, title, callback](const Artwork& cover) {
        if (!cover.url.isEmpty()) callback(cover);
        else fetchArtwork(title, ArtKind::Splash, callback);
    });
}

std::optional<GameInfo> GameDatabase::lookupForRom(const QString& romPath) const {
    n64forge::Cartridge cart;
    QByteArray romData;
    bool cartOk = cart.load(romPath.toStdString()) && !cart.rom().empty();
    if (cartOk) {
        romData = QByteArray(reinterpret_cast<const char*>(cart.rom().data()),
                             static_cast<int>(cart.rom().size()));
    } else {
        QFile file(romPath);
        if (file.open(QIODevice::ReadOnly)) {
            romData = file.readAll();
        }
    }

    if (romData.isEmpty()) return std::nullopt;

    const QString romMd5 = QString::fromLatin1(fileMd5(romData));
    const QString romCrc = fileCrc32(romData);

    if (auto info = lookup(romMd5, romCrc)) {
        return info;
    }

    if (cartOk) {
        const QString headerTitle = QString::fromStdString(cart.header().name);
        const QString cid = cartIdString(cart.header().cartridgeId);
        if (auto info = lookupByTitleOrId(headerTitle, cid)) {
            return info;
        }
    }

    const QString fileTitle = QFileInfo(romPath).completeBaseName();
    GameInfo fallback;
    fallback.title = fileTitle;
    fallback.cartridgeId.clear();
    fallback.romMd5 = romMd5;
    fallback.romCrc = romCrc;
    return fallback;
}

void GameDatabase::parseDat(const QByteArray& data) {
    byMd5_.clear();
    byCrc_.clear();
    byTitle_.clear();
    byCartridgeId_.clear();

    const QString text = QString::fromUtf8(data);

    if (text.contains(QStringLiteral("<datafile")) || text.contains(QStringLiteral("<?xml"))) {
        QXmlStreamReader reader(data);
        QString currentGame;
        QString currentDescription;

        while (!reader.atEnd()) {
            reader.readNext();
            if (!reader.isStartElement()) continue;
            const auto name = reader.name();
            if (name == QStringLiteral("game")) {
                currentGame = reader.attributes().value(QStringLiteral("name")).toString();
                currentDescription.clear();
            } else if (name == QStringLiteral("description")) {
                currentDescription = reader.readElementText();
            } else if (name == QStringLiteral("rom")) {
                const QString md5 = reader.attributes().value(QStringLiteral("md5")).toString();
                const QString crc = reader.attributes().value(QStringLiteral("crc")).toString();
                const QString title = currentGame.isEmpty() ? currentDescription : currentGame;
                if (title.isEmpty() || (md5.isEmpty() && crc.isEmpty())) continue;

                GameInfo info;
                info.title = title;
                info.romMd5 = md5.toLower();
                info.romCrc = crc.toLower();
                if (!md5.isEmpty()) byMd5_[md5Key(md5)] = info;
                if (!crc.isEmpty()) byCrc_[crcKey(crc)] = info;
                byTitle_[normalizeTitle(title)] = info;
            }
        }
        return;
    }

    bool inGame = false;
    QString currentTitle;
    for (const QString& rawLine : text.split(QLatin1Char('\n'))) {
        const QString line = rawLine.trimmed();
        if (!inGame) {
            if (line.startsWith(QStringLiteral("game")) && line.contains(QLatin1Char('('))) {
                inGame = true;
                currentTitle.clear();
            }
            continue;
        }
        if (line == QLatin1String(")")) {
            inGame = false;
            continue;
        }
        if (currentTitle.isEmpty() && line.startsWith(QStringLiteral("name"))) {
            currentTitle = quotedValue(line, QStringLiteral("name"));
            continue;
        }
        if (line.startsWith(QStringLiteral("rom")) && line.contains(QLatin1Char('('))) {
            const QString md5 = bareValue(line, QStringLiteral("md5"));
            const QString crc = bareValue(line, QStringLiteral("crc"));
            if (currentTitle.isEmpty() || (md5.isEmpty() && crc.isEmpty())) continue;

            GameInfo info;
            info.title = currentTitle;
            info.romMd5 = md5.toLower();
            info.romCrc = crc.toLower();
            if (!md5.isEmpty()) byMd5_[md5Key(md5)] = info;
            if (!crc.isEmpty()) byCrc_[crcKey(crc)] = info;
            byTitle_[normalizeTitle(currentTitle)] = info;
        }
    }
}

void GameDatabase::saveDatCache(const QByteArray& data) {
    const QString path = datCachePath();
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (file.open(QIODevice::WriteOnly)) {
        file.write(data);
    }
}

} // namespace n64forge::studio

#endif // N64FORGE_HAS_QT_NETWORK
