// Synthetic BAR0 aperture VRAM, no PCI/GPU hardware accessed.
#define COBALT_NAVI33_ENABLE_UNSAFE_VRAM_UPLOAD 1
#include "../src/Navi33Gmc11VramUpload.h"
#include <array>
#include <cassert>
#include <cstdio>
using namespace navi33;
namespace up=vram_upload;
struct FakeBar0 {
    std::array<uint64_t,vram_root::kRootWords> words{};
    uint64_t base=0x400000ull;
    unsigned readCount=0u,writeCount=0u;
    bool failWrite=false,failSync=false,corruptWrite=false;
    bool failPostRead=false;
};
static bool read64(void *opaque,uint64_t offset,uint64_t *out) {
    FakeBar0 &f=*static_cast<FakeBar0 *>(opaque);
    if(!out || offset<f.base || ((offset-f.base)&7u) ||
       (offset-f.base)/8u>=f.words.size() ||
       (f.failPostRead && f.readCount>=512u))
        return false;
    ++f.readCount;
    *out=f.words[(offset-f.base)/8u];
    return true;
}
static bool write64(void *opaque,uint64_t offset,uint64_t value) {
    FakeBar0 &f=*static_cast<FakeBar0 *>(opaque);
    if(offset<f.base || ((offset-f.base)&7u) ||
       (offset-f.base)/8u>=f.words.size() ||
       (f.failWrite && f.writeCount>=7u))
        return false;
    ++f.writeCount;
    f.words[(offset-f.base)/8u]=f.corruptWrite ? value^1ull:value;
    return true;
}
static bool sync(void *opaque) {
    return !static_cast<FakeBar0 *>(opaque)->failSync;
}
static up::IO io(FakeBar0 &f) {
    return {&f,read64,write64,sync};
}
static vram_geometry::Geometry geo() {
    vram_geometry::PhysicalInventory p{};
    p.realBytes=8ull*1024ull*1024ull*1024ull;
    p.assignedBar0Bytes=256ull*1024ull*1024ull;
    p.independentlyVerifiedMemsize=true;
    auto r=vram_geometry::decode(
        {0x6000u,0x61ffu,0x100u},{0x6000u,0x61ffu,0x100u},p);
    assert(r.code==vram_geometry::Code::Pass);
    return r.geometry;
}
static vram_bo::Authority auth() {
    vram_bo::Authority g{};
    g.pciDeviceExclusive=true;
    g.allDisplayClientsStopped=true;
    g.firmwareAndDisplayRangesInventoried=true;
    g.independentRecoveryAvailable=true;
    return g;
}
static up::Gate gate() {
    up::Gate g{};
    g.pciExclusivelyOwned=true;
    g.noDisplayOrFirmwareVRAMClients=true;
    g.physicalVramBoIsOwnedAndPinned=true;
    g.bar0ResourceExclusivelyMapped=true;
    g.independentResetAndRecovery=true;
    return g;
}
static mmhub11::Table table() {
    mmhub11::Table t{};
    t.gpuPhysicalPageTable=geo().physicalFbBase+0x400000ull;
    for(size_t i=0u;i<staging::kPages;++i) {
        auto p=gmc11::makeSystemPte(0x800000ull+i*4096ull,i!=0u);
        assert(p.code==gmc11::Code::Pass);
        t.entries[i]=p.bits;
    }
    assert(mmhub11::validTable(t));
    return t;
}
static void exercise(unsigned scenario) {
    int owner=0;
    FakeBar0 f{};
    vram_bo::Arena arena{};
    const auto g=geo();
    assert(arena.configure(g,auth(),{0x400000ull,4096ull},
                           nullptr,0u)==vram_bo::Code::Pass);
    auto lease=arena.allocate(&owner,4096u);
    assert(lease.code==vram_bo::Code::Pass);
    auto root=table();
    up::Gate requested=gate();
    if(scenario==1)requested.noDisplayOrFirmwareVRAMClients=false;
    if(scenario==2)f.words[300u]=1ull; // suspected prior owner
    if(scenario==3)f.failWrite=true;
    if(scenario==4)f.failSync=true;
    if(scenario==5)f.corruptWrite=true;
    if(scenario==6)f.failPostRead=true;
    if(scenario==7)root.gpuPhysicalPageTable+=4096ull;
    const auto r=up::uploadRoot(requested,g,arena,&owner,
                                lease.token,root,io(f));
    if(scenario==0) {
        assert(r.code==up::Code::UploadedButGpuVmNotInitialized);
        assert(r.reads==1024u && r.writes==512u);
        assert(r.rootPhysical==root.gpuPhysicalPageTable);
        for(size_t i=0u;i<f.words.size();++i)
            assert(f.words[i]==(i<staging::kPages?root.entries[i]:0ull));
        assert(arena.isQuarantined());
        assert(!arena.release(&owner,lease.token));
        assert(up::uploadRoot(requested,g,arena,&owner,lease.token,
                              root,io(f)).code==up::Code::Refused);
    } else if(scenario==1 || scenario==2 || scenario==7) {
        assert(r.code==(scenario==2?up::Code::RootNotClean:
                       up::Code::Refused));
        assert(!r.mustResetBeforeRelease);
        assert(!arena.isQuarantined() && f.writeCount==0u);
        assert(arena.release(&owner,lease.token));
    } else {
        assert(r.code==(scenario==3?up::Code::WriteFailure:
                       scenario==4?up::Code::SyncFailure:
                       scenario==5?up::Code::VerifyFailure:
                                   up::Code::ReadFailure));
        assert(r.mustResetBeforeRelease && arena.isQuarantined());
        assert(!arena.release(&owner,lease.token));
    }
    assert(!r.actualGpuExecutionProven);
}
int main() {
    static_assert(up::kWritePathCompiled,"fake backend explicitly opted in");
    for(unsigned i=0u;i<8u;++i)exercise(i);
    std::puts("VRAM GART root 4KiB BAR0 upload/readback with fake GPU only; no physical writes");
}
