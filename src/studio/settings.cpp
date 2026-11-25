#include "n64forge/studio/settings.hpp"
#include <QDir>
#include <QSettings>
#include <QtGlobal>

namespace n64forge::studio {

void StudioSettings::setUiParallaxEnabled(bool enabled) {
    settings_->setValue(QStringLiteral("appearance/uiParallax"), enabled);
}

bool StudioSettings::uiParallaxEnabled() const {
    return settings_->value(QStringLiteral("appearance/uiParallax"), true).toBool();
}

void StudioSettings::setRdpThreads(unsigned threads) {
    settings_->setValue(QStringLiteral("emulator/rdpThreads"), threads);
}

unsigned StudioSettings::rdpThreads() const {
    const unsigned count = settings_->value(QStringLiteral("emulator/rdpThreads"), 0).toUInt();
    return count == 1 || count == 2 || count == 4 || count == 8 ? count : 0;
}

void StudioSettings::setInternalScale(unsigned scale, bool sessionOnly) {
    if (scale < 1 || scale > 4) return;
    if (sessionOnly) {
        internalScaleOverride_ = scale;
    } else {
        internalScaleOverride_.reset();
        settings_->setValue(QStringLiteral("video/internalScale"), scale);
    }
}

unsigned StudioSettings::internalScale() const {
    if (internalScaleOverride_) return *internalScaleOverride_;
    const auto scale = settings_->value(QStringLiteral("video/internalScale"), 1).toUInt();
    return scale >= 1 && scale <= 4 ? scale : 1;
}

void StudioSettings::applyRecommendedVideoSettings() {
    setViewportScale(0);
    setUltrawideEnabled(false);
    setScalingFilter(QStringLiteral("nearest"));
    setFrameInterpolation(false);
}

StudioSettings::StudioSettings(QObject* parent)
    : QObject(parent)
    , settings_(std::make_unique<QSettings>(
          QSettings::IniFormat, QSettings::UserScope,
          QStringLiteral("N64Forge"), QStringLiteral("Studio"))) {}

void StudioSettings::setCpuMode(CpuExecutionMode mode, bool sessionOnly) {
    if (sessionOnly) {
        cpuModeOverride_ = mode;
    } else {
        cpuModeOverride_.reset();
        const auto name = mode == CpuExecutionMode::Jit ? "jit" :
                          mode == CpuExecutionMode::Interpreter ? "interpreter" : "cached";
        settings_->setValue(QStringLiteral("emulator/cpuMode"), QString::fromLatin1(name));
    }
}

CpuExecutionMode StudioSettings::cpuMode() const {
    if (cpuModeOverride_) return *cpuModeOverride_;
    return parseCpuMode(settings_->value(QStringLiteral("emulator/cpuMode")).toString().toStdString())
        .value_or(defaultDesktopCpuMode());
}

void StudioSettings::setBootSequenceEnabled(bool enabled) {
    settings_->setValue(QStringLiteral("video/bootSequence"), enabled);
}

bool StudioSettings::bootSequenceEnabled() const {
    return settings_->value(QStringLiteral("video/bootSequence"), true).toBool();
}

void StudioSettings::setSaveMedia(SaveMedia media) {
    settings_->setValue(QStringLiteral("emulator/saveMedia"), static_cast<int>(media));
    settings_->remove(QStringLiteral("emulator/flashRam"));
}

SaveMedia StudioSettings::saveMedia() const {
    if (!settings_->contains(QStringLiteral("emulator/saveMedia")) &&
        settings_->value(QStringLiteral("emulator/flashRam"), false).toBool())
        return SaveMedia::FlashRam;
    const auto value = settings_->value(QStringLiteral("emulator/saveMedia"),
                                        static_cast<int>(SaveMedia::Auto)).toInt();
    switch (value) {
        case static_cast<int>(SaveMedia::Auto): return SaveMedia::Auto;
        case static_cast<int>(SaveMedia::FlashRam): return SaveMedia::FlashRam;
        case static_cast<int>(SaveMedia::Sram): return SaveMedia::Sram;
        case static_cast<int>(SaveMedia::Eeprom4K): return SaveMedia::Eeprom4K;
        case static_cast<int>(SaveMedia::Eeprom16K): return SaveMedia::Eeprom16K;
        default: return SaveMedia::None;
    }
}

void StudioSettings::setFlashRamEnabled(bool enabled) {
    setSaveMedia(enabled ? SaveMedia::FlashRam : SaveMedia::None);
}

bool StudioSettings::flashRamEnabled() const {
    return saveMedia() == SaveMedia::FlashRam;
}

void StudioSettings::setWindowGeometry(const QByteArray& geometry) {
    settings_->setValue(QStringLiteral("window/geometry"), geometry);
}

QByteArray StudioSettings::windowGeometry() const {
    return settings_->value(QStringLiteral("window/geometry")).toByteArray();
}

void StudioSettings::setSplitterState(const QByteArray& state) {
    settings_->setValue(QStringLiteral("window/splitter"), state);
}

QByteArray StudioSettings::splitterState() const {
    return settings_->value(QStringLiteral("window/splitter")).toByteArray();
}

void StudioSettings::setLastProjectDirectory(const QString& path) {
    settings_->setValue(QStringLiteral("paths/lastProjectDir"), path);
}

QString StudioSettings::lastProjectDirectory() const {
    return settings_->value(QStringLiteral("paths/lastProjectDir"),
                            QDir::homePath()).toString();
}

void StudioSettings::setLastRomDirectory(const QString& path) {
    settings_->setValue(QStringLiteral("paths/lastRomDir"), path);
}

QString StudioSettings::lastRomDirectory() const {
    return settings_->value(QStringLiteral("paths/lastRomDir"),
                            QDir::homePath()).toString();
}

void StudioSettings::prependUnique(QStringList& list, const QString& path) {
    if (path.isEmpty()) return;
    list.removeAll(path);
    list.prepend(path);
    while (list.size() > MaxRecentFiles) list.removeLast();
}

void StudioSettings::addRecentProject(const QString& path) {
    auto list = recentProjects();
    prependUnique(list, path);
    settings_->setValue(QStringLiteral("recent/projects"), list);
}

QStringList StudioSettings::recentProjects() const {
    return settings_->value(QStringLiteral("recent/projects")).toStringList();
}

void StudioSettings::clearRecentProjects() {
    settings_->remove(QStringLiteral("recent/projects"));
}

void StudioSettings::addRecentRom(const QString& path) {
    auto list = recentRoms();
    prependUnique(list, path);
    settings_->setValue(QStringLiteral("recent/roms"), list);
}

QStringList StudioSettings::recentRoms() const {
    return settings_->value(QStringLiteral("recent/roms")).toStringList();
}

void StudioSettings::clearRecentRoms() {
    settings_->remove(QStringLiteral("recent/roms"));
}

void StudioSettings::setCurrentRom(const QString& path) {
    settings_->setValue(QStringLiteral("library/currentRom"), path);
}

QString StudioSettings::currentRom() const {
    return settings_->value(QStringLiteral("library/currentRom"), QString{}).toString();
}

void StudioSettings::setPauseOnFocusLoss(bool pause) {
    settings_->setValue(QStringLiteral("emulation/pauseOnFocusLoss"), pause);
}

bool StudioSettings::pauseOnFocusLoss() const {
    return settings_->value(QStringLiteral("emulation/pauseOnFocusLoss"), true).toBool();
}

void StudioSettings::setVSyncEnabled(bool enabled) {
    settings_->setValue(QStringLiteral("video/vsync"), enabled);
}

bool StudioSettings::vsyncEnabled() const {
    return settings_->value(QStringLiteral("video/vsync"), true).toBool();
}

void StudioSettings::setGraphicsApi(GraphicsApi api, bool sessionOnly) {
    if (sessionOnly) {
        graphicsApiOverride_ = api;
        return;
    }
    graphicsApiOverride_.reset();
    settings_->setValue(QStringLiteral("video/graphicsApi"), static_cast<int>(api));
}

StudioSettings::GraphicsApi StudioSettings::graphicsApi() const {
    if (graphicsApiOverride_) return *graphicsApiOverride_;
    const auto fallback = video::defaultGraphicsApi();
    const int value = settings_->value(QStringLiteral("video/graphicsApi"),
                                       static_cast<int>(fallback)).toInt();
    switch (value) {
        case static_cast<int>(GraphicsApi::Metal): return GraphicsApi::Metal;
        case static_cast<int>(GraphicsApi::MoltenVK): return GraphicsApi::MoltenVK;
        case static_cast<int>(GraphicsApi::KosmicKrisp): return GraphicsApi::KosmicKrisp;
        case static_cast<int>(GraphicsApi::Software): return GraphicsApi::Software;
        case static_cast<int>(GraphicsApi::Auto): return GraphicsApi::Auto;
        case static_cast<int>(GraphicsApi::D3D12): return GraphicsApi::D3D12;
        case static_cast<int>(GraphicsApi::Vulkan): return GraphicsApi::Vulkan;
        default: return fallback;
    }
}

void StudioSettings::setFrameInterpolation(bool enabled) {
    settings_->setValue(QStringLiteral("video/frameInterpolation"), enabled);
}

bool StudioSettings::frameInterpolation() const {
    return settings_->value(QStringLiteral("video/frameInterpolation"), false).toBool();
}

void StudioSettings::setScalingFilter(const QString& filter) {
    settings_->setValue(QStringLiteral("video/scalingFilter"), filter);
}

QString StudioSettings::scalingFilter() const {
    return settings_->value(QStringLiteral("video/scalingFilter"),
                            QStringLiteral("smooth")).toString();
}

void StudioSettings::setIntegerScaling(bool enabled) {
    settings_->setValue(QStringLiteral("video/integerScaling"), enabled);
}

bool StudioSettings::integerScaling() const {
    return settings_->value(QStringLiteral("video/integerScaling"), false).toBool();
}

void StudioSettings::setUltrawideEnabled(bool enabled) {
    settings_->setValue(QStringLiteral("video/ultrawide"), enabled);
}

bool StudioSettings::ultrawideEnabled() const {
    return settings_->value(QStringLiteral("video/ultrawide"), false).toBool();
}

void StudioSettings::setRenderFps(int fps) {
    settings_->setValue(QStringLiteral("video/renderFps"), fps);
}

int StudioSettings::renderFps() const {
    return settings_->value(QStringLiteral("video/renderFps"), 0).toInt();
}

void StudioSettings::setViewportScale(int scale) {
    settings_->setValue(QStringLiteral("video/viewportScale"), scale);
}

int StudioSettings::viewportScale() const {
    const auto scale = settings_->value(QStringLiteral("video/viewportScale"), 0).toInt();
    return scale == 1 || scale == 2 || scale == 4 ? scale : 0;
}

QVariant StudioSettings::value(const QString& key, const QVariant& defaultValue) const {
    return settings_->value(key, defaultValue);
}

void StudioSettings::setValue(const QString& key, const QVariant& value) {
    settings_->setValue(key, value);
}

void StudioSettings::remove(const QString& key) {
    settings_->remove(key);
}

void StudioSettings::setDiscordEnabled(bool enabled) {
    settings_->setValue(QStringLiteral("discord/enabled"), enabled);
}

bool StudioSettings::discordEnabled() const {
    return settings_->value(QStringLiteral("discord/enabled"), false).toBool();
}

void StudioSettings::setDiscordClientId(const QString& id) {
    settings_->setValue(QStringLiteral("discord/clientId"), id);
}

QString StudioSettings::discordClientId() const {
    return settings_->value(QStringLiteral("discord/clientId"), QString{}).toString();
}

} // namespace n64forge::studio
