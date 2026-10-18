// SPDX-License-Identifier: MIT
// Offline MQD memory image for the MES KIQ-pipe queue (gc_11_0_0, Navi33).
// Mirrors mes_v11_0_mqd_init() in upstream mes_v11_0.c (linux master, fetched
// 2026-10-18). The register-shared values come from the G20 plan
// (Navi33GfxMes.h), so the image and the register writes cannot drift apart.
// Field dword indices are the v11_compute_mqd ones checked for G19. The
// caller allocates and writes the image; this header does no I/O and is not
// hardware evidence.
#pragma once
#include "Navi33GfxMes.h"
#include "Navi33Gfx11Mqd.h"
#include <string.h>

namespace navi33 { namespace gfx11_mes_mqd {

// v11_structs.h: cp_hqd_iq_timer is dword 150.
constexpr uint32_t kCpHqdIqTimer=150u;
// mes_v11_0.c MES_EOP_SIZE.
constexpr uint32_t kMesEopBytes=2048u;
// gc_11_0_0_default.h values the MES MQD keeps as they are.
constexpr uint32_t kIbControlDefault=0x00300000u;
constexpr uint32_t kIqTimerDefault=0x00000000u;
constexpr uint32_t kQuantumDefault=0x00000000u;

enum Refusal : uint32_t {
    QueueRefused   = 1u<<0,  // the G20 queue inputs failed their own checks
    BadEopAddress  = 1u<<1,  // EOP buffer not 256-byte aligned or too wide
    MissingField   = 1u<<2,  // a G20 value the image expects is absent
};

struct Result {
    gfx11_mes::Code code=gfx11_mes::Code::Refused;
    uint32_t refusalMask=0u;
    gfx11_mqd::Image image{};
};

inline bool copyFromPlan(const gfx11_mes::Plan &plan,const char *name,
                         uint32_t dword,gfx11_mqd::Image &image) {
    for(uint8_t i=0u;i<plan.count;++i) {
        if(strcmp(plan.writes[i].name,name)==0) {
            image.dw[dword]=plan.writes[i].value;
            return true;
        }
    }
    return false;
}

// Builds the MES KIQ-pipe MQD in the order of mes_v11_0_mqd_init(). The image
// starts zeroed (upstream memsets it), so every field not named stays zero.
inline Result buildMesKiqMqd(const gfx11_mes::QueueInput &in) {
    namespace mqd=gfx11_mqd;
    namespace kiq=gfx11_kiq;
    Result r{};
    const gfx11_mes::Plan plan=gfx11_mes::buildKiqQueueRegs(in);
    if(plan.code!=gfx11_mes::Code::PlanOnly) {
        r.refusalMask|=QueueRefused;
        return r;
    }
    const uint64_t eop=in.q.eopGpuVa;
    if(!kiq::aligned(eop,256u) || eop>kiq::kMaxGpuAddress) {
        r.refusalMask|=BadEopAddress;
        return r;
    }
    memset(r.image.dw,0,sizeof(r.image.dw));
    mqd::Image &img=r.image;

    img.dw[0]=mqd::kHeader;
    img.dw[mqd::kPipelinestatEnable]=1u;
    img.dw[mqd::kStaticThreadMgmtSe0]=0xffffffffu;
    img.dw[mqd::kStaticThreadMgmtSe1]=0xffffffffu;
    img.dw[mqd::kStaticThreadMgmtSe2]=0xffffffffu;
    img.dw[mqd::kStaticThreadMgmtSe3]=0xffffffffu;
    img.dw[mqd::kMiscReserved]=7u;

    // EOP: base >> 8 and EOP_SIZE = order(MES_EOP_SIZE/4) - 1 over the default.
    img.dw[mqd::kCpHqdEopBaseLo]=static_cast<uint32_t>(eop>>8u);
    img.dw[mqd::kCpHqdEopBaseHi]=static_cast<uint32_t>(eop>>40u);
    img.dw[mqd::kCpHqdEopControl]=kiq::setField(kiq::kEopControlDefault,
        kiq::kEopSizeMask,0u,kiq::orderBase2(kMesEopBytes/4u)-1u);

    // Pointers reset (the queue starts empty).
    img.dw[mqd::kCpHqdPqRptr]=0u;
    img.dw[mqd::kCpHqdPqWptrLo]=0u;
    img.dw[mqd::kCpHqdPqWptrHi]=0u;

    // The MQD keeps CP_MQD_CONTROL's default with VMID 0; only the register
    // write in mes_v11_0_queue_init_register() uses a literal 0.
    img.dw[mqd::kCpMqdControl]=kiq::setField(kiq::kMqdControlDefault,
        kiq::kMqdVmidMask,0u,0u);

    img.dw[mqd::kCpHqdVmid]=0u;
    img.dw[mqd::kCpHqdIbControl]=kIbControlDefault;
    img.dw[kCpHqdIqTimer]=kIqTimerDefault;
    img.dw[mqd::kCpHqdQuantum]=kQuantumDefault;

    // Values shared with the G20 register plan.
    const struct { const char *name; uint32_t dword; } shared[]={
        {"CP_MQD_BASE_ADDR",mqd::kCpMqdBaseAddrLo},
        {"CP_MQD_BASE_ADDR_HI",mqd::kCpMqdBaseAddrHi},
        {"CP_HQD_PQ_BASE",mqd::kCpHqdPqBaseLo},
        {"CP_HQD_PQ_BASE_HI",mqd::kCpHqdPqBaseHi},
        {"CP_HQD_PQ_RPTR_REPORT_ADDR",mqd::kCpHqdPqRptrReportLo},
        {"CP_HQD_PQ_RPTR_REPORT_ADDR_HI",mqd::kCpHqdPqRptrReportHi},
        {"CP_HQD_PQ_WPTR_POLL_ADDR",mqd::kCpHqdPqWptrPollLo},
        {"CP_HQD_PQ_WPTR_POLL_ADDR_HI",mqd::kCpHqdPqWptrPollHi},
        {"CP_HQD_PQ_CONTROL",mqd::kCpHqdPqControl},
        {"CP_HQD_PQ_DOORBELL_CONTROL",mqd::kCpHqdPqDoorbellControl},
        {"CP_HQD_PERSISTENT_STATE",mqd::kCpHqdPersistentState},
        {"CP_HQD_ACTIVE=1",mqd::kCpHqdActive},
    };
    for(const auto &s : shared) {
        if(!copyFromPlan(plan,s.name,s.dword,img)) {
            r.refusalMask|=MissingField;
            return r;
        }
    }
    r.code=gfx11_mes::Code::PlanOnly;
    return r;
}

} } // namespace navi33::gfx11_mes_mqd
