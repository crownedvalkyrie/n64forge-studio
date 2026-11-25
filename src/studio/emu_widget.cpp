#include "n64forge/studio/emu_widget.hpp"
#include "n64forge/studio/guest_pacer.hpp"
#include "n64forge/studio/settings.hpp"
#include "n64forge/studio/audio_output.hpp"
#include "n64forge/studio/controller_manager.hpp"
#include "n64forge/core/emulator.hpp"
#include "n64forge/core/r4300_decode.hpp"
#include "n64forge/studio/art_repository.hpp"
#include "n64forge/studio/game_database.hpp"

#include <QPaintEvent>
#include <QApplication>
#include <QImage>
#include <QKeyEvent>
#include <QFontDatabase>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QStandardPaths>
#include <QLinearGradient>
#include <QPainter>
#include <QElapsedTimer>
#include <QTimer>
#include <QMutex>
#include <QMutexLocker>
#include <QResizeEvent>
#include <QScreen>
#include <QWindow>
#include <QStyle>
#include <QStyleOption>
#include <QDebug>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <span>
#include <vector>
#include <array>
#include <atomic>
#include <thread>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <functional>
#include <utility>
#include <sstream>
#include <iomanip>
#include <cstdint>

#ifdef N64FORGE_HAS_SDL3
#include "n64forge/input/sdl_input_manager.hpp"
#endif

namespace n64forge::studio {

constexpr auto kFrameTimerInterval = 16; // ~60 FPS timer
constexpr auto kUiUpdateInterval = 1000; // 1 second UI updates

namespace {
enum class SaveFileState { Missing, Valid, Unusable };

SaveFileState readSaveFile(const QString& path, qsizetype expectedSize, QByteArray& bytes) {
    const QFileInfo info(path);
    if (info.isSymLink()) return SaveFileState::Unusable; // Includes dangling links.
    if (!info.exists()) return SaveFileState::Missing;
    QFile file(path);
    if (!info.isFile() || !file.open(QIODevice::ReadOnly) || file.size() != expectedSize)
        return SaveFileState::Unusable;
    bytes = file.read(expectedSize + 1);
    return file.error() == QFileDevice::NoError && bytes.size() == expectedSize
        ? SaveFileState::Valid : SaveFileState::Unusable;
}
} // namespace

void EmuWidget::clearFrames() {
    if (frameMailbox_) frameMailbox_->reset();
    previousPacked_.reset();
    currentPacked_.reset();
    previousFrame_ = QImage();
    currentFrame_ = QImage();
    displayedFrame_ = QImage();
    presentedFrame_ = QImage();
    hasActiveVideo_ = false;
    currentTimestamp_ = 0;
    sourceInterval_ = 0;
    interpolationEstimateKey_ = 0;
    softwareEndpointShown_ = true;
    softwareRevision_ = 0;
}

void EmuWidget::startWorker() {
    stopWorker();
    clearFrames();

    emulator_ = std::make_unique<n64forge::Emulator>();
    {
        std::lock_guard lock(statsMutex_);
        saveStatus_.clear();
    }
    emulator_->rdp().setThreadCount(settings_ ? settings_->rdpThreads() : 0);
    emulator_->setInternalResolution(settings_ ? settings_->internalScale() : 1);
    emulator_->setTimingEnabled(true);
    const auto requested = settings_ ? settings_->cpuMode() : CpuExecutionMode::Cached;
    activeCpuMode_ = emulator_->setCpuExecutionMode(requested);
    if (activeCpuMode_ != requested)
        qWarning() << "ARM64 JIT unavailable; using cached interpreter";

    if (!emulator_->loadCartridge(romPath_.toStdString())) {
        emulator_.reset();
        running_.store(false, std::memory_order_relaxed);
        paused_.store(false, std::memory_order_relaxed);
        update();
        return;
    }

    prepareBootSequence();
    loadSaveMedia();
    publishControllers();
    // The scanout window is unknown until the guest configures VI, so reserve
    // the documented hardware maximum. Frames are still published at their
    // native dimensions; only the backing stores are fixed-size.
    const auto platform = QGuiApplication::platformName();
    if (platform == QStringLiteral("cocoa")) {
        constexpr auto capacity = Emulator::MaxScanoutPixels + 1024 * video::PackedFrameAlignment / sizeof(u32);
        frameMailbox_ = std::make_shared<video::FrameMailbox>(capacity, video::createPackedFrameBacking);
    } else {
        frameMailbox_ = std::make_shared<video::FrameMailbox>(Emulator::MaxScanoutPixels);
    }
    if (platform != QStringLiteral("offscreen") && platform != QStringLiteral("minimal")) {
        audio_ = std::make_unique<AudioOutput>();
        if (!audio_->start()) audio_.reset();
    }

    workerStopRequested_.store(false, std::memory_order_release);
    worker_ = std::jthread([this](std::stop_token stoken) {
        using Clock = std::chrono::steady_clock;
        Emulator::ScanoutHistory history;
        CoreStatsSampler sampler;
        bool sampling = false;
        u64 sourceFrame = 0;
        GuestPacer pacer(Clock::now());
        bool wasPaused = false;
        auto nextSave = Clock::now() + std::chrono::seconds(2);
        const auto stopRequested = [&] {
            return stoken.stop_requested() || workerStopRequested_.load(std::memory_order_acquire);
        };

        while (!stopRequested()) {
            if (paused_.load(std::memory_order_relaxed)) {
                if (!wasPaused) saveMedia();
                sampling = false;
                wasPaused = true;
                std::unique_lock<std::mutex> waitLock(workerWaitMutex_);
                workerWaitCv_.wait_for(waitLock, std::chrono::milliseconds(2), stopRequested);
                continue;
            }
            if (wasPaused) {
                pacer = GuestPacer(Clock::now());
                wasPaused = false;
            }

            if (!emulator_) break;

            const auto cycles = emulator_->runVideoFrame(false);
            if (Clock::now() >= nextSave) {
                saveMedia();
                nextSave = Clock::now() + std::chrono::seconds(2);
            }
            u8 motors = 0;
            for (unsigned port = 0; port < 4; ++port)
                if (emulator_->bus().hw().si().paks[port].rumbling()) motors |= 1u << port;
            motorMask_.store(motors, std::memory_order_relaxed);
            if (audio_) {
                std::size_t frames;
                const auto* samples = emulator_->audioSamples(frames);
                audio_->writeSamples(samples, frames);
            }
            if (bootTelemetryRequested_.load(std::memory_order_relaxed)) {
                bootRspLaunches_.store(emulator_->rsp().taskLaunches(), std::memory_order_relaxed);
                bootRdpSyncs_.store(emulator_->rdp().stats().fullSyncs, std::memory_order_relaxed);
            }

            if (statsRequested_.load(std::memory_order_relaxed)) {
                const auto now = Clock::now();
                const auto nowNs = static_cast<u64>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        now.time_since_epoch()).count());
                auto c = emulator_->counters();
                // Host presentation counters are injected here; they never
                // advance the guest or its audio clock.
                c.paints = paintedFrames_.load(std::memory_order_relaxed);
                c.presentationTicks = presentationTicks_.load(std::memory_order_relaxed);
                if (!sampling) {
                    sampler.reset(nowNs, c);
                    sampling = true;
                } else if (sampler.update(nowNs, c)) {
                    const auto snapshot = sampler.snapshot();
                    std::lock_guard lock(statsMutex_);
                    stats_ = snapshot;
                }
            } else {
                sampling = false;
            }

            if (auto* slot = frameMailbox_->acquire()) {
                const bool changed = emulator_->scanoutForPresentation(
                    frameMailbox_->storage(*slot), frameMailbox_->lastPixels(), history,
                    frameMailbox_->packed() ? video::PackedFrameAlignment : 0);
                if (changed) {
                    slot->width = emulator_->framebufferWidth();
                    slot->height = emulator_->framebufferHeight();
                    slot->pitch = emulator_->scanoutPitch();
                    slot->bytesPerPixel = emulator_->scanoutBytesPerPixel();
                    slot->timestamp = ++sourceFrame;
                    frameMailbox_->publish(*slot);
                } else {
                    slot->release();
                }
            }

            // Respect guest cycles even before VI is configured or with PAL.
            // The audio callback never drives the guest clock.
            const auto now = Clock::now();
            const auto nextField = pacer.advance(cycles, now);
            if (nextField > now) {
                std::unique_lock<std::mutex> waitLock(workerWaitMutex_);
                workerWaitCv_.wait_until(waitLock, nextField, stopRequested);
            }
        }
    });

    running_.store(true, std::memory_order_relaxed);
    paused_.store(false, std::memory_order_relaxed);
    frameClock_.start();
    nextPresent_ = 0;
    update();
    schedulePresentation();
}

void EmuWidget::stopWorker() {
    if (controllerManager_) controllerManager_->syncRumble(0);
    if (worker_.joinable()) {
        workerStopRequested_.store(true, std::memory_order_release);
        worker_.request_stop();
        // Wake the pacer wait immediately; join must not wait for a full field.
        workerWaitCv_.notify_all();
        worker_.join();
    }
    saveMedia();
    motorMask_.store(0, std::memory_order_relaxed);
    emulator_.reset();
    audio_.reset();
    viewportPresenter_.reset();
    delete viewportSurface_;
    viewportSurface_ = nullptr;
    viewportAttempted_ = false;
    viewportRevision_ = 0;
    viewportGeometry_ = {};
    if (frameMailbox_) frameMailbox_->reset();
    frameMailbox_.reset();
    bootSequence_.start(false);
    bootTelemetryRequested_.store(false, std::memory_order_relaxed);
    bootOpacity_ = 0;
    std::lock_guard lock(statsMutex_);
    stats_ = {};
}

void EmuWidget::resizeEvent(QResizeEvent* event) {
    Q_UNUSED(event)
    if (isPaused() && viewportPresenter_) frameTimer_->start(0);
    update();
}

bool EmuWidget::event(QEvent* event) {
    const bool handled = QWidget::event(event);
#if QT_VERSION >= QT_VERSION_CHECK(6, 6, 0)
    if (event->type() == QEvent::DevicePixelRatioChange && isPaused() && viewportPresenter_)
        frameTimer_->start(0);
#endif
    return handled;
}

void EmuWidget::publishControllers() {
    if (!emulator_ || !controllerManager_) return;
    // SDL and Qt stay on the UI thread; SI reads the packed atomic snapshot.
    controllerManager_->pollGamepads();
    controllerManager_->syncRumble(isPaused() ? 0 : motorMask_.load(std::memory_order_relaxed));
    for (int port = 0; port < 4; ++port)
        emulator_->bus().hw().setController(port, controllerManager_->combinedState(port),
            port == 0 || controllerManager_->connected(port) || !controllerManager_->mappings(port).empty());
}

void EmuWidget::loadSaveMedia(const QString& directory) {
    pakPaths_.fill({});
    savePath_.clear();
    autoEepromPath_.clear();
    autoSramPath_.clear();
    autoFlashPath_.clear();
    autoEepromLoaded4K_ = false;
    saveMedia_ = settings_ ? settings_->saveMedia() : SaveMedia::None;
    auto dir = directory;
    if (dir.isNull()) {
        dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        if (!dir.isEmpty()) dir += QStringLiteral("/saves");
    }
    auto& bus = emulator_->bus();

    const auto pathFor = [&](const QString& extension) {
        const auto& header = emulator_->cartridge().header();
        return dir + QStringLiteral("/%1-%2.%3")
            .arg(header.crc1, 8, 16, QLatin1Char('0'))
            .arg(header.crc2, 8, 16, QLatin1Char('0'))
            .arg(extension);
    };
    const auto loadFile = [&](const QString& path, qsizetype expectedSize, auto& device) {
        if (dir.isEmpty()) return false;
        QByteArray bytes;
        const auto state = readSaveFile(path, expectedSize, bytes);
        if (state == SaveFileState::Missing) return true;
        if (state == SaveFileState::Valid &&
            device.load({reinterpret_cast<const u8*>(bytes.constData()),
                         static_cast<std::size_t>(bytes.size())})) return true;
        qWarning() << "Preserving unreadable or wrong-size save file:" << path;
        return false;
    };

    if (saveMedia_ == SaveMedia::Auto) {
        const auto flashPath = pathFor(QStringLiteral("fla"));
        const auto sramPath = pathFor(QStringLiteral("sra"));
        const auto eepPath = pathFor(QStringLiteral("eep"));
        QByteArray existing;
        const bool flashImage = !dir.isEmpty() &&
            readSaveFile(flashPath, FlashRam::Size, existing) == SaveFileState::Valid;
        const bool sramImage = !dir.isEmpty() &&
            readSaveFile(sramPath, Sram::Size, existing) == SaveFileState::Valid;
        const bool wideEeprom = !dir.isEmpty() &&
            readSaveFile(eepPath, Eeprom::Size16K, existing) == SaveFileState::Valid;
        const bool narrowEeprom = !wideEeprom && !dir.isEmpty() &&
            readSaveFile(eepPath, Eeprom::Size4K, existing) == SaveFileState::Valid;
        if (static_cast<int>(flashImage) + static_cast<int>(sramImage) +
            static_cast<int>(wideEeprom || narrowEeprom) > 1) {
            qWarning() << "Ambiguous cartridge saves; leaving existing files untouched";
            bus.eeprom().attach(n64forge::Eeprom::Type::None);
            bus.setPiSaveMedia(n64forge::Bus::PiSaveMedia::None);
            // pakPaths_ below are still loaded independently of cartridge saves.
        } else {
            bus.configureAutoPiSave(flashImage);
            // Record the destination even when no file exists yet: detection
            // comes from guest bus activity, and the first dirty flush creates
            // only the save that the guest actually used.
            const auto loadAuto = [&](const QString& path, qsizetype expectedSize, auto& device) {
                if (dir.isEmpty()) return QString{};
                QByteArray bytes;
                const auto state = readSaveFile(path, expectedSize, bytes);
                if (state == SaveFileState::Unusable) {
                    qWarning() << "Preserving unreadable or wrong-size save file:" << path;
                    return QString{};
                }
                if (state == SaveFileState::Valid &&
                    !device.load({reinterpret_cast<const u8*>(bytes.constData()),
                                  static_cast<std::size_t>(bytes.size())})) {
                    qWarning() << "Could not load save file:" << path;
                    return QString{};
                }
                return path;
            };
            autoFlashPath_ = loadAuto(flashPath, FlashRam::Size, bus.flash());
            autoSramPath_ = loadAuto(sramPath, Sram::Size, bus.sram());
            auto& eeprom = bus.eeprom();
            eeprom.attach(wideEeprom ? n64forge::Eeprom::Type::Eeprom16K
                                     : n64forge::Eeprom::Type::Eeprom4K);
            eeprom.enableAutoExpansion();
            autoEepromPath_ = loadAuto(eepPath, eeprom.size(), eeprom);
            autoEepromLoaded4K_ = narrowEeprom;
        }
    }

    switch (saveMedia_) {
        case SaveMedia::FlashRam: {
            auto& flash = bus.flash();
            flash.attach(true);
            bus.setPiSaveMedia(n64forge::Bus::PiSaveMedia::FlashRam);
            const auto path = pathFor(QStringLiteral("fla"));
            if (loadFile(path, FlashRam::Size, flash)) savePath_ = path;
            break;
        }
        case SaveMedia::Sram: {
            auto& sram = bus.sram();
            sram.attach(true);
            bus.setPiSaveMedia(n64forge::Bus::PiSaveMedia::Sram);
            const auto path = pathFor(QStringLiteral("sra"));
            if (loadFile(path, Sram::Size, sram)) savePath_ = path;
            break;
        }
        case SaveMedia::Eeprom4K:
        case SaveMedia::Eeprom16K: {
            auto& eeprom = bus.eeprom();
            eeprom.attach(saveMedia_ == SaveMedia::Eeprom4K
                              ? n64forge::Eeprom::Type::Eeprom4K
                              : n64forge::Eeprom::Type::Eeprom16K);
            const auto path = pathFor(QStringLiteral("eep"));
            if (loadFile(path, eeprom.size(), eeprom)) savePath_ = path;
            break;
        }
        case SaveMedia::Auto:
        case SaveMedia::None: break;
    }

    if (!controllerManager_) return;
    for (int port = 0; port < 4; ++port) {
        auto& pak = emulator_->bus().hw().si().paks[port];
        const auto type = controllerManager_->pakType(port);
        pak.attach(type);
        if (type != PakType::Controller || dir.isEmpty()) continue;
        const auto path = dir + QStringLiteral("/controller-%1.mpk").arg(port + 1);
        if (loadFile(path, ControllerPak::Size, pak)) pakPaths_[port] = path;
    }
}

void EmuWidget::saveMedia() {
    if (!emulator_) return;
    QStringList status;
    const auto write = [](const QString& path, std::span<const u8> bytes, bool expand = false) {
        QSaveFile file(path);
        file.setDirectWriteFallback(false);
        QByteArray existing;
        const auto expected = static_cast<qsizetype>(bytes.size());
        // Check the existing file before QSaveFile opens: commit() replaces the
        // destination, so an unusable file must be left untouched.
        const bool safe = readSaveFile(path, expected, existing) != SaveFileState::Unusable ||
            (expand && readSaveFile(path, Eeprom::Size4K, existing) == SaveFileState::Valid);
        if (!safe || !QDir().mkpath(QFileInfo(path).absolutePath()) ||
            !file.open(QIODevice::WriteOnly) ||
            file.write(reinterpret_cast<const char*>(bytes.data()), expected) != expected ||
            !file.commit()) {
            qWarning() << "Could not write save media:" << path;
            return false;
        }
        return true;
    };
    auto& bus = emulator_->bus();
    const auto persist = [&](const QString& name, const QString& path, auto& device,
                             std::span<const u8> bytes, bool active = true, bool expand = false) {
        if (!active || path.isEmpty()) return false;
        const auto label = QStringLiteral("%1 — %2").arg(name, path);
        if (!device.dirty()) {
            status << label;
            return true;
        }
        if (!write(path, bytes, expand)) {
            status << label + tr(" (write failed)");
            return false;
        }
        device.markSaved();
        status << label;
        return true;
    };
    if (saveMedia_ == SaveMedia::Auto) {
        auto& eep = bus.eeprom();
        if (persist(eep.is16K() ? tr("EEPROM 16K") : tr("EEPROM 4K"), autoEepromPath_,
                    eep, {eep.image().data(), eep.size()}, eep.accessed() || eep.dirty(),
                    autoEepromLoaded4K_ && eep.is16K()))
            autoEepromLoaded4K_ = !eep.is16K();
        const bool sramActive = bus.piSaveMedia() == Bus::PiSaveMedia::Sram || bus.sram().dirty();
        persist(tr("SRAM 32 KiB"), autoSramPath_, bus.sram(), bus.sram().image(), sramActive);
        const bool flashActive = bus.piSaveMedia() == Bus::PiSaveMedia::FlashRam || bus.flash().dirty();
        persist(tr("FlashRAM 128 KiB"), autoFlashPath_, bus.flash(), bus.flash().image(), flashActive);
    }
    if (!savePath_.isEmpty()) {
        switch (saveMedia_) {
            case SaveMedia::FlashRam:
                persist(tr("FlashRAM 128 KiB"), savePath_, bus.flash(), bus.flash().image());
                break;
            case SaveMedia::Sram:
                persist(tr("SRAM 32 KiB"), savePath_, bus.sram(), bus.sram().image());
                break;
            case SaveMedia::Eeprom4K:
            case SaveMedia::Eeprom16K: {
                auto& eeprom = bus.eeprom();
                persist(eeprom.is16K() ? tr("EEPROM 16K") : tr("EEPROM 4K"), savePath_, eeprom,
                        {eeprom.image().data(), eeprom.size()});
                break;
            }
            case SaveMedia::Auto:
            case SaveMedia::None: break;
        }
    }
    for (unsigned port = 0; port < pakPaths_.size(); ++port) {
        const auto& path = pakPaths_[port];
        auto& pak = bus.hw().si().paks[port];
        persist(tr("Controller Pak %1").arg(port + 1), path, pak, pak.image());
    }
    std::lock_guard lock(statsMutex_);
    saveStatus_ = status.join(QLatin1Char('\n'));
}

void EmuWidget::keyPressEvent(QKeyEvent* event) {
    if (isRunning() && controllerManager_ &&
        !(event->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier))) {
        if (event->isAutoRepeat()) return;
        if (controllerManager_->handleKeyPress(event->key(), true)) {
            event->accept();
            return;
        }
    }
    QWidget::keyPressEvent(event);
}

void EmuWidget::keyReleaseEvent(QKeyEvent* event) {
    if (controllerManager_ && !event->isAutoRepeat() &&
        controllerManager_->handleKeyPress(event->key(), false)) {
        event->accept();
        return;
    }
    QWidget::keyReleaseEvent(event);
}

void EmuWidget::focusOutEvent(QFocusEvent* event) {
    if (controllerManager_) controllerManager_->resetAll();
    QWidget::focusOutEvent(event);
}

void EmuWidget::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event)
    QElapsedTimer timer;
    timer.start();

    QPainter painter(this);
    painter.fillRect(rect(), Qt::black);

    const QImage& frame = presentedFrame_;
    if (viewportSurface_ && viewportSurface_->isVisible()) return;

    if (frame.isNull()) {
        if (!hasActiveVideo_) {
            if (!currentCover_.isNull()) painter.drawPixmap(rect(), currentCover_);
            painter.setPen(Qt::white);
            painter.drawText(rect(), Qt::AlignCenter,
                running_ ? tr("No video output yet") : tr("Load a ROM to start emulation"));
        }
        paintBootSequence(painter);
        paintPerformanceHud(painter);
        return;
    }

    painter.setRenderHint(QPainter::SmoothPixmapTransform, smoothScaling_);
    painter.drawImage(videoRect(size(), frame.size(), outputScale_, ultrawide_), frame);
    if (frame.cacheKey() != lastPaintedKey_) {
        lastPaintedKey_ = frame.cacheKey();
        if (statsRequested_.load(std::memory_order_relaxed))
            paintedFrames_.fetch_add(1, std::memory_order_relaxed);
    }

    paintBootSequence(painter);
    paintPerformanceHud(painter);

    // Profile paint overhead
    if (performanceHud_) {
        qint64 elapsed = timer.elapsed();
        if (elapsed > 5) {
            static qint64 totalPaintTime = 0;
            static int paintCount = 0;
            totalPaintTime += elapsed;
            paintCount++;
            if (paintCount % 30 == 0) {
                qDebug() << "Avg paint time:" << (totalPaintTime / paintCount) << "ms";
            }
        }
    }
}

bool EmuWidget::bootSequenceEnabled() const {
    const auto platform = QGuiApplication::platformName();
    return platform != QStringLiteral("offscreen") && platform != QStringLiteral("minimal") &&
           settings_ && settings_->bootSequenceEnabled();
}

void EmuWidget::prepareBootSequence() {
    bootSequence_.start(bootSequenceEnabled());
    bootLines_.clear();
    bootOpacity_ = bootSequence_.advance(0);
    bootRspLaunches_.store(0, std::memory_order_relaxed);
    bootRdpSyncs_.store(0, std::memory_order_relaxed);
    bootTelemetryRequested_.store(bootSequence_.active(), std::memory_order_relaxed);
    if (!bootSequence_.active()) return;

    // Read the handoff before ownership of the emulator moves to the worker.
    const auto hex = [](u32 n, int digits = 8) {
        return QStringLiteral("%1").arg(n, digits, 16, QLatin1Char('0')).toUpper();
    };
    const auto& cart = emulator_->cartridge();
    const auto& header = cart.header();
    const auto& bus = emulator_->bus();
    const u32 entry = static_cast<u32>(emulator_->cpu().pc());
    bootLines_ << tr("ROM   %1").arg(QString::fromStdString(header.name).simplified())
               << tr("CART  %1 bytes / %2").arg(cart.size()).arg(QString::fromStdString(cart.formatName()))
               << tr("CRC   %1  %2").arg(hex(header.crc1), hex(header.crc2))
               << tr("CPU   header %1 / entry %2").arg(hex(header.bootPc), hex(entry))
               << tr("PIF   post-IPL handoff; authentication not simulated")
               << tr("CIC   profile seed 0x%1")
                      .arg(hex(bus.hw().si().cic6105 ? 0x91 : static_cast<u32>(emulator_->cpu().gpr()[22]), 2))
               << tr("RSP   SP_STATUS 0x%1").arg(hex(bus.read32(0x04040010)))
               << tr("RDP   DPC_STATUS 0x%1").arg(hex(bus.read32(0x0410000C)));
    const u32 physical = entry & 0x1FFFFFFF;
    const auto bytes = std::min({std::size_t{256}, cart.size() - 0x1000,
                                 std::size_t{Memory::RdramSize - physical}});
    for (u32 offset = 0; offset + 4 <= bytes; offset += 4) {
        const u32 word = bus.rdram().loadRdram<u32>(physical + offset);
        bootLines_ << QStringLiteral("%1  %2  %3").arg(hex(entry + offset), hex(word),
            QString::fromStdString(disassemble(decodeR4300(word), entry + offset)));
    }
}

void EmuWidget::paintBootSequence(QPainter& painter) const {
    if (bootOpacity_ <= 0 || !frameClock_.isValid() || bootLines_.isEmpty()) return;
    painter.save();
    painter.setOpacity(bootOpacity_);
    QLinearGradient background(0, 0, 0, height());
    background.setColorAt(0, QColor("#201C33"));
    background.setColorAt(1, QColor("#0D1017"));
    painter.fillRect(rect(), background);
    painter.setPen(QColor(145, 120, 229, 20));
    for (int y = 2; y < height(); y += 4) painter.drawLine(0, y, width(), y);
    auto font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    font.setPixelSize(std::clamp(width() / 60, 11, 18));
    painter.setFont(font);
    const int lineHeight = painter.fontMetrics().height() + 4;
    constexpr int margin = 16;
    const int top = margin + 2 * lineHeight;
    const auto elapsed = frameClock_.elapsed();
    const double phase = static_cast<double>(elapsed) / 46.0;
    const int cursor = static_cast<int>(phase) % static_cast<int>(bootLines_.size());
    const double drift = (phase - std::floor(phase)) * lineHeight;
    painter.setPen(QColor("#C4B5FD"));
    painter.drawText(margin, margin + lineHeight, tr("N64FORGE  /  HARDWARE SELF-CHECK"));
    painter.setClipRect(QRect(margin, top, std::max(0, width() - 2 * margin),
                              std::max(0, height() - top - 2 * lineHeight)));
    for (int i = 0; i * lineHeight < height() - top; ++i) {
        const int index = (cursor + static_cast<int>(bootLines_.size()) - i %
                           static_cast<int>(bootLines_.size())) % static_cast<int>(bootLines_.size());
        const int light = std::max(60, 240 - i * 13);
        painter.setPen(QColor(light / 2, light, light * 3 / 4));
        painter.drawText(QPointF(margin, top + i * lineHeight + drift), bootLines_[index]);
    }
    painter.setClipping(false);
    painter.setPen(bootSequence_.waitingForVideo() ? QColor("#F59E0B") : QColor("#6EE7B7"));
    painter.drawText(margin, height() - margin,
        tr("SP %1 launches  |  DP %2 syncs  |  %3")
            .arg(bootRspLaunches_.load(std::memory_order_relaxed))
            .arg(bootRdpSyncs_.load(std::memory_order_relaxed))
            .arg(bootSequence_.waitingForVideo() ? tr("Waiting for VI") : tr("VI active")));
    painter.restore();
}

void EmuWidget::paintPerformanceHud(QPainter& painter) {
    if (!performanceHud_) return;
    painter.save();
    auto mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    mono.setPixelSize(12);
    painter.setFont(mono);
    const auto text = QRect(20, 25, std::max(1, width() - 40), std::max(1, height() - 42));
    constexpr int flags = Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap;
    const auto bounds = painter.boundingRect(text, flags, hudText_).intersected(text);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setBrush(QColor(13, 16, 23, 235));
    const auto stats = performanceSnapshot();
    painter.setPen(stats.ready && stats.speed >= 100.0 ? QColor("#10B981") : QColor("#8D79D6"));
    painter.drawRoundedRect(bounds.adjusted(-8, -8, 8, 8), 6, 6);
    painter.setPen(QColor("#DDE7FA"));
    painter.setClipRect(text);
    painter.drawText(text, flags, hudText_);
    painter.restore();
}

QString EmuWidget::activePresenterName() const {
    if (!hasGame()) return tr("No game loaded");
    // The native child is hidden whenever the Emulator tab is not frontmost,
    // but the game can still be using the GPU presenter. Report the real
    // presenter state, not the Qt widget's current visibility.
    if (!viewportPresenter_ || bootSequence_.active()) return tr("Software");
    if (!viewportRevision_ || !viewportPresenter_->hasPresentedFrame())
        return tr("Waiting for native frame");
    switch (viewportPresenter_->api()) {
        case GraphicsApi::Metal: return tr("Metal");
        case GraphicsApi::MoltenVK: return tr("Vulkan (MoltenVK)");
        case GraphicsApi::KosmicKrisp: return tr("Vulkan (KosmicKrisp)");
        case GraphicsApi::D3D12: return tr("Direct3D 12");
        case GraphicsApi::Vulkan: return tr("Vulkan");
        case GraphicsApi::Auto: return tr("Auto");
        case GraphicsApi::Software: return tr("Software");
    }
    return tr("Software");
}

QString EmuWidget::activePresenterDevice() const {
    return viewportPresenter_ && viewportRevision_ && viewportPresenter_->hasPresentedFrame()
        ? QString::fromStdString(viewportPresenter_->deviceDescription()) : QString{};
}

QString EmuWidget::diagnosticText() const {
    QString saveText;
    {
        std::lock_guard lock(statsMutex_);
        saveText = saveStatus_;
    }
    const auto withSaves = [&](QString text) {
        return saveText.isEmpty() ? text
                                  : text + QStringLiteral("\nSave media:\n") + saveText;
    };
    if (!running_.load(std::memory_order_relaxed))
        return withSaves(tr("Load a ROM to collect performance stats."));
    if (paused_.load(std::memory_order_relaxed))
        return withSaves(tr("Emulation paused."));
    const auto s = performanceSnapshot();
    if (!s.ready) return withSaves(tr("Waiting for performance sample…"));
    const auto cache = s.hasCacheRate ? QString("%1%").arg(s.cacheHit, 0, 'f', 1) : QString("N/A (inactive)");
    const auto cpu = activeCpuMode_ == CpuExecutionMode::Jit ? tr("ARM64 JIT")
        : activeCpuMode_ == CpuExecutionMode::Cached ? tr("CACHED CPU") : tr("INTERPRETER");
    const auto presenter = activePresenterName().toUpper();
    const auto header = tr("RCP TELEMETRY  /  %1  /  %2  /  %3 MHz nominal\n")
        .arg(cpu, presenter).arg(HwRegisters::CpuClockHz / 1e6, 0, 'f', 2);
    const QString telemetry = header + QString("Speed %1%  ·  CPU %2 MIPS / %3 MHz  ·  Block-cache hits %4\n"
                   "VI %5 fields/s  ·  Display %6 FPS  ·  UI timer %7 Hz (target %8)\n"
                   "RDP %9x%10  ·  %11 triangles/s\n"
                   "Field %12 ms: CPU ~%13  RSP ~%14  RDP %15  sleep %16  other %17")
        .arg(s.speed, 0, 'f', 1)
        .arg(s.mips, 0, 'f', 1)
        .arg(s.mhz, 0, 'f', 1)
        .arg(cache)
        .arg(s.viFps, 0, 'f', 1)
        .arg(s.displayFps, 0, 'f', 1)
        .arg(s.presentationHz, 0, 'f', 1)
        .arg(renderFps_)
        .arg(s.counters.rdpWidth)
        .arg(s.counters.rdpHeight)
        .arg(s.trianglesPerSec, 0, 'f', 1)
        .arg(s.frameMs, 0, 'f', 2)
        .arg(s.cpuMs, 0, 'f', 1)
        .arg(s.rspMs, 0, 'f', 1)
        .arg(s.rdpMs, 0, 'f', 1)
        .arg(s.sleepMs, 0, 'f', 1)
        .arg(s.otherMs, 0, 'f', 1)
        + (audio_ ? tr("\nAudio %1: %2 Hz DAC, %3 queued, %4 non-silent frames sent, "
                       "%5 pushed @ peak %6, %7 underruns")
                        .arg(QString::fromLatin1(audio_->backend())).arg(s.counters.dacHz, 0, 'f', 1)
                        .arg(audio_->fifoStats().fillFrames).arg(audio_->nonSilentFrames())
                        .arg(audio_->writtenFrames()).arg(audio_->peakSample())
                        .arg(audio_->fifoStats().underruns)
                  : tr("\nAudio output unavailable"))
        + (viewportPresenter_ && viewportPresenter_->api() == GraphicsApi::Metal &&
           viewportPresenter_->hasPresentedFrame() ? tr("\nMetal viewport: %1 ms GPU command time")
                                 .arg(viewportPresenter_->gpuMilliseconds(), 0, 'f', 3) : QString{});
    return withSaves(telemetry);
}

CoreStats EmuWidget::performanceSnapshot() const {
    std::lock_guard lock(statsMutex_);
    return stats_;
}

bool EmuWidget::hasGame() const noexcept {
    return !romPath_.isEmpty();
}

void EmuWidget::updateStatsInterest() {
    const bool requested = performanceHud_ || diagnosticsVisible_;
    statsRequested_.store(requested, std::memory_order_relaxed);
    if (!requested) {
        std::lock_guard lock(statsMutex_);
        stats_ = {};
    }
}

void EmuWidget::schedulePresentation() {
    if (!running_.load(std::memory_order_relaxed) || paused_.load(std::memory_order_relaxed)) {
        frameTimer_->stop();
        return;
    }
    if (!frameClock_.isValid()) frameClock_.start();

    const qint64 now = frameClock_.nsecsElapsed();
    nextPresent_ = nextPresentationDeadline(now, nextPresent_, renderFps_);
    const qint64 delayNs = std::max<qint64>(1, nextPresent_ - now);
    const qint64 delayMs = std::min<qint64>((delayNs + 999'999) / 1'000'000, 60'000);
    frameTimer_->start(static_cast<int>(delayMs));
}

qint64 EmuWidget::nextPresentationDeadline(qint64 now, qint64 previous, int fps) noexcept {
    const qint64 period = 1'000'000'000LL / std::clamp(fps, 1, 1000);
    return previous <= 0 || now >= previous + period ? now + period : previous + period;
}

void EmuWidget::stop() {
    stopWorker();
    running_.store(false, std::memory_order_relaxed);
    paused_.store(false, std::memory_order_relaxed);
    update();
}

void EmuWidget::pause() {
    paused_.store(true, std::memory_order_relaxed);
    if (controllerManager_) controllerManager_->syncRumble(0);
}

void EmuWidget::reset() {
    if (!romPath_.isEmpty()) startWorker();
}

void EmuWidget::start() {
    running_.store(true, std::memory_order_relaxed);
    paused_.store(false, std::memory_order_relaxed);
    schedulePresentation();
}

void EmuWidget::resume() {
    paused_.store(false, std::memory_order_relaxed);
    schedulePresentation();
}

bool EmuWidget::loadRom(const QString& path) {
    romPath_ = path;
    startWorker();
    return emulator_ != nullptr;
}

void EmuWidget::onFrame() {
    if (!running_.load(std::memory_order_relaxed)) return;
    if (paused_.load(std::memory_order_relaxed)) {
        if (viewportPresenter_) {
            if (!viewportPresenter_->busy())
                presentNativeViewport(); // Resize/DPI updates do not advance the guest.
            if (viewportPresenter_ && (viewportPresenter_->busy() || viewportPresenter_->needsRetry()))
                frameTimer_->start(kFrameTimerInterval);
        }
        return;
    }
    publishControllers();

    bool fresh = false;
    bool newlyPublished = false;
    if (frameMailbox_) {
        if (auto* slot = frameMailbox_->take()) {
            if (slot->width && slot->height && slot->backing) {
                previousPacked_ = std::move(currentPacked_);
                currentPacked_ = std::shared_ptr<const video::FrameMailbox::Slot>(
                    slot, [mailbox = frameMailbox_](video::FrameMailbox::Slot* leased) {
                        // The mailbox owns the slot and must outlive its lease.
                        (void)mailbox;
                        leased->release();
                    });
                currentTimestamp_ = static_cast<qint64>(slot->timestamp);
                newlyPublished = true;
                if (bootSequence_.waitingForVideo()) {
                    const auto* base = reinterpret_cast<const u8*>(slot->pixelsData());
                    for (u32 y = 0; y < slot->height && bootSequence_.waitingForVideo(); ++y) {
                        for (u32 x = 0; x < slot->width; ++x) {
                            const auto* p = base + y * slot->pitch + x * slot->bytesPerPixel;
                            if (p[0] || (slot->bytesPerPixel == 2 ? (p[1] & 0xFE) : (p[1] | p[2]))) {
                                bootSequence_.observeVideo();
                                break;
                            }
                        }
                    }
                }
                fresh = true;
            } else if (slot->width && slot->height && slot->data) {
                struct FrameLease {
                    std::shared_ptr<video::FrameMailbox> mailbox;
                    video::FrameMailbox::Slot* slot;
                    ~FrameLease() { slot->release(); }
                };
                auto lease = std::unique_ptr<FrameLease>(new FrameLease{frameMailbox_, slot});
                QImage view(reinterpret_cast<const uchar*>(slot->data.get()),
                                  static_cast<int>(slot->width),
                                  static_cast<int>(slot->height),
                                  static_cast<int>(slot->width) * 4,
                                  QImage::Format_RGBA8888,
                                  [](void* data) { delete static_cast<FrameLease*>(data); }, lease.get());
                if (!view.isNull()) {
                    (void)lease.release(); // Last QImage reference releases the mailbox slot.
                    if (bootSequence_.waitingForVideo()) {
                        const auto pixels = slot->pixels();
                        if (std::any_of(pixels.begin(), pixels.end(),
                                        [](u32 px) { return (px & 0x00FFFFFF) != 0; }))
                            bootSequence_.observeVideo();
                    }
                    presentImage(std::move(view));
                    currentTimestamp_ = static_cast<qint64>(slot->timestamp);
                    fresh = true;
                    newlyPublished = true;
                }
            } else slot->release();
        }
    }

    if (bootSequence_.active()) {
        bootOpacity_ = bootSequence_.advance(frameClock_.elapsed());
        if (!bootSequence_.active()) bootTelemetryRequested_.store(false, std::memory_order_relaxed);
        update();
    }
    // Native backends scale the VI texture on the GPU. Keep QPainter for
    // fallback, boot/HUD overlays and offscreen tests.
    if (!presentNativeViewport()) {
        if (currentPacked_ && (fresh || currentFrame_.isNull())) {
            currentFrame_ = decodePackedFrame(*currentPacked_);
            previousFrame_ = previousPacked_ ? decodePackedFrame(*previousPacked_) : QImage();
            fresh = true;
        }
        const bool blend = newlyPublished && interpolationInputsValid() &&
            shouldBlendSoftwareFrame(currentTimestamp_, softwareRevision_,
                                     softwareEndpointShown_, interpolationEnabled_);
        if (blend) {
            composeFrame(0.5);
            softwareEndpointShown_ = false;
            softwareRevision_ = currentTimestamp_;
        } else if (fresh || (!currentFrame_.isNull() &&
                             currentFrame_.cacheKey() != presentedFrame_.cacheKey())) {
            composeFrame(1.0);
            softwareEndpointShown_ = true;
            softwareRevision_ = currentTimestamp_;
        }
    }
    presentationTicks_.fetch_add(1, std::memory_order_relaxed);
    schedulePresentation();
}

float EmuWidget::metalBlendValue(qint64 timestamp, u64 revision, float lastBlend,
                                bool enabled) noexcept {
    // At most one midpoint for a newly published image. The next tick shows
    // a real endpoint even if another image has just arrived. Static scenes
    // must not flicker back to old frames or consume another drawable.
    return enabled && revision != 0 && timestamp > 1 && u64(timestamp) != revision &&
           lastBlend == 1.f ? 0.5f : 1.f;
}

bool EmuWidget::shouldBlendSoftwareFrame(qint64 timestamp, qint64 lastShownTimestamp,
                                         bool priorEndpointShown, bool enabled) noexcept {
    // Mirror the Metal gate: one midpoint per newly published frame, and only
    // when the previous presentation was the real endpoint. Idle ticks show the
    // endpoint instead of toggling into another blended frame.
    return enabled && priorEndpointShown && timestamp > 1 && timestamp != lastShownTimestamp;
}

bool EmuWidget::presentNativeViewport() {
    if (graphicsApi_ == GraphicsApi::Software) return false;
    const QSize source = currentPacked_
        ? QSize(currentPacked_->width, currentPacked_->height) : currentFrame_.size();
    if (source.isEmpty() || bootSequence_.active() || performanceHud_) {
        if (viewportSurface_ && viewportSurface_->isVisible()) viewportSurface_->hide();
        if (viewportPresenter_ && viewportRevision_) {
            viewportPresenter_->discardFrames();
            viewportRevision_ = 0;
        }
        return false;
    }
    // A hidden ancestor makes isVisible() false even for an explicitly
    // shown native child. Do not tear down or retry its drawable off-tab.
    if (!isVisible()) return viewportPresenter_ != nullptr;
    if (!viewportAttempted_) {
        viewportAttempted_ = true;
        if (QGuiApplication::platformName() == QStringLiteral("cocoa"))
            viewportPresenter_ = video::createViewportPresenter(graphicsApi_);
        if (!viewportPresenter_)
            qInfo() << "Native viewport unavailable on platform" << QGuiApplication::platformName();
        if (viewportPresenter_) {
            viewportSurface_ = new QWidget(this);
            viewportSurface_->setAttribute(Qt::WA_NativeWindow);
            viewportSurface_->setAttribute(Qt::WA_TransparentForMouseEvents);
            viewportSurface_->setFocusPolicy(Qt::NoFocus);
            viewportSurface_->setGeometry(rect());
            viewportSurface_->show();
            if (!viewportPresenter_->attach(reinterpret_cast<void*>(viewportSurface_->winId()))) {
                qWarning() << "Native viewport attach failed; using QPainter presentation";
                viewportPresenter_.reset();
                delete viewportSurface_;
                viewportSurface_ = nullptr;
            } else {
                // Qt may finish attaching the NSView on the next event turn.
                return true;
            }
        }
    }
    if (!viewportPresenter_) return false;
    const auto geometry = presentationGeometry(size(), source,
                                               devicePixelRatioF(), outputScale_, ultrawide_);
    if (!geometry.imageWidth || !geometry.imageHeight) return false;
    const bool wasHidden = viewportSurface_->isHidden();
    if (viewportSurface_->geometry() != rect()) viewportSurface_->setGeometry(rect());
    if (wasHidden) {
        viewportSurface_->show();
        viewportGeometry_ = {};
        update();
        // Qt can defer parenting the native child until the next event turn.
        // Don't try to acquire a drawable on the just-shown child yet.
        return true;
    }
    const float blend = metalBlendValue(currentTimestamp_, viewportRevision_, viewportBlend_, interpolationEnabled_);
    if (viewportRevision_ == static_cast<u64>(currentTimestamp_) && viewportGeometry_ == geometry &&
        viewportBlend_ == blend && viewportSmooth_ == smoothScaling_ && !viewportPresenter_->needsRetry()) return true;
    // Keep showing the previous drawable instead of tearing down the surface.
    if (viewportPresenter_->busy()) return true;

    bool presented;
    if (currentPacked_) {
        const auto& s = *currentPacked_;
        presented = viewportPresenter_->presentPacked(
            {s.backing, currentPacked_, s.pitch, s.width, s.height, s.bytesPerPixel, s.timestamp},
            blend, smoothScaling_, geometry);
    } else {
        const auto pixels = std::span<const u32>(reinterpret_cast<const u32*>(currentFrame_.constBits()),
            static_cast<std::size_t>(currentFrame_.width()) * currentFrame_.height());
        presented = viewportPresenter_->present(pixels, currentFrame_.width(), currentFrame_.height(),
                                                 currentTimestamp_, blend, smoothScaling_, geometry);
    }
    if (!presented) {
        // Drawable acquisition can fail transiently on resize/tab changes.
        // Hiding the child here would make the next acquisition fail again.
        if (viewportPresenter_->api() == GraphicsApi::Metal) return true;
        viewportSurface_->hide();
        return false;
    }
    if (viewportPresenter_->needsRetry()) return true;
    paintedFrames_.fetch_add(1, std::memory_order_relaxed);
    viewportRevision_ = currentTimestamp_;
    viewportGeometry_ = geometry;
    viewportBlend_ = blend;
    viewportSmooth_ = smoothScaling_;
    hasActiveVideo_ = true;
    displayedFrame_ = QImage();
    presentedFrame_ = QImage();
    if (currentPacked_) {
        previousFrame_ = QImage();
        currentFrame_ = QImage();
    }
    return true;
}

void EmuWidget::presentImage(QImage image) {
    if (image.isNull() || image.width() <= 0 || image.height() <= 0) return;

    previousFrame_ = std::move(currentFrame_);
    currentFrame_ = std::move(image);
}

QImage EmuWidget::decodePackedFrame(const video::FrameMailbox::Slot& slot) {
    QImage output(slot.width, slot.height, QImage::Format_RGBA8888);
    if (output.isNull()) return output;
    const auto* src = reinterpret_cast<const u8*>(slot.pixelsData());
    for (u32 y = 0; y < slot.height; ++y) {
        auto* dst = output.scanLine(y);
        for (u32 x = 0; x < slot.width; ++x) {
            const auto* p = src + y * slot.pitch + x * slot.bytesPerPixel;
            if (slot.bytesPerPixel == 2) {
                const u32 raw = (u32{p[0]} << 8) | p[1];
                const auto expand = [](u32 n) { return (n << 3) | (n >> 2); };
                dst[x * 4] = expand(raw >> 11);
                dst[x * 4 + 1] = expand((raw >> 6) & 31);
                dst[x * 4 + 2] = expand((raw >> 1) & 31);
            } else {
                std::memcpy(dst + x * 4, p, 3);
            }
            dst[x * 4 + 3] = 255;
        }
    }
    return output;
}

bool EmuWidget::interpolationInputsValid() const {
    return !previousFrame_.isNull() && !currentFrame_.isNull() &&
           previousFrame_.size() == currentFrame_.size() &&
           previousFrame_.format() == QImage::Format_RGBA8888 &&
           currentFrame_.format() == QImage::Format_RGBA8888 &&
           previousFrame_.bytesPerLine() == previousFrame_.width() * 4 &&
           currentFrame_.bytesPerLine() == currentFrame_.width() * 4;
}

void EmuWidget::composeFrame(double fraction) {
    if (currentFrame_.isNull()) return;

    if (fraction < 0.999 && interpolationEnabled_ && interpolationInputsValid()) {
        QImage output(currentFrame_.size(), QImage::Format_RGBA8888);
        const auto span = [](const QImage& frame) {
            return std::span<const u32>(
                reinterpret_cast<const u32*>(frame.constBits()),
                static_cast<std::size_t>(frame.width()) * frame.height());
        };
        if (interpolationEstimateKey_ != currentFrame_.cacheKey()) {
            interpolation_.estimate(span(previousFrame_), span(currentFrame_),
                                    currentFrame_.width(), currentFrame_.height());
            interpolationEstimateKey_ = currentFrame_.cacheKey();
        }
        const auto out = std::span<u32>(
            reinterpret_cast<u32*>(output.bits()),
            static_cast<std::size_t>(output.width()) * output.height());
        if (interpolation_.synthesize(span(previousFrame_), span(currentFrame_), out, fraction))
            displayedFrame_ = std::move(output);
        else
            displayedFrame_ = currentFrame_;
    } else {
        displayedFrame_ = currentFrame_;
    }

    presentedFrame_ = displayedFrame_;
    hasActiveVideo_ = true;
    update();
}

void EmuWidget::setCover(const QPixmap& cover) {
    currentCover_ = cover;
    update();
}

QRect EmuWidget::videoRect(QSize viewport, QSize source, int scale, bool ultrawide) {
    if (viewport.isEmpty() || source.isEmpty()) return {};
    const qreal aspect = ultrawide && scale > 0 ? 21.0 / 9.0 : 4.0 / 3.0;
    qreal width = std::min<qreal>(viewport.width(), viewport.height() * aspect);
    if (scale > 0) width = std::min(width, static_cast<qreal>(source.width()) * scale);
    const int w = static_cast<int>(std::floor(width + 1e-9));
    const int h = static_cast<int>(std::floor(width / aspect + 1e-9));
    return {(viewport.width() - w) / 2, (viewport.height() - h) / 2, w, h};
}

video::PresentationGeometry EmuWidget::presentationGeometry(
    QSize viewport, QSize source, double dpr, int scale, bool ultrawide) {
    if (viewport.isEmpty() || source.isEmpty()) return {};
    const QSize drawable(qRound(viewport.width() * dpr), qRound(viewport.height() * dpr));
    const QSize physicalSource(qRound(source.width() * dpr), qRound(source.height() * dpr));
    const QRect image = videoRect(drawable, physicalSource, scale, ultrawide);
    return {static_cast<u32>(drawable.width()), static_cast<u32>(drawable.height()),
            static_cast<u32>(image.x()), static_cast<u32>(image.y()),
            static_cast<u32>(image.width()), static_cast<u32>(image.height()), dpr};
}

EmuWidget::EmuWidget(ControllerManager* controllerManager, StudioSettings* settings, QWidget* parent)
    : QWidget(parent)
    , controllerManager_(controllerManager)
    , settings_(settings)
    , frameTimer_(new QTimer(this))
    , hudTimer_(new QTimer(this)) {
    setAttribute(Qt::WA_OpaquePaintEvent);
    setFocusPolicy(Qt::StrongFocus);
    frameTimer_->setTimerType(Qt::PreciseTimer);
    frameTimer_->setSingleShot(true);
    frameTimer_->setInterval(kFrameTimerInterval);
    connect(frameTimer_, &QTimer::timeout, this, &EmuWidget::onFrame);
    hudTimer_->setInterval(kUiUpdateInterval);
    connect(hudTimer_, &QTimer::timeout, this, [this]() {
        hudText_ = diagnosticText();
    });
    applyVideoSettings();
}

EmuWidget::~EmuWidget() {
    stopWorker();
}

void EmuWidget::setRenderFps(int fps) {
    const int target = fps == 120 ? 120 : 60;
    if (renderFps_ == target) return;
    renderFps_ = target;
    if (running_.load(std::memory_order_relaxed)) {
        nextPresent_ = 0;
        schedulePresentation();
    }
}

void EmuWidget::setPerformanceHud(bool visible) {
    performanceHud_ = visible;
    if (visible && viewportSurface_) viewportSurface_->hide();
    if (visible) {
        // The guest may be paused, so don't wait for the next presentation
        // tick to provide a software frame under the HUD.
        if (currentPacked_ && presentedFrame_.isNull()) {
            currentFrame_ = decodePackedFrame(*currentPacked_);
            previousFrame_ = previousPacked_ ? decodePackedFrame(*previousPacked_) : QImage();
            composeFrame(1.0);
        }
        hudText_ = tr("Waiting for performance sample…");
        hudTimer_->start();
    } else {
        hudTimer_->stop();
    }
    updateStatsInterest();
    update();
}

void EmuWidget::setDiagnosticsVisible(bool visible) {
    diagnosticsVisible_ = visible;
    updateStatsInterest();
}

void EmuWidget::applyVideoSettings() {
    if (settings_) {
        const auto api = settings_->graphicsApi();
        if (api != graphicsApi_) {
            graphicsApi_ = api;
            viewportPresenter_.reset();
            delete viewportSurface_;
            viewportSurface_ = nullptr;
            viewportGeometry_ = {};
            viewportRevision_ = 0;
            viewportAttempted_ = false;
        }
        smoothScaling_ =
            settings_->scalingFilter().compare(QStringLiteral("nearest"), Qt::CaseInsensitive) != 0;
        interpolationEnabled_ = settings_->frameInterpolation();
        ultrawide_ = settings_->ultrawideEnabled();
        outputScale_ = settings_->viewportScale();
        const int fps = settings_->renderFps();
        renderFps_ = fps == 120 ? 120 : 60;
    }
    if (!bootSequenceEnabled()) {
        bootSequence_.start(false);
        bootOpacity_ = 0;
        bootTelemetryRequested_.store(false, std::memory_order_relaxed);
    }

    update();
    if (running_.load(std::memory_order_relaxed)) {
        nextPresent_ = 0;
        schedulePresentation();
    }
}

void EmuWidget::clearCover() {
    currentCover_ = QPixmap();
    update();
}

} // namespace n64forge::studio
