// Native Windows Vulkan viewport presenter. The loader (vulkan-1.dll) is
// opened at runtime; only Vulkan headers and HWND/HMODULE are compile-time
// dependencies. All Windows and Vulkan types stay inside this file.
#include "n64forge/video/viewport_presenter.hpp"

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace n64forge::video {
namespace {

constexpr VkImageSubresourceRange ColorRange{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
constexpr VkImageSubresourceLayers ColorLayer{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};

inline bool failed(VkResult result) noexcept { return result < 0; }

#define INSTANCE_FUNCTIONS(X) \
    X(DestroyInstance) X(CreateWin32SurfaceKHR) X(DestroySurfaceKHR) \
    X(EnumerateInstanceExtensionProperties) X(EnumeratePhysicalDevices) \
    X(EnumerateDeviceExtensionProperties) X(GetPhysicalDeviceQueueFamilyProperties) \
    X(GetPhysicalDeviceSurfaceSupportKHR) X(GetPhysicalDeviceSurfaceCapabilitiesKHR) \
    X(GetPhysicalDeviceSurfaceFormatsKHR) X(GetPhysicalDeviceFormatProperties) \
    X(GetPhysicalDeviceProperties) X(GetPhysicalDeviceMemoryProperties) \
    X(CreateDevice) X(GetDeviceProcAddr)
#define DEVICE_FUNCTIONS(X) \
    X(DestroyDevice) X(GetDeviceQueue) X(DeviceWaitIdle) \
    X(CreateSwapchainKHR) X(DestroySwapchainKHR) X(GetSwapchainImagesKHR) \
    X(AcquireNextImageKHR) X(QueuePresentKHR) X(QueueSubmit) \
    X(CreateCommandPool) X(DestroyCommandPool) X(AllocateCommandBuffers) X(ResetCommandBuffer) \
    X(BeginCommandBuffer) X(EndCommandBuffer) X(CreateSemaphore) X(DestroySemaphore) \
    X(CreateFence) X(DestroyFence) X(GetFenceStatus) X(ResetFences) \
    X(CreateBuffer) X(DestroyBuffer) X(GetBufferMemoryRequirements) X(BindBufferMemory) \
    X(CreateImage) X(DestroyImage) X(GetImageMemoryRequirements) X(BindImageMemory) \
    X(AllocateMemory) X(FreeMemory) X(MapMemory) X(UnmapMemory) \
    X(CmdPipelineBarrier) X(CmdCopyBufferToImage) X(CmdClearColorImage) X(CmdBlitImage)

#define DECLARE(name) PFN_vk##name name = nullptr;
INSTANCE_FUNCTIONS(DECLARE)
DEVICE_FUNCTIONS(DECLARE)
#undef DECLARE

VkInstance createProbeInstance(PFN_vkGetInstanceProcAddr getProc) {
    if (!getProc) return VK_NULL_HANDLE;
    const auto create = reinterpret_cast<PFN_vkCreateInstance>(getProc(nullptr, "vkCreateInstance"));
    const auto enumerate = reinterpret_cast<PFN_vkEnumerateInstanceExtensionProperties>(
        getProc(nullptr, "vkEnumerateInstanceExtensionProperties"));
    if (!create || !enumerate) return VK_NULL_HANDLE;
    u32 count = 0;
    if (failed(enumerate(nullptr, &count, nullptr))) return VK_NULL_HANDLE;
    std::vector<VkExtensionProperties> extensions(count);
    if (count && failed(enumerate(nullptr, &count, extensions.data()))) return VK_NULL_HANDLE;
    const auto has = [&](const char* name) {
        return std::any_of(extensions.begin(), extensions.end(), [&](const auto& extension) {
            return std::strcmp(extension.extensionName, name) == 0;
        });
    };
    if (!has(VK_KHR_SURFACE_EXTENSION_NAME) || !has(VK_KHR_WIN32_SURFACE_EXTENSION_NAME))
        return VK_NULL_HANDLE;
    const char* enabled[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME};
    VkApplicationInfo application{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    application.pApplicationName = "N64Forge Studio";
    application.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    info.pApplicationInfo = &application;
    info.enabledExtensionCount = 2;
    info.ppEnabledExtensionNames = enabled;
    VkInstance result = VK_NULL_HANDLE;
    return create(&info, nullptr, &result) == VK_SUCCESS ? result : VK_NULL_HANDLE;
}

class Win32VulkanViewport final : public ViewportPresenter {
public:
    GraphicsApi api() const noexcept override { return GraphicsApi::Vulkan; }

    ~Win32VulkanViewport() override {
        if (device_) {
            if (DeviceWaitIdle) DeviceWaitIdle(device_);
            releaseSwapchain();
            releaseSource();
            if (acquired_) DestroySemaphore(device_, acquired_, nullptr);
            if (fence_) DestroyFence(device_, fence_, nullptr);
            if (pool_) DestroyCommandPool(device_, pool_, nullptr);
            if (DestroyDevice) DestroyDevice(device_, nullptr);
        }
        if (surface_) DestroySurfaceKHR(instance_, surface_, nullptr);
        if (instance_ && DestroyInstance) DestroyInstance(instance_, nullptr);
        if (library_) FreeLibrary(library_);
    }

    bool attach(void* nativeView) override {
        HWND hwnd = reinterpret_cast<HWND>(nativeView);
        if (!hwnd || !IsWindow(hwnd)) return false;
        library_ = LoadLibraryA("vulkan-1.dll");
        if (!library_) {
            std::fprintf(stderr, "Vulkan viewport: vulkan-1.dll is not available\n");
            return false;
        }
        getInstance_ = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
            GetProcAddress(library_, "vkGetInstanceProcAddr"));
        instance_ = createProbeInstance(getInstance_);
        if (!instance_) {
            std::fprintf(stderr, "Vulkan viewport: no Win32 surface instance available\n");
            return false;
        }
#define LOAD(name) \
    name = reinterpret_cast<PFN_vk##name>(getInstance_(instance_, "vk" #name)); \
    if (!name) return false;
        INSTANCE_FUNCTIONS(LOAD)
#undef LOAD

        VkWin32SurfaceCreateInfoKHR surface{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
        surface.hinstance = GetModuleHandle(nullptr);
        surface.hwnd = hwnd;
        if (failed(CreateWin32SurfaceKHR(instance_, &surface, nullptr, &surface_)) ||
            !initializeDevice()) {
            std::fprintf(stderr, "Vulkan viewport: no compatible hardware device\n");
            return false;
        }
        hwnd_ = hwnd;
        return true;
    }

    bool present(std::span<const u32> pixels, u32 width, u32 height, u64 revision, float blend,
                 bool smooth, const PresentationGeometry& geometry) override {
        if (!width || !height || pixels.size() != size_t(width) * height) return false;
        if (revision_ != revision || width_ != width || height_ != height || current_.empty()) {
            nextFrame(width, height, revision);
            std::copy(pixels.begin(), pixels.end(), current_.begin());
        }
        return draw(blend, smooth, geometry);
    }

    bool presentPacked(const PackedPresentationFrame& frame, float blend, bool smooth,
                       const PresentationGeometry& geometry) override {
        const u32 bytes = frame.bytesPerPixel;
        if (!frame.backing || !frame.width || !frame.height || (bytes != 2 && bytes != 4) ||
            frame.pitch < u64{frame.width} * bytes) return false;
        if (revision_ != frame.revision || width_ != frame.width || height_ != frame.height ||
            current_.empty()) {
            nextFrame(frame.width, frame.height, frame.revision);
            const auto* base = reinterpret_cast<const u8*>(frame.backing->data());
            constexpr auto expand = [](u32 value) { return (value << 3) | (value >> 2); };
            for (u32 y = 0; y < height_; ++y) {
                const auto* row = base + size_t(y) * frame.pitch;
                auto* out = current_.data() + size_t(y) * width_;
                if (bytes == 4) {
                    std::memcpy(out, row, size_t(width_) * 4);
                } else {
                    for (u32 x = 0; x < width_; ++x) {
                        const u32 packed = (u32{row[x * 2]} << 8) | row[x * 2 + 1];
                        const u32 r = expand(packed >> 11);
                        const u32 g = expand((packed >> 6) & 31);
                        const u32 b = expand((packed >> 1) & 31);
                        out[x] = r | (g << 8) | (b << 16) | 0xFF000000u;
                    }
                }
            }
        }
        return draw(blend, smooth, geometry);
    }

    void discardFrames() override {
        current_.clear();
        previous_.clear();
        revision_ = 0;
        presented_ = false;
    }

    [[nodiscard]] bool busy() const override {
        return !broken_ && inFlight_ && GetFenceStatus(device_, fence_) == VK_NOT_READY;
    }
    [[nodiscard]] bool needsRetry() const override { return retry_; }
    [[nodiscard]] bool hasPresentedFrame() const override { return presented_; }
    [[nodiscard]] std::string deviceDescription() const override { return deviceDescription_; }
    [[nodiscard]] double gpuMilliseconds() const override { return 0.0; } // No timestamp dependency.

private:
    bool initializeDevice() {
        u32 count = 0;
        if (failed(EnumeratePhysicalDevices(instance_, &count, nullptr)) || !count) return false;
        std::vector<VkPhysicalDevice> devices(count);
        if (failed(EnumeratePhysicalDevices(instance_, &count, devices.data()))) return false;
        for (auto physical : devices) {
            VkPhysicalDeviceProperties properties{};
            GetPhysicalDeviceProperties(physical, &properties);
            if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU) continue;
            u32 extensionCount = 0;
            if (failed(EnumerateDeviceExtensionProperties(physical, nullptr, &extensionCount, nullptr)))
                continue;
            std::vector<VkExtensionProperties> extensions(extensionCount);
            if (failed(EnumerateDeviceExtensionProperties(physical, nullptr, &extensionCount,
                                                          extensions.data()))) continue;
            const auto hasExtension = [&](const char* name) {
                return std::any_of(extensions.begin(), extensions.end(), [&](const auto& extension) {
                    return std::strcmp(extension.extensionName, name) == 0;
                });
            };
            if (!hasExtension(VK_KHR_SWAPCHAIN_EXTENSION_NAME)) continue;
            u32 families = 0;
            GetPhysicalDeviceQueueFamilyProperties(physical, &families, nullptr);
            std::vector<VkQueueFamilyProperties> queueProperties(families);
            GetPhysicalDeviceQueueFamilyProperties(physical, &families, queueProperties.data());
            for (u32 family = 0; family < families; ++family) {
                VkBool32 canPresent = VK_FALSE;
                if (!(queueProperties[family].queueFlags & VK_QUEUE_GRAPHICS_BIT) ||
                    failed(GetPhysicalDeviceSurfaceSupportKHR(physical, family, surface_, &canPresent)) ||
                    !canPresent) continue;
                u32 formats = 0;
                if (failed(GetPhysicalDeviceSurfaceFormatsKHR(physical, surface_, &formats, nullptr)) ||
                    !formats) continue;
                std::vector<VkSurfaceFormatKHR> surfaceFormats(formats);
                if (failed(GetPhysicalDeviceSurfaceFormatsKHR(physical, surface_, &formats,
                                                              surfaceFormats.data()))) continue;
                const auto chosen = std::find_if(surfaceFormats.begin(), surfaceFormats.end(),
                                                 [&](const auto& format) {
                    if (format.format != VK_FORMAT_R8G8B8A8_UNORM &&
                        format.format != VK_FORMAT_B8G8R8A8_UNORM) return false;
                    VkFormatProperties available{};
                    GetPhysicalDeviceFormatProperties(physical, format.format, &available);
                    return (available.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT) != 0;
                });
                if (chosen == surfaceFormats.end()) continue;
                VkFormatProperties sourceProperties{};
                GetPhysicalDeviceFormatProperties(physical, chosen->format, &sourceProperties);
                if (!(sourceProperties.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT)) continue;

                const char* deviceExtensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
                constexpr float priority = 1.0f;
                VkDeviceQueueCreateInfo queue{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
                queue.queueFamilyIndex = family;
                queue.queueCount = 1;
                queue.pQueuePriorities = &priority;
                VkDeviceCreateInfo info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
                info.queueCreateInfoCount = 1;
                info.pQueueCreateInfos = &queue;
                info.enabledExtensionCount = 1;
                info.ppEnabledExtensionNames = deviceExtensions;
                if (failed(CreateDevice(physical, &info, nullptr, &device_))) continue;
#define LOAD(name) \
    name = reinterpret_cast<PFN_vk##name>(GetDeviceProcAddr(device_, "vk" #name)); \
    if (!name) return false;
                DEVICE_FUNCTIONS(LOAD)
#undef LOAD
                GetDeviceQueue(device_, family, 0, &queue_);
                GetPhysicalDeviceMemoryProperties(physical, &memory_);
                physical_ = physical;
                family_ = family;
                swapchainFormat_ = chosen->format;
                swapchainColorSpace_ = chosen->colorSpace;
                sourceFormat_ = chosen->format;
                redBlueSwapped_ = chosen->format == VK_FORMAT_B8G8R8A8_UNORM;
                linear_ = (sourceProperties.optimalTilingFeatures &
                           VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) != 0;
                deviceDescription_ = properties.deviceName;
                if (!createCommandResources()) return false;
                return true;
            }
        }
        return false;
    }

    bool createCommandResources() {
        VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pool.queueFamilyIndex = family_;
        if (failed(CreateCommandPool(device_, &pool, nullptr, &pool_))) return false;
        VkCommandBufferAllocateInfo command{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        command.commandPool = pool_;
        command.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        command.commandBufferCount = 1;
        VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        return !failed(AllocateCommandBuffers(device_, &command, &command_)) &&
               !failed(CreateSemaphore(device_, &semaphore, nullptr, &acquired_)) &&
               !failed(CreateFence(device_, &fence, nullptr, &fence_));
    }

    void releaseSwapchain() {
        for (auto signal : presentedSignals_)
            if (signal) DestroySemaphore(device_, signal, nullptr);
        presentedSignals_.clear();
        images_.clear();
        if (swapchain_) DestroySwapchainKHR(device_, swapchain_, nullptr);
        swapchain_ = VK_NULL_HANDLE;
    }

    bool createSwapchain(const PresentationGeometry& geometry) {
        VkSurfaceCapabilitiesKHR capabilities{};
        u32 count = 0;
        if (failed(GetPhysicalDeviceSurfaceCapabilitiesKHR(physical_, surface_, &capabilities)) ||
            !(capabilities.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT) ||
            !capabilities.supportedCompositeAlpha) return false;
        extent_ = capabilities.currentExtent.width != std::numeric_limits<u32>::max()
            ? capabilities.currentExtent
            : VkExtent2D{std::clamp(geometry.width, capabilities.minImageExtent.width,
                                    capabilities.maxImageExtent.width),
                         std::clamp(geometry.height, capabilities.minImageExtent.height,
                                    capabilities.maxImageExtent.height)};
        if (!extent_.width || !extent_.height) return false;
        const auto alphaFlags = capabilities.supportedCompositeAlpha;
        auto alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        if (!(alphaFlags & alpha)) alpha = static_cast<VkCompositeAlphaFlagBitsKHR>(
            alphaFlags & (0u - alphaFlags));
        VkSwapchainCreateInfoKHR info{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
        info.surface = surface_;
        info.minImageCount = std::max(capabilities.minImageCount, 3u);
        if (capabilities.maxImageCount) info.minImageCount = std::min(info.minImageCount, capabilities.maxImageCount);
        info.imageFormat = swapchainFormat_;
        info.imageColorSpace = swapchainColorSpace_;
        info.imageExtent = extent_;
        info.imageArrayLayers = 1;
        info.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        info.preTransform = capabilities.currentTransform;
        info.compositeAlpha = alpha;
        info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
        info.clipped = VK_TRUE;
        if (failed(CreateSwapchainKHR(device_, &info, nullptr, &swapchain_))) return false;
        if (failed(GetSwapchainImagesKHR(device_, swapchain_, &count, nullptr)) || !count) return false;
        images_.resize(count);
        if (failed(GetSwapchainImagesKHR(device_, swapchain_, &count, images_.data()))) return false;
        presentedSignals_.resize(count);
        VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        for (auto& signal : presentedSignals_)
            if (failed(CreateSemaphore(device_, &semaphore, nullptr, &signal))) return false;
        return true;
    }

    void releaseSource() {
        if (mapped_) UnmapMemory(device_, uploadMemory_);
        if (upload_) DestroyBuffer(device_, upload_, nullptr);
        if (source_) DestroyImage(device_, source_, nullptr);
        if (uploadMemory_) FreeMemory(device_, uploadMemory_, nullptr);
        if (sourceMemory_) FreeMemory(device_, sourceMemory_, nullptr);
        mapped_ = nullptr;
        upload_ = VK_NULL_HANDLE;
        source_ = VK_NULL_HANDLE;
        uploadMemory_ = sourceMemory_ = VK_NULL_HANDLE;
    }

    bool allocate(const VkMemoryRequirements& requirements, VkMemoryPropertyFlags flags,
                  VkDeviceMemory& memory) {
        for (u32 index = 0; index < memory_.memoryTypeCount; ++index) {
            if (!(requirements.memoryTypeBits & (1u << index)) ||
                (memory_.memoryTypes[index].propertyFlags & flags) != flags) continue;
            VkMemoryAllocateInfo info{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            info.allocationSize = requirements.size;
            info.memoryTypeIndex = index;
            return !failed(AllocateMemory(device_, &info, nullptr, &memory));
        }
        return false;
    }

    bool createSource() {
        VkBufferCreateInfo buffer{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        buffer.size = VkDeviceSize{width_} * height_ * 4;
        buffer.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        if (failed(CreateBuffer(device_, &buffer, nullptr, &upload_))) return false;
        VkMemoryRequirements requirements{};
        GetBufferMemoryRequirements(device_, upload_, &requirements);
        if (!allocate(requirements, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, uploadMemory_) ||
            failed(BindBufferMemory(device_, upload_, uploadMemory_, 0)) ||
            failed(MapMemory(device_, uploadMemory_, 0, VK_WHOLE_SIZE, 0, &mapped_))) return false;
        VkImageCreateInfo image{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        image.imageType = VK_IMAGE_TYPE_2D;
        image.format = sourceFormat_;
        image.extent = {width_, height_, 1};
        image.mipLevels = image.arrayLayers = 1;
        image.samples = VK_SAMPLE_COUNT_1_BIT;
        image.tiling = VK_IMAGE_TILING_OPTIMAL;
        image.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        if (failed(CreateImage(device_, &image, nullptr, &source_))) return false;
        GetImageMemoryRequirements(device_, source_, &requirements);
        if (!allocate(requirements, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, sourceMemory_) ||
            failed(BindImageMemory(device_, source_, sourceMemory_, 0))) return false;
        sourceWidth_ = width_;
        sourceHeight_ = height_;
        return true;
    }

    void nextFrame(u32 width, u32 height, u64 revision) {
        previous_ = width == width_ && height == height_ ? current_ : std::vector<u32>{};
        current_.resize(size_t(width) * height);
        width_ = width;
        height_ = height;
        revision_ = revision;
    }

    void compose(float blend) {
        if (!mapped_) return;
        auto* output = static_cast<u8*>(mapped_);
        const bool mix = blend < 1.0f && previous_.size() == current_.size();
        const u32 weight = static_cast<u32>(std::clamp(blend, 0.0f, 1.0f) * 256.0f);
        for (size_t i = 0; i < current_.size(); ++i) {
            const auto* source = reinterpret_cast<const u8*>(&current_[i]);
            u8 channels[3];
            if (mix) {
                const auto* older = reinterpret_cast<const u8*>(&previous_[i]);
                for (u32 channel = 0; channel < 3; ++channel)
                    channels[channel] = static_cast<u8>(
                        ((u32{source[channel]} * weight + u32{older[channel]} * (256u - weight) + 128u) >> 8));
            } else {
                std::memcpy(channels, source, 3);
            }
            auto* target = output + i * 4;
            if (redBlueSwapped_) {
                target[0] = channels[2];
                target[1] = channels[1];
                target[2] = channels[0];
            } else {
                std::memcpy(target, channels, 3);
            }
            target[3] = 255;
        }
    }

    bool draw(float blend, bool smooth, const PresentationGeometry& geometry) {
        if (broken_ || !hwnd_ || !IsWindow(hwnd_) || !geometry.width || !geometry.height ||
            !geometry.imageWidth || !geometry.imageHeight) return false;
        const auto fail = [this] { broken_ = true; return false; };
        if (inFlight_) {
            const auto status = GetFenceStatus(device_, fence_);
            if (status == VK_NOT_READY) {
                retry_ = true;
                return true;
            }
            if (failed(status)) return fail();
            inFlight_ = false;
        }
        retry_ = false;
        if (!swapchain_ || size_.width != geometry.width || size_.height != geometry.height) {
            if (failed(DeviceWaitIdle(device_))) return fail();
            releaseSwapchain();
            if (!createSwapchain(geometry)) return fail();
            size_ = {geometry.width, geometry.height};
        }
        if (!source_ || width_ != sourceWidth_ || height_ != sourceHeight_) {
            releaseSource();
            if (!createSource()) return fail();
        }
        u32 index = 0;
        const auto acquire = AcquireNextImageKHR(device_, swapchain_, 0, acquired_, VK_NULL_HANDLE, &index);
        if (acquire == VK_NOT_READY || acquire == VK_TIMEOUT || acquire == VK_ERROR_OUT_OF_DATE_KHR) {
            if (acquire == VK_ERROR_OUT_OF_DATE_KHR && !failed(DeviceWaitIdle(device_))) {
                inFlight_ = false;
                releaseSwapchain();
                size_ = {};
            }
            retry_ = true;
            return true;
        }
        if (acquire != VK_SUCCESS && acquire != VK_SUBOPTIMAL_KHR) return fail();
        compose(blend);
        if (!encode(index, smooth, geometry) || failed(ResetFences(device_, 1, &fence_))) return fail();
        const auto signal = presentedSignals_[index];
        const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.waitSemaphoreCount = submit.signalSemaphoreCount = submit.commandBufferCount = 1;
        submit.pWaitSemaphores = &acquired_;
        submit.pWaitDstStageMask = &waitStage;
        submit.pSignalSemaphores = &signal;
        submit.pCommandBuffers = &command_;
        if (failed(QueueSubmit(queue_, 1, &submit, fence_))) return fail();
        inFlight_ = true;
        VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        present.waitSemaphoreCount = present.swapchainCount = 1;
        present.pWaitSemaphores = &signal;
        present.pSwapchains = &swapchain_;
        present.pImageIndices = &index;
        const auto result = QueuePresentKHR(queue_, &present);
        if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
            if (failed(DeviceWaitIdle(device_))) return fail();
            inFlight_ = false;
            releaseSwapchain();
            size_ = {};
            retry_ = true;
            return true;
        }
        if (failed(result)) return fail();
        presented_ = true;
        return true;
    }

    bool encode(u32 index, bool smooth, const PresentationGeometry& geometry) {
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (failed(ResetCommandBuffer(command_, 0)) || failed(BeginCommandBuffer(command_, &begin)))
            return false;
        constexpr auto Transfer = VK_PIPELINE_STAGE_TRANSFER_BIT;
        const auto barrier = [&](VkImage image, VkImageLayout from, VkImageLayout to,
                                 VkAccessFlags before, VkAccessFlags after,
                                 VkPipelineStageFlags sourceStage, VkPipelineStageFlags targetStage) {
            VkImageMemoryBarrier value{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            value.srcAccessMask = before;
            value.dstAccessMask = after;
            value.oldLayout = from;
            value.newLayout = to;
            value.srcQueueFamilyIndex = value.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            value.image = image;
            value.subresourceRange = ColorRange;
            CmdPipelineBarrier(command_, sourceStage, targetStage, 0, 0, nullptr, 0, nullptr, 1, &value);
        };
        constexpr auto Destination = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        constexpr auto Source = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        constexpr auto Write = VK_ACCESS_TRANSFER_WRITE_BIT;
        constexpr auto Read = VK_ACCESS_TRANSFER_READ_BIT;
        const auto target = images_[index];
        barrier(source_, VK_IMAGE_LAYOUT_UNDEFINED, Destination, 0, Write,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, Transfer);
        VkBufferImageCopy copy{};
        copy.imageSubresource = ColorLayer;
        copy.imageExtent = {width_, height_, 1};
        CmdCopyBufferToImage(command_, upload_, source_, Destination, 1, &copy);
        barrier(source_, Destination, Source, Write, Read, Transfer, Transfer);
        barrier(target, VK_IMAGE_LAYOUT_UNDEFINED, Destination, 0, Write,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, Transfer);
        const VkClearColorValue black{{0.0f, 0.0f, 0.0f, 1.0f}};
        CmdClearColorImage(command_, target, Destination, &black, 1, &ColorRange);
        const auto scaleX = [&](u32 value) {
            return static_cast<s32>(u64{value} * extent_.width / geometry.width);
        };
        const auto scaleY = [&](u32 value) {
            return static_cast<s32>(u64{value} * extent_.height / geometry.height);
        };
        VkImageBlit blit{};
        blit.srcSubresource = blit.dstSubresource = ColorLayer;
        blit.srcOffsets[1] = {static_cast<s32>(width_), static_cast<s32>(height_), 1};
        blit.dstOffsets[0] = {scaleX(geometry.x), scaleY(geometry.y), 0};
        blit.dstOffsets[1] = {scaleX(geometry.x + geometry.imageWidth),
                              scaleY(geometry.y + geometry.imageHeight), 1};
        if (blit.dstOffsets[0].x < blit.dstOffsets[1].x &&
            blit.dstOffsets[0].y < blit.dstOffsets[1].y) {
            CmdBlitImage(command_, source_, Source, target, Destination, 1, &blit,
                         smooth && linear_ ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
        }
        barrier(target, Destination, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, Write, 0, Transfer,
                VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
        return !failed(EndCommandBuffer(command_));
    }

#define DECLARE(name) PFN_vk##name name = nullptr;
    INSTANCE_FUNCTIONS(DECLARE)
    DEVICE_FUNCTIONS(DECLARE)
#undef DECLARE
    HMODULE library_ = nullptr;
    HWND hwnd_ = nullptr;
    PFN_vkGetInstanceProcAddr getInstance_ = nullptr;
    VkInstance instance_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    u32 family_ = 0;
    VkPhysicalDeviceMemoryProperties memory_{};
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkExtent2D extent_{}, size_{};
    VkFormat swapchainFormat_ = VK_FORMAT_R8G8B8A8_UNORM;
    VkColorSpaceKHR swapchainColorSpace_ = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    VkFormat sourceFormat_ = VK_FORMAT_R8G8B8A8_UNORM;
    std::vector<VkImage> images_;
    std::vector<VkSemaphore> presentedSignals_;
    VkCommandPool pool_ = VK_NULL_HANDLE;
    VkCommandBuffer command_ = VK_NULL_HANDLE;
    VkFence fence_ = VK_NULL_HANDLE;
    VkSemaphore acquired_ = VK_NULL_HANDLE;
    VkBuffer upload_ = VK_NULL_HANDLE;
    VkImage source_ = VK_NULL_HANDLE;
    VkDeviceMemory uploadMemory_ = VK_NULL_HANDLE, sourceMemory_ = VK_NULL_HANDLE;
    void* mapped_ = nullptr;
    std::vector<u32> current_, previous_;
    std::string deviceDescription_;
    u32 width_ = 0, height_ = 0, sourceWidth_ = 0, sourceHeight_ = 0;
    u64 revision_ = 0;
    bool linear_ = false, broken_ = false, inFlight_ = false, retry_ = false;
    bool presented_ = false, redBlueSwapped_ = false;
};

} // namespace

std::unique_ptr<ViewportPresenter> createWin32VulkanViewport() {
    return std::make_unique<Win32VulkanViewport>();
}

bool win32VulkanDriverDetected(std::string* detail) {
    HMODULE library = LoadLibraryA("vulkan-1.dll");
    if (!library) return false;
    const auto getInstance = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
        GetProcAddress(library, "vkGetInstanceProcAddr"));
    const VkInstance instance = createProbeInstance(getInstance);
    bool found = false;
    if (instance) {
        const auto enumerate = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(
            getInstance(instance, "vkEnumeratePhysicalDevices"));
        const auto properties = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(
            getInstance(instance, "vkGetPhysicalDeviceProperties"));
        u32 count = 0;
        if (enumerate && properties && !failed(enumerate(instance, &count, nullptr)) && count) {
            std::vector<VkPhysicalDevice> devices(count);
            if (!failed(enumerate(instance, &count, devices.data()))) {
                for (auto device : devices) {
                    VkPhysicalDeviceProperties description{};
                    properties(device, &description);
                    if (description.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU) continue;
                    found = true;
                    if (detail) *detail = std::string("Vulkan hardware device: ") + description.deviceName;
                    break;
                }
            }
        }
        const auto destroy = reinterpret_cast<PFN_vkDestroyInstance>(
            getInstance(instance, "vkDestroyInstance"));
        if (destroy) destroy(instance, nullptr);
    }
    FreeLibrary(library);
    return found;
}

#undef INSTANCE_FUNCTIONS
#undef DEVICE_FUNCTIONS
} // namespace n64forge::video
