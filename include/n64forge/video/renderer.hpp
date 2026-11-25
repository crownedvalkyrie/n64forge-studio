#pragma once
#include <memory>
#include <string>

namespace n64forge {
struct RendererOptions {
    unsigned internalScale = 1; // 1 = native VI target; higher values are opt-in.
    bool vsync = true;
    bool frameInterpolation = false;
};
class Renderer {
public:
    virtual ~Renderer() = default;
    virtual bool initialize(void* nativeWindow, const RendererOptions& options) = 0;
    virtual void present() = 0;
    virtual std::string backendName() const = 0;
};
std::unique_ptr<Renderer> createRenderer();
}
