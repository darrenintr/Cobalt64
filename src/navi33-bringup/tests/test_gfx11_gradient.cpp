// SPDX-License-Identifier: MIT
#include "../src/Navi33Gfx11Gradient.h"
#include "rx7600_ipd_fixture.h"
#include <cassert>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <map>
#include <utility>
#include <vector>
using namespace navi33;
namespace gr=gradient;
namespace mq=mec_queue;
namespace gd=gfx11_dispatch;
namespace kiq=gfx11_kiq;
// A fake GC after G34: the queue is active with 16 dwords consumed. Its "CP"
// parses the PM4 the runner appends and emulates the dispatch only when the
// program address, thread masks, SH_MEM_CONFIG and kernargs are all right.
struct Fake {
    gr::Regs r; mq::Layout l{0x800fa00000ull}; gr::Screen s{0x8000000000ull,1024,768,1024};
    std::map<uint64_t,uint32_t> mem; std::map<std::pair<uint32_t,uint64_t>,uint32_t> bank;
    std::vector<uint32_t> vram=std::vector<uint32_t>(mq::kWindowBytes/4u,0u);
    std::vector<uint32_t> screen=std::vector<uint32_t>(1024u*768u,0x00203040u);
    std::vector<std::pair<uint64_t,uint32_t>> writes;
    uint32_t ms=0,rptr=16,dispatches=0; unsigned failWrite=0,attempts=0;
    bool ignorePoll=false, noMasks=false, corrupt=false, deaf=false;
    explicit Fake(gr::Regs regs):r(regs) {
        mem={{r.q.sos,42},{r.q.rlc,0xc000001fu},{r.q.grbm,0},{r.q.cntl,0x3c000000u},
             {r.q.scratch,0xdeadbeefu},{r.shMem,0x0000c000u},{r.q.poll1,0}};
        vram[mq::kFenceOff/4u]=mq::kFenceValue;vram[mq::kWptrOff/4u]=16u;
        const uint32_t sel=mq::grbmSelect(1,0,0);
        bank[{sel,mq::hqd(r.q,kiq::kCpHqdActive)}]=1u;
        bank[{sel,mq::hqd(r.q,kiq::kCpHqdPqBase)}]=uint32_t((l.windowMc+mq::kRingOff)>>8);
    }
    uint32_t sel() { return mem[r.q.grbm]; }
    bool banked(uint64_t off) const {
        for(uint32_t dw=mq::kHqdFirst;dw<=mq::kHqdLast;++dw)if(off==mq::hqd(r.q,dw))return true;
        return false;
    }
    uint32_t &at(uint64_t mc) {
        if(mc>=s.mc && mc<s.mc+4ull*screen.size())return screen[(mc-s.mc)/4u];
        assert(mc>=l.windowMc && mc<l.windowMc+mq::kWindowBytes);
        return vram[(mc-l.windowMc)/4u];
    }
    void run(uint32_t wptr) {
        if(deaf)return;
        uint32_t *ring=&vram[mq::kRingOff/4u];
        uint64_t pgm=0,kernarg=0; uint32_t masks=0,threads[3]{};
        for(uint32_t i=rptr;i<wptr;) {
            const uint32_t h=ring[i],op=(h>>8)&0xffu,n=((h>>16)&0x3fffu)+1u;
            if(h==mq::kNop){++i;continue;}
            const uint32_t *b=&ring[i+1];
            if(op==gd::kSetShReg) {
                const uint32_t reg=b[0]-gd::shOffset(0x2c00u)+0x2c00u-0x1260u+0x1260u;
                for(uint32_t k=0;k+1<n;++k) {
                    const uint32_t rg=gd::kShRegStart+b[0]+k-gd::kGcBase,v=b[1+k];
                    if(rg==gd::kPgmLo)pgm=(pgm&~0xffffffffull)|(uint64_t(v)<<8);
                    if(rg==gd::kPgmLo+1u)pgm|=uint64_t(v&0xffu)<<40;
                    if(rg==gd::kUserData0)kernarg=(kernarg&~0xffffffffull)|v;
                    if(rg==gd::kUserData0+1u)kernarg|=uint64_t(v)<<32;
                    if((rg==0x1bb6u||rg==0x1bb7u||rg==0x1bb9u||rg==0x1bbau) && v==~0u && !noMasks)++masks;
                    if(rg>=gd::kNumThreadX && rg<gd::kNumThreadX+3u)threads[rg-gd::kNumThreadX]=v;
                }
                (void)reg;
            } else if(op==gd::kDispatchDirect) {
                ++dispatches;
                // Waves launch only with all four SE masks, the shader at our
                // copy, 8x8 groups and SH_MEM_CONFIG 64-bit/unaligned.
                if(masks==4u && pgm==l.windowMc+gr::kShaderOff && threads[0]==8u && threads[1]==8u &&
                   mem[r.shMem]==gr::kShMemDefault && vram[gr::kShaderOff/4u]!=0u) {
                    const uint64_t px=at(kernarg)|(uint64_t(at(kernarg+4))<<32);
                    const uint32_t pitch=at(kernarg+8),w=at(kernarg+12),hh=at(kernarg+16),fr=at(kernarg+20);
                    for(uint32_t y=0;y<b[1]*8u;++y)for(uint32_t x=0;x<b[0]*8u;++x)
                        if(x<w && y<hh)at(px+4ull*(y*pitch+x))=
                            gfx11_shader::gradientPixel(x,y,w,hh,fr)^(corrupt && x==3 && y==5 ? 1u:0u);
                }
            } else if(op==gd::kReleaseMem) {
                at(b[2]|(uint64_t(b[3])<<32))=b[4];
            } else assert(!"unexpected packet");
            i+=1u+n;
        }
        rptr=wptr;bank[{mq::grbmSelect(1,0,0),mq::hqd(r.q,kiq::kCpHqdPqRptr)}]=rptr;
    }
    static bool read(void *c,uint64_t off,uint32_t *v) {
        auto &f=*static_cast<Fake *>(c);
        if(f.banked(off)) {
            if(off==mq::hqd(f.r.q,kiq::kCpHqdPqRptr) && f.sel()==mq::grbmSelect(1,0,0)) {*v=f.rptr;return true;}
            *v=f.bank[{f.sel(),off}];return true;
        }
        assert(f.mem.count(off));*v=f.mem[off];return true;
    }
    static bool write(void *c,uint64_t off,uint32_t v) {
        auto &f=*static_cast<Fake *>(c);
        assert(gr::allowed(f.r,off));
        if(++f.attempts==f.failWrite)return false;
        f.writes.emplace_back(off,v);
        if(f.banked(off))f.bank[{f.sel(),off}]=v; else f.mem[off]=v;
        if(off==f.r.q.poll1 && v==1u && f.sel()==mq::grbmSelect(1,0,0) && !f.ignorePoll)f.run(f.vram[mq::kWptrOff/4u]);
        if(off==mq::hqd(f.r.q,kiq::kCpHqdPqWptrLo) && f.sel()==mq::grbmSelect(1,0,0))f.run(v);
        return true;
    }
    static bool vw(void *c,uint32_t off,const uint32_t *src,uint32_t n) {
        auto &f=*static_cast<Fake *>(c);assert(off+4u*n<=mq::kWindowBytes);
        for(uint32_t i=0;i<n;++i)f.vram[off/4u+i]=src[i];
        return true;
    }
    static bool vr(void *c,uint32_t off,uint32_t *dst,uint32_t n) {
        auto &f=*static_cast<Fake *>(c);assert(off+4u*n<=mq::kWindowBytes);
        for(uint32_t i=0;i<n;++i)dst[i]=f.vram[off/4u+i];
        return true;
    }
    static bool sr(void *c,uint32_t off,uint32_t *dst) {
        auto &f=*static_cast<Fake *>(c);assert(off/4u<f.screen.size());*dst=f.screen[off/4u];return true;
    }
    static void sleep(void *c,uint32_t ms){static_cast<Fake *>(c)->ms+=ms;}
    gr::Hw hw(){return {this,read,write,vw,vr,sr,sleep};}
};
int main() {
    std::ifstream in("shaders/gradient.co",std::ios::binary);
    const std::vector<uint8_t> co((std::istreambuf_iterator<char>(in)),std::istreambuf_iterator<char>());
    assert(co.size()==gr::kCodeObjectBytes);
    auto b=rx7600_fixture::blob();IPDiscovery d;assert(d.parse(b.data(),b.size()));
    const auto r=gr::resolve(d,rx7600_fixture::kBar5);
    assert(r.valid && r.shMem==(0xa000u+0x9e4u)*4u);
    assert(!gr::allowed(r,r.q.pc) && !gr::allowed(r,r.q.cntl) && !gr::allowed(r,mq::hqd(r.q,kiq::kCpHqdActive)));
    bool attempted=false;
    {   Fake f(r);gr::Hw h=f.hw();h.write=nullptr;h.vramWrite=nullptr;
        auto o=gr::execute(r,f.l,f.s,co.data(),co.size(),h,false,true,attempted);
        assert(o.state==N33_PSPL_PLAN_OK && f.writes.empty() && !attempted && o.grad_screen==(1024u<<16|768u));
        assert(o.grad_shmem_before==0xc000u && o.grad_g34_fence==mq::kFenceValue);
        o=gr::execute(r,f.l,f.s,co.data(),co.size(),f.hw(),true,false,attempted);
        assert(o.state==N33_PSPL_WRITE_DISABLED && f.writes.empty());
    }
    {   Fake f(r);
        auto o=gr::execute(r,f.l,f.s,co.data(),co.size(),f.hw(),true,true,attempted);
        assert(o.state==N33_PSPL_GRAD_ON_SCREEN && attempted && o.steps_done==5);
        assert(!o.grad_bad_pixels && !o.grad_screen_bad && !o.grad_fallbacks && f.dispatches==2);
        assert(o.grad_fence_a==gr::kFenceValueA && o.grad_fence_b==gr::kFenceValueB && o.grad_rptr_before==16u);
        assert(o.grad_first_pixel==gfx11_shader::gradientPixel(1,1,64,64,gr::kFrameA));
        for(uint32_t y=0;y<768;y+=37)for(uint32_t x=0;x<1024;x+=41)
            assert(f.screen[y*1024+x]==gfx11_shader::gradientPixel(x,y,1024,768,0));
        const uint32_t sel=mq::grbmSelect(1,0,0);
        const std::vector<std::pair<uint64_t,uint32_t>> expected={{r.q.grbm,sel},{r.q.grbm,0},{r.shMem,0xc00cu},
            {r.q.grbm,sel},{r.q.poll1,1},{r.q.grbm,0},{r.q.grbm,sel},{r.q.poll1,1},{r.q.grbm,0}};
        assert(f.writes==expected && o.register_writes==9);
        o=gr::execute(r,f.l,f.s,co.data(),co.size(),f.hw(),true,true,attempted);
        assert(o.state!=N33_PSPL_GRAD_ON_SCREEN && f.writes.size()==9);
    }
    {   Fake f(r);gr::Screen none{};attempted=false;
        auto o=gr::execute(r,f.l,none,co.data(),co.size(),f.hw(),true,true,attempted);
        assert(o.state==N33_PSPL_GRAD_OK && f.dispatches==1 && f.screen[0]==0x00203040u);
    }
    {   Fake f(r);f.ignorePoll=true;attempted=false;   // the WPTR fallback carries both dispatches
        auto o=gr::execute(r,f.l,f.s,co.data(),co.size(),f.hw(),true,true,attempted);
        assert(o.state==N33_PSPL_GRAD_ON_SCREEN && o.grad_fallbacks==2 && o.register_writes==13);
    }
    {   Fake f(r);f.noMasks=true;attempted=false;      // waves never launch: no pixels, screen untouched
        auto o=gr::execute(r,f.l,f.s,co.data(),co.size(),f.hw(),true,true,attempted);
        assert(o.state==N33_PSPL_GRAD_BAD_PIXELS && o.grad_bad_pixels==4096u && f.screen[0]==0x00203040u);
    }
    {   Fake f(r);f.corrupt=true;attempted=false;
        auto o=gr::execute(r,f.l,f.s,co.data(),co.size(),f.hw(),true,true,attempted);
        assert(o.state==N33_PSPL_GRAD_BAD_PIXELS && o.grad_bad_pixels==1u && f.dispatches==1);
    }
    {   Fake f(r);f.deaf=true;attempted=false;
        auto o=gr::execute(r,f.l,f.s,co.data(),co.size(),f.hw(),true,true,attempted);
        assert(o.state==N33_PSPL_GRAD_NO_FENCE && o.grad_fallbacks==1 && f.mem[r.q.grbm]==0u && o.elapsed_ms==1000);
    }
    // Preconditions refuse with no write.
    const std::pair<uint64_t,uint32_t> bad[]={{r.q.cntl,0x400f0000u},{r.q.scratch,0xcafedeadu},{r.q.grbm,4u},
                                              {r.q.rlc,0x4000001fu},{r.q.sos,0u}};
    for(auto &[off,v]:bad) {
        Fake f(r);f.mem[off]=v;attempted=false;
        auto o=gr::execute(r,f.l,f.s,co.data(),co.size(),f.hw(),true,true,attempted);
        assert(o.state!=N33_PSPL_GRAD_ON_SCREEN && f.writes.empty() && !attempted);
    }
    {   Fake f(r);f.vram[mq::kWptrOff/4u]=40u;attempted=false;
        auto o=gr::execute(r,f.l,f.s,co.data(),co.size(),f.hw(),true,true,attempted);
        assert(o.state==N33_PSPL_GRAD_BAD_PRECONDITION && f.writes.empty());
    }
    {   Fake f(r);f.bank[{mq::grbmSelect(1,0,0),mq::hqd(r.q,kiq::kCpHqdActive)}]=0u;attempted=false;
        auto o=gr::execute(r,f.l,f.s,co.data(),co.size(),f.hw(),true,true,attempted);
        assert(o.state==N33_PSPL_GRAD_BAD_PRECONDITION && f.mem[r.q.grbm]==0u && f.writes.size()==2);
    }
    {   std::vector<uint8_t> other=co;other[200]^=1u;Fake f(r);attempted=false;
        auto o=gr::execute(r,f.l,f.s,other.data(),other.size(),f.hw(),true,true,attempted);
        assert(o.state==N33_PSPL_BAD_FIRMWARE && f.writes.empty() && !attempted);
    }
    for(unsigned at=1;at<=9;++at) {
        Fake f(r);f.failWrite=at;attempted=false;
        auto o=gr::execute(r,f.l,f.s,co.data(),co.size(),f.hw(),true,true,attempted);
        assert(o.state==N33_PSPL_GRAD_WRITE_FAILED && f.mem[r.q.grbm]==0u && o.register_writes==f.writes.size());
    }
    // Bare-metal console geometry (2026-10-20): 1920x1080, 7680-byte rows, 0x7e9000 bytes.
    {const auto c=gr::consoleScreen(0x8000000000ull,0x7e9000u,1920u,1080u,7680u,32u);
     assert(c.mc==0x8000000000ull && c.width==1920u && c.height==1080u && c.pitch==1920u);
     assert(gr::consoleScreen(0x8000000000ull,1024u*768u*4u,1024u,768u,4096u,32u).pitch==1024u);
     assert(!gr::consoleScreen(0x8000000000ull,0x7e9000u,1920u,1080u,7680u,16u).width);   // not 32 bpp
     assert(!gr::consoleScreen(0x8000000000ull,0x7e8000u,1920u,1080u,7680u,32u).width);   // range != rows
     assert(!gr::consoleScreen(0x8000000000ull,1366u*768u*4u,1366u,768u,5464u,32u).width);// width % 8
     assert(!gr::consoleScreen(0x8000000000ull,0x7e9000u,1920u,1080u,7682u,32u).width);   // row not dwords
     assert(!gr::consoleScreen(0x8000000000ull,4u*1000u*1080u,1920u,1080u,4000u,32u).width);}// row < width
    puts("GFX11 gradient: pinned shader, PM4 interpreted by a fake CP, test image and screen checks, fallback, gates: passed");
}
