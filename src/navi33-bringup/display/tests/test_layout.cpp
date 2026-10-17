#include "../src/Navi33GopLayout.h"
#include "../src/Navi33BarGuard.h"
#include <cassert>
#include <cstdio>
#include <cstdint>
using navi33fb::LayoutError;
using navi33fb::inspectGop;
using navi33fb::valid;
static_assert(valid(inspectGop(0x840000001ull,0,1920,1080,7680,32)),
              "Firmware low address flags must be ignored");
static_assert(!valid(inspectGop(0,0,1920,1080,7680,32)),
              "Framebuffer without physical address must fail closed");

int main() {
    const auto normal = inspectGop(0x840000001ull,0,1920,1080,7680,32);
    assert(valid(normal));
    assert(normal.physicalBase==0x840000000ull);
    assert(normal.length==7680ull*1080ull);
    assert(normal.width==1920&&normal.depth==32);

    assert(!valid(inspectGop(0,0,1920,1080,7680,32)));
    assert(inspectGop(0x10000000,0,0,1080,7680,32).error==LayoutError::InvalidDimension);
    assert(inspectGop(0x10000000,0,9000,1080,36000,32).error==LayoutError::InvalidDimension);
    // RDNA4FB accepts 24/30 GOP depths only when the *stride* is 4B/px.
    const auto d24=inspectGop(0x10000000,0,1920,1080,7680,24);
    const auto d30=inspectGop(0x10000000,0,1920,1080,7680,30);
    assert(valid(d24)&&d24.depth==32&&d24.firmwareDepth==24);
    assert(valid(d30)&&d30.depth==32&&d30.firmwareDepth==30);
    assert(inspectGop(0x10000000,0,1920,1080,5760,24).error==LayoutError::InvalidPitch);
    assert(inspectGop(0x10000000,0,1920,1080,7680,16).error==LayoutError::UnsupportedDepth);
    assert(inspectGop(0x10000000,0,1920,1080,7600,32).error==LayoutError::InvalidPitch);
    assert(inspectGop(0x10000000,0,1920,1080,7681,32).error==LayoutError::InvalidPitch);
    assert(inspectGop(0x10000000,65536,1920,1080,7680,32).error==LayoutError::InvalidLength);
    assert(inspectGop(0x10000000,0,8192,8192,32768,32).error==LayoutError::Ok);
    assert(inspectGop(0x10000000,0,8192,8192,65536,32).error==LayoutError::InvalidLength);
    assert(inspectGop(0xfffffffffffff001ull,0,1920,1080,7680,32).error==LayoutError::Overflow);

    for (uint32_t id=0;id<6;++id) {
        const auto c=navi33fb::channelMapping(id);
        assert(c.red<3&&c.green<3&&c.blue<3);
        assert(c.red!=c.green&&c.red!=c.blue&&c.green!=c.blue);
        assert((navi33fb::channelMask(c.red)&navi33fb::channelMask(c.blue))==0u);
    }
    using navi33fb::checkBar0;
    using navi33fb::ownedByBar0;
    const auto good = checkBar0(normal, 0x840000000ull, 256ull << 20);
    assert(ownedByBar0(good) && good.offset == 0);
    assert(ownedByBar0(checkBar0(normal, 0x83ff00000ull, 256ull << 20)));
    assert(!ownedByBar0(checkBar0(normal, 0, 0)));
    assert(!ownedByBar0(checkBar0(normal, 0x840001000ull, 256ull << 20)));
    assert(!ownedByBar0(checkBar0(normal, 0x840000000ull, 0x1000u)));
    assert(!ownedByBar0(checkBar0(normal, 0xffffffffffff0000ull, 256ull << 20)));
    assert(!ownedByBar0(checkBar0(inspectGop(0, 0, 1920, 1080, 7680, 32),
                                   0x840000000ull, 256ull << 20)));
    assert(ownedByBar0(checkBar0(normal, 0x840000000ull, 0x840000ull)));
    const auto defaultChannels=navi33fb::channelMapping(0);
    assert(defaultChannels.red==2&&defaultChannels.green==1&&defaultChannels.blue==0);
    std::puts("Navi33 GOP geometry / BAR0 ownership / pitch / bounds / pixel masks: PASS (CPU-only)");
}
