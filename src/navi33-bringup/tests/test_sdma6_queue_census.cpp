// No device access: synthetic BAR5 status registers, not real hardware.
#include "../src/Navi33Sdma6QueueCensus.h"
#include "../src/Navi33Sdma6MmioQueue.h" // host-only constant cross-check
#include <cassert>
#include <cstdio>
using namespace navi33;
namespace c=queue_census;
struct Fake {
    uint32_t registers[c::kCount]{};
    unsigned reads=0u,failAt=0u;
    bool allOnes=false,unstable=false;
};
static bool read(void *opaque,uint64_t offset,uint32_t *out) {
    auto &f=*static_cast<Fake *>(opaque);
    if(!out || offset<0x1000u || (offset&3u))return false;
    const uint64_t idx=(offset-0x1000u)/4u;
    if(idx>=c::kCount)return false;
    ++f.reads;
    if(f.failAt==f.reads)return false;
    *out=f.allOnes ? UINT32_MAX : f.registers[idx];
    if(f.unstable && f.reads>c::kCount && idx==0u)++*out;
    return true;
}
static c::Plan plan() {
    c::Plan p{};
    p.state=N33_QUEUE_OBSERVED;
    p.bar5Bytes=65536u;
    for(size_t i=0u;i<c::kCount;++i)
        p.offsets[i]=0x1000ull+i*4u;
    assert(c::validPlan(p));
    return p;
}
int main() {
    static_assert(c::kRegisters[0]==sdma6_mmio::kRbCntl,"Queue0 CNTL");
    static_assert(c::kRegisters[1]==sdma6_mmio::kDoorbell,"Queue0 doorbell");
    static_assert(c::kRegisters[2]==sdma6_mmio::kRptr,"Queue0 RPTR");
    static_assert(c::kRegisters[3]==sdma6_mmio::kRptrHi,"Queue0 RPTR_HI");
    static_assert(c::kRegisters[4]==sdma6_mmio::kWptr,"Queue0 WPTR");
    static_assert(c::kRegisters[5]==sdma6_mmio::kWptrHi,"Queue0 WPTR_HI");
    static_assert(c::kRegisters[6]==sdma6_mmio::kRbBase,"Queue0 BASE");
    static_assert(c::kRegisters[7]==sdma6_mmio::kRbBaseHi,"Queue0 BASE_HI");
    static_assert(!c::kCanWriteRegisters && !c::kCanAcquireQueue,
                  "production observer MUST NOT control GPU");
    const auto good=plan();
    Fake f{};
    assert(c::readStable(good,false,read,&f).state==N33_QUEUE_DISABLED);
    assert(f.reads==0u);
    assert(c::readStable({},true,read,&f).state==N33_QUEUE_NO_INVENTORY);
    assert(c::readStable(good,true,nullptr,&f).state==N33_QUEUE_READ_ERROR);
    assert(c::fromDiscovery(IPDiscovery{},65536u).state==
           N33_QUEUE_NO_INVENTORY);
    auto sample=c::readStable(good,true,read,&f);
    assert(sample.state==N33_QUEUE_OBSERVED && sample.reads==16u);
    assert(f.reads==16u &&
           sample.flags==N33_QUEUE_FLAG_APPARENTLY_PARKED);
    auto result=c::report(sample);
    assert(result.abi_version==9u && result.size==64u);
    assert(result.read_count==16u && result.ring_base_lo==0u);
    assert(result.reserved0==0u && result.reserved1==0u);

    f=Fake{};f.registers[0]=1u;f.registers[1]=2u;
    f.registers[2]=3u;f.registers[5]=4u;
    f.registers[6]=5u;f.registers[7]=6u;
    sample=c::readStable(good,true,read,&f);
    result=c::report(sample);
    assert(sample.state==N33_QUEUE_OBSERVED);
    assert((result.flags&(N33_QUEUE_FLAG_ENABLED|N33_QUEUE_FLAG_DOORBELL|
                          N33_QUEUE_FLAG_POINTERS|N33_QUEUE_FLAG_RING_BASE))==
           (N33_QUEUE_FLAG_ENABLED|N33_QUEUE_FLAG_DOORBELL|
            N33_QUEUE_FLAG_POINTERS|N33_QUEUE_FLAG_RING_BASE));
    assert(!(result.flags&N33_QUEUE_FLAG_APPARENTLY_PARKED));
    assert(result.doorbell_control==2u && result.ring_rptr_lo==3u);
    assert(result.ring_wptr_hi==4u && result.ring_base_lo==5u);
    assert(result.ring_base_hi==6u);

    f=Fake{};f.failAt=3u;
    sample=c::readStable(good,true,read,&f);
    assert(sample.state==N33_QUEUE_READ_ERROR && sample.reads==2u);
    result=c::report(sample);
    assert(result.ring_control==0u && result.flags==0u);
    f=Fake{};f.allOnes=true;
    assert(c::readStable(good,true,read,&f).state==N33_QUEUE_INVALID_READ);
    f=Fake{};f.unstable=true;
    sample=c::readStable(good,true,read,&f);
    assert(sample.state==N33_QUEUE_UNSTABLE && sample.reads==16u);
    assert(c::report(sample).ring_base_lo==0u);

    auto forged=good;forged.offsets[7]=forged.offsets[6];
    f=Fake{};
    assert(c::readStable(forged,true,read,&f).state==
           N33_QUEUE_BAD_MMIO_PLAN && f.reads==0u);
    forged=good;forged.offsets[0]=forged.bar5Bytes;
    assert(c::readStable(forged,true,read,&f).state==
           N33_QUEUE_BAD_MMIO_PLAN && f.reads==0u);
    forged=good;forged.bar5Bytes=1u;
    assert(!c::validPlan(forged));
    std::puts("Queue0 16-read stable physical register census: fake MMIO tests passed");
}
