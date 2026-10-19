// SPDX-License-Identifier: MIT
#include "../src/Navi33Gfx11MesStart.h"
#include "rx7600_ipd_fixture.h"
#include <cassert>
#include <cstdio>
#include <map>
#include <utility>
#include <vector>
using namespace navi33;
namespace ms=mes_start;
// GC registers on the RX 7600: CP_MES_* are banked by GRBM_GFX_CNTL (me 3,
// pipe 0/1); the firmware publishes its version in GP3_LO once a pipe runs.
struct Fake {
    ms::Regs r; std::map<uint64_t,uint32_t> mem;
    std::vector<std::pair<uint64_t,uint32_t>> writes;
    uint32_t version[2]={0x92u,0x113u}, ms=0, failRead=0, reads=0;
    unsigned failWrite=0, slow=0, attempts=0; bool started=false, dead=false;
    explicit Fake(ms::Regs regs):r(regs) {
        mem={{r.sos,42},{r.rlc,0xc000001fu},{r.cpStat,0},{r.grbm,0},{r.cntl,0x40000000u},
             {r.start,0},{r.startHi,0}};
    }
    static bool read(void *c,uint64_t off,uint32_t *v) {
        auto &f=*static_cast<Fake *>(c);
        if(++f.reads==f.failRead)return false;
        if(off==f.r.gp3 || off==f.r.pc) {
            const uint32_t sel=f.mem[f.r.grbm];
            assert(sel==0xcu || sel==0xdu);   // never read a MES bank unselected
            const bool up=f.started && !f.dead && !f.slow;
            *v=!up ? 0u : off==f.r.gp3 ? f.version[sel&1u] : 0x1400u+(sel&1u);
            return true;
        }
        assert(f.mem.count(off));
        *v=f.mem[off];return true;
    }
    static bool write(void *c,uint64_t off,uint32_t v) {
        auto &f=*static_cast<Fake *>(c);
        assert(ms::allowed(f.r,off));
        if(++f.attempts==f.failWrite)return false;   // one failed write, then the bus works
        f.writes.emplace_back(off,v);f.mem[off]=v;
        if(off==f.r.cntl && (v&ms::kActiveMask))f.started=true;
        return true;
    }
    static void sleep(void *c,uint32_t ms) {
        auto &f=*static_cast<Fake *>(c);f.ms+=ms;
        if(f.slow)--f.slow;
    }
    ms::Hw hw(){return {this,read,write,sleep};}
};
int main() {
    auto b=rx7600_fixture::blob();IPDiscovery d;assert(d.parse(b.data(),b.size()));
    const auto r=ms::resolve(d,rx7600_fixture::kBar5);
    // (GC seg1 0xA000 / seg0 0x1260 + dword) * 4, from the RX 7600 IP Discovery.
    assert(r.valid && r.grbm==0x2a400 && r.cntl==0x3201c && r.start==0x32000);
    assert(r.startHi==0x32274 && r.gp3==0x32124 && r.pc==0x3204c);
    assert(r.rlc==0x3ba08 && r.cpStat==0x8680 && r.sos==0x58244);
    assert(!ms::resolve(d,0x30000).valid);
    assert(!ms::allowed(r,r.gp3) && !ms::allowed(r,r.rlc) && !ms::allowed(r,r.sos));
    bool attempted=false;
    {   // Plan: reads only, no write callback needed, latch untouched.
        Fake f(r);ms::Hw h=f.hw();h.write=nullptr;
        auto o=ms::execute(r,h,false,true,attempted);
        assert(o.state==N33_PSPL_PLAN_OK && f.writes.empty() && !attempted);
        assert(o.mes_marker==N33_MES_MARKER && o.mes_cntl_before==0x40000000u && o.mes_rlc==0xc000001fu);
        o=ms::execute(r,f.hw(),true,false,attempted);
        assert(o.state==N33_PSPL_WRITE_DISABLED && f.writes.empty() && !attempted);
    }
    {   // The run: upstream's exact write order, then one readback round.
        Fake f(r);
        auto o=ms::execute(r,f.hw(),true,true,attempted);
        assert(o.state==N33_PSPL_MES_RUNNING && attempted && o.steps_done==6);
        const std::vector<std::pair<uint64_t,uint32_t>> expected={
            {r.cntl,0x40030000u},                              // reset pipes 0 and 1
            {r.grbm,0xcu},{r.start,0xfc001400u},{r.startHi,0x3fffffffu},
            {r.grbm,0xdu},{r.start,0xfc001400u},{r.startHi,0x3fffffffu},
            {r.grbm,0u},{r.cntl,0x0c000000u},                  // unhalt, pipes 0+1 active
            {r.grbm,0xcu},{r.grbm,0xdu},{r.grbm,0u}};          // version readback
        assert(f.writes==expected && o.register_writes==12);
        assert(o.mes_version[0]==0x92u && o.mes_version[1]==0x113u);
        assert(o.mes_pc[0]==0x1400u && o.mes_pc[1]==0x1401u);
        assert(o.mes_samples==1 && o.elapsed_ms==1 && f.ms==1 && o.mes_cntl_after==0x0c000000u);
        assert(f.mem[r.grbm]==0u);
        o=ms::execute(r,f.hw(),true,true,attempted);
        assert(o.state!=N33_PSPL_MES_RUNNING && f.writes.size()==12);
    }
    {   // Already attempted: nothing at all.
        Fake f(r);
        auto o=ms::execute(r,f.hw(),true,true,attempted);
        assert(o.state==N33_PSPL_ALREADY_ATTEMPTED && f.writes.empty());
    }
    {   // Slow firmware: later rounds, at most three.
        Fake f(r);f.slow=3;attempted=false;
        auto o=ms::execute(r,f.hw(),true,true,attempted);
        assert(o.state==N33_PSPL_MES_RUNNING && o.mes_samples==3 && o.elapsed_ms==1000);
        assert(o.register_writes==9+9 && f.mem[r.grbm]==0u);
    }
    {   // No version ever: report, do not halt or retry.
        Fake f(r);f.dead=true;attempted=false;
        auto o=ms::execute(r,f.hw(),true,true,attempted);
        assert(o.state==N33_PSPL_MES_NO_VERSION && o.mes_samples==3 && o.register_writes==18);
        assert(f.writes.back()==std::make_pair(r.grbm,0u));
        // The reset write keeps HALT (upstream's read-modify-write); nothing re-halts after activation.
        for(size_t i=9;i<f.writes.size();++i)assert(f.writes[i].first==r.grbm);
    }
    // Preconditions refuse before any write and leave the latch.
    const std::pair<uint64_t,uint32_t> bad[]={
        {r.rlc,0x4000001fu},{r.grbm,0xcu},{r.cntl,0x04000000u},{r.cntl,0x08000000u}};
    for(auto &[off,v]:bad) {
        Fake f(r);f.mem[off]=v;attempted=false;
        auto o=ms::execute(r,f.hw(),true,true,attempted);
        assert(o.state==N33_PSPL_MES_BAD_PRECONDITION && f.writes.empty() && !attempted);
    }
    {Fake f(r);f.mem[r.sos]=0;attempted=false;
     auto o=ms::execute(r,f.hw(),true,true,attempted);
     assert(o.state==N33_PSPL_SOS_NOT_ALIVE && f.writes.empty() && !attempted);}
    for(uint64_t off:{r.sos,r.rlc,r.cpStat,r.grbm,r.cntl}) {
        Fake f(r);f.mem[off]=0xffffffffu;attempted=false;
        auto o=ms::execute(r,f.hw(),true,true,attempted);
        assert(o.state==N33_PSPL_READ_FAILED && f.writes.empty() && !attempted);
    }
    // A failed write stops at once and drops any GRBM select it left behind.
    for(unsigned at=1;at<=12;++at) {
        Fake f(r);f.failWrite=at;attempted=false;
        auto o=ms::execute(r,f.hw(),true,true,attempted);
        assert(o.state==N33_PSPL_MES_WRITE_FAILED && attempted);
        assert(f.mem[r.grbm]==0u && o.register_writes==f.writes.size());
    }
    // A failed read during the readback also deselects.
    {Fake f(r);f.failRead=7;attempted=false;
     auto o=ms::execute(r,f.hw(),true,true,attempted);
     assert(o.state==N33_PSPL_READ_FAILED && attempted && f.mem[r.grbm]==0u);}
    puts("GFX11 MES start: upstream write order, GRBM select discipline, allowlist, gates and readback: passed");
}
