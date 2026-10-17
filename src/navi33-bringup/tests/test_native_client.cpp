#include "../src/Navi33NativePolicy.h"
#include "../src/Navi33NativeABI.h"
#include <cassert>
#include <cstdio>
using namespace navi33::native_policy;
int main() {
    assert(openAllowed(N33_UC_TYPE,true,true,false,true)==Access::Allow);
    assert(openAllowed(N33_UC_TYPE,true,false,false,true)==Access::Deny);
    assert(openAllowed(N33_UC_TYPE,true,true,true,true)==Access::Deny);
    assert(openAllowed(N33_UC_TYPE,true,true,false,false)==Access::Deny);
    assert(openAllowed(0x4e34384eu,true,true,false,true)==Access::Deny);
    assert(validate(N33_SEL_HELLO,2,3,0,0,true,true,false,false)==Shape::Valid);
    assert(validate(N33_SEL_HELLO,2,3,0,0,false,true,false,false)==Shape::BadArgument);
    assert(validate(N33_SEL_HELLO,2,4,0,0,true,true,false,false)==Shape::BadArgument);
    assert(validate(N33_SEL_QUERY_CAPS,0,0,0,64,false,false,true,false)==Shape::Valid);
    assert(validate(N33_SEL_QUERY_CAPS,0,0,0,63,false,false,true,false)==Shape::BadArgument);
    assert(validate(N33_SEL_QUERY_CAPS,0,0,0,64,false,false,false,false)==Shape::BadArgument);
    assert(validate(N33_SEL_QUERY_CAPS,0,0,0,64,false,false,true,true)==Shape::BadArgument);
    for(uint32_t sel=N33_SEL_BO_CREATE;sel<=N33_SEL_WAIT;++sel)
        assert(validate(sel,0,0,0,0,true,true,true,false)==Shape::Unsupported);
    assert(validate(N33_SEL_PSP_STATUS,0,0,0,sizeof(n33_psp_status),false,false,true,false)==Shape::Valid);
    assert(validate(N33_SEL_PSP_STATUS,0,0,0,63,false,false,true,false)==Shape::BadArgument);
    assert(validate(N33_SEL_SDMA_STATUS,0,0,0,sizeof(n33_sdma_status),false,false,true,false)==Shape::Valid);
    assert(validate(N33_SEL_SDMA_STATUS,1,0,0,sizeof(n33_sdma_status),true,false,true,false)==Shape::BadArgument);
    assert(validate(N33_SEL_DMA_STAGE,0,0,0,64,false,false,true,false)==Shape::Valid);
    assert(validate(N33_SEL_DMA_STATUS,0,0,0,64,false,false,true,false)==Shape::Valid);
    assert(validate(N33_SEL_DMA_STAGE,0,0,0,63,false,false,true,false)==Shape::BadArgument);
    assert(validate(N33_SEL_DMA_STAGE,0,0,0,64,false,false,true,true)==Shape::BadArgument);
    assert(validate(N33_SEL_DMA_RELEASE,0,0,0,0,false,false,false,false)==Shape::Valid);
    assert(validate(N33_SEL_DMA_RELEASE,0,0,0,64,false,false,true,false)==Shape::BadArgument);
    assert(validate(N33_SEL_DMA_RELEASE,1,0,0,0,true,false,false,false)==Shape::BadArgument);
    assert(validate(N33_SEL_HW_PREFLIGHT,0,0,0,sizeof(n33_hw_preflight),
                    false,false,true,false)==Shape::Valid);
    assert(validate(N33_SEL_HW_PREFLIGHT,0,0,0,63,
                    false,false,true,false)==Shape::BadArgument);
    assert(validate(N33_SEL_HW_PREFLIGHT,0,0,0,64,
                    false,false,true,true)==Shape::BadArgument);
    assert(validate(N33_SEL_HW_ROM_PREFLIGHT,0,0,4096,64,
                    false,false,true,false)==Shape::Valid);
    assert(validate(N33_SEL_HW_ROM_PREFLIGHT,0,0,32,64,
                    false,false,true,false)==Shape::BadArgument);
    assert(validate(N33_SEL_HW_ROM_PREFLIGHT,0,0,4194305u,64,
                    false,false,true,false)==Shape::BadArgument);
    const n33_capabilities info={N33_ABI_VERSION,sizeof(n33_capabilities),
        N33_PCI_VENDOR,N33_PCI_DEVICE,N33_GFX_IP_VERSION,N33_FAMILY_GFX1102,
        N33_CAP_PCI_INVENTORY|N33_CAP_ROM_DISCOVERY,0u,0u,0u,0u,0u};
    assert(!n33_can_submit(&info));
    assert(n33_is_reserved_clean(&info));
    std::puts("N33N root-gated selectors and opt-in DMA staging policy tests passed");
    return 0;
}
