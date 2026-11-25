#pragma once
#include "n64forge/core/cpu.hpp"
#include "n64forge/video/graphics_api.hpp"
#include <QByteArray>
#include <QObject>
#include <QSettings>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <memory>

namespace n64forge::studio {

// The cartridge save medium is explicit. Studio never infers it from ROM identity.
// Persisted values: append modes without renumbering old preferences.
enum class SaveMedia { None, FlashRam, Sram, Eeprom4K, Eeprom16K, Auto };

using GraphicsApi = video::GraphicsApi;

class StudioSettings : public QObject {
public:
    using SaveMedia = n64forge::studio::SaveMedia;
    using GraphicsApi = n64forge::studio::GraphicsApi;

    explicit StudioSettings(QObject* parent = nullptr);

    // Window layout
    void setWindowGeometry(const QByteArray& geometry);
    [[nodiscard]] QByteArray windowGeometry() const;

    void setSplitterState(const QByteArray& state);
    [[nodiscard]] QByteArray splitterState() const;

    // Directories remembered for file dialogs
    void setLastProjectDirectory(const QString& path);
    [[nodiscard]] QString lastProjectDirectory() const;

    void setLastRomDirectory(const QString& path);
    [[nodiscard]] QString lastRomDirectory() const;

    // Recent files
    void addRecentProject(const QString& path);
    [[nodiscard]] QStringList recentProjects() const;
    void clearRecentProjects();

    void addRecentRom(const QString& path);
    [[nodiscard]] QStringList recentRoms() const;
    void clearRecentRoms();

    // Currently playing ROM, used for per-game splash/cover handling.
    void setCurrentRom(const QString& path);
    [[nodiscard]] QString currentRom() const;

    // Emulation preferences
    void setCpuMode(CpuExecutionMode mode, bool sessionOnly = false);
    [[nodiscard]] CpuExecutionMode cpuMode() const;
    void setPauseOnFocusLoss(bool pause);
    [[nodiscard]] bool pauseOnFocusLoss() const;

    void setVSyncEnabled(bool enabled);
    [[nodiscard]] bool vsyncEnabled() const;

    // Video presentation preferences.
    void setGraphicsApi(GraphicsApi api, bool sessionOnly = false);
    [[nodiscard]] GraphicsApi graphicsApi() const;

    void setFrameInterpolation(bool enabled);
    [[nodiscard]] bool frameInterpolation() const;

    // "nearest" or "smooth".
    void setScalingFilter(const QString& filter);
    [[nodiscard]] QString scalingFilter() const;

    void setIntegerScaling(bool enabled);
    [[nodiscard]] bool integerScaling() const;

    void setUltrawideEnabled(bool enabled);
    [[nodiscard]] bool ultrawideEnabled() const;

    // Target render frame rate for presentation-only frame duplication
    // (0 = use the core-reported rate).
    void setRenderFps(int fps);
    [[nodiscard]] int renderFps() const;

    // 1/2/4x internal RDP resolution. RDRAM writeback remains native.
    void setViewportScale(int scale);
    [[nodiscard]] int viewportScale() const;
    void applyRecommendedVideoSettings();
    void setUiParallaxEnabled(bool enabled);
    [[nodiscard]] bool uiParallaxEnabled() const;
    void setRdpThreads(unsigned threads);
    [[nodiscard]] unsigned rdpThreads() const;
    void setInternalScale(unsigned scale, bool sessionOnly = false);
    [[nodiscard]] unsigned internalScale() const;
    void setBootSequenceEnabled(bool enabled);
    [[nodiscard]] bool bootSequenceEnabled() const;

    void setSaveMedia(SaveMedia media);
    [[nodiscard]] SaveMedia saveMedia() const;
    // Legacy wrappers kept for callers that only care about FlashRAM.
    void setFlashRamEnabled(bool enabled);
    [[nodiscard]] bool flashRamEnabled() const;

    // Generic access for arbitrary module settings.
    [[nodiscard]] QVariant value(const QString& key, const QVariant& defaultValue = QVariant()) const;
    void setValue(const QString& key, const QVariant& value);
    void remove(const QString& key);

    void setDiscordEnabled(bool enabled);
    [[nodiscard]] bool discordEnabled() const;
    void setDiscordClientId(const QString& id);
    [[nodiscard]] QString discordClientId() const;

private:
    std::optional<CpuExecutionMode> cpuModeOverride_;
    std::optional<GraphicsApi> graphicsApiOverride_;
    std::optional<unsigned> internalScaleOverride_;
    void prependUnique(QStringList& list, const QString& path);
    static constexpr int MaxRecentFiles = 10;
    std::unique_ptr<QSettings> settings_;
};

} // namespace n64forge::studio
