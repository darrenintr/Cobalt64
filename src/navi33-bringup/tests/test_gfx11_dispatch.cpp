// Host-only checks of the gfx1102 PM4 dispatch encoder against the committed
// shaders/gradient.co. Nothing is submitted. No GPU access.
#include "../src/Navi33Gfx11Dispatch.h"
#include <cassert>
#include <cstdio>
#include <vector>

using namespace navi33;
using namespace navi33::gfx11_dispatch;

namespace {
std::vector<uint8_t> load(const char *path) {
    std::vector<uint8_t> b; FILE *f=std::fopen(path,"rb");
    assert(f && "run from src/navi33-bringup");
    for(int c;(c=std::fgetc(f))!=EOF;) b.push_back(static_cast<uint8_t>(c));
    std::fclose(f); return b;
}
} // namespace

int main() {
    const std::vector<uint8_t> co=load("shaders/gradient.co");
    const gfx11_shader::Kernel k=gfx11_shader::parse(co.data(),co.size(),"cobalt_gradient");
    assert(k.code==gfx11_shader::Code::Ok);
    // 1024x768 console, 8x8 groups -> 128x96 groups.
    Request r{}; r.kernel=&k;
    r.shaderMc=0x800f808000ull; r.kernargMc=0x800f806000ull;
    r.fenceMc=0x800f807000ull; r.fenceValue=0x600dcafeu;
    r.groupsX=128u; r.groupsY=96u;
    const Stream s=encode(r);
    assert(s.code==Code::Ok);
    const uint32_t expect[]={
        0xc0027600u,0x20cu,0x800f8080u,0x00u,             // PGM_LO/HI
        0xc0027600u,0x212u,0xe0af0001u,0x0000099cu,       // PGM_RSRC1/2
        0xc0017600u,0x228u,0x40u,                         // PGM_RSRC3
        0xc0017600u,0x215u,0u,                            // RESOURCE_LIMITS
        0xc0017600u,0x218u,0u,                            // TMPRING_SIZE: no scratch
        0xc0037600u,0x204u,0u,0u,0u,                      // START_X/Y/Z
        0xc0037600u,0x207u,8u,8u,1u,                      // NUM_THREAD_X/Y/Z
    };
    const size_t n=sizeof(expect)/sizeof(expect[0]);
    for(size_t i=0;i<n;++i) assert(s.dwords[i]==expect[i]);
    // USER_DATA_0..13: kernarg pointer, then zeros.
    assert(s.dwords[n]==0xc00e7600u && s.dwords[n+1]==0x240u);
    assert(s.dwords[n+2]==0x0f806000u && s.dwords[n+3]==0x80u);
    for(size_t i=4;i<16;++i) assert(s.dwords[n+i]==0u);
    const size_t d=n+16;
    assert(s.dwords[d]==0xc0031500u && s.dwords[d+1]==128u && s.dwords[d+2]==96u &&
           s.dwords[d+3]==1u && s.dwords[d+4]==0x8005u);  // SHADER_EN|START_000|W32
    // RELEASE_MEM as gfx_v11_0_ring_emit_fence (32-bit, no IRQ).
    assert(s.dwords[d+5]==0xc0064900u && s.dwords[d+6]==0x06603514u &&
           s.dwords[d+7]==0x20000000u && s.dwords[d+8]==0x0f807000u &&
           s.dwords[d+9]==0x80u && s.dwords[d+10]==0x600dcafeu &&
           s.dwords[d+11]==0u && s.dwords[d+12]==0u);
    assert(s.count==d+13);
    // Refusals.
    Request b=r; b.shaderMc|=0x80u; assert(encode(b).code==Code::BadAddress);
    b=r; b.kernargMc|=2u;            assert(encode(b).code==Code::BadAddress);
    b=r; b.fenceMc=0u;               assert(encode(b).code==Code::BadAddress);
    b=r; b.groupsX=0u;               assert(encode(b).code==Code::BadGrid);
    b=r; b.groupsZ=2u;               assert(encode(b).code==Code::BadGrid); // no TGID_Z
    b=r; b.threadsX=64u; b.threadsY=32u; assert(encode(b).code==Code::BadGrid);
    b=r; b.kernel=nullptr;           assert(encode(b).code==Code::BadKernel);
    // Kernel arguments for the console: base, pitch, width, height, frame.
    uint8_t args[24];
    gradientKernargs(args,0x8000000000ull,1024u,1024u,768u,3u);
    const uint8_t want[24]={0,0,0,0,0x80,0,0,0, 0,4,0,0, 0,4,0,0, 0,3,0,0, 3,0,0,0};
    for(int i=0;i<24;++i) assert(args[i]==want[i]);
    std::printf("gfx1102 dispatch: %zu PM4 dwords (program, resources, kernarg in "
                "USER_DATA_0/1, DISPATCH_DIRECT wave32, RELEASE_MEM fence), refusals checked\n",
                s.count);
}
