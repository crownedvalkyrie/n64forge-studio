#include "n64forge/studio/library_widget.hpp"
#include "n64forge/studio/game_library.hpp"
#include "n64forge/core/memory.hpp"
#include <QAction>
#include <QCursor>
#include <QDir>
#include <QEvent>
#include <QFileDialog>
#include <QFontDatabase>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QSignalBlocker>
#include <QShowEvent>
#include <QToolButton>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QVariantAnimation>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>

namespace n64forge::studio {

QPointF cartridgeParallaxOffset(const QRectF& card, QPointF pointer, bool enabled) {
    if (!enabled || card.isEmpty() || !card.contains(pointer)) return {};
    const QPointF relative = pointer - card.center();
    return {6.0 * std::clamp(relative.x() * 2 / card.width(), -1.0, 1.0),
            6.0 * std::clamp(relative.y() * 2 / card.height(), -1.0, 1.0)};
}

namespace {

constexpr int CARD_WIDTH = 200;
constexpr int CARD_HEIGHT = 260;
constexpr int COVER_HEIGHT = 176;
constexpr int CARD_SPACING = 18;
constexpr int SpecsRole = Qt::UserRole + 1;
constexpr int VerifiedRole = Qt::UserRole + 2;
constexpr int ArtworkRole = Qt::UserRole + 3;

void setHardwarePill(QListWidgetItem& item, const GameEntry& entry) {
    item.setData(VerifiedRole, !entry.videoRegion.isEmpty() && !entry.cic.isEmpty());
    item.setData(SpecsRole, LibraryWidget::tr("%1 · %2").arg(
        entry.videoRegion.isEmpty() ? LibraryWidget::tr("REGION ?") : entry.videoRegion,
        entry.cic.isEmpty() ? LibraryWidget::tr("CIC ?") : entry.cic));
}

class CartridgeDelegate final : public QStyledItemDelegate {
public:
    explicit CartridgeDelegate(QListWidget* view) : QStyledItemDelegate(view), view_(view) {
        timer_.setInterval(16);
        connect(&timer_, &QTimer::timeout, this, [this] {
            for (auto it = offsets_.begin(); it != offsets_.end();) {
                if (!it.key().isValid()) {
                    it = offsets_.erase(it);
                    continue;
                }
                const auto target = it.key() == hovered_ ? target_ : QPointF{};
                it.value() += (target - it.value()) * 0.24;
                if (QLineF(it.value(), target).length() < 0.06) it.value() = target;
                view_->viewport()->update(view_->visualRect(it.key()));
                if (it.value().isNull() && target.isNull()) it = offsets_.erase(it);
                else ++it;
            }
            if (offsets_.isEmpty() ||
                (offsets_.size() == 1 && offsets_.contains(hovered_) &&
                 offsets_.value(hovered_) == target_)) timer_.stop();
        });
    }
    bool cards = true;

    void pointAt(const QModelIndex& index, QPointF offset) {
        if (hovered_ == index && target_ == offset) return;
        hovered_ = index;
        target_ = offset;
        if (hovered_.isValid() && !offset.isNull() && !offsets_.contains(hovered_))
            offsets_.insert(hovered_, {});
        if (!offsets_.isEmpty() && !timer_.isActive()) timer_.start();
    }

    void resetMotion() {
        timer_.stop();
        offsets_.clear();
        hovered_ = QPersistentModelIndex();
        target_ = {};
        view_->viewport()->update();
    }

    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        return cards ? QSize(CARD_WIDTH, CARD_HEIGHT) : QStyledItemDelegate::sizeHint(option, index);
    }

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override {
        if (!cards) {
            QStyledItemDelegate::paint(painter, option, index);
            return;
        }
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        painter->setClipRect(option.rect);
        const auto r = option.rect.adjusted(7, 7, -7, -7);
        const auto tilt = offsets_.value(index);
        painter->translate(QRectF(r).center());
        painter->shear(tilt.x() * 0.004, tilt.y() * 0.004);
        painter->translate(-QRectF(r).center());
        const bool hover = option.state.testFlag(QStyle::State_MouseOver);
        const bool selected = option.state.testFlag(QStyle::State_Selected);
        QLinearGradient shell(r.topLeft(), r.bottomLeft());
        shell.setColorAt(0, QColor("#343944"));
        shell.setColorAt(0.25, QColor("#242832"));
        shell.setColorAt(1, QColor("#1C2029"));
        const QRectF mold(r);
        const auto x = mold.left(), y = mold.top(), w = mold.width(), h = mold.height();
        QPainterPath silhouette;
        silhouette.moveTo(x + 7, y + h);
        silhouette.quadTo(x, y + h, x, y + h - 8);
        silhouette.lineTo(x, y + 35);
        silhouette.quadTo(x, y + 25, x + 7, y + 23);
        silhouette.lineTo(x + 14, y + 20);
        silhouette.quadTo(x + 19, y + 18, x + 19, y + 11);
        silhouette.quadTo(x + 20, y, x + 32, y);
        silhouette.lineTo(x + w - 32, y);
        silhouette.quadTo(x + w - 20, y, x + w - 19, y + 11);
        silhouette.quadTo(x + w - 19, y + 18, x + w - 14, y + 20);
        silhouette.lineTo(x + w - 7, y + 23);
        silhouette.quadTo(x + w, y + 25, x + w, y + 35);
        silhouette.lineTo(x + w, y + h - 8);
        silhouette.quadTo(x + w, y + h, x + w - 7, y + h);
        silhouette.closeSubpath();
        painter->setBrush(shell);
        painter->setPen(selected ? QColor("#91A5BE") : hover ? QColor("#697281") : QColor("#4A515E"));
        painter->drawPath(silhouette);
        painter->setPen(QColor(255, 255, 255, 33));
        painter->drawLine(QPointF(x + 30, y + 4), QPointF(x + w - 30, y + 4));
        for (int ridge = 0; ridge < 3; ++ridge) {
            const auto left = x + 5 + ridge * 3, right = x + w - 5 - ridge * 3;
            painter->drawLine(QPointF(left, y + 40), QPointF(left, y + h - 31));
            painter->drawLine(QPointF(right, y + 40), QPointF(right, y + h - 31));
        }
        painter->save();
        painter->translate(tilt * 0.43);
        const QRect recess(r.x() + 18, r.y() + 18, r.width() - 36, COVER_HEIGHT - 8);
        painter->setBrush(QColor("#0D1017"));
        painter->setPen(QColor("#62617A"));
        painter->drawRoundedRect(recess, 5, 5);
        auto art = recess.adjusted(2, 2, -2, -2);
        QPainterPath cutout;
        cutout.addRoundedRect(art, 3, 3);
        painter->save();
        painter->setClipPath(cutout);
        qvariant_cast<QIcon>(index.data(Qt::DecorationRole)).paint(painter, art);
        if (!tilt.isNull()) {
            const auto glint = QRectF(art).center() + QPointF(tilt.x() * 8, tilt.y() * 5);
            const QPointF axis(57, 32 + tilt.y() * 3);
            QLinearGradient sheen(glint - axis, glint + axis);
            sheen.setColorAt(0.0, Qt::transparent);
            sheen.setColorAt(0.5, QColor(219, 216, 255, std::min(62, int(QLineF({}, tilt).length() * 10))));
            sheen.setColorAt(1.0, Qt::transparent);
            painter->fillRect(art, sheen);
        }
        painter->restore();
        painter->restore();

        painter->setPen(QColor("#F0F3FA"));
        QFont title = option.font;
        title.setBold(true);
        painter->setFont(title);
        const QRect titleRect(r.x() + 10, recess.bottom() + 4, r.width() - 20, 23);
        painter->drawText(titleRect, Qt::AlignCenter, painter->fontMetrics().elidedText(
            index.data().toString(), Qt::ElideRight, titleRect.width()));
        const QRect pill(r.x() + 10, titleRect.bottom() + 2, r.width() - 20, 21);
        painter->translate(tilt * 0.68);
        painter->setBrush(QColor("#0D1017"));
        painter->setPen(QColor("#353A50"));
        painter->drawRoundedRect(pill, 9, 9);
        QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
        mono.setPixelSize(11);
        painter->setFont(mono);
        painter->setPen(index.data(VerifiedRole).toBool() ? QColor("#D3DBE5") : QColor("#AEB5C0"));
        painter->drawText(pill.adjusted(5, 0, -5, 0), Qt::AlignRight | Qt::AlignVCenter, painter->fontMetrics().elidedText(
            index.data(SpecsRole).toString(), Qt::ElideRight, pill.width() - 8));
        painter->restore();
    }

private:
    QListWidget* view_;
    QTimer timer_;
    QHash<QPersistentModelIndex, QPointF> offsets_;
    QPersistentModelIndex hovered_;
    QPointF target_;
};

} // namespace

class DeckHero final : public QWidget {
public:
    explicit DeckHero(QWidget* parent) : QWidget(parent) {
        setObjectName(QStringLiteral("N64ForgeStudio_CoverArt"));
        setProperty("cartridgeBanner", true);
        setMinimumHeight(128);
        fade_.setDuration(320);
        fade_.setStartValue(0.0);
        fade_.setEndValue(1.0);
        fade_.setEasingCurve(QEasingCurve::OutCubic);
        connect(&fade_, &QVariantAnimation::valueChanged, this, [this](const QVariant& value) {
            blend_ = value.toReal();
            update();
        });
        connect(&fade_, &QVariantAnimation::finished, this, [this] { previous_ = {}; });
        entrance_.setDuration(460);
        entrance_.setStartValue(0.0);
        entrance_.setEndValue(1.0);
        entrance_.setEasingCurve(QEasingCurve::OutCubic);
        connect(&entrance_, &QVariantAnimation::valueChanged, this, [this](const QVariant& value) {
            reveal_ = value.toReal();
            update();
        });
        follow_.setDuration(110);
        follow_.setEasingCurve(QEasingCurve::OutCubic);
        connect(&follow_, &QVariantAnimation::valueChanged, this, [this](const QVariant& value) {
            shift_ = value.toPointF();
            update();
        });
    }

    void showEvent(QShowEvent* event) override {
        QWidget::showEvent(event);
        if (entered_) return;
        entered_ = true;
        entrance_.stop();
        reveal_ = 0;
        entrance_.start();
    }

    void setArtwork(const QIcon& art, const QString& path, bool animate) {
        if (path == path_ && art.cacheKey() == key_) return;
        path_ = path;
        key_ = art.cacheKey();
        previous_ = current_;
        current_ = path.isEmpty() ? QPixmap{} : QPixmap(path);
        if (!path.isEmpty() && current_.isNull()) current_ = art.pixmap(QSize(CARD_WIDTH, COVER_HEIGHT));
        if (!animate) {
            fade_.stop();
            previous_ = {};
            blend_ = 1;
            update();
            return;
        }
        if (previous_.isNull() && current_.isNull()) return;
        fade_.stop();
        blend_ = 0;
        fade_.start();
    }

    void follow(QPointF tilt) {
        if (current_.isNull() && previous_.isNull()) return;
        const QPointF target = tilt * -1.9;
        if (target == follow_.endValue().toPointF()) return;
        follow_.stop();
        follow_.setStartValue(shift_);
        follow_.setEndValue(target);
        follow_.start();
    }

    void resetMotion() {
        fade_.stop();
        follow_.stop();
        follow_.setEndValue(QPointF{});
        blend_ = 1;
        previous_ = {};
        shift_ = {};
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.fillRect(rect(), QColor("#111318"));
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        const QRectF overscan = QRectF(rect()).adjusted(-16, -16, 16, 16)
            .translated(shift_ + QPointF(0, 8 * (1 - reveal_)));
        const auto layer = [&](const QPixmap& art, qreal opacity) {
            if (art.isNull() || opacity <= 0) return;
            const QSizeF size = QSizeF(art.size()).scaled(overscan.size(), Qt::KeepAspectRatioByExpanding);
            const QRectF target(overscan.center() - QPointF(size.width(), size.height()) * 0.5, size);
            painter.setOpacity(opacity);
            painter.drawPixmap(target, art, art.rect());
        };
        layer(previous_, (1 - blend_) * reveal_);
        layer(current_, blend_ * reveal_);
        painter.setOpacity(1);
        QLinearGradient vignette(0, 0, width(), height());
        vignette.setColorAt(0, QColor(13, 16, 23, 246));
        vignette.setColorAt(0.58, QColor(13, 16, 23, 212));
        vignette.setColorAt(1, QColor(13, 16, 23, 169));
        painter.fillRect(rect(), vignette);
        QLinearGradient edge(0, height() * 0.6, 0, height());
        edge.setColorAt(0, Qt::transparent);
        edge.setColorAt(1, QColor("#111318"));
        painter.fillRect(rect(), edge);
    }

private:
    QVariantAnimation fade_, follow_, entrance_;
    QPixmap previous_, current_;
    QString path_;
    qint64 key_ = 0;
    qreal blend_ = 1;
    qreal reveal_ = 1;
    bool entered_ = false;
    QPointF shift_;
};

LibraryWidget::LibraryWidget(GameLibrary* library, QWidget* parent)
    : QWidget(parent)
    , library_(library) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(20, 16, 20, 16);
    layout->setSpacing(12);
    hero_ = new DeckHero(this);
    auto* heroLayout = new QVBoxLayout(hero_);
    heroLayout->setContentsMargins(14, 10, 14, 10);
    auto* heading = new QLabel(tr("Game Library"), hero_);
    heading->setProperty("deck", QStringLiteral("heading"));
    auto* subtitle = new QLabel(
        tr("Browse your cartridges. Hover or select one to inspect its hardware."), hero_);
    subtitle->setProperty("deck", QStringLiteral("subtitle"));
    subtitle->setWordWrap(true);
    heading->setAttribute(Qt::WA_TransparentForMouseEvents);
    subtitle->setAttribute(Qt::WA_TransparentForMouseEvents);
    heroLayout->addWidget(heading);
    heroLayout->addWidget(subtitle);
    layout->addWidget(hero_);

    auto* toolbar = new QHBoxLayout();
    auto* addBtn = new QPushButton(tr("Add ROMs..."), this);
    auto* scanBtn = new QPushButton(tr("Scan Folder..."), this);
    search_ = new QLineEdit(this);
    search_->setPlaceholderText(tr("Search library..."));
    search_->setClearButtonEnabled(true);
    search_->setMaximumWidth(240);
    auto* viewBtn = new QPushButton(tr("Toggle View"), this);
    toolbar->addWidget(addBtn);
    toolbar->addWidget(scanBtn);
    toolbar->addWidget(search_, 1);
    toolbar->addWidget(viewBtn);
    layout->addLayout(toolbar);

    list_ = new QListWidget(this);
    list_->setItemDelegate(new CartridgeDelegate(list_));
    list_->setMouseTracking(true);
    list_->viewport()->setMouseTracking(true);
    list_->viewport()->installEventFilter(this);
    list_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    list_->setSpacing(0);
    list_->setResizeMode(QListView::Adjust);
    list_->setWrapping(true);
    list_->setFlow(QListView::LeftToRight);
    list_->setViewMode(QListView::IconMode);
    list_->setIconSize(QSize(CARD_WIDTH, COVER_HEIGHT));
    list_->setGridSize(QSize(CARD_WIDTH + CARD_SPACING, CARD_HEIGHT + CARD_SPACING));
    list_->setContextMenuPolicy(Qt::CustomContextMenu);
    layout->addWidget(list_, 1);

    auto* inspector = new QFrame(this);
    inspector->setProperty("deck", QStringLiteral("panel"));
    auto* hardware = new QVBoxLayout(inspector);
    hardwareTitle_ = new QLabel(inspector);
    hardwareTitle_->setTextFormat(Qt::PlainText);
    hardwareTitle_->setWordWrap(true);
    hardwareTitle_->setProperty("deck", QStringLiteral("heading"));
    hardwareDetails_ = new QLabel(inspector);
    hardwareDetails_->setObjectName(QStringLiteral("cartridgeHardwareDetails"));
    hardwareDetails_->setTextFormat(Qt::PlainText);
    hardwareDetails_->setWordWrap(true);
    hardwareDetails_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    recommended_ = new QPushButton(tr("Apply Video Baseline"), inspector);
    recommended_->setObjectName(QStringLiteral("applyRecommendedSettings"));
    recommended_->setProperty("deck", QStringLiteral("primary"));
    recommended_->setToolTip(tr("Apply Studio-wide Fit Window, nearest filtering, 4:3 and no interpolation. "
                                "Cartridge save settings are unchanged."));
    hardware->addWidget(hardwareTitle_);
    hardware->addWidget(hardwareDetails_);
    recommended_->hide(); // Retain the existing StudioVideoProbe binding, not a second settings surface.
    layout->addWidget(inspector);
    connect(recommended_, &QPushButton::clicked, this, &LibraryWidget::recommendedSettingsRequested);
    connect(list_, &QListWidget::itemEntered, this, &LibraryWidget::inspectCartridge);
    connect(list_, &QListWidget::currentItemChanged, this, &LibraryWidget::inspectCartridge);

    connect(addBtn, &QPushButton::clicked, this, &LibraryWidget::addRom);
    connect(scanBtn, &QPushButton::clicked, this, &LibraryWidget::scanDirectory);
    connect(viewBtn, &QPushButton::clicked, this, [this]() { setViewMode(!cardMode_); });
    connect(search_, &QLineEdit::textChanged, this, &LibraryWidget::applyFilter);
    connect(list_, &QListWidget::itemActivated, this, [this](QListWidgetItem* item) {
        if (item) emit runRequested(item->data(Qt::UserRole).toString());
    });
    connect(list_, &QListWidget::customContextMenuRequested, this, &LibraryWidget::showContextMenu);

    if (library_) {
        connect(library_, &GameLibrary::changed, this, &LibraryWidget::refresh);
    }

    refresh();
}

void LibraryWidget::setParallaxEnabled(bool enabled) {
    if (parallaxEnabled_ == enabled) return;
    parallaxEnabled_ = enabled;
    if (!enabled) {
        static_cast<CartridgeDelegate*>(list_->itemDelegate())->resetMotion();
        hero_->resetMotion();
    }
}

bool LibraryWidget::eventFilter(QObject* watched, QEvent* event) {
    if (list_ && watched == list_->viewport() && parallaxEnabled_) {
        auto* delegate = static_cast<CartridgeDelegate*>(list_->itemDelegate());
        if (event->type() == QEvent::MouseMove) {
            const auto pos = static_cast<QMouseEvent*>(event)->position();
            auto* item = itemAt(pos.toPoint());
            const auto index = item ? list_->indexFromItem(item) : QModelIndex();
            const auto tilt = item && cardMode_
                ? cartridgeParallaxOffset(list_->visualItemRect(item), pos, true) : QPointF{};
            if (item && inspected_ != index) inspectCartridge(item);
            delegate->pointAt(index, tilt);
            hero_->follow(tilt);
        } else if (event->type() == QEvent::Leave || event->type() == QEvent::Wheel) {
            delegate->pointAt({}, {});
            hero_->follow({});
        } else if (event->type() == QEvent::Hide) {
            delegate->resetMotion();
            hero_->follow({});
        }
    }
    return QWidget::eventFilter(watched, event);
}

void LibraryWidget::refresh() {
    static_cast<CartridgeDelegate*>(list_->itemDelegate())->resetMotion();
    const auto inspectedPath = inspected_.data(Qt::UserRole).toString();
    const auto currentPath = list_->currentItem() ? list_->currentItem()->data(Qt::UserRole).toString() : QString();
    const QSignalBlocker blocker(list_);
    list_->clear();
    if (!library_) {
        inspectCartridge(nullptr);
        return;
    }

    const auto& entries = library_->entries();
    QListWidgetItem* inspected = nullptr;
    for (const auto& entry : entries) {
        auto* item = new QListWidgetItem(makeCoverPixmap(entry), entry.title, list_);
        item->setData(Qt::UserRole, entry.romPath);
        item->setData(ArtworkRole, QFile::exists(entry.coverPath) ? entry.coverPath : QString());
        setHardwarePill(*item, entry);
        item->setToolTip(tr("%1\n%2\n%3").arg(entry.title, entry.romPath,
            QLocale().formattedDataSize(entry.size)));
        item->setTextAlignment(Qt::AlignHCenter | Qt::AlignBottom);
        if (entry.romPath == currentPath) list_->setCurrentItem(item);
        if (entry.romPath == inspectedPath) inspected = item;
    }
    if (search_) applyFilter(search_->text());
    inspectCartridge(inspected && !inspected->isHidden() ? inspected : nullptr);
}

void LibraryWidget::inspectCartridge(QListWidgetItem* item) {
    inspected_ = item ? list_->indexFromItem(item) : QModelIndex();
    hero_->setArtwork(qvariant_cast<QIcon>(inspected_.data(Qt::DecorationRole)),
                      inspected_.data(ArtworkRole).toString(), parallaxEnabled_);
    const auto entry = item && library_ ? library_->inspectRom(item->data(Qt::UserRole).toString())
                                         : std::nullopt;
    recommended_->setEnabled(entry && entry->revision >= 0);
    if (!entry) {
        hardwareTitle_->setText(tr("Cartridge details"));
        hardwareDetails_->setText(tr("Hover or select a cartridge to see its video region, CIC and memory information."));
        return;
    }
    setHardwarePill(*item, *entry);
    hardwareTitle_->setText(entry->title);
    const auto unknown = tr("Unknown");
    const auto country = entry->countryCode < 0 ? unknown
        : QStringLiteral("0x%1").arg(entry->countryCode, 2, 16, QLatin1Char('0'));
    hardwareDetails_->setText(
        tr("%1  ·  %2  ·  Header region %3  ·  Revision %4  ·  %5\n"
           "%6 MiB emulated RDRAM; Expansion Pak requirement and save hardware are not declared by the ROM header.")
            .arg(entry->videoRegion.isEmpty() ? unknown : entry->videoRegion,
                 entry->cic.isEmpty() ? unknown : entry->cic, country,
                 entry->revision < 0 ? unknown : QString::number(entry->revision),
                 QStringLiteral("%1 / %2").arg(entry->format, QLocale().formattedDataSize(entry->size)))
            .arg(Memory::RdramSize / (1024 * 1024)));
}

void LibraryWidget::applyFilter(const QString& text) {
    const QString needle = text.trimmed();
    for (int i = 0; i < list_->count(); ++i) {
        auto* item = list_->item(i);
        const bool visible = needle.isEmpty() ||
            item->text().contains(needle, Qt::CaseInsensitive) ||
            item->data(Qt::UserRole).toString().contains(needle, Qt::CaseInsensitive);
        item->setHidden(!visible);
    }
    if (!needle.isEmpty()) inspectCartridge(list_->currentItem() && !list_->currentItem()->isHidden()
                                            ? list_->currentItem() : nullptr);
}

void LibraryWidget::setViewMode(bool cards) {
    cardMode_ = cards;
    static_cast<CartridgeDelegate*>(list_->itemDelegate())->cards = cards;
    static_cast<CartridgeDelegate*>(list_->itemDelegate())->resetMotion();
    if (cards) {
        list_->setViewMode(QListView::IconMode);
        list_->setIconSize(QSize(CARD_WIDTH, COVER_HEIGHT));
        list_->setGridSize(QSize(CARD_WIDTH + CARD_SPACING, CARD_HEIGHT + CARD_SPACING));
        list_->setFlow(QListView::LeftToRight);
        list_->setWrapping(true);
    } else {
        list_->setViewMode(QListView::ListMode);
        list_->setIconSize(QSize(48, 48));
        list_->setGridSize(QSize());
        list_->setFlow(QListView::TopToBottom);
        list_->setWrapping(false);
    }
}

void LibraryWidget::addRom() {
    const QString path = QFileDialog::getOpenFileName(this, tr("Add ROM"), QString(),
        tr("N64 ROMs (*.z64 *.n64 *.v64 *.zip);;All Files (*)"));
    if (path.isEmpty() || !library_) return;
    library_->addRom(path);
}

void LibraryWidget::scanDirectory() {
    const QString dir = QFileDialog::getExistingDirectory(this, tr("Scan Folder"), QString());
    if (dir.isEmpty() || !library_) return;
    const QDir d(dir);
    const QStringList filters{QStringLiteral("*.z64"), QStringLiteral("*.n64"),
                              QStringLiteral("*.v64"), QStringLiteral("*.zip")};
    const auto files = d.entryInfoList(filters, QDir::Files | QDir::NoDotAndDotDot, QDir::Name);
    for (const auto& info : files) {
        library_->addRom(info.absoluteFilePath());
    }
}

void LibraryWidget::runSelected() {
    const auto selected = list_->selectedItems();
    if (!selected.isEmpty()) {
        emit runRequested(selected.first()->data(Qt::UserRole).toString());
    }
}

void LibraryWidget::showContextMenu(const QPoint& pos) {
    auto* item = itemAt(pos);
    if (!item) return;
    const QString path = item->data(Qt::UserRole).toString();

    QMenu menu(this);
    auto* runAction = menu.addAction(tr("Run"));
    auto* coverAction = menu.addAction(tr("Set Cover Image..."));
    auto* removeAction = menu.addAction(tr("Remove"));

    QAction* chosen = menu.exec(list_->viewport()->mapToGlobal(pos));
    if (chosen == runAction) {
        emit runRequested(path);
    } else if (chosen == coverAction) {
        const QString cover = QFileDialog::getOpenFileName(this, tr("Select Cover Image"), QString(),
            tr("Images (*.png *.jpg *.jpeg *.bmp *.webp);;All Files (*)"));
        if (!cover.isEmpty() && library_) {
            library_->setCover(path, cover);
        }
    } else if (chosen == removeAction) {
        if (library_) library_->removeRom(path);
    }
}

void LibraryWidget::setCoverImage() {
    showContextMenu(list_->mapFromGlobal(QCursor::pos()));
}

void LibraryWidget::removeSelected() {
    const auto selected = list_->selectedItems();
    if (!selected.isEmpty() && library_) {
        library_->removeRom(selected.first()->data(Qt::UserRole).toString());
    }
}

QPixmap LibraryWidget::makeCoverPixmap(const GameEntry& entry) const {
    QPixmap pixmap(CARD_WIDTH, COVER_HEIGHT);
    pixmap.fill(Qt::transparent);

    if (!entry.coverPath.isEmpty() && QFile::exists(entry.coverPath)) {
        QPixmap cover(entry.coverPath);
        if (!cover.isNull()) {
            return cover.scaled(CARD_WIDTH, COVER_HEIGHT, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        }
    }

    // Placeholder: gradient card with title initial.
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    const QRect rect(0, 0, CARD_WIDTH, COVER_HEIGHT);
    QColor base = QColor::fromHsl((qHash(entry.title) % 360), 160, 60);
    painter.fillRect(rect, base);
    painter.setPen(QPen(Qt::white, 2));
    painter.drawRoundedRect(rect.adjusted(2, 2, -2, -2), 8, 8);

    QString initial = entry.title.isEmpty() ? QStringLiteral("?") : entry.title.at(0).toUpper();
    painter.setPen(Qt::white);
    QFont font = painter.font();
    font.setPointSize(48);
    font.setBold(true);
    painter.setFont(font);
    painter.drawText(rect, Qt::AlignCenter, initial);
    return pixmap;
}

QListWidgetItem* LibraryWidget::itemAt(const QPoint& pos) const {
    return list_->itemAt(pos);
}

} // namespace n64forge::studio
