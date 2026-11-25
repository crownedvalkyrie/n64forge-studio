#pragma once

#include "n64forge/input/controller.hpp"
#include "n64forge/studio/controller_manager.hpp"
#include <QDialog>
#include <QKeyEvent>
#include <QTableWidget>
#include <QComboBox>
#include <QSlider>
#include <QLabel>

namespace n64forge::studio {

class ControlsDialog : public QDialog {
    Q_OBJECT
public:
    explicit ControlsDialog(ControllerManager& manager, QWidget* parent = nullptr);

protected:
    void keyPressEvent(QKeyEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void timerEvent(QTimerEvent* event) override;

private slots:
    void onPlayerChanged(int index);
    void onDeviceChanged(int port, int index);
    void onDeadzoneChanged(int port, int value);
    void onSensitivityChanged(int port, int value);
    void onBindingSelected();
    void onCaptureKey();
    void onCaptureGamepad();
    void onClearBinding();
    void onResetDefaults();
    void onAccept();

private:
    void populatePlayerCombo();
    void populateDeviceCombo(int port);
    void populateBindingsTable(int port);
    void refreshDevices();
    void updateSliders();
    void updateBindingRow(int row, input::Control control, int port);
    void finishGamepadCapture();

    ControllerManager& manager_;
    QComboBox* playerCombo_ = nullptr;
    QComboBox* deviceCombo_ = nullptr;
    QComboBox* pakCombo_ = nullptr;
    QSlider* deadzoneSlider_ = nullptr;
    QLabel* deadzoneLabel_ = nullptr;
    QSlider* sensitivitySlider_ = nullptr;
    QLabel* sensitivityLabel_ = nullptr;
    QTableWidget* table_ = nullptr;
    QPushButton* captureKeyBtn_ = nullptr;
    QPushButton* captureGamepadBtn_ = nullptr;
    QPushButton* clearBtn_ = nullptr;
    QPushButton* resetBtn_ = nullptr;
    QPushButton* okBtn_ = nullptr;
    QPushButton* cancelBtn_ = nullptr;

    int currentPort_ = 0;
    int capturingRow_ = -1;
    int captureTimer_ = 0;
    bool capturingKey_ = false;
};

} // namespace n64forge::studio
