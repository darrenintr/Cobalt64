// CPU-only VRAM BO/root readback contract. NO GPU or physical BAR0 access.
#include "../src/Navi33Gmc11RootVram.h"
#include <array>
#include <cassert>
#include <cstdio>
using namespace navi33;
namespace vr=vram_root;
struct MockVram {
    uint64_t root=0x400000ull;
    std::array<uint64_t,vr::kRootWords> words{};
    bool failRead=false;
    size_t reads=0u;
};
static bool read(void *opaque,uint64_t addr,uint64_t *value) {
    auto &f=*static_cast<MockVram*>(opaque);
    if(!value || f.failRead || addr<f.root || ((addr-f.root)%8u) ||
       (addr-f.root)/8u>=f.words.size())return false;
    ++f.reads;*value=f.words[(addr-f.root)/8u];
    return true;
}
static mmhub11::Table table() {
    mmhub11::Table t{};
    t.gpuPhysicalPageTable=0x400000ull;
    for(size_t i=0;i<staging::kPages;++i) {
        const auto p=gmc11::makeSystemPte(0x800000ull+i*4096ull,i!=0u);
        assert(p.code==gmc11::Code::Pass);
        t.entries[i]=p.bits;
    }
    return t;
}
static vr::Reservation reservation() {
    vr::Reservation r{};
    r.mcVramStart=0x200000ull;r.mcVramBytes=8ull*1024u*1024u;
    r.ownedBoMcStart=0x400000ull;r.ownedBoBytes=4096ull;
    r.rootMcAddress=0x400000ull;return r;
}
int main() {
    const auto t=table();
    MockVram m{};
    for(size_t i=0;i<staging::kPages;++i)m.words[i]=t.entries[i];
    auto r=vr::inspect(reservation(),t,read,&m);
    assert(r.code==vr::Code::ImageMatches &&
           r.checkedWords==vr::kRootWords &&
           m.reads==vr::kRootWords &&
           !r.hardwareVmReady);
    assert(vr::inspect(reservation(),t,nullptr,&m).code==
           vr::Code::NoReadback);
    m.words[staging::kPages+1u]=0x9999u;
    assert(vr::inspect(reservation(),t,read,&m).code==
           vr::Code::ContentMismatch);
    m.words[staging::kPages+1u]=0u;
    m.failRead=true;
    assert(vr::inspect(reservation(),t,read,&m).code==
           vr::Code::ReadFailed);
    m.failRead=false;
    auto bad=reservation();bad.ownedBoMcStart=0x600000ull;
    assert(vr::inspect(bad,t,read,&m).code==
           vr::Code::RootOutsideOwnedBo);
    bad=reservation();bad.mcVramStart=0x500000ull;
    assert(vr::inspect(bad,t,read,&m).code==
           vr::Code::RootOutsideVram);
    bad=reservation();bad.rootMcAddress=0x400004ull;
    assert(vr::inspect(bad,t,read,&m).code==vr::Code::Invalid);
    bad=reservation();bad.ownedBoBytes=2048ull;
    assert(vr::inspect(bad,t,read,&m).code==vr::Code::Invalid);
    auto invalid=t;invalid.entries[0]^=gmc11::kPteValid;
    assert(vr::inspect(reservation(),invalid,read,&m).code==
           vr::Code::Invalid);
    std::puts("GMC11 dedicated VRAM root size/ownership/readback contract OK (mock VRAM)");
}
