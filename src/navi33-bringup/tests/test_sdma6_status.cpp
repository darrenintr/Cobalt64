#include "../src/Navi33Sdma6Status.h"
#include <cassert>
#include <cstdio>
using namespace navi33;
using namespace navi33::sdma6live;
struct Fake {unsigned calls=0u,failAt=0u;bool ones=false;};
static bool read(void *ctx,uint64_t offset,uint32_t *dst) {
    auto* fake=static_cast<Fake*>(ctx);
    ++fake->calls;
    if(fake->calls==fake->failAt)return false;
    *dst=fake->ones?UINT32_MAX:static_cast<uint32_t>(offset/4u);
    return true;
}
int main(){
    Plan plan{};plan.state=N33_SDMA_OBSERVED;
    for(size_t i=0;i<kRegisterCount;++i)plan.offsets[i]=0x1000u+i*4u;
    Fake f{};
    assert(readOnce(plan,false,read,&f).state==N33_SDMA_DISABLED && f.calls==0u);
    assert(readOnce(plan,true,nullptr,&f).state==N33_SDMA_READ_ERROR);
    assert(readOnce(Plan{},true,read,&f).state==N33_SDMA_NO_INVENTORY);
    auto s=readOnce(plan,true,read,&f);
    assert(s.state==N33_SDMA_OBSERVED && s.readCount==5u && f.calls==5u);
    assert(s.values[0]==0x400u && s.values[4]==0x404u);
    const n33_sdma_status out=report(s);
    assert(out.size==64u && out.ring_wptr==0x404u);
    assert(out.reserved1==0u && out.reserved4==0u);
    f=Fake{};f.failAt=3u;
    s=readOnce(plan,true,read,&f);
    assert(s.state==N33_SDMA_READ_ERROR);
    assert(report(s).ring_wptr==0u); // no partially exposed stale pointers
    f=Fake{};f.ones=true;
    assert(readOnce(plan,true,read,&f).state==N33_SDMA_INVALID_READ);
    plan.offsets[2]=plan.offsets[1];f=Fake{};
    assert(readOnce(plan,true,read,&f).state==N33_SDMA_BAD_MMIO_PLAN);
    assert(f.calls==0u);
    static_assert(!kMaySubmitSdma && !kMayRingDoorbell,"no GPU writes");
    std::puts("SDMA6 GFX11 queue status validation passed (mock MMIO only)");
}
