// GMC11 MMHUB 3.0.x MMIO tests: all registers and DMA pages are fake.
#define COBALT_NAVI33_ENABLE_UNSAFE_GMC11_MMIO 1
#include "../src/Navi33Gmc11Mmhub.h"
#include <cassert>
#include <cstdio>
#include <vector>
using namespace navi33;
using namespace navi33::mmhub11;

struct Fake {
    std::vector<uint32_t> words;
    unsigned writes=0u, polls=0u, syncs=0u, hdps=0u;
    unsigned failAtWrite=0u, failAgainAtWrite=0u;
    bool ackOnInvalidate=true, acquireSemaphore=true, badSync=false;
    uint64_t sem=0u,req=0u,ack=0u;
    Fake():words(65536u/4u,0u){}
};
static bool read(void *ctx,uint64_t off,uint32_t *value) {
    auto *f=static_cast<Fake *>(ctx);
    if(!value || (off&3ull) || off/4u>=f->words.size())return false;
    if(off==f->sem) {
        *value=f->acquireSemaphore ? 1u : 0u;
        return true;
    }
    *value=f->words[off/4u];
    return true;
}
static bool write(void *ctx,uint64_t off,uint32_t value) {
    auto *f=static_cast<Fake *>(ctx);
    if((off&3ull) || off/4u>=f->words.size())return false;
    ++f->writes;
    if(f->failAtWrite==f->writes || f->failAgainAtWrite==f->writes)
        return false;
    f->words[off/4u]=value;
    if(off==f->req && f->ackOnInvalidate)
        f->words[f->ack/4u]|=kInvalidateVmid0;
    return true;
}
static bool sync(void *ctx) {
    auto *f=static_cast<Fake *>(ctx);++f->syncs;return !f->badSync;
}
static bool hdp(void *ctx) {
    auto *f=static_cast<Fake *>(ctx);++f->hdps;return !f->badSync;
}
static void delay(void *ctx,unsigned microseconds) {
    auto *f=static_cast<Fake *>(ctx);
    assert(microseconds>0u);++f->polls;
}
static IO callbacks(Fake &f) {
    return {&f,read,write,sync,hdp,delay};
}
static Gate gate() {
    Gate g{};
    g.dedicatedGpuOwnership=true;
    g.noActiveDisplayConsumers=true;
    g.vmhubExclusivelyOwned=true;
    g.noOutstandingGpuJobs=true;
    g.firmwareAndHdpVerified=true;
    g.mmhubCacheAlreadyConfigured=true;
    g.gpuTableMemoryPinnedAndTrusted=true;
    g.isolatedPowerCycleAvailable=true;
    return g;
}
static Table table() {
    Table t{};
    t.gpuPhysicalPageTable=0x400000ull;
    for(size_t i=0;i<staging::kPages;++i) {
        const auto pte=gmc11::makeSystemPte(0x800000ull+i*4096ull,
                                             i!=staging::kSourcePage);
        assert(pte.code==gmc11::Code::Pass);
        t.entries[i]=pte.bits;
    }
    return t;
}
static Plan makePlan(uint8_t revision=2u) {
    IPEntry mmhub{};
    mmhub.hwId=ipid::MMHUB;
    mmhub.major=3u;mmhub.minor=0u;mmhub.revision=revision;
    mmhub.numBases=1u;mmhub.bases[0]=0x100u;
    return planFromEntry(mmhub,true,65536u);
}
static Fake fakeFor(const Plan &p) {
    Fake f{};
    f.sem=p.offsets[static_cast<size_t>(Register::InvalidateSem)];
    f.req=p.offsets[static_cast<size_t>(Register::InvalidateReq)];
    f.ack=p.offsets[static_cast<size_t>(Register::InvalidateAck)];
    return f;
}
static void offsetTests() {
    const auto p=makePlan();
    assert(p.code==Code::Prepared && validPlan(p));
    assert(p.offsets[0u]==(0x100ull+0x06c0ull)*4ull);
    assert(p.offsets[1u]==(0x100ull+0x072bull)*4ull);
    assert(p.offsets[8u]==(0x100ull+0x06f4ull)*4ull);
    assert(p.offsets[9u]==(0x100ull+0x0706ull)*4ull);
    assert(p.offsets[12u]==(0x100ull+0x069bull)*4ull);
    const auto older=makePlan(0u);
    assert(older.code==Code::Prepared && validPlan(older));
    assert(older.offsets[0u]==(0x100ull+0x0740ull)*4ull);
    assert(older.offsets[1u]==(0x100ull+0x07abull)*4ull);
    assert(older.offsets[8u]==(0x100ull+0x0774ull)*4ull);
    assert(makePlan(1u).code==Code::UnsupportedVersion);
    IPEntry entry{};entry.hwId=ipid::MMHUB;entry.major=3u;
    entry.minor=0u;entry.revision=2u;entry.numBases=1u;
    entry.bases[0]=0xffffffffu;
    assert(planFromEntry(entry,true,65536u).code==Code::InvalidBar);
    assert(planFromEntry(entry,false,65536u).code==Code::NoDiscovery);
    entry.bases[0]=0x100u;
    assert(planFromEntry(entry,true,16u).code==Code::InvalidBar);
    Plan tampered=p;
    tampered.offsets[4u]=p.offsets[3u];
    assert(!validPlan(tampered));
    tampered=p;tampered.offsets[5u]=p.barBytes+4u;
    assert(!validPlan(tampered));
    tampered=p;
    tampered.offsets[5u]+=4u; // aligned and within BAR5 but wrong register
    assert(!validPlan(tampered));
    assert(planFromDiscovery(IPDiscovery{},65536u).code==Code::NoDiscovery);
}
static void successfulMockFlush() {
    const Plan p=makePlan();
    Fake f=fakeFor(p);
    Controller vm{};
    Gate g=gate();
    g.vmhubExclusivelyOwned=false;
    assert(vm.prepare(p,g,table(),callbacks(f)).code==Code::Denied);
    assert(f.writes==0u);
    g.vmhubExclusivelyOwned=true;
    Table t=table();
    t.entries[0u]|=gmc11::kPteWriteable;
    assert(vm.prepare(p,g,t,callbacks(f)).code==Code::InvalidTable);
    assert(f.writes==0u);
    auto a=vm.prepare(p,g,table(),callbacks(f));
    assert(a.code==Code::Prepared && a.writes==8u);
    assert(f.syncs==1u);
    assert(f.words[p.offsets[1u]/4u]==0x400000u);
    assert(f.words[p.offsets[3u]/4u]==(staging::kGuestGpuVa>>12u));
    assert(f.words[p.offsets[5u]/4u]==
           ((staging::kGuestGpuVa+staging::kBytes-1u)>>12u));
    assert((f.words[p.offsets[0u]/4u]&kContextEnable)==0u);
    assert(!vm.canUseGpuPageTable() && !vm.mayReleaseMappedMemory());
    a=vm.invalidateAndActivate(8u,10u);
    assert(a.code==Code::Flushed && a.acknowledgement==1u);
    assert(vm.canUseGpuPageTable() && vm.requiresRecovery());
    assert(!vm.mayReleaseMappedMemory());
    assert(f.hdps==1u && f.words[f.req/4u]==kInvalidateRequest);
    assert(f.words[f.sem/4u]==0u);
    assert((f.words[p.offsets[12u]/4u]&kPrivateInvalidateBit)!=0u);
    assert(vm.abortBeforeActivation().code==Code::Denied);
}
static void gfxhubMockFlush() {
    IPEntry gc{};
    gc.hwId=ipid::GC;
    gc.instance=0u;gc.major=11u;gc.minor=0u;gc.revision=2u;
    gc.numBases=1u;gc.bases[0u]=0x100u;
    const Plan p=planGfxFromEntry(gc,true,65536u);
    assert(p.hub==Hub::Gfxhub && p.code==Code::Prepared);
    assert(validPlan(p));
    assert(p.offsets[0u]==(0x100ull+0x1688ull)*4ull);
    assert(p.offsets[1u]==(0x100ull+0x16f3ull)*4ull);
    assert(p.offsets[8u]==(0x100ull+0x16bcull)*4ull);
    assert(p.offsets[9u]==(0x100ull+0x16ceull)*4ull);
    assert(p.offsets[10u]==(0x100ull+0x16f1ull)*4ull);
    assert(p.offsets[12u]==(0x100ull+0x15d7ull)*4ull);
    IPEntry wrong=gc;wrong.revision=3u;
    assert(planGfxFromEntry(wrong,true,65536u).code==
           Code::UnsupportedVersion);
    assert(planGfxFromEntry(gc,true,1024u).code==Code::InvalidBar);
    Fake f=fakeFor(p);
    f.acquireSemaphore=false; // GFXHUB must NOT acquire MMHUB's semaphore
    Controller vm{};
    assert(vm.prepare(p,gate(),table(),callbacks(f)).code==Code::Prepared);
    const auto result=vm.invalidateAndActivate(5u,10u);
    assert(result.code==Code::Flushed);
    assert(result.acknowledgement==1u);
    assert(f.hdps==1u && f.polls==0u);
    assert(f.words[f.sem/4u]==0u); // untouched GFXHUB semaphore
    assert(f.words[f.req/4u]==kInvalidateRequest);
    assert(f.words[p.offsets[12u]/4u]==0u); // no MMHUB private flush on GC
    assert(vm.canUseGpuPageTable());
    assert(vm.requiresRecovery() && !vm.mayReleaseMappedMemory());

    const Plan mp=makePlan(2u);
    Fake mf=fakeFor(mp);
    Controller mm{};
    assert(!dualHubReady(mm,vm)); // one flush is insufficient
    assert(mm.prepare(mp,gate(),table(),callbacks(mf)).code==Code::Prepared);
    assert(!dualHubReady(mm,vm)); // merely programming registers is not enough
    assert(mm.invalidateAndActivate(5u,10u).code==Code::Flushed);
    assert(dualHubReady(mm,vm));
    assert(!dualHubReady(vm,mm)); // hub identity matters
    assert(mm.verifiedTableAddress()==0x400000ull);
    assert(vm.verifiedTableAddress()==0x400000ull);
}
static void cancellationAndFaults() {
    {
        auto p=makePlan();Fake f=fakeFor(p);Controller vm{};
        assert(vm.prepare(p,gate(),table(),callbacks(f)).code==Code::Prepared);
        auto o=vm.abortBeforeActivation();
        assert(o.code==Code::Aborted && o.writes==8u);
        assert(vm.mayReleaseMappedMemory());
        for(unsigned i: {1u,2u,3u,4u,5u,6u,10u,11u})
            assert(f.words[p.offsets[i]/4u]==0u);
    }
    {
        auto p=makePlan();Fake f=fakeFor(p);Controller vm{};
        f.words[p.offsets[0u]/4u]=kContextEnable;
        assert(vm.prepare(p,gate(),table(),callbacks(f)).code==Code::Busy);
        assert(f.writes==0u);
    }
    {
        auto p=makePlan();Fake f=fakeFor(p);Controller vm{};
        f.words[f.ack/4u]=1u;
        assert(vm.prepare(p,gate(),table(),callbacks(f)).code==Code::Busy);
        assert(f.writes==0u);
    }
    {
        auto p=makePlan();Fake f=fakeFor(p);Controller vm{};
        f.failAtWrite=3u;
        assert(vm.prepare(p,gate(),table(),callbacks(f)).code==Code::WriteFailure);
        assert(vm.mayReleaseMappedMemory());
        for(unsigned i: {1u,2u,3u,4u,5u,6u,10u,11u})
            assert(f.words[p.offsets[i]/4u]==0u);
    }
    {
        auto p=makePlan();Fake f=fakeFor(p);Controller vm{};
        f.failAtWrite=3u;
        f.failAgainAtWrite=4u;
        assert(vm.prepare(p,gate(),table(),callbacks(f)).code==
               Code::RecoveryRequired);
        assert(vm.requiresRecovery() && !vm.mayReleaseMappedMemory());
    }
    {
        auto p=makePlan();Fake f=fakeFor(p);Controller vm{};
        assert(vm.prepare(p,gate(),table(),callbacks(f)).code==Code::Prepared);
        f.acquireSemaphore=false;
        assert(vm.invalidateAndActivate(3u,5u).code==Code::Timeout);
        assert(vm.requiresRecovery() && !vm.mayReleaseMappedMemory());
        assert(f.polls==3u);
    }
    {
        auto p=makePlan();Fake f=fakeFor(p);Controller vm{};
        assert(vm.prepare(p,gate(),table(),callbacks(f)).code==Code::Prepared);
        f.ackOnInvalidate=false;
        assert(vm.invalidateAndActivate(4u,5u).code==Code::Timeout);
        assert(vm.requiresRecovery() && !vm.mayReleaseMappedMemory());
        assert(f.polls==4u); // ACK wait: four delays
    }
    {
        auto p=makePlan();Fake f=fakeFor(p);Controller vm{};
        f.badSync=true;
        assert(vm.prepare(p,gate(),table(),callbacks(f)).code==Code::Denied);
        assert(f.writes==0u);
    }
}
int main() {
    static_assert(kWritePathCompiled,"mock harness only");
    offsetTests();
    successfulMockFlush();
    gfxhubMockFlush();
    cancellationAndFaults();
    std::puts("GMC11 MMHUB3 VMID0 MMIO programming / TLB ACK tested on FAKE registers only");
}
