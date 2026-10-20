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
    // Client GPU selectors: scalars only, exact counts; the kext checks the boot-arg gate.
    assert(validate(N33_SEL_CTX_CREATE,1,1,0,0,true,true,false,false)==Shape::Valid);
    assert(validate(N33_SEL_BO_CREATE,2,2,0,0,true,true,false,false)==Shape::Valid);
    assert(validate(N33_SEL_BO_FREE,1,0,0,0,true,false,false,false)==Shape::Valid);
    assert(validate(N33_SEL_VM_MAP,4,0,0,0,true,false,false,false)==Shape::Valid);
    assert(validate(N33_SEL_WAIT,2,2,0,0,true,true,false,false)==Shape::Valid);
    for(uint32_t n=1;n<=N33_SUBMIT_MAX_IBS;++n)
        assert(validate(N33_SEL_SUBMIT,1+2*n,1,0,0,true,true,false,false)==Shape::Valid);
    assert(validate(N33_SEL_SUBMIT,2,1,0,0,true,true,false,false)==Shape::BadArgument);
    assert(validate(N33_SEL_SUBMIT,11,1,0,0,true,true,false,false)==Shape::BadArgument);
    assert(validate(N33_SEL_BO_CREATE,2,2,64,0,true,true,false,false)==Shape::BadArgument);   // no structs
    assert(validate(N33_SEL_BO_CREATE,2,2,0,64,true,true,true,false)==Shape::BadArgument);
    assert(validate(N33_SEL_BO_FREE,1,0,0,0,true,false,false,true)==Shape::BadArgument);    // no descriptors
    assert(validate(N33_SEL_VM_MAP,3,0,0,0,true,false,false,false)==Shape::BadArgument);
    assert(validate(N33_SEL_CTX_CREATE,1,0,0,0,true,false,false,false)==Shape::BadArgument);
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
    assert(validate(N33_SEL_VRAM_GEOMETRY,0,0,0,
                    sizeof(n33_vram_geometry_status),false,false,
                    true,false)==Shape::Valid);
    assert(validate(N33_SEL_VRAM_GEOMETRY,0,0,0,63,
                    false,false,true,false)==Shape::BadArgument);
    assert(validate(N33_SEL_VRAM_GEOMETRY,0,0,1,64,
                    false,false,true,false)==Shape::BadArgument);
    assert(validate(N33_SEL_QUEUE_CENSUS,0,0,0,
                    sizeof(n33_queue_census_status),false,false,
                    true,false)==Shape::Valid);
    assert(validate(N33_SEL_QUEUE_CENSUS,0,0,0,63,
                    false,false,true,false)==Shape::BadArgument);
    assert(validate(N33_SEL_QUEUE_CENSUS,0,0,64,64,
                    false,false,true,false)==Shape::BadArgument);
    assert(validate(N33_SEL_QUEUE_CENSUS,0,0,0,64,
                    false,false,true,true)==Shape::BadArgument);
    // MMIO peek: 32-byte request plus inline IP Discovery, 304-byte result.
    assert(validate(N33_SEL_MMIO_PEEK,0,0,32+464,sizeof(n33_peek_result),
                    false,false,true,false)==Shape::Valid);
    assert(validate(N33_SEL_MMIO_PEEK,0,0,32+63,sizeof(n33_peek_result),
                    false,false,true,false)==Shape::BadArgument);
    assert(validate(N33_SEL_MMIO_PEEK,0,0,4097,sizeof(n33_peek_result),
                    false,false,true,false)==Shape::BadArgument);
    assert(validate(N33_SEL_MMIO_PEEK,0,0,496,64,
                    false,false,true,false)==Shape::BadArgument);
    assert(validate(N33_SEL_MMIO_PEEK,1,0,496,sizeof(n33_peek_result),
                    true,false,true,false)==Shape::BadArgument);
    assert(validate(N33_SEL_MMIO_PEEK,0,0,496,sizeof(n33_peek_result),
                    false,false,true,true)==Shape::BadArgument);
    // PSP load: 32-byte request, IP Discovery and the 248 KB sOS container
    // (out of line in IOKit), 528-byte result. The write gate is a boot-arg.
    const uint32_t pspIn=32u+464u+248496u;
    assert(validate(N33_SEL_PSP_LOAD,0,0,pspIn,sizeof(n33_psp_load_result),
                    false,false,true,false)==Shape::Valid);
    assert(validate(N33_SEL_PSP_LOAD,0,0,32u+64u,sizeof(n33_psp_load_result),
                    false,false,true,false)==Shape::BadArgument);
    assert(validate(N33_SEL_PSP_LOAD,0,0,N33_PSP_LOAD_MAX_INPUT+1u,
                    sizeof(n33_psp_load_result),false,false,true,
                    false)==Shape::BadArgument);
    assert(validate(N33_SEL_PSP_LOAD,0,0,pspIn,64,
                    false,false,true,false)==Shape::BadArgument);
    assert(validate(N33_SEL_PSP_LOAD,1,0,pspIn,sizeof(n33_psp_load_result),
                    true,false,true,false)==Shape::BadArgument);
    assert(validate(N33_SEL_PSP_LOAD,0,0,pspIn,sizeof(n33_psp_load_result),
                    false,false,false,false)==Shape::BadArgument);
    assert(validate(N33_SEL_PSP_LOAD,0,0,pspIn,sizeof(n33_psp_load_result),
                    false,false,true,true)==Shape::BadArgument);
    static_assert(sizeof(n33_psp_load_request)==32u &&
                  sizeof(n33_psp_load_result)==528u,"PSP load ABI sizes");
    static_assert(N33_SEL_PSP_LOAD==18 && N33_SEL_COUNT==19,"selector numbering");
    assert(validate(N33_SEL_COUNT,0,0,0,64,
                    false,false,true,false)==Shape::Unsupported);
    static_assert(N33_ABI_VERSION==21u,"client GPU selectors ABI bump");
    const n33_capabilities info={N33_ABI_VERSION,sizeof(n33_capabilities),
        N33_PCI_VENDOR,N33_PCI_DEVICE,N33_GFX_IP_VERSION,N33_FAMILY_GFX1102,
        N33_CAP_PCI_INVENTORY|N33_CAP_ROM_DISCOVERY,0u,0u,0u,0u,0u};
    assert(!n33_can_submit(&info));
    assert(n33_is_reserved_clean(&info));
    std::puts("N33N root-gated selectors and opt-in DMA staging policy tests passed");
    return 0;
}
