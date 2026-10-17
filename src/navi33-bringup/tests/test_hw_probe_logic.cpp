#include "../src/Navi33HwProbeLogic.h"
#include <cassert>
#include <cstdio>
using namespace navi33;
using namespace navi33::hwprobe;
int main() {
    assert(blank().reason==N33_HW_DISABLED);
    assert(blank().size==64u);
    uint64_t off=0u;
    assert(resolve(0x100u,0x1688u,65536u,&off));
    assert(off==(0x100ull+0x1688ull)*4ull);
    assert(!resolve(UINT64_MAX,1u,65536u,&off));
    assert(!resolve(0x100u,0xffffffu,65536u,&off));
    assert(!planHubs(IPDiscovery{},65536u).valid);
    n33_psp_status p{};
    n33_sdma_status s{};
    p.state=N33_PSP_RUNNING;p.sign_of_life=1u;
    p.sos_version=0x123u;p.read_count=3u;
    s.state=N33_SDMA_OBSERVED;s.read_count=5u;s.status_register=0x7u;
    auto r=classify(p,s,0u,0u,65536u);
    assert(r.reason==N33_HW_NO_OWNED_GPU_VM);
    assert(r.mmio_reads==10u && r.bar5_kib==64u);
    assert((r.flags&N33_HW_FLAG_HUBS_READ)!=0u);
    p.state=N33_PSP_STOPPED;
    assert(classify(p,s,0u,0u,65536u).reason==N33_HW_PSP_NOT_RUNNING);
    p.state=N33_PSP_RUNNING;s.state=N33_SDMA_READ_ERROR;
    assert(classify(p,s,0u,0u,65536u).reason==N33_HW_SDMA_READ_FAILED);
    // A dark GC block reads 0 everywhere; that is not an idle SDMA.
    s.state=N33_SDMA_OBSERVED;s.status_register=0u;
    r=classify(p,s,0u,0u,65536u);
    assert(r.reason==N33_HW_SDMA_READ_FAILED);
    assert((r.flags&N33_HW_FLAG_SDMA_READ)==0u);
    s.status_register=0x7u;s.ring_control=1u;
    assert(classify(p,s,0u,0u,65536u).reason==N33_HW_SHARED_GPU_STATE);
    s.ring_control=0u;
    assert(classify(p,s,1u,0u,65536u).reason==N33_HW_SHARED_GPU_STATE);
    assert(classify(p,s,UINT32_MAX,0u,65536u).reason==N33_HW_MMIO_READ_FAILED);
    assert(classify(p,s,0u,0u,65536u).reason!=N33_HW_COMPUTE_COMPLETE);
    std::puts("Navi33 physical hardware probe decision tests passed (synthetic inputs)");
}
