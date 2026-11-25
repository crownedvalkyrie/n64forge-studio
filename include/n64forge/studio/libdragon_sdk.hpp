#pragma once
#include <QString>
#include <QStringList>
#include <vector>

namespace n64forge::studio {

struct LibdragonSdk {
    QString rootPath;
    QString gccPath;
    QString makePath;
    QString version;
    bool valid = false;
    QString error;

    [[nodiscard]] bool isValid() const noexcept { return valid; }
};

class LibdragonSdkDetector {
public:
    [[nodiscard]] static QStringList defaultSearchPaths();
    [[nodiscard]] static LibdragonSdk detect(const QString& path);
    [[nodiscard]] static LibdragonSdk autodetect();
};

} // namespace n64forge::studio
