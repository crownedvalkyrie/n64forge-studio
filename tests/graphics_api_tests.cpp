#include "n64forge/video/graphics_api.hpp"

#include <cassert>
#include <iostream>

namespace {

using n64forge::video::GraphicsApi;
using n64forge::video::GraphicsBackendAvailability;
using n64forge::video::defaultGraphicsApi;
using n64forge::video::graphicsApiName;
using n64forge::video::parseGraphicsApi;
using n64forge::video::resolveGraphicsApi;

} // namespace

int runGraphicsApiTests() {
    using enum GraphicsApi;

    // Persisted names must stay stable: settings files from older builds keep
    // loading, and the new Windows values round-trip through the same setter.
    assert(parseGraphicsApi("metal") == Metal);
    assert(parseGraphicsApi("moltenvk") == MoltenVK);
    assert(parseGraphicsApi("kosmickrisp") == KosmicKrisp);
    assert(parseGraphicsApi("software") == Software);
    assert(parseGraphicsApi("auto") == Auto);
    assert(parseGraphicsApi("d3d12") == D3D12);
    assert(parseGraphicsApi("vulkan") == Vulkan);
    assert(!parseGraphicsApi("direct3d"));
    assert(!parseGraphicsApi(""));

    for (auto api : {Metal, MoltenVK, KosmicKrisp, Software, Auto, D3D12, Vulkan}) {
        const auto name = graphicsApiName(api);
        assert(parseGraphicsApi(name) == api);
    }

#if defined(_WIN32)
    assert(defaultGraphicsApi() == Auto);
#else
    assert(defaultGraphicsApi() == Metal);
#endif

    const GraphicsBackendAvailability none{};
    assert(resolveGraphicsApi(Software, none) == Software);
    assert(resolveGraphicsApi(Auto, none) == Software);
    assert(resolveGraphicsApi(Metal, none) == Software);
    assert(resolveGraphicsApi(MoltenVK, none) == Software);
    assert(resolveGraphicsApi(KosmicKrisp, none) == Software);
    assert(resolveGraphicsApi(D3D12, none) == Software);
    assert(resolveGraphicsApi(Vulkan, none) == Software);

#if defined(_WIN32)
    GraphicsBackendAvailability available{};
    available.win32Vulkan = true;
    assert(resolveGraphicsApi(Auto, available) == Vulkan);
    assert(resolveGraphicsApi(Vulkan, available) == Vulkan);
    assert(resolveGraphicsApi(D3D12, available) == Software);

    available.d3d12 = true;
    assert(resolveGraphicsApi(Auto, available) == D3D12);
    assert(resolveGraphicsApi(D3D12, available) == D3D12);
    // An explicit hardware request never silently switches hardware APIs.
    assert(resolveGraphicsApi(Vulkan, available) == Vulkan);
#else
    GraphicsBackendAvailability available{};
    available.kosmicVulkan = true;
    assert(resolveGraphicsApi(Auto, available) == KosmicKrisp);
    assert(resolveGraphicsApi(KosmicKrisp, available) == KosmicKrisp);
    assert(resolveGraphicsApi(MoltenVK, available) == Software);

    available.moltenVulkan = true;
    assert(resolveGraphicsApi(Auto, available) == MoltenVK);
    available.metal = true;
    assert(resolveGraphicsApi(Auto, available) == Metal);
    assert(resolveGraphicsApi(Metal, available) == Metal);
    assert(resolveGraphicsApi(MoltenVK, available) == MoltenVK);
#endif

    std::cout << "Graphics API parse/default/fallback selection deterministic\n";
    return 0;
}
