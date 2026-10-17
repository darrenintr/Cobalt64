// Offline fake-MMIO test for the real SDMA6 register write transport.
// NO external GPU device is accessed. Real write code is compile-enabled
// here only to verify transaction ordering, timeout and pinned-memory gates.
#define COBALT_NAVI33_ENABLE_UNSAFE_MMIO 1
#include "../src/Navi33Sdma6MmioQueue.h"
#include <vector>
#include <cassert>
#include <cstdio>
using namespace navi33;
using namespace navi33::sdma6_mmio;

struct Fake {
    uint32_t regs[256]{};
    std::vector<uint8_t> mem;
    unsigned writes=0u, delays=0u, before=0u, after=0u;
    unsigned reads=0u, failWriteAt=0u;
    bool failWriteAfterCommit=false;
    bool unstableSnapshot=false, lateOwnerRace=false;
    bool lateDoorbellRace=false;
    bool triggerGpu=true;
    bool copyData=true;
    bool writeFence=true;
    bool failSync=false;
    Fake():mem(staging::kBytes,0u) {
        const dma::Segment segs[]={{0x1000000ull,staging::kBytes}};
        assert(staging::prepare(segs,1u,mem.data(),mem.size()).code==
               staging::Code::Pass);
    }
};
static bool readReg(void *p,uint32_t r,uint32_t *value) {
    auto *f=static_cast<Fake *>(p);
    if(!value || r>=256u)return false;
    ++f->reads;
    *value=f->regs[r];
    // Change the second snapshot, or the later pre-write cross-check.
    if(f->unstableSnapshot && f->reads==9u && r==kRbCntl)
        *value=kEnable;
    if(f->lateOwnerRace && f->reads==17u && r==kRbBase)
        *value=0x100u;
    if(f->lateDoorbellRace && f->reads==25u && r==kDoorbell)
        *value=1u;
    return true;
}
static bool writeReg(void *p,uint32_t r,uint32_t value) {
    auto *f=static_cast<Fake *>(p);
    if(r>=256u)return false;
    ++f->writes;
    if(f->failWriteAt==f->writes && !f->failWriteAfterCommit)
        return false;
    f->regs[r]=value;
    if(f->failWriteAt==f->writes && f->failWriteAfterCommit)
        return false; // device accepted MMIO despite software error
    if(r==kWptr && value==kCommandsBytes && f->triggerGpu) {
        if(f->copyData)
            for(size_t i=0;i<staging::kBytePage;++i)
                f->mem[staging::kTargetPage*staging::kBytePage+i]=
                f->mem[staging::kSourcePage*staging::kBytePage+i];
        if(f->writeFence)
            staging::write32(f->mem.data()+staging::kFencePage*
                             staging::kBytePage,staging::kFenceCookie);
        f->regs[kRptr]=value;
    }
    return true;
}
static bool syncBefore(void *p) {
    auto *f=static_cast<Fake *>(p);++f->before;return !f->failSync;
}
static bool syncAfter(void *p) {
    auto *f=static_cast<Fake *>(p);++f->after;return !f->failSync;
}
static bool readByte(void *p,size_t offset,uint8_t *value) {
    auto *f=static_cast<Fake *>(p);
    if(!value || offset>=f->mem.size())return false;
    *value=f->mem[offset];return true;
}
static void delay(void *p,uint32_t usec) {
    auto *f=static_cast<Fake *>(p);assert(usec>=1u);++f->delays;
}
static IO io(Fake *f) {return {f,readReg,writeReg,syncBefore,
                             syncAfter,readByte,delay};}
static Preconditions ready() {
    Preconditions p{};
    p.firmwareVerified=true;p.queueExclusivelyOwned=true;
    p.vmid0MappedByThisDriver=true;p.tlbFlushedAfterMap=true;
    p.ringGpuVaResolved=true;p.dmaOwnershipHeld=true;
    p.independentGpuRecovery=true;p.cpuGpuCacheMaintenance=true;
    return p;
}
static Request request() {
    Request r{};
    r.sourceGpuVa=staging::kGuestGpuVa;
    r.targetGpuVa=r.sourceGpuVa+staging::kBytePage;
    r.ringGpuVa=r.sourceGpuVa+2u*staging::kBytePage;
    r.fenceGpuVa=r.sourceGpuVa+3u*staging::kBytePage;
    r.cookie=staging::kFenceCookie;
    return r;
}
static void success() {
    Fake f{};
    Queue q{};
    Preconditions p=ready();
    Request r=request();
    p.tlbFlushedAfterMap=false;
    assert(q.configure(p,r,io(&f)).status==Code::Denied);
    assert(f.writes==0u);
    p.tlbFlushedAfterMap=true;
    r.ringGpuVa+=4u;
    assert(q.configure(p,r,io(&f)).status==Code::Invalid);
    assert(f.writes==0u);
    r=request();
    auto a=q.configure(p,r,io(&f));
    assert(a.status==Code::Ready && a.mmioWrites==7u);
    assert(f.regs[kRbBase]==(r.ringGpuVa>>8u));
    assert(f.regs[kRbBaseHi]==0u);
    assert(f.regs[kRbCntl] & kRbPriv);
    assert(!(f.regs[kRbCntl] & kEnable));
    assert(((f.regs[kRbCntl]&kRbSizeMask)>>1u)==10u);
    assert(!q.mayUnmapGpuMemory());
    a=q.submit();
    assert(a.status==Code::Submitted && a.mmioWrites==3u);
    assert(f.before==1u);
    assert(f.regs[kRbCntl]&kEnable);
    assert(f.regs[kWptr]==64u);
    a=q.waitAndVerify(8u);
    assert(a.status==Code::Completed && a.fence==staging::kFenceCookie);
    assert(a.finalWptr==64u && a.finalRptr==64u);
    assert(f.after>=1u);
    assert(!q.mayUnmapGpuMemory()); // stop before releasing OS IOMMU
    a=q.stopAfterCompleted();
    assert(a.status==Code::Completed);
    assert(!(f.regs[kRbCntl]&kEnable));
    assert(f.regs[kRbBase]==0u && f.regs[kRbBaseHi]==0u);
    assert(f.regs[kRptr]==0u && f.regs[kWptr]==0u);
    assert(!q.mayUnmapGpuMemory());
    assert(q.requiresIndependentResetBeforeUnmap());
    assert(!q.requiresRecovery()); // queued work retired, VM teardown unproven
    const unsigned previouslyWritten=f.writes;
    assert(q.configure(ready(),request(),io(&f)).status==Code::Busy);
    assert(f.writes==previouslyWritten); // no rearm after attempted MMIO
}
static void refusalAndFaults() {
    {
        Fake f{};f.regs[kRbCntl]=kEnable;
        Queue q{};
        assert(q.configure(ready(),request(),io(&f)).status==Code::Busy);
        assert(f.writes==0u && q.mayUnmapGpuMemory());
    }
    {
        Fake f{};f.regs[kDoorbell]=1u;
        Queue q{};
        assert(q.configure(ready(),request(),io(&f)).status==Code::Busy);
        assert(f.writes==0u);
    }
    {
        Fake f{};f.mem[staging::kRingPage*staging::kBytePage+4u]^=1u;
        Queue q{};
        assert(q.configure(ready(),request(),io(&f)).status==Code::Invalid);
        assert(f.writes==0u);
    }
    {
        Fake f{};f.regs[kRbBase]=0x10u;
        Queue q{};
        assert(q.configure(ready(),request(),io(&f)).status==Code::Busy);
        assert(f.writes==0u && q.mayUnmapGpuMemory());
    }
    {
        Fake f{};f.unstableSnapshot=true;
        Queue q{};
        assert(q.configure(ready(),request(),io(&f)).status==Code::Unstable);
        assert(f.writes==0u && !q.requiresIndependentResetBeforeUnmap());
    }
    {
        Fake f{};f.lateOwnerRace=true;
        Queue q{};
        assert(q.configure(ready(),request(),io(&f)).status==Code::Unstable);
        assert(f.writes==0u && q.mayUnmapGpuMemory());
    }
    {
        Fake f{};f.lateDoorbellRace=true;
        Queue q{};
        assert(q.configure(ready(),request(),io(&f)).status==Code::Unstable);
        assert(f.writes==0u && q.mayUnmapGpuMemory());
    }
    {
        Fake f{};f.failWriteAt=1u;f.failWriteAfterCommit=true;
        Queue q{};
        const auto o=q.configure(ready(),request(),io(&f));
        assert(o.status==Code::RecoveryRequired && o.mmioWrites==1u);
        assert(f.regs[kRbBase]!=0u); // hardware accepted failed write
        assert(q.requiresRecovery() && !q.mayUnmapGpuMemory());
        assert(q.requiresIndependentResetBeforeUnmap());
    }
    {
        Fake f{};f.failWriteAt=3u;
        Queue q{};
        const auto o=q.configure(ready(),request(),io(&f));
        assert(o.status==Code::RecoveryRequired && o.mmioWrites==3u);
        assert(q.requiresRecovery() && !q.mayUnmapGpuMemory());
        assert(q.requiresIndependentResetBeforeUnmap());
        assert(f.regs[kRbBase]!=0u); // no unsafe speculative rollback
    }
    {
        Fake f{};f.triggerGpu=false;
        Queue q{};
        assert(q.configure(ready(),request(),io(&f)).status==Code::Ready);
        assert(q.submit().status==Code::Submitted);
        auto a=q.waitAndVerify(3u,100u);
        assert(a.status==Code::Timeout && a.polls==3u && f.delays==3u);
        assert(q.requiresRecovery() && !q.mayUnmapGpuMemory());
        assert(q.stopAfterCompleted().status==Code::Denied);
    }
    {
        Fake f{};f.copyData=false;f.writeFence=true;
        Queue q{};
        assert(q.configure(ready(),request(),io(&f)).status==Code::Ready);
        assert(q.submit().status==Code::Submitted);
        assert(q.waitAndVerify(1u).status==Code::VerifyFailed);
        assert(q.requiresRecovery() && !q.mayUnmapGpuMemory());
    }
    {
        Fake f{};f.failSync=true;
        Queue q{};
        assert(q.configure(ready(),request(),io(&f)).status==Code::Ready);
        assert(q.submit().status==Code::ReadFailed);
        assert(q.abortBeforeSubmit().status==Code::Completed);
        assert(!q.mayUnmapGpuMemory());
        assert(q.requiresIndependentResetBeforeUnmap());
    }
    {
        Fake f{};
        Queue q{};
        assert(q.configure(ready(),request(),io(&f)).status==Code::Ready);
        assert(q.abortBeforeSubmit().status==Code::Completed);
        assert(!q.mayUnmapGpuMemory());
        assert(q.requiresIndependentResetBeforeUnmap());
        assert(q.configure(ready(),request(),io(&f)).status==Code::Busy);
        assert((f.regs[kRbCntl]&kEnable)==0u);
        assert(f.regs[kRbBase]==0u && f.regs[kRbBaseHi]==0u);
        assert(q.submit().status==Code::Denied);
    }
}
int main() {
    static_assert(kWriteBackendCompiled,"only mock harness opts into write code");
    success();
    refusalAndFaults();
    std::puts("Navi33 SDMA6 MMIO configuration / actual wptr / hardware-fence interface verified with fake GPU only");
    return 0;
}
