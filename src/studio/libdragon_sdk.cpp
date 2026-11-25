#include "n64forge/studio/libdragon_sdk.hpp"
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QProcessEnvironment>
#include <QtGlobal>

namespace n64forge::studio {

QStringList LibdragonSdkDetector::defaultSearchPaths() {
    QStringList paths;
    const QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    if (env.contains(QStringLiteral("N64_INST"))) {
        paths.append(env.value(QStringLiteral("N64_INST")));
    }
    paths.append(QStringLiteral("/opt/libdragon"));
    paths.append(QStringLiteral("/usr/local"));
    paths.append(QStringLiteral("/opt/homebrew/opt/libdragon"));
    paths.append(QDir::homePath() + QStringLiteral("/libdragon"));
    return paths;
}

LibdragonSdk LibdragonSdkDetector::detect(const QString& path) {
    LibdragonSdk sdk;
    sdk.rootPath = QDir(path).absolutePath();

    if (sdk.rootPath.isEmpty() || !QDir(sdk.rootPath).exists()) {
        sdk.error = QObject::tr("SDK path does not exist: %1").arg(path);
        return sdk;
    }

    const QString n64mk = sdk.rootPath + QStringLiteral("/include/n64.mk");
    if (!QFile::exists(n64mk)) {
        sdk.error = QObject::tr("Missing libdragon build file: %1").arg(n64mk);
        return sdk;
    }

    sdk.gccPath = sdk.rootPath + QStringLiteral("/bin/mips64-elf-gcc");
    if (!QFile::exists(sdk.gccPath)) {
        sdk.gccPath = sdk.rootPath + QStringLiteral("/bin/mips64-gcc");
        if (!QFile::exists(sdk.gccPath)) {
            sdk.error = QObject::tr("Missing MIPS64 GCC compiler in %1/bin").arg(sdk.rootPath);
            return sdk;
        }
    }

    QProcess makeProc;
    makeProc.start(QStringLiteral("make"), QStringList{QStringLiteral("--version")});
    if (!makeProc.waitForStarted() || !makeProc.waitForFinished()) {
        sdk.error = QObject::tr("'make' is not available in PATH");
        return sdk;
    }
    sdk.makePath = QStringLiteral("make");

    QProcess gccProc;
    gccProc.start(sdk.gccPath, QStringList{QStringLiteral("--version")});
    if (gccProc.waitForStarted() && gccProc.waitForFinished()) {
        const QString output = QString::fromLocal8Bit(gccProc.readAllStandardOutput().trimmed());
        sdk.version = output.split(QLatin1Char('\n')).value(0);
    } else {
        sdk.error = QObject::tr("Failed to run compiler: %1").arg(sdk.gccPath);
        return sdk;
    }

    sdk.valid = true;
    return sdk;
}

LibdragonSdk LibdragonSdkDetector::autodetect() {
    for (const QString& path : defaultSearchPaths()) {
        LibdragonSdk sdk = detect(path);
        if (sdk.isValid()) return sdk;
    }
    LibdragonSdk fail;
    fail.error = QObject::tr("No libdragon SDK found. Set N64_INST or configure the path manually.");
    return fail;
}

} // namespace n64forge::studio
