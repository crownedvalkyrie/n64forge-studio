#pragma once

#include <QCryptographicHash>
#include <QDir>
#include <QUrl>

namespace n64forge::studio {

enum class ArtKind { Cover, Splash, Logo, Snap };

class ArtRepository {
public:
    static QString defaultUrl() {
        return QStringLiteral("https://github.com/crownedvalkyrie/N64ForgeStudio_CoverArt.git");
    }

    // Only accept HTTPS GitHub repositories. This is also the source for the
    // opt-in git clone action; no arbitrary URL schemes or git transports.
    static QString canonicalUrl(const QString& input) {
        const QUrl url(input.trimmed(), QUrl::StrictMode);
        if (!url.isValid() || url.scheme() != QStringLiteral("https") ||
            url.host() != QStringLiteral("github.com") || !url.userInfo().isEmpty() ||
            url.port() != -1 || url.hasQuery() || url.hasFragment()) return {};
        auto path = url.path().split(QLatin1Char('/'), Qt::SkipEmptyParts);
        if (path.size() != 2) return {};
        if (path[1].endsWith(QStringLiteral(".git"))) path[1].chop(4);
        for (const auto& part : path) {
            if (part.isEmpty() || part == QStringLiteral(".") || part == QStringLiteral("..")) return {};
            for (const auto ch : part) {
                const auto n = ch.unicode();
                if (!((n >= 'a' && n <= 'z') || (n >= 'A' && n <= 'Z') ||
                      (n >= '0' && n <= '9') || n == '.' || n == '-' || n == '_')) return {};
            }
        }
        return QStringLiteral("https://github.com/%1/%2.git").arg(path[0], path[1]);
    }

    explicit ArtRepository(const QString& url = defaultUrl(), const QString& home = QDir::homePath())
        : url_(canonicalUrl(url)), home_(home) {
        if (url_.isEmpty()) url_ = defaultUrl();
    }

    const QString& url() const { return url_; }
    QString imageUrl(ArtKind kind, const QString& title) const {
        if (title.trimmed().isEmpty()) return {};
        const auto repo = url_.mid(QStringLiteral("https://github.com/").size()).chopped(4);
        return QStringLiteral("https://raw.githubusercontent.com/%1/main/%2/%3")
            .arg(repo, category(kind), QString::fromLatin1(QUrl::toPercentEncoding(fileName(title))));
    }

    static QString fileName(const QString& title) {
        QString name = title.trimmed();
        // Repo filenames follow the existing no-intro art naming convention.
        for (const auto ch : QStringLiteral("&*/:<>?\\|")) name.replace(ch, QLatin1Char('_'));
        return name + QStringLiteral(".png");
    }

    QString root() const {
#ifdef Q_OS_MACOS
        return home_ + QStringLiteral("/Library/Caches/n64forge/covers");
#else
        return home_ + QStringLiteral("/.cache/n64forge/covers");
#endif
    }
    QString repoCache() const { return root() + QLatin1Char('/') + key(url_.toLower()); }
    QString imagePath(ArtKind kind, const QString& title) const {
        return repoCache() + QLatin1Char('/') + category(kind) + QLatin1Char('/') +
               key(fileName(title)) + QStringLiteral(".png");
    }
    QString packPath() const { return repoCache() + QStringLiteral("/pack"); }
    QString packedImagePath(ArtKind kind, const QString& title) const {
        return packPath() + QLatin1Char('/') + category(kind) + QLatin1Char('/') + fileName(title);
    }
    static QString discordImage(const QString& cover, const QString& title) {
        if (!cover.isEmpty()) return cover;
        if (!title.isEmpty()) return title;
        return QStringLiteral("n64forge_logo");
    }

private:
    static QString category(ArtKind kind) {
        switch (kind) {
            case ArtKind::Cover: return QStringLiteral("Named_Boxarts");
            case ArtKind::Splash: return QStringLiteral("Named_Titles");
            case ArtKind::Logo: return QStringLiteral("Named_Logos");
            case ArtKind::Snap: return QStringLiteral("Named_Snaps");
        }
        return {};
    }
    static QString key(const QString& value) {
        return QString::fromLatin1(QCryptographicHash::hash(value.toUtf8(), QCryptographicHash::Sha256).toHex());
    }
    QString url_, home_;
};
} // namespace n64forge::studio
