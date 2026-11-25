#include "n64forge/video/viewport_presenter.hpp"
#import <AppKit/AppKit.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <dispatch/dispatch.h>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace n64forge::video {
namespace {
constexpr char shader[] = R"(
#include <metal_stdlib>
using namespace metal;
struct Vertex { float4 position [[position]]; float2 uv; };
vertex Vertex fullscreen_quad(uint i [[vertex_id]]) {
    float2 uv = float2(float((i << 1) & 2), float(i & 2));
    return {float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.0, 1.0), uv};
}
fragment float4 image(Vertex v [[stage_in]], texture2d<float> current [[texture(0)]],
                      texture2d<float> previous [[texture(1)]], sampler filter [[sampler(0)]],
                      constant float& blend [[buffer(0)]]) {
    float3 rgb = current.sample(filter, v.uv).rgb;
    if (blend < 1.0) rgb = mix(previous.sample(filter, v.uv).rgb, rgb, blend);
    return float4(rgb, 1.0);
}
float3 unpack5551(texture2d<uint> image, uint2 xy) {
    uint2 bytes = image.read(xy).rg;
    uint raw = (bytes.x << 8) | bytes.y;
    uint3 rgb = (uint3(raw) >> uint3(11, 6, 1)) & 31u;
    return float3((rgb << 3) | (rgb >> 2)) / 255.0;
}
float3 sample5551(texture2d<uint> image, float2 uv, bool smooth) {
    const uint2 size = uint2(image.get_width(), image.get_height());
    if (!smooth) return unpack5551(image, min(uint2(uv * float2(size)), size - 1u));
    const float2 pos = clamp(uv * float2(size) - 0.5, float2(0), float2(size - 1u));
    const uint2 lo = uint2(pos), hi = min(lo + 1u, size - 1u);
    const float2 t = fract(pos);
    return mix(mix(unpack5551(image, lo), unpack5551(image, uint2(hi.x, lo.y)), t.x),
               mix(unpack5551(image, uint2(lo.x, hi.y)), unpack5551(image, hi), t.x), t.y);
}
fragment float4 image16(Vertex v [[stage_in]], texture2d<uint> current [[texture(0)]],
                        texture2d<uint> previous [[texture(1)]], constant float& blend [[buffer(0)]],
                        constant uint& smooth [[buffer(1)]]) {
    float3 rgb = sample5551(current, v.uv, smooth != 0);
    if (blend < 1.0) rgb = mix(sample5551(previous, v.uv, smooth != 0), rgb, blend);
    return float4(rgb, 1.0);
})";

class MetalFrameBacking final : public FrameBacking {
public:
    explicit MetalFrameBacking(id<MTLBuffer> buffer) : buffer_(buffer) {}
    u32* data() const noexcept override { return static_cast<u32*>(buffer_.contents); }
    id<MTLTexture> texture(const PackedPresentationFrame& frame) const {
        const auto format = frame.bytesPerPixel == 2 ? MTLPixelFormatRG8Uint : MTLPixelFormatRGBA8Unorm;
        if (texture_ && texture_.width == frame.width && texture_.height == frame.height &&
            texture_.pixelFormat == format && pitch_ == frame.pitch) return texture_;
        auto* desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format
            width:frame.width height:frame.height mipmapped:NO];
        desc.storageMode = MTLStorageModeShared;
        desc.usage = MTLTextureUsageShaderRead;
        texture_ = [buffer_ newTextureWithDescriptor:desc offset:0 bytesPerRow:frame.pitch];
        pitch_ = frame.pitch;
        return texture_;
    }
private:
    id<MTLBuffer> buffer_;
    mutable id<MTLTexture> texture_ = nil;
    mutable u32 pitch_ = 0;
};

class MetalViewport final : public ViewportPresenter {
public:
    GraphicsApi api() const noexcept override { return GraphicsApi::Metal; }
    ~MetalViewport() override {
        discardFrames();
        [view_ removeFromSuperview];
    }

    std::string deviceDescription() const override {
        if (!device_) return {};
        const char* name = device_.name.UTF8String;
        return name ? name : "";
    }

    bool attach(void* nativeView) override {
        NSView* parent = (__bridge NSView*)nativeView;
        if (!parent) {
            std::fprintf(stderr, "Metal viewport: Qt did not supply an NSView\n");
            return false;
        }
        if (!initialize()) return false;
        layer_ = [CAMetalLayer layer];
        layer_.device = device_;
        layer_.pixelFormat = MTLPixelFormatBGRA8Unorm;
        layer_.opaque = YES;
        layer_.maximumDrawableCount = 3;
        layer_.allowsNextDrawableTimeout = YES;
        view_ = [[NSView alloc] initWithFrame:parent.bounds];
        view_.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
        view_.wantsLayer = YES;
        view_.layer = layer_;
        [parent addSubview:view_];
        return true;
    }

    bool initialize() {
        device_ = MTLCreateSystemDefaultDevice();
        if (!device_) {
            std::fprintf(stderr, "Metal viewport: MTLCreateSystemDefaultDevice returned nil\n");
            return false;
        }
        queue_ = [device_ newCommandQueue];
        NSError* error = nil;
        id<MTLLibrary> lib = [device_ newLibraryWithSource:[NSString stringWithUTF8String:shader]
                                                  options:nil error:&error];
        if (!lib || !queue_) {
            std::fprintf(stderr, "Metal viewport: library failed: %s\n",
                         error ? error.localizedDescription.UTF8String : "no device queue");
            return false;
        }
        MTLRenderPipelineDescriptor* desc = [MTLRenderPipelineDescriptor new];
        desc.vertexFunction = [lib newFunctionWithName:@"fullscreen_quad"];
        desc.fragmentFunction = [lib newFunctionWithName:@"image"];
        desc.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
        pipeline_ = [device_ newRenderPipelineStateWithDescriptor:desc error:&error];
        desc.fragmentFunction = [lib newFunctionWithName:@"image16"];
        pipeline16_ = [device_ newRenderPipelineStateWithDescriptor:desc error:&error];
        if (!pipeline_ || !pipeline16_) {
            std::fprintf(stderr, "Metal viewport: pipeline failed: %s\n",
                         error ? error.localizedDescription.UTF8String : "unknown error");
            return false;
        }
        MTLSamplerDescriptor* sampler = [MTLSamplerDescriptor new];
        nearest_ = [device_ newSamplerStateWithDescriptor:sampler];
        sampler.minFilter = sampler.magFilter = MTLSamplerMinMagFilterLinear;
        linear_ = [device_ newSamplerStateWithDescriptor:sampler];
        if (!nearest_ || !linear_) {
            std::fprintf(stderr, "Metal viewport: failed to create texture samplers\n");
            return false;
        }
        return true;
    }

    bool present(std::span<const u32> pixels, u32 width, u32 height, u64 revision,
                 float blend, bool smooth, const PresentationGeometry& geometry) override {
        @autoreleasepool {
            const auto reject = [this](int reason, const char* text) {
                if (!logged_[reason]) {
                    logged_[reason] = true;
                    std::fprintf(stderr, "Metal viewport: %s\n", text);
                }
                return false;
            };
            if (busy()) return false;
            if (!width || !height || pixels.size() != size_t(width) * height)
                return reject(2, "invalid texture size");
            if (uploaded_ != revision) {
                MTLTextureDescriptor* texture = [MTLTextureDescriptor
                    texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:width
                    height:height mipmapped:NO];
                texture.storageMode = MTLStorageModeShared;
                texture.usage = MTLTextureUsageShaderRead;
                // Do not overwrite a texture retained by an earlier command.
                auto next = [device_ newTextureWithDescriptor:texture];
                if (!next) return false;
                havePrevious_ = current_ && current_.width == width && current_.height == height;
                previous_ = havePrevious_ ? current_ : nil;
                current_ = next;
                [current_ replaceRegion:MTLRegionMake2D(0, 0, width, height)
                            mipmapLevel:0 withBytes:pixels.data() bytesPerRow:width * sizeof(u32)];
                uploaded_ = revision;
            }
            return draw(current_, havePrevious_ ? previous_ : current_, false,
                        havePrevious_ ? blend : 1.0f, smooth, geometry);
        }
    }

    bool presentPacked(const PackedPresentationFrame& frame, float blend, bool smooth,
                       const PresentationGeometry& geometry) override {
        @autoreleasepool {
            if (busy()) return false;
            if (uploaded_ != frame.revision) {
                const auto* backing = dynamic_cast<const MetalFrameBacking*>(frame.backing.get());
                if (!backing) return false;
                auto texture = backing->texture(frame);
                if (!texture) return false;
                havePrevious_ = current_ && current_.width == frame.width && current_.height == frame.height &&
                                current_.pixelFormat == texture.pixelFormat;
                previous_ = havePrevious_ ? current_ : nil;
                previousLease_ = havePrevious_ ? currentLease_ : nullptr;
                current_ = texture;
                currentLease_ = frame.lease;
                uploaded_ = frame.revision;
            }
            return draw(current_, havePrevious_ ? previous_ : current_, frame.bytesPerPixel == 2,
                        havePrevious_ ? blend : 1.0f, smooth, geometry);
        }
    }

    bool busy() const override {
        return flight_->acquiring.load(std::memory_order_acquire) ||
               flight_->submitted.load(std::memory_order_acquire) >= 2;
    }
    bool needsRetry() const override { return flight_->retry.load(std::memory_order_acquire); }
    bool hasPresentedFrame() const override {
        return flight_->completedEpoch.load(std::memory_order_acquire) != ~u64{0};
    }
    double gpuMilliseconds() const override { return flight_->gpuMs.load(std::memory_order_relaxed); }
    void discardFrames() override {
        flight_->epoch.fetch_add(1, std::memory_order_relaxed);
        flight_->completedEpoch.store(~u64{0}, std::memory_order_release);
        current_ = previous_ = nil;
        currentLease_.reset();
        previousLease_.reset();
        uploaded_ = 0;
        havePrevious_ = false;
    }

    bool verifyPackedPixels() {
        auto backing = createPackedFrameBacking(PackedFrameAlignment / sizeof(u32) * 2);
        if (!backing) {
            std::fprintf(stderr, "Metal packed readback skipped: shared texture storage unavailable\n");
            return true;
        }
        if (!initialize()) return false;
        constexpr PresentationGeometry geometry{7, 5, 2, 1, 3, 3, 1};
        auto* desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
            width:geometry.width height:geometry.height mipmapped:NO];
        desc.storageMode = MTLStorageModeShared;
        desc.usage = MTLTextureUsageRenderTarget;
        auto target = [device_ newTextureWithDescriptor:desc];
        if (!target) return false;

        for (const u32 bytes : {2u, 4u}) {
            auto* raw = reinterpret_cast<u8*>(backing->data());
            if (bytes == 2) {
                const u8 top[] = {0xF8, 0x00, 0x07, 0xC1}, bottom[] = {0x00, 0x3F, 0xFF, 0xFF};
                std::memcpy(raw, top, sizeof(top));
                std::memcpy(raw + PackedFrameAlignment, bottom, sizeof(bottom));
            } else {
                const u8 top[] = {255, 0, 0, 0, 0, 255, 0, 255};
                const u8 bottom[] = {0, 0, 255, 255, 255, 255, 255, 0};
                std::memcpy(raw, top, sizeof(top));
                std::memcpy(raw + PackedFrameAlignment, bottom, sizeof(bottom));
            }
            PackedPresentationFrame frame{backing, {}, PackedFrameAlignment, 2, 2, bytes, 1};
            auto current = static_cast<const MetalFrameBacking&>(*backing).texture(frame);
            if (!current) return false;
            auto previousBacking = createPackedFrameBacking(PackedFrameAlignment / sizeof(u32) * 2);
            if (!previousBacking) return false;
            std::memset(previousBacking->data(), 0, PackedFrameAlignment * 2);
            auto previous = static_cast<const MetalFrameBacking&>(*previousBacking).texture(frame);
            if (!previous) return false;

            for (const bool smooth : {false, true}) {
                auto cmd = [queue_ commandBuffer];
                const float blend = smooth ? 0.5f : 1.0f;
                if (!cmd || !encode(cmd, target, current, previous, bytes == 2, blend, smooth, geometry,
                                    bytes == 2 ? pipeline16_ : pipeline_, smooth ? linear_ : nearest_))
                    return false;
                [cmd commit];
                [cmd waitUntilCompleted];
                if (cmd.status != MTLCommandBufferStatusCompleted) return false;
                std::array<u8, geometry.width * geometry.height * 4> image{};
                [target getBytes:image.data() bytesPerRow:geometry.width * 4
                      fromRegion:MTLRegionMake2D(0, 0, geometry.width, geometry.height) mipmapLevel:0];
                const auto matches = [&](u32 x, u32 y, int r, int g, int b, int tol = 0) {
                    const auto* p = &image[(y * geometry.width + x) * 4];
                    return std::abs(p[2] - r) <= tol && std::abs(p[1] - g) <= tol &&
                           std::abs(p[0] - b) <= tol && p[3] == 255;
                };
                if (!matches(0, 0, 0, 0, 0) || !matches(5, 2, 0, 0, 0)) return false;
                if (!smooth && (!matches(2, 1, 255, 0, 0) || !matches(4, 1, 0, 255, 0) ||
                                !matches(2, 3, 0, 0, 255) || !matches(4, 3, 255, 255, 255))) return false;
                if (smooth && !matches(3, 2, 64, 64, 64, 1)) return false;
            }
        }
        return true;
    }

private:
    bool draw(id<MTLTexture> current, id<MTLTexture> previous, bool packed16, float blend,
              bool smooth, const PresentationGeometry& geometry) {
        if (!view_.window || NSIsEmptyRect(view_.bounds)) return false;
        if (layer_.contentsScale != geometry.devicePixelRatio) layer_.contentsScale = geometry.devicePixelRatio;
        const CGSize output = CGSizeMake(geometry.width, geometry.height);
        if (!CGSizeEqualToSize(layer_.drawableSize, output)) layer_.drawableSize = output;

        const auto flight = flight_;
        const auto keepCurrent = currentLease_, keepPrevious = previousLease_;
        const auto layer = layer_;
        const auto queue = queue_;
        const auto pipeline = packed16 ? pipeline16_ : pipeline_;
        const auto sampler = smooth ? linear_ : nearest_;
        const auto outputGeometry = geometry;
        // Every submitted frame gets its own generation. Stale completions can
        // then be ignored without retracting a newer frame's presentation flag.
        const auto epoch = flight->epoch.fetch_add(1, std::memory_order_relaxed) + 1;
        flight->acquiring.store(true, std::memory_order_relaxed);
        flight->retry.store(false, std::memory_order_relaxed);
        // nextDrawable can wait for the compositor even with three drawables.
        // Keep that wait off Qt and off the guest; never queue stale frames
        // behind a pending acquisition. Blocks retain resources, not `this`.
        dispatch_async(acquisition_, ^{
            @autoreleasepool {
                const auto drawable = [layer nextDrawable];
                if (epoch == flight->epoch.load(std::memory_order_relaxed)) {
                    auto cmd = drawable && drawable.texture.width == outputGeometry.width &&
                               drawable.texture.height == outputGeometry.height
                               ? [queue commandBuffer] : nil;
                    if (cmd && encode(cmd, drawable.texture, current, previous, packed16, blend, smooth,
                                      outputGeometry, pipeline, sampler)) {
                        flight->submitted.fetch_add(1, std::memory_order_relaxed);
                        [cmd addCompletedHandler:^(id<MTLCommandBuffer> done) {
                            (void)keepCurrent;
                            (void)keepPrevious;
                            if (done.status == MTLCommandBufferStatusCompleted) {
                                flight->gpuMs.store((done.GPUEndTime - done.GPUStartTime) * 1000,
                                                    std::memory_order_relaxed);
                                // Completions can arrive out of order; never let an
                                // older frame retract a newer frame's completion.
                                const auto completed =
                                    flight->completedEpoch.load(std::memory_order_acquire);
                                if (epoch == flight->epoch.load(std::memory_order_relaxed) &&
                                    (completed == ~u64{0} || epoch > completed))
                                    flight->completedEpoch.store(epoch, std::memory_order_release);
                            } else if (epoch == flight->epoch.load(std::memory_order_relaxed))
                                flight->retry.store(true, std::memory_order_release);
                            flight->submitted.fetch_sub(1, std::memory_order_release);
                        }];
                        [cmd presentDrawable:drawable];
                        [cmd commit];
                    } else {
                        flight->retry.store(true, std::memory_order_release);
                    }
                }
                flight->acquiring.store(false, std::memory_order_release);
            }
        });
        return true;
    }

    static bool encode(id<MTLCommandBuffer> cmd, id<MTLTexture> target, id<MTLTexture> current,
                       id<MTLTexture> previous, bool packed16, float blend, bool smooth,
                       const PresentationGeometry& geometry, id<MTLRenderPipelineState> pipeline,
                       id<MTLSamplerState> sampler) {
        MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture = target;
        pass.colorAttachments[0].loadAction = MTLLoadActionClear;
        pass.colorAttachments[0].clearColor = MTLClearColor{0, 0, 0, 1};
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        auto encoder = [cmd renderCommandEncoderWithDescriptor:pass];
        if (!encoder) return false;
        [encoder setViewport:MTLViewport{double(geometry.x), double(geometry.y),
                                          double(geometry.imageWidth), double(geometry.imageHeight), 0, 1}];
        [encoder setScissorRect:MTLScissorRect{geometry.x, geometry.y,
                                              geometry.imageWidth, geometry.imageHeight}];
        [encoder setRenderPipelineState:pipeline];
        [encoder setFragmentTexture:current atIndex:0];
        [encoder setFragmentTexture:previous atIndex:1];
        if (packed16) {
            const u32 filter = smooth;
            [encoder setFragmentBytes:&filter length:sizeof(filter) atIndex:1];
        } else {
            [encoder setFragmentSamplerState:sampler atIndex:0];
        }
        [encoder setFragmentBytes:&blend length:sizeof(blend) atIndex:0];
        [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [encoder endEncoding];
        return true;
    }

    struct Flight {
        std::atomic<bool> acquiring{false}, retry{false};
        std::atomic<unsigned> submitted{0};
        std::atomic<u64> epoch{0};
        std::atomic<u64> completedEpoch{~u64{0}};
        std::atomic<double> gpuMs{0};
    };
    std::shared_ptr<Flight> flight_ = std::make_shared<Flight>();
    dispatch_queue_t acquisition_ = dispatch_queue_create("studio.metal.drawable", DISPATCH_QUEUE_SERIAL);
    NSView* view_ = nil;
    CAMetalLayer* layer_ = nil;
    id<MTLDevice> device_ = nil;
    id<MTLCommandQueue> queue_ = nil;
    id<MTLRenderPipelineState> pipeline_ = nil;
    id<MTLRenderPipelineState> pipeline16_ = nil;
    id<MTLSamplerState> nearest_ = nil, linear_ = nil;
    id<MTLTexture> current_ = nil, previous_ = nil;
    std::shared_ptr<const void> currentLease_, previousLease_;
    u64 uploaded_ = 0;
    bool havePrevious_ = false;
    std::array<bool, 5> logged_{};
};
} // namespace

std::unique_ptr<ViewportPresenter> createMetalViewport() { return std::make_unique<MetalViewport>(); }
std::string metalDeviceName() {
    @autoreleasepool {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        const auto* name = device.name.UTF8String;
        return name ? name : "";
    }
}
bool verifyMetalPackedPixels() {
    @autoreleasepool {
        return MetalViewport{}.verifyPackedPixels();
    }
}
std::shared_ptr<FrameBacking> createMetalPackedFrameBacking(u32 wordCapacity) {
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (!device || !device.hasUnifiedMemory) return {};
    for (auto format : {MTLPixelFormatRG8Uint, MTLPixelFormatRGBA8Unorm}) {
        const auto alignment = [device minimumLinearTextureAlignmentForPixelFormat:format];
        if (!alignment || PackedFrameAlignment % alignment) return {};
    }
    id<MTLBuffer> buffer = [device newBufferWithLength:NSUInteger(wordCapacity) * sizeof(u32)
                                               options:MTLResourceStorageModeShared];
    if (!buffer) return {};
    return std::make_shared<MetalFrameBacking>(buffer);
}
} // namespace n64forge::video
