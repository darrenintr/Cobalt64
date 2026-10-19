// SPDX-License-Identifier: MIT
#include "../src/Navi33Gfx11Vmid.h"
#include "rx7600_ipd_fixture.h"
#include <cassert>
#include <cstdio>
#include <map>
#include <utility>
#include <vector>
using namespace navi33;
namespace vm=vmid1;
namespace mq=mec_queue;
namespace kiq=gfx11_kiq;
// A fake GC after the autostart. Its GFXHUB translates VMID 1 only once L1 mode
// 3 + ADM, the L2 cache, a disabled identity aperture and an enabled flat
// context 1 are in place, by walking the PTEs the runner put in fake VRAM. The
// queue-1 "CP" fetches its ring and writes its fence through that translation.
struct Fake {
    vm::Regs r; vm::Layout l{0x800fb00000ull,0x0fb00000ull};
    std::map<uint64_t,uint32_t> mem; std::map<std::pair<uint32_t,uint64_t>,uint32_t> bank;
    std::vector<uint32_t> vram=std::vector<uint32_t>(vm::kWindowBytes/4u,0x5a5a5a5au);
    std::vector<std::pair<uint64_t,uint32_t>> writes;
    unsigned failWrite=0,attempts=0; uint32_t ms=0; bool ran=false,badPte=false,noL2=false;
    explicit Fake(vm::Regs regs):r(regs) {
        mem={{r.q.sos,42},{r.q.rlc,0xc000001fu},{r.q.grbm,0},{r.q.cntl,0x3c000000u},{r.q.scratch,0xdeadbeefu},
             {r.q.poll1,0},{r.l1,vm::kL1Default},{r.l2,vm::kL2Default},{r.l2c2,0},{r.l2c3,vm::kL2Cntl3Default},
             {r.l2c4,vm::kL2Cntl4Default},{r.l2c5,vm::kL2Cntl5Default},{r.ctx,vm::kCtxDefault},{r.baseLo,0},
             {r.baseHi,0},{r.startLo,0},{r.startHi,0},{r.endLo,0},{r.endHi,0},{r.faultStatus,0},{r.faultAddr,0}};
        for(auto o:r.ident)mem[o]=0;
    }
    bool banked(uint64_t off) const {
        for(uint32_t dw=mq::kHqdFirst;dw<=mq::kHqdLast;++dw)if(off==mq::hqd(r.q,dw))return true;
        return false;
    }
    uint32_t sel() { return mem[r.q.grbm]; }
    // Physical VRAM offset -> our window, or "elsewhere" (an untranslated access).
    uint32_t *phys(uint64_t vramOff) {
        if(vramOff<l.windowVram || vramOff>=l.windowVram+vm::kWindowBytes)return nullptr;
        return &vram[(vramOff-l.windowVram)/4u];
    }
    uint32_t *translate(uint64_t va) {
        const bool hub=(mem[r.l1]&0x58u)==0x58u && (mem[r.l2]&1u) && !noL2 &&
            mem[r.ident[0]]==0xffffffffu && mem[r.ident[1]]==0xfu && (mem[r.ctx]&7u)==1u;
        if(!hub){mem[r.faultStatus]=0x1u;return nullptr;}
        const uint64_t base=(mem[r.baseLo]|(uint64_t(mem[r.baseHi])<<32));
        assert(base&1u);
        const uint64_t page=va>>12;
        if(page>mem[r.endLo]){mem[r.faultStatus]=0x2u;return nullptr;}
        uint32_t *pt=phys((base&~0xfffull)+8u*page);
        const uint64_t pte=pt[0]|(uint64_t(pt[1])<<32);
        if(!(pte&1u) || badPte){mem[r.faultStatus]=0x4u;mem[r.faultAddr]=uint32_t(page);return nullptr;}
        return phys((pte&0x0000fffffffff000ull)+(va&0xfffu));
    }
    void cp() {
        const uint32_t s=mq::grbmSelect(1,0,1);
        if(ran || !(mem[r.q.poll1]&2u) || !(bank[{s,mq::hqd(r.q,kiq::kCpHqdActive)}]&1u))return;
        ran=true;
        assert(bank[{s,mq::hqd(r.q,kiq::kCpHqdVmid)}]==1u);
        // The MQD is fetched in VMID 0, physically.
        const uint64_t mqd=bank[{s,mq::hqd(r.q,0x1fa9u)}]|(uint64_t(bank[{s,mq::hqd(r.q,0x1faau)}])<<32);
        assert(mqd==l.windowMc+4096u*vm::kMqdPage);
        const uint64_t ringVa=uint64_t(bank[{s,mq::hqd(r.q,kiq::kCpHqdPqBase)}])<<8;
        uint32_t *wp=translate(bank[{s,mq::hqd(r.q,kiq::kCpHqdPqWptrPollAddr)}]);
        if(!wp)return;
        const uint32_t wptr=*wp;
        uint32_t ring[16];
        for(uint32_t i=0;i<wptr;++i){uint32_t *p=translate(ringVa+4u*i);if(!p)return;ring[i]=*p;}
        assert(ring[0]==gfx11_dispatch::packet3(0x79u,1u) && ring[1]==0x40u);
        mem[r.q.scratch]=ring[2];
        if(uint32_t *f=translate(ring[6]|(uint64_t(ring[7])<<32)))*f=ring[8];
        bank[{s,mq::hqd(r.q,kiq::kCpHqdPqRptr)}]=wptr;
    }
    static bool read(void *c,uint64_t off,uint32_t *v) {
        auto &f=*static_cast<Fake *>(c);
        if(f.banked(off)){*v=f.bank[{f.sel(),off}];return true;}
        assert(f.mem.count(off));*v=f.mem[off];return true;
    }
    static bool write(void *c,uint64_t off,uint32_t v) {
        auto &f=*static_cast<Fake *>(c);
        assert(vm::allowed(f.r,off));
        if(++f.attempts==f.failWrite)return false;
        f.writes.emplace_back(off,v);
        if(f.banked(off))f.bank[{f.sel(),off}]=v; else f.mem[off]=v;
        f.cp();
        return true;
    }
    static bool vw(void *c,uint32_t off,const uint32_t *src,uint32_t n) {
        auto &f=*static_cast<Fake *>(c);assert(off+4u*n<=vm::kWindowBytes);
        for(uint32_t i=0;i<n;++i)f.vram[off/4u+i]=src[i];
        return true;
    }
    static bool vr(void *c,uint32_t off,uint32_t *dst,uint32_t n) {
        auto &f=*static_cast<Fake *>(c);assert(off+4u*n<=vm::kWindowBytes);
        for(uint32_t i=0;i<n;++i)dst[i]=f.vram[off/4u+i];
        return true;
    }
    static void sleep(void *c,uint32_t ms){static_cast<Fake *>(c)->ms+=ms;}
    vm::Hw hw(){return {this,read,write,vw,vr,sleep};}
};
int main() {
    auto b=rx7600_fixture::blob();IPDiscovery d;assert(d.parse(b.data(),b.size()));
    const auto r=vm::resolve(d,rx7600_fixture::kBar5);
    // (GC seg0 0x1260 + dword) * 4 on the RX 7600.
    assert(r.valid && r.l1==(0x1260u+0x167fu)*4u && r.ctx==(0x1260u+0x1689u)*4u && r.l2==(0x1260u+0x15bcu)*4u);
    assert(r.baseLo==(0x1260u+0x16f5u)*4u && r.endLo==(0x1260u+0x1735u)*4u && r.ident[0]==(0x1260u+0x15ceu)*4u);
    // Never the MEC program counters/control, the fault registers or anything of MMHUB.
    assert(!vm::allowed(r,r.q.pc) && !vm::allowed(r,r.q.cntl) && !vm::allowed(r,r.faultStatus));
    assert(vm::allowed(r,r.l1) && vm::allowed(r,r.q.poll1) && vm::allowed(r,mq::hqd(r.q,kiq::kCpHqdVmid)));
    // The PTEs: page 0 invalid, page p -> window page 15-p, VRAM bits only.
    {   uint64_t pte[16];vm::pageTable(vm::Layout{0x800fb00000ull,0x0fb00000ull},pte);
        assert(pte[0]==0u && pte[1]==((0x0fb00000ull+14u*4096u)|0x71u) && pte[15]==(0x0fb00000ull|0x71u));
        for(int p=1;p<16;++p)assert(!(pte[p]&2u) && (pte[p]>>48)==0u);
    }
    bool attempted=false;
    {   Fake f(r);vm::Hw h=f.hw();h.write=nullptr;h.vramWrite=nullptr;
        auto o=vm::execute(r,f.l,h,false,true,attempted);
        assert(o.state==N33_PSPL_PLAN_OK && f.writes.empty() && !attempted && o.vm_marker==N33_VM_MARKER);
        assert(o.vm_window==0x800fbu && f.vram[0]==0x5a5a5a5au);
        o=vm::execute(r,f.l,f.hw(),true,false,attempted);
        assert(o.state==N33_PSPL_WRITE_DISABLED && f.writes.empty());
    }
    {   Fake f(r);
        auto o=vm::execute(r,f.l,f.hw(),true,true,attempted);
        assert(o.state==N33_PSPL_VM_QUEUE_OK && attempted && o.steps_done==5 && f.ran);
        assert(o.vm_scratch==0xdeadbeefu && o.vm_fence==vm::kFenceValue && o.vm_rptr==16u && (o.vm_active&1u));
        // The fence sits on the page the table maps VA 0x4000 to (window page 11).
        assert(f.vram[(11u*4096u)/4u]==vm::kFenceValue && f.vram[(4u*4096u)/4u]==0u);
        assert(f.mem[r.l1]==0x1859u && f.mem[r.l2]==0x00080e01u && f.mem[r.l2c3]==0x80130009u &&
               f.mem[r.ctx]==0x01fffe01u && f.mem[r.endLo]==15u);
        assert(f.mem[r.q.grbm]==0u && o.register_writes==f.writes.size() && f.writes.size()<=vm::kMaxWrites);
        // Upstream order: hub (12), context 1 (7, enable last), then the queue.
        assert(f.writes[0].first==r.l1 && f.writes[11].first==r.ident[5] && f.writes[18].first==r.ctx);
        assert(f.writes[19]==std::make_pair(r.q.scratch,0xcafedeadu));
        o=vm::execute(r,f.l,f.hw(),true,true,attempted);
        assert(o.state!=N33_PSPL_VM_QUEUE_OK && o.register_writes==0u);
    }
    {   Fake f(r);f.badPte=true;attempted=false;   // a bad table shows as a hub fault, not a pass
        auto o=vm::execute(r,f.l,f.hw(),true,true,attempted);
        assert(o.state==N33_PSPL_VM_FAULT && o.vm_fault_status==4u && f.mem[r.q.grbm]==0u);
    }
    {   Fake f(r);f.noL2=true;attempted=false;
        auto o=vm::execute(r,f.l,f.hw(),true,true,attempted);
        assert(o.state==N33_PSPL_VM_FAULT && o.vm_scratch==0xcafedeadu);
    }
    // Preconditions: the hub must be untouched and the autostart's queue up.
    const std::pair<uint64_t,uint32_t> bad[]={{r.l1,0x1859u},{r.l2,0x00080e01u},{r.ctx,0x01fffe01u},
        {r.faultStatus,1u},{r.q.cntl,0x400f0000u},{r.q.scratch,0xcafedeadu},{r.q.grbm,4u},{r.q.sos,0u}};
    for(auto &[off,v]:bad) {
        Fake f(r);f.mem[off]=v;attempted=false;
        auto o=vm::execute(r,f.l,f.hw(),true,true,attempted);
        assert(o.state!=N33_PSPL_VM_QUEUE_OK && f.writes.empty() && !attempted && f.vram[0]==0x5a5a5a5au);
    }
    {   Fake f(r);f.bank[{mq::grbmSelect(1,0,1),mq::hqd(r.q,kiq::kCpHqdActive)}]=1u;attempted=false;
        auto o=vm::execute(r,f.l,f.hw(),true,true,attempted);
        assert(o.state==N33_PSPL_VM_BAD_PRECONDITION && f.mem[r.q.grbm]==0u);
    }
    for(uint64_t mc:{0ull,0x800fb01000ull}) {
        Fake f(r);vm::Layout l{mc,0x0fb00000ull};attempted=false;
        auto o=vm::execute(r,l,f.hw(),true,true,attempted);
        assert(o.state==N33_PSPL_NO_WINDOW && f.writes.empty());
    }
    for(unsigned at=1;at<=85;++at) {
        Fake f(r);f.failWrite=at;attempted=false;
        auto o=vm::execute(r,f.l,f.hw(),true,true,attempted);
        assert(o.state==N33_PSPL_VM_WRITE_FAILED && f.mem[r.q.grbm]==0u && o.register_writes==f.writes.size());
    }
    puts("GFX11 VMID1: gfxhub_v3_0 words, flat reversed page table, VMID1 queue fetch and fence through translation, gates: passed");
}
