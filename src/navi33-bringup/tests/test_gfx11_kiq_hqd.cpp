// Host-only checks for the GFX11 KIQ HQD register plan (no hardware).
// Expected values are derived by hand from gc_11_0_0 field positions and
// the upstream gfx_v11_0.c order, not read back from the planner.
#include "../src/Navi33Gfx11KiqHqd.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

namespace kiq = navi33::gfx11_kiq;

static kiq::KiqQueue goodQueue() {
    kiq::KiqQueue q{};
    q.mqdGpuVa=0x0000000200002000ull;
    q.eopGpuVa=0x0000000300000000ull;
    q.pqGpuVa=0x0000000400010000ull;
    q.rptrGpuVa=0x0000000500000100ull;
    q.wptrGpuVa=0x0000000500000108ull;
    q.queueBytes=0x1000u;
    q.eopBytes=2048u;
    q.useDoorbell=true;
    q.doorbellIndex=0x40u;
    q.doorbellRangeLower=0x40u;
    q.doorbellRangeUpper=0x7fu;
    q.pollCntlCurrent=0xffffffffu;
    q.pqStatusCurrent=0xffffffffu;
    return q;
}

static const kiq::Write *find(const kiq::Plan &p,const char *name) {
    for(uint8_t i=0u;i<p.count;++i)
        if(strcmp(p.writes[i].name,name)==0)
            return &p.writes[i];
    return nullptr;
}

static void expectValue(const kiq::Plan &p,const char *name,uint32_t value) {
    const kiq::Write *w=find(p,name);
    assert(w && "register missing from plan");
    assert(w->value==value);
}

static void goldenKiqPlan() {
    const kiq::Plan p=kiq::buildKiqInit(goodQueue());
    assert(p.code==kiq::Code::PlanOnly);
    assert(p.refusalMask==0u);
    // First and last steps follow gfx_v11_0_kiq_init_register().
    assert(strcmp(p.writes[0].name,"CP_PQ_WPTR_POLL_CNTL.EN=0")==0);
    assert(strcmp(p.writes[p.count-1].name,"CP_PQ_STATUS.DOORBELL_ENABLE=1")==0);
    assert(p.count<=kiq::kMaxWrites);

    // EOP: EOP_SIZE = order(2048/4) - 1 = 8 over the default 0x6.
    expectValue(p,"CP_HQD_EOP_CONTROL",0x00000008u);
    expectValue(p,"CP_HQD_EOP_BASE_ADDR",0x03000000u);   // eop >> 8
    expectValue(p,"CP_HQD_EOP_BASE_ADDR_HI",0u);         // eop >> 40
    // Doorbell: offset (0x40 << 2) at bits 2..27 plus DOORBELL_EN (bit 30).
    expectValue(p,"CP_HQD_PQ_DOORBELL_CONTROL",0x40000100u);
    // MQD address split, VMID 0 over the default 0x100.
    expectValue(p,"CP_MQD_BASE_ADDR",0x00002000u);
    expectValue(p,"CP_MQD_BASE_ADDR_HI",0x2u);
    expectValue(p,"CP_MQD_CONTROL",0x00000100u);
    // Ring base is >> 8.
    expectValue(p,"CP_HQD_PQ_BASE",0x04000100u);
    expectValue(p,"CP_HQD_PQ_BASE_HI",0u);
    // PQ control, from default 0x00308509:
    //   QUEUE_SIZE = order(4096/4)-1 = 9 in bits 0..5 (already 9)
    //   RPTR_BLOCK_SIZE = 9 in bits 8..13 (default 5, so 0x500 -> 0x900)
    //   UNORD_DISPATCH (bit 28), PRIV_STATE (bit 30), KMD_QUEUE (bit 31)
    expectValue(p,"CP_HQD_PQ_CONTROL",0xd0308909u);
    expectValue(p,"CP_HQD_PQ_RPTR_REPORT_ADDR",0x00000100u);
    expectValue(p,"CP_HQD_PQ_RPTR_REPORT_ADDR_HI",0x5u);
    expectValue(p,"CP_HQD_PQ_WPTR_POLL_ADDR",0x00000108u);
    expectValue(p,"CP_HQD_PQ_WPTR_POLL_ADDR_HI",0x5u);
    // Doorbell range: (index * 2) << 2.
    expectValue(p,"CP_MEC_DOORBELL_RANGE_LOWER",0x00000200u);
    expectValue(p,"CP_MEC_DOORBELL_RANGE_UPPER",0x000003f8u);
    expectValue(p,"CP_HQD_PQ_WPTR_LO",0u);
    expectValue(p,"CP_HQD_PQ_WPTR_HI",0u);
    expectValue(p,"CP_HQD_VMID",0u);
    // PRELOAD_SIZE 0x55 is already the default value here.
    expectValue(p,"CP_HQD_PERSISTENT_STATE",0x0be05501u);
    expectValue(p,"CP_HQD_ACTIVE (KIQ)",1u);
    // CP_PQ_WPTR_POLL_CNTL.EN cleared; CP_PQ_STATUS keeps DOORBELL_ENABLE.
    expectValue(p,"CP_PQ_WPTR_POLL_CNTL.EN=0",0x7fffffffu);
    expectValue(p,"CP_PQ_STATUS.DOORBELL_ENABLE=1",0xffffffffu);
}

static void noDoorbellPath() {
    kiq::KiqQueue q=goodQueue();
    q.useDoorbell=false;
    q.pqStatusCurrent=0u;
    const kiq::Plan p=kiq::buildKiqInit(q);
    assert(p.code==kiq::Code::PlanOnly);
    // No range writes and no DOORBELL_ENABLE step without a doorbell.
    assert(find(p,"CP_MEC_DOORBELL_RANGE_LOWER")==nullptr);
    assert(find(p,"CP_MEC_DOORBELL_RANGE_UPPER")==nullptr);
    assert(find(p,"CP_PQ_STATUS.DOORBELL_ENABLE=1")==nullptr);
    // EN=0 gives the zero doorbell word.
    expectValue(p,"CP_HQD_PQ_DOORBELL_CONTROL",0u);
}

static void tmzAndTunnelingBits() {
    kiq::KiqQueue q=goodQueue();
    q.tmzQueue=true;
    q.allowTunneling=true;
    const kiq::Plan p=kiq::buildKiqInit(q);
    const kiq::Write *w=find(p,"CP_HQD_PQ_CONTROL");
    assert(w);
    assert(w->value&0x00400000u);   // TMZ, bit 22
    assert(w->value&0x20000000u);   // TUNNEL_DISPATCH, bit 29
}

static void refusals() {
    kiq::KiqQueue q=goodQueue();
    q.queueBytes=0x3000u;                       // not a power of two
    assert(kiq::buildKiqInit(q).refusalMask&kiq::BadQueueSize);

    q=goodQueue();
    q.queueBytes=0x800u;                        // below one GPU page
    assert(kiq::buildKiqInit(q).refusalMask&kiq::BadQueueSize);

    q=goodQueue();
    q.eopBytes=2000u;
    assert(kiq::buildKiqInit(q).refusalMask&kiq::BadEopSize);

    q=goodQueue();
    q.eopGpuVa+=0x80u;                          // not 256-byte aligned
    assert(kiq::buildKiqInit(q).refusalMask&kiq::BadAlignment);

    q=goodQueue();
    q.wptrGpuVa|=2u;                            // not 4-byte aligned
    assert(kiq::buildKiqInit(q).refusalMask&kiq::BadAlignment);

    q=goodQueue();
    q.mqdGpuVa=1ull<<48;
    assert(kiq::buildKiqInit(q).refusalMask&kiq::AddressTooWide);

    q=goodQueue();
    q.doorbellIndex=kiq::kMaxDoorbellIndex+1u;
    assert(kiq::buildKiqInit(q).refusalMask&kiq::BadDoorbell);

    q=goodQueue();
    q.hqdActiveAtStart=true;                    // dequeue path not modelled
    const kiq::Plan p=kiq::buildKiqInit(q);
    assert(p.refusalMask&kiq::DequeueRequired);
    assert(p.code==kiq::Code::Refused);
    assert(p.count==0u);
}

int main() {
    goldenKiqPlan();
    noDoorbellPath();
    tmzAndTunnelingBits();
    refusals();
    puts("Navi33 GFX11 KIQ HQD init plan: PASS (offline)");
}
