#pragma once
#include <QObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QString>
#include <QStringList>

namespace n64forge::studio {

// Command descriptions are intentionally token-based: no shell quoting or
// shell-concatenated command strings are used anywhere in the Studio.
[[nodiscard]] QString formatCommandInvocation(const QString& program,
                                              const QStringList& arguments);
[[nodiscard]] QString formatEnvironmentForDisplay(const QProcessEnvironment& environment);
[[nodiscard]] QString describeCommand(const QString& program,
                                      const QStringList& arguments,
                                      const QString& workingDirectory,
                                      const QProcessEnvironment& environment);

[[nodiscard]] QStringList recompilerArguments(const QString& subCommand,
                                              const QString& romPath,
                                              const QStringList& extraArgs = {});
[[nodiscard]] QString resolveSiblingCliExecutable(const QString& studioExecutablePath);
[[nodiscard]] QStringList cmakeConfigureArguments(const QString& sourceDir,
                                                  const QString& buildDir);
[[nodiscard]] QStringList cmakeBuildArguments(const QString& buildDir);

class BuildRunner : public QObject {
    Q_OBJECT
public:
    explicit BuildRunner(QObject* parent = nullptr);

    void start(const QString& program,
               const QString& workingDir,
               const QStringList& arguments,
               const QProcessEnvironment& environment);
    void cancel();
    [[nodiscard]] bool isRunning() const;

signals:
    void output(const QString& text);
    void finished(int exitCode);
    void errorOccurred(const QString& message);
    void cancelled();

private:
    QProcess* process_ = nullptr;
    bool processActive_ = false;
    bool cancelRequested_ = false;
};

} // namespace n64forge::studio
