// Host-only checks for the MES KIQ-pipe MQD image (no hardware, no memory I/O).
// Expected dwords are derived by hand from mes_v11_0_mqd_init() and the
// v11_compute_mqd offsets, not read back from the builder.
#include "../src/Navi33GfxMesMqd.h"
#include <assert.h>
#include <stdio.h>

namespace mes=navi33::gfx11_mes;
namespace mm=navi33::gfx11_mes_mqd;
namespace mqd=navi33::gfx11_mqd;
namespace kiq=navi33::gfx11_kiq;

static mes::QueueInput goodQueue() {
    mes::QueueInput in{};
    in.q.mqdGpuVa=0x0000000200002000ull;
    in.q.eopGpuVa=0x0000000300000000ull;
    in.q.pqGpuVa=0x0000000400010000ull;
    in.q.rptrGpuVa=0x0000000500000100ull;
    in.q.wptrGpuVa=0x0000000500000108ull;
    in.q.queueBytes=0x1000u;
    in.q.useDoorbell=true;
    in.q.doorbellIndex=0x40u;
    return in;
}

static void goldenImage() {
    const mm::Result r=mm::buildMesKiqMqd(goodQueue());
    assert(r.code==mes::Code::PlanOnly && r.refusalMask==0u);
    const uint32_t *d=r.image.dw;
    assert(d[0]==0xC0310800u);
    assert(d[mqd::kPipelinestatEnable]==1u);
    assert(d[mqd::kStaticThreadMgmtSe0]==0xffffffffu);
    assert(d[mqd::kMiscReserved]==7u);
    assert(d[mqd::kCpHqdEopBaseLo]==0x03000000u);     // eop >> 8
    assert(d[mqd::kCpHqdEopBaseHi]==0u);
    assert(d[mqd::kCpHqdEopControl]==0x00000008u);    // order(512)-1 = 8
    assert(d[mqd::kCpMqdBaseAddrLo]==0x00002000u);
    assert(d[mqd::kCpMqdBaseAddrHi]==0x2u);
    assert(d[mqd::kCpMqdControl]==0x00000100u);       // default, VMID 0
    assert(d[mqd::kCpHqdPqBaseLo]==0x04000100u);
    assert(d[mqd::kCpHqdPqRptrReportLo]==0x100u && d[mqd::kCpHqdPqRptrReportHi]==0x5u);
    assert(d[mqd::kCpHqdPqWptrPollLo]==0x108u && d[mqd::kCpHqdPqWptrPollHi]==0x5u);
    assert(d[mqd::kCpHqdPqControl]==0xd8308009u);     // MES word, RPTR_BLOCK 0
    assert(d[mqd::kCpHqdPqDoorbellControl]==0x40000100u);
    assert(d[mqd::kCpHqdPersistentState]==0x0be05501u);
    assert(d[mqd::kCpHqdActive]==1u);
    assert(d[mqd::kCpHqdVmid]==0u);
    assert(d[mqd::kCpHqdIbControl]==0x00300000u);
    assert(d[mm::kCpHqdIqTimer]==0u);
    assert(d[mqd::kCpHqdQuantum]==0u);
    assert(d[mqd::kCpHqdPqRptr]==0u && d[mqd::kCpHqdPqWptrLo]==0u);
    assert(d[mqd::kFenceAddressLo]==0u && d[511]==0u);
}

static void differsFromLegacyOnPurpose() {
    mes::QueueInput in=goodQueue();
    const mm::Result m=mm::buildMesKiqMqd(in);
    const mqd::Result legacy=mqd::buildKiqMqd(in.q);
    assert(legacy.code==kiq::Code::PlanOnly);
    // The legacy KIQ sets a quantum; MES keeps the default 0.
    assert(legacy.image.dw[mqd::kCpHqdQuantum]==0x111u);
    assert(m.image.dw[mqd::kCpHqdQuantum]==0u);
    // MES pq control: NO_UPDATE_RPTR set and RPTR_BLOCK_SIZE 0 (upstream quirk).
    assert(legacy.image.dw[mqd::kCpHqdPqControl]==0xd0308909u);
    assert(m.image.dw[mqd::kCpHqdPqControl]==0xd8308009u);
    // The MQD keeps 0x100 even though the MES register write uses a literal 0.
    const mes::Plan plan=mes::buildKiqQueueRegs(in);
    bool sawLiteralZero=false;
    for(uint8_t i=0u;i<plan.count;++i)
        if(plan.writes[i].dword==kiq::kCpMqdControl && plan.writes[i].value==0u)
            sawLiteralZero=true;
    assert(sawLiteralZero && m.image.dw[mqd::kCpMqdControl]==0x100u);
}

static void refusals() {
    mes::QueueInput in=goodQueue();
    in.q.eopGpuVa+=0x80u;                      // not 256-byte aligned
    mm::Result r=mm::buildMesKiqMqd(in);
    assert(r.code==mes::Code::Refused && (r.refusalMask&mm::BadEopAddress));

    in=goodQueue();
    in.q.queueBytes=0x3000u;                   // G20 refuses it first
    r=mm::buildMesKiqMqd(in);
    assert(r.refusalMask&mm::QueueRefused);
    for(uint32_t dw : r.image.dw) assert(dw==0u);
}

int main() {
    goldenImage();
    differsFromLegacyOnPurpose();
    refusals();
    puts("Navi33 MES KIQ-pipe MQD image: PASS (offline)");
}
