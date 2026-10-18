// Host-only checks for the GFX11 MES bring-up plan (no hardware).
// Expected values are derived by hand from the upstream mes_v11_0.c order and
// the gc_11_0_0 field positions, not read back from the planner.
#include "../src/Navi33GfxMes.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

namespace mes=navi33::gfx11_mes;
namespace kiq=navi33::gfx11_kiq;

static const mes::Write *find(const mes::Plan &p,const char *name) {
    for(uint8_t i=0u;i<p.count;++i)
        if(strcmp(p.writes[i].name,name)==0)
            return &p.writes[i];
    return nullptr;
}

static void expectValue(const mes::Plan &p,const char *name,uint32_t value) {
    const mes::Write *w=find(p,name);
    assert(w && "register missing from plan");
    assert(w->value==value);
}

static void enableWithBothPipes() {
    mes::EnableInput in{};
    in.ucStart[0]=0x1000u;
    in.ucStart[1]=0x2000u;
    const mes::Plan p=mes::buildEnable(in);
    assert(p.code==mes::Code::PlanOnly && p.refusalMask==0u);
    assert(p.count==6u);
    // Step 1: reset both pipes from zero.
    assert(p.writes[0].dword==mes::kCpMesCntl);
    assert(p.writes[0].value==0x00030000u);
    // Step 2: per pipe, start address = uc >> 2, under the pipe's select.
    assert(p.writes[1].dword==mes::kCpMesPrgrmCntrStart);
    assert(p.writes[1].value==0x400u && p.writes[1].grbmPipe==0u);
    assert(p.writes[2].dword==mes::kCpMesPrgrmCntrStartHi);
    assert(p.writes[2].value==0u);
    assert(p.writes[3].value==0x800u && p.writes[3].grbmPipe==1u);
    // Step 3: unhalt and activate both pipes, built from zero.
    assert(p.writes[5].dword==mes::kCpMesCntl);
    assert(p.writes[5].value==0x0c000000u);
    assert(p.writes[5].grbmPipe==mes::kNoGrbmPipe);
    for(uint8_t i=0u;i<p.count;++i)
        assert(p.writes[i].grbmMe==3u && p.writes[i].seg==1u);
}

static void enableWithRealFirmwareStart() {
    // gc_11_0_2_mes_2.bin and gc_11_0_2_mes1.bin (linux-firmware 20260915) both
    // have mes_uc_start_addr_lo = 0xf0005000 and _hi = 0xffffffff, so the
    // 64-bit start is 0xffffffff_f0005000 (header offsets 56 and 60).
    mes::EnableInput in{};
    in.ucStart[0]=0xffffffff'f0005000ull;
    in.ucStart[1]=0xffffffff'f0005000ull;
    const mes::Plan p=mes::buildEnable(in);
    assert(p.code==mes::Code::PlanOnly);
    // start >> 2 = 0x3fffffff_fc001400.
    assert(p.writes[1].value==0xfc001400u);
    assert(p.writes[2].value==0x3fffffffu);
}

static void enableSchedPipeOnly() {
    mes::EnableInput in{};
    in.ucStart[0]=0x1000u;
    in.kiq=false;
    const mes::Plan p=mes::buildEnable(in);
    assert(p.code==mes::Code::PlanOnly);
    assert(p.count==4u);
    assert(p.writes[0].value==0x00010000u);    // PIPE1_RESET stays 0
    assert(p.writes[3].value==0x04000000u);    // only PIPE0_ACTIVE
    for(uint8_t i=0u;i<p.count;++i)
        assert(p.writes[i].grbmPipe!=1u);      // pipe 1 untouched
}

static void enableRefusals() {
    mes::EnableInput in{};
    in.ucStart[0]=0u;
    assert(mes::buildEnable(in).refusalMask&mes::NoFirmware);

    in=mes::EnableInput{};
    in.ucStart[0]=0x1000u;
    in.ucStart[1]=0u;                          // KIQ pipe needs firmware too
    assert(mes::buildEnable(in).refusalMask&mes::NoFirmware);

    in=mes::EnableInput{};
    in.ucStart[0]=0x1002u;                     // not 4-byte aligned
    in.ucStart[1]=0x2000u;
    assert(mes::buildEnable(in).refusalMask&mes::BadStartAddr);
}

static void disableClearsActiveAndHalts() {
    const mes::Plan p=mes::buildDisable(0x0c000000u,true);
    assert(p.count==1u && p.code==mes::Code::PlanOnly);
    // ACTIVE cleared, ICACHE set, both resets, HALT: 0x40000000|0x10|0x30000.
    assert(p.writes[0].value==0x40030010u);
    const mes::Plan keep=mes::buildDisable(0x00000001u,true);
    assert((keep.writes[0].value&0x1u)==0x1u);  // unrelated bit preserved
}

static mes::QueueInput goodQueue() {
    mes::QueueInput in{};
    in.q.mqdGpuVa=0x0000000200002000ull;
    in.q.pqGpuVa=0x0000000400010000ull;
    in.q.rptrGpuVa=0x0000000500000100ull;
    in.q.wptrGpuVa=0x0000000500000108ull;
    in.q.queueBytes=0x1000u;
    in.q.useDoorbell=true;
    in.q.doorbellIndex=0x40u;
    in.vmidCurrent=0xffffffffu;
    in.doorbellCurrent=0xffffffffu;
    return in;
}

static void kiqQueueGolden() {
    const mes::Plan p=mes::buildKiqQueueRegs(goodQueue());
    assert(p.code==mes::Code::PlanOnly && p.refusalMask==0u);
    assert(p.count==15u);
    assert(strcmp(p.writes[0].name,"CP_HQD_VMID.VMID=0")==0);
    assert(strcmp(p.writes[p.count-1].name,"CP_HQD_ACTIVE=1")==0);
    for(uint8_t i=0u;i<p.count;++i)
        assert(p.writes[i].grbmMe==3u && p.writes[i].grbmPipe==1u);

    expectValue(p,"CP_HQD_VMID.VMID=0",0xfffffff0u);
    expectValue(p,"CP_HQD_PQ_DOORBELL_CONTROL.EN=0",0xbfffffffu);
    expectValue(p,"CP_MQD_BASE_ADDR",0x00002000u);
    expectValue(p,"CP_MQD_BASE_ADDR_HI",0x2u);
    expectValue(p,"CP_MQD_CONTROL=0",0u);          // literal 0, not the mqd
    expectValue(p,"CP_HQD_PQ_BASE",0x04000100u);
    expectValue(p,"CP_HQD_PQ_RPTR_REPORT_ADDR",0x100u);
    expectValue(p,"CP_HQD_PQ_RPTR_REPORT_ADDR_HI",0x5u);
    // PQ control from default 0x00308509: QUEUE_SIZE 9 (already), RPTR_BLOCK
    // cleared (see the quirk test), UNORD 1, PRIV 1, KMD 1, NO_UPDATE_RPTR 1.
    expectValue(p,"CP_HQD_PQ_CONTROL",0xd8308009u);
    expectValue(p,"CP_HQD_PQ_WPTR_POLL_ADDR",0x108u);   // 8-byte masked
    expectValue(p,"CP_HQD_PQ_WPTR_POLL_ADDR_HI",0x5u);
    expectValue(p,"CP_HQD_PQ_DOORBELL_CONTROL",0x40000100u);
    expectValue(p,"CP_HQD_PERSISTENT_STATE",0x0be05501u);
    expectValue(p,"CP_HQD_ACTIVE=1",1u);
}

static void rptrBlockSizeQuirk() {
    // Upstream passes (order(1024)-1) << 8 into REG_SET_FIELD, which shifts
    // again. The RPTR_BLOCK_SIZE field (bits 8..13) therefore reads 0, not 9.
    const mes::Plan p=mes::buildKiqQueueRegs(goodQueue());
    const mes::Write *w=find(p,"CP_HQD_PQ_CONTROL");
    assert(w && ((w->value>>8u)&0x3fu)==0u);

    // The legacy KIQ path writes 9 there, so the two paths differ on purpose.
    mes::QueueInput in=goodQueue();
    const kiq::Plan legacy=kiq::buildKiqInit(in.q);
    const kiq::Write *lw=nullptr;
    for(uint8_t i=0u;i<legacy.count;++i)
        if(strcmp(legacy.writes[i].name,"CP_HQD_PQ_CONTROL")==0)
            lw=&legacy.writes[i];
    assert(lw && ((lw->value>>8u)&0x3fu)==9u);
}

static void kiqQueueRefusals() {
    mes::QueueInput in=goodQueue();
    in.q.queueBytes=0x3000u;
    assert(mes::buildKiqQueueRegs(in).refusalMask==mes::QueueRefused);

    in=goodQueue();
    in.q.wptrGpuVa=0x0000000500000104ull;      // 4-byte only; MES needs 8
    assert(mes::buildKiqQueueRegs(in).refusalMask==mes::QueueRefused);

    in=goodQueue();
    in.q.hqdActiveAtStart=true;
    assert(mes::buildKiqQueueRegs(in).refusalMask==mes::QueueRefused);
}

static void mapMesRingPacket() {
    mes::MapInput in{};
    in.mqdGpuVa=0x0000000200002000ull;
    in.wptrGpuVa=0x0000000500000108ull;
    in.doorbellIndex=0x2u;
    const mes::MapPacket m=mes::buildMapMesRing(in);
    assert(m.ok);
    assert(m.dw[0]==0xc005a200u);              // type 3, op 0xa2, count 5
    assert(m.dw[1]==0x34080000u);              // ME 2 (<<18), ENGINE 5 (<<26), NUM_Q 1 (<<29)
    assert(m.dw[2]==0x8u);                     // doorbell offset << 2
    assert(m.dw[3]==0x2000u && m.dw[4]==0x2u);
    assert(m.dw[5]==0x108u && m.dw[6]==0x5u);

    in.pipe=1u;                                // the KIQ pipe bit
    assert(mes::buildMapMesRing(in).dw[1]==0x34090000u);

    in.mqdGpuVa=0x0000000200002003ull;         // not 4-byte aligned
    assert(!mes::buildMapMesRing(in).ok);
}

int main() {
    enableWithBothPipes();
    enableWithRealFirmwareStart();
    enableSchedPipeOnly();
    enableRefusals();
    disableClearsActiveAndHalts();
    kiqQueueGolden();
    rptrBlockSizeQuirk();
    kiqQueueRefusals();
    mapMesRingPacket();
    puts("Navi33 GFX11 MES bring-up plan: PASS (offline)");
}
