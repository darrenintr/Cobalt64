// SPDX-License-Identifier: MIT
#include "../src/Navi33Smu13Enable.h"
#include "rx7600_ipd_fixture.h"
#include <cassert>
#include <map>
#include <vector>
#include <utility>
#include <cstdio>
using namespace navi33;
namespace sm=smu13;
struct Fake {
    sm::Regs r; std::map<uint64_t,uint32_t> mem;
    std::vector<std::pair<uint64_t,uint32_t>> writes;
    unsigned messages=0,failMessage=0,failWrite=0,failRead=0,reads=0,delayed=0;
    uint32_t reject=0xfd,ms=0; bool timeout=false,allOnes=false,gc=false;
    explicit Fake(sm::Regs regs):r(regs) {
        mem={{r.sos,42},{r.resp,1},{r.msg,0},{r.arg,0},{r.scratch0,0x0b214700},
             {r.scratch1,0xbabeface},{r.core,2},{r.reset,0x10},{r.rlc,0}};
    }
    static bool read(void *c,uint64_t off,uint32_t *v) {
        auto &f=*static_cast<Fake *>(c);
        if(++f.reads==f.failRead)return false;
        assert(f.mem.count(off));
        if(off==f.r.resp && f.messages && f.delayed) {
            --f.delayed;*v=0;return true;
        }
        *v=f.mem[off];return true;
    }
    static bool write(void *c,uint64_t off,uint32_t v) {
        auto &f=*static_cast<Fake *>(c);
        assert(off==f.r.msg || off==f.r.arg || off==f.r.resp);
        if(f.writes.size()+1==f.failWrite)return false;
        f.writes.emplace_back(off,v);f.mem[off]=v;
        if(off==f.r.msg) {
            ++f.messages;
            f.mem[f.r.resp]=f.allOnes ? 0xffffffffu : f.timeout ? 0u :
                f.messages==f.failMessage ? f.reject : 1u;
            if(f.messages==3 && f.gc) {
                f.mem[f.r.core]=3;f.mem[f.r.reset]=0x1f;f.mem[f.r.rlc]=0xc000001f;
            }
        }
        return true;
    }
    static void sleep(void *c,uint32_t ms){static_cast<Fake *>(c)->ms+=ms;}
    sm::Hw hw(){return {this,read,write,sleep};}
};
int main() {
    auto b=rx7600_fixture::blob();IPDiscovery d;assert(d.parse(b.data(),b.size()));
    auto r=sm::resolve(d,rx7600_fixture::kBar5);
    assert(r.valid && r.msg==0x58a08 && r.arg==0x58a48 && r.resp==0x58a68);
    assert(r.sos==0x58244 && r.core==0x382dc && r.reset==0x382f0 && r.rlc==0x3ba08);
    assert(!sm::resolve(d,0x58000).valid);
    auto bad=rx7600_fixture::blob();bad[270]=8; // MP1 13.0.8 is not Navi33
    rx7600_fixture::put16(bad,14,rx7600_fixture::sum(bad,128,bad.size()));
    rx7600_fixture::put16(bad,8,rx7600_fixture::sum(bad,10,bad.size()));
    IPDiscovery wrong;assert(wrong.parse(bad.data(),bad.size()));
    assert(!sm::resolve(wrong,rx7600_fixture::kBar5).valid);
    // Independent enumeration from init_allowed_features + Navi33 PG defaults.
    const unsigned bits[]={0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,17,18,19,20,21,22,23,24,
                           28,29,30,34,37,39,40,41,45,47,48};
    uint64_t mask=0;for(auto bit:bits)mask|=uint64_t(1)<<bit;
    assert(sm::kAllowed==mask && sm::kAllowed==0x0001a3a471fe7fffull);
    bool attempted=false;
    Fake f(r);
    auto o=sm::execute(r,f.hw(),false,true,attempted);
    assert(o.state==N33_PSPL_PLAN_OK && f.writes.empty() && !attempted);
    o=sm::execute(r,f.hw(),true,false,attempted);
    assert(o.state==N33_PSPL_WRITE_DISABLED && f.writes.empty() && !attempted);
    o=sm::execute(r,f.hw(),true,true,attempted);
    assert(o.state==N33_PSPL_SMU_ACKED && o.steps_done==3 && attempted);
    assert(o.register_writes==9 && o.elapsed_ms==1000 && f.ms==1000);
    const std::vector<std::pair<uint64_t,uint32_t>> expected={
        {r.resp,0},{r.arg,0x1a3a4},{r.msg,5},
        {r.resp,0},{r.arg,0x71fe7fff},{r.msg,4},
        {r.resp,0},{r.arg,0},{r.msg,6}};
    assert(f.writes==expected);
    o=sm::execute(r,f.hw(),true,true,attempted);
    assert(o.state==N33_PSPL_ALREADY_ATTEMPTED && f.writes.size()==9);
    // Every error response stops immediately, without retries or later commands.
    for(unsigned step=1;step<=3;++step)for(uint32_t reply:{0xfcu,0xfdu,0xfeu,0xffu,2u}) {
        Fake x(r);x.failMessage=step;x.reject=reply;attempted=false;
        o=sm::execute(r,x.hw(),true,true,attempted);
        assert(o.state==N33_PSPL_SMU_REJECTED && attempted);
        assert(o.steps_done==step-1 && o.register_writes==step*3 && x.messages==step);
        assert(o.smu_responses[step-1]==reply);
    }
    for(uint64_t off:{r.sos,r.msg,r.arg,r.resp,r.scratch0,r.scratch1,r.core,r.reset,r.rlc}) {
        Fake x(r);x.mem[off]=0xffffffff;attempted=false;
        o=sm::execute(r,x.hw(),true,true,attempted);
        assert(o.state==N33_PSPL_READ_FAILED && !attempted && x.writes.empty());
    }
    for(uint64_t off:{r.scratch0,r.scratch1,r.core,r.reset,r.rlc}) {
        Fake x(r);x.mem[off]=0; if(off==r.rlc)x.mem[off]=0x80000000;
        attempted=false;o=sm::execute(r,x.hw(),true,true,attempted);
        assert(o.state==N33_PSPL_SMU_BAD_PRECONDITION && !attempted && x.writes.empty());
    }
    for(uint32_t reply:{0u,0xfdu}) {
        Fake x(r);x.mem[r.resp]=reply;attempted=false;
        o=sm::execute(r,x.hw(),true,true,attempted);
        assert(o.state==N33_PSPL_SMU_NOT_READY && !attempted && x.writes.empty());
    }
    for(unsigned at=1;at<=9;++at) {
        Fake x(r);x.failWrite=at;attempted=false;
        o=sm::execute(r,x.hw(),true,true,attempted);
        assert(o.state==N33_PSPL_SMU_WRITE_FAILED && attempted && o.register_writes==at-1);
    }
    {Fake x(r);x.timeout=true;attempted=false;o=sm::execute(r,x.hw(),true,true,attempted);
     assert(o.state==N33_PSPL_SMU_TIMEOUT && o.elapsed_ms==1000 && x.messages==1 && attempted);}
    {Fake x(r);x.allOnes=true;attempted=false;o=sm::execute(r,x.hw(),true,true,attempted);
     assert(o.state==N33_PSPL_READ_FAILED && x.messages==1 && attempted);}
    {Fake x(r);x.gc=true;attempted=false;o=sm::execute(r,x.hw(),true,true,attempted);
     assert(o.state==N33_PSPL_SMU_GC_READY && o.smu_reset_after==0x1f && o.smu_core_after==3);}
    {Fake x(r);x.delayed=3;attempted=false;o=sm::execute(r,x.hw(),true,true,attempted);
     assert(o.state==N33_PSPL_SMU_ACKED && o.elapsed_ms==1003);}
    // Failed reads before/after the first write consume the latch only after it.
    for(unsigned at=1;at<=20;++at) {
        Fake x(r);x.failRead=at;attempted=false;
        o=sm::execute(r,x.hw(),true,true,attempted);
        assert(o.state==N33_PSPL_READ_FAILED && attempted==(at>9));
    }
    puts("SMU13 fixed sequence, gates, one-shot, response errors and GC observation: passed");
}
