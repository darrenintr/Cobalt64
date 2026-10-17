// Mock MMHUB3 / GC11 register reading; no physical PCI resource.
#include "../src/Navi33Gmc11VramLive.h"
#include <vector>
#include <cassert>
#include <cstdio>
using namespace navi33;
namespace live=vram_live;
struct Fake {
    std::vector<uint32_t> regs=std::vector<uint32_t>(131072u/4u);
    uint64_t unstableOffset=0u;
    uint32_t reads=0u;
};
static bool read(void *v,uint64_t offset,uint32_t *value) {
    auto &f=*static_cast<Fake*>(v);
    if(!value || (offset&3u) || offset/4u>=f.regs.size())
        return false;
    ++f.reads;
    *value=f.regs[offset/4u];
    if(f.unstableOffset==offset && f.reads>6u)(*value)++;
    return true;
}
static IPEntry mm(uint8_t rev) {
    IPEntry e{};
    e.hwId=ipid::MMHUB;
    e.major=3u;e.minor=0u;e.revision=rev;
    e.numBases=1u;e.bases[0]=0x100u;
    return e;
}
static IPEntry gc() {
    IPEntry e{};
    e.hwId=ipid::GC;
    e.major=11u;e.minor=0u;e.revision=2u;
    e.numBases=1u;e.bases[0]=0x100u;
    return e;
}
static void set(Fake &f,const live::Plan &p) {
    const uint32_t values[6u]={0x6000u,0x61ffu,0x100u,
                                0x6000u,0x61ffu,0x100u};
    for(size_t i=0;i<6u;++i)f.regs[p.offsets[i]/4u]=values[i];
}
int main() {
    auto p=live::fromEntries(mm(2u),gc(),true,131072u);
    assert(p.code==live::PlanCode::Pass && p.mmRevision==2u);
    assert(p.offsets[0]==(0x100ull+0x086cull)*4ull);
    assert(p.offsets[1]==(0x100ull+0x086dull)*4ull);
    assert(p.offsets[2]==(0x100ull+0x0857ull)*4ull);
    assert(p.offsets[3]==(0x100ull+0x1678ull)*4ull);
    assert(p.offsets[4]==(0x100ull+0x1679ull)*4ull);
    assert(p.offsets[5]==(0x100ull+0x15a7ull)*4ull);
    assert(live::fromEntries(mm(0u),gc(),true,131072u)
           .offsets[0]==(0x100ull+0x08ecull)*4ull);
    assert(live::fromEntries(mm(1u),gc(),true,131072u).code==
           live::PlanCode::UnsupportedVersion);
    assert(live::fromEntries(mm(2u),gc(),false,131072u).code==
           live::PlanCode::NoInventory);
    assert(live::fromEntries(mm(2u),gc(),true,16u).code==
           live::PlanCode::InvalidBar5);
    Fake f{};set(f,p);
    auto snapshot=live::inspect(p,read,&f);
    assert(snapshot.state==N33_VRAM_PAIRED_READ);
    assert(snapshot.reads==12u && f.reads==12u);
    assert(snapshot.flags&N33_VRAM_FLAG_PAIRED);
    auto result=live::report(snapshot,256ull*1024ull*1024ull,
                             131072u,p.mmRevision);
    assert(result.abi_version==N33_ABI_VERSION && result.size==64u);
    assert(result.mm_fb_base==0x6000u && result.gfx_fb_base==0x6000u);
    assert(result.mm_fb_offset==0x100u && result.gfx_fb_offset==0x100u);
    assert(result.candidate_physical_fb_base==(0x100ull<<24u));
    assert(result.bar0_mib==256u && result.bar5_kib==128u);
    f.regs[p.offsets[4]/4u]=0x61feu;
    assert(live::inspect(p,read,&f).state==N33_VRAM_DISAGREE);
    f.regs[p.offsets[4]/4u]=0x61ffu;
    f.reads=0u;f.unstableOffset=p.offsets[1];
    assert(live::inspect(p,read,&f).state==N33_VRAM_UNSTABLE);
    f.unstableOffset=0u;f.reads=0u;
    f.regs[p.offsets[0]/4u]=UINT32_MAX;
    assert(live::inspect(p,read,&f).state==N33_VRAM_MMIO_FAILED);
    assert(live::inspect({},read,&f).state==N33_VRAM_BAD_OFFSETS);
    const live::Sample blank{};
    assert(live::report(blank,0u,0u,0u).state==N33_VRAM_DISABLED);
    std::puts("Paired GMC11 FB geometry BAR5 plans and stable readback OK (fake MMIO)");
}
