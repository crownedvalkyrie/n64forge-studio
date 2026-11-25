#include "n64forge/studio/style.hpp"

#include <QColor>

namespace n64forge::studio {
namespace {
constexpr auto Window = "#14171D";
constexpr auto Base = "#0F1218";
constexpr auto Elev = "#1B2029";
constexpr auto Text = "#F1F5F9";
constexpr auto Subtle = "#CBD5E1";
constexpr auto Muted = "#94A3B8";
constexpr auto Line = "#3A4252";
constexpr auto Accent = "#5B8DEF";
} // namespace

QPalette darkPalette() {
    QPalette palette;
    palette.setColor(QPalette::Window, QColor(Window));
    palette.setColor(QPalette::WindowText, QColor(Text));
    palette.setColor(QPalette::Base, QColor(Base));
    palette.setColor(QPalette::AlternateBase, QColor(Elev));
    palette.setColor(QPalette::ToolTipBase, QColor(Elev));
    palette.setColor(QPalette::ToolTipText, QColor(Text));
    palette.setColor(QPalette::PlaceholderText, QColor(Muted));
    palette.setColor(QPalette::Text, QColor(Text));
    palette.setColor(QPalette::Button, QColor("#232936"));
    palette.setColor(QPalette::ButtonText, QColor(Text));
    palette.setColor(QPalette::BrightText, QColor("#FFFFFF"));
    palette.setColor(QPalette::Light, QColor("#4A5468"));
    palette.setColor(QPalette::Midlight, QColor("#2C3444"));
    palette.setColor(QPalette::Mid, QColor(Line));
    palette.setColor(QPalette::Dark, QColor("#0B0E13"));
    palette.setColor(QPalette::Shadow, QColor("#05070A"));
    palette.setColor(QPalette::Highlight, QColor(Accent));
    palette.setColor(QPalette::HighlightedText, QColor("#FFFFFF"));
    palette.setColor(QPalette::Link, QColor("#8AB4F8"));
    palette.setColor(QPalette::LinkVisited, QColor("#B49CF8"));
#if QT_VERSION >= QT_VERSION_CHECK(6, 6, 0)
    palette.setColor(QPalette::Accent, QColor(Accent));
#endif
    palette.setColor(QPalette::Disabled, QPalette::WindowText, QColor(Muted));
    palette.setColor(QPalette::Disabled, QPalette::Text, QColor(Muted));
    palette.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(Muted));
    return palette;
}

QString retroStyleSheet() {
    return QStringLiteral(R"(
QWidget {
    color: #F1F5F9;
}
QMainWindow, QDialog, QWidget#studioSettingsPage {
    background: #14171D;
}
QLabel, QCheckBox, QRadioButton, QGroupBox {
    color: #F1F5F9;
    background: transparent;
}
QLabel:disabled, QCheckBox:disabled, QRadioButton:disabled {
    color: #94A3B8;
}
QMenuBar, QMenu {
    background: #1B2029;
    color: #F1F5F9;
}
QMenuBar::item:selected, QMenu::item:selected {
    background: #2B3342;
    color: #FFFFFF;
}
QMenu::separator {
    height: 1px;
    background: #3A4252;
    margin: 4px 8px;
}
QToolTip {
    background: #1B2029;
    color: #F1F5F9;
    border: 1px solid #3A4252;
    padding: 4px 6px;
}
QLineEdit, QPlainTextEdit, QTextEdit, QSpinBox, QDoubleSpinBox {
    background: #0F1218;
    color: #F1F5F9;
    border: 1px solid #3A4252;
    border-radius: 5px;
    padding: 4px 7px;
    selection-background-color: #5B8DEF;
    selection-color: #FFFFFF;
}
QLineEdit:focus, QPlainTextEdit:focus, QTextEdit:focus, QSpinBox:focus, QDoubleSpinBox:focus {
    border-color: #5B8DEF;
}
QComboBox {
    background: #1B2029;
    color: #F1F5F9;
    border: 1px solid #3A4252;
    border-radius: 5px;
    padding: 4px 8px;
    min-height: 20px;
}
QComboBox:hover, QComboBox:focus {
    border-color: #5B8DEF;
}
QComboBox::drop-down {
    border: 0;
    width: 20px;
}
QComboBox QAbstractItemView {
    background: #1B2029;
    color: #F1F5F9;
    border: 1px solid #3A4252;
    selection-background-color: #2B3342;
    selection-color: #FFFFFF;
    outline: 0;
}
QComboBox QAbstractItemView::item {
    padding: 4px 8px;
}
QPushButton, QToolButton {
    background: #232936;
    color: #F1F5F9;
    border: 1px solid #3A4252;
    border-radius: 5px;
    padding: 5px 10px;
}
QPushButton:hover, QToolButton:hover {
    background: #2B3342;
    border-color: #55617A;
}
QPushButton:pressed, QToolButton:pressed {
    background: #1A2030;
    border-color: #5B8DEF;
}
QPushButton:checked, QToolButton:checked {
    background: #2B3342;
    border-color: #5B8DEF;
}
QPushButton:disabled, QToolButton:disabled {
    color: #6B7688;
    background: #1A1E27;
    border-color: #2B3240;
}
QCheckBox::indicator, QRadioButton::indicator {
    width: 14px;
    height: 14px;
    border: 1px solid #55617A;
    background: #0F1218;
}
QCheckBox::indicator { border-radius: 3px; }
QRadioButton::indicator { border-radius: 7px; }
QCheckBox::indicator:checked, QRadioButton::indicator:checked {
    background: #5B8DEF;
    border-color: #8AB4F8;
}
QCheckBox::indicator:disabled, QRadioButton::indicator:disabled {
    background: #1B2029;
    border-color: #3A4252;
}
QGroupBox {
    border: 1px solid #3A4252;
    border-radius: 6px;
    margin-top: 10px;
    padding-top: 8px;
}
QGroupBox::title {
    subcontrol-origin: margin;
    left: 10px;
    padding: 0 4px;
    color: #CBD5E1;
}
QTabWidget::pane {
    border: 1px solid #3A4252;
    border-radius: 6px;
    background: #14171D;
}
QTabBar::tab {
    background: #1B2029;
    color: #CBD5E1;
    border: 1px solid #3A4252;
    border-bottom: 0;
    padding: 7px 14px;
    margin-right: 2px;
    border-top-left-radius: 5px;
    border-top-right-radius: 5px;
}
QTabBar::tab:hover {
    color: #F1F5F9;
    background: #232936;
}
QTabBar::tab:selected {
    color: #FFFFFF;
    background: #2B3342;
    border-color: #5B8DEF;
}
QTabBar::tab:disabled {
    color: #6B7688;
}
QHeaderView::section {
    background: #1B2029;
    color: #CBD5E1;
    border: 0;
    border-right: 1px solid #3A4252;
    border-bottom: 1px solid #3A4252;
    padding: 5px 8px;
}
QListView, QTreeView, QTableView {
    background: #0F1218;
    color: #F1F5F9;
    border: 1px solid #3A4252;
    alternate-background-color: #151922;
    selection-background-color: #2B3342;
    selection-color: #FFFFFF;
}
QScrollBar:vertical {
    background: #14171D;
    width: 11px;
    margin: 0;
}
QScrollBar:horizontal {
    background: #14171D;
    height: 11px;
    margin: 0;
}
QScrollBar::handle:vertical, QScrollBar::handle:horizontal {
    background: #3A4252;
    border-radius: 5px;
    min-height: 24px;
    min-width: 24px;
}
QScrollBar::handle:hover {
    background: #55617A;
}
QScrollBar::add-line, QScrollBar::sub-line {
    width: 0;
    height: 0;
}
QScrollBar::add-page, QScrollBar::sub-page {
    background: transparent;
}
QSlider::groove:horizontal {
    background: #0F1218;
    border: 1px solid #3A4252;
    height: 6px;
    border-radius: 3px;
}
QSlider::handle:horizontal {
    background: #CBD5E1;
    border: 1px solid #8AB4F8;
    width: 14px;
    margin: -5px 0;
    border-radius: 7px;
}
QSlider::sub-page:horizontal {
    background: #5B8DEF;
    border-radius: 3px;
}
QStatusBar {
    background: #1B2029;
    color: #CBD5E1;
}
QToolBar#mainToolBar {
    background: #1B2029;
    border: 0;
    border-bottom: 1px solid #3A4252;
    spacing: 12px;
    padding: 8px 16px;
}
QToolBar#mainToolBar QToolButton {
    padding: 6px 9px;
    margin: 0 2px;
    border: 1px solid transparent;
    border-radius: 5px;
    background: transparent;
}
QToolBar#mainToolBar QToolButton:hover,
QToolBar#mainToolBar QToolButton:focus {
    border-color: #55617A;
    background: #232936;
}
QToolBar#mainToolBar QToolButton:checked {
    background: #2B3342;
    border-color: #5B8DEF;
    font-weight: 600;
}
QToolBar#mainToolBar QToolButton:disabled {
    color: #6B7688;
}
QFrame[deck="panel"] {
    border: 1px solid #3A4252;
    border-radius: 6px;
    background: #1B2029;
}
QLabel[deck="heading"] {
    font-weight: 600;
    color: #F1F5F9;
}
QLabel[deck="subtitle"] {
    color: #CBD5E1;
}
QScrollArea {
    background: transparent;
    border: 0;
}
QScrollArea > QWidget > QWidget {
    background: transparent;
}
QLabel[deck="telemetry"], QPlainTextEdit {
    font-family: "Menlo", "Consolas", monospace;
}
QWidget[cartridgeBanner="true"] QLabel {
    color: #E7ECF3;
}
QWidget[cartridgeBanner="true"] QLabel[deck="heading"] {
    font-size: 21px;
}
QWidget#creditsView {
    background: #14171D;
}
QFrame#creditCard {
    background: #1B2029;
    border: 1px solid #3A4252;
    border-radius: 8px;
}
)");
}

} // namespace n64forge::studio
