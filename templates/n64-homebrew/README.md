# N64 Homebrew Starter Template

This is a source/project placeholder for a libdragon-based Nintendo 64 homebrew project.

## Before building
1. Install libdragon using its official instructions for your OS and selected release:
   https://libdragon.dev/
2. Follow that release's documented toolchain setup and environment configuration.
3. Configure the SDK/toolchain path in N64Forge Studio.
4. Use the sample project/build files supplied by the exact libdragon release you installed.

N64Forge deliberately does not guess a Makefile include path or SDK version. The toolkit will validate the installed SDK and generate a project against the selected supported version.

The resulting output is a Nintendo 64 ROM, not a Windows executable or macOS app.
