// Offline: run the G30 SDMA0 Queue0 ring-test plan (Navi33Sdma6RingTest.h)
// against the n33peek section of a results file and print the write list or
// the refusals. Reads text only; never opens a device.
//   sdma6-ring-plan RESULTS_FILE
#include "../src/Navi33Sdma6RingTest.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

using namespace navi33::sdma6_test;

namespace {
struct Window { uint32_t first=0u; std::vector<uint32_t> words; };
using Windows=std::map<std::string,Window>;

Windows parse(FILE *f) {
    Windows w; std::string cur; char line[1024];
    while(std::fgets(line,sizeof(line),f)) {
        char name[128]; unsigned seg=0u,first=0u,count=0u;
        if(std::sscanf(line,"== %127s %*s seg%u reg 0x%x x%u",name,&seg,&first,&count)==4) {
            cur=name; w[cur].first=first; w[cur].words.clear(); continue;
        }
        if(std::strncmp(line,"== ",3)==0 || std::strncmp(line,"GPU writes",10)==0) {
            cur.clear(); continue;
        }
        unsigned at=0u; int used=0;
        if(cur.empty() || std::sscanf(line," 0x%x:%n",&at,&used)!=1) continue;
        const char *p=line+used; char *end=nullptr;
        for(;;) {
            const unsigned long v=std::strtoul(p,&end,16);
            if(end==p) break;
            w[cur].words.push_back(static_cast<uint32_t>(v)); p=end;
        }
    }
    return w;
}
bool word(const Windows &w,const char *name,size_t i,uint32_t &out) {
    auto it=w.find(name);
    if(it==w.end() || i>=it->second.words.size()) return false;
    out=it->second.words[i]; return true;
}
const char *regName(uint16_t r) {
    switch(r) {
    case SemWaitFailTimer: return "SEM_WAIT_FAIL_TIMER_CNTL";
    case WatchdogCntl: return "WATCHDOG_CNTL";
    case Utcl1Cntl: return "UTCL1_CNTL";
    case Utcl1Page: return "UTCL1_PAGE";
    case Q0RbCntl: return "QUEUE0_RB_CNTL";
    case Q0RbBase: return "QUEUE0_RB_BASE";
    case Q0RbBaseHi: return "QUEUE0_RB_BASE_HI";
    case Q0Rptr: return "QUEUE0_RB_RPTR";
    case Q0RptrHi: return "QUEUE0_RB_RPTR_HI";
    case Q0Wptr: return "QUEUE0_RB_WPTR";
    case Q0WptrHi: return "QUEUE0_RB_WPTR_HI";
    case Q0RptrAddrHi: return "QUEUE0_RB_RPTR_ADDR_HI";
    case Q0RptrAddrLo: return "QUEUE0_RB_RPTR_ADDR_LO";
    case Q0IbCntl: return "QUEUE0_IB_CNTL";
    case Q0Doorbell: return "QUEUE0_DOORBELL";
    case Q0WptrPollAddrHi: return "QUEUE0_RB_WPTR_POLL_ADDR_HI";
    case Q0WptrPollAddrLo: return "QUEUE0_RB_WPTR_POLL_ADDR_LO";
    case Q0MinorPtrUpdate: return "QUEUE0_MINOR_PTR_UPDATE";
    case F32Cntl: return "F32_CNTL";
    default: return "?";
    }
}
} // namespace

int main(int argc,char **argv) {
    if(argc!=2) { std::fputs("usage: sdma6-ring-plan RESULTS_FILE\n",stderr); return 2; }
    FILE *f=std::fopen(argv[1],"r");
    if(!f) { std::perror(argv[1]); return 2; }
    const Windows w=parse(f); std::fclose(f);
    Snapshot s{}; bool ok=true;
    ok&=word(w,"RLC_BOOTLOAD_STATUS",0,s.rlcBootload);
    ok&=word(w,"PSP_C2PMSG",49,s.psp81);              // C2PMSG_81
    ok&=word(w,"SDMA0_ID",0,s.sdmaId);
    ok&=word(w,"SDMA0_STATUS",0,s.sdmaStatus);
    ok&=word(w,"GCMC_VM_MX_L1_TLB_CNTL",0,s.gcL1TlbCntl);
    ok&=word(w,"SDMA0_F32_CNTL",0,s.f32Cntl);
    ok&=word(w,"SDMA0_UTCL1",0,s.utcl1Cntl);
    ok&=word(w,"SDMA0_UTCL1",3,s.utcl1Page);
    ok&=word(w,"SDMA0_WATCHDOG",3,s.watchdog);
    for(size_t i=0;i<7u;++i) ok&=word(w,"SDMA0_Q0_RB",i,s.q0[i]);
    ok&=word(w,"SDMA0_Q0_CTL",2,s.ibCntl);           // 0x008a
    ok&=word(w,"SDMA0_Q0_CTL",10,s.doorbell);        // 0x0092
    ok&=word(w,"GCMC_FB_LOCATION",0,s.fbBase);
    ok&=word(w,"GCMC_FB_LOCATION",1,s.fbTop);
    if(!ok) {
        std::puts("missing windows: this results file predates kext 0.0.21");
        return 3;
    }
    const Plan p=build(s);
    if(!p.ok()) {
        static const char *names[]={"GcNotUp","SosNotAlive","NoSdma","ReadFailed",
            "QueueBusy","NotHalted","SdmaNotIdle","NotPhysicalMode","BadFbLocation"};
        std::printf("REFUSED 0x%x:",p.refusal);
        for(unsigned b=0;b<9u;++b) if(p.refusal&(1u<<b)) std::printf(" %s",names[b]);
        std::puts("");
        return 1;
    }
    std::printf("plan OK (read-only; nothing is written by this tool)\n"
                "VRAM window MC 0x%llx: ring 0x%llx test 0x%llx fence 0x%llx "
                "source 0x%llx target 0x%llx\n",
                (unsigned long long)p.windowMc,(unsigned long long)p.ringMc,
                (unsigned long long)p.testMc,(unsigned long long)p.fenceMc,
                (unsigned long long)p.sourceMc,(unsigned long long)p.targetMc);
    for(size_t i=0;i<p.setupCount;++i)
        std::printf("%2zu seg%u 0x%04x %-28s 0x%08x\n",i+1,segmentOf(p.setup[i].reg),
                    p.setup[i].reg,regName(p.setup[i].reg),p.setup[i].value);
    std::printf("ring A (WPTR %u):",p.wptrAfterA);
    for(uint32_t v:p.ringA) std::printf(" %08x",v);
    std::printf("\nring B (WPTR %u):",p.wptrAfterB);
    for(uint32_t v:p.ringB) std::printf(" %08x",v);
    std::puts("");
    for(const Write &x:p.park)
        std::printf("park seg%u 0x%04x %-28s 0x%08x\n",segmentOf(x.reg),x.reg,
                    regName(x.reg),x.value);
    return 0;
}
