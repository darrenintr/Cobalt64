// SPDX-License-Identifier: MIT
// SDK compile/run check for the Metal enum numbers used by the Navi33 format
// table. This validates enum compatibility only, not runtime format support.
// It does not create a Metal device or command queue and submits no GPU work.
#import <Metal/Metal.h>

#include <stdio.h>

#include "../src/Navi33Gfx11Formats.h"

int main(void) {
    if ((NSUInteger)N33_MTL_RGBA8_UNORM !=
        (NSUInteger)MTLPixelFormatRGBA8Unorm) {
        fprintf(stderr,
                "RGBA8 enum mismatch: Navi33=%lu MetalSDK=%lu\n",
                (unsigned long)N33_MTL_RGBA8_UNORM,
                (unsigned long)MTLPixelFormatRGBA8Unorm);
        return 1;
    }
    if ((NSUInteger)N33_MTL_BGRA8_UNORM !=
        (NSUInteger)MTLPixelFormatBGRA8Unorm) {
        fprintf(stderr,
                "BGRA8 enum mismatch: Navi33=%lu MetalSDK=%lu\n",
                (unsigned long)N33_MTL_BGRA8_UNORM,
                (unsigned long)MTLPixelFormatBGRA8Unorm);
        return 1;
    }

    printf("Metal SDK enum compatibility: PASS (RGBA8=%lu, BGRA8=%lu)\n",
           (unsigned long)N33_MTL_RGBA8_UNORM,
           (unsigned long)N33_MTL_BGRA8_UNORM);
    return 0;
}
