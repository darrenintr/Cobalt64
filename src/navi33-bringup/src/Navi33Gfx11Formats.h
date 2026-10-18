// Navi33 / GFX11 mapping for the two common 8-bit UNORM Metal render targets.
//
// This is format metadata only; it is not a render backend, a capability
// declaration, or a hardware path. It is not connected to the kext or N33N
// user-client. A RADV backend must still query format features before
// exposing a Metal format.
//
// Sources checked for this subset:
// - Metal pixel format numbers: local Navi48 gfx12_formats.h, which records
//   that its MTLPixelFormat values were checked against the macOS SDK.
// - VkFormat numbers: KhronosGroup/Vulkan-Headers, vulkan_core.h.
// - GFX11_FORMAT: Mesa src/amd/registers/gfx11-rsrc.json.
// - CB ColorFormat, SurfaceNumber, SurfaceSwap: Mesa
//   src/amd/registers/gfx11.json. Mesa src/amd/common/ac_formats.c maps plain
//   8:8:8:8 UNORM surfaces to COLOR_8_8_8_8 / NUMBER_UNORM and chooses
//   SWAP_STD for RGBA or SWAP_ALT for BGRA. radv_formats.c accepts color
//   targets when this mapping and the color swap are valid.
// - SQ_SEL_XYZW01 values: Mesa src/amd/registers/gfx6.json and ac_formats.c's
//   ac_map_swizzle().
//
// The table intentionally excludes sRGB, integer, packed, depth/stencil and
// all other formats until a separate use case needs and verifies them.

#ifndef NAVI33_GFX11_FORMATS_H
#define NAVI33_GFX11_FORMATS_H

#include <stdint.h>

enum {
    N33_MTL_RGBA8_UNORM = 70,
    N33_MTL_BGRA8_UNORM = 80,
};

enum {
    N33_VK_R8G8B8A8_UNORM = 37,
    N33_VK_B8G8R8A8_UNORM = 44,
};

enum {
    N33_GFX11_SEL_ZERO = 0,
    N33_GFX11_SEL_ONE = 1,
    N33_GFX11_SEL_X = 4,
    N33_GFX11_SEL_Y = 5,
    N33_GFX11_SEL_Z = 6,
    N33_GFX11_SEL_W = 7,
};

enum {
    N33_GFX11_IMG_FMT_8_8_8_8_UNORM = 42,
    N33_GFX11_CB_FMT_8_8_8_8 = 10,
    N33_GFX11_CB_NUMBER_UNORM = 0,
    N33_GFX11_CB_SWAP_STD = 0,
    N33_GFX11_CB_SWAP_ALT = 1,
};

typedef struct Navi33Gfx11RenderTargetFormat {
    uint32_t metalPixelFormat;
    uint32_t vkFormat;            // Vulkan VkFormat numeric value
    uint16_t cbFormat;            // CB ColorFormat for a color render target
    uint8_t cbNumberType;         // CB SurfaceNumber
    uint8_t cbSwap;               // CB SurfaceSwap
    uint16_t imageFormat;         // GFX11_FORMAT for the texture descriptor
    uint8_t bytesPerTexel;
    uint8_t imageDstSelect[4];    // shader-visible R, G, B, A selectors
} Navi33Gfx11RenderTargetFormat;

#define N33_GFX11_SELECT_RGBA \
    { N33_GFX11_SEL_X, N33_GFX11_SEL_Y, N33_GFX11_SEL_Z, N33_GFX11_SEL_W }
#define N33_GFX11_SELECT_BGRA \
    { N33_GFX11_SEL_Z, N33_GFX11_SEL_Y, N33_GFX11_SEL_X, N33_GFX11_SEL_W }

static const Navi33Gfx11RenderTargetFormat kNavi33Gfx11RenderTargetFormats[] = {
    { N33_MTL_RGBA8_UNORM, N33_VK_R8G8B8A8_UNORM,
      N33_GFX11_CB_FMT_8_8_8_8, N33_GFX11_CB_NUMBER_UNORM, N33_GFX11_CB_SWAP_STD,
      N33_GFX11_IMG_FMT_8_8_8_8_UNORM, 4, N33_GFX11_SELECT_RGBA },
    { N33_MTL_BGRA8_UNORM, N33_VK_B8G8R8A8_UNORM,
      N33_GFX11_CB_FMT_8_8_8_8, N33_GFX11_CB_NUMBER_UNORM, N33_GFX11_CB_SWAP_ALT,
      N33_GFX11_IMG_FMT_8_8_8_8_UNORM, 4, N33_GFX11_SELECT_BGRA },
};

static inline const Navi33Gfx11RenderTargetFormat *
navi33Gfx11RenderTargetFormatForMetal(uint32_t metalPixelFormat)
{
    const unsigned count = (unsigned)(sizeof(kNavi33Gfx11RenderTargetFormats) /
                                      sizeof(kNavi33Gfx11RenderTargetFormats[0]));
    for (unsigned i = 0; i < count; ++i) {
        if (kNavi33Gfx11RenderTargetFormats[i].metalPixelFormat == metalPixelFormat)
            return &kNavi33Gfx11RenderTargetFormats[i];
    }
    return 0;
}

#undef N33_GFX11_SELECT_RGBA
#undef N33_GFX11_SELECT_BGRA

#endif // NAVI33_GFX11_FORMATS_H
