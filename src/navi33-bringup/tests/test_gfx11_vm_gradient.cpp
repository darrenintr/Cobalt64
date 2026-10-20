// SPDX-License-Identifier: MIT
#include "../src/Navi33Gfx11VmGradient.h"
#include "rx7600_ipd_fixture.h"
#include <cassert>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <map>
#include <utility>
#include <vector>
using namespace navi33;
namespace vg=vm_gradient;
namespace v1=vmid1;
namespace gr=gradient;
namespace mq=mec_queue;
namespace gd=gfx11_dispatch;
namespace kiq=gfx11_kiq;
// A fake GC after the autostart. Its GFXHUB translates VMID 1 only once the hub is
// set up as gfxhub_v3_0 does and context 1 is enabled at depth 3, by walking
// PDB2 -> PDB1 -> PDB0 -> PTB in fake VRAM (the window, plus the console at VRAM
// 0). The queue-1 "CP" fetches its ring through that walk, interprets the ring
// test and the PM4 the runner appends, and launches waves only when VMID 1's
// SH_MEM_CONFIG is set; the waves read their kernargs and write their pixels
// through the same walk.
struct Fake {
    vg::Regs r; vg::Layout l{0x800fc00000ull,0x0fc00000ull};
    vg::Screen s{0u,1024u*768u*4u,1024,768,1024};
    std::map<uint64_t,uint32_t> mem; std::map<std::pair<uint32_t,uint64_t>,uint32_t> bank;
    std::vector<uint32_t> vram=std::vector<uint32_t>(vg::kWindowBytes/4u,0x5a5a5a5au);
    std::vector<uint32_t> screen=std::vector<uint32_t>(1024u*768u,0x00203040u);
    std::vector<std::pair<uint64_t,uint32_t>> writes;
    unsigned failWrite=0,attempts=0,walks=0; uint32_t ms=0,rptr=0,dispatches=0;
    bool loaded=false,flat=false,badPde=false,ignorePoll=false,deaf=false;
    explicit Fake(vg::Regs regs):r(regs) {
        const v1::Regs &v=r.v;
        mem={{v.q.sos,42},{v.q.rlc,0xc000001fu},{v.q.grbm,0},{v.q.cntl,0x3c000000u},{v.q.scratch,0xdeadbeefu},
             {v.q.poll1,0},{v.l1,v1::kL1Default},{v.l2,v1::kL2Default},{v.l2c2,0},{v.l2c3,v1::kL2Cntl3Default},
             {v.l2c4,v1::kL2Cntl4Default},{v.l2c5,v1::kL2Cntl5Default},{v.ctx,v1::kCtxDefault},{v.baseLo,0},
             {v.baseHi,0},{v.startLo,0},{v.startHi,0},{v.endLo,0},{v.endHi,0},{v.faultStatus,0},{v.faultAddr,0}};
        for(auto o:v.ident)mem[o]=0;
        bank[{0u,r.shMem}]=0x0000c000u;
    }
    bool banked(uint64_t off) const {
        if(off==r.shMem || off==r.shBases)return true;
        for(uint32_t dw=mq::kHqdFirst;dw<=mq::kHqdLast;++dw)if(off==mq::hqd(r.v.q,dw))return true;
        return false;
    }
    uint32_t sel() { return mem[r.v.q.grbm]; }
    uint32_t *phys(uint64_t off) {
        if(off>=l.windowVram && off<l.windowVram+vg::kWindowBytes)return &vram[(off-l.windowVram)/4u];
        if(off<4ull*screen.size())return &screen[off/4u];
        return nullptr;
    }
    void fault(uint32_t st,uint64_t va){if(!mem[r.v.faultStatus]){mem[r.v.faultStatus]=st;mem[r.v.faultAddr]=uint32_t(va>>12);}}
    uint32_t *translate(uint64_t va) {
        const v1::Regs &v=r.v;
        const bool hub=(mem[v.l1]&0x58u)==0x58u && (mem[v.l2]&1u) && mem[v.ident[0]]==0xffffffffu &&
            mem[v.ident[1]]==0xfu && (mem[v.ctx]&1u);
        if(!hub){fault(1u,va);return nullptr;}
        const uint32_t depth=(mem[v.ctx]>>1)&3u;
        const uint64_t page=va>>12, end=mem[v.endLo]|(uint64_t(mem[v.endHi])<<32);
        if(page>end || (mem[v.ctx]&0x78u)){fault(2u,va);return nullptr;}
        uint64_t table=(mem[v.baseLo]|(uint64_t(mem[v.baseHi])<<32));
        assert(table&1u);table&=0x0000fffffffff000ull;
        ++walks;
        for(uint32_t lv=depth;;--lv) {
            uint32_t *e=phys(table+8u*((va>>(12u+9u*lv))&511u));
            if(!e){fault(8u,va);return nullptr;}
            const uint64_t ent=e[0]|(uint64_t(e[1])<<32);
            // A directory entry is VRAM | VALID only (no system, no PTE/BFS/TF bits).
            if(!(ent&1u) || (lv && (ent&0xffff000000000ffeull)) || (lv && badPde && lv==2)){fault(4u,va);return nullptr;}
            table=ent&0x0000fffffffff000ull;
            if(!lv)return phys(table+(va&0xfffu));
        }
    }
    uint32_t rd(uint64_t va){uint32_t *p=translate(va);return p ? *p : 0xdeadu;}
    void cp() {
        const uint32_t qs=mq::grbmSelect(1,0,1);
        if(deaf || !(bank[{qs,mq::hqd(r.v.q,kiq::kCpHqdActive)}]&1u))return;
        if(!loaded) {
            assert(bank[{qs,mq::hqd(r.v.q,kiq::kCpHqdVmid)}]==1u);
            const uint64_t mqd=bank[{qs,mq::hqd(r.v.q,0x1fa9u)}]|(uint64_t(bank[{qs,mq::hqd(r.v.q,0x1faau)}])<<32);
            assert(mqd==l.windowMc+4096u*vg::kMqdPage);
            loaded=true;
        }
        const uint64_t ringVa=(uint64_t(bank[{qs,mq::hqd(r.v.q,kiq::kCpHqdPqBase)}])|
                               (uint64_t(bank[{qs,mq::hqd(r.v.q,kiq::kCpHqdPqBase+1u)}])<<32))<<8;
        const uint64_t pollVa=bank[{qs,mq::hqd(r.v.q,kiq::kCpHqdPqWptrPollAddr)}]|
            (uint64_t(bank[{qs,mq::hqd(r.v.q,kiq::kCpHqdPqWptrPollAddrHi)}])<<32);
        uint32_t *wp=translate(pollVa);
        if(!wp)return;
        run(ringVa,*wp);
    }
    void run(uint64_t ringVa,uint32_t wptr) {
        uint64_t pgm=0,kernarg=0;uint32_t masks=0,threads[3]{};
        for(uint32_t i=rptr;i<wptr;) {
            const uint32_t h=rd(ringVa+4u*i),op=(h>>8)&0xffu,n=((h>>16)&0x3fffu)+1u;
            if(h==mq::kNop){++i;continue;}
            std::vector<uint32_t> b(n);
            for(uint32_t k=0;k<n;++k)b[k]=rd(ringVa+4u*(i+1u+k));
            if(op==mq::kSetUconfig) {
                assert(b[0]==0x40u);mem[r.v.q.scratch]=b[1];
            } else if(op==gd::kSetShReg) {
                for(uint32_t k=0;k+1<n;++k) {
                    const uint32_t rg=gd::kShRegStart+b[0]+k-gd::kGcBase,v=b[1+k];
                    if(rg==gd::kPgmLo)pgm=(pgm&~0xffffffffull)|(uint64_t(v)<<8);
                    if(rg==gd::kPgmLo+1u)pgm|=uint64_t(v&0xffu)<<40;
                    if(rg==gd::kUserData0)kernarg=(kernarg&~0xffffffffull)|v;
                    if(rg==gd::kUserData0+1u)kernarg|=uint64_t(v)<<32;
                    if((rg==0x1bb6u||rg==0x1bb7u||rg==0x1bb9u||rg==0x1bbau) && v==~0u)++masks;
                    if(rg>=gd::kNumThreadX && rg<gd::kNumThreadX+3u)threads[rg-gd::kNumThreadX]=v;
                }
            } else if(op==gd::kDispatchDirect) {
                ++dispatches;
                // VMID 1 waves: the shader at its VA, SH_MEM_CONFIG and the apertures for VMID 1.
                uint32_t *code=translate(pgm);
                if(masks==4u && pgm==vg::kVaBase+gr::kShaderOff && code && *code && threads[0]==8u &&
                   bank[{vg::kGrbmVmid1,r.shMem}]==gr::kShMemDefault && bank[{vg::kGrbmVmid1,r.shBases}]==0x20001000u) {
                    const uint64_t px=rd(kernarg)|(uint64_t(rd(kernarg+4))<<32);
                    const uint32_t pitch=rd(kernarg+8),w=rd(kernarg+12),hh=rd(kernarg+16),fr=rd(kernarg+20);
                    for(uint32_t y=0;y<b[1]*8u;++y)for(uint32_t x=0;x<b[0]*8u;++x)
                        if(x<w && y<hh)if(uint32_t *p=translate(px+4ull*(y*pitch+x)))*p=gfx11_shader::gradientPixel(x,y,w,hh,fr);
                }
                masks=0;
            } else if(op==gd::kReleaseMem) {
                if(uint32_t *f=translate(b[2]|(uint64_t(b[3])<<32)))*f=b[4];
            } else assert(!"unexpected packet");
            i+=1u+n;
        }
        rptr=wptr;bank[{mq::grbmSelect(1,0,1),mq::hqd(r.v.q,kiq::kCpHqdPqRptr)}]=rptr;
    }
    static bool read(void *c,uint64_t off,uint32_t *v) {
        auto &f=*static_cast<Fake *>(c);
        if(f.banked(off)){*v=f.bank[{f.sel(),off}];return true;}
        assert(f.mem.count(off));*v=f.mem[off];return true;
    }
    static bool write(void *c,uint64_t off,uint32_t v) {
        auto &f=*static_cast<Fake *>(c);
        assert(vg::allowed(f.r,off));
        if(++f.attempts==f.failWrite)return false;
        f.writes.emplace_back(off,v);
        if(f.banked(off))f.bank[{f.sel(),off}]=v; else f.mem[off]=v;
        if(off==f.r.v.q.poll1 && (v&2u) && !f.ignorePoll)f.cp();
        if(off==mq::hqd(f.r.v.q,kiq::kCpHqdPqWptrLo) && f.sel()==mq::grbmSelect(1,0,1) && f.loaded && v) {
            const uint32_t qs=mq::grbmSelect(1,0,1);
            f.run(uint64_t(f.bank[{qs,mq::hqd(f.r.v.q,kiq::kCpHqdPqBase)}]|
                  (uint64_t(f.bank[{qs,mq::hqd(f.r.v.q,kiq::kCpHqdPqBase+1u)}])<<32))<<8,v);
        }
        return true;
    }
    static bool vw(void *c,uint32_t off,const uint32_t *src,uint32_t n) {
        auto &f=*static_cast<Fake *>(c);assert(off+4u*n<=vg::kWindowBytes);
        for(uint32_t i=0;i<n;++i)f.vram[off/4u+i]=src[i];
        return true;
    }
    static bool vr(void *c,uint32_t off,uint32_t *dst,uint32_t n) {
        auto &f=*static_cast<Fake *>(c);assert(off+4u*n<=vg::kWindowBytes);
        for(uint32_t i=0;i<n;++i)dst[i]=f.vram[off/4u+i];
        return true;
    }
    static bool sr(void *c,uint32_t off,uint32_t *dst) {
        auto &f=*static_cast<Fake *>(c);assert(off/4u<f.screen.size());*dst=f.screen[off/4u];return true;
    }
    static void sleep(void *c,uint32_t ms){static_cast<Fake *>(c)->ms+=ms;}
    vg::Hw hw(){return {this,read,write,vw,vr,sr,sleep};}
};
int main() {
    std::ifstream in("shaders/gradient.co",std::ios::binary);
    const std::vector<uint8_t> co((std::istreambuf_iterator<char>(in)),std::istreambuf_iterator<char>());
    assert(co.size()==gr::kCodeObjectBytes);
    auto b=rx7600_fixture::blob();IPDiscovery d;assert(d.parse(b.data(),b.size()));
    const auto r=vg::resolve(d,rx7600_fixture::kBar5);
    // SH_MEM_BASES/CONFIG: GC seg1 0xA000 + 0x9e3/0x9e4 on the RX 7600.
    assert(r.valid && r.shBases==(0xa000u+0x9e3u)*4u && r.shMem==(0xa000u+0x9e4u)*4u);
    assert(vg::allowed(r,r.shBases) && vg::allowed(r,r.v.ctx) && !vg::allowed(r,r.v.q.pc) &&
           !vg::allowed(r,r.v.q.cntl) && !vg::allowed(r,r.v.faultStatus));
    // Directory indices: every level non-zero; the work and screen VAs in one PDB1 entry.
    assert(vg::level(vg::kVaBase,3)==0xabu && vg::level(vg::kVaBase,2)==0x155u && vg::level(vg::kVaBase,1)==0xf0u &&
           vg::level(vg::kVaBase,0)==0u && vg::level(vg::kScreenVa,1)==0xf1u);
    {   static vg::Tables t;vg::tables(vg::Layout{0x800fc00000ull,0x0fc00000ull},vg::Screen{0u,3u<<20,1024,768,1024},t);
        assert(t.e[vg::kPdb2][0xab]==(0x0fc00000ull+4096u)+1u && t.e[vg::kPdb1][0x155]==0x0fc02001ull);
        assert(t.e[vg::kPtbWork][0]==0u && t.e[vg::kPtbWork][1]==((0x0fc00000ull+254u*4096u)|0x71u));
        assert(t.e[vg::kPdb0][0xf1]==0x0fc04001ull && t.e[vg::kPdb0][0xf2]==0x0fc05001ull && t.e[vg::kPdb0][0xf3]==0u);
        assert(t.e[vg::kPtbScreen+1][767u-512u]==((767ull*4096u)|0x71u) && t.e[vg::kPtbScreen+1][256]==0u);
    }
    bool attempted=false;
    {   Fake f(r);vg::Hw h=f.hw();h.write=nullptr;h.vramWrite=nullptr;
        auto o=vg::execute(r,f.l,f.s,co.data(),co.size(),h,false,true,attempted);
        assert(o.state==N33_PSPL_PLAN_OK && f.writes.empty() && !attempted && o.vm_marker==N33_VMG_MARKER);
        assert(o.grad_marker==N33_VMG_MARKER && o.vm_window==0x800fcu && o.grad_screen==(1024u<<16|768u));
        o=vg::execute(r,f.l,f.s,co.data(),co.size(),f.hw(),true,false,attempted);
        assert(o.state==N33_PSPL_WRITE_DISABLED && f.writes.empty() && f.vram[0]==0x5a5a5a5au);
    }
    {   Fake f(r);
        auto o=vg::execute(r,f.l,f.s,co.data(),co.size(),f.hw(),true,true,attempted);
        assert(o.state==N33_PSPL_VMG_ON_SCREEN && attempted && o.steps_done==5 && f.dispatches==2);
        assert(o.vm_scratch==0xdeadbeefu && o.vm_fence==vg::kFenceValue && (o.vm_active&1u) && !o.vm_fault_status);
        assert(o.grad_fence_a==vg::kFenceValueA && o.grad_fence_b==vg::kFenceValueB && !o.grad_bad_pixels &&
               !o.grad_screen_bad && !o.grad_fallbacks && o.grad_rptr_before==16u);
        assert(o.grad_first_pixel==gfx11_shader::gradientPixel(1,1,64,64,vg::kFrameA));
        for(uint32_t y=0;y<768;y+=37)for(uint32_t x=0;x<1024;x+=41)
            assert(f.screen[y*1024+x]==gfx11_shader::gradientPixel(x,y,1024,768,vg::kFrameB));
        // The ring-test fence sits on the page the table maps VA page 4 to (window page 251).
        assert(f.vram[(251u*4096u)/4u]==vg::kFenceValue && f.vram[(4u*4096u)/4u]!=vg::kFenceValue);
        assert(f.mem[r.v.ctx]==0x01fffe07u && f.mem[r.v.endLo]==0xffffffffu && f.mem[r.v.endHi]==0xfu &&
               f.mem[r.v.baseLo]==0x0fc00001u && f.mem[r.v.l1]==0x1859u);
        assert(f.bank[{0u,r.shMem}]==0x0000c000u);   // VMID 0 untouched
        assert(f.mem[r.v.q.grbm]==0u && o.register_writes==f.writes.size() && f.writes.size()<=vg::kMaxWrites);
        o=vg::execute(r,f.l,f.s,co.data(),co.size(),f.hw(),true,true,attempted);
        assert(o.state==N33_PSPL_VMG_BAD_PRECONDITION && o.register_writes==0u);
    }
    {   Fake f(r);vg::Screen none{};attempted=false;
        auto o=vg::execute(r,f.l,none,co.data(),co.size(),f.hw(),true,true,attempted);
        assert(o.state==N33_PSPL_VMG_OK && f.dispatches==1 && f.screen[0]==0x00203040u);
    }
    {   Fake f(r);f.ignorePoll=true;attempted=false;   // no poll at all: the ring test never runs
        auto o=vg::execute(r,f.l,f.s,co.data(),co.size(),f.hw(),true,true,attempted);
        assert(o.state==N33_PSPL_VMG_NO_FETCH && f.mem[r.v.q.grbm]==0u);
    }
    {   Fake f(r);f.badPde=true;attempted=false;      // a broken PDB0 shows as a hub fault
        auto o=vg::execute(r,f.l,f.s,co.data(),co.size(),f.hw(),true,true,attempted);
        assert(o.state==N33_PSPL_VMG_FAULT && o.vm_fault_status==4u && f.mem[r.v.q.grbm]==0u);
    }
    // Preconditions refuse with no write, as G36.
    const std::pair<uint64_t,uint32_t> bad[]={{r.v.l1,0x1859u},{r.v.l2,0x00080e01u},{r.v.ctx,0x01fffe01u},
        {r.v.faultStatus,1u},{r.v.q.cntl,0x400f0000u},{r.v.q.scratch,0xcafedeadu},{r.v.q.grbm,4u},{r.v.q.sos,0u}};
    for(auto &[off,v]:bad) {
        Fake f(r);f.mem[off]=v;attempted=false;
        auto o=vg::execute(r,f.l,f.s,co.data(),co.size(),f.hw(),true,true,attempted);
        assert(o.state!=N33_PSPL_VMG_ON_SCREEN && f.writes.empty() && !attempted && f.vram[0]==0x5a5a5a5au);
    }
    {   Fake f(r);f.bank[{mq::grbmSelect(1,0,1),mq::hqd(r.v.q,kiq::kCpHqdActive)}]=1u;attempted=false;
        auto o=vg::execute(r,f.l,f.s,co.data(),co.size(),f.hw(),true,true,attempted);
        assert(o.state==N33_PSPL_VMG_BAD_PRECONDITION && f.mem[r.v.q.grbm]==0u);
    }
    {   std::vector<uint8_t> other=co;other[200]^=1u;Fake f(r);attempted=false;
        auto o=vg::execute(r,f.l,f.s,other.data(),other.size(),f.hw(),true,true,attempted);
        assert(o.state==N33_PSPL_BAD_FIRMWARE && f.writes.empty() && !attempted);
    }
    for(uint64_t mc:{0ull,0x800fc01000ull}) {
        Fake f(r);vg::Layout l{mc,0x0fc00000ull};attempted=false;
        auto o=vg::execute(r,l,f.s,co.data(),co.size(),f.hw(),true,true,attempted);
        assert(o.state==N33_PSPL_NO_WINDOW && f.writes.empty());
    }
    {   Fake f(r);vg::Screen big{0u,vg::kMaxScreenBytes+4096u,1920,1080,1920};attempted=false;
        assert(!vg::screenOk(big));
        auto o=vg::execute(r,f.l,big,co.data(),co.size(),f.hw(),true,true,attempted);
        assert(o.state==N33_PSPL_VMG_OK && o.grad_screen==0u);   // too big to map: test image only
    }
    unsigned total=0;
    {   Fake f(r);attempted=false;vg::execute(r,f.l,f.s,co.data(),co.size(),f.hw(),true,true,attempted);total=f.writes.size();}
    for(unsigned at=1;at<=total;++at) {
        Fake f(r);f.failWrite=at;attempted=false;
        auto o=vg::execute(r,f.l,f.s,co.data(),co.size(),f.hw(),true,true,attempted);
        assert(o.state==N33_PSPL_VMG_WRITE_FAILED && f.mem[r.v.q.grbm]==0u && o.register_writes==f.writes.size());
    }
    printf("GFX11 VMID1 gradient: four-level table walked by a fake GFXHUB, ring test, test image and screen "
           "through VMID 1, %u register writes, gates: passed\n",total);
}
