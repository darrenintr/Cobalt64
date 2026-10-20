// Navi33 NBIO 4.3 physical VRAM size: simulated MMIO ONLY.
// This verifies register offsets, capacity and paired FB consistency.
#include "../src/Navi33VramSurvey.h"
#include <cassert>
#include <cstdio>
#include <vector>
using namespace navi33;
namespace n=nbio4;
namespace fb=vram_live;
namespace survey=vram_survey;
struct Registers {
    std::vector<uint32_t> words=std::vector<uint32_t>(32768u,0u);
    uint64_t memOffset=0u;
    uint32_t reads=0u;
    bool unstable=false;
    bool readError=false;
};
static bool read(void *ctx,uint64_t offset,uint32_t *out) {
    Registers &r=*static_cast<Registers*>(ctx);
    if(!out||r.readError||(offset&3u)||offset/4u>=r.words.size())
        return false;
    ++r.reads;
    *out=r.words[offset/4u];
    if(r.unstable && offset==r.memOffset && r.reads>=2u)++*out;
    return true;
}
static IPEntry nbif(uint8_t revision=0u) {
    IPEntry r{};
    r.hwId=ipid::NBIF;r.instance=0u;
    r.major=4u;r.minor=3u;r.revision=revision;
    r.numBases=3u;
    r.bases[0u]=0x100u;r.bases[1u]=0x200u;
    r.bases[2u]=0x300u;
    return r;
}
static n33_vram_geometry_status paired() {
    fb::Sample s{};
    s.state=N33_VRAM_PAIRED_READ;
    s.flags=N33_VRAM_FLAG_PAIRED;
    s.reads=12u;
    s.raw[0]=s.raw[3]=0x6000u;
    s.raw[1]=s.raw[4]=0x61ffu;
    s.raw[2]=s.raw[5]=0x100u;
    return fb::report(s,256ull*1024ull*1024ull,131072u,2u);
}
int main() {
    const auto p=n::fromEntry(nbif(),true,131072u);
    assert(p.code==n::Code::Pass);
    assert(p.byteOffset==(0x300ull+0x00c3ull)*4ull);
    assert(p.nbioRevision==0x00040300u);
    assert(n::fromEntry(nbif(1u),true,131072u).code==n::Code::Pass);
    assert(n::fromEntry(nbif(3u),true,131072u).code==
           n::Code::UnsupportedRevision);
    assert(n::fromEntry(nbif(),false,131072u).code==n::Code::NoInventory);
    auto noBase=nbif();noBase.numBases=2u;
    assert(n::fromEntry(noBase,true,131072u).code==n::Code::MissingBase);
    auto badBase=nbif();badBase.bases[2u]=UINT64_MAX;
    assert(n::fromEntry(badBase,true,131072u).code==n::Code::OutsideBar5);
    assert(n::fromEntry(nbif(),true,16u).code==n::Code::OutsideBar5);

    Registers regs{};
    regs.memOffset=p.byteOffset;
    regs.words[p.byteOffset/4u]=8192u;
    const auto sample=n::inspect(p,read,&regs);
    assert(sample.code==n::Code::Pass&&sample.capacityMiB==8192u);
    assert(sample.readCount==2u);
    const auto good=survey::combine(paired(),sample);
    assert(good.nbio_state==N33_NBIO_STABLE);
    assert(good.nbio_memsize_mib==8192u);
    assert(good.nbio_revision==0x00040300u);
    assert((good.flags&N33_VRAM_FLAG_MC_GEOMETRY_VALID)!=0u);
    assert((good.flags&N33_VRAM_FLAG_CAPACITY_VALID)!=0u);
    assert(good.verified_vram_bytes==8192ull*1024ull*1024ull);
    assert(good.verified_mc_start==(0x6000ull<<24u));

    regs.reads=0u;regs.unstable=true;
    assert(n::inspect(p,read,&regs).code==n::Code::Unstable);
    regs.unstable=false;regs.reads=0u;
    regs.words[p.byteOffset/4u]=UINT32_MAX;
    assert(n::inspect(p,read,&regs).code==n::Code::ReadFailed);
    regs.words[p.byteOffset/4u]=0u;regs.reads=0u;
    assert(n::inspect(p,read,&regs).code==n::Code::InvalidCapacity);
    regs.words[p.byteOffset/4u]=8200u;regs.reads=0u;
    assert(n::inspect(p,read,&regs).code==n::Code::InvalidCapacity);
    // The real RX 7600 value (0x1ff0 = 8 GiB minus 16 MiB) must pass.
    regs.words[p.byteOffset/4u]=8176u;regs.reads=0u;
    assert(n::inspect(p,read,&regs).code==n::Code::Pass && n::inspect(p,read,&regs).capacityMiB==8176u);
    regs.words[p.byteOffset/4u]=16384u;regs.reads=0u;
    const auto larger=n::inspect(p,read,&regs);
    assert(larger.code==n::Code::Pass);
    const auto mismatch=survey::combine(paired(),larger);
    assert(mismatch.nbio_state==N33_NBIO_GEOMETRY_MISMATCH);
    assert((mismatch.flags&N33_VRAM_FLAG_CAPACITY_VALID)!=0u);
    assert((mismatch.flags&N33_VRAM_FLAG_MC_GEOMETRY_VALID)==0u);
    assert(mismatch.verified_vram_bytes==0u);
    regs.words[p.byteOffset/4u]=8192u;regs.readError=true;
    assert(n::inspect(p,read,&regs).code==n::Code::ReadFailed);
    assert(n::inspect(p,nullptr,nullptr).code==n::Code::ReadFailed);
    const auto without=n::inspect({},read,&regs);
    assert(without.code==n::Code::NoInventory);
    auto only=paired();
    only.flags&=~N33_VRAM_FLAG_BAR0_ASSIGNED;
    assert(survey::combine(only,sample).verified_vram_bytes==0u);
    only=paired();only.state=N33_VRAM_DISAGREE;
    assert(survey::combine(only,sample).verified_vram_bytes==0u);
    assert(sizeof(n33_vram_geometry_status)==96u);
    std::puts("NBIO4.3 independent MiB memory size and paired GPU address space checks OK (fake MMIO)");
}
