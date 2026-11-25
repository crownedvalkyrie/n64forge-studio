#include "n64forge/video/renderer.hpp"
#include <memory>
#if defined(_WIN32) && defined(N64FORGE_HAS_D3D12)
std::unique_ptr<n64forge::Renderer> createD3D12Renderer();
#elif defined(__APPLE__) && defined(N64FORGE_HAS_METAL)
std::unique_ptr<n64forge::Renderer> createMetalRenderer();
#endif
namespace n64forge {
std::unique_ptr<Renderer> createRenderer() {
#if defined(_WIN32) && defined(N64FORGE_HAS_D3D12)
    return createD3D12Renderer();
#elif defined(__APPLE__) && defined(N64FORGE_HAS_METAL)
    return createMetalRenderer();
#else
    return {};
#endif
}
}
