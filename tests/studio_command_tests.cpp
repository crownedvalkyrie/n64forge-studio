// Deterministic Studio command-safety tests. CMakeLists.txt is intentionally
// untouched by this change; wire this file into n64forge_tests separately.
#include "n64forge/studio/build_runner.hpp"
#include "n64forge/studio/project_wizard.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <QtGlobal>
#include <cstdlib>
#include <iostream>

using namespace n64forge::studio;

// project_wizard.hpp is outside this task's edit scope; declare the helper
// locally so this standalone test can exercise it once linked with the source.
namespace n64forge::studio {
[[nodiscard]] bool isValidProjectName(const QString& name);
}

namespace {

void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "studio_command test failed: " << message << '\n';
        std::abort();
    }
}

void writeExecutable(const QString& path) {
    QFile file(path);
    expect(file.open(QIODevice::WriteOnly), "create fake executable");
    file.write("#!/bin/sh\nexit 0\n");
    file.close();
    expect(file.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner |
                               QFile::ReadGroup | QFile::ExeGroup |
                               QFile::ReadOther | QFile::ExeOther),
           "set fake executable permissions");
}

void testProjectNameValidation() {
    const QStringList valid{
        QStringLiteral("x"),
        QStringLiteral("MyN64Game"),
        QStringLiteral("game_v1.2-final"),
        QStringLiteral("A"),
    };
    for (const QString& name : valid) {
        expect(isValidProjectName(name), "valid project name rejected");
    }

    const QStringList invalid{
        QString(),
        QStringLiteral(" x"),
        QStringLiteral("x "),
        QStringLiteral("x$(shell touch marker)"),
        QStringLiteral("x;rm -rf /"),
        QStringLiteral("x/y"),
        QStringLiteral("x\ny"),
        QStringLiteral(".hidden"),
        QStringLiteral("-dash"),
        QStringLiteral("a b"),
        QStringLiteral("a@b"),
    };
    for (const QString& name : invalid) {
        expect(!isValidProjectName(name), "invalid project name accepted");
    }
}

void testRecompilerArguments() {
    const QStringList args = recompilerArguments(
        QStringLiteral("recompile"),
        QStringLiteral("/roms/game with spaces.z64"),
        QStringList{QStringLiteral("-o"), QStringLiteral("/out/$(touch marker)")});
    expect(args.size() == 4, "recompiler argv token count");
    expect(args.at(0) == QStringLiteral("recompile"), "argv[0] is the subcommand");
    expect(args.at(1) == QStringLiteral("/roms/game with spaces.z64"), "argv[1] is the ROM path");
    expect(args.at(2) == QStringLiteral("-o"), "argv[2] is -o");
    expect(args.at(3) == QStringLiteral("/out/$(touch marker)"), "argv[3] is kept verbatim");
}

void testCmakeArguments() {
    const QStringList configure = cmakeConfigureArguments(
        QStringLiteral("/src dir"), QStringLiteral("/src dir/build-native"));
    expect(configure == QStringList({QStringLiteral("-S"), QStringLiteral("/src dir"),
                                     QStringLiteral("-B"), QStringLiteral("/src dir/build-native")}),
           "cmake configure argv");

    const QStringList build = cmakeBuildArguments(QStringLiteral("/src dir/build-native"));
    expect(build == QStringList({QStringLiteral("--build"), QStringLiteral("/src dir/build-native"),
                                 QStringLiteral("--config"), QStringLiteral("Release")}),
           "cmake build argv");
}

void testCommandDescription() {
    QProcessEnvironment env;
    env.insert(QStringLiteral("N64FORGE_COMMAND_TEST"), QStringLiteral("token-with spaces"));
    env.insert(QStringLiteral("N64_INST"), QStringLiteral("/sdk"));

    const QStringList args{QStringLiteral("--build"), QStringLiteral("/tmp/build dir")};
    const QString text = describeCommand(QStringLiteral("/path with space/cmake"), args,
                                         QStringLiteral("/work dir"), env);

    expect(text.contains(QStringLiteral("Program: /path with space/cmake")), "program preview");
    expect(text.contains(QStringLiteral("  [0] /path with space/cmake")), "argv[0] preview");
    expect(text.contains(QStringLiteral("  [1] --build")), "argv[1] preview");
    expect(text.contains(QStringLiteral("  [2] /tmp/build dir")), "argv[2] preview");
    expect(!text.contains(QStringLiteral("cmake --build")), "no shell-joined command");
    expect(text.contains(QStringLiteral("Working directory: /work dir")), "working directory preview");
    expect(text.contains(QStringLiteral("N64_FORGE_COMMAND_TEST=")) == false, "env key not mutated");
    expect(text.contains(QStringLiteral("N64FORGE_COMMAND_TEST=token-with spaces")),
           "full environment preview");
}

void testSiblingCliResolution() {
    QTemporaryDir temp;
    expect(temp.isValid(), "temporary directory");
    const QDir root(temp.path());
    expect(root.mkpath(QStringLiteral("N64Forge Studio.app/Contents/MacOS")), "studio bundle path");
    const QString studio = root.filePath(
        QStringLiteral("N64Forge Studio.app/Contents/MacOS/n64forge_studio"));
    writeExecutable(studio);

    const QString flatCli = root.filePath(QStringLiteral("n64forge"));
    writeExecutable(flatCli);
#ifdef Q_OS_WIN
    const QString flatCliExe = root.filePath(QStringLiteral("n64forge.exe"));
    writeExecutable(flatCliExe);
#endif
    const QString resolved = resolveSiblingCliExecutable(studio);
    expect(!resolved.isEmpty(), "flat sibling CLI resolved");
    const QFileInfo resolvedInfo(resolved);
    expect(resolvedInfo.absolutePath() == QFileInfo(flatCli).canonicalPath(),
           "CLI resolved next to Studio app");

#ifndef Q_OS_WIN
    expect(QFile::remove(flatCli), "remove flat CLI");
    expect(root.mkpath(QStringLiteral("n64forge.app/Contents/MacOS")), "CLI bundle path");
    const QString bundledCli = root.filePath(
        QStringLiteral("n64forge.app/Contents/MacOS/n64forge"));
    writeExecutable(bundledCli);
    const QString resolvedBundle = resolveSiblingCliExecutable(studio);
    expect(QFileInfo(resolvedBundle).canonicalFilePath() ==
               QFileInfo(bundledCli).canonicalFilePath(),
           "macOS CLI bundle resolved");
#endif
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    testProjectNameValidation();
    testRecompilerArguments();
    testCmakeArguments();
    testCommandDescription();
    testSiblingCliResolution();
    std::cout << "studio_command tests passed\n";
    return 0;
}
