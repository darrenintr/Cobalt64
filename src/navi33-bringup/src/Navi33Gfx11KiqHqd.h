// SPDX-License-Identifier: MIT
// Offline GFX11 KIQ (kernel interface queue) HQD register plan for gfx1102.
// Mirrors gfx_v11_0_kiq_init_register() and the mqd fields that
// gfx_v11_0_compute_mqd_init() derives for it, for gc_11_0_0 (Navi33).
// The result is an ordered list of register writes the caller may later
// perform. This header performs no register or memory I/O and is not
// hardware evidence. The MQD memory image and the dequeue path are separate.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace navi33 { namespace gfx11_kiq {

// gc_11_0_0_offset.h dword offsets, all GC segment 0 unless noted.
constexpr uint32_t kCpMqdBaseAddr=0x1fa9u;
constexpr uint32_t kCpMqdBaseAddrHi=0x1faau;
constexpr uint32_t kCpHqdActive=0x1fabu;
constexpr uint32_t kCpHqdVmid=0x1facu;
constexpr uint32_t kCpHqdPersistentState=0x1fadu;
constexpr uint32_t kCpHqdPqBase=0x1fb1u;
constexpr uint32_t kCpHqdPqBaseHi=0x1fb2u;
constexpr uint32_t kCpHqdPqRptr=0x1fb3u;
constexpr uint32_t kCpHqdPqRptrReportAddr=0x1fb4u;
constexpr uint32_t kCpHqdPqRptrReportAddrHi=0x1fb5u;
constexpr uint32_t kCpHqdPqWptrPollAddr=0x1fb6u;
constexpr uint32_t kCpHqdPqWptrPollAddrHi=0x1fb7u;
constexpr uint32_t kCpHqdPqDoorbellControl=0x1fb8u;
constexpr uint32_t kCpHqdPqControl=0x1fbau;
constexpr uint32_t kCpMqdControl=0x1fcbu;
constexpr uint32_t kCpHqdEopBaseAddr=0x1fceu;
constexpr uint32_t kCpHqdEopBaseAddrHi=0x1fcfu;
constexpr uint32_t kCpHqdEopControl=0x1fd0u;
constexpr uint32_t kCpHqdPqWptrLo=0x1fdfu;
constexpr uint32_t kCpHqdPqWptrHi=0x1fe0u;
constexpr uint32_t kCpMecDoorbellRangeLower=0x1dfcu;
constexpr uint32_t kCpMecDoorbellRangeUpper=0x1dfdu;
constexpr uint32_t kCpPqWptrPollCntl=0x1e23u;
constexpr uint32_t kCpPqStatus=0x1e58u;

// gc_11_0_0_default.h reset values the driver starts from.
constexpr uint32_t kEopControlDefault=0x00000006u;
constexpr uint32_t kPqDoorbellControlDefault=0x00000000u;
constexpr uint32_t kMqdControlDefault=0x00000100u;
constexpr uint32_t kPqControlDefault=0x00308509u;
constexpr uint32_t kPersistentStateDefault=0x0be05501u;

// gc_11_0_0_sh_mask.h field positions and masks.
constexpr uint32_t kEopSizeMask=0x0000003fu;
constexpr uint32_t kDoorbellOffsetMask=0x0ffffffcu, kDoorbellOffsetShift=2u;
constexpr uint32_t kDoorbellSourceMask=0x10000000u;
constexpr uint32_t kDoorbellEnMask=0x40000000u;
constexpr uint32_t kDoorbellHitMask=0x80000000u;
constexpr uint32_t kPqQueueSizeMask=0x0000003fu;
constexpr uint32_t kPqRptrBlockSizeMask=0x00003f00u, kPqRptrBlockSizeShift=8u;
constexpr uint32_t kPqUnordDispatchMask=0x10000000u;
constexpr uint32_t kPqTunnelDispatchMask=0x20000000u;
constexpr uint32_t kPqPrivStateMask=0x40000000u;
constexpr uint32_t kPqKmdQueueMask=0x80000000u;
constexpr uint32_t kMqdVmidMask=0x0000000fu;
constexpr uint32_t kPersistentPreloadMask=0x0003ff00u, kPersistentPreloadShift=8u;
constexpr uint32_t kPollEnMask=0x80000000u;
constexpr uint32_t kPqDoorbellEnableMask=0x00000002u, kPqDoorbellEnableShift=1u;
constexpr uint32_t kPersistentPreloadSize=0x55u;

// The GFX11 driver's MEC EOP buffer size (GFX11_MEC_HPD_SIZE in gfx_v11_0.c).
constexpr uint32_t kMecHpdSize=2048u;
constexpr uint32_t kGpuPageBytes=4096u;
constexpr uint32_t kMaxQueueBytes=1u<<20;
constexpr uint32_t kMaxDoorbellIndex=(1u<<26)-1u;
constexpr uint64_t kMaxGpuAddress=(1ull<<48)-1ull;

enum class Code : uint8_t { Refused=0, PlanOnly };
enum Refusal : uint32_t {
    BadQueueSize    = 1u<<0,
    BadEopSize      = 1u<<1,
    BadAlignment    = 1u<<2,
    AddressTooWide  = 1u<<3,
    BadDoorbell     = 1u<<4,
    DequeueRequired = 1u<<5,
};

// Each input is a value the caller has already read or chosen. Nothing here
// is discovered from hardware. For the KIQ, hqd_active is 1 (upstream
// amdgpu_ring.c: prop->hqd_active is set only for AMDGPU_RING_TYPE_KIQ) and
// kernel_queue is always true.
struct KiqQueue {
    uint64_t mqdGpuVa=0u;          // MQD memory (written separately)
    uint64_t eopGpuVa=0u;          // 256-byte aligned
    uint64_t pqGpuVa=0u;           // ring base, 256-byte aligned
    uint64_t rptrGpuVa=0u;         // rptr write-back, 4-byte aligned
    uint64_t wptrGpuVa=0u;         // wptr poll address, 4-byte aligned
    uint32_t queueBytes=0u;        // power of two, 4 KiB .. 1 MiB
    uint32_t eopBytes=kMecHpdSize; // power of two, 256 B .. 64 KiB
    bool useDoorbell=true;
    uint32_t doorbellIndex=0u;     // this queue's doorbell (KIQ)
    uint32_t doorbellRangeLower=0u;  // adev->doorbell_index.kiq
    uint32_t doorbellRangeUpper=0u;  // adev->doorbell_index.userqueue_end
    bool tmzQueue=false;
    bool allowTunneling=false;     // upstream: prop->allow_tunneling
    bool hqdActiveAtStart=false;   // CP_HQD_ACTIVE & 1 before init
    uint32_t pollCntlCurrent=0u;   // CP_PQ_WPTR_POLL_CNTL, read before RMW
    uint32_t pqStatusCurrent=0u;   // CP_PQ_STATUS, read before RMW
};

struct Write {
    uint32_t dword;
    uint32_t value;
    const char *name;
};

constexpr size_t kMaxWrites=32;
struct Plan {
    Code code=Code::Refused;
    uint32_t refusalMask=0u;
    uint8_t count=0u;
    Write writes[kMaxWrites]{};
};

constexpr uint32_t setField(uint32_t reg,uint32_t mask,uint32_t shift,
                            uint32_t value) {
    return (reg&~mask)|((value<<shift)&mask);
}

constexpr uint32_t orderBase2(uint32_t n) {
    uint32_t order=0u;
    while((1u<<order)<n) ++order;
    return order;
}

constexpr bool isPow2(uint32_t n) { return n!=0u && (n&(n-1u))==0u; }

constexpr bool aligned(uint64_t v,uint64_t align) { return (v&(align-1u))==0u; }

inline void push(Plan &p,uint32_t dword,uint32_t value,const char *name) {
    if(p.count<kMaxWrites)
        p.writes[p.count++]={dword,value,name};
}

// Builds the KIQ init write list in upstream order. Every input is checked
// first; any refusal returns an empty plan. The dequeue path (taken only when
// the HQD is already active) is refused, not modelled.
inline Plan buildKiqInit(const KiqQueue &q) {
    Plan p{};
    uint32_t refused=0u;
    if(!isPow2(q.queueBytes) || q.queueBytes<kGpuPageBytes ||
       q.queueBytes>kMaxQueueBytes)
        refused|=BadQueueSize;
    if(!isPow2(q.eopBytes) || q.eopBytes<256u || q.eopBytes>65536u ||
       q.eopBytes/4u<2u)
        refused|=BadEopSize;
    if(!aligned(q.mqdGpuVa,4u) || !aligned(q.eopGpuVa,256u) ||
       !aligned(q.pqGpuVa,256u) || !aligned(q.rptrGpuVa,4u) ||
       !aligned(q.wptrGpuVa,4u))
        refused|=BadAlignment;
    if(q.mqdGpuVa>kMaxGpuAddress || q.eopGpuVa>kMaxGpuAddress ||
       q.pqGpuVa>kMaxGpuAddress || q.rptrGpuVa>kMaxGpuAddress ||
       q.wptrGpuVa>kMaxGpuAddress)
        refused|=AddressTooWide;
    if(q.useDoorbell && (q.doorbellIndex>kMaxDoorbellIndex ||
                         q.doorbellRangeLower>q.doorbellRangeUpper))
        refused|=BadDoorbell;
    if(q.hqdActiveAtStart)
        refused|=DequeueRequired;
    if(refused) {
        p.refusalMask=refused;
        return p;
    }

    // mqd->cp_hqd_eop_control: EOP_SIZE = order(eop/4) - 1.
    const uint32_t eopControl=setField(kEopControlDefault,kEopSizeMask,0u,
        orderBase2(q.eopBytes/4u)-1u);

    // mqd->cp_hqd_pq_doorbell_control.
    uint32_t doorbell=kPqDoorbellControlDefault;
    if(q.useDoorbell) {
        doorbell=setField(doorbell,kDoorbellOffsetMask,kDoorbellOffsetShift,
                          q.doorbellIndex);
        doorbell=setField(doorbell,kDoorbellSourceMask,28u,0u);
        doorbell=setField(doorbell,kDoorbellEnMask,30u,1u);
        doorbell=setField(doorbell,kDoorbellHitMask,31u,0u);
    } else {
        doorbell=setField(doorbell,kDoorbellEnMask,30u,0u);
    }

    // mqd->cp_mqd_control: VMID 0.
    const uint32_t mqdControl=setField(kMqdControlDefault,kMqdVmidMask,0u,0u);

    // mqd->cp_hqd_pq_control.
    uint32_t pq=kPqControlDefault;
    pq=setField(pq,kPqQueueSizeMask,0u,orderBase2(q.queueBytes/4u)-1u);
    pq=setField(pq,kPqRptrBlockSizeMask,kPqRptrBlockSizeShift,
                orderBase2(kGpuPageBytes/4u)-1u);
    pq=setField(pq,kPqUnordDispatchMask,28u,1u);
    pq=setField(pq,kPqTunnelDispatchMask,29u,q.allowTunneling ? 1u : 0u);
    pq=setField(pq,kPqPrivStateMask,30u,1u);   // kernel_queue
    pq=setField(pq,kPqKmdQueueMask,31u,1u);    // kernel_queue
    pq=setField(pq,0x00400000u,22u,q.tmzQueue ? 1u : 0u); // TMZ

    // mqd->cp_hqd_persistent_state: PRELOAD_SIZE 0x55.
    const uint32_t persistent=setField(kPersistentStateDefault,
        kPersistentPreloadMask,kPersistentPreloadShift,kPersistentPreloadSize);

    // gfx_v11_0_kiq_init_register(), in its order.
    const uint32_t pollCntl=q.pollCntlCurrent&~kPollEnMask;   // EN = 0
    push(p,kCpPqWptrPollCntl,pollCntl,"CP_PQ_WPTR_POLL_CNTL.EN=0");
    push(p,kCpHqdEopBaseAddr,
         static_cast<uint32_t>(q.eopGpuVa>>8u),"CP_HQD_EOP_BASE_ADDR");
    push(p,kCpHqdEopBaseAddrHi,
         static_cast<uint32_t>(q.eopGpuVa>>40u),"CP_HQD_EOP_BASE_ADDR_HI");
    push(p,kCpHqdEopControl,eopControl,"CP_HQD_EOP_CONTROL");
    push(p,kCpHqdPqDoorbellControl,doorbell,"CP_HQD_PQ_DOORBELL_CONTROL");
    push(p,kCpMqdBaseAddr,
         static_cast<uint32_t>(q.mqdGpuVa&0xfffffffcu),"CP_MQD_BASE_ADDR");
    push(p,kCpMqdBaseAddrHi,
         static_cast<uint32_t>(q.mqdGpuVa>>32u),"CP_MQD_BASE_ADDR_HI");
    push(p,kCpMqdControl,mqdControl,"CP_MQD_CONTROL");
    push(p,kCpHqdPqBase,static_cast<uint32_t>(q.pqGpuVa>>8u),"CP_HQD_PQ_BASE");
    push(p,kCpHqdPqBaseHi,
         static_cast<uint32_t>(q.pqGpuVa>>40u),"CP_HQD_PQ_BASE_HI");
    push(p,kCpHqdPqControl,pq,"CP_HQD_PQ_CONTROL");
    push(p,kCpHqdPqRptrReportAddr,
         static_cast<uint32_t>(q.rptrGpuVa&0xfffffffcu),
         "CP_HQD_PQ_RPTR_REPORT_ADDR");
    push(p,kCpHqdPqRptrReportAddrHi,
         static_cast<uint32_t>((q.rptrGpuVa>>32u)&0xffffu),
         "CP_HQD_PQ_RPTR_REPORT_ADDR_HI");
    push(p,kCpHqdPqWptrPollAddr,
         static_cast<uint32_t>(q.wptrGpuVa&0xfffffffcu),
         "CP_HQD_PQ_WPTR_POLL_ADDR");
    push(p,kCpHqdPqWptrPollAddrHi,
         static_cast<uint32_t>((q.wptrGpuVa>>32u)&0xffffu),
         "CP_HQD_PQ_WPTR_POLL_ADDR_HI");
    if(q.useDoorbell) {
        push(p,kCpMecDoorbellRangeLower,(q.doorbellRangeLower*2u)<<2u,
             "CP_MEC_DOORBELL_RANGE_LOWER");
        push(p,kCpMecDoorbellRangeUpper,(q.doorbellRangeUpper*2u)<<2u,
             "CP_MEC_DOORBELL_RANGE_UPPER");
    }
    push(p,kCpHqdPqDoorbellControl,doorbell,"CP_HQD_PQ_DOORBELL_CONTROL");
    push(p,kCpHqdPqWptrLo,0u,"CP_HQD_PQ_WPTR_LO");
    push(p,kCpHqdPqWptrHi,0u,"CP_HQD_PQ_WPTR_HI");
    push(p,kCpHqdVmid,0u,"CP_HQD_VMID");
    push(p,kCpHqdPersistentState,persistent,"CP_HQD_PERSISTENT_STATE");
    push(p,kCpHqdActive,1u,"CP_HQD_ACTIVE (KIQ)");
    if(q.useDoorbell) {
        const uint32_t pqStatus=setField(q.pqStatusCurrent,
            kPqDoorbellEnableMask,kPqDoorbellEnableShift,1u);
        push(p,kCpPqStatus,pqStatus,"CP_PQ_STATUS.DOORBELL_ENABLE=1");
    }

    p.code=Code::PlanOnly;
    return p;
}

} } // namespace navi33::gfx11_kiq
