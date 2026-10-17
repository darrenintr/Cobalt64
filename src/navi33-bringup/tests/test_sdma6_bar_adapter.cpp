// SPDX-License-Identifier: MIT
// BAR5 checked callbacks use synthetic MMIO, never a physical GPU.
#define COBALT_NAVI33_ENABLE_UNSAFE_MMIO 1
#include "../src/Navi33Sdma6BarAdapter.h"
#include <cassert>
#include <cstdio>
using namespace navi33;
using namespace navi33::sdma6_bar;
struct Fake { uint32_t regs[4096]{};uint32_t writes=0u; };
static bool get(void *ctx,uint64_t off,uint32_t *out) {
    if(!ctx || !out || off&3u || off/4u>=4096u) return false;
    *out=static_cast<Fake*>(ctx)->regs[off/4u];
    return true;
}
static bool put(void *ctx,uint64_t off,uint32_t val) {
    if(!ctx || off&3u || off/4u>=4096u) return false;
    auto* f=static_cast<Fake*>(ctx);
    f->regs[off/4u]=val;++f->writes;return true;
}
int main() {
    Plan p{};p.code=AddressCode::Pass;p.bar5Length=16384u;
    for(size_t i=0;i<kCount;++i)
        p.byteOffset[i]=(0x100ull+kRegs[i])*4ull;
    Fake f{};
    Adapter a{p,&f,get,put,false};
    assert(!write(&a,sdma6_mmio::kRbBase,0x1234u));
    assert(f.writes==0u);
    a.exclusiveOwned=true;
    assert(write(&a,sdma6_mmio::kRbBase,0x1234u));
    uint32_t got=0u;
    assert(read(&a,sdma6_mmio::kRbBase,&got) && got==0x1234u);
    assert(f.writes==1u);
    assert(!write(&a,sdma6_mmio::kDoorbell,1u));
    assert(!write(&a,sdma6_mmio::kMinorPtr,1u));
    assert(!write(&a,0x1234u,1u));
    assert(!read(&a,UINT32_MAX,&got));
    assert(f.writes==1u);
    p.byteOffset[1]=16385u; a.plan=p;
    assert(!write(&a,sdma6_mmio::kRbBase,0u));
    assert(f.writes==1u);
    assert(!read(nullptr,sdma6_mmio::kRbBase,&got));
    // A Queue1 capability must NEVER address the same-register Queue0
    // bank; both the allowlist and the GC-relative BAR5 translation
    // enforce the selected queue.
    Plan q1{};q1.code=AddressCode::Pass;q1.bar5Length=16384u;
    q1.queueIndex=1u;
    const uint32_t delta=sdma6_mmio::kQueue1DwordDelta;
    for(size_t i=0;i<kCount;++i)
        q1.byteOffset[i]=(0x100ull+kRegs[i]+delta)*4ull;
    assert(validPlan(q1));
    Adapter b{q1,&f,get,put,false};
    const unsigned before=f.writes;
    assert(!write(&b,sdma6_mmio::kRbBase+delta,0x2222u));
    assert(f.writes==before);
    b.exclusiveOwned=true;
    assert(write(&b,sdma6_mmio::kRbBase+delta,0x2222u));
    assert(read(&b,sdma6_mmio::kRbBase+delta,&got) && got==0x2222u);
    assert(!write(&b,sdma6_mmio::kRbBase,0x3333u));
    assert(!read(&b,sdma6_mmio::kRbCntl,&got));
    assert(!write(&b,sdma6_mmio::kDoorbell+delta,1u));
    assert(!write(&b,sdma6_mmio::kMinorPtr+delta,1u));
    assert(f.writes==before+1u);
    // Reject a register-bank spoof (wrong offset, duplicate or overflow).
    q1.byteOffset[1u]=q1.byteOffset[0u];
    b.plan=q1;
    assert(!validPlan(q1));
    assert(!write(&b,sdma6_mmio::kRbBase+delta,3u));
    q1.byteOffset[1u]=(0x100ull+kRegs[1u]+delta)*4ull;
    q1.queueIndex=2u;b.plan=q1;
    assert(!validPlan(q1));
    assert(!write(&b,sdma6_mmio::kRbBase+delta,3u));
    q1.queueIndex=1u;q1.bar5Length=4u;b.plan=q1;
    assert(!validPlan(q1));
    assert(!read(&b,sdma6_mmio::kRbBase+delta,&got));
    assert(!buildPlan(IPDiscovery{},16384u,1u).byteOffset[0u]);
    assert(buildPlan(IPDiscovery{},16384u,2u).code==
           AddressCode::InventoryRejected);
    std::puts("GC11 BAR5 register allowlist / writes / bounds passed (FAKE MMIO)");
}
