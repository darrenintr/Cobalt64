// SPDX-License-Identifier: MIT
#include "../src/Navi33Gfx11MecQueue.h"
#include "rx7600_ipd_fixture.h"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <map>
#include <utility>
#include <vector>
using namespace navi33;
namespace mq=mec_queue;
namespace kiq=gfx11_kiq;
// A fake GC: HQD registers banked by GRBM_GFX_CNTL, a 1 MiB VRAM window, and a
// "CP" that runs the ring only when the MEC was given its program counters,
// unhalted, and the selected queue was activated after a WPTR poll trigger.
struct Fake {
    mq::Regs r; mq::Layout l{0x800fa00000ull};
    std::map<uint64_t,uint32_t> mem;                         // unbanked registers
    std::map<std::pair<uint32_t,uint64_t>,uint32_t> bank;    // (select, offset)
    std::vector<uint32_t> vram=std::vector<uint32_t>(mq::kWindowBytes/4u,0x5a5a5a5au);
    std::vector<std::pair<uint64_t,uint32_t>> writes;
    unsigned failWrite=0,attempts=0,failRead=0,reads=0; uint32_t ms=0; bool allowMap=false;
    bool pcs[4]{}, polled=false, ran=false, noFence=false, deaf=false;
    explicit Fake(mq::Regs regs):r(regs) {
        mem={{r.sos,42},{r.rlc,0xc000001fu},{r.grbm,0},{r.cntl,0x400f0000u},{r.scratch,0},{r.poll1,0},{r.scheduler,0x3038u}};
    }
    bool banked(uint64_t off) const {
        if(off==r.pc || off==r.pcHi)return true;
        for(uint32_t dw=mq::kHqdFirst;dw<=mq::kHqdLast;++dw)if(off==mq::hqd(r,dw))return true;
        return false;
    }
    uint32_t sel() { return mem[r.grbm]; }
    void cp() {
        const uint32_t c=mem[r.cntl];
        const bool mecOn=!(c&gfx11_mec::kMecHalt) && (c&0x04000000u) && !(c&gfx11_mec::kMecPipeResetAll);
        const uint32_t s=mq::grbmSelect(1,0,0);
        if(ran || deaf || !mecOn || !pcs[0] || !polled || !(bank[{s,mq::hqd(r,kiq::kCpHqdActive)}]&1u))return;
        // The CP reads the MQD-programmed ring base and the polled WPTR from VRAM.
        const uint64_t base=uint64_t(bank[{s,mq::hqd(r,kiq::kCpHqdPqBase)}])<<8;
        const uint64_t poll=bank[{s,mq::hqd(r,kiq::kCpHqdPqWptrPollAddr)}]|
            (uint64_t(bank[{s,mq::hqd(r,kiq::kCpHqdPqWptrPollAddrHi)}])<<32);
        assert(base==l.windowMc+mq::kRingOff && poll==l.windowMc+mq::kWptrOff);
        const uint32_t wptr=vram[mq::kWptrOff/4u];
        const uint32_t *ring=&vram[mq::kRingOff/4u];
        assert(wptr==mq::kRingDwords);
        assert(ring[0]==gfx11_dispatch::packet3(0x79u,1u) && ring[1]==0x40u);
        mem[r.scratch]=ring[2];
        assert(ring[3]==gfx11_dispatch::packet3(0x49u,6u));
        const uint64_t fence=ring[6]|(uint64_t(ring[7])<<32);
        if(!noFence)vram[(fence-l.windowMc)/4u]=ring[8];
        bank[{s,mq::hqd(r,kiq::kCpHqdPqRptr)}]=wptr;
        ran=true;
    }
    static bool read(void *c,uint64_t off,uint32_t *v) {
        auto &f=*static_cast<Fake *>(c);
        if(++f.reads==f.failRead)return false;
        *v=f.banked(off) ? f.bank[{f.sel(),off}] : (assert(f.mem.count(off)),f.mem[off]);
        return true;
    }
    static bool write(void *c,uint64_t off,uint32_t v) {
        auto &f=*static_cast<Fake *>(c);
        assert(mq::allowed(f.r,off,f.allowMap));
        if(++f.attempts==f.failWrite)return false;
        f.writes.emplace_back(off,v);
        if(f.banked(off)) {
            f.bank[{f.sel(),off}]=v;
            if(off==f.r.pc && v==mq::kPcStart && (f.sel()&0xcu)==4u)f.pcs[f.sel()&3u]=true;
        } else f.mem[off]=v;
        if(off==f.r.poll1 && v==1u)f.polled=true;
        f.cp();
        return true;
    }
    static bool vw(void *c,uint32_t off,const uint32_t *src,uint32_t n) {
        auto &f=*static_cast<Fake *>(c);
        assert(!(off&3u) && off+4u*n<=mq::kWindowBytes);
        for(uint32_t i=0;i<n;++i)f.vram[off/4u+i]=src[i];
        return true;
    }
    static bool vr(void *c,uint32_t off,uint32_t *dst,uint32_t n) {
        auto &f=*static_cast<Fake *>(c);
        assert(!(off&3u) && off+4u*n<=mq::kWindowBytes);
        for(uint32_t i=0;i<n;++i)dst[i]=f.vram[off/4u+i];
        return true;
    }
    static void sleep(void *c,uint32_t ms){static_cast<Fake *>(c)->ms+=ms;}
    mq::Hw hw(){return {this,read,write,vw,vr,sleep};}
};
int main() {
    auto b=rx7600_fixture::blob();IPDiscovery d;assert(d.parse(b.data(),b.size()));
    const auto r=mq::resolve(d,rx7600_fixture::kBar5);
    // (GC seg0 0x1260 / seg1 0xA000 + dword) * 4 on the RX 7600.
    assert(r.valid && r.grbm==0x2a400 && r.pc==0x32400 && r.pcHi==0x324e0 && r.cntl==0x32410);
    assert(r.scratch==0x30100 && r.poll1==0xc210 && mq::hqd(r,0x1fa9)==0xc824 && mq::hqd(r,0x1fe0)==0xc900);
    assert(!mq::allowed(r,mq::hqd(r,mq::kHqdHole)) && !mq::allowed(r,r.rlc) && !mq::allowed(r,r.sos));
    assert(r.scheduler && !mq::allowed(r,r.scheduler) && mq::allowed(r,r.scheduler,true));
    bool attempted=false;
    {   Fake f(r);mq::Hw h=f.hw();h.write=nullptr;h.vramWrite=nullptr;
        auto o=mq::execute(r,f.l,h,false,true,attempted);
        assert(o.state==N33_PSPL_PLAN_OK && f.writes.empty() && !attempted && o.mec_marker==N33_MEC_MARKER);
        assert(o.mec_cntl_before==0x400f0000u && o.mec_window_mc==0x800fa00000ull);
        o=mq::execute(r,f.l,f.hw(),true,false,attempted);
        assert(o.state==N33_PSPL_WRITE_DISABLED && f.writes.empty() && !attempted);
        assert(f.vram[0]==0x5a5a5a5au);   // the plan never touches VRAM either
    }
    {   Fake f(r);
        auto o=mq::execute(r,f.l,f.hw(),true,true,attempted);
        assert(o.state==N33_PSPL_MEC_QUEUE_OK && attempted && o.steps_done==5);
        assert(o.mec_scratch==0xdeadbeefu && o.mec_fence==mq::kFenceValue && o.mec_rptr==16u);
        assert(o.mec_cntl_after==0x3c000000u && o.mec_hqd_active_before==0u && (o.mec_hqd_active_after&1u));
        assert(f.mem[r.grbm]==0u && (o.flags&N33_PSPL_FLAG_WROTE_VRAM));
        // Upstream order: PCs per pipe, reset/clear, enable, preset, HQD load.
        const auto &w=f.writes;
        for(uint32_t p=0;p<4u;++p) {
            assert(w[3*p]==std::make_pair(r.grbm,mq::grbmSelect(1,p,0)));
            assert(w[3*p+1]==std::make_pair(r.pc,0xc00u) && w[3*p+2]==std::make_pair(r.pcHi,0x1c000u));
        }
        assert(w[12]==std::make_pair(r.grbm,0u) && w[13]==std::make_pair(r.cntl,0x400f0000u));
        assert(w[14]==std::make_pair(r.cntl,0x40000000u) && w[15]==std::make_pair(r.cntl,0x3c000000u));
        assert(w[16]==std::make_pair(r.scratch,0xcafedeadu) && w[17]==std::make_pair(r.grbm,0x4u));
        size_t i=18;
        for(uint32_t dw=mq::kHqdFirst;dw<=mq::kHqdLast;++dw)if(dw!=mq::kHqdHole)assert(w[i++].first==mq::hqd(r,dw));
        assert(w[i].first==mq::hqd(r,kiq::kCpHqdPqDoorbellControl) && (w[i].second&mq::kDoorbellEn));
        assert(w[i+5]==std::make_pair(r.poll1,1u) && (w[i+6].second&mq::kEopInitFetcher));
        assert(w[i+7].first==mq::hqd(r,kiq::kCpHqdActive) && w[i+8]==std::make_pair(r.grbm,0u));
        assert(o.register_writes==w.size() && w.size()==i+11);   // + RPTR readback select/deselect
        // The MQD in VRAM is the G19 image for this window.
        const auto img=gfx11_mqd::buildKiqMqd(mq::queueFor(f.l));
        for(uint32_t k=0;k<gfx11_mqd::kDwords;++k)assert(f.vram[k]==img.image.dw[k]);
        // A second call writes nothing: the MEC is no longer halted (and the latch is spent).
        const size_t n=f.writes.size();
        o=mq::execute(r,f.l,f.hw(),true,true,attempted);
        assert(o.state==N33_PSPL_MEC_BAD_PRECONDITION && f.writes.size()==n);
        f.mem[r.cntl]=0x400f0000u;
        o=mq::execute(r,f.l,f.hw(),true,true,attempted);
        assert(o.state==N33_PSPL_ALREADY_ATTEMPTED && f.writes.size()==n);
    }
    {   Fake f(r); f.allowMap=true; uint32_t before=0,after=0;attempted=false;
        auto o=mq::execute(r,f.l,f.hw(),true,true,attempted,true,&before,&after);
        assert(o.state==N33_PSPL_MEC_QUEUE_OK && before==0x3038u && after==0x30a0u);
        const auto &w=f.writes;auto it=std::find(w.begin(),w.end(),std::make_pair(r.scheduler,0x30a0u));
        assert(it!=w.end());const size_t i=static_cast<size_t>(it-w.begin());
        assert(w[i-1u]==std::make_pair(r.grbm,mq::grbmSelect(1,0,0)));
        assert(w[i+1u].first==mq::hqd(r,kiq::kCpMqdBaseAddr));
        assert(w.size()==84u+1u); // one gated scheduler write, before HQD load
    }
    {   Fake f(r); f.allowMap=true;f.mem[r.scheduler]=0x30a0u;attempted=false;uint32_t before=0,after=0;
        auto o=mq::execute(r,f.l,f.hw(),true,true,attempted,true,&before,&after);
        assert(o.state==N33_PSPL_MEC_BAD_PRECONDITION && before==0x30a0u && after==0u);
        assert(std::none_of(f.writes.begin(),f.writes.end(),[&](const auto &w){return w.first==r.scheduler;}));
        assert(std::none_of(f.writes.begin(),f.writes.end(),[&](const auto &w){return w.first==mq::hqd(r,kiq::kCpHqdActive);}));
    }
    {   Fake f(r);f.noFence=true;attempted=false;
        auto o=mq::execute(r,f.l,f.hw(),true,true,attempted);
        assert(o.state==N33_PSPL_MEC_NO_FENCE && o.mec_scratch==0xdeadbeefu && o.elapsed_ms==1001 && f.mem[r.grbm]==0u);
    }
    {   Fake f(r);f.deaf=true;attempted=false;
        auto o=mq::execute(r,f.l,f.hw(),true,true,attempted);
        assert(o.state==N33_PSPL_MEC_NO_FETCH && o.mec_scratch==0xcafedeadu && f.mem[r.grbm]==0u);
        for(auto &w:f.writes)assert(!(w.first==r.cntl && (w.second&gfx11_mec::kMecHalt) && &w>&f.writes[14]));
    }
    // Preconditions refuse before any write or VRAM access.
    const std::pair<uint64_t,uint32_t> bad[]={{r.rlc,0x4000001fu},{r.grbm,4u},{r.cntl,0x3c000000u},{r.sos,0u}};
    for(auto &[off,v]:bad) {
        Fake f(r);f.mem[off]=v;attempted=false;
        auto o=mq::execute(r,f.l,f.hw(),true,true,attempted);
        assert(o.state!=N33_PSPL_MEC_QUEUE_OK && f.writes.empty() && !attempted && f.vram[0]==0x5a5a5a5au);
    }
    {   // An already active HQD refuses after selecting it, and deselects.
        Fake f(r);f.bank[{mq::grbmSelect(1,0,0),mq::hqd(r,kiq::kCpHqdActive)}]=1u;attempted=false;
        auto o=mq::execute(r,f.l,f.hw(),true,true,attempted);
        assert(o.state==N33_PSPL_MEC_BAD_PRECONDITION && f.mem[r.grbm]==0u && o.mec_hqd_active_before==1u);
    }
    for(uint64_t mc:{0ull,0x800fa01000ull}) {
        Fake f(r);mq::Layout l{mc};attempted=false;
        auto o=mq::execute(r,l,f.hw(),true,true,attempted);
        assert(o.state==N33_PSPL_NO_WINDOW && f.writes.empty() && !attempted);
    }
    // Any failed write stops at once and leaves GRBM deselected.
    for(unsigned at=1;at<=80;++at) {
        Fake f(r);f.failWrite=at;attempted=false;
        auto o=mq::execute(r,f.l,f.hw(),true,true,attempted);
        assert(o.state==N33_PSPL_MEC_WRITE_FAILED && attempted && f.mem[r.grbm]==0u);
        assert(o.register_writes==f.writes.size());
    }
    puts("GFX11 MEC queue: upstream order, banked HQD load, allowlist, VRAM layout, ring test and fence: passed");
}
