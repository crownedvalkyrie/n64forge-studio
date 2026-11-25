#pragma once
#include <QByteArray>
#include <QColor>
#include <QPixmap>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QWidget>

class QTabWidget;
class QVBoxLayout;

namespace n64forge::studio {

struct CreditEntry {
    QString name;
    QString role;
    QString note;
    QString pfp;
};

struct CreditGroup {
    QString key;
    QString title;
    QVector<CreditEntry> entries;
};

struct CreditBook {
    QVector<CreditGroup> groups;
    QString source;
    QString error;
    bool ok = false;
};

// Bundled default data (`:/credits/credits.json`), also used as a fallback.
[[nodiscard]] QByteArray bundledCreditsJson();

// Parses the human-editable credits document. Unknown keys are ignored; each
// entry needs at least a display name. Group order is Devs, Supporters, Testers.
[[nodiscard]] CreditBook parseCredits(const QByteArray& json,
                                      const QString& source = QString());

// First letter (or two) of each significant word, upper-cased.
[[nodiscard]] QString creditInitials(const QString& name);

// Stable accent derived from the name; never random, so tests and UI agree.
[[nodiscard]] QColor creditAccent(const QString& name);

// Loads `entry.pfp` (absolute, or relative to baseDir, then the Qt resource
// path) and masks it to a circle. Missing files render a deterministic initials
// badge instead, so a new contributor never needs an image to appear.
[[nodiscard]] QPixmap renderCreditAvatar(const CreditEntry& entry, int size,
                                         const QString& baseDir = QString());

class CreditsView final : public QWidget {
public:
    explicit CreditsView(QWidget* parent = nullptr);

    // Reloads from the bundled data and any filesystem override.
    void reload();
    void loadFromFile(const QString& path);

    [[nodiscard]] const CreditBook& book() const { return book_; }
    [[nodiscard]] int cardCount(const QString& groupKey) const;
    [[nodiscard]] QStringList groupKeys() const;

private:
    void rebuild();
    CreditBook book_;
    QString baseDir_;
    QTabWidget* tabs_ = nullptr;
};

} // namespace n64forge::studio
