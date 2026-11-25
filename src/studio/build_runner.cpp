#include "n64forge/studio/build_runner.hpp"
#include <QDir>
#include <QFileInfo>

namespace n64forge::studio {

QString formatCommandInvocation(const QString& program, const QStringList& arguments) {
    QString text = QStringLiteral("Program: %1\nargv:\n  [0] %2").arg(program, program);
    for (int i = 0; i < arguments.size(); ++i) {
        text += QStringLiteral("\n  [%1] %2").arg(i + 1).arg(arguments.at(i));
    }
    return text;
}

QString formatEnvironmentForDisplay(const QProcessEnvironment& environment) {
    QStringList keys = environment.keys();
    keys.sort(Qt::CaseInsensitive);
    if (keys.isEmpty()) return QStringLiteral("<empty>");

    QStringList lines;
    lines.reserve(keys.size());
    for (const QString& key : keys) {
        lines.append(key + QLatin1Char('=') + environment.value(key));
    }
    return lines.join(QLatin1Char('\n'));
}

QString describeCommand(const QString& program,
                        const QStringList& arguments,
                        const QString& workingDirectory,
                        const QProcessEnvironment& environment) {
    return QStringLiteral("%1\nWorking directory: %2\nEnvironment:\n%3")
        .arg(formatCommandInvocation(program, arguments), workingDirectory,
             formatEnvironmentForDisplay(environment));
}

QStringList recompilerArguments(const QString& subCommand,
                                const QString& romPath,
                                const QStringList& extraArgs) {
    QStringList arguments;
    arguments.append(subCommand);
    arguments.append(romPath);
    arguments.append(extraArgs);
    return arguments;
}

QString resolveSiblingCliExecutable(const QString& studioExecutablePath) {
    if (studioExecutablePath.isEmpty()) return {};

    QDir dir = QFileInfo(studioExecutablePath).absoluteDir();
    QStringList directories;
    const auto addDirectory = [&directories](const QString& path) {
        if (!path.isEmpty() && !directories.contains(path)) directories.append(path);
    };
    addDirectory(dir.absolutePath());
    // macOS: .../MyApp.app/Contents/MacOS -> Contents -> MyApp.app -> parent.
    for (int i = 0; i < 3; ++i) {
        if (!dir.cdUp()) break;
        addDirectory(dir.absolutePath());
    }

    // The CLI target may be a plain executable, a Windows .exe, or a macOS
    // bundle whose inner executable is named n64forge.
    const QStringList names{
        QStringLiteral("n64forge"),
        QStringLiteral("n64forge.exe"),
        QStringLiteral("n64forge.app/Contents/MacOS/n64forge"),
    };
    for (const QString& directory : directories) {
        for (const QString& name : names) {
            const QFileInfo candidate(QDir(directory).filePath(name));
            if (candidate.exists() && candidate.isExecutable()) {
                return candidate.canonicalFilePath();
            }
        }
    }
    return {};
}

QStringList cmakeConfigureArguments(const QString& sourceDir, const QString& buildDir) {
    return QStringList{QStringLiteral("-S"), sourceDir,
                       QStringLiteral("-B"), buildDir};
}

QStringList cmakeBuildArguments(const QString& buildDir) {
    return QStringList{QStringLiteral("--build"), buildDir,
                       QStringLiteral("--config"), QStringLiteral("Release")};
}

BuildRunner::BuildRunner(QObject* parent)
    : QObject(parent)
    , process_(new QProcess(this)) {
    // One long-lived QProcess: connections are installed once so repeated
    // builds do not accumulate child objects or signal handlers.
    connect(process_, &QProcess::readyReadStandardOutput, this, [this]() {
        emit output(QString::fromLocal8Bit(process_->readAllStandardOutput()));
    });
    connect(process_, &QProcess::readyReadStandardError, this, [this]() {
        emit output(QString::fromLocal8Bit(process_->readAllStandardError()));
    });
    connect(process_, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this](int exitCode, QProcess::ExitStatus) {
                const bool wasCancelled = cancelRequested_;
                processActive_ = false;
                cancelRequested_ = false;
                if (wasCancelled) emit cancelled();
                emit finished(exitCode);
            });
    connect(process_, &QProcess::errorOccurred, this,
            [this](QProcess::ProcessError error) {
                if (cancelRequested_) return;
                const bool terminal = error == QProcess::FailedToStart;
                if (terminal) {
                    processActive_ = false;
                    cancelRequested_ = false;
                }
                emit errorOccurred(tr("Build process error %1: %2")
                                       .arg(static_cast<int>(error))
                                       .arg(process_->errorString()));
            });
}

void BuildRunner::start(const QString& program,
                        const QString& workingDir,
                        const QStringList& arguments,
                        const QProcessEnvironment& environment) {
    if (processActive_) {
        emit errorOccurred(tr("A build is already running."));
        return;
    }
    if (program.trimmed().isEmpty()) {
        emit errorOccurred(tr("No build program was configured."));
        return;
    }

    cancelRequested_ = false;
    process_->setProgram(program);
    process_->setArguments(arguments);
    process_->setWorkingDirectory(workingDir);
    process_->setProcessEnvironment(environment);
    processActive_ = true;
    process_->start();
}

void BuildRunner::cancel() {
    if (processActive_ && process_->state() != QProcess::NotRunning) {
        cancelRequested_ = true;
        process_->terminate();
        if (!process_->waitForFinished(3000)) {
            process_->kill();
        }
    }
}

bool BuildRunner::isRunning() const {
    return processActive_;
}

} // namespace n64forge::studio
