#pragma once

#include "n64forge/studio/game_library.hpp"
#include <QListWidget>
#include <QPersistentModelIndex>
#include <QWidget>

class QLabel;
class QLineEdit;
class QPushButton;

namespace n64forge::studio {

class GameLibrary;
class DeckHero;

// A neutral result for disabled effects or for a pointer outside the card.
[[nodiscard]] QPointF cartridgeParallaxOffset(const QRectF& card, QPointF pointer, bool enabled);

class LibraryWidget : public QWidget {
    Q_OBJECT
public:
    explicit LibraryWidget(GameLibrary* library, QWidget* parent = nullptr);
    void setParallaxEnabled(bool enabled);

signals:
    void runRequested(const QString& romPath);
    void recommendedSettingsRequested();

private slots:
    void refresh();
    void setViewMode(bool cards);
    void addRom();
    void scanDirectory();
    void runSelected();
    void showContextMenu(const QPoint& pos);
    void setCoverImage();
    void removeSelected();
    void applyFilter(const QString& text);

private:
    bool eventFilter(QObject* watched, QEvent* event) override;
    QPixmap makeCoverPixmap(const GameEntry& entry) const;
    void inspectCartridge(QListWidgetItem* item);
    QListWidgetItem* itemAt(const QPoint& pos) const;

    GameLibrary* library_ = nullptr;
    QListWidget* list_ = nullptr;
    QLineEdit* search_ = nullptr;
    QLabel* hardwareTitle_ = nullptr;
    QLabel* hardwareDetails_ = nullptr;
    QPushButton* recommended_ = nullptr;
    DeckHero* hero_ = nullptr;
    QPersistentModelIndex inspected_;
    bool parallaxEnabled_ = true;
    bool cardMode_ = true;
};

} // namespace n64forge::studio
