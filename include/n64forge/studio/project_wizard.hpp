#pragma once
#include "n64forge/studio/libdragon_sdk.hpp"
#include <QDialog>
#include <QString>
#include <vector>

class QLineEdit;
class QComboBox;
class QPushButton;

namespace n64forge::studio {

class ProjectWizard : public QDialog {
    Q_OBJECT
public:
    explicit ProjectWizard(QWidget* parent = nullptr);

    [[nodiscard]] QString projectName() const;
    [[nodiscard]] QString projectDirectory() const;
    [[nodiscard]] LibdragonSdk selectedSdk() const;

private slots:
    void browseDirectory();
    void browseSdk();
    void refreshSdks();
    void validateAndAccept();

private:
    void setupUi();
    void populateSdks();

    QLineEdit* nameEdit_ = nullptr;
    QLineEdit* directoryEdit_ = nullptr;
    QComboBox* sdkCombo_ = nullptr;
    QPushButton* detectButton_ = nullptr;
    std::vector<LibdragonSdk> sdks_;
};

[[nodiscard]] bool createLibdragonProject(const QString& parentDir,
                                          const QString& name,
                                          const LibdragonSdk& sdk,
                                          QString* outProjectDir = nullptr);

} // namespace n64forge::studio
