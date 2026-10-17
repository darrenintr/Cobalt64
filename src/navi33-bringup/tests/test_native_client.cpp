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
    for(uint32_t sel=N33_SEL_BO_CREATE;sel<N33_SEL_COUNT;++sel)
        assert(validate(sel,0,0,0,0,true,true,true,false)==Shape::Unsupported);
    const n33_capabilities info={N33_ABI_VERSION,sizeof(n33_capabilities),
        N33_PCI_VENDOR,N33_PCI_DEVICE,N33_GFX_IP_VERSION,N33_FAMILY_GFX1102,
        N33_CAP_PCI_INVENTORY|N33_CAP_ROM_DISCOVERY,0u,0u,0u,0u,0u};
    assert(!n33_can_submit(&info));
    assert(n33_is_reserved_clean(&info));
    std::puts("N33N root-gated read-only user-client policy tests passed");
    return 0;
}
