// Mock-only end-to-end test. No physical GPU is contacted.
#define COBALT_NAVI33_ENABLE_UNSAFE_GMC11_MMIO 1
#define COBALT_NAVI33_ENABLE_UNSAFE_MMIO 1
#include "../src/Navi33Sdma6ProofRun.h"
#include <array>
#include <vector>
#include <cassert>
#include <cstdio>
using namespace navi33;
namespace p=sdma6_proof;
namespace h=mmhub11;
namespace s=sdma6_mmio;
struct Fake {
    std::array<uint32_t,32768> regs{};
    std::vector<uint8_t> mem=std::vector<uint8_t>(staging::kBytes);
    h::Plan mm{},gfx{};
    std::array<uint64_t,vram_root::kRootWords> rootPtes{};
    vram_geometry::Geometry geometry{};
    vram_bo::Arena rootArena{};
    vram_bo::Token rootToken{};
    queue_owner::Registry lease{};
    bool corruptRoot=false;
    unsigned writes=0u;
    bool failCopy=false,failAck=false,failQueue=false;
    Fake() {
        const dma::Segment seg[]={{0x800000ull,staging::kBytes}};
        assert(staging::prepare(seg,1u,mem.data(),mem.size()).code==staging::Code::Pass);
        IPEntry a{};a.hwId=ipid::MMHUB;a.major=3;a.revision=2;a.numBases=1;
        a.bases[0]=0x100u;
        mm=h::planFromEntry(a,true,131072u);
        a.hwId=ipid::GC;a.major=11;a.revision=2;a.bases[0]=0x100u;
        gfx=h::planGfxFromEntry(a,true,131072u);
        assert(h::validPlan(mm)&&h::validPlan(gfx));
        geometry.mcStart=0u;
        geometry.mcWindowBytes=8ull*1024ull*1024ull*1024ull;
        geometry.physicalFbBase=0u;
        geometry.realBytes=geometry.mcWindowBytes;
        geometry.cpuVisibleBytes=256ull*1024ull*1024ull;
        vram_bo::Authority owner{};
        owner.pciDeviceExclusive=true;
        owner.allDisplayClientsStopped=true;
        owner.firmwareAndDisplayRangesInventoried=true;
        owner.independentRecoveryAvailable=true;
        assert(rootArena.configure(geometry,owner,{0x400000ull,0x10000ull},
                                   nullptr,0u)==vram_bo::Code::Pass);
        const auto root=rootArena.allocate(this,staging::kBytePage);
        assert(root.code==vram_bo::Code::Pass);
        assert(root.address.gpuPhysicalAddress==0x400000ull);
        rootToken=root.token;
    }
};
static bool hubRead(void *v,uint64_t offset,uint32_t *out) {
    auto &f=*static_cast<Fake*>(v);
    if(!out || offset%4u || offset/4u>=f.regs.size())return false;
    if(offset==f.mm.offsets[static_cast<size_t>(h::Register::InvalidateSem)]) {
        *out=1u;return true;
    }
    *out=f.regs[offset/4u];return true;
}
static bool hubWrite(void *v,uint64_t offset,uint32_t value) {
    auto &f=*static_cast<Fake*>(v);
    if(offset%4u || offset/4u>=f.regs.size())return false;
    ++f.writes;f.regs[offset/4u]=value;
    if(!f.failAck) for(const auto &plan:{f.mm,f.gfx})
        if(offset==plan.offsets[static_cast<size_t>(h::Register::InvalidateReq)])
            f.regs[plan.offsets[static_cast<size_t>(h::Register::InvalidateAck)]/4u]=1u;
    return true;
}
static bool yes(void*) {return true;}
static void hubDelay(void*,unsigned) {}
static h::IO hubIO(Fake &f){return {&f,hubRead,hubWrite,yes,yes,hubDelay};}
static bool sdmaRead(void *v,uint32_t reg,uint32_t *out) {
    auto &f=*static_cast<Fake*>(v);
    if(!out || reg>=256u)return false;
    *out=f.regs[reg];return true;
}
static bool sdmaWrite(void *v,uint32_t reg,uint32_t word) {
    auto &f=*static_cast<Fake*>(v);
    if(reg>=256u)return false;
    ++f.writes;f.regs[reg]=word;
    if(reg==s::kWptr && word==s::kCommandsBytes && !f.failQueue) {
        if(!f.failCopy) for(size_t i=0;i<staging::kBytePage;++i)
            f.mem[staging::kBytePage+i]=f.mem[i];
        staging::write32(f.mem.data()+3u*staging::kBytePage,
                         staging::kFenceCookie);
        f.regs[s::kRptr]=word;
    }
    return true;
}
static bool readByte(void *v,size_t offset,uint8_t *out) {
    auto &f=*static_cast<Fake*>(v);
    if(!out || offset>=f.mem.size())return false;
    *out=f.mem[offset];return true;
}
static void sdmaDelay(void*,uint32_t) {}
static s::IO sdmaIO(Fake &f) {
    return {&f,sdmaRead,sdmaWrite,yes,yes,readByte,sdmaDelay};
}
static h::Table table() {
    h::Table t{};t.gpuPhysicalPageTable=0x400000u;
    for(size_t i=0;i<staging::kPages;++i) {
        const auto v=gmc11::makeSystemPte(0x800000u+i*4096u,i!=0u);
        assert(v.code==gmc11::Code::Pass);
        t.entries[i]=v.bits;
    }
    return t;
}
static s::Request request() {
    s::Request r{};
    r.sourceGpuVa=staging::kGuestGpuVa;
    r.targetGpuVa=r.sourceGpuVa+4096u;
    r.ringGpuVa=r.sourceGpuVa+8192u;
    r.fenceGpuVa=r.sourceGpuVa+12288u;
    r.cookie=staging::kFenceCookie;return r;
}
static p::Isolation isolation() {
    p::Isolation g{};
    g.pci1002_7480Owned=true;g.exclusiveGpuAndBothVmHubs=true;
    g.noActiveDisplayOrOtherGpuClients=true;
    g.pspAndSdmaFirmwareVerified=true;g.mmhubCacheAndHdpInitialized=true;
    g.ownedPhysicalGpuPageTablePinned=true;g.dmaIommuMappingHeld=true;
    g.deviceCacheMaintenanceAvailable=true;g.exclusiveSdma0Queue=true;
    g.independentPowerCycleAndRecovery=true;return g;
}
static bool readVramRoot(void *v,uint64_t addr,uint64_t *out) {
    Fake &f=*static_cast<Fake*>(v);
    if(!out || addr<0x400000ull || (addr&7ull) ||
       (addr-0x400000ull)/8u>=f.rootPtes.size())return false;
    *out=f.rootPtes[(addr-0x400000ull)/8u];return true;
}
static p::Report run(Fake &f,p::Isolation e=isolation()) {
    const auto t=table();
    for(size_t i=0;i<staging::kPages;++i)
        f.rootPtes[i]=t.entries[i]; // model a separately pinned VRAM BO
    if(f.corruptRoot)f.rootPtes[100u]=0xdeadbeefull;
    return p::run(e,f.mm,f.gfx,t,request(),f.geometry,
                  f.rootArena,f.rootToken,readVramRoot,&f,f.lease,&f,
                  hubIO(f),hubIO(f),sdmaIO(f),4u,4u);
}
int main() {
    static_assert(p::kWritePathCompiled,"Only a fake backend can enable unsafe code");
    {
        Fake f;auto e=isolation();e.independentPowerCycleAndRecovery=false;
        const auto r=run(f,e);
        assert(r.verdict==p::Verdict::Refused && f.writes==0u);
    }
    {
        Fake f;f.gfx.offsets[0]=f.mm.offsets[0];
        const auto r=run(f);
        assert(r.verdict==p::Verdict::Refused && f.writes==0u);
    }
    {
        Fake f;auto r=run(f);
        assert(r.verdict==p::Verdict::CopiedButVmStillActive);
        assert(r.ringWasSubmitted && r.fourKiBCopyVerified);
        assert(r.observedFence==staging::kFenceCookie);
        assert(!r.stagingMayBeReleased && r.physicalResetBeforeRelease);
        assert(f.lease.phase()==queue_owner::Phase::GpuMayHaveFetched);
        assert(f.rootArena.isQuarantined());
        assert(!f.rootArena.release(&f,f.rootToken));
        const unsigned written=f.writes;
        assert(run(f).verdict==p::Verdict::Refused);
        assert(f.writes==written); // second job cannot reprogram Queue0
    }
    {
        Fake f;assert(f.rootArena.release(&f,f.rootToken));
        assert(run(f).verdict==p::Verdict::Refused);
        assert(f.writes==0u);
    }
    {
        Fake f;f.corruptRoot=true;
        assert(run(f).verdict==p::Verdict::Refused);
        assert(f.writes==0u);
        assert(f.lease.phase()==queue_owner::Phase::Available);
    }
    {
        Fake f;f.failQueue=true;auto r=run(f);
        assert(r.verdict==p::Verdict::FenceOrCopyFailed);
        assert(r.ringWasSubmitted && !r.fourKiBCopyVerified);
        assert(!r.stagingMayBeReleased && r.physicalResetBeforeRelease);
    }
    {
        Fake f;f.failCopy=true;auto r=run(f);
        assert(r.verdict==p::Verdict::FenceOrCopyFailed);
        assert(r.ringWasSubmitted && !r.fourKiBCopyVerified);
    }
    {
        Fake f;f.failAck=true;auto r=run(f);
        assert(r.verdict==p::Verdict::MmhubActivateFailed);
        assert(!r.ringWasSubmitted && r.physicalResetBeforeRelease);
    }
    std::puts("Dual-hub SDMA6 COPY/FENCE orchestration verified on FAKE GPU only");
}
