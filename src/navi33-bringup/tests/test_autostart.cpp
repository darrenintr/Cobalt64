// SPDX-License-Identifier: MIT
#include "../src/Navi33Autostart.h"
#include <cassert>
#include <cstdio>
#include <vector>
using namespace navi33;
namespace as=autostart;
struct Fake {
    std::vector<uint32_t> modes; uint32_t ms=0;
    unsigned failAt=99, transientAt=99, transients=0, transientWrites=0;
    static uint32_t step(void *c,const as::Step &s,uint32_t *w) {
        auto &f=*static_cast<Fake *>(c);
        const unsigned i=static_cast<unsigned>(f.modes.size());
        f.modes.push_back(s.mode);
        unsigned idx=0;for(;idx<as::kStepCount && &as::kSteps[idx]!=&s;++idx){}
        if(idx==f.transientAt && f.transients) {--f.transients;*w=f.transientWrites;return s.retryState;}
        if(idx==f.failAt){*w=3;return N33_PSPL_CMD_FAILED;}
        (void)i;*w=1;return s.success;
    }
    static void sleep(void *c,uint32_t ms){static_cast<Fake *>(c)->ms+=ms;}
};
int main() {
    // The proven order and success states, and the firmware ranges.
    const uint32_t modes[]={N33_PSP_LOAD_RUN,N33_PSP_LOAD_RING_RUN,N33_PSP_LOAD_FW_RUN,N33_PSP_LOAD_FW_RUN,
                            N33_PSP_LOAD_FW_RUN,N33_SMU_SCPM_RUN,N33_MES_START_RUN,N33_MEC_QUEUE_RUN};
    const uint32_t ok[]={20,28,32,32,32,39,43,48};
    static_assert(as::kStepCount==8,"eight stages");
    for(unsigned i=0;i<8;++i)assert(as::kSteps[i].mode==modes[i] && as::kSteps[i].success==ok[i]);
    assert(as::kSteps[2].startAt==26 && as::kSteps[2].stopAfter==27);           // SMU image first, alone
    assert(as::kSteps[3].startAt==0 && as::kSteps[3].stopAfter==26);
    assert(as::kSteps[4].stopAfter==N33_PSP_FW_STOP_AUTOLOAD && as::kSteps[4].autoload);
    for(unsigned i=0;i<8;++i)assert(as::kSteps[i].autoload==(i==4));
    {   Fake f;auto r=as::run(&f,Fake::step,Fake::sleep);
        assert(r.done && r.reached==8 && f.modes.size()==8 && f.ms==0);
        for(unsigned i=0;i<8;++i)assert(f.modes[i]==modes[i] && r.state[i]==ok[i] && r.tries[i]==1);
    }
    for(unsigned at=0;at<8;++at) {   // stops at the first failure, runs nothing after it
        Fake f;f.failAt=at;auto r=as::run(&f,Fake::step,Fake::sleep);
        assert(!r.done && r.reached==at && f.modes.size()==at+1 && r.state[at]==N33_PSPL_CMD_FAILED);
    }
    {   Fake f;f.transientAt=5;f.transients=3;auto r=as::run(&f,Fake::step,Fake::sleep);   // IMU flicker
        assert(r.done && r.tries[5]==4 && f.ms==3*as::kRetryMs);
    }
    {   Fake f;f.transientAt=1;f.transients=2;auto r=as::run(&f,Fake::step,Fake::sleep);   // ring not ready yet
        assert(r.done && r.tries[1]==3);
    }
    {   Fake f;f.transientAt=5;f.transients=1;f.transientWrites=3;                       // wrote: never retried
        auto r=as::run(&f,Fake::step,Fake::sleep);
        assert(!r.done && r.reached==5 && r.tries[5]==1);
    }
    {   Fake f;f.transientAt=5;f.transients=1000;auto r=as::run(&f,Fake::step,Fake::sleep); // bounded
        assert(!r.done && r.tries[5]==as::kRetries+1);
    }
    {   Fake f;f.transientAt=0;f.transients=1;auto r=as::run(&f,Fake::step,Fake::sleep);   // sOS has no retry
        assert(!r.done && r.reached==0 && r.tries[0]==1);
    }
    as::Gates g{};g.on=true;g.hwProbe=true;assert(as::allowed(g));
    bool as::Gates::*kills[]={&as::Gates::autostartOff,&as::Gates::pspOff,&as::Gates::smuOff,
                              &as::Gates::mesOff,&as::Gates::mecOff,&as::Gates::hwOff};
    for(auto k:kills){as::Gates x=g;x.*k=true;assert(!as::allowed(x));}
    {as::Gates x=g;x.on=false;assert(!as::allowed(x));x=g;x.hwProbe=false;assert(!as::allowed(x));}
    puts("Autostart: proven order and success states, stop at first failure, bounded zero-write retries, gates: passed");
}
