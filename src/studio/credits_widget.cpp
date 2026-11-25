#include "n64forge/studio/credits_widget.hpp"
#include "n64forge/core/types.hpp"

#include <QCoreApplication>
#include <QFont>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QLabel>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QPlainTextEdit>
#include <QRegularExpression>
#include <QScrollArea>
#include <QStringList>
#include <QTabWidget>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>
#include <iterator>

namespace n64forge::studio {
namespace {

constexpr int AvatarSize = 44;

const char* const DefaultCredits = R"JSON({
  "devs": [
    {
      "name": "N64Forge Team",
      "role": "Developer",
      "note": "Emulator core, renderer, Studio shell and documentation.",
      "pfp": ""
    }
  ],
  "supporters": [],
  "testers": []
})JSON";

struct GroupSpec {
    const char* key;
    const char* title;
};

constexpr GroupSpec GroupSpecs[] = {
    {"devs", "Devs"},
    {"supporters", "Supporters"},
    {"testers", "Testers"},
};

QString resolveAvatarPath(const QString& pfp, const QString& baseDir) {
    if (pfp.isEmpty()) return {};
    const QFileInfo info(pfp);
    if (info.isAbsolute()) return info.exists() ? pfp : QString{};
    const auto exists = [](const QString& path) {
        return !path.isEmpty() && QFileInfo::exists(path);
    };
    if (exists(baseDir + QLatin1Char('/') + pfp)) return baseDir + QLatin1Char('/') + pfp;
    if (exists(QStringLiteral(":/credits/") + pfp))
        return QStringLiteral(":/credits/") + pfp;
    const auto appDir = QCoreApplication::applicationDirPath();
    for (const auto& root : {
             appDir + QStringLiteral("/../Resources/credits"),
             appDir + QStringLiteral("/resources/credits"),
             QDir::currentPath() + QStringLiteral("/resources/credits"),
         }) {
        const auto candidate = root + QLatin1Char('/') + pfp;
        if (exists(candidate)) return candidate;
    }
    return {};
}

} // namespace

QByteArray bundledCreditsJson() {
    QFile file(QStringLiteral(":/credits/credits.json"));
    if (file.open(QIODevice::ReadOnly)) {
        const auto data = file.readAll();
        if (!data.trimmed().isEmpty()) return data;
    }
    return QByteArray(DefaultCredits);
}

CreditBook parseCredits(const QByteArray& json, const QString& source) {
    CreditBook book;
    book.source = source;
    QJsonParseError error{};
    const auto document = QJsonDocument::fromJson(json, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        book.error = error.error == QJsonParseError::NoError
            ? QStringLiteral("Credits root must be a JSON object.")
            : error.errorString();
        return book;
    }
    const auto root = document.object();
    for (const auto& spec : GroupSpecs) {
        CreditGroup group;
        group.key = QString::fromLatin1(spec.key);
        group.title = QCoreApplication::translate("CreditsView", spec.title);
        const auto value = root.value(group.key);
        if (!value.isUndefined() && !value.isArray()) {
            book.error = QStringLiteral("\"%1\" must be an array.").arg(group.key);
            return book;
        }
        for (const auto& item : value.toArray()) {
            if (!item.isObject()) continue;
            const auto object = item.toObject();
            CreditEntry entry;
            entry.name = object.value(QStringLiteral("name")).toString().trimmed();
            if (entry.name.isEmpty()) continue;
            entry.role = object.value(QStringLiteral("role")).toString().trimmed();
            entry.note = object.value(QStringLiteral("note")).toString().trimmed();
            entry.pfp = object.value(QStringLiteral("pfp")).toString().trimmed();
            group.entries.push_back(std::move(entry));
        }
        book.groups.push_back(std::move(group));
    }
    book.ok = true;
    return book;
}

QString creditInitials(const QString& name) {
    QStringList words;
    for (const auto& raw : name.split(QRegularExpression(QStringLiteral("[\\s_./-]+")),
                                      Qt::SkipEmptyParts)) {
        const auto trimmed = raw.trimmed();
        if (!trimmed.isEmpty()) words.push_back(trimmed);
    }
    if (words.isEmpty()) return QStringLiteral("?");
    const auto first = words.front().front().toUpper();
    if (words.size() == 1) {
        const auto& word = words.front();
        return word.size() >= 2 ? QString(first) + word[1].toUpper() : QString(first);
    }
    return QString(first) + words.back().front().toUpper();
}

QColor creditAccent(const QString& name) {
    // Deterministic FNV-1a hash over UTF-8, mapped to a restrained accent set.
    u32 hash = 2166136261u;
    for (const auto byte : name.toUtf8()) {
        hash ^= static_cast<u8>(byte);
        hash *= 16777619u;
    }
    static constexpr int hues[] = {210, 255, 320, 168, 32, 288};
    return QColor::fromHsv(hues[hash % std::size(hues)], 130, 205);
}

QPixmap renderCreditAvatar(const CreditEntry& entry, int size, const QString& baseDir) {
    QPixmap canvas(std::max(1, size), std::max(1, size));
    canvas.fill(Qt::transparent);
    QPainter painter(&canvas);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    const QRectF bounds(0.5, 0.5, canvas.width() - 1.0, canvas.height() - 1.0);
    QPainterPath circle;
    circle.addEllipse(bounds);

    const auto path = resolveAvatarPath(entry.pfp, baseDir);
    QPixmap source(path);
    if (!source.isNull()) {
        const auto scaled = source.scaled(canvas.size(), Qt::KeepAspectRatioByExpanding,
                                          Qt::SmoothTransformation);
        const QRect crop((scaled.width() - canvas.width()) / 2,
                         (scaled.height() - canvas.height()) / 2,
                         canvas.width(), canvas.height());
        painter.save();
        painter.setClipPath(circle);
        painter.drawPixmap(QRect(0, 0, canvas.width(), canvas.height()), scaled, crop);
        painter.restore();
    } else {
        const auto accent = creditAccent(entry.name);
        QLinearGradient fill(0, 0, canvas.width(), canvas.height());
        fill.setColorAt(0, accent.lighter(122));
        fill.setColorAt(1, accent.darker(142));
        painter.fillPath(circle, fill);
        QFont font = painter.font();
        font.setBold(true);
        font.setPixelSize(std::max(11, size / 3));
        painter.setFont(font);
        painter.setPen(QColor("#F8FAFC"));
        painter.drawText(QRectF(0, 0, canvas.width(), canvas.height()), Qt::AlignCenter,
                         creditInitials(entry.name));
    }
    painter.setPen(QPen(creditAccent(entry.name), 2.0));
    painter.setBrush(Qt::NoBrush);
    painter.drawEllipse(bounds);
    return canvas;
}

CreditsView::CreditsView(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("creditsView"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(20, 16, 20, 16);
    layout->setSpacing(12);

    auto* heading = new QLabel(tr("Credits"), this);
    heading->setProperty("deck", QStringLiteral("heading"));
    layout->addWidget(heading);
    auto* subtitle = new QLabel(
        tr("The people behind N64Forge Studio. Entries are data-driven so the list is easy to extend."),
        this);
    subtitle->setProperty("deck", QStringLiteral("subtitle"));
    subtitle->setWordWrap(true);
    layout->addWidget(subtitle);

    tabs_ = new QTabWidget(this);
    tabs_->setObjectName(QStringLiteral("creditsTabs"));
    tabs_->setDocumentMode(true);
    layout->addWidget(tabs_, 1);
    reload();
}


void CreditsView::reload() {
    QString path = qEnvironmentVariable("N64FORGE_CREDITS_JSON");
    if (path.isEmpty() || !QFileInfo::exists(path)) {
        path.clear();
        const auto appDir = QCoreApplication::applicationDirPath();
        const QStringList candidates{
            appDir + QStringLiteral("/../Resources/credits/credits.json"),
            appDir + QStringLiteral("/resources/credits/credits.json"),
            QDir::currentPath() + QStringLiteral("/resources/credits/credits.json"),
        };
        for (const auto& candidate : candidates) {
            if (QFileInfo::exists(candidate)) {
                path = candidate;
                break;
            }
        }
    }
    if (!path.isEmpty()) {
        loadFromFile(path);
        return;
    }
    book_ = parseCredits(bundledCreditsJson(), QStringLiteral(":/credits/credits.json"));
    baseDir_ = QStringLiteral(":/credits");
    rebuild();
}

void CreditsView::loadFromFile(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        book_ = {};
        book_.source = path;
        book_.error = tr("Could not open credits file: %1").arg(path);
        baseDir_ = QFileInfo(path).absolutePath();
        rebuild();
        return;
    }
    book_ = parseCredits(file.readAll(), path);
    baseDir_ = QFileInfo(path).absolutePath();
    rebuild();
}

QStringList CreditsView::groupKeys() const {
    QStringList keys;
    for (const auto& group : book_.groups) keys.push_back(group.key);
    return keys;
}

int CreditsView::cardCount(const QString& groupKey) const {
    for (const auto& group : book_.groups)
        if (group.key == groupKey) return group.entries.size();
    return 0;
}

void CreditsView::rebuild() {
    if (!tabs_) return;
    while (tabs_->count() > 0) {
        auto* page = tabs_->widget(0);
        tabs_->removeTab(0);
        page->deleteLater();
    }
    for (const auto& group : book_.groups) {
        auto* scroll = new QScrollArea(tabs_);
        scroll->setObjectName(QStringLiteral("credits%1Tab").arg(
            group.title.left(1).toUpper() + group.title.mid(1)));
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setProperty("creditGroup", group.key);
        auto* body = new QWidget(scroll);
        auto* cards = new QVBoxLayout(body);
        cards->setContentsMargins(14, 14, 14, 14);
        cards->setSpacing(10);
        cards->setAlignment(Qt::AlignTop);

        if (group.entries.isEmpty()) {
            auto* empty = new QLabel(
                tr("No %1 yet. Add one entry in credits.json — the guide above shows the format.")
                    .arg(group.title.toLower()),
                body);
            empty->setProperty("deck", QStringLiteral("subtitle"));
            empty->setWordWrap(true);
            cards->addWidget(empty);
        }
        for (const auto& entry : group.entries) {
            auto* card = new QFrame(body);
            card->setObjectName(QStringLiteral("creditCard"));
            card->setProperty("creditGroup", group.key);
            card->setProperty("creditName", entry.name);
            card->setMinimumHeight(88);
            auto* row = new QHBoxLayout(card);
            row->setContentsMargins(14, 12, 14, 12);
            row->setSpacing(14);

            auto* avatar = new QLabel(card);
            avatar->setObjectName(QStringLiteral("creditAvatar"));
            avatar->setFixedSize(AvatarSize, AvatarSize);
            avatar->setPixmap(renderCreditAvatar(entry, AvatarSize, baseDir_));
            avatar->setAttribute(Qt::WA_TransparentForMouseEvents);
            row->addWidget(avatar, 0, Qt::AlignTop);

            auto* text = new QVBoxLayout();
            text->setSpacing(4);
            auto* top = new QHBoxLayout();
            top->setSpacing(8);
            auto* name = new QLabel(entry.name, card);
            name->setObjectName(QStringLiteral("creditName"));
            QFont nameFont = name->font();
            nameFont.setBold(true);
            name->setFont(nameFont);
            top->addWidget(name, 0, Qt::AlignVCenter);
            if (!entry.role.isEmpty()) {
                auto* role = new QLabel(entry.role, card);
                role->setObjectName(QStringLiteral("creditRole"));
                const auto accent = creditAccent(entry.name);
                role->setStyleSheet(QStringLiteral(
                    "QLabel { color: %1; background: %2; border: 1px solid %3;"
                    " border-radius: 8px; padding: 1px 8px; font-size: 11px; }")
                    .arg(accent.lighter(155).name(), QColor(accent).darker(210).name(),
                         accent.name()));
                top->addWidget(role, 0, Qt::AlignVCenter);
            }
            top->addStretch(1);
            text->addLayout(top);
            if (!entry.note.isEmpty()) {
                auto* note = new QLabel(entry.note, card);
                note->setObjectName(QStringLiteral("creditNote"));
                note->setProperty("deck", QStringLiteral("subtitle"));
                note->setWordWrap(true);
                text->addWidget(note);
            }
            row->addLayout(text, 1);
            cards->addWidget(card);
        }
        cards->addStretch(1);
        scroll->setWidget(body);
        tabs_->addTab(scroll, group.title);
    }
    setProperty("creditSource", book_.source);
    setProperty("creditError", book_.error);
    setProperty("creditLoaded", book_.ok);
}

} // namespace n64forge::studio
