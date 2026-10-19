// Host-only tests for the SDMA0 Queue0 ring-test plan. No GPU access: the
// snapshot is the RX 7600's real census (runs/20261019-143838, kext 0.0.21,
// after the sOS -> SCPM sequence).
#include "../src/Navi33Sdma6RingTest.h"
#include <cassert>
#include <cstdio>

using namespace navi33::sdma6_test;

namespace {
Snapshot census() {
    Snapshot s{};
    s.rlcBootload=0xc000001fu; s.psp81=0x1d91ce37u;
    s.sdmaId=1u; s.sdmaStatus=0x46dee557u;
    s.gcL1TlbCntl=0x501u; s.f32Cntl=0x08084001u;
    s.utcl1Cntl=0x1f000288u; s.utcl1Page=0x010cec20u; s.watchdog=0u;
    s.q0[0]=0x00040800u;          // RB_CNTL; base and pointers 0
    s.ibCntl=0x100u; s.doorbell=0u;
    s.fbBase=0x8000u; s.fbTop=0x81ffu;
    return s;
}
uint32_t find(const Plan &p,uint16_t reg,size_t nth=0) {
    for(size_t i=0;i<p.setupCount;++i)
        if(p.setup[i].reg==reg && nth--==0) return p.setup[i].value;
    assert(!"register not in plan");
    return 0u;
}
} // namespace

int main() {
    const Plan p=build(census());
    assert(p.ok());
    // Every buffer is VRAM at the FB base + 248 MiB, inside the 256 MiB BAR0.
    assert(p.windowMc==0x800f800000ull && p.ringMc==0x800f800000ull);
    assert(p.testMc==0x800f802000ull && p.fenceMc==0x800f803000ull);
    assert(p.sourceMc==0x800f804000ull && p.targetMc==0x800f805000ull);
    // Linux's values, computed from what the card holds now.
    assert(p.setup[0].reg==SemWaitFailTimer && p.setup[0].value==0u);
    assert(find(p,Q0RbCntl,0)==0x00840814u);   // RB_SIZE 10, RB_PRIV
    assert(find(p,Q0RbCntl,1)==0x00841815u);   // + RPTR_WB, F32 poll, enable
    assert(find(p,Q0RbBase)==0x800f8000u && find(p,Q0RbBaseHi)==0u);
    assert(find(p,Q0RptrAddrLo)==0x0f801000u && find(p,Q0RptrAddrHi)==0x80u);
    assert(find(p,Q0WptrPollAddrLo)==0x0f801008u && find(p,Q0WptrPollAddrHi)==0x80u);
    assert(find(p,Utcl1Cntl)==0x1f000689u);    // RESP_MODE 3, REDO_DELAY 9
    assert(find(p,Utcl1Page)==0x010cec20u);    // already Linux's value
    assert(find(p,WatchdogCntl)==1u);
    assert(find(p,F32Cntl)==0x08084000u);      // HALT cleared, TH1 kept
    assert(find(p,Q0IbCntl)==0x101u);
    assert(find(p,Q0MinorPtrUpdate,0)==1u && find(p,Q0MinorPtrUpdate,1)==0u);
    // Order: the engine is unhalted only after the ring is described, and
    // the ring is enabled last but one, as in sdma_v6_0_gfx_resume_instance.
    assert(p.setup[p.setupCount-3].reg==F32Cntl);
    assert(p.setup[p.setupCount-2].reg==Q0RbCntl &&
           (p.setup[p.setupCount-2].value&kRbEnable));
    assert(p.setup[p.setupCount-1].reg==Q0IbCntl);
    for(size_t i=0;i+3<p.setupCount;++i)
        assert(!(p.setup[i].reg==Q0RbCntl && (p.setup[i].value&kRbEnable)) &&
               p.setup[i].reg!=F32Cntl);
    assert(p.setupCount==23u);
    // Packets: WRITE_LINEAR one dword, COPY_LINEAR 4 KiB, FENCE UC, NOP pad.
    const uint32_t a[5]={2u,0x0f802000u,0x80u,0u,0xdeadbeefu};
    for(int i=0;i<5;++i) assert(p.ringA[i]==a[i]);
    for(uint32_t i=5;i<kAlignDwords;++i) assert(p.ringA[i]==kOpNop);
    const uint32_t b[11]={1u,4095u,0u,0x0f804000u,0x80u,0x0f805000u,0x80u,
                          0x00030005u,0x0f803000u,0x80u,0xc0b1606au};
    for(int i=0;i<11;++i) assert(p.ringB[i]==b[i]);
    for(uint32_t i=11;i<kAlignDwords;++i) assert(p.ringB[i]==kOpNop);
    assert(p.wptrAfterA==64u && p.wptrAfterB==128u);
    // Park: ring off, IBs off, engine halted again.
    assert(p.park[0].reg==Q0RbCntl && p.park[0].value==0x00841814u);
    assert(p.park[1].reg==Q0IbCntl && p.park[1].value==0x100u);
    assert(p.park[2].reg==F32Cntl && p.park[2].value==0x08084001u);
    // Only SDMA0 registers; no hub, no MMHUB, no page table.
    for(size_t i=0;i<p.setupCount;++i) assert(allowed(p.setup[i].reg));
    for(const Write &w:p.park) assert(allowed(w.reg));
    assert(!allowed(0x15bcu) && !allowed(0x167fu) && !allowed(0x1689u) &&
           !allowed(0x00d8u) && !allowed(0x0025u));
    assert(segmentOf(F32Cntl)==1u && segmentOf(Q0RbCntl)==0u);

    // Every refusal on its own.
    struct Case { void (*mutate)(Snapshot &); uint32_t bit; } cases[]={
        {[](Snapshot &s){s.rlcBootload=0x1fu;},GcNotUp},
        {[](Snapshot &s){s.psp81=0u;},SosNotAlive},
        {[](Snapshot &s){s.sdmaId=0u;},NoSdma},
        {[](Snapshot &s){s.q0[0]|=kRbEnable;},QueueBusy},
        {[](Snapshot &s){s.q0[5]=0x40u;},QueueBusy},
        {[](Snapshot &s){s.ibCntl|=kIbEnable;},QueueBusy},
        {[](Snapshot &s){s.doorbell=kDoorbellEnable;},QueueBusy},
        {[](Snapshot &s){s.f32Cntl&=~kHalt;},NotHalted},
        {[](Snapshot &s){s.sdmaStatus&=~1u;},SdmaNotIdle},
        {[](Snapshot &s){s.gcL1TlbCntl=0x519u;},NotPhysicalMode},  // mode 3
        {[](Snapshot &s){s.gcL1TlbCntl=0x500u;},NotPhysicalMode},  // L1 off
        {[](Snapshot &s){s.fbBase=0u;},BadFbLocation},
        {[](Snapshot &s){s.fbTop=s.fbBase;},BadFbLocation},        // 16 MiB FB
        {[](Snapshot &s){s.utcl1Page=UINT32_MAX;},ReadFailed},
    };
    for(const Case &c:cases) {
        Snapshot s=census(); c.mutate(s);
        const Plan r=build(s);
        assert(!r.ok() && (r.refusal&c.bit) && r.setupCount==0u);
    }
    std::printf("SDMA6 Queue0 ring-test plan: %zu setup writes from the live census, "
                "VRAM-only, SDMA0 registers only, every refusal checked\n",
                p.setupCount);
}
