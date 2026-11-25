#include "n64forge/studio/controls_dialog.hpp"
#include "n64forge/input/controller.hpp"
#ifdef N64FORGE_HAS_SDL3
#include "n64forge/input/sdl_input_manager.hpp"
#endif
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QMessageBox>
#include <QApplication>
#include <QSignalBlocker>

namespace n64forge::studio {

using n64forge::input::Control;
using n64forge::input::ControlCount;
using n64forge::input::DigitalControls;
using n64forge::input::ControlButtons;
using n64forge::input::controlName;
#ifdef N64FORGE_HAS_SDL3
using n64forge::input::PadBinding;
#endif

ControlsDialog::ControlsDialog(ControllerManager& manager, QWidget* parent)
    : QDialog(parent), manager_(manager) {
    setWindowTitle(tr("Controls / Keybindings"));
    setMinimumSize(720, 580);
    setModal(true);

    auto* mainLayout = new QVBoxLayout(this);

    // Port selector and device settings
    auto* portLayout = new QFormLayout();
    playerCombo_ = new QComboBox(this);
    populatePlayerCombo();
    connect(playerCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &ControlsDialog::onPlayerChanged);
    portLayout->addRow(tr("Controller Port:"), playerCombo_);

    deviceCombo_ = new QComboBox(this);
    connect(deviceCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int idx) { onDeviceChanged(currentPort_, idx); });
    portLayout->addRow(tr("Gamepad Device:"), deviceCombo_);
    pakCombo_ = new QComboBox(this);
    pakCombo_->addItem(tr("None"));
    pakCombo_->addItem(tr("Controller Pak"));
    pakCombo_->addItem(tr("Rumble Pak"));
    pakCombo_->setToolTip(tr("Accessory changes apply on the next ROM boot or reset."));
    portLayout->addRow(tr("Controller Accessory:"), pakCombo_);
    connect(pakCombo_, &QComboBox::currentIndexChanged, this, [this](int) {
        manager_.setPakOptions(currentPort_, static_cast<PakType>(pakCombo_->currentIndex()),
                               manager_.rumbleIntensity(currentPort_));
    });

    deadzoneSlider_ = new QSlider(Qt::Horizontal, this);
    deadzoneSlider_->setRange(0, 95);
    deadzoneSlider_->setSingleStep(1);
    deadzoneSlider_->setPageStep(5);
    connect(deadzoneSlider_, &QSlider::valueChanged, this, [this](int v) { onDeadzoneChanged(currentPort_, v); });
    deadzoneLabel_ = new QLabel(this);
    auto* deadzoneLayout = new QHBoxLayout();
    deadzoneLayout->addWidget(deadzoneSlider_);
    deadzoneLayout->addWidget(deadzoneLabel_);
    portLayout->addRow(tr("Deadzone:"), deadzoneLayout);

    sensitivitySlider_ = new QSlider(Qt::Horizontal, this);
    sensitivitySlider_->setRange(50, 200);
    sensitivitySlider_->setSingleStep(5);
    sensitivitySlider_->setPageStep(10);
    connect(sensitivitySlider_, &QSlider::valueChanged, this, [this](int v) { onSensitivityChanged(currentPort_, v); });
    sensitivityLabel_ = new QLabel(this);
    auto* sensLayout = new QHBoxLayout();
    sensLayout->addWidget(sensitivitySlider_);
    sensLayout->addWidget(sensitivityLabel_);
    portLayout->addRow(tr("Sensitivity:"), sensLayout);

    mainLayout->addLayout(portLayout);

    // Bindings table
    table_ = new QTableWidget(this);
    table_->setColumnCount(4);
    table_->setHorizontalHeaderLabels(QStringList{tr("Control"), tr("Keyboard"), tr("Gamepad"), tr("")});
    table_->horizontalHeader()->setStretchLastSection(false);
    table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    table_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    table_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    table_->verticalHeader()->setVisible(false);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    connect(table_, &QTableWidget::itemSelectionChanged, this, &ControlsDialog::onBindingSelected);
    mainLayout->addWidget(table_);

    // Capture buttons
    auto* captureLayout = new QHBoxLayout();
    captureKeyBtn_ = new QPushButton(tr("Capture Key"), this);
    captureKeyBtn_->setEnabled(false);
    connect(captureKeyBtn_, &QPushButton::clicked, this, &ControlsDialog::onCaptureKey);
    captureLayout->addWidget(captureKeyBtn_);

    captureGamepadBtn_ = new QPushButton(tr("Capture Gamepad"), this);
    captureGamepadBtn_->setEnabled(false);
    connect(captureGamepadBtn_, &QPushButton::clicked, this, &ControlsDialog::onCaptureGamepad);
    captureLayout->addWidget(captureGamepadBtn_);

    clearBtn_ = new QPushButton(tr("Clear Binding"), this);
    clearBtn_->setEnabled(false);
    connect(clearBtn_, &QPushButton::clicked, this, &ControlsDialog::onClearBinding);
    captureLayout->addWidget(clearBtn_);

    captureLayout->addStretch();

    resetBtn_ = new QPushButton(tr("Reset to Defaults"), this);
    connect(resetBtn_, &QPushButton::clicked, this, &ControlsDialog::onResetDefaults);
    captureLayout->addWidget(resetBtn_);

    mainLayout->addLayout(captureLayout);

    // Dialog buttons
    auto* btnLayout = new QHBoxLayout();
    btnLayout->addStretch();
    okBtn_ = new QPushButton(tr("OK"), this);
    connect(okBtn_, &QPushButton::clicked, this, &ControlsDialog::onAccept);
    btnLayout->addWidget(okBtn_);

    cancelBtn_ = new QPushButton(tr("Cancel"), this);
    connect(cancelBtn_, &QPushButton::clicked, this, &QDialog::reject);
    btnLayout->addWidget(cancelBtn_);

    mainLayout->addLayout(btnLayout);

    populateDeviceCombo(currentPort_);
    populateBindingsTable(currentPort_);
    updateSliders();
}

void ControlsDialog::showEvent(QShowEvent* event) {
    QDialog::showEvent(event);
    refreshDevices();
    populateDeviceCombo(currentPort_);
    populateBindingsTable(currentPort_);
    updateSliders();
}

void ControlsDialog::populatePlayerCombo() {
    playerCombo_->clear();
    for (int i = 0; i < 4; ++i) {
        playerCombo_->addItem(tr("Port %1").arg(i + 1), i);
    }
}

void ControlsDialog::refreshDevices() {
#ifdef N64FORGE_HAS_SDL3
    auto& gamepads = manager_.gamepads();
    gamepads.update();
#endif
}

void ControlsDialog::populateDeviceCombo(int port) {
    const QSignalBlocker block(deviceCombo_);
    deviceCombo_->clear();
    deviceCombo_->addItem(tr("Auto-detect"), "");
    deviceCombo_->addItem(tr("None (Keyboard Only)"), "-");

#ifdef N64FORGE_HAS_SDL3
    auto& gamepads = manager_.gamepads();
    const auto devices = gamepads.devices();
    for (const auto& dev : devices) {
        deviceCombo_->addItem(QString::fromStdString(dev.name), QString::fromStdString(dev.key));
    }
#endif

    // Select currently assigned device
    const auto& assigned = manager_.assignedDevice(port);
    int idx = deviceCombo_->findData(QString::fromStdString(assigned));
    if (idx < 0) {
        deviceCombo_->addItem(tr("Assigned device (disconnected)"), QString::fromStdString(assigned));
        idx = deviceCombo_->count() - 1;
    }
    if (idx >= 0) deviceCombo_->setCurrentIndex(idx);
#ifndef N64FORGE_HAS_SDL3
    deviceCombo_->setEnabled(false);
#endif
}

void ControlsDialog::populateBindingsTable(int port) {
    table_->setRowCount(static_cast<int>(ControlCount));
    const auto& mappings = manager_.mappings(port);
#ifdef N64FORGE_HAS_SDL3
    const auto& profile = manager_.profile(port);
#endif

    for (unsigned i = 0; i < ControlCount; ++i) {
        const Control control = static_cast<Control>(i);
        const QString name = QString::fromUtf8(controlName(control));

        // Control name
        auto* nameItem = new QTableWidgetItem(name);
        nameItem->setFlags(nameItem->flags() & ~Qt::ItemIsEditable);
        table_->setItem(static_cast<int>(i), 0, nameItem);

        // Keyboard binding
        QString kbdText = tr("(none)");
        for (const auto& km : mappings) {
            if (km.control == control) {
                kbdText = manager_.keyName(km.key);
                break;
            }
        }
        auto* kbdItem = new QTableWidgetItem(kbdText);
        kbdItem->setFlags(kbdItem->flags() & ~Qt::ItemIsEditable);
        table_->setItem(static_cast<int>(i), 1, kbdItem);

        // Gamepad binding
        QString gpText = tr("(none)");
#ifdef N64FORGE_HAS_SDL3
        if (i < profile.bindings.size()) {
            const auto& binding = profile.bindings[i];
            if (binding.kind != PadBinding::None) {
                if (binding.kind == PadBinding::Button) {
                    gpText = QString("Button %1").arg(binding.index);
                } else if (binding.kind == PadBinding::PositiveAxis) {
                    gpText = QString("Axis %1+").arg(binding.index);
                } else if (binding.kind == PadBinding::NegativeAxis) {
                    gpText = QString("Axis %1-").arg(binding.index);
                }
            }
        }
#endif
        auto* gpItem = new QTableWidgetItem(gpText);
        gpItem->setFlags(gpItem->flags() & ~Qt::ItemIsEditable);
        table_->setItem(static_cast<int>(i), 2, gpItem);

        // Clear button placeholder (handled by capture buttons)
        auto* actionItem = new QTableWidgetItem("");
        actionItem->setFlags(actionItem->flags() & ~Qt::ItemIsEditable);
        table_->setItem(static_cast<int>(i), 3, actionItem);
    }
}

void ControlsDialog::updateBindingRow(int row, Control control, int port) {
    const auto& mappings = manager_.mappings(port);

    QString kbdText = tr("(none)");
    for (const auto& km : mappings) {
        if (km.control == control) {
            kbdText = manager_.keyName(km.key);
            break;
        }
    }
    if (auto* item = table_->item(row, 1)) item->setText(kbdText);

    QString gpText = tr("(none)");
#ifdef N64FORGE_HAS_SDL3
    const auto& profile = manager_.profile(port);
    if (static_cast<unsigned>(row) < profile.bindings.size()) {
        const auto& binding = profile.bindings[row];
        if (binding.kind != PadBinding::None) {
            if (binding.kind == PadBinding::Button) {
                gpText = QString("Button %1").arg(binding.index);
            } else if (binding.kind == PadBinding::PositiveAxis) {
                gpText = QString("Axis %1+").arg(binding.index);
            } else if (binding.kind == PadBinding::NegativeAxis) {
                gpText = QString("Axis %1-").arg(binding.index);
            }
        }
    }
#endif
    if (auto* item = table_->item(row, 2)) item->setText(gpText);
}

void ControlsDialog::updateSliders() {
    const QSignalBlocker pakBlock(pakCombo_);
    pakCombo_->setCurrentIndex(static_cast<int>(manager_.pakType(currentPort_)));
#ifdef N64FORGE_HAS_SDL3
    const auto& profile = manager_.profile(currentPort_);
    deadzoneSlider_->blockSignals(true);
    deadzoneSlider_->setValue(static_cast<int>(std::round(profile.deadzone * 100)));
    deadzoneLabel_->setText(QString("%1%").arg(deadzoneSlider_->value()));
    deadzoneSlider_->blockSignals(false);

    sensitivitySlider_->blockSignals(true);
    sensitivitySlider_->setValue(static_cast<int>(std::round(profile.sensitivity * 100)));
    sensitivityLabel_->setText(QString("%1%").arg(sensitivitySlider_->value()));
    sensitivitySlider_->blockSignals(false);
#else
    deadzoneSlider_->setEnabled(false);
    sensitivitySlider_->setEnabled(false);
    deadzoneLabel_->setText(tr("N/A (SDL3 not available)"));
    sensitivityLabel_->setText(tr("N/A (SDL3 not available)"));
#endif
}

void ControlsDialog::onPlayerChanged(int index) {
    currentPort_ = playerCombo_->currentData().toInt();
    populateDeviceCombo(currentPort_);
    populateBindingsTable(currentPort_);
    updateSliders();
}

void ControlsDialog::onDeviceChanged(int port, int index) {
    if (index < 0) return;
    const QString key = deviceCombo_->itemData(index).toString();
    manager_.setAssignedDevice(port, key.toStdString());
    populateDeviceCombo(port);
}

void ControlsDialog::onDeadzoneChanged(int port, int value) {
#ifdef N64FORGE_HAS_SDL3
    auto profile = manager_.profile(port);
    profile.deadzone = value / 100.0;
    deadzoneLabel_->setText(QString("%1%").arg(value));
    manager_.setProfile(port, profile);
#endif
}

void ControlsDialog::onSensitivityChanged(int port, int value) {
#ifdef N64FORGE_HAS_SDL3
    auto profile = manager_.profile(port);
    profile.sensitivity = value / 100.0;
    sensitivityLabel_->setText(QString("%1%").arg(value));
    manager_.setProfile(port, profile);
#endif
}

void ControlsDialog::onBindingSelected() {
    const int row = table_->currentRow();
    capturingRow_ = row;
    const bool hasSelection = row >= 0 && row < static_cast<int>(ControlCount);
    captureKeyBtn_->setEnabled(hasSelection);
#ifdef N64FORGE_HAS_SDL3
    captureGamepadBtn_->setEnabled(hasSelection);
#endif
    clearBtn_->setEnabled(hasSelection);
}

void ControlsDialog::onCaptureKey() {
    if (capturingRow_ < 0 || capturingRow_ >= static_cast<int>(ControlCount)) return;
    capturingKey_ = true;
    setFocus(Qt::OtherFocusReason);
    grabKeyboard();
    // The key will be captured in keyPressEvent
}

void ControlsDialog::onCaptureGamepad() {
#ifdef N64FORGE_HAS_SDL3
    if (captureTimer_ || capturingRow_ < 0 || capturingRow_ >= static_cast<int>(ControlCount)) return;
    captureTimer_ = startTimer(30);
    if (!captureTimer_) return;
    captureGamepadBtn_->setText(tr("Waiting for input..."));
    captureKeyBtn_->setEnabled(false);
    clearBtn_->setEnabled(false);
    table_->setEnabled(false);
    deviceCombo_->setEnabled(false);
    playerCombo_->setEnabled(false);
    setFocus(Qt::OtherFocusReason);
#endif
}

void ControlsDialog::onClearBinding() {
    if (capturingRow_ < 0 || capturingRow_ >= static_cast<int>(ControlCount)) return;
    const Control control = static_cast<Control>(capturingRow_);
    manager_.unbind(currentPort_, control);
#ifdef N64FORGE_HAS_SDL3
    auto profile = manager_.profile(currentPort_);
    if (static_cast<unsigned>(capturingRow_) < profile.bindings.size()) {
        profile.bindings[capturingRow_] = PadBinding{};
        manager_.setProfile(currentPort_, profile);
    }
#endif
    populateBindingsTable(currentPort_);
}

void ControlsDialog::onResetDefaults() {
    auto reply = QMessageBox::question(this, tr("Reset to Defaults"),
        tr("This will reset all bindings for all ports to defaults. Continue?"),
        QMessageBox::Yes | QMessageBox::No);
    if (reply != QMessageBox::Yes) return;

    for (int i = 0; i < 4; ++i) {
        manager_.setMappings(i, ControllerManager::defaults(i));
#ifdef N64FORGE_HAS_SDL3
        manager_.setProfile(i, n64forge::input::PadProfile::defaults());
#endif
    }
    populateBindingsTable(currentPort_);
    updateSliders();
}

void ControlsDialog::onAccept() {
    manager_.saveSettings();
    accept();
}

void ControlsDialog::keyPressEvent(QKeyEvent* event) {
    if (captureTimer_ && event->key() == Qt::Key_Escape) {
        finishGamepadCapture();
        event->accept();
        return;
    }
    if (capturingKey_ && capturingRow_ >= 0 && capturingRow_ < static_cast<int>(ControlCount)) {
        if (event->isAutoRepeat()) return;
        const int key = event->key();
        if (key != Qt::Key_Escape &&
            key != Qt::Key_Control && key != Qt::Key_Alt && key != Qt::Key_Meta) {
            const Control control = static_cast<Control>(capturingRow_);
            manager_.bind(currentPort_, control, key);
            populateBindingsTable(currentPort_);
        }
        capturingKey_ = false;
        capturingRow_ = -1;
        releaseKeyboard();
        table_->clearSelection();
        event->accept();
        return;
    }
    QDialog::keyPressEvent(event);
}

void ControlsDialog::finishGamepadCapture() {
    if (!captureTimer_) return;
    killTimer(captureTimer_);
    captureTimer_ = 0;
    captureGamepadBtn_->setText(tr("Capture Gamepad"));
    captureKeyBtn_->setEnabled(true);
    clearBtn_->setEnabled(true);
    table_->setEnabled(true);
    deviceCombo_->setEnabled(true);
    playerCombo_->setEnabled(true);
}

void ControlsDialog::timerEvent(QTimerEvent* event) {
#ifdef N64FORGE_HAS_SDL3
    if (captureTimer_ && event->timerId() == captureTimer_) {
        auto& gamepads = manager_.gamepads();
        gamepads.update();

        const auto& assigned = manager_.assignedDevice(currentPort_);
        for (const auto& device : gamepads.devices()) {
            if (!assigned.empty() && device.key != assigned) continue;
            const auto* snap = gamepads.pad(device.slot);
            if (!snap) continue;
            if (auto binding = snap->activeBinding()) {
                auto profile = manager_.profile(currentPort_);
                if (static_cast<unsigned>(capturingRow_) < profile.bindings.size()) {
                    profile.bindings[capturingRow_] = *binding;
                    manager_.setProfile(currentPort_, profile);
                }
                finishGamepadCapture();
                populateBindingsTable(currentPort_);
                return;
            }
        }
        return;
    }
#endif
    QDialog::timerEvent(event);
}

} // namespace n64forge::studio
