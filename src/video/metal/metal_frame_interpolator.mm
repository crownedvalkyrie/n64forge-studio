#import "metal_frame_loop.hpp"
#import "n64forge/video/frame_interpolation.hpp"
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <QuartzCore/CADisplayLink.h>
#import <QuartzCore/CAMetalLayer.h>
#import <AppKit/AppKit.h>
#import <algorithm>
#import <cmath>
#import <unordered_map>

namespace n64forge::video::metal {

namespace {

// Match the current frame's draws against the previous frame by identity key,
// returning the best previous draw for interpolation. Falls back to the same
// index when no key match exists.
n64forge::video::DrawCallSnapshot MatchPreviousDraw(
    const n64forge::video::DrawCallSnapshot& current,
    const std::unordered_map<uint64_t, n64forge::video::DrawCallSnapshot>& previousByKey,
    const n64forge::video::DrawCallSnapshot* previousSameIndex) {
    const auto it = previousByKey.find(current.matchKey);
    if (it != previousByKey.end()) {
        return it->second;
    }
    if (previousSameIndex) {
        return *previousSameIndex;
    }
    return current;
}

} // namespace

} // namespace n64forge::video::metal

@interface MetalFrameLoop () {
    id<MTLDevice> _device;
    id<MTLCommandQueue> _commandQueue;
    n64forge::emulation::FrameRingBuffer* _frameRing;
    CADisplayLink* _displayLink;
    CAMetalLayer* _layer;
    double _linkStartTime;
    double _sourceFps;
    uint64_t _lastConsumedOrdinal;
}
@end

@implementation MetalFrameLoop

- (instancetype)initWithDevice:(void*)device
                  commandQueue:(void*)commandQueue
                      frameRing:(n64forge::emulation::FrameRingBuffer*)frameRing
                      sourceFps:(double)sourceFps {
    self = [super init];
    if (self) {
        _device = (__bridge id<MTLDevice>)device;
        _commandQueue = (__bridge id<MTLCommandQueue>)commandQueue;
        _frameRing = frameRing;
        _sourceFps = sourceFps;
        _linkStartTime = CACurrentMediaTime();
        _lastConsumedOrdinal = 0;
    }
    return self;
}

- (void)startWithLayer:(void*)layer {
    if (_displayLink) return;
    _layer = (__bridge CAMetalLayer*)layer;
    // CADisplayLink on macOS is surfaced through the screen/view APIs; use the
    // screen-backed display link which fires on display vsync.
    NSScreen *targetScreen = [NSScreen mainScreen] ?: [[NSScreen screens] firstObject];
    _displayLink = [targetScreen displayLinkWithTarget:self selector:@selector(displayLinkTick:)];
    [_displayLink addToRunLoop:[NSRunLoop mainRunLoop] forMode:NSDefaultRunLoopMode];
}

- (void)stop {
    if (_displayLink) {
        [_displayLink invalidate];
        _displayLink = nil;
    }
    _layer = nil;
}

- (void)displayLinkTick:(CADisplayLink*)link {
    (void)link;
    if (!_device || !_commandQueue || !_frameRing) return;

    // Host wall-clock time (seconds) since the display link started.
    const double hostTime = CACurrentMediaTime() - _linkStartTime;
    const double sourcePeriod = (_sourceFps > 0.0) ? (1.0 / _sourceFps) : (1.0 / 30.0);
    const double sourceTime = hostTime / sourcePeriod;
    const uint64_t currentOrdinal = static_cast<uint64_t>(std::floor(sourceTime)) + 1;

    if (currentOrdinal == _lastConsumedOrdinal) {
        return;
    }
    _lastConsumedOrdinal = currentOrdinal;

    const float alpha = n64forge::video::ComputeInterpolationAlpha(_sourceFps, hostTime, 0.0);

    const auto currentFrame = _frameRing->waitFrame(currentOrdinal);
    const auto previousFrame = _frameRing->waitFrame(currentOrdinal - 1);
    if (!currentFrame) return;

    // Build a hash map of previous-frame draws by identity key for fast matching.
    std::unordered_map<uint64_t, n64forge::video::DrawCallSnapshot> previousByKey;
    if (previousFrame) {
        previousByKey.reserve(previousFrame->draws.size());
        for (const auto& draw : previousFrame->draws) {
            previousByKey.emplace(draw.matchKey, draw);
        }
    }

    id<MTLCommandBuffer> commandBuffer = [_commandQueue commandBuffer];
    MTLRenderPassDescriptor* passDescriptor = [MTLRenderPassDescriptor renderPassDescriptor];
    // Color attachment setup is intentionally left minimal; the owning renderer
    // is responsible for configuring the full render pass when it takes over.
    id<MTLRenderCommandEncoder> encoder = [commandBuffer renderCommandEncoderWithDescriptor:passDescriptor];

    for (std::size_t i = 0; i < currentFrame->draws.size(); ++i) {
        const auto& current = currentFrame->draws[i];
        const auto& previous = previousFrame && i < previousFrame->draws.size()
                                   ? previousFrame->draws[i]
                                   : current;
        bool wasCut = false;
        const n64forge::video::DrawCallSnapshot& prevForInterp =
            n64forge::video::metal::MatchPreviousDraw(current, previousByKey,
                                                      previousFrame ? &previous : nullptr);
        const n64forge::video::Mat4 mvp =
            n64forge::video::InterpolateMvp(prevForInterp, current, alpha, wasCut);

        // Push the interpolated MVP as a per-draw vertex uniform at index 1.
        [encoder setVertexBytes:&mvp length:sizeof(mvp) atIndex:1];

        // TODO: bind vertex buffer, textures and issue the actual draw call once
        // the HLE display-list parser supplies real geometry.
    }

    [encoder endEncoding];
    [commandBuffer presentDrawable:[_layer nextDrawable]];
    [commandBuffer commit];
}

@end
