// Native Windows Direct3D 12 viewport presenter. Every Windows SDK type in
// this backend is file-local; the public presenter interface stays renderer
// neutral (no HWND, COM or DXGI type crosses it).
#include "d3d12_viewport.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <string>

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

namespace n64forge::video {
namespace {

using Microsoft::WRL::ComPtr;

inline bool failed(HRESULT result) { return FAILED(result); }

constexpr u32 BufferCount = 3;
constexpr u32 SlotCount = 3;
constexpr DXGI_FORMAT SwapChainFormat = DXGI_FORMAT_R8G8B8A8_UNORM;

// The fullscreen triangle and both pixel shaders mirror the macOS Metal
// presenter exactly. RGBA8888 is sampled by the fixed-function filter;
// RGBA5551 is unpacked in the shader so no CPU colour conversion is needed.
constexpr char VertexShader[] = R"(
struct VSOut { float4 position : SV_POSITION; float2 uv : TEXCOORD0; };
VSOut fullscreen(uint id : SV_VertexID) {
    float2 uv = float2(float((id << 1) & 2), float(id & 2));
    VSOut output;
    output.position = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.0, 1.0);
    output.uv = uv;
    return output;
}
)";

constexpr char PixelShader8888[] = R"(
cbuffer Parameters : register(b0) { float blend; uint smooth; uint pad0; uint pad1; };
Texture2D<float4> currentImage : register(t0);
Texture2D<float4> previousImage : register(t1);
SamplerState imageFilter : register(s0);
struct VSOut { float4 position : SV_POSITION; float2 uv : TEXCOORD0; };
float4 image(VSOut input) : SV_Target {
    float3 rgb = currentImage.Sample(imageFilter, input.uv).rgb;
    if (blend < 1.0) rgb = lerp(previousImage.Sample(imageFilter, input.uv).rgb, rgb, blend);
    return float4(rgb, 1.0);
}
)";

constexpr char PixelShader5551[] = R"(
cbuffer Parameters : register(b0) { float blend; uint smooth; uint pad0; uint pad1; };
Texture2D<uint2> currentImage : register(t0);
Texture2D<uint2> previousImage : register(t1);
SamplerState imageFilter : register(s0);
struct VSOut { float4 position : SV_POSITION; float2 uv : TEXCOORD0; };
float3 unpack5551(uint2 xy, Texture2D<uint2> image) {
    uint2 bytes = image.Load(int3(xy, 0)).rg;
    uint raw = (bytes.x << 8) | bytes.y;
    uint3 rgb = (uint3(raw) >> uint3(11, 6, 1)) & 31u;
    return float3((rgb << 3) | (rgb >> 2)) / 255.0;
}
float3 sample5551(Texture2D<uint2> image, float2 uv, bool linear) {
    uint2 size;
    image.GetDimensions(size.x, size.y);
    if (!linear) return unpack5551(min(uint2(uv * float2(size)), size - 1u), image);
    float2 position = clamp(uv * float2(size) - 0.5, float2(0), float2(size - 1u));
    uint2 lo = uint2(position);
    uint2 hi = min(lo + 1u, size - 1u);
    float2 t = frac(position);
    return lerp(lerp(unpack5551(lo, image), unpack5551(uint2(hi.x, lo.y), image), t.x),
                lerp(unpack5551(uint2(lo.x, hi.y), image), unpack5551(hi, image), t.x), t.y);
}
float4 image(VSOut input) : SV_Target {
    bool linear = smooth != 0;
    float3 rgb = sample5551(currentImage, input.uv, linear);
    if (blend < 1.0) rgb = lerp(sample5551(previousImage, input.uv, linear), rgb, blend);
    return float4(rgb, 1.0);
}
)";

std::string adapterName(IDXGIAdapter1& adapter) {
    DXGI_ADAPTER_DESC1 description{};
    if (FAILED(adapter.GetDesc1(&description))) return {};
    const int length = WideCharToMultiByte(CP_UTF8, 0, description.Description, -1,
                                           nullptr, 0, nullptr, nullptr);
    if (length <= 1) return {};
    std::string name(static_cast<size_t>(length - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, description.Description, -1, name.data(), length,
                        nullptr, nullptr);
    return name;
}

ComPtr<IDXGIAdapter1> firstHardwareAdapter(IDXGIFactory4& factory) {
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT index = 0; factory.EnumAdapters1(index, adapter.ReleaseAndGetAddressOf()) == S_OK;
         ++index) {
        DXGI_ADAPTER_DESC1 description{};
        if (FAILED(adapter->GetDesc1(&description))) continue;
        if ((description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0) return adapter;
        adapter.Reset();
    }
    return {};
}

ComPtr<ID3DBlob> compile(const char* source, const char* target, const char* entry) {
    ComPtr<ID3DBlob> code;
    ComPtr<ID3DBlob> errors;
    const HRESULT result = D3DCompile(source, std::strlen(source), nullptr, nullptr, nullptr,
                                      entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                                      code.ReleaseAndGetAddressOf(), errors.ReleaseAndGetAddressOf());
    if (FAILED(result)) {
        if (errors) std::fprintf(stderr, "D3D12 shader (%s): %s\n", entry,
                                 static_cast<const char*>(errors->GetBufferPointer()));
        return {};
    }
    return code;
}

struct Slot {
    ComPtr<ID3D12Resource> resource;
    u8* mapped = nullptr;
    u32 width = 0;
    u32 height = 0;
    u32 bytesPerPixel = 0;
    u64 fence = 0;
};

class D3D12Viewport final : public ViewportPresenter {
public:
    GraphicsApi api() const noexcept override { return GraphicsApi::D3D12; }

    ~D3D12Viewport() override {
        if (fence_ && fenceValue_) waitFor(fenceValue_);
        if (fenceEvent_) CloseHandle(fenceEvent_);
    }

    std::string deviceDescription() const override { return deviceDescription_; }
    double gpuMilliseconds() const override { return 0.0; } // No timestamp-query dependency.
    bool busy() const override { return false; }            // Slot fences serialize uploads.
    bool needsRetry() const override { return false; }
    bool hasPresentedFrame() const override { return presented_; }

    void discardFrames() override {
        uploaded_ = 0;
        currentSlot_ = 0;
        hasPrevious_ = false;
        presented_ = false;
    }

    bool attach(void* nativeView) override {
        if (!nativeView) return false;
        hwnd_ = reinterpret_cast<HWND>(nativeView);
        if (!initializeDevice()) return false;
        RECT rect{};
        if (!GetClientRect(hwnd_, &rect)) return false;
        const u32 width = std::max<u32>(1, static_cast<u32>(rect.right - rect.left));
        const u32 height = std::max<u32>(1, static_cast<u32>(rect.bottom - rect.top));
        if (!createSwapChain(width, height)) return false;
        attached_ = true;
        return true;
    }

    bool present(std::span<const u32> pixels, u32 width, u32 height, u64 revision,
                 float blend, bool smooth, const PresentationGeometry& geometry) override {
        if (!attached_ || broken_) return false;
        if (failed(device_->GetDeviceRemovedReason())) {
            broken_ = true;
            return false;
        }
        if (!width || !height || pixels.size() != size_t(width) * height) return false;
        if (!resize(geometry.width, geometry.height)) return false;
        if (uploaded_ != revision) {
            const u32 next = (currentSlot_ + 1) % SlotCount;
            if (!upload(next, pixels.data(), width, height, sizeof(u32), 0)) return false;
            hasPrevious_ = currentSlot_ != next && uploaded_ != 0;
            previousSlot_ = hasPrevious_ ? currentSlot_ : next;
            currentSlot_ = next;
            uploaded_ = revision;
        }
        return draw(hasPrevious_ ? blend : 1.0f, smooth, geometry);
    }

    bool presentPacked(const PackedPresentationFrame& frame, float blend, bool smooth,
                       const PresentationGeometry& geometry) override {
        if (!attached_ || broken_) return false;
        if (failed(device_->GetDeviceRemovedReason())) {
            broken_ = true;
            return false;
        }
        const u32 bytes = frame.bytesPerPixel;
        if (!frame.backing || !frame.width || !frame.height || (bytes != 2 && bytes != 4) ||
            frame.pitch < u64{frame.width} * bytes) return false;
        if (!resize(geometry.width, geometry.height)) return false;
        if (uploaded_ != frame.revision) {
            const u32 next = (currentSlot_ + 1) % SlotCount;
            if (!upload(next, frame.backing->data(), frame.width, frame.height, bytes,
                        frame.pitch)) return false;
            hasPrevious_ = currentSlot_ != next && uploaded_ != 0;
            previousSlot_ = hasPrevious_ ? currentSlot_ : next;
            currentSlot_ = next;
            uploaded_ = frame.revision;
        }
        return draw(hasPrevious_ ? blend : 1.0f, smooth, geometry);
    }

private:
    bool initializeDevice() {
        ComPtr<IDXGIFactory4> factory;
        if (failed(CreateDXGIFactory2(0, IID_PPV_ARGS(factory.ReleaseAndGetAddressOf()))))
            return false;
        auto adapter = firstHardwareAdapter(*factory.Get());
        if (!adapter) {
            std::fprintf(stderr, "D3D12 viewport: no hardware adapter available\n");
            return false;
        }
        if (failed(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0,
                                     IID_PPV_ARGS(device_.ReleaseAndGetAddressOf()))))
            return false;
        if (failed(device_->GetDeviceRemovedReason())) return false;
        deviceDescription_ = adapterName(*adapter.Get());
        factory_ = std::move(factory);

        D3D12_COMMAND_QUEUE_DESC queueDescription{};
        queueDescription.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        queueDescription.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
        queueDescription.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
        if (failed(device_->CreateCommandQueue(&queueDescription,
                                               IID_PPV_ARGS(queue_.ReleaseAndGetAddressOf()))))
            return false;

        D3D12_DESCRIPTOR_HEAP_DESC rtvDescription{};
        rtvDescription.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        rtvDescription.NumDescriptors = BufferCount;
        if (failed(device_->CreateDescriptorHeap(&rtvDescription,
                                                 IID_PPV_ARGS(rtvHeap_.ReleaseAndGetAddressOf()))))
            return false;
        rtvDescriptorSize_ = device_->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

        // Three (current, previous) descriptor pairs: one per swapchain buffer.
        D3D12_DESCRIPTOR_HEAP_DESC srvDescription{};
        srvDescription.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        srvDescription.NumDescriptors = BufferCount * 2;
        srvDescription.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (failed(device_->CreateDescriptorHeap(&srvDescription,
                                                 IID_PPV_ARGS(srvHeap_.ReleaseAndGetAddressOf()))))
            return false;
        srvDescriptorSize_ = device_->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

        D3D12_DESCRIPTOR_HEAP_DESC samplerDescription{};
        samplerDescription.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
        samplerDescription.NumDescriptors = 2;
        samplerDescription.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (failed(device_->CreateDescriptorHeap(
                &samplerDescription, IID_PPV_ARGS(samplerHeap_.ReleaseAndGetAddressOf()))))
            return false;
        samplerDescriptorSize_ = device_->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
        const auto samplerBase = samplerHeap_->GetCPUDescriptorHandleForHeapStart();
        for (u32 index = 0; index < 2; ++index) {
            D3D12_SAMPLER_DESC sampler{};
            sampler.Filter = index == 0 ? D3D12_FILTER_MIN_MAG_MIP_POINT
                                        : D3D12_FILTER_MIN_MAG_MIP_LINEAR;
            sampler.AddressU = sampler.AddressV = sampler.AddressW =
                D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
            sampler.MaxLOD = D3D12_FLOAT32_MAX;
            D3D12_CPU_DESCRIPTOR_HANDLE handle = samplerBase;
            handle.ptr += size_t(index) * samplerDescriptorSize_;
            device_->CreateSampler(&sampler, handle);
        }

        if (!createRootSignature() || !createPipelines()) return false;

        if (failed(device_->CreateCommandAllocator(
                D3D12_COMMAND_LIST_TYPE_DIRECT,
                IID_PPV_ARGS(allocator_.ReleaseAndGetAddressOf())))) return false;
        if (failed(device_->CreateCommandList(
                0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator_.Get(), nullptr,
                IID_PPV_ARGS(list_.ReleaseAndGetAddressOf())))) return false;
        if (failed(list_->Close())) return false;
        if (failed(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                        IID_PPV_ARGS(fence_.ReleaseAndGetAddressOf()))))
            return false;
        fenceEvent_ = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        return fenceEvent_ != nullptr;
    }

    bool createRootSignature() {
        D3D12_DESCRIPTOR_RANGE srvRange{};
        srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        srvRange.NumDescriptors = 2;
        srvRange.BaseShaderRegister = 0;
        srvRange.RegisterSpace = 0;
        srvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        D3D12_DESCRIPTOR_RANGE samplerRange{};
        samplerRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
        samplerRange.NumDescriptors = 1;
        samplerRange.BaseShaderRegister = 0;
        samplerRange.RegisterSpace = 0;
        samplerRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        std::array<D3D12_ROOT_PARAMETER, 3> parameters{};
        parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameters[0].DescriptorTable.NumDescriptorRanges = 1;
        parameters[0].DescriptorTable.pDescriptorRanges = &srvRange;
        parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameters[1].DescriptorTable.NumDescriptorRanges = 1;
        parameters[1].DescriptorTable.pDescriptorRanges = &samplerRange;
        parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        parameters[2].Constants.ShaderRegister = 0;
        parameters[2].Constants.RegisterSpace = 0;
        parameters[2].Constants.Num32BitValues = 4;
        parameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_ROOT_SIGNATURE_DESC description{};
        description.NumParameters = static_cast<UINT>(parameters.size());
        description.pParameters = parameters.data();
        description.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
        ComPtr<ID3DBlob> blob;
        ComPtr<ID3DBlob> errors;
        if (failed(D3D12SerializeRootSignature(&description, D3D_ROOT_SIGNATURE_VERSION_1,
                                                blob.ReleaseAndGetAddressOf(),
                                                errors.ReleaseAndGetAddressOf()))) {
            if (errors) std::fprintf(stderr, "D3D12 root signature: %s\n",
                                     static_cast<const char*>(errors->GetBufferPointer()));
            return false;
        }
        return SUCCEEDED(device_->CreateRootSignature(0, blob->GetBufferPointer(),
                                                      blob->GetBufferSize(),
                                                      IID_PPV_ARGS(root_.ReleaseAndGetAddressOf())));
    }

    bool createPipelines() {
        auto vertex = compile(VertexShader, "vs_5_0", "fullscreen");
        auto pixel8888 = compile(PixelShader8888, "ps_5_0", "image");
        auto pixel5551 = compile(PixelShader5551, "ps_5_0", "image");
        if (!vertex || !pixel8888 || !pixel5551) return false;

        D3D12_GRAPHICS_PIPELINE_STATE_DESC description{};
        description.pRootSignature = root_.Get();
        description.VS = {vertex->GetBufferPointer(), vertex->GetBufferSize()};
        description.BlendState.RenderTarget[0].BlendEnable = FALSE;
        description.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        description.SampleMask = ~0u;
        description.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        description.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        description.DepthStencilState.DepthEnable = FALSE;
        description.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        description.NumRenderTargets = 1;
        description.RTVFormats[0] = SwapChainFormat;
        description.SampleDesc.Count = 1;
        description.PS = {pixel8888->GetBufferPointer(), pixel8888->GetBufferSize()};
        if (failed(device_->CreateGraphicsPipelineState(
                &description, IID_PPV_ARGS(pso8888_.ReleaseAndGetAddressOf())))) return false;
        description.PS = {pixel5551->GetBufferPointer(), pixel5551->GetBufferSize()};
        return SUCCEEDED(device_->CreateGraphicsPipelineState(
            &description, IID_PPV_ARGS(pso5551_.ReleaseAndGetAddressOf())));
    }

    bool createSwapChain(u32 width, u32 height) {
        DXGI_SWAP_CHAIN_DESC1 description{};
        description.Width = width;
        description.Height = height;
        description.Format = SwapChainFormat;
        description.SampleDesc.Count = 1;
        description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        description.BufferCount = BufferCount;
        description.Scaling = DXGI_SCALING_STRETCH;
        description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        description.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        DXGI_SWAP_CHAIN_FULLSCREEN_DESC fullscreen{};
        fullscreen.Windowed = TRUE;
        ComPtr<IDXGISwapChain1> swapChain;
        if (failed(factory_->CreateSwapChainForHwnd(queue_.Get(), hwnd_, &description,
                                                    &fullscreen, nullptr,
                                                    swapChain.ReleaseAndGetAddressOf()))) {
            std::fprintf(stderr, "D3D12 viewport: swapchain creation failed\n");
            return false;
        }
        if (failed(swapChain->QueryInterface(IID_PPV_ARGS(swapChain_.ReleaseAndGetAddressOf()))))
            return false;
        factory_->MakeWindowAssociation(hwnd_, DXGI_MWA_NO_ALT_ENTER);
        swapWidth_ = width;
        swapHeight_ = height;
        return recreateRenderTargets();
    }

    bool recreateRenderTargets() {
        const auto base = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
        for (u32 index = 0; index < BufferCount; ++index) {
            if (failed(swapChain_->GetBuffer(
                    index, IID_PPV_ARGS(backBuffers_[index].ReleaseAndGetAddressOf()))))
                return false;
            D3D12_CPU_DESCRIPTOR_HANDLE handle = base;
            handle.ptr += size_t(index) * rtvDescriptorSize_;
            device_->CreateRenderTargetView(backBuffers_[index].Get(), nullptr, handle);
            inRenderState_[index] = false; // Swapchain buffers start in PRESENT.
        }
        return true;
    }

    void transition(u32 index, bool toRenderTarget) {
        if (inRenderState_[index] == toRenderTarget) return;
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        barrier.Transition.pResource = backBuffers_[index].Get();
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = toRenderTarget ? D3D12_RESOURCE_STATE_PRESENT
                                                        : D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter = toRenderTarget ? D3D12_RESOURCE_STATE_RENDER_TARGET
                                                       : D3D12_RESOURCE_STATE_PRESENT;
        list_->ResourceBarrier(1, &barrier);
        inRenderState_[index] = toRenderTarget;
    }

    bool resize(u32 width, u32 height) {
        if (!width || !height || (width == swapWidth_ && height == swapHeight_)) return true;
        waitFor(fenceValue_);
        if (failed(swapChain_->ResizeBuffers(BufferCount, width, height, SwapChainFormat, 0)))
            return false;
        swapWidth_ = width;
        swapHeight_ = height;
        return recreateRenderTargets();
    }

    void waitFor(u64 value) {
        if (!fence_ || !fenceEvent_ || !value || fence_->GetCompletedValue() >= value) return;
        if (SUCCEEDED(fence_->SetEventOnCompletion(value, fenceEvent_)))
            WaitForSingleObject(fenceEvent_, INFINITE);
    }

    // Copies raw VI bytes only: RGBA5551 unpacking and RGBA8888 filtering both
    // happen in the pixel shader. Upload heaps are triple-buffered and fenced.
    bool upload(u32 slotIndex, const void* pixels, u32 width, u32 height, u32 bytesPerPixel,
                u64 sourcePitch) {
        auto& slot = slots_[slotIndex];
        waitFor(slot.fence);
        const u64 minimumPitch = u64{width} * bytesPerPixel;
        const u64 pitch = sourcePitch ? sourcePitch : minimumPitch;
        if (pitch < minimumPitch) return false;
        const u32 rowPitch = static_cast<u32>(((pitch + 255) / 256) * 256);
        const DXGI_FORMAT format = bytesPerPixel == 2 ? DXGI_FORMAT_R8G8_UINT : SwapChainFormat;
        if (slot.width != width || slot.height != height ||
            slot.bytesPerPixel != bytesPerPixel || slot.mapped == nullptr) {
            slot.resource.Reset();
            D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_UPLOAD, D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
                                       D3D12_MEMORY_POOL_UNKNOWN, 1, 1};
            D3D12_RESOURCE_DESC resource{};
            resource.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            resource.Width = width;
            resource.Height = height;
            resource.DepthOrArraySize = 1;
            resource.MipLevels = 1;
            resource.Format = format;
            resource.SampleDesc.Count = 1;
            resource.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
            if (failed(device_->CreateCommittedResource(
                    &heap, D3D12_HEAP_FLAG_NONE, &resource, D3D12_RESOURCE_STATE_GENERIC_READ,
                    nullptr, IID_PPV_ARGS(slot.resource.ReleaseAndGetAddressOf()))))
                return false;
            if (failed(slot.resource->Map(0, nullptr, reinterpret_cast<void**>(&slot.mapped))))
                return false;
            slot.width = width;
            slot.height = height;
            slot.bytesPerPixel = bytesPerPixel;
        }
        const auto* source = static_cast<const u8*>(pixels);
        for (u32 row = 0; row < height; ++row)
            std::memcpy(slot.mapped + size_t(row) * rowPitch, source + size_t(row) * pitch,
                        size_t(width) * bytesPerPixel);
        return true;
    }

    void writeDescriptor(u32 index, const Slot& slot) {
        D3D12_SHADER_RESOURCE_VIEW_DESC description{};
        description.Format = slot.bytesPerPixel == 2 ? DXGI_FORMAT_R8G8_UINT : SwapChainFormat;
        description.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        description.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        description.Texture2D.MipLevels = 1;
        D3D12_CPU_DESCRIPTOR_HANDLE handle = srvHeap_->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += size_t(index) * srvDescriptorSize_;
        device_->CreateShaderResourceView(slot.resource.Get(), &description, handle);
    }

    bool draw(float blend, bool smooth, const PresentationGeometry& geometry) {
        if (!geometry.width || !geometry.height || !geometry.imageWidth || !geometry.imageHeight)
            return false;
        const u32 frameIndex = swapChain_->GetCurrentBackBufferIndex();
        // One command allocator/list is reused for every back buffer, so the
        // latest submitted fence must retire before either is reset.
        waitFor(fenceValue_);
        writeDescriptor(frameIndex * 2, slots_[currentSlot_]);
        writeDescriptor(frameIndex * 2 + 1, slots_[previousSlot_]);

        if (failed(allocator_->Reset()) || failed(list_->Reset(allocator_.Get(), nullptr)))
            return false;
        transition(frameIndex, true);
        list_->SetGraphicsRootSignature(root_.Get());
        ID3D12DescriptorHeap* heaps[] = {srvHeap_.Get(), samplerHeap_.Get()};
        list_->SetDescriptorHeaps(2, heaps);

        D3D12_VIEWPORT viewport{static_cast<float>(geometry.x), static_cast<float>(geometry.y),
                                static_cast<float>(geometry.imageWidth),
                                static_cast<float>(geometry.imageHeight), 0.0f, 1.0f};
        D3D12_RECT scissor{static_cast<LONG>(geometry.x), static_cast<LONG>(geometry.y),
                           static_cast<LONG>(geometry.x + geometry.imageWidth),
                           static_cast<LONG>(geometry.y + geometry.imageHeight)};
        list_->RSSetViewports(1, &viewport);
        list_->RSSetScissorRects(1, &scissor);

        D3D12_CPU_DESCRIPTOR_HANDLE renderTarget =
            rtvHeap_->GetCPUDescriptorHandleForHeapStart();
        renderTarget.ptr += size_t(frameIndex) * rtvDescriptorSize_;
        const float clear[] = {0.0f, 0.0f, 0.0f, 1.0f};
        list_->ClearRenderTargetView(renderTarget, clear, 0, nullptr);
        list_->OMSetRenderTargets(1, &renderTarget, FALSE, nullptr);

        const bool packed5551 = slots_[currentSlot_].bytesPerPixel == 2;
        list_->SetPipelineState(packed5551 ? pso5551_.Get() : pso8888_.Get());
        list_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        D3D12_GPU_DESCRIPTOR_HANDLE textures = srvHeap_->GetGPUDescriptorHandleForHeapStart();
        textures.ptr += size_t(frameIndex * 2) * srvDescriptorSize_;
        D3D12_GPU_DESCRIPTOR_HANDLE sampler = samplerHeap_->GetGPUDescriptorHandleForHeapStart();
        sampler.ptr += size_t(smooth ? 1 : 0) * samplerDescriptorSize_;
        list_->SetGraphicsRootDescriptorTable(0, textures);
        list_->SetGraphicsRootDescriptorTable(1, sampler);
        const u32 constants[4] = {*reinterpret_cast<const u32*>(&blend), smooth ? 1u : 0u, 0u, 0u};
        list_->SetGraphicsRoot32BitConstants(2, 4, constants, 0);
        list_->DrawInstanced(3, 1, 0, 0);
        transition(frameIndex, false);

        if (failed(list_->Close())) return false;
        ID3D12CommandList* commands[] = {list_.Get()};
        queue_->ExecuteCommandLists(1, commands);
        if (failed(swapChain_->Present(1, 0))) return false;
        ++fenceValue_;
        if (failed(queue_->Signal(fence_.Get(), fenceValue_))) return false;
        slots_[currentSlot_].fence = fenceValue_;
        presented_ = true;
        return true;
    }

    ComPtr<IDXGIFactory4> factory_;
    ComPtr<ID3D12Device> device_;
    ComPtr<ID3D12CommandQueue> queue_;
    ComPtr<IDXGISwapChain3> swapChain_;
    ComPtr<ID3D12CommandAllocator> allocator_;
    ComPtr<ID3D12GraphicsCommandList> list_;
    ComPtr<ID3D12DescriptorHeap> rtvHeap_;
    ComPtr<ID3D12DescriptorHeap> srvHeap_;
    ComPtr<ID3D12DescriptorHeap> samplerHeap_;
    ComPtr<ID3D12RootSignature> root_;
    ComPtr<ID3D12PipelineState> pso8888_;
    ComPtr<ID3D12PipelineState> pso5551_;
    ComPtr<ID3D12Fence> fence_;
    HANDLE fenceEvent_ = nullptr;
    u64 fenceValue_ = 0;
    std::array<ComPtr<ID3D12Resource>, BufferCount> backBuffers_;
    std::array<bool, BufferCount> inRenderState_{};
    std::array<Slot, SlotCount> slots_;
    u32 rtvDescriptorSize_ = 0;
    u32 srvDescriptorSize_ = 0;
    u32 samplerDescriptorSize_ = 0;
    u32 swapWidth_ = 0;
    u32 swapHeight_ = 0;
    u32 currentSlot_ = 0;
    u32 previousSlot_ = 0;
    u64 uploaded_ = 0;
    bool hasPrevious_ = false;
    bool attached_ = false;
    bool broken_ = false;
    bool presented_ = false;
    HWND hwnd_ = nullptr;
    std::string deviceDescription_;
};

} // namespace

std::unique_ptr<ViewportPresenter> createD3D12Viewport() {
    return std::make_unique<D3D12Viewport>();
}

bool d3d12DeviceAvailable(std::string* description) {
    ComPtr<IDXGIFactory4> factory;
    if (failed(CreateDXGIFactory2(0, IID_PPV_ARGS(factory.ReleaseAndGetAddressOf()))))
        return false;
    auto adapter = firstHardwareAdapter(*factory.Get());
    if (!adapter) return false;
    ComPtr<ID3D12Device> device;
    if (failed(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0,
                                 IID_PPV_ARGS(device.ReleaseAndGetAddressOf()))))
        return false;
    if (failed(device->GetDeviceRemovedReason())) return false;
    if (description) *description = adapterName(*adapter.Get());
    return true;
}

} // namespace n64forge::video
