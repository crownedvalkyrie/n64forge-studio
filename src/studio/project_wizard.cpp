#include "n64forge/studio/project_wizard.hpp"
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QVBoxLayout>
#include <QtGlobal>
#include <fstream>

namespace n64forge::studio {

namespace {

QString projectNamePattern() {
    return QStringLiteral("^[A-Za-z0-9_][A-Za-z0-9_.-]*$");
}

QString defaultProjectParentDir() {
    return QDir::homePath();
}

const char* mainCTemplate = R"(#include <libdragon.h>

/*
 * Minimal N64Forge homebrew project.
 *
 * Add libdragon initialization and game logic for the selected SDK version.
 * See https://libdragon.dev/ref/ for the exact API.
 */
int main(void) {
    /* TODO: initialize display, controllers, audio, etc. */
    while (1) {
    }
    return 0;
}
)";

} // namespace

bool isValidProjectName(const QString& name) {
    static const QRegularExpression pattern(projectNamePattern());
    const QRegularExpressionMatch match = pattern.match(name);
    // QRegularExpression '$' can match before a trailing newline; require a
    // full-string match so metacharacters and whitespace cannot slip through.
    return match.hasMatch() && match.capturedLength(0) == name.size();
}

ProjectWizard::ProjectWizard(QWidget* parent) : QDialog(parent) {
    setWindowTitle(tr("New N64 Project"));
    setMinimumWidth(500);
    setupUi();
    refreshSdks();
}

void ProjectWizard::setupUi() {
    auto* layout = new QVBoxLayout(this);
    auto* form = new QFormLayout();

    nameEdit_ = new QLineEdit(this);
    nameEdit_->setPlaceholderText(tr("MyN64Game"));
    nameEdit_->setValidator(new QRegularExpressionValidator(
        QRegularExpression(projectNamePattern()), this));
    form->addRow(tr("Project name:"), nameEdit_);

    auto* dirLayout = new QHBoxLayout();
    directoryEdit_ = new QLineEdit(this);
    directoryEdit_->setText(defaultProjectParentDir());
    auto* dirButton = new QPushButton(tr("Browse..."), this);
    connect(dirButton, &QPushButton::clicked, this, &ProjectWizard::browseDirectory);
    dirLayout->addWidget(directoryEdit_);
    dirLayout->addWidget(dirButton);
    form->addRow(tr("Parent directory:"), dirLayout);

    auto* sdkLayout = new QHBoxLayout();
    sdkCombo_ = new QComboBox(this);
    sdkCombo_->setMinimumWidth(300);
    auto* sdkButton = new QPushButton(tr("Browse SDK..."), this);
    connect(sdkButton, &QPushButton::clicked, this, &ProjectWizard::browseSdk);
    detectButton_ = new QPushButton(tr("Detect"), this);
    connect(detectButton_, &QPushButton::clicked, this, &ProjectWizard::refreshSdks);
    sdkLayout->addWidget(sdkCombo_, 1);
    sdkLayout->addWidget(sdkButton);
    sdkLayout->addWidget(detectButton_);
    form->addRow(tr("libdragon SDK:"), sdkLayout);

    layout->addLayout(form);

    auto* help = new QLabel(
        tr("The libdragon SDK must be installed separately. Set N64_INST or browse to the SDK root."),
        this);
    help->setWordWrap(true);
    layout->addWidget(help);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &ProjectWizard::validateAndAccept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}

void ProjectWizard::browseDirectory() {
    const QString dir = QFileDialog::getExistingDirectory(
        this, tr("Select Parent Directory"), directoryEdit_->text());
    if (!dir.isEmpty()) directoryEdit_->setText(dir);
}

void ProjectWizard::browseSdk() {
    const QString dir = QFileDialog::getExistingDirectory(
        this, tr("Select libdragon SDK Root"), QDir::homePath());
    if (dir.isEmpty()) return;
    LibdragonSdk sdk = LibdragonSdkDetector::detect(dir);
    if (!sdk.isValid()) {
        QMessageBox::warning(this, tr("Invalid SDK"), sdk.error);
        return;
    }
    sdks_.push_back(sdk);
    sdkCombo_->addItem(sdk.rootPath + QStringLiteral(" (") + sdk.version + QLatin1Char(')'));
    sdkCombo_->setCurrentIndex(sdkCombo_->count() - 1);
}

void ProjectWizard::refreshSdks() {
    sdkCombo_->clear();
    sdks_.clear();

    LibdragonSdk sdk = LibdragonSdkDetector::autodetect();
    if (sdk.isValid()) {
        sdks_.push_back(sdk);
        sdkCombo_->addItem(sdk.rootPath + QStringLiteral(" (") + sdk.version + QLatin1Char(')'));
    } else {
        sdkCombo_->addItem(tr("No SDK detected"));
        sdkCombo_->setEnabled(false);
    }
}

void ProjectWizard::validateAndAccept() {
    if (!isValidProjectName(projectName())) {
        QMessageBox::warning(this, tr("Invalid Name"),
            tr("Use letters, digits, '_', '.', or '-'. The first character must be a letter, "
               "digit, or '_'."));
        return;
    }
    if (!selectedSdk().isValid()) {
        QMessageBox::warning(this, tr("Invalid SDK"), tr("Select a valid libdragon SDK."));
        return;
    }
    accept();
}

QString ProjectWizard::projectName() const {
    return nameEdit_->text().trimmed();
}

QString ProjectWizard::projectDirectory() const {
    return directoryEdit_->text().trimmed();
}

LibdragonSdk ProjectWizard::selectedSdk() const {
    const int idx = sdkCombo_->currentIndex();
    if (idx >= 0 && idx < static_cast<int>(sdks_.size())) return sdks_[idx];
    return {};
}

bool createLibdragonProject(const QString& parentDir,
                            const QString& name,
                            const LibdragonSdk& sdk,
                            QString* outProjectDir) {
    if (!isValidProjectName(name)) return false;

    QDir base(parentDir);
    if (!base.exists()) return false;
    if (!base.mkpath(name)) return false;

    QDir project(base);
    if (!project.cd(name)) return false;

    if (!project.mkpath(QStringLiteral("src"))) return false;

    const QString mainC = project.absoluteFilePath(QStringLiteral("src/main.c"));
    {
        std::ofstream file(mainC.toUtf8().constData());
        if (!file.is_open()) return false;
        file << mainCTemplate;
    }

    const QString makefile = project.absoluteFilePath(QStringLiteral("Makefile"));
    {
        std::ofstream file(makefile.toUtf8().constData());
        if (!file.is_open()) return false;
        file << "ROMNAME := " << name.toUtf8().constData() << "\n";
        file << "\nBUILD_DIR = build\n";
        file << "\nifeq ($(N64_INST),)\n";
        file << "\t$(error N64_INST is not set)\n";
        file << "endif\n";
        file << "\t$(info Using N64_INST=$(N64_INST))\n";
        file << "ifeq ($(wildcard $(N64_INST)/include/n64.mk),)\n";
        file << "\t$(error $(N64_INST)/include/n64.mk does not exist: N64_INST may be incorrect)\n";
        file << "endif\n";
        file << "\n";
        file << "# Including n64.mk is of primary importance. It essentially contains the build system.\n";
        file << "# Do not hand-roll your own make rules as they may break with libdragon updates.\n";
        file << "include $(N64_INST)/include/n64.mk\n";
        file << "\n";
        file << "all: $(ROMNAME).z64\n";
        file << ".PHONY: all\n";
        file << "\n";
        file << "C_FILES := $(shell find src -name '*.c')\n";
        file << "OBJS := $(addprefix $(BUILD_DIR)/,$(C_FILES:.c=.o))\n";
        file << "\n";
        file << "$(BUILD_DIR)/$(ROMNAME).elf: $(OBJS)\n";
        file << "\n";
        file << "$(ROMNAME).z64: $(BUILD_DIR)/$(ROMNAME).elf\n";
        file << "\n";
        file << "clean:\n";
        file << "\t$(RM) -r $(BUILD_DIR) *.z64\n";
        file << ".PHONY: clean\n";
        file << "\n";
        file << "ifneq ($(wildcard $(BUILD_DIR)),)\n";
        file << "  -include $(shell find $(BUILD_DIR) -name '*.d')\n";
        file << "endif\n";
    }

    if (outProjectDir) *outProjectDir = project.absolutePath();
    return true;
}

} // namespace n64forge::studio
