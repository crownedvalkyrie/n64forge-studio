#include "n64forge/video/viewport_presenter.hpp"

#import <AppKit/AppKit.h>
#import <QuartzCore/CAMetalLayer.h>
#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_METAL_EXT
#include <vulkan/vulkan.h>
#include <dlfcn.h>

#include <algorithm>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace n64forge::video {
namespace {
namespace fs = std::filesystem;
constexpr VkFormat SourceFormat = VK_FORMAT_R8G8B8A8_UNORM;
constexpr VkImageSubresourceRange ColorRange{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
constexpr VkImageSubresourceLayers ColorLayer{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};

bool isFile(const fs::path& path) {
    std::error_code error;
    return fs::is_regular_file(path, error);
}

std::vector<fs::path> sdkPaths(const char* directory) {
    std::vector<fs::path> paths;
    if (const auto* sdk = std::getenv("VULKAN_SDK"); sdk && *sdk) paths.emplace_back(fs::path(sdk) / directory);
    for (const auto* root : {"/opt/homebrew", "/usr/local", "/opt/local"})
        paths.emplace_back(fs::path(root) / directory);
    return paths;
}

fs::path findManifest(GraphicsApi api) {
    const auto matches = [api](const fs::path& path) {
        auto name = path.filename().string();
        for (auto& c : name) if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
        return name.ends_with(".json") && isFile(path) &&
            name.find(api == GraphicsApi::MoltenVK ? "moltenvk" : "kosmickrisp") != name.npos;
    };
    for (const auto* variable : {"VK_DRIVER_FILES", "VK_ICD_FILENAMES", "VK_ADD_DRIVER_FILES"}) {
        if (const auto* value = std::getenv(variable)) {
            std::string_view entries(value);
            while (!entries.empty()) {
                const auto separator = entries.find(':');
                fs::path path(entries.substr(0, separator));
                if (matches(path)) return path;
                if (separator == entries.npos) break;
                entries.remove_prefix(separator + 1);
            }
        }
    }
    auto paths = sdkPaths("share/vulkan/icd.d");
    paths.emplace_back("/etc/vulkan/icd.d");
    if (const auto* home = std::getenv("HOME"))
        paths.emplace_back(fs::path(home) / "Library/Application Support/vulkan/icd.d");
    for (const auto& dir : paths) {
        std::error_code error;
        for (fs::directory_iterator it(dir, error), end; !error && it != end; it.increment(error))
            if (matches(it->path())) return it->path();
    }
    return {};
}

// The loader reads driver selection when creating the instance. Scope the
// override to that call; never change the user's launch environment permanently.
struct DriverSelection {
    explicit DriverSelection(const fs::path& path) {
        if (path.empty()) return;
        if (const auto* existing = std::getenv("VK_DRIVER_FILES")) old = existing;
        changed = setenv("VK_DRIVER_FILES", path.c_str(), 1) == 0;
        valid = changed;
    }
    ~DriverSelection() {
        if (changed) {
            if (old) setenv("VK_DRIVER_FILES", old->c_str(), 1);
            else unsetenv("VK_DRIVER_FILES");
        }
    }
    std::optional<std::string> old;
    bool changed = false;
    bool valid = true;
};

#define INSTANCE_FUNCTIONS(X) \
    X(DestroyInstance) X(CreateMetalSurfaceEXT) X(DestroySurfaceKHR) \
    X(EnumeratePhysicalDevices) X(EnumerateDeviceExtensionProperties) \
    X(GetPhysicalDeviceQueueFamilyProperties) X(GetPhysicalDeviceSurfaceSupportKHR) \
    X(GetPhysicalDeviceSurfaceCapabilitiesKHR) X(GetPhysicalDeviceSurfaceFormatsKHR) \
    X(GetPhysicalDeviceMemoryProperties) X(GetPhysicalDeviceFormatProperties) \
    X(GetPhysicalDeviceProperties) X(GetPhysicalDeviceProperties2) \
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

class VulkanViewport final : public ViewportPresenter {
public:
    explicit VulkanViewport(GraphicsApi api) : api_(api) {}
    GraphicsApi api() const noexcept override { return api_; }
    ~VulkanViewport() override {
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
        [view_ removeFromSuperview];
        if (library_) dlclose(library_);
    }

    bool attach(void* nativeView) override {
        NSView* parent = (__bridge NSView*)nativeView;
        if (!parent || !initializeInstance()) {
            std::fprintf(stderr, "Vulkan viewport (%s): could not load driver or create instance\n",
                         graphicsApiName(api_).data());
            return false;
        }
        layer_ = [CAMetalLayer layer];
        layer_.opaque = YES;
        layer_.maximumDrawableCount = 3;
        layer_.framebufferOnly = NO; // Vulkan transfers target the swapchain images.
        view_ = [[NSView alloc] initWithFrame:parent.bounds];
        view_.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
        view_.wantsLayer = YES;
        view_.layer = layer_;
        [parent addSubview:view_];

        VkMetalSurfaceCreateInfoEXT info{VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT};
        info.pLayer = layer_;
        if (CreateMetalSurfaceEXT(instance_, &info, nullptr, &surface_) != VK_SUCCESS || !initializeDevice()) {
            std::fprintf(stderr, "Vulkan viewport (%s): no compatible Metal surface / transfer queue\n",
                         graphicsApiName(api_).data());
            return false;
        }
        return true;
    }

    bool present(std::span<const u32> pixels, u32 width, u32 height, u64 revision, float blend,
                 bool smooth, const PresentationGeometry& geometry) override {
        if (!width || !height || pixels.size() != size_t(width) * height) return false;
        if (revision_ != revision || current_.empty()) {
            nextFrame(width, height, revision);
            std::copy(pixels.begin(), pixels.end(), current_.begin());
        }
        return draw(blend, smooth, geometry);
    }

    bool presentPacked(const PackedPresentationFrame& frame, float blend, bool smooth,
                       const PresentationGeometry& geometry) override {
        const auto bytes = frame.bytesPerPixel;
        if (!frame.backing || !frame.width || !frame.height || (bytes != 2 && bytes != 4) ||
            frame.pitch < u64{frame.width} * bytes) return false;
        if (revision_ != frame.revision || current_.empty()) {
            nextFrame(frame.width, frame.height, frame.revision);
            const auto* base = reinterpret_cast<const u8*>(frame.backing->data());
            constexpr auto expand = [](u32 v) { return (v << 3) | (v >> 2); };
            for (u32 y = 0; y < height_; ++y) {
                const auto* row = base + size_t(y) * frame.pitch;
                auto* out = current_.data() + size_t(y) * width_;
                if (bytes == 4) {
                    std::memcpy(out, row, size_t(width_) * 4);
                } else {
                    for (u32 x = 0; x < width_; ++x) {
                        const u32 packed = (u32{row[x * 2]} << 8) | row[x * 2 + 1];
                        out[x] = expand(packed >> 11) | (expand((packed >> 6) & 31) << 8) |
                                 (expand((packed >> 1) & 31) << 16);
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
    [[nodiscard]] bool hasPresentedFrame() const override { return presented_; }
    bool busy() const override { return !broken_ && inFlight_ && GetFenceStatus(device_, fence_) == VK_NOT_READY; }
    bool needsRetry() const override { return retry_; }
    std::string deviceDescription() const override { return deviceDescription_; }
    double gpuMilliseconds() const override { return 0; } // No timestamp-query dependency.

private:
    bool initializeInstance() {
        fs::path manifest;
        if (api_ == GraphicsApi::MoltenVK) {
            for (const auto& dir : sdkPaths("lib")) {
                if ((library_ = dlopen((dir / "libMoltenVK.dylib").c_str(), RTLD_NOW | RTLD_LOCAL))) break;
            }
            if (!library_) library_ = dlopen("libMoltenVK.dylib", RTLD_NOW | RTLD_LOCAL);
        }
        if (!library_) {
            manifest = findManifest(api_);
            // A driver can also be exposed by the loader's standard search
            // or a manifest with a nonstandard filename. Verify the physical
            // driver's properties rather than treating a filename as proof.
            verifyDriver_ = true;
            for (const auto& dir : sdkPaths("lib")) {
                if ((library_ = dlopen((dir / "libvulkan.1.dylib").c_str(), RTLD_NOW | RTLD_LOCAL))) break;
            }
            if (!library_) library_ = dlopen("libvulkan.1.dylib", RTLD_NOW | RTLD_LOCAL);
        }
        if (!library_) return false;
        getInstance_ = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(library_, "vkGetInstanceProcAddr"));
        if (!getInstance_) return false;
        const auto create = reinterpret_cast<PFN_vkCreateInstance>(getInstance_(nullptr, "vkCreateInstance"));
        const auto enumerate = reinterpret_cast<PFN_vkEnumerateInstanceExtensionProperties>(
            getInstance_(nullptr, "vkEnumerateInstanceExtensionProperties"));
        if (!create || !enumerate) return false;

        DriverSelection selection(manifest);
        if (!selection.valid) return false;
        u32 count = 0;
        if (enumerate(nullptr, &count, nullptr) != VK_SUCCESS) return false;
        std::vector<VkExtensionProperties> extensions(count);
        if (enumerate(nullptr, &count, extensions.data()) != VK_SUCCESS) return false;
        const auto has = [&](const char* name) {
            return std::any_of(extensions.begin(), extensions.end(), [&](const auto& e) {
                return std::strcmp(e.extensionName, name) == 0;
            });
        };
        if (!has(VK_KHR_SURFACE_EXTENSION_NAME) || !has(VK_EXT_METAL_SURFACE_EXTENSION_NAME)) return false;
        std::vector<const char*> enabled{VK_KHR_SURFACE_EXTENSION_NAME, VK_EXT_METAL_SURFACE_EXTENSION_NAME};
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.pApplicationName = "N64Forge Studio";
        app.apiVersion = VK_API_VERSION_1_1;
        VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        info.pApplicationInfo = &app;
        if (has(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)) {
            enabled.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
            info.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
        }
        info.enabledExtensionCount = static_cast<u32>(enabled.size());
        info.ppEnabledExtensionNames = enabled.data();
        if (create(&info, nullptr, &instance_) != VK_SUCCESS) return false;
#define LOAD(name) name = reinterpret_cast<PFN_vk##name>(getInstance_(instance_, "vk" #name)); if (!name) return false;
        INSTANCE_FUNCTIONS(LOAD)
#undef LOAD
        return true;
    }

    bool initializeDevice() {
        u32 count = 0;
        if (EnumeratePhysicalDevices(instance_, &count, nullptr) != VK_SUCCESS || !count) return false;
        std::vector<VkPhysicalDevice> devices(count);
        if (EnumeratePhysicalDevices(instance_, &count, devices.data()) != VK_SUCCESS) return false;
        for (auto physical : devices) {
            u32 n = 0;
            if (EnumerateDeviceExtensionProperties(physical, nullptr, &n, nullptr) != VK_SUCCESS) continue;
            std::vector<VkExtensionProperties> deviceExtensions(n);
            if (EnumerateDeviceExtensionProperties(physical, nullptr, &n, deviceExtensions.data()) != VK_SUCCESS)
                continue;
            const auto has = [&](const char* name) {
                return std::any_of(deviceExtensions.begin(), deviceExtensions.end(), [&](const auto& e) {
                    return std::strcmp(e.extensionName, name) == 0;
                });
            };
            if (!has(VK_KHR_SWAPCHAIN_EXTENSION_NAME)) continue;
            VkPhysicalDeviceProperties gpu{};
            GetPhysicalDeviceProperties(physical, &gpu);
            VkPhysicalDeviceDriverProperties driver{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
            if (gpu.apiVersion >= VK_API_VERSION_1_2 || has(VK_KHR_DRIVER_PROPERTIES_EXTENSION_NAME)) {
                VkPhysicalDeviceProperties2 details{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
                details.pNext = &driver;
                GetPhysicalDeviceProperties2(physical, &details);
            }
            if (verifyDriver_) {
                // Verify the selected driver even if an old loader ignores
                // VK_DRIVER_FILES or an implicit layer exposes another device.
                const std::string_view requested = api_ == GraphicsApi::MoltenVK ? "MoltenVK" : "KosmicKrisp";
                if (std::string_view(driver.driverName).find(requested) == std::string_view::npos) continue;
            }
            u32 families = 0;
            GetPhysicalDeviceQueueFamilyProperties(physical, &families, nullptr);
            std::vector<VkQueueFamilyProperties> properties(families);
            GetPhysicalDeviceQueueFamilyProperties(physical, &families, properties.data());
            for (u32 i = 0; i < families; ++i) {
                VkBool32 canPresent = false;
                if (!(properties[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) ||
                    GetPhysicalDeviceSurfaceSupportKHR(physical, i, surface_, &canPresent) != VK_SUCCESS ||
                    !canPresent) continue;
                VkFormatProperties format{};
                GetPhysicalDeviceFormatProperties(physical, SourceFormat, &format);
                if (!(format.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT)) continue;
                linear_ = (format.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) != 0;
                std::vector<const char*> extensions{VK_KHR_SWAPCHAIN_EXTENSION_NAME};
                if (has("VK_KHR_portability_subset")) extensions.push_back("VK_KHR_portability_subset");
                constexpr float priority = 1;
                VkDeviceQueueCreateInfo queue{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
                queue.queueFamilyIndex = i;
                queue.queueCount = 1;
                queue.pQueuePriorities = &priority;
                VkDeviceCreateInfo info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
                info.queueCreateInfoCount = 1;
                info.pQueueCreateInfos = &queue;
                info.enabledExtensionCount = static_cast<u32>(extensions.size());
                info.ppEnabledExtensionNames = extensions.data();
                if (CreateDevice(physical, &info, nullptr, &device_) != VK_SUCCESS) continue;
                deviceDescription_ = gpu.deviceName;
                if (driver.driverName[0]) deviceDescription_ += std::string(" · ") + driver.driverName;
                physical_ = physical;
                family_ = i;
                break;
            }
            if (physical_) break;
        }
        if (!device_) return false;
#define LOAD(name) name = reinterpret_cast<PFN_vk##name>(GetDeviceProcAddr(device_, "vk" #name)); if (!name) return false;
        DEVICE_FUNCTIONS(LOAD)
#undef LOAD
        GetDeviceQueue(device_, family_, 0, &queue_);
        GetPhysicalDeviceMemoryProperties(physical_, &memory_);
        VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pool.queueFamilyIndex = family_;
        if (CreateCommandPool(device_, &pool, nullptr, &pool_) != VK_SUCCESS) return false;
        VkCommandBufferAllocateInfo command{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        command.commandPool = pool_;
        command.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        command.commandBufferCount = 1;
        VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        return AllocateCommandBuffers(device_, &command, &command_) == VK_SUCCESS &&
               CreateSemaphore(device_, &semaphore, nullptr, &acquired_) == VK_SUCCESS &&
               CreateFence(device_, &fence, nullptr, &fence_) == VK_SUCCESS;
    }

    void releaseSwapchain() {
        for (const auto signal : rendered_) if (signal) DestroySemaphore(device_, signal, nullptr);
        rendered_.clear();
        images_.clear();
        if (swapchain_) DestroySwapchainKHR(device_, swapchain_, nullptr);
        swapchain_ = VK_NULL_HANDLE;
    }

    bool createSwapchain(const PresentationGeometry& geometry) {
        VkSurfaceCapabilitiesKHR caps{};
        u32 count = 0;
        if (GetPhysicalDeviceSurfaceCapabilitiesKHR(physical_, surface_, &caps) != VK_SUCCESS ||
            !(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT) ||
            GetPhysicalDeviceSurfaceFormatsKHR(physical_, surface_, &count, nullptr) != VK_SUCCESS ||
            !count) return false;
        std::vector<VkSurfaceFormatKHR> formats(count);
        if (GetPhysicalDeviceSurfaceFormatsKHR(physical_, surface_, &count, formats.data()) != VK_SUCCESS)
            return false;
        const auto chosen = std::find_if(formats.begin(), formats.end(), [&](const auto& candidate) {
            if (candidate.format != VK_FORMAT_B8G8R8A8_UNORM && candidate.format != SourceFormat) return false;
            VkFormatProperties p{};
            GetPhysicalDeviceFormatProperties(physical_, candidate.format, &p);
            return (p.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT) != 0;
        });
        if (chosen == formats.end() || !caps.supportedCompositeAlpha) return false;
        extent_ = caps.currentExtent.width != UINT32_MAX ? caps.currentExtent : VkExtent2D{
            std::clamp(geometry.width, caps.minImageExtent.width, caps.maxImageExtent.width),
            std::clamp(geometry.height, caps.minImageExtent.height, caps.maxImageExtent.height)};
        if (!extent_.width || !extent_.height) return false;
        VkSwapchainCreateInfoKHR info{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
        info.surface = surface_;
        info.minImageCount = std::max(caps.minImageCount, 3u);
        if (caps.maxImageCount) info.minImageCount = std::min(info.minImageCount, caps.maxImageCount);
        info.imageFormat = chosen->format;
        info.imageColorSpace = chosen->colorSpace;
        info.imageExtent = extent_;
        info.imageArrayLayers = 1;
        info.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        info.preTransform = caps.currentTransform;
        info.compositeAlpha = static_cast<VkCompositeAlphaFlagBitsKHR>(
            caps.supportedCompositeAlpha & (0u - caps.supportedCompositeAlpha));
        info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
        info.clipped = VK_TRUE;
        if (CreateSwapchainKHR(device_, &info, nullptr, &swapchain_) != VK_SUCCESS ||
            GetSwapchainImagesKHR(device_, swapchain_, &count, nullptr) != VK_SUCCESS || !count) return false;
        images_.resize(count);
        if (GetSwapchainImagesKHR(device_, swapchain_, &count, images_.data()) != VK_SUCCESS) return false;
        rendered_.resize(count);
        VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        for (auto& signal : rendered_) if (CreateSemaphore(device_, &semaphore, nullptr, &signal) != VK_SUCCESS)
            return false;
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

    bool allocate(const VkMemoryRequirements& req, VkMemoryPropertyFlags flags, VkDeviceMemory& memory) {
        for (u32 i = 0; i < memory_.memoryTypeCount; ++i) {
            if (!(req.memoryTypeBits & (1u << i)) || (memory_.memoryTypes[i].propertyFlags & flags) != flags)
                continue;
            VkMemoryAllocateInfo info{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            info.allocationSize = req.size;
            info.memoryTypeIndex = i;
            return AllocateMemory(device_, &info, nullptr, &memory) == VK_SUCCESS;
        }
        return false;
    }

    bool createSource() {
        VkBufferCreateInfo buffer{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        buffer.size = VkDeviceSize(width_) * height_ * 4;
        buffer.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        if (CreateBuffer(device_, &buffer, nullptr, &upload_) != VK_SUCCESS) return false;
        VkMemoryRequirements req;
        GetBufferMemoryRequirements(device_, upload_, &req);
        if (!allocate(req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, uploadMemory_) ||
            BindBufferMemory(device_, upload_, uploadMemory_, 0) != VK_SUCCESS ||
            MapMemory(device_, uploadMemory_, 0, VK_WHOLE_SIZE, 0, &mapped_) != VK_SUCCESS) return false;
        VkImageCreateInfo image{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        image.imageType = VK_IMAGE_TYPE_2D;
        image.format = SourceFormat;
        image.extent = {width_, height_, 1};
        image.mipLevels = image.arrayLayers = 1;
        image.samples = VK_SAMPLE_COUNT_1_BIT;
        image.tiling = VK_IMAGE_TILING_OPTIMAL;
        image.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        if (CreateImage(device_, &image, nullptr, &source_) != VK_SUCCESS) return false;
        GetImageMemoryRequirements(device_, source_, &req);
        if (!allocate(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, sourceMemory_) ||
            BindImageMemory(device_, source_, sourceMemory_, 0) != VK_SUCCESS) return false;
        sourceWidth_ = width_;
        sourceHeight_ = height_;
        return true;
    }

    void nextFrame(u32 width, u32 height, u64 revision) {
        if (width == width_ && height == height_) previous_.swap(current_);
        else previous_.clear();
        current_.resize(size_t(width) * height);
        width_ = width;
        height_ = height;
        revision_ = revision;
    }

    bool draw(float blend, bool smooth, const PresentationGeometry& geometry) {
        if (broken_ || !geometry.width || !geometry.height || !view_.window) return false;
        // Report terminal driver errors through the normal software fallback.
        const auto fail = [&] { broken_ = true; return false; };
        if (inFlight_) {
            const auto fence = GetFenceStatus(device_, fence_);
            if (fence == VK_NOT_READY) { retry_ = true; return true; }
            if (fence != VK_SUCCESS) return fail();
            inFlight_ = false;
        }
        retry_ = false;
        if (!swapchain_ || resized_ || size_.width != geometry.width || size_.height != geometry.height) {
            layer_.contentsScale = geometry.devicePixelRatio;
            layer_.drawableSize = CGSizeMake(geometry.width, geometry.height);
            if (DeviceWaitIdle(device_) != VK_SUCCESS) return fail();
            releaseSwapchain();
            if (!createSwapchain(geometry)) return fail();
            size_ = {geometry.width, geometry.height};
            resized_ = false;
        }
        if (!source_ || width_ != sourceWidth_ || height_ != sourceHeight_) {
            releaseSource();
            if (!createSource()) return fail();
        }
        u32 index = 0;
        const auto acquire = AcquireNextImageKHR(device_, swapchain_, 0, acquired_, VK_NULL_HANDLE, &index);
        if (acquire == VK_NOT_READY || acquire == VK_TIMEOUT || acquire == VK_ERROR_OUT_OF_DATE_KHR) {
            resized_ = acquire == VK_ERROR_OUT_OF_DATE_KHR;
            retry_ = true;
            return true;
        }
        if (acquire != VK_SUCCESS && acquire != VK_SUBOPTIMAL_KHR) return fail();
        auto* pixels = static_cast<u32*>(mapped_);
        if (blend < 1 && current_.size() == previous_.size()) {
            const auto weight = static_cast<u32>(std::clamp(blend, 0.f, 1.f) * 256);
            for (size_t i = 0; i < current_.size(); ++i) {
                const u32 a = current_[i], b = previous_[i];
                u32 value = 0xFF000000;
                for (unsigned shift : {0u, 8u, 16u})
                    value |= ((((a >> shift) & 255) * weight + ((b >> shift) & 255) * (256 - weight) + 128) >> 8)
                                << shift;
                pixels[i] = value;
            }
        } else {
            for (size_t i = 0; i < current_.size(); ++i) pixels[i] = current_[i] | 0xFF000000;
        }
        if (!encode(index, smooth, geometry) || ResetFences(device_, 1, &fence_) != VK_SUCCESS) return fail();
        const auto signal = rendered_[index]; // Reuse only when this image is acquired again.
        const VkPipelineStageFlags transfer = VK_PIPELINE_STAGE_TRANSFER_BIT;
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.waitSemaphoreCount = submit.signalSemaphoreCount = submit.commandBufferCount = 1;
        submit.pWaitSemaphores = &acquired_;
        submit.pWaitDstStageMask = &transfer;
        submit.pSignalSemaphores = &signal;
        submit.pCommandBuffers = &command_;
        if (QueueSubmit(queue_, 1, &submit, fence_) != VK_SUCCESS) return fail();
        inFlight_ = true;
        VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        present.waitSemaphoreCount = present.swapchainCount = 1;
        present.pWaitSemaphores = &signal;
        present.pSwapchains = &swapchain_;
        present.pImageIndices = &index;
        const auto result = QueuePresentKHR(queue_, &present);
        if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
            resized_ = retry_ = true;
            return true;
        }
        if (result != VK_SUCCESS) return fail();
        presented_ = true;
        return true;
    }

    bool encode(u32 index, bool smooth, const PresentationGeometry& geometry) {
        VkCommandBufferBeginInfo info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (ResetCommandBuffer(command_, 0) != VK_SUCCESS || BeginCommandBuffer(command_, &info) != VK_SUCCESS)
            return false;
        constexpr auto Transfer = VK_PIPELINE_STAGE_TRANSFER_BIT;
        const auto barrier = [&](VkImage image, VkImageLayout from, VkImageLayout to,
                                 VkAccessFlags before, VkAccessFlags after,
                                 VkPipelineStageFlags src, VkPipelineStageFlags dst) {
            VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            b.srcAccessMask = before; b.dstAccessMask = after;
            b.oldLayout = from; b.newLayout = to;
            b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = image; b.subresourceRange = ColorRange;
            CmdPipelineBarrier(command_, src, dst, 0, 0, nullptr, 0, nullptr, 1, &b);
        };
        constexpr auto Dst = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, Src = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        constexpr auto Write = VK_ACCESS_TRANSFER_WRITE_BIT, Read = VK_ACCESS_TRANSFER_READ_BIT;
        const auto target = images_[index];
        barrier(source_, VK_IMAGE_LAYOUT_UNDEFINED, Dst, 0, Write, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, Transfer);
        VkBufferImageCopy copy{};
        copy.imageSubresource = ColorLayer;
        copy.imageExtent = {width_, height_, 1};
        CmdCopyBufferToImage(command_, upload_, source_, Dst, 1, &copy);
        barrier(source_, Dst, Src, Write, Read, Transfer, Transfer);
        barrier(target, VK_IMAGE_LAYOUT_UNDEFINED, Dst, 0, Write, Transfer, Transfer);
        const VkClearColorValue black{{0, 0, 0, 1}};
        CmdClearColorImage(command_, target, Dst, &black, 1, &ColorRange);
        barrier(target, Dst, Dst, Write, Write, Transfer, Transfer);
        const auto x = [&](u32 v) { return static_cast<s32>(u64{v} * extent_.width / geometry.width); };
        const auto y = [&](u32 v) { return static_cast<s32>(u64{v} * extent_.height / geometry.height); };
        VkImageBlit blit{};
        blit.srcSubresource = blit.dstSubresource = ColorLayer;
        blit.srcOffsets[1] = {static_cast<s32>(width_), static_cast<s32>(height_), 1};
        blit.dstOffsets[0] = {x(geometry.x), y(geometry.y), 0};
        blit.dstOffsets[1] = {x(geometry.x + geometry.imageWidth), y(geometry.y + geometry.imageHeight), 1};
        if (blit.dstOffsets[0].x < blit.dstOffsets[1].x && blit.dstOffsets[0].y < blit.dstOffsets[1].y)
            CmdBlitImage(command_, source_, Src, target, Dst, 1, &blit,
                         smooth && linear_ ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
        barrier(target, Dst, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, Write, 0, Transfer,
                VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
        return EndCommandBuffer(command_) == VK_SUCCESS;
    }

#define DECLARE(name) PFN_vk##name name = nullptr;
    INSTANCE_FUNCTIONS(DECLARE)
    DEVICE_FUNCTIONS(DECLARE)
#undef DECLARE
    GraphicsApi api_;
    void* library_ = nullptr;
    NSView* view_ = nil;
    CAMetalLayer* layer_ = nil;
    PFN_vkGetInstanceProcAddr getInstance_ = nullptr;
    VkInstance instance_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    u32 family_ = 0;
    VkPhysicalDeviceMemoryProperties memory_{};
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkExtent2D extent_{}, size_{};
    std::vector<VkImage> images_;
    std::vector<VkSemaphore> rendered_;
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
    bool verifyDriver_ = false, linear_ = false, broken_ = false, inFlight_ = false;
    bool resized_ = false, retry_ = false, presented_ = false;
};

#undef INSTANCE_FUNCTIONS
#undef DEVICE_FUNCTIONS
} // namespace

std::unique_ptr<ViewportPresenter> createVulkanViewport(GraphicsApi api) {
    return std::make_unique<VulkanViewport>(api);
}

bool vulkanDriverDetected(GraphicsApi api) {
    if (api == GraphicsApi::MoltenVK) {
        for (const auto& dir : sdkPaths("lib"))
            if (isFile(dir / "libMoltenVK.dylib")) return true;
    }
    return !findManifest(api).empty();
}
} // namespace n64forge::video
