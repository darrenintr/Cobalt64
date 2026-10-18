// SPDX-License-Identifier: MIT
// Offline GFX11 compute MQD (memory queue descriptor) image for the KIQ.
// Mirrors the memory fields of gfx_v11_0_compute_mqd_init() on top of the
// register values from Navi33Gfx11KiqHqd.h, so the register writes and the
// image cannot drift apart. Field dword indices come from struct
// v11_compute_mqd in upstream v11_structs.h (linux master, fetched 2026-10-18).
// The caller allocates and writes the image; this header does no I/O and is
// not hardware evidence.
#pragma once
#include "Navi33Gfx11KiqHqd.h"
#include <string.h>

namespace navi33 { namespace gfx11_mqd {

constexpr size_t kDwords=512u;            // struct v11_compute_mqd, 2 KiB
constexpr uint32_t kHeader=0xC0310800u;   // mqd->header

// v11_structs.h dword offsets for the fields this image sets.
constexpr uint32_t kPipelinestatEnable=11u;
constexpr uint32_t kStaticThreadMgmtSe0=23u;
constexpr uint32_t kStaticThreadMgmtSe1=24u;
constexpr uint32_t kStaticThreadMgmtSe2=26u;
constexpr uint32_t kStaticThreadMgmtSe3=27u;
constexpr uint32_t kMiscReserved=32u;
constexpr uint32_t kCpMqdBaseAddrLo=128u;
constexpr uint32_t kCpMqdBaseAddrHi=129u;
constexpr uint32_t kCpHqdActive=130u;
constexpr uint32_t kCpHqdVmid=131u;
constexpr uint32_t kCpHqdPersistentState=132u;
constexpr uint32_t kCpHqdPipePriority=133u;
constexpr uint32_t kCpHqdQueuePriority=134u;
constexpr uint32_t kCpHqdQuantum=135u;
constexpr uint32_t kCpHqdPqBaseLo=136u;
constexpr uint32_t kCpHqdPqBaseHi=137u;
constexpr uint32_t kCpHqdPqRptr=138u;
constexpr uint32_t kCpHqdPqRptrReportLo=139u;
constexpr uint32_t kCpHqdPqRptrReportHi=140u;
constexpr uint32_t kCpHqdPqWptrPollLo=141u;
constexpr uint32_t kCpHqdPqWptrPollHi=142u;
constexpr uint32_t kCpHqdPqDoorbellControl=143u;
constexpr uint32_t kCpHqdPqControl=145u;
constexpr uint32_t kCpHqdIbControl=149u;
constexpr uint32_t kCpHqdDequeueRequest=152u;
constexpr uint32_t kCpMqdControl=162u;
constexpr uint32_t kCpHqdEopBaseLo=165u;
constexpr uint32_t kCpHqdEopBaseHi=166u;
constexpr uint32_t kCpHqdEopControl=167u;
constexpr uint32_t kCpHqdPqWptrLo=182u;
constexpr uint32_t kCpHqdPqWptrHi=183u;
constexpr uint32_t kFenceAddressLo=446u;
constexpr uint32_t kFenceAddressHi=447u;

// A register-plan value the image expects is missing. This is a planner bug,
// so it is reported as a refusal rather than an empty image.
constexpr uint32_t kMissingField=1u<<0;

// gc_11_0_0_default.h CP_HQD_IB_CONTROL_DEFAULT is 0x00300000, which already
// holds MIN_IB_AVAIL_SIZE = 3 (bits 20..21).
constexpr uint32_t kIbControl=0x00300000u;
// CP_HQD_QUANTUM: QUANTUM_EN (bit 0) | QUANTUM_SCALE (bit 4) |
// QUANTUM_DURATION = 1 (bits 8..13). The default 0 is overwritten.
constexpr uint32_t kQuantum=(1u<<0)|(1u<<4)|(1u<<8);

struct Image {
    uint32_t dw[kDwords];
};

struct Result {
    gfx11_kiq::Code code=gfx11_kiq::Code::Refused;
    uint32_t refusalMask=0u;
    Image image{};
};

// Copies the value of the named register write into an MQD dword. The
// register-only writes (poll control, pq status, doorbell ranges) have no
// MQD field and are not mapped.
inline bool copyFromPlan(const gfx11_kiq::Plan &plan,const char *name,
                         uint32_t mqdDword,Image &image) {
    for(uint8_t i=0u;i<plan.count;++i) {
        if(strcmp(plan.writes[i].name,name)==0) {
            image.dw[mqdDword]=plan.writes[i].value;
            return true;
        }
    }
    return false;
}

// Builds the KIQ MQD image in the order of gfx_v11_0_compute_mqd_init(). The
// caller memsets the MQD to zero first (gfx_v11_0_kiq_init_queue does), so
// every field not named here stays zero. The fence address is zero: the KIQ
// prop from amdgpu_ring_init_mqd() carries none.
inline Result buildKiqMqd(const gfx11_kiq::KiqQueue &q) {
    Result r{};
    const gfx11_kiq::Plan plan=gfx11_kiq::buildKiqInit(q);
    if(plan.code!=gfx11_kiq::Code::PlanOnly) {
        r.refusalMask=plan.refusalMask;
        return r;
    }
    memset(r.image.dw,0,sizeof(r.image.dw));

    Image &img=r.image;
    img.dw[0]=kHeader;
    img.dw[kPipelinestatEnable]=1u;
    img.dw[kStaticThreadMgmtSe0]=0xffffffffu;
    img.dw[kStaticThreadMgmtSe1]=0xffffffffu;
    img.dw[kStaticThreadMgmtSe2]=0xffffffffu;
    img.dw[kStaticThreadMgmtSe3]=0xffffffffu;
    img.dw[kMiscReserved]=7u;
    img.dw[kCpHqdPipePriority]=0u;      // prop->hqd_pipe_priority (default)
    img.dw[kCpHqdQueuePriority]=0u;     // prop->hqd_queue_priority (default)
    img.dw[kCpHqdQuantum]=kQuantum;
    img.dw[kCpHqdIbControl]=kIbControl;
    img.dw[kCpHqdDequeueRequest]=0u;
    img.dw[kCpHqdPqRptr]=0u;
    img.dw[kFenceAddressLo]=0u;
    img.dw[kFenceAddressHi]=0u;

    // Register values shared with the KIQ register plan.
    const struct { const char *name; uint32_t dword; } shared[]={
        {"CP_MQD_BASE_ADDR",kCpMqdBaseAddrLo},
        {"CP_MQD_BASE_ADDR_HI",kCpMqdBaseAddrHi},
        {"CP_HQD_ACTIVE (KIQ)",kCpHqdActive},
        {"CP_HQD_VMID",kCpHqdVmid},
        {"CP_HQD_PERSISTENT_STATE",kCpHqdPersistentState},
        {"CP_HQD_PQ_BASE",kCpHqdPqBaseLo},
        {"CP_HQD_PQ_BASE_HI",kCpHqdPqBaseHi},
        {"CP_HQD_PQ_RPTR_REPORT_ADDR",kCpHqdPqRptrReportLo},
        {"CP_HQD_PQ_RPTR_REPORT_ADDR_HI",kCpHqdPqRptrReportHi},
        {"CP_HQD_PQ_WPTR_POLL_ADDR",kCpHqdPqWptrPollLo},
        {"CP_HQD_PQ_WPTR_POLL_ADDR_HI",kCpHqdPqWptrPollHi},
        {"CP_HQD_PQ_DOORBELL_CONTROL",kCpHqdPqDoorbellControl},
        {"CP_HQD_PQ_CONTROL",kCpHqdPqControl},
        {"CP_MQD_CONTROL",kCpMqdControl},
        {"CP_HQD_EOP_BASE_ADDR",kCpHqdEopBaseLo},
        {"CP_HQD_EOP_BASE_ADDR_HI",kCpHqdEopBaseHi},
        {"CP_HQD_EOP_CONTROL",kCpHqdEopControl},
        {"CP_HQD_PQ_WPTR_LO",kCpHqdPqWptrLo},
        {"CP_HQD_PQ_WPTR_HI",kCpHqdPqWptrHi},
    };
    for(const auto &s : shared) {
        if(!copyFromPlan(plan,s.name,s.dword,img)) {
            r.code=gfx11_kiq::Code::Refused;
            r.refusalMask|=kMissingField;
            return r;
        }
    }
    r.code=gfx11_kiq::Code::PlanOnly;
    return r;
}

} } // namespace navi33::gfx11_mqd
