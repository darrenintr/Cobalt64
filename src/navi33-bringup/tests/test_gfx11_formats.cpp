#include "../src/Navi33Gfx11Formats.h"
#include <cstdio>

static unsigned failures;

static void expect(bool condition, const char *what)
{
    if (!condition) {
        std::printf("FAIL: %s\n", what);
        ++failures;
    }
}

static bool selectorsEqual(const Navi33Gfx11RenderTargetFormat *f,
                           uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    return f->imageDstSelect[0] == r && f->imageDstSelect[1] == g &&
           f->imageDstSelect[2] == b && f->imageDstSelect[3] == a;
}

int main()
{
    const unsigned count = (unsigned)(sizeof(kNavi33Gfx11RenderTargetFormats) /
                                      sizeof(kNavi33Gfx11RenderTargetFormats[0]));
    expect(count == 2, "only the reviewed RGBA8Unorm and BGRA8Unorm targets are mapped");

    const Navi33Gfx11RenderTargetFormat *rgba =
        navi33Gfx11RenderTargetFormatForMetal(N33_MTL_RGBA8_UNORM);
    expect(rgba != 0, "RGBA8Unorm lookup succeeds");
    if (rgba) {
        expect(rgba->vkFormat == N33_VK_R8G8B8A8_UNORM,
               "RGBA8Unorm maps to Vulkan R8G8B8A8_UNORM");
        expect(rgba->cbFormat == N33_GFX11_CB_FMT_8_8_8_8 &&
               rgba->cbNumberType == N33_GFX11_CB_NUMBER_UNORM &&
               rgba->cbSwap == N33_GFX11_CB_SWAP_STD,
               "RGBA8Unorm uses GFX11 COLOR_8_8_8_8 / UNORM / SWAP_STD");
        expect(rgba->imageFormat == N33_GFX11_IMG_FMT_8_8_8_8_UNORM &&
               rgba->bytesPerTexel == 4,
               "RGBA8Unorm texture descriptor has the matching format and texel size");
        expect(selectorsEqual(rgba, N33_GFX11_SEL_X, N33_GFX11_SEL_Y,
                              N33_GFX11_SEL_Z, N33_GFX11_SEL_W),
               "RGBA8Unorm texture channels stay in RGBA order");
    }

    const Navi33Gfx11RenderTargetFormat *bgra =
        navi33Gfx11RenderTargetFormatForMetal(N33_MTL_BGRA8_UNORM);
    expect(bgra != 0, "BGRA8Unorm lookup succeeds");
    if (bgra) {
        expect(bgra->vkFormat == N33_VK_B8G8R8A8_UNORM,
               "BGRA8Unorm maps to Vulkan B8G8R8A8_UNORM");
        expect(bgra->cbFormat == N33_GFX11_CB_FMT_8_8_8_8 &&
               bgra->cbNumberType == N33_GFX11_CB_NUMBER_UNORM &&
               bgra->cbSwap == N33_GFX11_CB_SWAP_ALT,
               "BGRA8Unorm uses GFX11 COLOR_8_8_8_8 / UNORM / SWAP_ALT");
        expect(bgra->imageFormat == N33_GFX11_IMG_FMT_8_8_8_8_UNORM &&
               bgra->bytesPerTexel == 4,
               "BGRA8Unorm texture descriptor has the matching format and texel size");
        expect(selectorsEqual(bgra, N33_GFX11_SEL_Z, N33_GFX11_SEL_Y,
                              N33_GFX11_SEL_X, N33_GFX11_SEL_W),
               "BGRA8Unorm texture swizzle restores logical RGBA channels");
        expect(bgra != rgba, "RGBA and BGRA are separate mapping rows");
    }

    expect(navi33Gfx11RenderTargetFormatForMetal(0xBEEFu) == 0,
           "unknown, packed, and depth/stencil formats fail closed");
    expect(navi33Gfx11RenderTargetFormatForMetal(0u) == 0,
           "invalid Metal format fails closed");

    if (failures) {
        std::printf("GFX11 render-target mapping: %u failure(s)\n", failures);
        return 1;
    }
    std::puts("GFX11 RGBA/BGRA render-target mapping passed (offline enum checks only)");
    return 0;
}
