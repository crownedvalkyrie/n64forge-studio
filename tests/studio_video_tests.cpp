#include "n64forge/studio/emu_widget.hpp"
#include "n64forge/studio/audio_output.hpp"
#include "n64forge/studio/settings.hpp"
#include "n64forge/studio/controller_manager.hpp"
#include "n64forge/studio/credits_widget.hpp"
#include "n64forge/core/emulator.hpp"
#include "n64forge/studio/art_repository.hpp"
#include "n64forge/studio/game_database.hpp"
#include "n64forge/studio/guest_pacer.hpp"
#include "n64forge/studio/library_widget.hpp"
#include "n64forge/studio/style.hpp"

#include <QApplication>
#include <QColor>
#include <QTemporaryDir>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QPixmap>
#include <QPushButton>
#include <QStandardPaths>
#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iostream>
#include <utility>

#ifdef __APPLE__
namespace n64forge::video { bool verifyMetalPackedPixels(); }
#endif

namespace n64forge::studio {

struct StudioVideoProbe {
    static const QImage& image(const EmuWidget& widget) { return widget.presentedFrame_; }
    static QSize nativeSize(const EmuWidget& widget) { return widget.displayedFrame_.size(); }
    static qint64 timestamp(const EmuWidget& widget) { return widget.currentTimestamp_; }
    static bool interpolated(const EmuWidget& widget) {
        return widget.displayedFrame_.cacheKey() != widget.currentFrame_.cacheKey();
    }
    static u64 fullSyncs(const EmuWidget& widget) {
        return widget.emulator_->rdp().stats().fullSyncs;
    }
    static CoreStats stats(const EmuWidget& widget) { return widget.performanceSnapshot(); }
    static u64 underruns(const EmuWidget& widget) {
        return widget.audio_ ? widget.audio_->fifoStats().underruns : 0;
    }
    static void stopGuest(EmuWidget& widget) { widget.stopWorker(); widget.onFrame(); }
    static u64 ticks(const EmuWidget& widget) { return widget.presentationTicks_.load(); }
    static bool bootActive(const EmuWidget& widget) { return widget.bootSequence_.active(); }
    static QImage nativeFrame(const EmuWidget& widget) {
        return widget.currentPacked_ ? EmuWidget::decodePackedFrame(*widget.currentPacked_) : widget.currentFrame_;
    }
    static double gpuMs(const EmuWidget& widget) {
        return widget.viewportPresenter_ ? widget.viewportPresenter_->gpuMilliseconds() : 0;
    }
    static bool metalActive(const EmuWidget& widget) { return widget.viewportPresenter_ != nullptr; }
    static bool packedMetal(const EmuWidget& widget) { return bool(widget.currentPacked_); }
    static auto metalGeometry(const EmuWidget& widget) { return widget.viewportGeometry_; }
    static auto presentation(const EmuWidget& widget) {
        struct Result {
            n64forge::Emulator::ScanoutLayout layout;
            const n64forge::Rdp* rdp;
        };
        return Result{widget.emulator_ ? widget.emulator_->presentationLayout()
                                       : n64forge::Emulator::ScanoutLayout{},
                      widget.emulator_ ? &widget.emulator_->rdp() : nullptr};
    }
    static float metalBlend(qint64 timestamp, u64 revision, float last, bool enabled = true) {
        return EmuWidget::metalBlendValue(timestamp, revision, last, enabled);
    }
    static qint64 nextPresent(qint64 now, qint64 prev, int fps) {
        return EmuWidget::nextPresentationDeadline(now, prev, fps);
    }
    static auto geometry(QSize viewport, double dpr) {
        return EmuWidget::presentationGeometry(viewport, {320, 237}, dpr, 0);
    }
    static float metalBlend(const EmuWidget& widget) { return widget.viewportBlend_; }
    static void presentMetal(EmuWidget& widget) { widget.presentNativeViewport(); }
    static void testSaveReload(EmuWidget& widget, const QString& path) {
        widget.emulator_ = std::make_unique<Emulator>();
        widget.pakPaths_[0] = path;
        auto& pak = widget.emulator_->bus().hw().si().paks[0];
        pak.attach(PakType::Controller);
        assert(pak.info(false) == 3);
        std::array<u8, 32> data{};
        data.fill(0xA5);
        assert(pak.write(0x7FE0 | ControllerPak::addressChecksum(0x7FE0), data) ==
               ControllerPak::dataCrc(data));
        widget.stopWorker(); // saves after the guest has stopped
        QFile file(path);
        assert(file.open(QIODevice::ReadOnly));
        const auto bytes = file.readAll();
        ControllerPak reopened;
        assert(reopened.load({reinterpret_cast<const u8*>(bytes.constData()), size_t(bytes.size())}));
        assert(reopened.image().back() == 0xA5);
        assert(reopened.image()[0x101] == 0x71 && reopened.image()[0x201] == 0x71);
    }
    static void testLifecycle(EmuWidget& widget, ControllerManager& manager, const QString& rom) {
        int stopNotices = 0;
        u8 lastMask = 0xFF;
        manager.setRumbleObserver([&](u8 mask) {
            lastMask = mask;
            if (mask == 0) ++stopNotices;
        });

        // The observer sees the exact guest motor mask before SDL dispatch.
        manager.syncRumble(0b0101);
        assert(lastMask == 0b0101);

        // Pause must stop every motor immediately.
        widget.pause();
        assert(lastMask == 0 && stopNotices >= 1);
        assert(widget.isPaused());

        // Reset must clear active rumble even when a replacement worker starts.
        assert(widget.loadRom(rom));
        manager.syncRumble(0b0001);
        const auto beforeReset = stopNotices;
        widget.reset();
        assert(lastMask == 0 && stopNotices > beforeReset);
        assert(widget.audio_ == nullptr);

        // A fresh guest motor write before close must be cleared by stopWorker.
        manager.syncRumble(0b1010);
        widget.motorMask_.store(0b1010, std::memory_order_relaxed);
        widget.stop();
        assert(lastMask == 0 && stopNotices >= 2);
        assert(widget.audio_ == nullptr);
        assert(widget.motorMask_.load() == 0);

        manager.setRumbleObserver({});
    }
    static void testCartridgeSaveMedia(EmuWidget& widget, StudioSettings& settings) {
        using Media = StudioSettings::SaveMedia;
        QTemporaryDir dir;
        assert(dir.isValid());
        const auto saves = dir.path();
        const auto load = [&] {
            widget.emulator_ = std::make_unique<Emulator>();
            widget.loadSaveMedia(saves);
        };

        const auto verify = [&](Media media, const QString& suffix, qsizetype expectedSize,
                                const auto& dirty) {
            // Each variant starts from an empty save directory; a wrong-size
            // file from a previous size must be preserved, not overwritten.
            for (const auto* extension : {"fla", "sra", "eep"})
                QFile::remove(saves + QStringLiteral("/00000000-00000000.") + extension);
            settings.setSaveMedia(media);
            load();
            assert(widget.saveMedia_ == media);
            assert(widget.savePath_.endsWith(suffix));
            dirty(widget.emulator_->bus());
            widget.saveMedia();
            QFile file(widget.savePath_);
            assert(file.exists() && file.size() == expectedSize);
            assert(file.open(QIODevice::ReadOnly));
            assert(static_cast<u8>(file.readAll().front()) == 0x5A);
        };

        verify(Media::FlashRam, QStringLiteral(".fla"), FlashRam::Size, [](Bus& bus) {
            auto& flash = bus.flash();
            flash.command(0xB4000000); // load page
            flash.write(0, 0x5A);
            flash.command(0xA5000000); // program page 0
            flash.advance(HwRegisters::CpuClockHz);
            flash.command(0xF0000000); // array mode
            assert(flash.read(0) == 0x5A);
        });
        verify(Media::Sram, QStringLiteral(".sra"), Sram::Size, [](Bus& bus) {
            bus.sram().write(0, 0x5A);
        });
        verify(Media::Eeprom4K, QStringLiteral(".eep"), Eeprom::Size4K, [](Bus& bus) {
            assert(bus.eeprom().writeBlock(0, std::array<u8, 8>{0x5A, 0, 0, 0, 0, 0, 0, 0}));
        });
        verify(Media::Eeprom16K, QStringLiteral(".eep"), Eeprom::Size16K, [](Bus& bus) {
            assert(bus.eeprom().writeBlock(0, std::array<u8, 8>{0x5A, 0, 0, 0, 0, 0, 0, 0}));
        });
        assert(widget.emulator_->bus().eeprom().attached());
        assert(widget.emulator_->bus().piSaveMedia() == Bus::PiSaveMedia::None);

        settings.setSaveMedia(Media::Auto);
        load();
        // Auto reuses the single existing EEPROM but keeps its selection mode.
        assert(widget.saveMedia_ == Media::Auto && widget.savePath_.isEmpty());
        assert(!widget.autoEepromPath_.isEmpty());
        assert(widget.emulator_->bus().eeprom().is16K());
        std::array<u8, 8> block{};
        assert(widget.emulator_->bus().eeprom().readBlock(0, block) && block[0] == 0x5A);
        const auto sramPath = dir.filePath("00000000-00000000.sra");
        {
            QFile file(sramPath);
            assert(file.open(QIODevice::WriteOnly));
            assert(file.write(QByteArray(Sram::Size, '\x20')) == Sram::Size);
        }
        load();
        // Two valid save images are ambiguous: neither is attached or persisted.
        assert(widget.saveMedia_ == Media::Auto);
        assert(widget.autoEepromPath_.isEmpty() && widget.autoSramPath_.isEmpty() &&
               widget.autoFlashPath_.isEmpty());
        assert(!widget.emulator_->bus().eeprom().attached());
        assert(widget.emulator_->bus().piSaveMedia() == Bus::PiSaveMedia::None);
        assert(QFile::remove(sramPath));
        load();
        assert(widget.saveMedia_ == Media::Auto && widget.emulator_->bus().eeprom().is16K());
        const auto path = widget.autoEepromPath_;

        // A wrong-size replacement after load must survive a dirty flush too.
        assert(widget.emulator_->bus().eeprom().writeBlock(0, std::array<u8, 8>{0xAB}));
        {
            QFile file(path);
            assert(file.open(QIODevice::WriteOnly));
            assert(file.write(QByteArray(1, '\x11')) == 1);
        }
        widget.saveMedia();
        assert(QDir(saves).entryList(QDir::Files | QDir::Hidden) ==
               QStringList{QStringLiteral("00000000-00000000.eep")});

        // A corrupt save file is left untouched and is never marked active.
        settings.setSaveMedia(Media::Eeprom16K);
        load();
        assert(widget.savePath_.isEmpty());
        {
            QFile corrupt(path);
            assert(corrupt.open(QIODevice::ReadOnly) && corrupt.readAll() == QByteArray(1, '\x11'));
        }
        assert(QFile::remove(path));
        assert(QDir().mkpath(path)); // Non-regular path cannot be used as a save.
        load();
        assert(widget.savePath_.isEmpty());
        assert(widget.emulator_->bus().eeprom().writeBlock(0, std::array<u8, 8>{0xAB}));
        widget.saveMedia();
        assert(QFileInfo(path).isDir());

        settings.setSaveMedia(Media::None);
        widget.emulator_.reset();
    }

    static void testAutoSaveCreation(EmuWidget& widget, StudioSettings& settings) {
        using Media = StudioSettings::SaveMedia;
        QTemporaryDir dir;
        assert(dir.isValid());
        const auto saves = dir.path();
        const auto base = QStringLiteral("00000000-00000000.");
        const auto clear = [&] {
            for (const auto* extension : {"fla", "sra", "eep"})
                QFile::remove(saves + QLatin1Char('/') + base + extension);
        };
        const auto fresh = [&] {
            widget.emulator_ = std::make_unique<Emulator>();
            widget.loadSaveMedia(saves);
            assert(widget.saveMedia_ == Media::Auto && widget.savePath_.isEmpty());
            assert(!widget.autoEepromPath_.isEmpty() && !widget.autoSramPath_.isEmpty() &&
                   !widget.autoFlashPath_.isEmpty());
            assert(widget.emulator_->bus().eeprom().type() == Eeprom::Type::Eeprom4K);
            assert(widget.emulator_->bus().piSaveMedia() == Bus::PiSaveMedia::None);
        };
        const auto firstByte = [](const QString& path, qsizetype expectedSize) {
            QFile file(path);
            assert(file.exists() && file.size() == expectedSize);
            assert(file.open(QIODevice::ReadOnly));
            return static_cast<u8>(file.readAll().front());
        };

        settings.setSaveMedia(Media::Auto);

        // EEPROM detection comes from real Joybus channel-4 traffic, and block
        // 70 expands an auto-selected 4 Kbit device to 16 Kbit.
        clear();
        fresh();
        {
            auto& bus = widget.emulator_->bus();
            auto& si = bus.hw().si();
            si.pifRam.fill(0);
            si.pifRam[0] = 10; si.pifRam[1] = 1; si.pifRam[2] = 5; si.pifRam[3] = 70;
            for (int i = 0; i < 8; ++i) si.pifRam[4 + i] = 0x5A;
            si.joybusOffsets[4] = 0;
            si.preparePifRead({}, &bus.eeprom());
            assert(bus.eeprom().is16K() && bus.eeprom().dirty());
            bus.eeprom().advance(Eeprom::WriteCycles + 1);
            assert(bus.eeprom().writeBlock(0, std::array<u8, 8>{0x5A}));
        }
        widget.saveMedia();
        assert(firstByte(widget.autoEepromPath_, Eeprom::Size16K) == 0x5A);
        assert(!QFile::exists(widget.autoSramPath_) && !QFile::exists(widget.autoFlashPath_));
        assert(widget.diagnosticText().contains(QStringLiteral("Save media:")) &&
               widget.diagnosticText().contains(QStringLiteral("EEPROM 16K")));

        // SRAM detection comes from a PI-domain write, not a ROM header hint.
        clear();
        fresh();
        widget.emulator_->bus().write8(Sram::Base, 0x5A);
        assert(widget.emulator_->bus().piSaveMedia() == Bus::PiSaveMedia::Sram);
        assert(widget.emulator_->bus().sram().dirty());
        widget.saveMedia();
        assert(firstByte(widget.autoSramPath_, Sram::Size) == 0x5A);
        assert(!QFile::exists(widget.autoEepromPath_) && !QFile::exists(widget.autoFlashPath_));
        assert(widget.diagnosticText().contains(QStringLiteral("Save media:")) &&
               widget.diagnosticText().contains(QStringLiteral("SRAM 32 KiB")));

        // FlashRAM detection comes from a genuine command write plus page program.
        clear();
        fresh();
        {
            auto& bus = widget.emulator_->bus();
            bus.write32(FlashRam::Command, 0xB4000000);
            bus.write8(FlashRam::Base, 0x5A);
            bus.write32(FlashRam::Command, 0xA5000000);
            bus.flash().advance(HwRegisters::CpuClockHz);
            bus.write32(FlashRam::Command, 0xF0000000);
            assert(bus.piSaveMedia() == Bus::PiSaveMedia::FlashRam);
            assert(bus.flash().dirty() && bus.flash().read(0) == 0x5A);
        }
        widget.saveMedia();
        assert(firstByte(widget.autoFlashPath_, FlashRam::Size) == 0x5A);
        assert(!QFile::exists(widget.autoEepromPath_) && !QFile::exists(widget.autoSramPath_));
        assert(widget.diagnosticText().contains(QStringLiteral("Save media:")) &&
               widget.diagnosticText().contains(QStringLiteral("FlashRAM 128 KiB")));

        settings.setSaveMedia(Media::None);
        widget.emulator_.reset();
    }

    static void testPakPreservation(EmuWidget& widget, ControllerManager& manager) {
        QTemporaryDir dir;
        assert(dir.isValid());
        const auto path = dir.filePath("controller-1.mpk");
        {
            QFile file(path);
            assert(file.open(QIODevice::WriteOnly));
            assert(file.write(QByteArray(64, '\x5A')) == 64);
        }
        manager.setPakOptions(0, PakType::Controller, 100);
        widget.controllerManager_ = &manager;
        widget.emulator_ = std::make_unique<Emulator>();
        widget.loadSaveMedia(dir.path());
        assert(widget.emulator_->bus().hw().si().paks[0].info(false) == 3);
        assert(widget.pakPaths_[0].isEmpty()); // wrong size is never overwritten
        QFile file(path);
        assert(file.open(QIODevice::ReadOnly) && file.size() == 64);
        assert(file.readAll() == QByteArray(64, '\x5A'));
        widget.emulator_.reset();
        widget.controllerManager_.clear();
    }
};

namespace {

void testBootSequence() {
    BootSequence boot;
    boot.start(false);
    boot.observeVideo();
    assert(!boot.active() && boot.advance(2000) == 0);
    boot.start(true);
    boot.observeVideo();
    assert(boot.advance(1199) == 1 && boot.advance(1200) == 1);
    assert(boot.advance(1360) == 0.5 && boot.advance(1520) == 0 && !boot.active());
    boot.start(true);
    assert(boot.waitingForVideo() && boot.advance(4000) == 1);
    boot.observeVideo();
    assert(boot.advance(4000) == 1 && boot.advance(4160) == 0.5);
    boot.start(false);
    assert(boot.advance(4200) == 0);
    StudioSettings settings;
    assert(settings.bootSequenceEnabled());
    settings.setBootSequenceEnabled(false);
    assert(!StudioSettings{}.bootSequenceEnabled());
    settings.setBootSequenceEnabled(true);
    assert(StudioSettings{}.bootSequenceEnabled());
}

void testStudioSaveMedia() {
    StudioSettings settings;
    assert(settings.saveMedia() == StudioSettings::SaveMedia::Auto);
    settings.setSaveMedia(StudioSettings::SaveMedia::None);
    assert(StudioSettings{}.saveMedia() == StudioSettings::SaveMedia::None);
    settings.setSaveMedia(StudioSettings::SaveMedia::Sram);
    assert(StudioSettings{}.saveMedia() == StudioSettings::SaveMedia::Sram);
    settings.setFlashRamEnabled(true);
    assert(StudioSettings{}.saveMedia() == StudioSettings::SaveMedia::FlashRam);
    settings.setFlashRamEnabled(false);
    assert(StudioSettings{}.saveMedia() == StudioSettings::SaveMedia::None);
    settings.setSaveMedia(StudioSettings::SaveMedia::Eeprom16K);
    assert(StudioSettings{}.saveMedia() == StudioSettings::SaveMedia::Eeprom16K);
    settings.setSaveMedia(StudioSettings::SaveMedia::Auto);
    assert(StudioSettings{}.saveMedia() == StudioSettings::SaveMedia::Auto);
}

void testCpuModePreferences() {
    using n64forge::CpuExecutionMode;
    StudioSettings settings;
    assert(settings.cpuMode() == n64forge::defaultDesktopCpuMode());
    settings.setCpuMode(CpuExecutionMode::Interpreter);
    assert(StudioSettings{}.cpuMode() == CpuExecutionMode::Interpreter);
    settings.setCpuMode(CpuExecutionMode::Jit, true);
    assert(settings.cpuMode() == CpuExecutionMode::Jit);
    assert(StudioSettings{}.cpuMode() == CpuExecutionMode::Interpreter);
    settings.setCpuMode(CpuExecutionMode::Cached);
}

void testGraphicsApiPreferences() {
    using namespace n64forge::video;
    StudioSettings settings;
    const auto platformDefault = defaultGraphicsApi();
    assert(settings.graphicsApi() == platformDefault);
    assert(!parseGraphicsApi("bad"));
    for (const auto api : {GraphicsApi::Metal, GraphicsApi::MoltenVK,
                           GraphicsApi::KosmicKrisp, GraphicsApi::Software,
                           GraphicsApi::Auto, GraphicsApi::D3D12, GraphicsApi::Vulkan}) {
        assert(parseGraphicsApi(graphicsApiName(api)) == api);
        settings.setGraphicsApi(api);
        assert(StudioSettings{}.graphicsApi() == api);
        if (api == GraphicsApi::Software) assert(!createViewportPresenter(api));
        const auto probe = probeGraphicsApi(api);
        if (!probe.driverDetected) assert(probe.fallback == GraphicsApi::Software);
    }
    settings.setGraphicsApi(platformDefault);
    settings.setGraphicsApi(GraphicsApi::Auto, true);
    assert(settings.graphicsApi() == GraphicsApi::Auto);
    assert(StudioSettings{}.graphicsApi() == platformDefault);
}

void testCartridgeDeck(const QTemporaryDir& dir) {
    QByteArray rom(0x2000, '\0');
    rom.replace(0, 4, QByteArray::fromHex("80371240"));
    rom[0x3E] = 'P';
    rom[0x3F] = 2;
    QFile file(dir.filePath(QStringLiteral("deck.z64")));
    assert(file.open(QIODevice::WriteOnly));
    assert(file.write(rom) == rom.size());
    file.close();

    StudioSettings settings;
    assert(settings.uiParallaxEnabled());
    settings.setUiParallaxEnabled(false);
    assert(!StudioSettings{}.uiParallaxEnabled());
    settings.setUiParallaxEnabled(true);
    assert(StudioSettings{}.uiParallaxEnabled());
    assert(settings.rdpThreads() == 0);
    settings.setRdpThreads(4);
    assert(StudioSettings{}.rdpThreads() == 4);
    settings.setRdpThreads(3);
    assert(StudioSettings{}.rdpThreads() == 0);
    assert(settings.internalScale() == 1);
    settings.setInternalScale(3);
    assert(StudioSettings{}.internalScale() == 3);
    settings.setInternalScale(4, true);
    assert(settings.internalScale() == 4 && StudioSettings{}.internalScale() == 3);
    settings.setInternalScale(1);

    const QRectF card(30, 60, 160, 200);
    const QPointF pointer(175, 95);
    const auto tilt = cartridgeParallaxOffset(card, pointer, true);
    assert(tilt.x() > 0.0 && tilt.x() <= 6.0 && tilt.y() < 0.0 && tilt.y() >= -6.0);
    assert(cartridgeParallaxOffset(card, pointer, false) == QPointF{});
    assert(cartridgeParallaxOffset(card, card.center(), true) == QPointF{});
    assert(cartridgeParallaxOffset(card, QPointF(29, 95), true) == QPointF{});
    settings.setSaveMedia(StudioSettings::SaveMedia::FlashRam);
    settings.setViewportScale(4);
    settings.setUltrawideEnabled(true);
    settings.setScalingFilter(QStringLiteral("smooth"));
    settings.setFrameInterpolation(true);
    GameLibrary library(&settings);
    library.addRom(file.fileName());
    const auto entry = GameLibrary(&settings).find(file.fileName());
    assert(entry && entry->videoRegion == QStringLiteral("PAL") && entry->revision == 2);
    assert(entry->cic.isEmpty()); // No boot code or authenticated CIC.

    LibraryWidget widget(&library);
    widget.setStyleSheet(retroStyleSheet());
    widget.resize(640, 440);
    widget.show();
    auto* list = widget.findChild<QListWidget*>();
    auto* details = widget.findChild<QLabel*>(QStringLiteral("cartridgeHardwareDetails"));
    auto* apply = widget.findChild<QPushButton*>(QStringLiteral("applyRecommendedSettings"));
    assert(list && list->count() == 1 && details && apply && !apply->isEnabled());
    assert(!apply->isVisible());
    list->setCurrentRow(0);
    assert(details->text().contains(QStringLiteral("PAL  ·  Unknown")));
    assert(details->text().contains(QStringLiteral("not declared")));
    assert(apply->isEnabled());
    QObject::connect(&widget, &LibraryWidget::recommendedSettingsRequested, &settings,
                     &StudioSettings::applyRecommendedVideoSettings);
    apply->click();
    assert(settings.viewportScale() == 0 && !settings.ultrawideEnabled() &&
           settings.scalingFilter() == QStringLiteral("nearest") && !settings.frameInterpolation());
    assert(settings.saveMedia() == StudioSettings::SaveMedia::FlashRam);
    widget.setParallaxEnabled(false);
    assert(!widget.grab().isNull());
}

void testControllerSettings() {
    using namespace n64forge::input;
    const auto oldFormat = QSettings::defaultFormat();
    QSettings::setDefaultFormat(QSettings::IniFormat);
    {
        ControllerManager manager;
        assert(manager.mappings(1).empty());
        assert(manager.handleKeyPress(Qt::Key_Space, true));
        assert(manager.combinedState(0).buttons & static_cast<u16>(ControlButtons[0]));
        manager.resetAll();
        assert(manager.combinedState(0).buttons == 0);
        manager.bind(0, Control::A, Qt::Key_M);
#ifdef N64FORGE_HAS_SDL3
        assert(SdlInputManager::rumbleStrength(0) == 0);
        assert(SdlInputManager::rumbleStrength(1) == 655);
        assert(SdlInputManager::rumbleStrength(35) == 22937);
        assert(SdlInputManager::rumbleStrength(100) == 65535);
        assert(SdlInputManager::rumbleStrength(200) == 65535);
        for (unsigned percent = 1; percent <= 100; ++percent) {
            assert(SdlInputManager::rumbleStrength(percent) >
                   SdlInputManager::rumbleStrength(percent - 1));
        }
        auto profile = PadProfile::defaults();
        profile.deadzone = 0.25;
        profile.bindings[0] = {};
        manager.setProfile(0, profile);
        manager.saveSettings();
        PadSnapshot sample;
        sample.axes[SDL_GAMEPAD_AXIS_LEFTX] = 4000;
        assert(sample.mapped(profile).stickX == 0);
        sample.axes[SDL_GAMEPAD_AXIS_LEFTX] = -32768;
        assert(sample.mapped(profile).stickX == -80);
        sample.axes[SDL_GAMEPAD_AXIS_LEFTX] = 32767;
        assert(sample.mapped(profile).stickX == 80);
#endif
        manager.setPakOptions(1, PakType::Rumble, 35);
        manager.setPakOptions(2, PakType::Controller, 80);
        manager.saveSettings();
        ControllerManager reloaded;
        assert(reloaded.pakType(0) == PakType::Controller);
        assert(reloaded.pakType(1) == PakType::Rumble && reloaded.rumbleIntensity(1) == 35);
        assert(reloaded.pakType(2) == PakType::Controller && reloaded.rumbleIntensity(2) == 80);
        assert(!reloaded.handleKeyPress(Qt::Key_Space, true));
        assert(reloaded.handleKeyPress(Qt::Key_M, true));
        assert(reloaded.combinedState(0).buttons & static_cast<u16>(ControlButtons[0]));
        const auto mappings = reloaded.mappings(0);
        reloaded.loadSettings();
        assert(mappings == reloaded.mappings(0));
#ifdef N64FORGE_HAS_SDL3
        assert(reloaded.profile(0).deadzone == 0.25);
        assert(reloaded.profile(0).bindings[0].kind == PadBinding::None);
#endif
    }
    QSettings::setDefaultFormat(oldFormat);
}

void testArtRepository(const QTemporaryDir& dir) {
    const ArtRepository repo(ArtRepository::defaultUrl(), dir.path());
    const auto title = QString::fromUtf8("Custom Racer #1 (Éurope)");
    const auto cover = repo.imageUrl(ArtKind::Cover, title);
    const auto splash = repo.imageUrl(ArtKind::Splash, title);
    assert(cover == QStringLiteral("https://raw.githubusercontent.com/crownedvalkyrie/"
        "N64ForgeStudio_CoverArt/main/Named_Boxarts/Custom%20Racer%20%231%20%28%C3%89urope%29.png"));
    assert(splash.contains("/Named_Titles/"));
    assert(repo.imageUrl(ArtKind::Logo, title).contains("/Named_Logos/"));
    assert(repo.imageUrl(ArtKind::Snap, title).contains("/Named_Snaps/"));
    assert(ArtRepository::discordImage(cover, splash) == cover);
    assert(ArtRepository::discordImage({}, splash) == splash);
    assert(ArtRepository::discordImage({}, {}) == QStringLiteral("n64forge_logo"));
    assert(ArtRepository::canonicalUrl("https://github.com/any-owner/art") ==
           QStringLiteral("https://github.com/any-owner/art.git"));
    assert(ArtRepository::canonicalUrl("https://github.com/../art").isEmpty());
    assert(ArtRepository::canonicalUrl("file:///some/path").isEmpty());
    const ArtRepository other("https://github.com/any-owner/art", dir.path());
    assert(other.imagePath(ArtKind::Cover, title) != repo.imagePath(ArtKind::Cover, title));
    assert(repo.imagePath(ArtKind::Cover, title) != repo.imagePath(ArtKind::Splash, title));
#ifdef Q_OS_MACOS
    assert(repo.root() == dir.filePath("Library/Caches/n64forge/covers"));
#else
    assert(repo.root() == dir.filePath(".cache/n64forge/covers"));
#endif
#ifdef N64FORGE_HAS_QT_NETWORK
    GameDatabase database(nullptr, repo);
    const auto localCover = repo.imagePath(ArtKind::Cover, title);
    const auto localSplash = repo.packedImagePath(ArtKind::Splash, title);
    QPixmap image(4, 4);
    image.fill(Qt::cyan);
    for (const auto& path : {localCover, localSplash}) {
        assert(QDir().mkpath(QFileInfo(path).absolutePath()));
        assert(image.save(path, "PNG"));
    }
    bool usedCache = false;
    database.fetchCover(title, [&](const Artwork& art) {
        usedCache = true;
        assert(art.url == cover && art.path == localCover && !art.image.isNull());
    });
    assert(usedCache); // no network or event processing
    const auto local = database.cachedArtwork(title, ArtKind::Splash);
    assert(local.url == splash && local.path == localSplash && !local.image.isNull());
#endif
}

void testHostCadenceWithoutGuest(EmuWidget& widget, StudioSettings& settings) {
    StudioVideoProbe::stopGuest(widget);
    settings.setFrameInterpolation(true);
    for (int hz : {60, 120}) {
        settings.setRenderFps(hz);
        widget.applyVideoSettings();
        const auto start = StudioVideoProbe::ticks(widget);
        const auto guestStamp = StudioVideoProbe::timestamp(widget);
        QElapsedTimer clock;
        clock.start();
        while (clock.elapsed() < 400) {
            QApplication::processEvents();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        const auto measured = (StudioVideoProbe::ticks(widget) - start) * 1000.0 / clock.elapsed();
        assert(StudioVideoProbe::timestamp(widget) == guestStamp);
        assert(measured > hz * 0.77 && measured < hz * 1.13);
    }
}

void testMetalInterpolationBlend() {
    using Probe = StudioVideoProbe;
    assert(Probe::metalBlend(1, 0, 1.f) == 1.f);
    assert(Probe::metalBlend(2, 1, 1.f) == 0.5f);
    assert(Probe::metalBlend(2, 2, 0.5f) == 1.f);
    assert(Probe::metalBlend(2, 2, 1.f) == 1.f); // Static: never return to an old image.
    assert(Probe::metalBlend(3, 2, 0.5f) == 1.f); // No midpoint starvation at source cadence.
    assert(Probe::metalBlend(3, 2, 1.f, false) == 1.f);
    for (const int fps : {60, 120}) {
        const qint64 period = 1'000'000'000 / fps;
        const auto first = Probe::nextPresent(0, 0, fps);
        assert(first == period);
        assert(Probe::nextPresent(first + 1000, first, fps) == 2 * period);
        assert(Probe::nextPresent(3 * period, first, fps) == 4 * period);
    }
}

void testCreditsAndPalette() {
    const auto book = parseCredits(bundledCreditsJson(), QStringLiteral(":/credits/credits.json"));
    assert(book.ok && book.error.isEmpty());
    // The bundled resource, not just the compiled-in emergency defaults, must
    // be reachable from the static viewport library.
    assert(bundledCreditsJson().contains("Homebrew Community"));
    assert(book.groups.size() == 3);
    assert(book.groups[0].key == QStringLiteral("devs"));
    assert(book.groups[1].key == QStringLiteral("supporters"));
    assert(book.groups[2].key == QStringLiteral("testers"));
    assert(!book.groups[0].entries.isEmpty());

    const auto broken = parseCredits(QByteArrayLiteral("{ not json"));
    assert(!broken.ok && !broken.error.isEmpty());
    const auto wrongType = parseCredits(QByteArrayLiteral("{\"devs\": 4}"));
    assert(!wrongType.ok && wrongType.error.contains(QStringLiteral("devs")));
    // Entries without a name are skipped instead of producing an empty card.
    const auto nameless = parseCredits(QByteArrayLiteral(
        "{\"devs\":[{\"role\":\"x\"},{\"name\":\"Ada Lovelace\",\"role\":\"Dev\"}]}"));
    assert(nameless.ok && nameless.groups[0].entries.size() == 1);
    assert(nameless.groups[0].entries[0].name == QStringLiteral("Ada Lovelace"));

    assert(creditInitials(QStringLiteral("Ada Lovelace")) == QStringLiteral("AL"));
    assert(creditInitials(QStringLiteral("N64Forge Team")) == QStringLiteral("NT"));
    assert(creditInitials(QStringLiteral("Prince")) == QStringLiteral("PR"));
    assert(creditInitials(QString()) == QStringLiteral("?"));
    // Accent colors are hash-derived so the same name always looks the same.
    assert(creditAccent(QStringLiteral("Ada Lovelace")) ==
           creditAccent(QStringLiteral("Ada Lovelace")));

    CreditEntry entry;
    entry.name = QStringLiteral("Ada Lovelace");
    const auto avatar = renderCreditAvatar(entry, 44);
    assert(!avatar.isNull() && avatar.size() == QSize(44, 44));
    assert(avatar.toImage().pixelColor(22, 22).alpha() > 0);

    CreditsView view;
    assert(view.groupKeys() ==
           QStringList({QStringLiteral("devs"), QStringLiteral("supporters"),
                        QStringLiteral("testers")}));
    assert(view.cardCount(QStringLiteral("devs")) >= 1);
    QTemporaryDir dir;
    assert(dir.isValid());
    const auto path = dir.filePath(QStringLiteral("credits.json"));
    QFile file(path);
    assert(file.open(QIODevice::WriteOnly));
    assert(file.write(QByteArrayLiteral(
        "{\"devs\":[{\"name\":\"A\",\"pfp\":\"missing.png\"}],"
        "\"supporters\":[],\"testers\":[{\"name\":\"B\"},{\"name\":\"C\"}]}")) > 0);
    file.close();
    view.loadFromFile(path);
    assert(view.book().ok);
    assert(view.cardCount(QStringLiteral("testers")) == 2);
    view.reload();
    assert(view.book().ok);

    // macOS must never paint black text on the charcoal window surface.
    const auto palette = darkPalette();
    const auto luminance = [](QColor color) {
        const auto channel = [](double value) {
            value /= 255.0;
            return value <= 0.03928 ? value / 12.92
                                    : std::pow((value + 0.055) / 1.055, 2.4);
        };
        return 0.2126 * channel(color.red()) + 0.7152 * channel(color.green()) +
               0.0722 * channel(color.blue());
    };
    const auto window = palette.color(QPalette::Window);
    const auto text = palette.color(QPalette::WindowText);
    assert(window.lightness() < 60);
    const auto light = luminance(text), dark = luminance(window);
    const double contrast = (std::max(light, dark) + 0.05) / (std::min(light, dark) + 0.05);
    assert(contrast >= 7.0); // WCAG AAA for body text.
    assert(palette.color(QPalette::Base).lightness() < 60);
    assert(palette.color(QPalette::Text).lightness() > 200);
    assert(!retroStyleSheet().isEmpty());
}

void testVideoHeaderHygiene() {
    const QStringList forbidden{QStringLiteral("@interface"), QStringLiteral("@implementation"),
                                QStringLiteral("Foundation.h"), QStringLiteral("Metal.h"),
                                QStringLiteral("QuartzCore.h")};
    const auto locateHeaders = [](const QDir& start) {
        QDir dir = start;
        for (int depth = 0; depth < 8; ++depth) {
            const QString candidate = dir.filePath(QStringLiteral("include/n64forge/video"));
            if (QDir(candidate).exists()) return candidate;
            if (!dir.cdUp()) break;
        }
        return QString{};
    };
    // The source path is absolute in CMake builds; the other roots cover
    // out-of-tree builds and ctest's build-directory working directory.
    QString path;
    const QFileInfo source(QString::fromUtf8(__FILE__));
    if (source.exists()) path = locateHeaders(QDir(source.absolutePath()));
    if (path.isEmpty()) path = locateHeaders(QDir::current());
    if (path.isEmpty()) path = locateHeaders(QDir(QApplication::applicationDirPath()));
    assert(!path.isEmpty());
    if (path.isEmpty()) {
        std::cerr << "Could not locate include/n64forge/video for header hygiene check\n";
        return;
    }

    int files = 0;
    const auto scan = [&](const auto& self, const QDir& dir) -> void {
        for (const QString& name : dir.entryList(QDir::Files | QDir::Hidden, QDir::Name)) {
            QFile file(dir.filePath(name));
            assert(file.open(QIODevice::ReadOnly));
            if (!file.isOpen()) continue;
            const QByteArray contents = file.readAll();
            for (const QString& token : forbidden) {
                const QByteArray needle = token.toUtf8();
                const bool found = contents.contains(needle);
                if (found) {
                    std::cerr << "Public video header contains " << token.toStdString()
                              << ": " << file.fileName().toStdString() << '\n';
                }
                assert(!found);
            }
            ++files;
        }
        const QDir::Filters dirs =
            QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks | QDir::Hidden;
        for (const QString& name : dir.entryList(dirs, QDir::Name))
            self(self, QDir(dir.filePath(name)));
    };
    scan(scan, QDir(path));
    assert(files > 0);
}

void testRetinaViewport() {
    using Geometry = n64forge::video::PresentationGeometry;
    using Probe = StudioVideoProbe;
    for (const double dpr : {1.0, 2.0}) {
        const auto s = static_cast<u32>(dpr);
        assert((Probe::geometry({1600, 900}, dpr) ==
                Geometry{1600*s, 900*s, 200*s, 0, 1200*s, 900*s, dpr}));
        assert((Probe::geometry({640, 800}, dpr) ==
                Geometry{640*s, 800*s, 0, 160*s, 640*s, 480*s, dpr}));
        assert((Probe::geometry({512, 512}, dpr) ==
                Geometry{512*s, 512*s, 0, 64*s, 512*s, 384*s, dpr}));
        const auto odd = Probe::geometry({641, 799}, dpr);
        assert(odd.x + odd.imageWidth <= odd.width && odd.y + odd.imageHeight <= odd.height);
        assert(odd.height - (odd.y + odd.imageHeight) - odd.y <= 1);
    }
}

void testGuestPacing() {
    using namespace std::chrono_literals;
    const GuestPacer::Clock::time_point start{};
    GuestPacer pacer(start);
    // 93,750 sub-nanosecond-rounded increments still sum to exactly one second.
    auto deadline = start;
    for (int i = 0; i < 93'750; ++i) deadline = pacer.advance(1000, deadline);
    assert(deadline == start + 1s);
    assert(pacer.advance(937'500, start + 1s + 70ms) == start + 1s + 10ms);
    assert(pacer.advance(937'500, start + 2s) == start + 2s);
    assert(pacer.advance(937'500, start + 2s) == start + 2s + 10ms);
}

// Uses real MIPS stores to configure VI, write a pixel, and submit RDP work.
// No boot firmware, external ROM, timer hooks or injected GUI frames.
QString writeVideoRom(const QTemporaryDir& dir) {
    QByteArray rom(0x2000, '\0');
    const auto word = [&](int offset, u32 value) {
        for (int i = 0; i < 4; ++i) rom[offset + i] = char(value >> (24 - i * 8));
    };
    word(0, 0x80371240);
    word(8, 0x80000400);
    int offset = 0x1000;
    const auto instruction = [&](u32 value) { word(offset, value); offset += 4; };
    instruction(0x3C01A440); // $1 = uncached VI base
    const auto writeVi = [&](u32 reg, u32 value) {
        instruction(0x3C020000 | (value >> 16));
        instruction(0x34420000 | (value & 0xFFFF));
        instruction(0xAC220000 | reg);
    };
    writeVi(0x04, 0x2000);
    writeVi(0x08, 16);
    writeVi(0x18, 525);
    writeVi(0x1C, 3093);
    writeVi(0x24, (108 << 16) | 124);
    writeVi(0x28, (37 << 16) | 69);
    writeVi(0x30, 1024);
    writeVi(0x34, 1024);
    writeVi(0x00, 3);
    instruction(0x3C02FF00);
    instruction(0x344200FF);
    instruction(0x3C04A000); // $4 = uncached RDRAM base
    instruction(0xAC822000); // red pixel directly to guest RDRAM
    instruction(0x3C01A410); // DPC registers
    instruction(0x240204C0);
    instruction(0xAC220000); // DPC_START
    instruction(0x240204E8);
    instruction(0xAC220004); // DPC_END
    instruction(0x340240FF); // third pixel alternates blue intensity across fields
    instruction(0xAC822008);
    instruction(0x3C030010);
    instruction(0x2463FFFF);
    instruction(0x1460FFFE);
    instruction(0);
    instruction(0x38428000);
    instruction(0x1000FFF9);
    instruction(0);
    offset = 0x10C0;
    for (u64 command : {0xFF18000F00002000ULL, // RGBA32, width 16
                        0xEF30000000000000ULL, // fill cycle
                        0xF700000000FF00FFULL, // green
                        0xF600400000004000ULL, // fill (1,0)
                        0xE900000000000000ULL}) {
        instruction(static_cast<u32>(command >> 32));
        instruction(static_cast<u32>(command));
    }
    QFile file(dir.filePath("video.z64"));
    assert(file.open(QIODevice::WriteOnly));
    assert(file.write(rom) == rom.size());
    return file.fileName();
}

int nonBlack(const QImage& image) {
    int count = 0;
    for (int y = 0; y < image.height(); ++y) {
        const auto* line = image.constScanLine(y);
        for (int x = 0; x < image.width(); ++x)
            count += bool(line[x * 4] | line[x * 4 + 1] | line[x * 4 + 2]);
    }
    return count;
}

bool waitForVideo(EmuWidget& widget, int scale, bool synthetic, u64 minSyncs,
                  qint64 interpolateAfter = -1) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 25'000) {
        QApplication::processEvents();
        const auto& frame = StudioVideoProbe::image(widget);
        const u64 syncs = StudioVideoProbe::fullSyncs(widget);
        if (interpolateAfter >= 0 &&
            (StudioVideoProbe::timestamp(widget) <= interpolateAfter || !StudioVideoProbe::interpolated(widget))) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }
        if (syncs >= minSyncs && frame.size() == StudioVideoProbe::nativeSize(widget) &&
            (!synthetic || frame.size() == QSize(16, 16)) &&
            frame.format() == QImage::Format_RGBA8888 && nonBlack(frame) > 0) {
            const auto painted = widget.grab().toImage().convertToFormat(QImage::Format_RGBA8888);
            if (nonBlack(painted) == 0) {
                std::cerr << "Studio image arrived but widget grab was black\n";
                return false;
            }
            if (synthetic) {
                const auto target = EmuWidget::videoRect(widget.size(), frame.size(), scale);
                if (frame.pixelColor(0, 0) != QColor(Qt::red) ||
                    frame.pixelColor(1, 0) != QColor(Qt::green) ||
                    nonBlack(frame) != 3 ||
                    painted.pixelColor(target.topLeft()) != QColor(Qt::red)) {
                    std::cerr << "Unexpected synthetic image at " << scale << "x: "
                              << frame.pixelColor(0, 0).name().toStdString() << ", "
                              << frame.pixelColor(1, 0).name().toStdString() << ", "
                              << nonBlack(frame) << " non-black pixels\n";
                    return false;
                }
            }
            std::cout << "Studio RGBA " << frame.width() << 'x' << frame.height()
                      << ", non-black pixels: " << nonBlack(frame)
                      << ", RDP full syncs: " << syncs << '\n';
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    std::cerr << "Timed out waiting for non-black Studio video at " << scale
              << "x (minimum RDP full syncs: " << minSyncs << ")\n";
    return false;
}

} // namespace
} // namespace n64forge::studio

int run_studio_video_tests(int argc, char** argv) {
    using namespace n64forge::studio;
    const bool artProbe = std::strcmp(argv[1], "--studio-art") == 0;
    const bool nativeProbe = std::strcmp(argv[1], "--studio-native") == 0;
    const bool benchProbe = std::strcmp(argv[1], "--studio-bench") == 0;
    const auto rom = argc > 2 ? QString::fromLocal8Bit(argv[2]) : QString{};
    const auto minSyncs = rom.isEmpty() ? 1 : argc > 3 ? QString::fromLatin1(argv[3]).toULongLong() : 0;
    const int sampleAfterMs = argc > 4 ? std::clamp(QString::fromLatin1(argv[4]).toInt(), 0, 9'000) : 0;
    QStandardPaths::setTestModeEnabled(true);
    QApplication app(argc, argv);
    app.setOrganizationName(QStringLiteral("N64ForgeTests"));
    app.setApplicationName(QStringLiteral("StudioVideo"));
    QTemporaryDir dir;
    assert(dir.isValid());
    if (artProbe) {
#ifdef N64FORGE_HAS_QT_NETWORK
        if (rom.isEmpty()) return 2;
        GameDatabase database(nullptr, ArtRepository(ArtRepository::defaultUrl(), dir.path()));
        int remaining = 4, downloaded = 0;
        for (auto kind : {ArtKind::Cover, ArtKind::Splash, ArtKind::Logo, ArtKind::Snap}) {
            database.fetchArtwork(rom, kind, [&](const Artwork& art) {
                --remaining;
                if (!art.image.isNull() && QFileInfo::exists(art.path)) {
                    ++downloaded;
                    std::cout << art.url.toStdString() << '\n';
                }
            });
        }
        QElapsedTimer deadline;
        deadline.start();
        while (remaining && deadline.elapsed() < 15'000) {
            QApplication::processEvents();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return downloaded == 4 ? 0 : 1;
#else
        return 77;
#endif
    }
    if (rom.isEmpty()) {
        testArtRepository(dir);
        testCreditsAndPalette();
        assert(EmuWidget::videoRect({1600, 900}, {320, 237}, 0) == QRect(200, 0, 1200, 900));
        assert(EmuWidget::videoRect({1600, 900}, {320, 237}, 0, true) == QRect(200, 0, 1200, 900));
        assert(EmuWidget::videoRect({900, 1600}, {320, 237}, 0, true) == QRect(0, 462, 900, 675));
        assert(EmuWidget::videoRect({640, 800}, {320, 237}, 0) == QRect(0, 160, 640, 480));
        assert(EmuWidget::videoRect({1600, 900}, {320, 237}, 2) == QRect(480, 210, 640, 480));
        assert(EmuWidget::videoRect({640, 480}, {320, 237}, 4) == QRect(0, 0, 640, 480));
    }
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, dir.path());
    if (rom.isEmpty()) {
        testBootSequence();
        testStudioSaveMedia();
        testCpuModePreferences();
        testGraphicsApiPreferences();
        testCartridgeDeck(dir);
        testMetalInterpolationBlend();
        testVideoHeaderHygiene();
        testRetinaViewport();
#ifdef __APPLE__
        assert(n64forge::video::verifyMetalPackedPixels());
#endif
        testGuestPacing();
        testControllerSettings();
        ControllerManager lifecycleManager;
        StudioSettings lifecycleSettings;
        lifecycleSettings.setSaveMedia(StudioSettings::SaveMedia::None);
        for (int port = 0; port < 4; ++port)
            lifecycleManager.setPakOptions(port, n64forge::PakType::None, 100);
        EmuWidget lifecycleWidget(&lifecycleManager, &lifecycleSettings);
        StudioVideoProbe::testLifecycle(lifecycleWidget, lifecycleManager, writeVideoRom(dir));
    }
    StudioSettings settings;
    // ROM probes must not load, create or update Studio save images.
    settings.setSaveMedia(StudioSettings::SaveMedia::None);
    settings.setScalingFilter("nearest");
    settings.setBootSequenceEnabled(false);
    EmuWidget widget(nullptr, &settings);
    if (benchProbe) {
        if (rom.isEmpty() || QApplication::platformName() != QStringLiteral("cocoa")) return 2;
        const int scale = argc > 3 ? std::clamp(QString::fromLatin1(argv[3]).toInt(), 0, 4) : 4;
        const int seconds = argc > 4 ? std::clamp(QString::fromLatin1(argv[4]).toInt(), 1, 30) : 8;
        const bool smooth = argc > 5 && std::strcmp(argv[5], "linear") == 0;
        const int fps = argc > 6 && std::strcmp(argv[6], "60") == 0 ? 60 : 120;
        if (argc > 7) {
            const auto mode = n64forge::parseCpuMode(argv[7]);
            if (!mode) return 2;
            settings.setCpuMode(*mode, true);
        }
        if (argc > 8) {
            const auto api = n64forge::video::parseGraphicsApi(argv[8]);
            if (!api || *api == GraphicsApi::Software) return 2;
            settings.setGraphicsApi(*api, true);
        }
        if (argc > 9) {
            bool valid = false;
            const auto internalScale = QString::fromLatin1(argv[9]).toUInt(&valid);
            if (!valid || internalScale < 1 || internalScale > 4) return 2;
            settings.setInternalScale(internalScale, true);
        }
        settings.setViewportScale(scale);
        settings.setScalingFilter(smooth ? "linear" : "nearest");
        settings.setRenderFps(fps);
        settings.setFrameInterpolation(true);
        widget.setDiagnosticsVisible(true);
        widget.applyVideoSettings();
        // Fit must exceed the 1280-pixel width of a typical 320-wide 4×
        // source, or the two settings exercise the same fitted rectangle.
        widget.resize(1600, 1050);
        widget.show();
        const bool synthetic = rom == QStringLiteral("--synthetic");
        if (!widget.loadRom(synthetic ? writeVideoRom(dir) : rom)) return 1;
        QElapsedTimer deadline;
        deadline.start();
        while (deadline.elapsed() < 3000) {
            QApplication::processEvents();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        double minSpeed = 1e9, fields = 0, display = 0, worstGpu = 0, gpu = 0;
        double cpuMs = 0, rspMs = 0, rdpMs = 0, triangles = 0;
        int samples = 0;
        const auto startingUnderruns = StudioVideoProbe::underruns(widget);
        deadline.restart();
        while (deadline.elapsed() < seconds * 1000) {
            QApplication::processEvents();
            const auto s = StudioVideoProbe::stats(widget);
            if (s.ready) {
                minSpeed = std::min(minSpeed, s.speed);
                fields += s.viFps;
                display += s.displayFps;
                cpuMs += s.cpuMs;
                rspMs += s.rspMs;
                rdpMs += s.rdpMs;
                triangles += s.trianglesPerSec;
                const auto presentMs = StudioVideoProbe::gpuMs(widget);
                gpu += presentMs;
                worstGpu = std::max(worstGpu, presentMs);
                ++samples;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        const auto native = StudioVideoProbe::nativeFrame(widget);
        const auto pixels = nonBlack(native);
        const auto geometry = StudioVideoProbe::metalGeometry(widget);
        const auto counters = StudioVideoProbe::stats(widget).counters;
        const auto jitSteps = counters.jitSteps;
        std::cout << "Bench " << scale << "x/" << fps << "Hz/" << (smooth ? "linear" : "nearest")
                  << '/' << seconds << "s: min speed " << (samples ? minSpeed : 0)
                  << "%, VI " << (samples ? fields / samples : 0)
                  << " fields/s, display " << (samples ? display / samples : 0)
                  << " FPS, non-black " << pixels << ", RDP triangles/s " << (samples ? triangles / samples : 0)
                  << '\n'
                  << "Mean ms/guest field: CPU " << (samples ? cpuMs / samples : 0)
                  << ", RSP " << (samples ? rspMs / samples : 0)
                  << ", RDP " << (samples ? rdpMs / samples : 0)
                  << "; GPU Present ms (per command): " << (samples ? gpu / samples : 0)
                  << " average, " << worstGpu << " max\n"
                  << "Drawable " << geometry.width << 'x' << geometry.height << " @ "
                  << geometry.devicePixelRatio << "x; image " << geometry.imageWidth << 'x'
                  << geometry.imageHeight << " at " << geometry.x << ',' << geometry.y
                  << "; shared packed VI " << (StudioVideoProbe::packedMetal(widget) ? "yes" : "no") << '\n';
        std::cout << "Native JIT steps: " << jitSteps << '\n';
        std::cout << "Profile: JIT dispatch attempts " << counters.jitInvocations
                  << ", native steps " << jitSteps
                  << ", cached dispatch lookups " << counters.cacheLookups
                  << ", interpreter steps " << (counters.instructions > jitSteps
                      ? counters.instructions - jitSteps : 0) << '\n';
        const auto diagnostic = StudioVideoProbe::presentation(widget);
        const auto& layout = diagnostic.layout;
        const bool scaledEligible = diagnostic.rdp &&
            diagnostic.rdp->canPresentScaled(layout.start, layout.stride, layout.width,
                                             layout.height, layout.bytes);
        std::cout << "VI layout " << layout.width << 'x' << layout.height
                  << " start=0x" << std::hex << layout.start << std::dec
                  << " stride=" << layout.stride << " rowWidth=" << layout.rowWidth
                  << " bytes=" << layout.bytes << "; RDP fine samples="
                  << (diagnostic.rdp && diagnostic.rdp->hasFineSamples() ? "yes" : "no")
                  << ", scaled source eligible=" << (scaledEligible ? "yes" : "no") << '\n';
        if (diagnostic.rdp) {
            const auto source = diagnostic.rdp->scaledSourceInfo();
            std::cout << "RDP color image " << source.colorWidth << 'x' << source.nativeHeight
                      << " start=0x" << std::hex << source.colorAddress << std::dec
                      << " size=" << source.colorSize << " fmt=" << source.colorFormat
                      << "; native framebuffer " << source.nativeWidth << 'x' << source.nativeHeight
                      << "; bus=" << (source.busReady ? "yes" : "no") << '\n';
            for (const auto& surface : diagnostic.rdp->fineSurfaceSummaries())
                std::cout << "  fine surface 0x" << std::hex << surface.address << std::dec
                          << ' ' << surface.width << 'x' << surface.nativeHeight
                          << " size=" << surface.size << " fmt=" << surface.fmt
                          << " epoch=" << surface.epoch << '\n';
        }
        std::cout << "Internal scale " << settings.internalScale() << "×, presented source "
                  << native.width() << 'x' << native.height() << '\n';
        std::cout << "Audio underruns during sample: "
                  << StudioVideoProbe::underruns(widget) - startingUnderruns << '\n';
        std::cout << widget.diagnosticText().toStdString() << '\n';
        const auto active = widget.activePresenterName();
        const auto requestedApi = settings.graphicsApi();
        const bool backendPresented =
            requestedApi == GraphicsApi::Metal ? active == QStringLiteral("Metal") && worstGpu > 0
            : requestedApi == GraphicsApi::D3D12 ? active == QStringLiteral("Direct3D 12")
            : requestedApi == GraphicsApi::Auto
                ? active == QStringLiteral("Metal") || active.startsWith(QStringLiteral("Vulkan"))
                : active.startsWith(QStringLiteral("Vulkan"));
        const bool sourceCorrect = !synthetic || (native.size() == QSize(16, 16) && pixels == 3 &&
            native.pixelColor(0, 0) == QColor(Qt::red) && native.pixelColor(1, 0) == QColor(Qt::green));
        return samples && pixels && backendPresented && sourceCorrect ? 0 : 1;
    }
    if (nativeProbe) {
        if (rom.isEmpty() || QApplication::platformName() != QStringLiteral("cocoa")) return 2;
        settings.setFlashRamEnabled(argc > 5 && std::strcmp(argv[5], "--flashram") == 0);
        settings.setRenderFps(120);
        settings.setFrameInterpolation(true);
        widget.applyVideoSettings();
        widget.resize(1440, 900);
        widget.show();
        if (!widget.loadRom(rom)) return 1;
        widget.setDiagnosticsVisible(true);
        QElapsedTimer deadline;
        deadline.start();
        while (deadline.elapsed() < std::max(2500, sampleAfterMs)) {
            QApplication::processEvents();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        const auto& frame = StudioVideoProbe::nativeFrame(widget);
        const auto pixels = nonBlack(frame);
        const auto syncs = StudioVideoProbe::fullSyncs(widget);
        std::cout << "Native Fit Window: " << frame.width() << 'x' << frame.height()
                  << ", non-black pixels: " << pixels << ", full syncs: " << syncs << '\n'
                  << widget.diagnosticText().toStdString() << '\n';
        return pixels && syncs >= minSyncs && StudioVideoProbe::gpuMs(widget) > 0 ? 0 : 1;
    }
    if (rom.isEmpty()) {
        StudioVideoProbe::testSaveReload(widget, dir.filePath("controller-1.mpk"));
        ControllerManager manager;
        StudioVideoProbe::testPakPreservation(widget, manager);
        StudioVideoProbe::testCartridgeSaveMedia(widget, settings);
        StudioVideoProbe::testAutoSaveCreation(widget, settings);
    }
    widget.resize(320, 320);
    widget.show();
    if (!widget.loadRom(rom.isEmpty() ? writeVideoRom(dir) : rom)) return 1;
    if (QApplication::platformName() == QStringLiteral("offscreen"))
        assert(!StudioVideoProbe::bootActive(widget));
    widget.setDiagnosticsVisible(true);
    QPixmap cover(16, 16);
    cover.fill(Qt::magenta);
    widget.setCover(cover); // Downloaded art must not cover active video.
    if (!waitForVideo(widget, 0, rom.isEmpty(), minSyncs)) return 1;
    for (const auto [scale, fps] : std::array{std::pair{2, 60}, std::pair{4, 120}, std::pair{0, 0}}) {
        const auto interpolateAfter = fps && rom.isEmpty() ? StudioVideoProbe::timestamp(widget) : -1;
        settings.setViewportScale(scale);
        settings.setFrameInterpolation(fps != 0);
        settings.setRenderFps(fps);
        widget.applyVideoSettings();
        if (!waitForVideo(widget, scale, rom.isEmpty(), minSyncs, interpolateAfter)) return 1;
    }
    QElapsedTimer timer;
    timer.start();
    n64forge::CoreStats stats;
    do {
        QApplication::processEvents();
        stats = StudioVideoProbe::stats(widget);
        if (stats.ready && stats.viFps > 0 && timer.elapsed() >= sampleAfterMs) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    } while (timer.elapsed() < 10'000);
    assert(stats.ready && stats.speed > 0 && stats.mips > 0 && stats.viFps > 0);
    assert(stats.hasCacheRate && stats.counters.cacheHits > 0 && stats.counters.sampledSteps > 0);
    const auto text = widget.diagnosticText();
    assert(text.contains("MIPS") && text.contains("triangles/s") && !text.contains("%25"));
    std::cout << text.toStdString() << '\n';
    widget.setPerformanceHud(true);
    QApplication::processEvents();
    assert(!widget.grab().isNull());
    if (rom.isEmpty()) testHostCadenceWithoutGuest(widget, settings);
    return 0;
}
