#pragma once

#ifdef __APPLE__

#import <Foundation/Foundation.h>

#include "n64forge/emulation/frame_ring_buffer.hpp"

// Objective-C++ wrapper for the Metal/CADisplayLink sub-frame interpolation loop.
// Private to the Metal renderer: the renderer-neutral public include tree must
// not expose Objective-C types or Apple framework headers.
@interface MetalFrameLoop : NSObject

- (instancetype)initWithDevice:(void*)device
                  commandQueue:(void*)commandQueue
                      frameRing:(n64forge::emulation::FrameRingBuffer*)frameRing
                      sourceFps:(double)sourceFps;

- (void)startWithLayer:(void*)layer;
- (void)stop;

@end

#endif // __APPLE__
