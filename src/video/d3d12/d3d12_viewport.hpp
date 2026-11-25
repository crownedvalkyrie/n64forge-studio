#pragma once

// Renderer-neutral factory/probe declarations. Windows SDK headers are kept
// exclusively in d3d12_viewport.cpp; nothing here leaks a COM or HWND type.

#include "n64forge/video/viewport_presenter.hpp"

#include <memory>
#include <string>

namespace n64forge::video {

[[nodiscard]] std::unique_ptr<ViewportPresenter> createD3D12Viewport();

// Nonvisual probe used by Graphics API = Auto. It only asks D3D12 whether a
// device can be created; swapchain creation is still validated by attach().
[[nodiscard]] bool d3d12DeviceAvailable(std::string* description = nullptr);

} // namespace n64forge::video
