// SPDX-License-Identifier: MIT
#include "../src/Navi33Gfx11VmAlloc.h"
#include "rx7600_ipd_fixture.h"
#include <cassert>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <map>
#include <unordered_map>
#include <utility>
#include <vector>
using namespace navi33;
namespace va=vm_alloc;
namespace vg=vm_gradient;
namespace v1=vmid1;
namespace vh=vram_heap;
namespace gv=gmc11_vm;
namespace gr=gradient;
namespace mq=mec_queue;
namespace gd=gfx11_dispatch;
namespace kiq=gfx11_kiq;
// A fake GC after the autostart, with sparse VRAM. The CPU may write only inside
// live, unpinned heap blocks (as the IOKit side enforces). The GFXHUB walks the
// manager's four levels wherever the heap put them; the CP and the waves reach
// every buffer, and the console at VRAM 0, only through that walk.
struct Fake {
    vg::Regs r; va::Layout l{0x8000000000ull,256ull<<20};
    vg::Screen s{0u,1024u*768u*4u,1024,768,1024};
    vh::Heap heap; gv::Space space;
    std::map<uint64_t,uint32_t> mem; std::map<std::pair<uint32_t,uint64_t>,uint32_t> bank;
    std::unordered_map<uint64_t,uint32_t> vram;     // by VRAM offset, dword-aligned
    std::vector<std::pair<uint64_t,uint32_t>> writes;
    unsigned failWrite=0,attempts=0,cpuWrites=0; uint32_t ms=0,rptr=0,dispatches=0;
    bool loaded=false,deaf=false;
    explicit Fake(vg::Regs regs):r(regs) {
        const v1::Regs &v=r.v;
        mem={{v.q.sos,42},{v.q.rlc,0xc000001fu},{v.q.grbm,0},{v.q.cntl,0x3c000000u},{v.q.scratch,0xdeadbeefu},
             {v.q.poll1,0},{v.l1,v1::kL1Default},{v.l2,v1::kL2Default},{v.l2c2,0},{v.l2c3,v1::kL2Cntl3Default},
             {v.l2c4,v1::kL2Cntl4Default},{v.l2c5,v1::kL2Cntl5Default},{v.ctx,v1::kCtxDefault},{v.baseLo,0},
             {v.baseHi,0},{v.startLo,0},{v.startHi,0},{v.endLo,0},{v.endHi,0},{v.faultStatus,0},{v.faultAddr,0}};
        for(auto o:v.ident)mem[o]=0;
        bank[{0u,r.shMem}]=0x0000c000u;
        for(uint32_t i=0;i<1024u*768u;i+=97)vram[4u*i]=0x00203040u;   // the console before
    }
    uint32_t get(uint64_t off){auto i=vram.find(off);return i==vram.end() ? 0x5a5a5a5au : i->second;}
    bool banked(uint64_t off) const {
        if(off==r.shMem || off==r.shBases)return true;
        for(uint32_t dw=mq::kHqdFirst;dw<=mq::kHqdLast;++dw)if(off==mq::hqd(r.v.q,dw))return true;
        return false;
    }
    uint32_t sel(){return mem[r.v.q.grbm];}
    void fault(uint32_t st,uint64_t a){if(!mem[r.v.faultStatus]){mem[r.v.faultStatus]=st;mem[r.v.faultAddr]=uint32_t(a>>12);}}
    // VA -> VRAM offset, or ~0 after recording a fault.
    uint64_t translate(uint64_t addr) {
        const v1::Regs &v=r.v;
        const bool hub=(mem[v.l1]&0x58u)==0x58u && (mem[v.l2]&1u) && mem[v.ident[0]]==0xffffffffu && (mem[v.ctx]&1u);
        if(!hub || ((mem[v.ctx]>>1)&3u)!=3u){fault(1u,addr);return ~0ull;}
        uint64_t table=(mem[v.baseLo]|(uint64_t(mem[v.baseHi])<<32));
        assert(table&1u);table&=gv::kAddrMask;
        for(uint32_t lv=3;;--lv) {
            const uint64_t e=get(table+8u*gv::index(addr,lv))|(uint64_t(get(table+8u*gv::index(addr,lv)+4u))<<32);
            if(!(e&1u) || e==0x5a5a5a5a5a5a5a5aull){fault(4u,addr);return ~0ull;}
            table=e&gv::kAddrMask;
            if(!lv)return table+(addr&0xfffu);
        }
    }
    uint32_t rd(uint64_t a){const uint64_t p=translate(a);return p==~0ull ? 0xdeadu : get(p);}
    void wr(uint64_t a,uint32_t v){const uint64_t p=translate(a);if(p!=~0ull)vram[p]=v;}
    void cp() {
        const uint32_t qs=mq::grbmSelect(1,0,1);
        if(deaf || !(bank[{qs,mq::hqd(r.v.q,kiq::kCpHqdActive)}]&1u))return;
        if(!loaded) {
            assert((bank[{qs,mq::hqd(r.v.q,kiq::kCpHqdVmid)}]==1u));
            const uint64_t mqd=bank[{qs,mq::hqd(r.v.q,0x1fa9u)}]|(uint64_t(bank[{qs,mq::hqd(r.v.q,0x1faau)}])<<32);
            assert(mqd>=l.fbBaseMc && heap.covers(mqd-l.fbBaseMc,4096));   // a heap buffer, physical
            loaded=true;
        }
        const uint64_t ringVa=(uint64_t(bank[{qs,mq::hqd(r.v.q,kiq::kCpHqdPqBase)}])|
                               (uint64_t(bank[{qs,mq::hqd(r.v.q,kiq::kCpHqdPqBaseHi)}])<<32))<<8;
        const uint64_t pollVa=bank[{qs,mq::hqd(r.v.q,kiq::kCpHqdPqWptrPollAddr)}]|
            (uint64_t(bank[{qs,mq::hqd(r.v.q,kiq::kCpHqdPqWptrPollAddrHi)}])<<32);
        const uint32_t wptr=rd(pollVa);
        if(mem[r.v.faultStatus])return;
        run(ringVa,wptr);
    }
    void run(uint64_t ringVa,uint32_t wptr) {
        uint64_t pgm=0,kernarg=0;uint32_t masks=0,threads[3]{};
        for(uint32_t i=rptr;i<wptr;) {
            const uint32_t h=rd(ringVa+4u*i),op=(h>>8)&0xffu,n=((h>>16)&0x3fffu)+1u;
            if(h==mq::kNop){++i;continue;}
            std::vector<uint32_t> b(n);
            for(uint32_t k=0;k<n;++k)b[k]=rd(ringVa+4u*(i+1u+k));
            if(op==mq::kSetUconfig){assert(b[0]==0x40u);mem[r.v.q.scratch]=b[1];}
            else if(op==gd::kSetShReg) {
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
                if(masks==4u && pgm==va::kShaderVa && rd(pgm)!=0u && threads[0]==8u &&
                   bank[{vg::kGrbmVmid1,r.shMem}]==gr::kShMemDefault) {
                    const uint64_t px=rd(kernarg)|(uint64_t(rd(kernarg+4))<<32);
                    const uint32_t pitch=rd(kernarg+8),w=rd(kernarg+12),hh=rd(kernarg+16),fr=rd(kernarg+20);
                    for(uint32_t y=0;y<b[1]*8u;++y)for(uint32_t x=0;x<b[0]*8u;++x)
                        if(x<w && y<hh)wr(px+4ull*(y*pitch+x),gfx11_shader::gradientPixel(x,y,w,hh,fr));
                }
                masks=0;
            } else if(op==gd::kReleaseMem) wr(b[2]|(uint64_t(b[3])<<32),b[4]);
            else assert(!"unexpected packet");
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
        if(off==f.r.v.q.poll1 && (v&2u))f.cp();
        return true;
    }
    static bool vw(void *c,uint64_t off,const uint32_t *src,uint32_t n) {
        auto &f=*static_cast<Fake *>(c);
        assert(f.heap.covers(off,4ull*n));         // never pinned memory, never outside a block
        ++f.cpuWrites;
        for(uint32_t i=0;i<n;++i)f.vram[off+4u*i]=src[i];
        return true;
    }
    static bool vr(void *c,uint64_t off,uint32_t *dst,uint32_t n) {
        auto &f=*static_cast<Fake *>(c);assert(f.heap.covers(off,4ull*n));
        for(uint32_t i=0;i<n;++i)dst[i]=f.get(off+4u*i);
        return true;
    }
    static bool sr(void *c,uint32_t off,uint32_t *dst){auto &f=*static_cast<Fake *>(c);*dst=f.get(f.s.vram+off);return true;}
    static void sleep(void *c,uint32_t ms){static_cast<Fake *>(c)->ms+=ms;}
    va::Hw hw(){return {this,read,write,vw,vr,sr,sleep};}
    n33_psp_load_result go(const std::vector<uint8_t> &co,bool run,bool gate,bool &att,const va::Hw *h=nullptr) {
        const va::Hw x=h ? *h : hw();
        return va::execute(r,l,s,co.data(),co.size(),heap,space,x,run,gate,att);
    }
};
int main() {
    std::ifstream in("shaders/gradient.co",std::ios::binary);
    const std::vector<uint8_t> co((std::istreambuf_iterator<char>(in)),std::istreambuf_iterator<char>());
    assert(co.size()==gr::kCodeObjectBytes);
    auto b=rx7600_fixture::blob();IPDiscovery d;assert(d.parse(b.data(),b.size()));
    const auto r=vg::resolve(d,rx7600_fixture::kBar5);assert(r.valid);
    // The kext's reservations: console/VGA, the bring-up windows, the top.
    {   vh::Heap h;assert(va::reservations(h,256ull<<20,0,3u<<20) && h.liveBlocks()==3u);
        assert(h.usedBytes(va::kTagBoot)==16ull<<20 && h.usedBytes(va::kTagStages)==16ull<<20 &&
               h.usedBytes(va::kTagTop)==112ull<<20 && h.consistent());
        assert(va::reservations(h,256ull<<20,0,3u<<20) && h.liveBlocks()==3u);   // once per boot
        vh::Heap h2;assert(va::reservations(h2,256ull<<20,0,(8u<<20)+4096u) && h2.usedBytes(va::kTagBoot)==16ull<<20);
        vh::Heap h3;assert(va::reservations(h3,256ull<<20,0x0,20u<<20) && h3.usedBytes(va::kTagBoot)==20ull<<20);
        vh::Heap h4;assert(!va::reservations(h4,128ull<<20,0,3u<<20));            // BAR0 must reach past 256 MiB
        vh::Heap h5;assert(!va::reservations(h5,256ull<<20,236u<<20,8u<<20));     // console over the windows
    }
    bool att=false;
    {   Fake f(r);va::Hw h=f.hw();h.write=nullptr;h.vramWrite=nullptr;
        auto o=f.go(co,false,true,att,&h);
        assert(o.state==N33_PSPL_PLAN_OK && f.writes.empty() && !att && o.vm_marker==N33_VMA_MARKER && !f.heap.total());
        o=f.go(co,true,false,att);
        assert(o.state==N33_PSPL_WRITE_DISABLED && f.writes.empty() && !f.heap.total());
    }
    unsigned total=0;
    {   Fake f(r);
        auto o=f.go(co,true,true,att);
        assert(o.state==N33_PSPL_VMA_ON_SCREEN && att && o.steps_done==5 && f.dispatches==2);
        assert(o.vm_scratch==0xdeadbeefu && o.vm_fence==va::kFenceValue && !o.vm_fault_status && (o.vm_active&1u));
        assert(o.grad_fence_a==va::kFenceValueA && o.grad_fence_b==va::kFenceValueB && !o.grad_bad_pixels && !o.grad_screen_bad);
        for(uint32_t y=0;y<768;y+=37)for(uint32_t x=0;x<1024;x+=41)
            assert(f.get(4u*(y*1024u+x))==gfx11_shader::gradientPixel(x,y,1024,768,va::kFrameB));
        // Everything placed by the heap in BAR0 above the reservations; tables from the manager.
        assert(f.space.rootVram()==(16ull<<20)+0x13000u && o.vm_window==((16u<<20)+0x13000u)>>12 && (o.vm_reserved&0xffffu)==f.space.tables());
        assert(f.space.tables()==6u && (o.vm_reserved>>16)==5u && f.heap.usedBytes(gv::kTagTables)==6u*4096u);
        assert(f.heap.usedBytes(va::kTagBo)==va::kQueueBytes+4096u+gr::kShaderMax+4096u+gr::kImageBytes);
        assert(f.heap.consistent() && f.mem[r.v.q.grbm]==0u && f.mem[r.v.ctx]==0x01fffe07u);
        assert(f.mem[r.v.baseLo]==(((16u<<20)+0x13000u)|1u) && f.mem[r.v.baseHi]==0u);
        total=f.writes.size();assert(total==o.register_writes && total<=vg::kMaxWrites);
        // The latch: a second run in the same boot does nothing, the heap keeps its buffers.
        o=f.go(co,true,true,att);
        assert(o.register_writes==0u && o.state!=N33_PSPL_VMA_ON_SCREEN && f.heap.liveBlocks()==3u+5u+6u);
    }
    {   Fake f(r);f.s=vg::Screen{};att=false;
        auto o=f.go(co,true,true,att);
        assert(o.state==N33_PSPL_VMA_OK && f.dispatches==1 && f.space.tables()==4u);
    }
    {   Fake f(r);f.deaf=true;att=false;
        auto o=f.go(co,true,true,att);
        assert(o.state==N33_PSPL_VMG_NO_FETCH && f.mem[r.v.q.grbm]==0u && f.heap.liveBlocks()==3u+5u+6u);
    }
    {   Fake f(r);att=false;   // a heap with no room left in BAR0 stops before any register write
        assert(va::reservations(f.heap,256ull<<20,0,3u<<20));
        assert(f.heap.alloc(224ull<<20,4096,vh::Place::Visible,1).code==vh::Code::Ok);
        auto o=f.go(co,true,true,att);
        assert(o.state==N33_PSPL_VMA_NO_MEMORY && f.writes.empty() && att);
    }
    const std::pair<uint64_t,uint32_t> bad[]={{r.v.l1,0x1859u},{r.v.ctx,0x01fffe01u},{r.v.faultStatus,1u},
        {r.v.q.cntl,0x400f0000u},{r.v.q.scratch,0xcafedeadu},{r.v.q.sos,0u}};
    for(auto &[off,v]:bad) {
        Fake f(r);f.mem[off]=v;att=false;
        auto o=f.go(co,true,true,att);
        assert(o.state!=N33_PSPL_VMA_ON_SCREEN && f.writes.empty() && !att && !f.heap.total());
    }
    {   std::vector<uint8_t> other=co;other[200]^=1u;Fake f(r);att=false;
        auto o=f.go(other,true,true,att);
        assert(o.state==N33_PSPL_BAD_FIRMWARE && f.writes.empty() && !att);
    }
    for(unsigned at=1;at<=total;++at) {
        Fake f(r);f.failWrite=at;att=false;
        auto o=f.go(co,true,true,att);
        assert(o.state==N33_PSPL_VMG_WRITE_FAILED && f.mem[r.v.q.grbm]==0u && o.register_writes==f.writes.size());
    }
    printf("GFX11 VMID1 with the VRAM heap: reservations, heap buffers and manager tables, ring test, test image "
           "and screen through VMID 1, %u register writes, CPU writes only inside heap blocks: passed\n",total);
}
