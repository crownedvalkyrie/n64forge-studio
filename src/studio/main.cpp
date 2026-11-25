#include "n64forge/core/emulator.hpp"
#include "n64forge/studio/build_runner.hpp"
#include "n64forge/studio/controller_manager.hpp"
#include "n64forge/studio/controls_dialog.hpp"
#include "n64forge/studio/credits_widget.hpp"
#include "n64forge/studio/discord_rpc.hpp"
#include "n64forge/studio/emu_widget.hpp"
#ifdef N64FORGE_HAS_QT_NETWORK
#include "n64forge/studio/game_database.hpp"
#endif
#include "n64forge/studio/game_library.hpp"
#include "n64forge/studio/libdragon_sdk.hpp"
#include "n64forge/studio/library_widget.hpp"
#include "n64forge/studio/project_wizard.hpp"
#include "n64forge/studio/settings.hpp"
#include "n64forge/studio/splash_dialog.hpp"
#include "n64forge/studio/art_repository.hpp"
#include "n64forge/studio/style.hpp"
#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QCommandLineParser>
#include <QDialog>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFileSystemModel>
#include <QFrame>
#include <QHBoxLayout>
#include <QFont>
#include <QFontDatabase>
#include <QIcon>
#include <QKeyEvent>
#include <memory>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QSlider>
#include <QMenu>
#include <QDockWidget>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QPalette>
#include <QPlainTextEdit>
#include <QProcess>
#include <QToolBar>
#include <QToolButton>
#include <QStandardItemModel>
#include <QInputDialog>
#include <QStandardPaths>
#include <QScrollArea>
#include <QProcessEnvironment>
#include <QPushButton>
#include <QSplitter>
#include <QStatusBar>
#include <QStyle>
#include <QTabWidget>
#include <QTabBar>
#include <QTextEdit>
#include <QToolBar>
#include <QTreeView>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>

using n64forge::studio::BuildRunner;
using n64forge::studio::ArtKind;
using n64forge::studio::ArtRepository;
using n64forge::studio::ControllerManager;
using n64forge::studio::ControlsDialog;
using n64forge::studio::CreditsView;
using n64forge::studio::DiscordRpc;
using n64forge::studio::EmuWidget;
#ifdef N64FORGE_HAS_QT_NETWORK
using n64forge::studio::GameDatabase;
#endif
using n64forge::studio::GameLibrary;
using n64forge::studio::LibraryWidget;
using n64forge::studio::LibdragonSdk;
using n64forge::studio::LibdragonSdkDetector;
using n64forge::studio::ProjectWizard;
using n64forge::studio::SplashDialog;
using n64forge::studio::StudioSettings;
using n64forge::studio::cmakeBuildArguments;
using n64forge::studio::cmakeConfigureArguments;
using n64forge::studio::createLibdragonProject;
using n64forge::studio::recompilerArguments;
using n64forge::studio::resolveSiblingCliExecutable;
using n64forge::studio::retroStyleSheet;

namespace {

bool approveCommand(QWidget* parent, const QString& title, const QString& prompt,
                    const QString& program, const QStringList& arguments,
                    const QString& workingDirectory, const QProcessEnvironment& environment) {
    QMessageBox box(parent);
    box.setWindowTitle(title);
    box.setIcon(QMessageBox::Question);
    box.setText(prompt);
    box.setInformativeText(n64forge::studio::describeCommand(
        program, arguments, workingDirectory, environment));
    box.setStandardButtons(QMessageBox::Yes | QMessageBox::No);
    box.setDefaultButton(QMessageBox::No);
    return box.exec() == QMessageBox::Yes;
}

void appendCommandStart(QPlainTextEdit* output, const QString& heading,
                        const QString& program, const QStringList& arguments,
                        const QString& workingDirectory,
                        const QProcessEnvironment& environment) {
    output->appendPlainText(heading);
    output->appendPlainText(n64forge::studio::describeCommand(
        program, arguments, workingDirectory, environment));
}

QString resolveProgram(const QString& configured, const QString& fallbackName) {
    const QString requested = configured.trimmed().isEmpty() ? fallbackName : configured.trimmed();
    if (!requested.isEmpty() && QDir::isAbsolutePath(requested) && QFileInfo(requested).exists()) {
        return QFileInfo(requested).canonicalFilePath();
    }
    return QStandardPaths::findExecutable(requested);
}

} // namespace

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(StudioSettings& settings, const QString& artOverride, QWidget* parent = nullptr);

protected:
    void closeEvent(QCloseEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

public slots:
    void runRom(const QString& path);

private slots:
    void newProject();
    void openProject();
    void openProjectDir(const QString& path);
    void openRom();
    void addRecentRom(const QString& path);
    void buildProject();
    void cleanProject();
    void cancelBuild();
    void runInEmulator();
    void analyzeCurrentRom();
    void recompileCurrentRom();
    void buildNativeExecutable();
    void cancelRecompProcess();
    void togglePause();
    void toggleFullScreen();
    void onBuildOutput(const QString& text);
    void onBuildFinished(int exitCode);
    void onBuildError(const QString& message);
    void onRecompFinished(int exitCode);
    void onRecompError(const QString& message);
    void onNativeBuildFinished(int exitCode);
    void onNativeBuildError(const QString& message);

private:
    enum class NativeBuildStage { None, Configure, Build };

    void buildMenus();
    void buildToolBar();
    void buildSettingsPage();
    void updateRecentMenus();
    [[nodiscard]] QString projectRomPath() const;
    void startBuild(const QStringList& arguments);
    void runRecompCommand(const QString& romPath, const QString& subCommand,
                          const QStringList& extraArgs = {});
    void setRecompButtonsEnabled(bool enabled);
    void resetNativeBuildState();
    void updatePresenceForCurrentGame(bool preserveTimestamp = true);
#ifdef N64FORGE_HAS_QT_NETWORK
    void requestSplash(const QString& path, const QString& title);
    void downloadArtworkPack();
#endif

    StudioSettings& settings_;
    QFileSystemModel* fsModel_ = nullptr;
    QTreeView* fileTree_ = nullptr;
    QPlainTextEdit* editor_ = nullptr;
    QSplitter* split_ = nullptr;
    QMenu* recentProjectsMenu_ = nullptr;
    QMenu* recentRomsMenu_ = nullptr;
    QLabel* toolkitStatus_ = nullptr;
    QPlainTextEdit* buildOutput_ = nullptr;
    QPlainTextEdit* recompOutput_ = nullptr;
    BuildRunner* buildRunner_ = nullptr;
    BuildRunner* nativeBuildRunner_ = nullptr;
    QProcess* recompProcess_ = nullptr;
    ControllerManager* controllerManager_ = nullptr;
    EmuWidget* emuWidget_ = nullptr;
    GameLibrary* gameLibrary_ = nullptr;
    LibraryWidget* libraryWidget_ = nullptr;
#ifdef N64FORGE_HAS_QT_NETWORK
    GameDatabase* gameDatabase_ = nullptr;
#endif
    std::unique_ptr<DiscordRpc> discord_;
    QString presenceTitleArt_;
    QTabWidget* tabs_ = nullptr;
    QWidget* emuPage_ = nullptr;
    QWidget* settingsPage_ = nullptr;
    QAction* controlsAction_ = nullptr;
    QString currentProjectDir_;
    QAction* buildAction_ = nullptr;
    QAction* cleanAction_ = nullptr;
    QAction* cancelBuildAction_ = nullptr;
    QAction* runAction_ = nullptr;
    QAction* pauseAction_ = nullptr;
    QAction* resetEmuAction_ = nullptr;
    QAction* stopEmuAction_ = nullptr;
    QAction* preferencesAction_ = nullptr;
    QAction* fullScreenAction_ = nullptr;
    QPushButton* analyzeButton_ = nullptr;
    QPushButton* recompileButton_ = nullptr;
    QPushButton* nativeBuildButton_ = nullptr;
    QPushButton* cancelRecompButton_ = nullptr;
    NativeBuildStage nativeBuildStage_ = NativeBuildStage::None;
    QString nativeBuildProgram_;
    QString nativeBuildDir_;
    QProcessEnvironment nativeBuildEnvironment_;
    bool recompCancelled_ = false;
    bool autoPaused_ = false;
};

MainWindow::MainWindow(StudioSettings& settings, const QString& artOverride, QWidget* parent)
    : QMainWindow(parent)
    , settings_(settings)
    , buildRunner_(new BuildRunner(this))
    , nativeBuildRunner_(new BuildRunner(this))
    , controllerManager_(new ControllerManager(this)) {
    qApp->installEventFilter(this);
    discord_ = std::make_unique<DiscordRpc>();
    discord_->setClientId(settings_.discordClientId().toStdString());
    discord_->setEnabled(settings_.discordEnabled());

    setWindowTitle(tr("N64Forge Studio"));
    resize(1280, 800);
    setAcceptDrops(true);

    tabs_ = new QTabWidget(this);

    auto* emuPage = emuPage_ = new QWidget(tabs_);
    auto* emuLayout = new QVBoxLayout(emuPage);
    emuLayout->setContentsMargins(0, 0, 0, 0);
    emuLayout->setSpacing(0);
    emuWidget_ = new EmuWidget(controllerManager_, &settings_, emuPage);
    emuLayout->addWidget(emuWidget_, 1);


#ifdef N64FORGE_HAS_QT_NETWORK
    gameDatabase_ = new GameDatabase(this);
    gameDatabase_->setArtRepository(artOverride.isEmpty()
        ? settings_.value(QStringLiteral("art/repository"), ArtRepository::defaultUrl()).toString() : artOverride);
    gameDatabase_->ensureLoaded();
    gameLibrary_ = new GameLibrary(&settings_, gameDatabase_, this);
    connect(gameLibrary_, &GameLibrary::artworkResolved, this, [this](const QString& path) {
        if (emuWidget_->romPath() != path || !emuWidget_->isRunning()) return;
        if (const auto entry = gameLibrary_->find(path)) requestSplash(path, entry->title);
        updatePresenceForCurrentGame();
    });
#else
    Q_UNUSED(artOverride)
    gameLibrary_ = new GameLibrary(&settings_, this);
#endif
    libraryWidget_ = new LibraryWidget(gameLibrary_, tabs_);
    connect(libraryWidget_, &LibraryWidget::runRequested, this, &MainWindow::runRom);
    tabs_->addTab(libraryWidget_, tr("Library"));
    tabs_->addTab(emuPage, tr("Emulator"));

    auto* toolkitPage = new QWidget(tabs_);
    auto* toolLayout = new QVBoxLayout(toolkitPage);
    split_ = new QSplitter(toolkitPage);
    fileTree_ = new QTreeView(split_);
    fsModel_ = new QFileSystemModel(fileTree_);
    fsModel_->setRootPath(QDir::currentPath());
    fileTree_->setModel(fsModel_);
    fileTree_->setRootIndex(fsModel_->index(QDir::currentPath()));
    editor_ = new QPlainTextEdit(split_);
    editor_->setPlaceholderText(tr("Open a project and select a source file to edit."));
    split_->addWidget(fileTree_);
    split_->addWidget(editor_);
    toolLayout->addWidget(split_, 2);

    toolkitStatus_ = new QLabel(
        tr("No project open. Use File > New Project or Open Project Folder."),
        toolkitPage);
    toolkitStatus_->setWordWrap(true);
    toolLayout->addWidget(toolkitStatus_);

    buildOutput_ = new QPlainTextEdit(toolkitPage);
    buildOutput_->setPlaceholderText(tr("Build output will appear here."));
    buildOutput_->setReadOnly(true);
    toolLayout->addWidget(buildOutput_, 1);

    auto* recompBox = new QWidget(toolkitPage);
    auto* recompLayout = new QVBoxLayout(recompBox);
    recompLayout->setContentsMargins(0, 0, 0, 0);
    auto* recompButtons = new QHBoxLayout();
    analyzeButton_ = new QPushButton(tr("Analyze ROM"), recompBox);
    recompileButton_ = new QPushButton(tr("Recompile to C++"), recompBox);
    nativeBuildButton_ = new QPushButton(tr("Build Native Executable"), recompBox);
    cancelRecompButton_ = new QPushButton(tr("Cancel Process"), recompBox);
    cancelRecompButton_->setEnabled(false);
    recompButtons->addWidget(analyzeButton_);
    recompButtons->addWidget(recompileButton_);
    recompButtons->addWidget(nativeBuildButton_);
    recompButtons->addWidget(cancelRecompButton_);
    recompButtons->addStretch();
    recompLayout->addLayout(recompButtons);

    recompOutput_ = new QPlainTextEdit(recompBox);
    recompOutput_->setPlaceholderText(tr("Recompiler / analyzer output will appear here."));
    recompOutput_->setReadOnly(true);
    recompLayout->addWidget(recompOutput_, 1);
    toolLayout->addWidget(recompBox, 1);

    connect(analyzeButton_, &QPushButton::clicked, this, &MainWindow::analyzeCurrentRom);
    connect(recompileButton_, &QPushButton::clicked, this, &MainWindow::recompileCurrentRom);
    connect(nativeBuildButton_, &QPushButton::clicked, this, &MainWindow::buildNativeExecutable);
    connect(cancelRecompButton_, &QPushButton::clicked, this, &MainWindow::cancelRecompProcess);

    recompProcess_ = new QProcess(this);
    connect(recompProcess_, &QProcess::readyReadStandardOutput, this, [this]() {
        recompOutput_->moveCursor(QTextCursor::End);
        recompOutput_->insertPlainText(QString::fromLocal8Bit(recompProcess_->readAllStandardOutput()));
        recompOutput_->ensureCursorVisible();
    });
    connect(recompProcess_, &QProcess::readyReadStandardError, this, [this]() {
        recompOutput_->moveCursor(QTextCursor::End);
        recompOutput_->insertPlainText(QString::fromLocal8Bit(recompProcess_->readAllStandardError()));
        recompOutput_->ensureCursorVisible();
    });
    connect(recompProcess_, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &MainWindow::onRecompFinished);
    connect(recompProcess_, &QProcess::errorOccurred, this,
            [this](QProcess::ProcessError error) {
                if (recompCancelled_) return;
                onRecompError(tr("Recompiler process error %1: %2")
                                  .arg(static_cast<int>(error))
                                  .arg(recompProcess_->errorString()));
            });

    connect(nativeBuildRunner_, &BuildRunner::output, this, [this](const QString& text) {
        recompOutput_->moveCursor(QTextCursor::End);
        recompOutput_->insertPlainText(text);
        recompOutput_->ensureCursorVisible();
    });
    connect(nativeBuildRunner_, &BuildRunner::finished,
            this, &MainWindow::onNativeBuildFinished);
    connect(nativeBuildRunner_, &BuildRunner::errorOccurred,
            this, &MainWindow::onNativeBuildError);
    connect(nativeBuildRunner_, &BuildRunner::cancelled, this, [this]() {
        recompOutput_->appendPlainText(tr("\nNative build cancelled."));
        resetNativeBuildState();
    });

    tabs_->addTab(toolkitPage, tr("Toolkit"));
    tabs_->tabBar()->hide();
    tabs_->setCurrentWidget(libraryWidget_);

    setCentralWidget(tabs_);
    statusBar()->showMessage(tr("Library"));

    connect(buildRunner_, &BuildRunner::output, this, &MainWindow::onBuildOutput);
    connect(buildRunner_, &BuildRunner::finished, this, &MainWindow::onBuildFinished);
    connect(buildRunner_, &BuildRunner::errorOccurred, this, &MainWindow::onBuildError);

    connect(tabs_, &QTabWidget::currentChanged, this, [this](int) {
        const bool inEmulator = tabs_->currentWidget() == emuPage_;
        controllerManager_->resetAll();
        if (!inEmulator && emuWidget_ && emuWidget_->isRunning() && !emuWidget_->isPaused()) {
            emuWidget_->pause();
            autoPaused_ = true;
            if (pauseAction_) pauseAction_->setText(tr("Resume"));
        } else if (inEmulator && autoPaused_ && emuWidget_ && emuWidget_->isRunning() &&
                   emuWidget_->isPaused() && isActiveWindow()) {
            emuWidget_->resume();
            autoPaused_ = false;
            if (pauseAction_) pauseAction_->setText(tr("Pause"));
        }
    });

    buildMenus();
    buildSettingsPage();
    tabs_->addTab(new CreditsView(tabs_), tr("Credits"));
    buildToolBar();
    setRecompButtonsEnabled(true);

    if (!settings_.windowGeometry().isEmpty()) {
        restoreGeometry(settings_.windowGeometry());
    }
    if (!settings_.splitterState().isEmpty()) {
        split_->restoreState(settings_.splitterState());
    }
}

void MainWindow::buildMenus() {
    auto* fileMenu = menuBar()->addMenu(tr("File"));

    auto* newProjectAction = fileMenu->addAction(tr("New Project..."));
    connect(newProjectAction, &QAction::triggered, this, &MainWindow::newProject);

    auto* openProjectAction = fileMenu->addAction(tr("Open Project Folder..."));
    connect(openProjectAction, &QAction::triggered, this, &MainWindow::openProject);

    auto* openRomAction = fileMenu->addAction(tr("Open ROM..."));
    openRomAction->setShortcut(QKeySequence::Open);
    connect(openRomAction, &QAction::triggered, this, &MainWindow::openRom);

    fileMenu->addSeparator();

    recentProjectsMenu_ = fileMenu->addMenu(tr("Recent Projects"));
    recentRomsMenu_ = fileMenu->addMenu(tr("Recent ROMs"));

    fileMenu->addSeparator();

    auto* clearRecentAction = fileMenu->addAction(tr("Clear Recent"));
    connect(clearRecentAction, &QAction::triggered, [this]() {
        settings_.clearRecentProjects();
        settings_.clearRecentRoms();
        updateRecentMenus();
    });

    fileMenu->addSeparator();

    auto* exitAction = fileMenu->addAction(tr("Exit"));
    exitAction->setShortcut(QKeySequence::Quit);
    connect(exitAction, &QAction::triggered, this, &QWidget::close);

    auto* buildMenu = menuBar()->addMenu(tr("Build"));
    buildAction_ = buildMenu->addAction(tr("Build Project"));
    connect(buildAction_, &QAction::triggered, this, &MainWindow::buildProject);
    cleanAction_ = buildMenu->addAction(tr("Clean Project"));
    connect(cleanAction_, &QAction::triggered, this, &MainWindow::cleanProject);
    cancelBuildAction_ = buildMenu->addAction(tr("Cancel Build"));
    connect(cancelBuildAction_, &QAction::triggered, this, &MainWindow::cancelBuild);
    runAction_ = buildMenu->addAction(tr("Run in Emulator"));
    connect(runAction_, &QAction::triggered, this, &MainWindow::runInEmulator);
    buildMenu->addSeparator();
    auto* sdkInfoAction = buildMenu->addAction(tr("Detect libdragon SDK"));
    connect(sdkInfoAction, &QAction::triggered, [this]() {
        LibdragonSdk sdk = LibdragonSdkDetector::autodetect();
        if (sdk.isValid()) {
            QMessageBox::information(this, tr("libdragon SDK"),
                tr("Found SDK at %1\nCompiler: %2\nVersion: %3")
                    .arg(sdk.rootPath, sdk.gccPath, sdk.version));
        } else {
            QMessageBox::warning(this, tr("libdragon SDK"),
                tr("No SDK detected. %1").arg(sdk.error));
        }
    });

    auto* emuMenu = menuBar()->addMenu(tr("Emulator"));
    auto* resetEmuAction = resetEmuAction_ = emuMenu->addAction(tr("Reset"));
    resetEmuAction->setEnabled(false);
    connect(resetEmuAction, &QAction::triggered, [this]() {
        if (emuWidget_ && emuWidget_->hasGame()) {
            emuWidget_->reset();
            autoPaused_ = false;
            pauseAction_->setText(tr("Pause"));
            statusBar()->showMessage(tr("Reset"));
        }
        if (discord_) updatePresenceForCurrentGame();
    });
    auto* stopEmuAction = stopEmuAction_ = emuMenu->addAction(tr("Stop"));
    stopEmuAction->setEnabled(false);
    connect(stopEmuAction, &QAction::triggered, [this]() {
        if (emuWidget_) {
            emuWidget_->stop();
            controllerManager_->resetAll();
            autoPaused_ = false;
            if (pauseAction_) pauseAction_->setText(tr("Pause"));
            if (discord_) discord_->clearPresence();
            pauseAction_->setEnabled(false);
            resetEmuAction_->setEnabled(false);
            stopEmuAction_->setEnabled(false);
            tabs_->setCurrentWidget(libraryWidget_);
            setWindowTitle(tr("N64Forge Studio"));
            statusBar()->showMessage(tr("Emulation stopped"));
        }
    });
    pauseAction_ = emuMenu->addAction(tr("Pause"));
    pauseAction_->setEnabled(false);
    pauseAction_->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_P));
    connect(pauseAction_, &QAction::triggered, this, &MainWindow::togglePause);
    fullScreenAction_ = emuMenu->addAction(tr("Full Screen"));
    fullScreenAction_->setShortcut(QKeySequence(Qt::Key_F11));
    fullScreenAction_->setCheckable(true);
    connect(fullScreenAction_, &QAction::triggered, this, &MainWindow::toggleFullScreen);
    auto* controlsAction = controlsAction_ = new QAction(this);
    connect(controlsAction, &QAction::triggered, [this]() {
        ControlsDialog dialog(*controllerManager_, this);
        if (dialog.exec() == QDialog::Accepted) {
            controllerManager_->saveSettings();
        }
    });
    auto* settingsAction = preferencesAction_ = emuMenu->addAction(tr("Settings…"));
    settingsAction->setMenuRole(QAction::PreferencesRole);
    connect(settingsAction, &QAction::triggered, this, [this] { tabs_->setCurrentWidget(settingsPage_); });
    auto* viewMenu = menuBar()->addMenu(tr("View"));
    auto* hudAction = viewMenu->addAction(tr("Show Performance Overlay"));
    hudAction->setCheckable(true);
    hudAction->setShortcut(QKeySequence(Qt::Key_F3));
    connect(hudAction, &QAction::toggled, emuWidget_, &EmuWidget::setPerformanceHud);

    auto* diagnostics = new QDockWidget(tr("Diagnostics"), this);
    diagnostics->setObjectName(QStringLiteral("diagnosticsDock"));
    auto* statsLabel = new QLabel(diagnostics);
    statsLabel->setProperty("deck", QStringLiteral("telemetry"));
    statsLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    statsLabel->setMargin(8);
    diagnostics->setWidget(statsLabel);
    addDockWidget(Qt::BottomDockWidgetArea, diagnostics);
    diagnostics->hide();
    viewMenu->addAction(diagnostics->toggleViewAction());
    auto* statsTimer = new QTimer(diagnostics);
    statsTimer->setInterval(250);
    const auto refreshStats = [this, statsLabel] {
        const auto text = emuWidget_->diagnosticText();
        if (statsLabel->text() != text) statsLabel->setText(text);
    };
    connect(statsTimer, &QTimer::timeout, diagnostics, refreshStats);
    connect(diagnostics, &QDockWidget::visibilityChanged, this, [this, statsTimer, refreshStats](bool visible) {
        emuWidget_->setDiagnosticsVisible(visible);
        if (visible) {
            refreshStats();
            statsTimer->start();
        } else {
            statsTimer->stop();
        }
    });

    auto* helpMenu = menuBar()->addMenu(tr("Help"));
    auto* aboutAction = helpMenu->addAction(tr("About N64Forge Studio"));
    connect(aboutAction, &QAction::triggered, this, [this]() {
        QMessageBox::about(this, tr("About N64Forge Studio"),
            tr("<h2>N64Forge Studio %1</h2>"
               "<p>An N64 emulator and homebrew toolkit for Windows and macOS.</p>")
                .arg(QCoreApplication::applicationVersion()));
    });

    updateRecentMenus();
    onBuildFinished(-1);
}

void MainWindow::buildSettingsPage() {
    settingsPage_ = new QWidget(tabs_);
    settingsPage_->setObjectName(QStringLiteral("studioSettingsPage"));
    tabs_->addTab(settingsPage_, tr("Settings"));
    libraryWidget_->setParallaxEnabled(settings_.uiParallaxEnabled());

    auto* layout = new QVBoxLayout(settingsPage_);
    layout->setContentsMargins(20, 16, 20, 16);
    layout->setSpacing(14);
    auto* pages = new QTabWidget(settingsPage_);
    pages->setDocumentMode(true);
    layout->addWidget(pages, 1);

    const auto page = [&](const QString& name) {
        auto* scroll = new QScrollArea(pages);
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        auto* body = new QWidget(scroll);
        auto* box = new QVBoxLayout(body);
        box->setContentsMargins(20, 20, 20, 20);
        box->setSpacing(16);
        box->setAlignment(Qt::AlignTop);
        scroll->setWidget(body);
        pages->addTab(scroll, name);
        return box;
    };
    const auto explain = [&](QVBoxLayout* box, const QString& text) {
        auto* label = new QLabel(text, settingsPage_);
        label->setProperty("deck", QStringLiteral("subtitle"));
        label->setWordWrap(true);
        box->addWidget(label);
    };
    const auto settingRow = [&](QVBoxLayout* box, const QString& title, QWidget* control,
                                const QString& help) {
        auto* row = new QHBoxLayout();
        row->setSpacing(16);
        auto* label = new QLabel(title, settingsPage_);
        label->setMinimumWidth(165);
        row->addWidget(label, 0, Qt::AlignVCenter);
        row->addWidget(control, 1);
        box->addLayout(row);
        explain(box, help);
    };

    auto* general = page(tr("General"));
    auto* graphics = page(tr("Graphics"));
    auto* performance = page(tr("Performance"));
    auto* audio = page(tr("Audio & Controls"));
    auto* paths = page(tr("Discord & Paths"));

    auto* parallaxBox = new QCheckBox(tr("Enable UI Parallax Effects"), settingsPage_);
    parallaxBox->setObjectName(QStringLiteral("uiParallaxSetting"));
    parallaxBox->setChecked(settings_.uiParallaxEnabled());
    general->addWidget(parallaxBox);
    explain(general, tr("Tilt covers and move the Library artwork as you browse. Turn off for a still interface."));

    auto* focusBox = new QCheckBox(tr("Pause when Studio loses focus"), settingsPage_);
    focusBox->setObjectName(QStringLiteral("pauseOnFocusLossSetting"));
    focusBox->setChecked(settings_.pauseOnFocusLoss());
    general->addWidget(focusBox);
    explain(general, tr("Pause the game when switching to another window."));

    auto* bootBox = new QCheckBox(tr("Show boot animation"), settingsPage_);
    bootBox->setObjectName(QStringLiteral("bootAnimationSetting"));
    bootBox->setChecked(settings_.bootSequenceEnabled());
    general->addWidget(bootBox);
    explain(general, tr("Show the Studio intro when a game starts."));

    auto* saveBox = new QComboBox(settingsPage_);
    saveBox->setObjectName(QStringLiteral("saveMediaSetting"));
    using Media = StudioSettings::SaveMedia;
    saveBox->addItem(tr("Auto (use existing save)"), static_cast<int>(Media::Auto));
    saveBox->addItem(tr("None"), static_cast<int>(Media::None));
    saveBox->addItem(tr("FlashRAM (1 Mbit)"), static_cast<int>(Media::FlashRam));
    saveBox->addItem(tr("SRAM (32 KiB)"), static_cast<int>(Media::Sram));
    saveBox->addItem(tr("EEPROM (4 Kbit)"), static_cast<int>(Media::Eeprom4K));
    saveBox->addItem(tr("EEPROM (16 Kbit)"), static_cast<int>(Media::Eeprom16K));
    saveBox->setCurrentIndex(saveBox->findData(static_cast<int>(settings_.saveMedia())));
    settingRow(general, tr("Save media"), saveBox,
               tr("Choose the save chip for the next game, or use the existing save file."));

    auto* interpolationMode = new QComboBox(settingsPage_);
    interpolationMode->setObjectName(QStringLiteral("frameInterpolationSetting"));
    interpolationMode->addItem(tr("Off"), 0);
    interpolationMode->addItem(tr("60 FPS target"), 60);
    interpolationMode->addItem(tr("120 FPS target"), 120);
    interpolationMode->setCurrentIndex(!settings_.frameInterpolation() ? 0 :
                                       settings_.renderFps() == 120 ? 2 : 1);
    settingRow(graphics, tr("Frame interpolation"), interpolationMode,
               tr("Smooths frames shown on your monitor without changing game or audio timing."));

    auto* graphicsCombo = new QComboBox(settingsPage_);
    graphicsCombo->setObjectName(QStringLiteral("graphicsApiSetting"));
#if defined(_WIN32)
    graphicsCombo->addItem(tr("Auto"), static_cast<int>(n64forge::studio::GraphicsApi::Auto));
    graphicsCombo->addItem(tr("Direct3D 12"), static_cast<int>(n64forge::studio::GraphicsApi::D3D12));
    graphicsCombo->addItem(tr("Vulkan"), static_cast<int>(n64forge::studio::GraphicsApi::Vulkan));
#else
    graphicsCombo->addItem(tr("Auto"), static_cast<int>(n64forge::studio::GraphicsApi::Auto));
    graphicsCombo->addItem(tr("Metal"), static_cast<int>(n64forge::studio::GraphicsApi::Metal));
    graphicsCombo->addItem(tr("MoltenVK"), static_cast<int>(n64forge::studio::GraphicsApi::MoltenVK));
    graphicsCombo->addItem(tr("KosmicKrisp"), static_cast<int>(n64forge::studio::GraphicsApi::KosmicKrisp));
#endif
    graphicsCombo->addItem(tr("Software"), static_cast<int>(n64forge::studio::GraphicsApi::Software));
    const int api = static_cast<int>(settings_.graphicsApi());
    const int fallbackApi = static_cast<int>(n64forge::video::defaultGraphicsApi());
    graphicsCombo->setCurrentIndex(graphicsCombo->findData(api) >= 0 ? graphicsCombo->findData(api)
                                                                     : graphicsCombo->findData(fallbackApi));
    settingRow(graphics, tr("Graphics API"), graphicsCombo,
               tr("Choose how Studio presents the image. Auto probes the native backends in "
                  "preference order; other unsupported choices use software."));

    auto* driverLabel = new QLabel(settingsPage_);
    driverLabel->setObjectName(QStringLiteral("graphicsDriverInfo"));
    driverLabel->setWordWrap(true);
    driverLabel->setTextFormat(Qt::PlainText);
    graphics->addWidget(driverLabel);
    const auto updateGraphics = [graphicsCombo, driverLabel, this] {
        const auto selected = static_cast<n64forge::video::GraphicsApi>(graphicsCombo->currentData().toInt());
        const auto info = n64forge::video::probeGraphicsApi(selected);
        auto active = emuWidget_->activePresenterName();
        if (const auto device = emuWidget_->activePresenterDevice(); !device.isEmpty())
            active += QStringLiteral(" — ") + device;
        driverLabel->setText(tr("Selected driver: %1\nActive presenter: %2")
            .arg(QString::fromStdString(info.detail), active));
    };
    updateGraphics();

    auto* filterCombo = new QComboBox(settingsPage_);
    filterCombo->setObjectName(QStringLiteral("scalingFilterSetting"));
    filterCombo->addItem(tr("Smooth Bilinear"), QStringLiteral("smooth"));
    filterCombo->addItem(tr("Crisp Nearest"), QStringLiteral("nearest"));
    const QString currentFilter = settings_.scalingFilter();
    filterCombo->setCurrentIndex(filterCombo->findData(currentFilter) >= 0
                                     ? filterCombo->findData(currentFilter) : 0);
    settingRow(graphics, tr("Scaling filter"), filterCombo,
               tr("Choose crisp pixels or a softer image when scaling."));

    auto* internalScale = new QComboBox(settingsPage_);
    internalScale->setObjectName(QStringLiteral("internalResolutionSetting"));
    internalScale->addItem(tr("1× Native"), 1);
    internalScale->addItem(tr("2×"), 2);
    internalScale->addItem(tr("3×"), 3);
    internalScale->addItem(tr("4×"), 4);
    internalScale->setCurrentIndex(internalScale->findData(static_cast<int>(settings_.internalScale())));
    settingRow(graphics, tr("Internal resolution"), internalScale,
               tr("Refine opaque triangle interiors after loading or resetting. Guest memory stays native; "
                  "higher settings may reduce speed."));

    auto* viewportCombo = new QComboBox(settingsPage_);
    viewportCombo->setObjectName(QStringLiteral("aspectScalingSetting"));
    viewportCombo->addItem(tr("Fit Window"), 0);
    viewportCombo->addItem(tr("1x"), 1);
    viewportCombo->addItem(tr("2x"), 2);
    viewportCombo->addItem(tr("4x"), 4);
    const int currentRes = settings_.viewportScale();
    viewportCombo->setCurrentIndex(viewportCombo->findData(currentRes) >= 0
                                       ? viewportCombo->findData(currentRes) : 0);
    settingRow(graphics, tr("Aspect / scaling"), viewportCombo,
               tr("Fit Window fills the viewport while preserving the console's aspect ratio. Fixed sizes "
                  "change only presentation, not rasterization or guest memory."));

    auto* ultrawideBox = new QCheckBox(tr("Ultrawide aspect ratio (21:9)"), settingsPage_);
    ultrawideBox->setObjectName(QStringLiteral("aspectRatioSetting"));
    ultrawideBox->setChecked(settings_.ultrawideEnabled());
    graphics->addWidget(ultrawideBox);
    explain(graphics, tr("Stretch fixed viewport sizes to 21:9. Fit Window always keeps 4:3."));
#if defined(_WIN32)
    explain(graphics, tr("Auto tries Direct3D 12, then Vulkan (vulkan-1.dll), then Software. "
                         "Explicit native choices fall back only to Software."));
#else
    explain(graphics, tr("Auto tries Metal, then installed Vulkan drivers, then Software. "
                         "Explicit native choices fall back only to Software."));
#endif

    auto* cpuBox = new QComboBox(settingsPage_);
    cpuBox->setObjectName(QStringLiteral("cpuModeSetting"));
    cpuBox->addItem(tr("Native JIT"), static_cast<int>(n64forge::CpuExecutionMode::Jit));
    cpuBox->addItem(tr("Cached interpreter"), static_cast<int>(n64forge::CpuExecutionMode::Cached));
    cpuBox->addItem(tr("Interpreter"), static_cast<int>(n64forge::CpuExecutionMode::Interpreter));
    cpuBox->setCurrentIndex(cpuBox->findData(static_cast<int>(settings_.cpuMode())));
    settingRow(performance, tr("CPU mode"), cpuBox,
               tr("Choose how Studio runs the N64 CPU. Changes take effect when you load or reset a game. "
                  "Use Interpreter to compare behavior; JIT falls back to Cached if unavailable."));

    auto* rdpBox = new QComboBox(settingsPage_);
    rdpBox->setObjectName(QStringLiteral("rdpThreadsSetting"));
    rdpBox->addItem(tr("Auto"), 0);
    for (const int count : {1, 2, 4, 8})
        rdpBox->addItem(tr("%n thread(s)", nullptr, count), count);
    rdpBox->setCurrentIndex(rdpBox->findData(static_cast<int>(settings_.rdpThreads())));
    settingRow(performance, tr("RDP threads"), rdpBox,
               tr("Choose the number of rasterizer workers used after loading or resetting a game. "
                  "This is Studio's only RCP concurrency control."));

    auto* controlsButton = new QPushButton(tr("Configure controllers…"), settingsPage_);
    controlsButton->setObjectName(QStringLiteral("configureControllersButton"));
    audio->addWidget(controlsButton, 0, Qt::AlignLeft);
    explain(audio, tr("Map buttons and choose a Controller Pak or Rumble Pak for each port."));

    auto* rumblePort = new QComboBox(settingsPage_);
    rumblePort->setObjectName(QStringLiteral("rumblePortSetting"));
    for (int port = 0; port < 4; ++port) rumblePort->addItem(tr("Port %1").arg(port + 1), port);
    settingRow(audio, tr("Rumble port"), rumblePort,
               tr("Choose which controller the strength below applies to."));

    auto* rumbleSlider = new QSlider(Qt::Horizontal, settingsPage_);
    rumbleSlider->setObjectName(QStringLiteral("rumbleIntensitySetting"));
    rumbleSlider->setRange(0, 100);
    auto* rumbleValue = new QLabel(settingsPage_);
    auto* rumbleRowHolder = new QWidget(settingsPage_);
    auto* rumbleRow = new QHBoxLayout(rumbleRowHolder);
    rumbleRow->setContentsMargins(0, 0, 0, 0);
    rumbleRow->setSpacing(16);
    rumbleRow->addWidget(rumbleSlider, 1);
    rumbleRow->addWidget(rumbleValue, 0, Qt::AlignRight | Qt::AlignVCenter);
    settingRow(audio, tr("Rumble intensity"), rumbleRowHolder,
               tr("Set vibration strength for the selected port. Changes are saved immediately."));
    const auto applyRumble = [=, this] {
        const int port = rumblePort->currentData().toInt();
        rumbleValue->setText(tr("%1%").arg(rumbleSlider->value()));
        controllerManager_->setPakOptions(port, controllerManager_->pakType(port),
                                          static_cast<unsigned>(rumbleSlider->value()));
        controllerManager_->saveSettings();
    };
    const auto refreshRumble = [=, this] {
        const int port = rumblePort->currentData().toInt();
        const QSignalBlocker block(rumbleSlider);
        rumbleSlider->setValue(static_cast<int>(controllerManager_->rumbleIntensity(port)));
        rumbleValue->setText(tr("%1%").arg(rumbleSlider->value()));
    };
    refreshRumble();
    connect(rumblePort, &QComboBox::currentIndexChanged, settingsPage_, refreshRumble);
    connect(rumbleSlider, &QSlider::valueChanged, settingsPage_, applyRumble);
    explain(audio, tr("Game audio uses the system output device."));

    auto* discordBox = new QCheckBox(tr("Enable Discord Rich Presence"), settingsPage_);
    discordBox->setObjectName(QStringLiteral("discordEnabledSetting"));
    discordBox->setChecked(settings_.discordEnabled());
    paths->addWidget(discordBox);
    explain(paths, tr("Show the current game in Discord. Cover art uses the configured artwork repository."));

    auto* discordId = new QLineEdit(settings_.discordClientId(), settingsPage_);
    discordId->setObjectName(QStringLiteral("discordClientIdSetting"));
    discordId->setPlaceholderText(tr("Discord Application Client ID (optional)"));
    settingRow(paths, tr("Discord client ID"), discordId,
               tr("A default ID is used automatically. Create an application at discord.com/developers "
                  "only if you want to override it."));

#ifdef N64FORGE_HAS_QT_NETWORK
    auto* repoButton = new QPushButton(tr("Artwork repository…"), settingsPage_);
    repoButton->setObjectName(QStringLiteral("artworkRepositorySetting"));
    paths->addWidget(repoButton, 0, Qt::AlignLeft);
    auto* repoLabel = new QLabel(settingsPage_);
    repoLabel->setObjectName(QStringLiteral("artworkRepositoryPath"));
    repoLabel->setTextFormat(Qt::PlainText);
    repoLabel->setWordWrap(true);
    const auto refreshRepo = [this, repoLabel] {
        repoLabel->setText(tr("Repository: %1").arg(gameDatabase_->artRepository().url()));
    };
    refreshRepo();
    paths->addWidget(repoLabel);
    connect(repoButton, &QPushButton::clicked, settingsPage_, [this, refreshRepo] {
        bool accepted = false;
        const auto url = QInputDialog::getText(this, tr("Artwork Repository"),
            tr("HTTPS GitHub repository (artwork on main):"), QLineEdit::Normal,
            gameDatabase_->artRepository().url(), &accepted);
        if (!accepted) return;
        const auto repo = ArtRepository::canonicalUrl(url);
        if (repo.isEmpty()) {
            QMessageBox::warning(this, tr("Artwork Repository"), tr("Enter a valid HTTPS GitHub repository URL."));
            return;
        }
        settings_.setValue(QStringLiteral("art/repository"), repo);
        if (repo != gameDatabase_->artRepository().url()) {
            emuWidget_->clearCover();
            gameDatabase_->setArtRepository(repo);
            updatePresenceForCurrentGame();
        }
        refreshRepo();
    });
    auto* downloadButton = new QPushButton(tr("Download artwork pack…"), settingsPage_);
    downloadButton->setObjectName(QStringLiteral("downloadArtworkPackButton"));
    paths->addWidget(downloadButton, 0, Qt::AlignLeft);
    connect(downloadButton, &QPushButton::clicked, this, &MainWindow::downloadArtworkPack);
    explain(paths, tr("Artwork paths and downloads live here; the View menu no longer duplicates them."));
#endif

    auto* romDir = new QLabel(tr("ROM folder: %1").arg(settings_.lastRomDirectory()), settingsPage_);
    auto* projectDir = new QLabel(tr("Project folder: %1").arg(settings_.lastProjectDirectory()), settingsPage_);
    for (auto* label : {romDir, projectDir}) {
        label->setTextFormat(Qt::PlainText);
        label->setWordWrap(true);
        paths->addWidget(label);
    }

    connect(parallaxBox, &QCheckBox::toggled, settingsPage_, [=, this](bool enabled) {
        settings_.setUiParallaxEnabled(enabled);
        libraryWidget_->setParallaxEnabled(enabled);
    });
    connect(focusBox, &QCheckBox::toggled, settingsPage_, [this](bool checked) {
        settings_.setPauseOnFocusLoss(checked);
    });
    connect(saveBox, &QComboBox::currentIndexChanged, settingsPage_, [this, saveBox](int) {
        settings_.setSaveMedia(static_cast<Media>(saveBox->currentData().toInt()));
    });
    connect(cpuBox, &QComboBox::currentIndexChanged, settingsPage_, [this, cpuBox](int) {
        settings_.setCpuMode(static_cast<n64forge::CpuExecutionMode>(cpuBox->currentData().toInt()));
    });
    connect(rdpBox, &QComboBox::currentIndexChanged, settingsPage_, [this, rdpBox](int) {
        settings_.setRdpThreads(rdpBox->currentData().toUInt());
    });
    connect(graphicsCombo, &QComboBox::currentIndexChanged, settingsPage_, [this, graphicsCombo, updateGraphics](int) {
        settings_.setGraphicsApi(static_cast<n64forge::studio::GraphicsApi>(graphicsCombo->currentData().toInt()));
        emuWidget_->applyVideoSettings();
        updateGraphics();
    });
    connect(internalScale, &QComboBox::currentIndexChanged, settingsPage_, [this, internalScale](int) {
        settings_.setInternalScale(internalScale->currentData().toUInt());
    });
    connect(discordBox, &QCheckBox::toggled, settingsPage_, [this](bool enabled) {
        settings_.setDiscordEnabled(enabled);
        if (discord_) discord_->setEnabled(enabled);
    });
    connect(discordId, &QLineEdit::editingFinished, settingsPage_, [this, discordId] {
        settings_.setDiscordClientId(discordId->text().trimmed());
        if (discord_) discord_->setClientId(settings_.discordClientId().toStdString());
    });
    connect(controlsButton, &QPushButton::clicked, controlsAction_, &QAction::trigger);

    const auto applyVideo = [=, this] {
        const int targetFps = interpolationMode->currentData().toInt();
        settings_.setBootSequenceEnabled(bootBox->isChecked());
        settings_.setFrameInterpolation(targetFps != 0);
        settings_.setScalingFilter(filterCombo->currentData().toString());
        settings_.setRenderFps(targetFps);
        settings_.setUltrawideEnabled(ultrawideBox->isChecked());
        settings_.setViewportScale(viewportCombo->currentData().toInt());
        emuWidget_->applyVideoSettings();
    };
    for (auto* box : {bootBox, ultrawideBox})
        connect(box, &QCheckBox::toggled, settingsPage_, applyVideo);
    for (auto* combo : {interpolationMode, filterCombo, viewportCombo})
        connect(combo, &QComboBox::currentIndexChanged, settingsPage_, applyVideo);
    connect(tabs_, &QTabWidget::currentChanged, settingsPage_, [this, updateGraphics](int) {
        if (tabs_->currentWidget() == settingsPage_) updateGraphics();
    });
}

void MainWindow::buildToolBar() {
    auto* bar = addToolBar(tr("Main"));
    bar->setObjectName(QStringLiteral("mainToolBar"));
    bar->setMovable(false);
    bar->setFloatable(false);
    bar->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    bar->setIconSize(QSize(16, 16));
    bar->setContentsMargins(16, 8, 16, 8);

    auto* navigation = new QActionGroup(bar);
    navigation->setExclusive(true);
    const auto addPage = [&](int index) {
        auto* action = bar->addAction(tabs_->tabText(index));
        navigation->addAction(action);
        action->setCheckable(true);
        action->setChecked(tabs_->currentIndex() == index);
        connect(action, &QAction::triggered, this, [this, index] { tabs_->setCurrentIndex(index); });
        connect(tabs_, &QTabWidget::currentChanged, action,
                [action, index](int page) { action->setChecked(index == page); });
    };
    for (int index = 0; index < tabs_->count(); ++index) addPage(index);
    const auto addSpace = [&] {
        auto* spacer = new QWidget(bar);
        spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        bar->addWidget(spacer);
    };
    addSpace();
    auto* openAction = bar->addAction(style()->standardIcon(QStyle::SP_MediaPlay),
                                      tr("Open ROM"));
    openAction->setToolTip(tr("Play a cartridge file"));
    connect(openAction, &QAction::triggered, this, &MainWindow::openRom);

    pauseAction_->setIcon(style()->standardIcon(QStyle::SP_MediaPause));
    bar->addAction(pauseAction_);

    resetEmuAction_->setIcon(style()->standardIcon(QStyle::SP_BrowserReload));
    bar->addAction(resetEmuAction_);
    stopEmuAction_->setIcon(style()->standardIcon(QStyle::SP_MediaStop));
    stopEmuAction_->setToolTip(tr("Stop emulation and return to the Library"));
    bar->addAction(stopEmuAction_);
    addSpace();
    fullScreenAction_->setIcon(style()->standardIcon(QStyle::SP_TitleBarMaxButton));
    bar->addAction(fullScreenAction_);
}

void MainWindow::updateRecentMenus() {
    recentProjectsMenu_->clear();
    const auto projects = settings_.recentProjects();
    if (projects.isEmpty()) {
        auto* action = recentProjectsMenu_->addAction(tr("(none)"));
        action->setEnabled(false);
    } else {
        for (const auto& path : projects) {
            auto* action = recentProjectsMenu_->addAction(path);
            connect(action, &QAction::triggered, this, [this, path]() {
                openProjectDir(path);
            });
        }
    }

    recentRomsMenu_->clear();
    const auto roms = settings_.recentRoms();
    if (roms.isEmpty()) {
        auto* action = recentRomsMenu_->addAction(tr("(none)"));
        action->setEnabled(false);
    } else {
        for (const auto& path : roms) {
            auto* action = recentRomsMenu_->addAction(path);
            connect(action, &QAction::triggered, this, [this, path]() {
                runRom(path);
            });
        }
    }
}

void MainWindow::newProject() {
    ProjectWizard wizard(this);
    if (wizard.exec() != QDialog::Accepted) return;

    QString projectDir;
    if (!createLibdragonProject(wizard.projectDirectory(), wizard.projectName(),
                                wizard.selectedSdk(), &projectDir)) {
        QMessageBox::critical(this, tr("Project Creation Failed"),
            tr("Could not create project '%1' in '%2'.")
                .arg(wizard.projectName(), wizard.projectDirectory()));
        return;
    }

    openProjectDir(projectDir);
    buildOutput_->clear();
    buildOutput_->appendPlainText(tr("Created project '%1' at %2")
                             .arg(wizard.projectName(), projectDir));
}

void MainWindow::openProject() {
    const QString dir = QFileDialog::getExistingDirectory(
        this, tr("Open Project Folder"), settings_.lastProjectDirectory());
    if (dir.isEmpty()) return;
    openProjectDir(dir);
}

void MainWindow::openProjectDir(const QString& path) {
    const QDir dir(path);
    if (!dir.exists()) {
        QMessageBox::warning(this, tr("Project Not Found"),
            tr("The project folder no longer exists:\n%1").arg(path));
        settings_.addRecentProject(path);
        updateRecentMenus();
        return;
    }
    currentProjectDir_ = dir.absolutePath();
    settings_.setLastProjectDirectory(currentProjectDir_);
    settings_.addRecentProject(currentProjectDir_);
    fsModel_->setRootPath(currentProjectDir_);
    fileTree_->setRootIndex(fsModel_->index(currentProjectDir_));
    toolkitStatus_->setText(tr("Project: %1").arg(currentProjectDir_));
    updateRecentMenus();
    buildOutput_->clear();
    buildOutput_->appendPlainText(tr("Opened project %1").arg(currentProjectDir_));
}

void MainWindow::openRom() {
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Open ROM"), settings_.lastRomDirectory(),
        tr("N64 ROMs (*.z64 *.n64 *.v64 *.zip);;All Files (*)"));
    if (path.isEmpty()) return;

    const QFileInfo info(path);
    settings_.setLastRomDirectory(info.absolutePath());
    runRom(path);
}

void MainWindow::addRecentRom(const QString& path) {
    settings_.addRecentRom(path);
    if (gameLibrary_) gameLibrary_->addRom(path);
    updateRecentMenus();
}

QString MainWindow::projectRomPath() const {
    if (currentProjectDir_.isEmpty()) return {};
    const QDir dir(currentProjectDir_);
    return dir.absoluteFilePath(dir.dirName() + QStringLiteral(".z64"));
}

void MainWindow::buildProject() {
    if (currentProjectDir_.isEmpty()) {
        QMessageBox::information(this, tr("No Project"),
            tr("Open or create a project before building."));
        return;
    }
    startBuild(QStringList{});
}

void MainWindow::cleanProject() {
    if (currentProjectDir_.isEmpty()) {
        QMessageBox::information(this, tr("No Project"),
            tr("Open or create a project before cleaning."));
        return;
    }
    startBuild(QStringList{QStringLiteral("clean")});
}

void MainWindow::cancelBuild() {
    buildRunner_->cancel();
}

void MainWindow::startBuild(const QStringList& arguments) {
    if (buildRunner_->isRunning()) {
        QMessageBox::information(this, tr("Build in Progress"),
            tr("A build is already running. Cancel it first or wait for it to finish."));
        return;
    }

    const LibdragonSdk sdk = LibdragonSdkDetector::autodetect();
    if (!sdk.isValid()) {
        QMessageBox::warning(this, tr("libdragon SDK Missing"),
            tr("Cannot build: %1").arg(sdk.error));
        return;
    }

    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("N64_INST"), sdk.rootPath);

    const QString program = resolveProgram(sdk.makePath, QStringLiteral("make"));
    if (program.isEmpty()) {
        QMessageBox::warning(this, tr("Build Tool Missing"),
            tr("Could not resolve 'make'. Configure the SDK toolchain first."));
        return;
    }

    if (!approveCommand(this, tr("Approve Build Command"),
                        tr("Run the following build command?"),
                        program, arguments, currentProjectDir_, env)) {
        return;
    }

    buildOutput_->clear();
    appendCommandStart(buildOutput_, tr("Approved build command:"),
                       program, arguments, currentProjectDir_, env);
    buildOutput_->appendPlainText(QLatin1String(""));

    buildAction_->setEnabled(false);
    cleanAction_->setEnabled(false);
    runAction_->setEnabled(false);
    cancelBuildAction_->setEnabled(true);

    buildRunner_->start(program, currentProjectDir_, arguments, env);
}

void MainWindow::onBuildOutput(const QString& text) {
    buildOutput_->moveCursor(QTextCursor::End);
    buildOutput_->insertPlainText(text);
    buildOutput_->ensureCursorVisible();
}

void MainWindow::onBuildFinished(int exitCode) {
    buildAction_->setEnabled(!currentProjectDir_.isEmpty());
    cleanAction_->setEnabled(!currentProjectDir_.isEmpty());
    runAction_->setEnabled(!projectRomPath().isEmpty() && QFile::exists(projectRomPath()));
    cancelBuildAction_->setEnabled(buildRunner_->isRunning());

    if (exitCode == 0) {
        buildOutput_->appendPlainText(tr("\nBuild succeeded (exit code 0)."));
        const QString rom = projectRomPath();
        if (!rom.isEmpty() && QFile::exists(rom)) {
            buildOutput_->appendPlainText(tr("Output ROM: %1").arg(rom));
        }
    } else if (exitCode > 0) {
        buildOutput_->appendPlainText(tr("\nBuild failed (exit code %1).").arg(exitCode));
    }
}

void MainWindow::onBuildError(const QString& message) {
    buildOutput_->appendPlainText(message);
    onBuildFinished(-1);
}

void MainWindow::runInEmulator() {
    QString romPath = projectRomPath();
    if (romPath.isEmpty() || !QFile::exists(romPath)) {
        const QString path = QFileDialog::getOpenFileName(
            this, tr("Select ROM"), settings_.lastRomDirectory(),
            tr("N64 ROMs (*.z64 *.n64 *.v64 *.zip);;All Files (*)"));
        if (path.isEmpty()) return;
        romPath = path;
        settings_.setLastRomDirectory(QFileInfo(romPath).absolutePath());
    }
    runRom(romPath);
}

void MainWindow::runRom(const QString& requestedPath) {
    if (requestedPath.isEmpty()) return;
    const auto path = QFileInfo(requestedPath).absoluteFilePath();
    if (!QFile::exists(path)) {
        QMessageBox::warning(this, tr("ROM Not Found"),
            tr("The ROM file no longer exists:\n%1").arg(path));
        return;
    }
    addRecentRom(path);
    settings_.setCurrentRom(path);
    autoPaused_ = false;
    if (pauseAction_) pauseAction_->setText(tr("Pause"));
    if (emuWidget_->loadRom(path)) {
        pauseAction_->setEnabled(true);
        resetEmuAction_->setEnabled(true);
        stopEmuAction_->setEnabled(true);
        tabs_->setCurrentWidget(emuPage_);
        presenceTitleArt_.clear();
        emuWidget_->setFocus(Qt::OtherFocusReason);
        emuWidget_->clearCover();
        if (gameLibrary_) gameLibrary_->recordPlayed(path);
        QString title = QFileInfo(path).completeBaseName();
        if (gameLibrary_) {
            if (auto entry = gameLibrary_->find(path)) {
                title = entry->title;
            }
        }
#ifdef N64FORGE_HAS_QT_NETWORK
        requestSplash(emuWidget_->romPath(), title);
#endif
        updatePresenceForCurrentGame(false);
        setWindowTitle(tr("%1 — N64Forge Studio").arg(title));
        statusBar()->showMessage(tr("Running %1").arg(title));
    } else {
        const bool loaded = emuWidget_->hasGame();
        pauseAction_->setEnabled(loaded);
        resetEmuAction_->setEnabled(loaded);
        stopEmuAction_->setEnabled(loaded);
        if (discord_) discord_->clearPresence();
        statusBar()->showMessage(tr("Could not load %1").arg(QFileInfo(path).fileName()));
    }
}

void MainWindow::updatePresenceForCurrentGame(bool preserveTimestamp) {
    if (!emuWidget_ || !emuWidget_->isRunning()) {
        if (discord_) discord_->clearPresence();
        return;
    }

    QString title = QFileInfo(emuWidget_->romPath()).completeBaseName();
    if (gameLibrary_) {
        if (auto entry = gameLibrary_->find(emuWidget_->romPath())) {
            title = entry->title;
        }
    }

    if (discord_) {
        QString coverUrl;
        if (gameLibrary_) {
            if (auto entry = gameLibrary_->find(emuWidget_->romPath())) {
                coverUrl = entry->coverUrl;
            }
        }
        coverUrl = ArtRepository::discordImage(coverUrl, presenceTitleArt_);
        const QString state = emuWidget_->isPaused() ? QStringLiteral("Paused") : QStringLiteral("Playing");
        // Pause/resume/reset updates keep the original session start timestamp.
        discord_->updatePresence(state.toStdString(),
                                 title.toStdString(),
                                 coverUrl.toStdString(), title.toStdString(),
                                 QStringLiteral("n64forge").toStdString(),
                                 QStringLiteral("Native Core").toStdString(),
                                 preserveTimestamp);
    }
}

#ifdef N64FORGE_HAS_QT_NETWORK
void MainWindow::requestSplash(const QString& path, const QString& title) {
    const auto entry = gameLibrary_->find(path);
    if (entry && !entry->coverPath.isEmpty()) {
        const QPixmap cover(entry->coverPath);
        if (!cover.isNull()) emuWidget_->setCover(cover);
    }
    gameDatabase_->fetchArtwork(title, ArtKind::Splash, [this, title, path](const auto& art) {
        if (emuWidget_->romPath() != path || !emuWidget_->isRunning() || art.image.isNull()) return;
        const auto current = gameLibrary_->find(path);
        if (!current || current->title == title) {
            emuWidget_->setCover(art.image);
            presenceTitleArt_ = art.url;
            updatePresenceForCurrentGame();
        }
    });
    gameDatabase_->fetchArtwork(title, ArtKind::Logo, [](const auto&) {});
    gameDatabase_->fetchArtwork(title, ArtKind::Snap, [](const auto&) {});
}

void MainWindow::downloadArtworkPack() {
    const auto repo = gameDatabase_->artRepository();
    const QString destination = repo.packPath();
    if (QFileInfo::exists(destination)) {
        statusBar()->showMessage(tr("Artwork pack already exists at %1").arg(destination), 8000);
        return;
    }
    const auto git = QStandardPaths::findExecutable(QStringLiteral("git"));
    if (git.isEmpty()) {
        QMessageBox::warning(this, tr("Artwork Pack"), tr("Git is required for pack downloads."));
        return;
    }
    if (QMessageBox::question(this, tr("Download Artwork Pack"),
            tr("Shallow-clone the main branch of\n%1\ninto\n%2?").arg(repo.url(), destination))
        != QMessageBox::Yes) return;
    if (!QDir().mkpath(repo.repoCache())) {
        statusBar()->showMessage(tr("Could not create the artwork cache."), 8000);
        return;
    }
    auto* process = new QProcess(this);
    process->setWorkingDirectory(repo.repoCache());
    auto env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("GIT_TERMINAL_PROMPT"), QStringLiteral("0"));
    process->setProcessEnvironment(env);
    process->setProgram(git);
    process->setArguments({QStringLiteral("clone"), QStringLiteral("--depth"), QStringLiteral("1"),
                           QStringLiteral("--single-branch"), QStringLiteral("--branch"), QStringLiteral("main"),
                           QStringLiteral("--"), repo.url(), destination});
    connect(process, &QProcess::finished, this, [this, process](int code, QProcess::ExitStatus status) {
        if (status == QProcess::NormalExit && code == 0) {
            gameLibrary_->refreshArtwork();
            statusBar()->showMessage(tr("Artwork pack downloaded."), 8000);
        } else {
            statusBar()->showMessage(tr("Artwork pack download failed: %1")
                .arg(QString::fromLocal8Bit(process->readAllStandardError()).trimmed().right(300)), 8000);
        }
        process->deleteLater();
    });
    connect(process, &QProcess::errorOccurred, this, [this, process](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart) return;
        statusBar()->showMessage(process->errorString(), 8000);
        process->deleteLater();
    });
    statusBar()->showMessage(tr("Downloading artwork pack…"));
    process->start();
}
#endif

void MainWindow::togglePause() {
    if (!emuWidget_ || !emuWidget_->isRunning()) {
        statusBar()->showMessage(tr("Nothing is running"));
        return;
    }
    if (emuWidget_->isPaused()) {
        emuWidget_->resume();
        emuWidget_->setFocus(Qt::OtherFocusReason);
        autoPaused_ = false;
        if (pauseAction_) pauseAction_->setText(tr("Pause"));
        statusBar()->showMessage(tr("Resumed"));
    } else {
        emuWidget_->pause();
        if (pauseAction_) pauseAction_->setText(tr("Resume"));
        statusBar()->showMessage(tr("Paused"));
    }
    updatePresenceForCurrentGame();
}

void MainWindow::toggleFullScreen() {
    if (isFullScreen()) {
        showNormal();
        menuBar()->show();
        statusBar()->show();
        if (fullScreenAction_) fullScreenAction_->setChecked(false);
    } else {
        menuBar()->hide();
        statusBar()->hide();
        showFullScreen();
        if (fullScreenAction_) fullScreenAction_->setChecked(true);
    }
}

void MainWindow::closeEvent(QCloseEvent* event) {
    if (emuWidget_) emuWidget_->stop();
    settings_.setWindowGeometry(saveGeometry());
    settings_.setSplitterState(split_->saveState());
    controllerManager_->saveSettings();
    event->accept();
}

bool MainWindow::eventFilter(QObject* watched, QEvent* event) {
    if (watched == this) {
        if (event->type() == QEvent::WindowDeactivate) {
            controllerManager_->resetAll();
            if (settings_.pauseOnFocusLoss() && emuWidget_ && emuWidget_->isRunning() &&
                !emuWidget_->isPaused()) {
                emuWidget_->pause();
                autoPaused_ = true;
                if (pauseAction_) pauseAction_->setText(tr("Resume"));
            }
        } else if (event->type() == QEvent::WindowActivate) {
            if (autoPaused_ && emuWidget_ && emuWidget_->isRunning() && emuWidget_->isPaused() &&
                tabs_->currentWidget() == emuPage_) {
                emuWidget_->resume();
                autoPaused_ = false;
                if (pauseAction_) pauseAction_->setText(tr("Pause"));
            }
        }
        return false;
    }

    if (event->type() == QEvent::KeyPress || event->type() == QEvent::KeyRelease) {
        if (!emuWidget_ || tabs_->currentWidget() != emuPage_ ||
            !emuWidget_->isRunning() || !isActiveWindow()) {
            return false;
        }
        auto* keyEvent = static_cast<QKeyEvent*>(event);
        if (keyEvent->isAutoRepeat()) return false;
        if (keyEvent->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier)) {
            return false;
        }
        if (keyEvent->type() == QEvent::KeyPress && keyEvent->key() == Qt::Key_Escape &&
            isFullScreen()) {
            toggleFullScreen();
            return true;
        }
    }
    return false;
}

void MainWindow::dragEnterEvent(QDragEnterEvent* event) {
    if (event->mimeData()->hasUrls()) {
        event->acceptProposedAction();
    }
}

void MainWindow::dropEvent(QDropEvent* event) {
    const auto urls = event->mimeData()->urls();
    for (const auto& url : urls) {
        const QString path = url.toLocalFile();
        if (path.isEmpty()) continue;
        const QFileInfo info(path);
        if (info.isDir()) {
            openProjectDir(path);
        } else if (path.endsWith(QLatin1String(".z64"), Qt::CaseInsensitive) ||
                   path.endsWith(QLatin1String(".n64"), Qt::CaseInsensitive) ||
                   path.endsWith(QLatin1String(".v64"), Qt::CaseInsensitive) ||
                   path.endsWith(QLatin1String(".zip"), Qt::CaseInsensitive)) {
            runRom(path);
            break;
        }
    }
}

void MainWindow::analyzeCurrentRom() {
    QString romPath = projectRomPath();
    if (romPath.isEmpty() || !QFile::exists(romPath)) {
        romPath = QFileDialog::getOpenFileName(
            this, tr("Select ROM to Analyze"), settings_.lastRomDirectory(),
            tr("N64 ROMs (*.z64 *.n64 *.v64 *.zip);;All Files (*)"));
        if (romPath.isEmpty()) return;
        settings_.setLastRomDirectory(QFileInfo(romPath).absolutePath());
    }
    runRecompCommand(romPath, QStringLiteral("analyze"));
}

void MainWindow::recompileCurrentRom() {
    QString romPath = projectRomPath();
    if (romPath.isEmpty() || !QFile::exists(romPath)) {
        romPath = QFileDialog::getOpenFileName(
            this, tr("Select ROM to Recompile"), settings_.lastRomDirectory(),
            tr("N64 ROMs (*.z64 *.n64 *.v64 *.zip);;All Files (*)"));
        if (romPath.isEmpty()) return;
        settings_.setLastRomDirectory(QFileInfo(romPath).absolutePath());
    }
    const QString outDir = QFileDialog::getExistingDirectory(
        this, tr("Select Output Directory for Generated C++"),
        settings_.lastProjectDirectory(),
        QFileDialog::ShowDirsOnly);
    if (outDir.isEmpty()) return;
    runRecompCommand(romPath, QStringLiteral("recompile"),
                     QStringList{QStringLiteral("-o"), outDir});
}

void MainWindow::setRecompButtonsEnabled(bool enabled) {
    if (analyzeButton_) analyzeButton_->setEnabled(enabled);
    if (recompileButton_) recompileButton_->setEnabled(enabled);
    if (nativeBuildButton_) nativeBuildButton_->setEnabled(enabled);
    if (cancelRecompButton_) cancelRecompButton_->setEnabled(!enabled);
}

void MainWindow::resetNativeBuildState() {
    nativeBuildStage_ = NativeBuildStage::None;
    nativeBuildProgram_.clear();
    nativeBuildDir_.clear();
    nativeBuildEnvironment_ = QProcessEnvironment();
    setRecompButtonsEnabled(true);
}

void MainWindow::cancelRecompProcess() {
    bool requested = false;
    if (recompProcess_->state() != QProcess::NotRunning) {
        recompCancelled_ = true;
        recompOutput_->appendPlainText(tr("\nTerminating analyzer/recompiler process..."));
        recompProcess_->terminate();
        if (!recompProcess_->waitForFinished(3000)) recompProcess_->kill();
        requested = true;
    }
    if (nativeBuildRunner_->isRunning()) {
        recompOutput_->appendPlainText(tr("\nCancelling native build..."));
        nativeBuildRunner_->cancel();
        requested = true;
    }
    if (requested) cancelRecompButton_->setEnabled(false);
}

void MainWindow::onRecompFinished(int exitCode) {
    if (recompCancelled_) {
        recompOutput_->appendPlainText(tr("\nProcess cancelled."));
        recompCancelled_ = false;
    } else {
        recompOutput_->appendPlainText(tr("\nProcess finished with exit code %1.").arg(exitCode));
    }
    setRecompButtonsEnabled(true);
}

void MainWindow::onRecompError(const QString& message) {
    recompOutput_->appendPlainText(tr("\n%1").arg(message));
    if (recompProcess_->state() == QProcess::NotRunning) {
        recompCancelled_ = false;
        setRecompButtonsEnabled(true);
    }
}

void MainWindow::buildNativeExecutable() {
    if (recompProcess_->state() != QProcess::NotRunning || nativeBuildRunner_->isRunning()) {
        QMessageBox::information(this, tr("Process Running"),
            tr("A recompiler/analyzer or native build process is already running."));
        return;
    }

    const QString outDir = QFileDialog::getExistingDirectory(
        this, tr("Select Recompiled Output Directory"),
        settings_.lastProjectDirectory(),
        QFileDialog::ShowDirsOnly);
    if (outDir.isEmpty()) return;

    const QString sourceDir = QDir(outDir).absolutePath();
    if (!QFile::exists(QDir(sourceDir).filePath(QStringLiteral("CMakeLists.txt")))) {
        recompOutput_->clear();
        recompOutput_->appendPlainText(
            tr("No CMakeLists.txt found in %1. Recompile a ROM first.").arg(sourceDir));
        return;
    }

    nativeBuildProgram_ = resolveProgram(
        settings_.value(QStringLiteral("tools/cmakePath")).toString(),
        QStringLiteral("cmake"));
    if (nativeBuildProgram_.isEmpty()) {
        QMessageBox::warning(this, tr("CMake Missing"),
            tr("Could not resolve 'cmake'. Install CMake or configure "
               "tools/cmakePath in Studio settings."));
        return;
    }

    const QString buildDir = QDir(sourceDir).filePath(QStringLiteral("build-native"));
    if (!QDir().mkpath(buildDir)) {
        QMessageBox::warning(this, tr("Build Directory"),
            tr("Could not create build directory:\n%1").arg(buildDir));
        return;
    }

    nativeBuildDir_ = buildDir;
    nativeBuildEnvironment_ = QProcessEnvironment::systemEnvironment();
    const QStringList configureArgs = cmakeConfigureArguments(sourceDir, buildDir);
    if (!approveCommand(this, tr("Approve CMake Configure"),
                        tr("Run the following CMake configure command?"),
                        nativeBuildProgram_, configureArgs, sourceDir,
                        nativeBuildEnvironment_)) {
        resetNativeBuildState();
        return;
    }

    recompOutput_->clear();
    appendCommandStart(recompOutput_, tr("Approved CMake configure command:"),
                       nativeBuildProgram_, configureArgs, sourceDir,
                       nativeBuildEnvironment_);
    setRecompButtonsEnabled(false);
    nativeBuildStage_ = NativeBuildStage::Configure;
    nativeBuildRunner_->start(nativeBuildProgram_, sourceDir, configureArgs,
                              nativeBuildEnvironment_);
}

void MainWindow::onNativeBuildFinished(int exitCode) {
    if (nativeBuildStage_ == NativeBuildStage::Configure) {
        if (exitCode != 0) {
            recompOutput_->appendPlainText(
                tr("\nCMake configure failed (exit code %1).").arg(exitCode));
            resetNativeBuildState();
            return;
        }

        const QStringList buildArgs = cmakeBuildArguments(nativeBuildDir_);
        if (!approveCommand(this, tr("Approve CMake Build"),
                            tr("Run the following CMake build command?"),
                            nativeBuildProgram_, buildArgs, nativeBuildDir_,
                            nativeBuildEnvironment_)) {
            recompOutput_->appendPlainText(tr("\nNative build cancelled by user."));
            resetNativeBuildState();
            return;
        }

        nativeBuildStage_ = NativeBuildStage::Build;
        appendCommandStart(recompOutput_, tr("Approved CMake build command:"),
                           nativeBuildProgram_, buildArgs, nativeBuildDir_,
                           nativeBuildEnvironment_);
        nativeBuildRunner_->start(nativeBuildProgram_, nativeBuildDir_, buildArgs,
                                  nativeBuildEnvironment_);
        return;
    }

    if (nativeBuildStage_ == NativeBuildStage::Build) {
        if (exitCode == 0) {
            recompOutput_->appendPlainText(
                tr("\nNative build succeeded (exit code 0).\nOutput directory: %1")
                    .arg(nativeBuildDir_));
        } else {
            recompOutput_->appendPlainText(
                tr("\nNative build failed (exit code %1).").arg(exitCode));
        }
    }
    resetNativeBuildState();
}

void MainWindow::onNativeBuildError(const QString& message) {
    recompOutput_->appendPlainText(tr("\n%1").arg(message));
    if (!nativeBuildRunner_->isRunning()) resetNativeBuildState();
}

void MainWindow::runRecompCommand(const QString& romPath, const QString& subCommand,
                                  const QStringList& extraArgs) {
    if (recompProcess_->state() != QProcess::NotRunning || nativeBuildRunner_->isRunning()) {
        QMessageBox::information(this, tr("Process Running"),
            tr("A recompiler/analyzer or native build process is already running."));
        return;
    }

    const QString cliPath = resolveSiblingCliExecutable(
        QCoreApplication::applicationFilePath());
    if (cliPath.isEmpty()) {
        QMessageBox::warning(this, tr("CLI Not Found"),
            tr("Could not find the sibling 'n64forge' CLI executable next to "
               "N64Forge Studio. Analyze and Recompile require the CLI."));
        recompOutput_->appendPlainText(tr("Could not resolve sibling n64forge CLI executable."));
        return;
    }

    const QStringList args = recompilerArguments(subCommand, romPath, extraArgs);
    const QString workingDir = currentProjectDir_.isEmpty()
        ? QFileInfo(romPath).absolutePath() : currentProjectDir_;
    const QProcessEnvironment env = QProcessEnvironment::systemEnvironment();

    if (!approveCommand(this, tr("Approve CLI Command"),
                        tr("Run the following n64forge CLI command?"),
                        cliPath, args, workingDir, env)) {
        return;
    }

    recompCancelled_ = false;
    recompOutput_->clear();
    appendCommandStart(recompOutput_, tr("Approved CLI command:"),
                       cliPath, args, workingDir, env);
    setRecompButtonsEnabled(false);

    recompProcess_->setProgram(cliPath);
    recompProcess_->setArguments(args);
    recompProcess_->setWorkingDirectory(workingDir);
    recompProcess_->setProcessEnvironment(env);
    recompProcess_->start();
}

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    app.setOrganizationName(QStringLiteral("N64Forge"));
    app.setApplicationName(QStringLiteral("Studio"));
    app.setApplicationVersion(QStringLiteral(N64FORGE_STUDIO_VERSION));
    {
        QFont appFont = QFontDatabase::systemFont(QFontDatabase::SystemFont::GeneralFont);
        appFont.setFamily(QStringLiteral("Helvetica Neue"));
        appFont.setPointSize(13);
        app.setFont(appFont);
    }
    app.setWindowIcon(QIcon(n64forge::studio::valkyrieIcon(256)));
    app.setPalette(n64forge::studio::darkPalette());
    app.setStyleSheet(retroStyleSheet());

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QCoreApplication::translate("main", "N64Forge Studio — emulator and homebrew toolkit."));
    parser.addHelpOption();
    parser.addVersionOption();
    QCommandLineOption runRomOption(QStringList{QStringLiteral("r"), QStringLiteral("run-rom")},
                                    QCoreApplication::translate("main",
                                        "Automatically load and run the specified ROM on startup."),
                                    QStringLiteral("path"));
    parser.addOption(runRomOption);
    const QCommandLineOption artRepoOption(QStringLiteral("art-repo-url"),
        QCoreApplication::translate("main", "Use an alternate HTTPS GitHub artwork repository for this session."),
        QStringLiteral("url"));
    parser.addOption(artRepoOption);
    const QCommandLineOption cpuModeOption(QStringLiteral("cpu-mode"),
        QStringLiteral("Use jit, cached, or interpreter for this session."), QStringLiteral("mode"));
    parser.addOption(cpuModeOption);
    const QCommandLineOption scaleOption(QStringLiteral("internal-scale"),
        QObject::tr("Internal resolution for presentation (1–4)."), QStringLiteral("scale"));
    parser.addOption(scaleOption);
    const QCommandLineOption graphicsOption(QStringLiteral("graphics-api"),
        QStringLiteral("Use auto, metal, moltenvk, kosmickrisp, software, d3d12, or vulkan for this session."), QStringLiteral("api"));
    parser.addOption(graphicsOption);
    parser.process(app);
    StudioSettings settings;
    if (parser.isSet(scaleOption)) {
        bool valid = false;
        const auto scale = parser.value(scaleOption).toUInt(&valid);
        if (!valid || scale < 1 || scale > 4) {
            qCritical("--internal-scale expects 1, 2, 3, or 4");
            return 2;
        }
        settings.setInternalScale(scale, true);
    }
    if (parser.isSet(graphicsOption)) {
        const auto api = n64forge::video::parseGraphicsApi(parser.value(graphicsOption).toStdString());
        if (!api) {
            qCritical("--graphics-api expects auto, metal, moltenvk, kosmickrisp, software, d3d12, or vulkan");
            return 2;
        }
        settings.setGraphicsApi(*api, true);
    }
    if (parser.isSet(cpuModeOption)) {
        const auto mode = n64forge::parseCpuMode(parser.value(cpuModeOption).toStdString());
        if (!mode) {
            qCritical("--cpu-mode expects jit, cached, or interpreter");
            return 2;
        }
        settings.setCpuMode(*mode, true);
    }
    QString artOverride;
    if (parser.isSet(artRepoOption)) {
        artOverride = ArtRepository::canonicalUrl(parser.value(artRepoOption));
        if (artOverride.isEmpty()) {
            qCritical("Invalid --art-repo-url: expected https://github.com/OWNER/REPOSITORY.git");
            return 2;
        }
    }

    MainWindow window(settings, artOverride);
    if (parser.isSet(runRomOption)) {
        window.runRom(parser.value(runRomOption));
    }
    window.show();
    return app.exec();
}

#include "main.moc"
