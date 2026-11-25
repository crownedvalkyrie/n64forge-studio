#include "n64forge/video/renderer.hpp"
#import "metal_frame_loop.hpp"
#include "n64forge/emulation/frame_ring_buffer.hpp"
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#include <memory>

namespace {

class MetalRenderer final : public n64forge::Renderer {
public:
    MetalRenderer() {
        frameRing_ = std::make_unique<n64forge::emulation::FrameRingBuffer>();
    }

    ~MetalRenderer() override { stopFrameLoop(); }

    bool initialize(void* nativeWindow, const n64forge::RendererOptions& options) override {
        stopFrameLoop(); // Release any loop left over from an earlier initialize.
        (void)options;
        device_ = MTLCreateSystemDefaultDevice();
        if (!device_) return false;

        commandQueue_ = [device_ newCommandQueue];
        if (!commandQueue_) return false;

        // Source FPS is not known at renderer creation time; the loop will be
        // restarted with the correct rate once the core reports AV info.
        frameLoop_ = [[MetalFrameLoop alloc] initWithDevice:(__bridge void*)device_
                                               commandQueue:(__bridge void*)commandQueue_
                                                  frameRing:frameRing_.get()
                                                  sourceFps:60.0];

        CAMetalLayer* layer = nil;
        if (nativeWindow) {
            // The native window handle is expected to be a CAMetalLayer on macOS.
            layer = (__bridge CAMetalLayer*)nativeWindow;
        }
        if (layer) {
            layer.device = device_;
            [frameLoop_ startWithLayer:(__bridge void*)layer];
        }
        return true;
    }

    void present() override {
        // The CADisplayLink callback drives the actual host presentation; this
        // method is a no-op for now. In the future it can be used to throttle
        // or synchronize with the Qt event loop.
    }

    std::string backendName() const override { return "Metal (sub-frame interpolation)"; }

    [[nodiscard]] n64forge::emulation::FrameRingBuffer* frameRing() const noexcept {
        return frameRing_.get();
    }

private:
    void stopFrameLoop() {
        if (frameLoop_) {
            [frameLoop_ stop];
            frameLoop_ = nil; // ARC releases the last strong reference here.
        }
        device_ = nil;
        commandQueue_ = nil;
    }

    id<MTLDevice> __strong device_ = nil;
    id<MTLCommandQueue> __strong commandQueue_ = nil;
    MetalFrameLoop* __strong frameLoop_ = nil;
    std::unique_ptr<n64forge::emulation::FrameRingBuffer> frameRing_;
};

} // namespace

std::unique_ptr<n64forge::Renderer> createMetalRenderer() {
    return std::make_unique<MetalRenderer>();
}
