// Host-only checks for the GFX11 KIQ MQD image (no hardware, no memory I/O).
// Expected dwords are derived by hand from v11_compute_mqd offsets and the
// gfx_v11_0_compute_mqd_init() formulas, not read back from the builder.
#include "../src/Navi33Gfx11Mqd.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

namespace kiq=navi33::gfx11_kiq;
namespace mqd=navi33::gfx11_mqd;

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

static void goldenImage() {
    const mqd::Result r=mqd::buildKiqMqd(goodQueue());
    assert(r.code==kiq::Code::PlanOnly);
    assert(r.refusalMask==0u);
    const uint32_t *d=r.image.dw;
    static_assert(sizeof(r.image.dw)==2048u, "MQD is 2 KiB");

    assert(d[0]==0xC0310800u);                // header
    assert(d[mqd::kPipelinestatEnable]==1u);
    assert(d[mqd::kStaticThreadMgmtSe0]==0xffffffffu);
    assert(d[mqd::kStaticThreadMgmtSe3]==0xffffffffu);
    assert(d[mqd::kMiscReserved]==7u);
    assert(d[mqd::kCpHqdQuantum]==0x111u);
    assert(d[mqd::kCpHqdIbControl]==0x00300000u);

    // Queue addresses, using the same shifts as the register plan.
    assert(d[mqd::kCpMqdBaseAddrLo]==0x00002000u);
    assert(d[mqd::kCpMqdBaseAddrHi]==0x2u);
    assert(d[mqd::kCpHqdPqBaseLo]==0x04000100u);      // pq >> 8
    assert(d[mqd::kCpHqdPqBaseHi]==0u);
    assert(d[mqd::kCpHqdPqRptrReportLo]==0x100u);
    assert(d[mqd::kCpHqdPqRptrReportHi]==0x5u);
    assert(d[mqd::kCpHqdPqWptrPollLo]==0x108u);
    assert(d[mqd::kCpHqdPqWptrPollHi]==0x5u);
    assert(d[mqd::kCpHqdEopBaseLo]==0x03000000u);     // eop >> 8
    assert(d[mqd::kCpHqdEopBaseHi]==0u);

    // Queue control words, same values as the KIQ register plan.
    assert(d[mqd::kCpHqdEopControl]==0x00000008u);
    assert(d[mqd::kCpHqdPqDoorbellControl]==0x40000100u);
    assert(d[mqd::kCpHqdPqControl]==0xd0308909u);
    assert(d[mqd::kCpMqdControl]==0x00000100u);
    assert(d[mqd::kCpHqdPersistentState]==0x0be05501u);
    assert(d[mqd::kCpHqdVmid]==0u);
    assert(d[mqd::kCpHqdActive]==1u);                 // KIQ is active at init

    // Pointers reset and the fence left at zero (KIQ prop has none).
    assert(d[mqd::kCpHqdPqRptr]==0u);
    assert(d[mqd::kCpHqdPqWptrLo]==0u);
    assert(d[mqd::kCpHqdPqWptrHi]==0u);
    assert(d[mqd::kCpHqdDequeueRequest]==0u);
    assert(d[mqd::kFenceAddressLo]==0u);
    assert(d[mqd::kFenceAddressHi]==0u);

    // A field the init path never sets stays zero.
    assert(d[300]==0u);
    assert(d[511]==0u);
}

static void imageMatchesRegisterPlan() {
    const kiq::KiqQueue q=goodQueue();
    const kiq::Plan plan=kiq::buildKiqInit(q);
    const mqd::Result r=mqd::buildKiqMqd(q);
    assert(r.code==kiq::Code::PlanOnly);
    // The shared values must be identical: each MQD field is the value the
    // driver writes to the matching HQD register.
    const struct { const char *reg; uint32_t dword; } pairs[]={
        {"CP_HQD_EOP_CONTROL",mqd::kCpHqdEopControl},
        {"CP_HQD_PQ_CONTROL",mqd::kCpHqdPqControl},
        {"CP_HQD_PQ_DOORBELL_CONTROL",mqd::kCpHqdPqDoorbellControl},
        {"CP_HQD_PERSISTENT_STATE",mqd::kCpHqdPersistentState},
        {"CP_MQD_CONTROL",mqd::kCpMqdControl},
        {"CP_HQD_ACTIVE (KIQ)",mqd::kCpHqdActive},
    };
    for(const auto &p : pairs) {
        bool found=false;
        for(uint8_t i=0u;i<plan.count;++i)
            if(strcmp(plan.writes[i].name,p.reg)==0) {
                assert(plan.writes[i].value==r.image.dw[p.dword]);
                found=true;
            }
        assert(found);
    }
}

static void refusalPropagates() {
    kiq::KiqQueue q=goodQueue();
    q.hqdActiveAtStart=true;
    const mqd::Result r=mqd::buildKiqMqd(q);
    assert(r.code==kiq::Code::Refused);
    assert(r.refusalMask&kiq::DequeueRequired);
    for(uint32_t dw : r.image.dw) assert(dw==0u);

    q=goodQueue();
    q.queueBytes=0x3000u;
    assert(mqd::buildKiqMqd(q).refusalMask&kiq::BadQueueSize);
}

int main() {
    goldenImage();
    imageMatchesRegisterPlan();
    refusalPropagates();
    puts("Navi33 GFX11 KIQ MQD image: PASS (offline)");
}
