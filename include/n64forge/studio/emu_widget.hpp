#pragma once

#include "n64forge/core/types.hpp"
#include "n64forge/core/core_stats.hpp"
#include "n64forge/studio/boot_sequence.hpp"
#include "n64forge/studio/settings.hpp"
#include "n64forge/video/frame_mailbox.hpp"
#include "n64forge/video/vi_interpolation.hpp"
#include "n64forge/video/viewport_presenter.hpp"
#include <QElapsedTimer>
#include <QImage>
#include <QStringList>
#include <QPointer>
#include <QTimer>
#include <QWidget>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

namespace n64forge {
class Emulator;
}
namespace n64forge::input {
class SdlInputManager;
}

namespace n64forge::studio {

class AudioOutput;
class ControllerManager;

class EmuWidget : public QWidget {
    Q_OBJECT
public:
    explicit EmuWidget(ControllerManager* controllerManager,
                       StudioSettings* settings,
                       QWidget* parent = nullptr);
    ~EmuWidget() override;

    [[nodiscard]] bool isRunning() const noexcept { return running_.load(std::memory_order_relaxed); }
    [[nodiscard]] bool isPaused() const noexcept { return paused_.load(std::memory_order_relaxed); }
    [[nodiscard]] QString romPath() const { return romPath_; }
    [[nodiscard]] bool hasGame() const noexcept;
    void setCover(const QPixmap& cover);
    void clearCover();
    void setPerformanceHud(bool visible);
    void setDiagnosticsVisible(bool visible);
    [[nodiscard]] QString diagnosticText() const;
    [[nodiscard]] QString activePresenterName() const;
    [[nodiscard]] QString activePresenterDevice() const;

    // Rectangle for the nominal 4:3 console picture or the opt-in 21:9 mode.
    [[nodiscard]] static QRect videoRect(QSize viewport, QSize source, int scale, bool ultrawide = false);

public slots:
    bool loadRom(const QString& path);
    void start();
    void stop();
    void pause();
    void resume();
    void reset();
    void applyVideoSettings();
    void setRenderFps(int fps);

protected:
    bool event(QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void keyReleaseEvent(QKeyEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;

private slots:
    void onFrame();

private:
    friend struct StudioVideoProbe;
    friend struct SoftwareBlendProbe;
    void startWorker();
    void stopWorker();
    void clearFrames();
    void schedulePresentation();
    // Stores a completed guest frame without uploading or blending it.
    void presentImage(QImage image);
    [[nodiscard]] static QImage decodePackedFrame(const video::FrameMailbox::Slot& slot);
    // Presents either the newest guest frame (1.0) or a synthesized midpoint.
    void composeFrame(double fraction);
    [[nodiscard]] bool interpolationInputsValid() const;
    void updateStatsInterest();
    [[nodiscard]] CoreStats performanceSnapshot() const;
    [[nodiscard]] QString audioStatsText(bool detailed) const;
    void paintPerformanceHud(QPainter& painter);
    [[nodiscard]] bool bootSequenceEnabled() const;
    void prepareBootSequence();
    void paintBootSequence(QPainter& painter) const;
    void publishControllers();
    void loadSaveMedia(const QString& directory = {});
    void saveMedia();
    bool presentNativeViewport();
    [[nodiscard]] static float metalBlendValue(qint64 timestamp, u64 revision, float lastBlend,
                                               bool enabled) noexcept;
    // Software counterpart to metalBlendValue: blend only when a new source
    // frame exists and the previous tick showed a real endpoint.
    [[nodiscard]] static bool shouldBlendSoftwareFrame(qint64 timestamp, qint64 lastShownTimestamp,
                                                       bool priorEndpointShown,
                                                       bool enabled) noexcept;
    [[nodiscard]] static qint64 nextPresentationDeadline(qint64 now, qint64 previous, int fps) noexcept;
    [[nodiscard]] static video::PresentationGeometry presentationGeometry(
        QSize viewport, QSize source, double dpr, int scale, bool ultrawide = false);

    QPointer<ControllerManager> controllerManager_;
    std::array<QString, 4> pakPaths_;
    SaveMedia saveMedia_ = SaveMedia::None;
    QString savePath_;
    QString autoEepromPath_, autoSramPath_, autoFlashPath_;
    bool autoEepromLoaded4K_ = false;
    QString saveStatus_; // Published under statsMutex_ by the guest worker.
    std::atomic<u8> motorMask_{0};
    StudioSettings* settings_ = nullptr;

    std::unique_ptr<n64forge::Emulator> emulator_;

    std::unique_ptr<AudioOutput> audio_;
    QTimer* frameTimer_ = nullptr;

    std::jthread worker_;
    std::mutex workerWaitMutex_;
    std::condition_variable workerWaitCv_;
    std::atomic<bool> workerStopRequested_{false};
    std::atomic<bool> statsRequested_{false};
    std::atomic<u64> paintedFrames_{0};
    std::atomic<u64> presentationTicks_{0};
    qint64 lastPaintedKey_ = 0;
    mutable std::mutex statsMutex_;
    CoreStats stats_;
    QTimer* hudTimer_ = nullptr;
    QString hudText_;
    bool diagnosticsVisible_ = false;
    bool performanceHud_ = false;
    std::shared_ptr<video::FrameMailbox> frameMailbox_;
    std::shared_ptr<const video::FrameMailbox::Slot> currentPacked_, previousPacked_;
    QImage previousFrame_;
    QImage currentFrame_;
    video::ViInterpolation interpolation_;
    qint64 interpolationEstimateKey_ = 0;
    qint64 currentTimestamp_ = 0;
    qint64 sourceInterval_ = 0;
    // Software interpolation presents at most one midpoint per newly published
    // frame, and only after the previous frame was shown at its real endpoint.
    bool softwareEndpointShown_ = true;
    qint64 softwareRevision_ = 0;
    QImage displayedFrame_;
    QImage presentedFrame_;
    std::unique_ptr<video::ViewportPresenter> viewportPresenter_;
    QWidget* viewportSurface_ = nullptr;
    bool viewportAttempted_ = false;
    u64 viewportRevision_ = 0;
    float viewportBlend_ = 1;
    video::PresentationGeometry viewportGeometry_;
    bool viewportSmooth_ = false;
    int outputScale_ = 0;
    bool hasActiveVideo_ = false;
    QString romPath_;
    std::atomic<bool> running_{false};
    std::atomic<bool> paused_{false};
    int renderFps_ = 60;
    CpuExecutionMode activeCpuMode_ = CpuExecutionMode::Cached;
    QElapsedTimer frameClock_;
    qint64 nextPresent_ = 0;
    BootSequence bootSequence_;
    QStringList bootLines_;
    double bootOpacity_ = 0;
    std::atomic<bool> bootTelemetryRequested_{false};
    std::atomic<u64> bootRspLaunches_{0}, bootRdpSyncs_{0};

    bool interpolationEnabled_ = false;
    bool smoothScaling_ = true;
    bool ultrawide_ = false;
    GraphicsApi graphicsApi_ = GraphicsApi::Metal;

    QPixmap currentCover_;
};

} // namespace n64forge::studio
