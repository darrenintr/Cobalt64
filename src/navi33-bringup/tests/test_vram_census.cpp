// Tests only: fake MMHUB3 BAR5 DWORDs; no GPU access.
#include "../src/Navi33VramCensus.h"
#include <array>
#include <cassert>
#include <cstdio>
using namespace navi33;
using namespace navi33::vram_census;
struct Fake {
    std::array<uint32_t,3> values{{0x100u,0x2ffu,0x40u}};
    uint64_t offsets[3]{0x1000u,0x1004u,0x1008u};
    uint32_t reads=0u;
    bool fail=false,unstable=false;
};
static bool read(void *context,uint64_t offset,uint32_t *out) {
    auto *f=static_cast<Fake *>(context);
    if(!out || f->fail)return false;
    for(size_t i=0;i<3u;++i)
        if(f->offsets[i]==offset) {
            *out=f->values[i]+(f->unstable && f->reads>=3u &&
                               i==1u ? 1u:0u);
            ++f->reads;
            return true;
        }
    return false;
}
static Plan plan() {
    Plan p{};
    p.state=N33_VRAM_OBSERVED;
    p.revision=2u;
    p.bar5Bytes=0x8000u;
    p.offsets[0]=0x1000u;p.offsets[1]=0x1004u;
    p.offsets[2]=0x1008u;
    return p;
}
int main() {
    static_assert(kFbFieldMask==0x00ffffffu,"Linux MMHUB3 FB mask");
    static_assert(kMmhub302Regs[0]==0x086cu &&
                  kMmhub302Regs[1]==0x086du &&
                  kMmhub302Regs[2]==0x0857u,"MMHUB3.0.2 offsets");
    static_assert(kMmhub300Regs[0]==0x08ecu &&
                  kMmhub300Regs[1]==0x08edu &&
                  kMmhub300Regs[2]==0x08d7u,"MMHUB3.0.0 offsets");
    assert(makePlan(IPDiscovery{},0x10000u).state==N33_VRAM_NO_INVENTORY);
    Fake f{};
    auto s=observe(plan(),false,read,&f);
    assert(s.state==N33_VRAM_DISABLED && f.reads==0u);
    s=observe(plan(),true,read,&f);
    assert(s.state==N33_VRAM_OBSERVED && s.reads==6u);
    assert(s.fbBase==(0x100ull<<24u));
    assert(s.windowBytes==((0x2ffull-0x100ull+1ull)<<24u));
    assert(s.mcFbOffset==(0x40ull<<24u));
    assert(s.fbTopInclusive==s.fbBase+s.windowBytes-1ull);
    const auto report=::navi33::vram_census::report(s,256ull<<20u);
    assert(report.state==N33_VRAM_OBSERVED &&
           report.fb_window_bytes==8ull<<30u);
    assert(report.assigned_bar0_bytes==256ull<<20u);
    assert(report.flags==(N33_VRAM_FLAG_MMIO_STABLE|
                          N33_VRAM_FLAG_BAR0_PRESENT));
    assert(report.reserved==0u);
    f.reads=0u;f.unstable=true;
    assert(observe(plan(),true,read,&f).state==N33_VRAM_UNSTABLE);
    f.unstable=false;f.reads=0u;f.fail=true;
    assert(observe(plan(),true,read,&f).state==N33_VRAM_READ_ERROR);
    f.fail=false;f.reads=0u;f.values[0]=0x300u;
    assert(observe(plan(),true,read,&f).state==N33_VRAM_INVALID_WINDOW);
    f.values[0]=0x100u;f.values[1]=0xffffffffu;f.reads=0u;
    assert(observe(plan(),true,read,&f).state==N33_VRAM_INVALID_WINDOW);
    f.values[1]=0x4000u;f.reads=0u;
    assert(observe(plan(),true,read,&f).state==N33_VRAM_INVALID_WINDOW);
    f.values[1]=0x2ffu;f.values[2]=0xfffffeu;f.reads=0u;
    assert(observe(plan(),true,read,&f).state==N33_VRAM_INVALID_WINDOW);
    f.values[2]=0x40u;f.reads=0u;
    Plan missing=plan();missing.state=N33_VRAM_NO_BAR5;
    assert(observe(missing,true,read,&f).state==N33_VRAM_NO_BAR5);
    assert(f.reads==0u);
    std::puts("MMHUB3 framebuffer window observation/malformed-register tests passed (fake BAR5)");
}
