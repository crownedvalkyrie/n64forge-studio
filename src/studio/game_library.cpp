#include "n64forge/studio/game_library.hpp"
#include "n64forge/core/cartridge.hpp"
#include "n64forge/core/ipl.hpp"
#include "n64forge/studio/game_database.hpp"
#include "n64forge/studio/settings.hpp"
#include <QFile>
#include <QFileInfo>
#include <QtGlobal>

namespace n64forge::studio {

namespace {

QString formatName(const QString& path) {
    if (path.endsWith(QLatin1String(".z64"), Qt::CaseInsensitive)) return QStringLiteral("z64");
    if (path.endsWith(QLatin1String(".n64"), Qt::CaseInsensitive)) return QStringLiteral("n64");
    if (path.endsWith(QLatin1String(".v64"), Qt::CaseInsensitive)) return QStringLiteral("v64");
    return QStringLiteral("unknown");
}

QString normalizeTitle(const std::string& raw) {
    QString s = QString::fromStdString(raw);
    // N64 headers often pad with spaces/trailing nulls.
    while (!s.isEmpty() && (s.back().isSpace() || s.back() == QChar::Null)) {
        s.chop(1);
    }
    return s;
}

QString videoRegion(u8 country) {
    switch (country) {
        case 'B': return QStringLiteral("MPAL");
        case 'D': case 'F': case 'H': case 'I': case 'L': case 'P':
        case 'S': case 'U': case 'W': case 'X': case 'Y': return QStringLiteral("PAL");
        case 'A': case 'C': case 'E': case 'J': case 'K': case 'N': return QStringLiteral("NTSC");
        default: return {};
    }
}

void readCartridgeFacts(GameEntry& entry, const QFileInfo& info) {
    entry.format = formatName(entry.romPath);
    entry.size = info.size();
    entry.videoRegion.clear();
    entry.cic.clear();
    entry.countryCode = entry.revision = -1;
    // The header and IPL2 checksum need only the first 0x1004 bytes.
    // Read one power-of-two prefix compatible with the cartridge decoder.
    Cartridge cart;
    if (entry.romPath.endsWith(QLatin1String(".zip"), Qt::CaseInsensitive)) {
        // A ZIP needs the central directory and full member payload; parse it
        // in memory without extracting to disk.
        if (!cart.load(info.absoluteFilePath().toStdString())) return;
        entry.size = cart.size();
    } else {
        QFile file(info.absoluteFilePath());
        if (!file.open(QIODevice::ReadOnly)) return;
        const auto prefix = file.read(0x2000);
        if (prefix.size() < 0x40) return;
        const auto* bytes = reinterpret_cast<const u8*>(prefix.constData());
        if (!cart.loadFromBuffer(std::vector<u8>(bytes, bytes + prefix.size()))) return;
    }
    const auto& header = cart.header();
    if (const auto title = normalizeTitle(header.name); !title.isEmpty()) entry.title = title;
    entry.format = QString::fromStdString(cart.formatName());
    entry.countryCode = header.countryCode;
    entry.revision = header.version;
    entry.videoRegion = videoRegion(header.countryCode);
    const auto profile = detectCic(cart.rom());
    if (profile.variant != CicVariant::Unknown) {
        entry.cic = QStringLiteral("CIC-%1").arg(profile.id(entry.videoRegion == QStringLiteral("PAL")));
    }
}

} // namespace

GameLibrary::GameLibrary(StudioSettings* settings, QObject* parent)
    : QObject(parent)
    , settings_(settings) {
    load();
}

GameLibrary::GameLibrary(StudioSettings* settings, GameDatabase* database, QObject* parent)
    : QObject(parent)
    , settings_(settings)
    , database_(database) {
    load();
#ifdef N64FORGE_HAS_QT_NETWORK
    if (database_) {
        connect(database_, &GameDatabase::loaded, this, &GameLibrary::refreshArtwork);
        connect(database_, &GameDatabase::artRepositoryChanged, this, &GameLibrary::refreshArtwork);
        refreshArtwork();
    }
#endif
}

void GameLibrary::addRom(const QString& path) {
    if (path.isEmpty()) return;
    const QFileInfo info(path);
    if (!info.exists() || !info.isFile()) return;

    // Update existing entry if present.
    for (auto& entry : entries_) {
        if (entry.romPath == info.absoluteFilePath()) {
            entry.title = info.completeBaseName();
            readCartridgeFacts(entry, info);
            queryDatabase(entry);
            save();
            emit changed();
            return;
        }
    }

    GameEntry entry;
    entry.romPath = info.absoluteFilePath();
    entry.title = info.completeBaseName();
    readCartridgeFacts(entry, info);

    entries_.push_back(std::move(entry));
    queryDatabase(entries_.back());
    save();
    emit changed();
}

void GameLibrary::removeRom(const QString& path) {
    const auto it = std::remove_if(entries_.begin(), entries_.end(),
        [&path](const GameEntry& e) { return e.romPath == path; });
    if (it != entries_.end()) {
        entries_.erase(it, entries_.end());
        save();
        emit changed();
    }
}

void GameLibrary::setCover(const QString& romPath, const QString& coverPath) {
    for (auto& entry : entries_) {
        if (entry.romPath == romPath) {
            entry.coverPath = coverPath;
            entry.manualCover = !coverPath.isEmpty();
            save();
            emit changed();
            return;
        }
    }
}

void GameLibrary::recordPlayed(const QString& romPath) {
    for (auto& entry : entries_) {
        if (entry.romPath == romPath) {
            entry.lastPlayed = QDateTime::currentDateTime();
            ++entry.playCount;
            save();
            emit changed();
            return;
        }
    }
}

std::optional<GameEntry> GameLibrary::find(const QString& romPath) const {
    for (const auto& entry : entries_) {
        if (entry.romPath == romPath) return entry;
    }
    return std::nullopt;
}

std::optional<GameEntry> GameLibrary::inspectRom(const QString& romPath) {
    for (auto& entry : entries_) {
        if (entry.romPath != romPath) continue;
        if (entry.revision < 0 && !inspectedPaths_.contains(romPath)) {
            // Older settings did not store header facts. Inspect lazily, and
            // preserve the library's resolved title and artwork.
            inspectedPaths_.insert(romPath);
            auto refreshed = entry;
            readCartridgeFacts(refreshed, QFileInfo(romPath));
            if (refreshed.revision >= 0) {
                refreshed.title = entry.title;
                entry = std::move(refreshed);
            }
        }
        return entry;
    }
    return std::nullopt;
}

void GameLibrary::load() {
    entries_.clear();
    if (!settings_) return;
    bool ok = false;
    const int count = settings_->value(QStringLiteral("library/count"), 0).toInt(&ok);
    if (!ok) return;
    for (int i = 0; i < count; ++i) {
        const QString prefix = QStringLiteral("library/entry%1/").arg(i);
        GameEntry e;
        e.title = settings_->value(prefix + QStringLiteral("title")).toString();
        e.romPath = settings_->value(prefix + QStringLiteral("romPath")).toString();
        e.coverPath = settings_->value(prefix + QStringLiteral("coverPath")).toString();
        e.coverUrl = settings_->value(prefix + QStringLiteral("coverUrl")).toString();
        e.manualCover = settings_->value(prefix + QStringLiteral("manualCover")).toBool();
        e.format = settings_->value(prefix + QStringLiteral("format")).toString();
        e.videoRegion = settings_->value(prefix + QStringLiteral("videoRegion")).toString();
        e.cic = settings_->value(prefix + QStringLiteral("cic")).toString();
        e.countryCode = settings_->value(prefix + QStringLiteral("countryCode"), -1).toInt();
        e.revision = settings_->value(prefix + QStringLiteral("revision"), -1).toInt();
        e.size = settings_->value(prefix + QStringLiteral("size"), 0).toLongLong();
        e.lastPlayed = settings_->value(prefix + QStringLiteral("lastPlayed")).toDateTime();
        e.playCount = settings_->value(prefix + QStringLiteral("playCount"), 0).toInt();
        if (!e.romPath.isEmpty()) {
            entries_.push_back(std::move(e));
        }
    }
}

void GameLibrary::save() {
    if (!settings_) return;
    settings_->remove(QStringLiteral("library"));
    settings_->setValue(QStringLiteral("library/count"), static_cast<int>(entries_.size()));
    for (size_t i = 0; i < entries_.size(); ++i) {
        const QString prefix = QStringLiteral("library/entry%1/").arg(static_cast<int>(i));
        const auto& e = entries_[i];
        settings_->setValue(prefix + QStringLiteral("title"), e.title);
        settings_->setValue(prefix + QStringLiteral("romPath"), e.romPath);
        settings_->setValue(prefix + QStringLiteral("coverPath"), e.coverPath);
        settings_->setValue(prefix + QStringLiteral("coverUrl"), e.coverUrl);
        settings_->setValue(prefix + QStringLiteral("manualCover"), e.manualCover);
        settings_->setValue(prefix + QStringLiteral("format"), e.format);
        settings_->setValue(prefix + QStringLiteral("videoRegion"), e.videoRegion);
        settings_->setValue(prefix + QStringLiteral("cic"), e.cic);
        settings_->setValue(prefix + QStringLiteral("countryCode"), e.countryCode);
        settings_->setValue(prefix + QStringLiteral("revision"), e.revision);
        settings_->setValue(prefix + QStringLiteral("size"), e.size);
        settings_->setValue(prefix + QStringLiteral("lastPlayed"), e.lastPlayed);
        settings_->setValue(prefix + QStringLiteral("playCount"), e.playCount);
    }
}

void GameLibrary::refreshArtwork() {
    for (auto& entry : entries_) queryDatabase(entry);
    save();
    emit changed();
}

void GameLibrary::queryDatabase(GameEntry& entry) {
    if (!database_) return;
#ifdef N64FORGE_HAS_QT_NETWORK
    database_->ensureLoaded();
    if (auto info = database_->lookupForRom(entry.romPath)) {
        entry.title = info->title;
        entry.coverUrl.clear();
        if (!entry.manualCover) entry.coverPath.clear();
        database_->fetchCover(info->title, [this, title = info->title, path = entry.romPath](const Artwork& art) {
            for (auto& e : entries_) {
                if (e.romPath == path && e.title == title) {
                    e.coverUrl = art.url;
                    if (!e.manualCover) e.coverPath = art.path;
                    save();
                    emit changed();
                    emit artworkResolved(path);
                    break;
                }
            }
        });
    }
#else
    (void)entry;
#endif
}

} // namespace n64forge::studio
