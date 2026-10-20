// SPDX-License-Identifier: MIT
// G42: one MAP_QUEUES for gfx ME0/pipe0/queue0 on the existing G34 queue.
// Linux gfx_v11_0.c gfx11_kiq_map_queues/gfx_v11_0_gfx_mqd_init,
// include/v11_structs.h and nvd.h, upstream fetched 2026-10-20.
#pragma once
#include "Navi33Gfx11GfxRing.h"
#include "Navi33HardwarePlan.h"
namespace navi33 { namespace gfx_map {
constexpr uint32_t kSchedulerSeg=1u,kSchedulerDw=0x098au; // RLC_CP_SCHEDULERS, gc_11_0_0_offset.h
constexpr uint32_t kMappedDw=0x1e94u;                    // CP_GFX_HQD_MAPPED (read only)
// Raw post-MAP_QUEUES snapshot, gc_11_0_0_offset.h. These are diagnostics only.
constexpr uint32_t kGfxActiveDw=0x1e80u,kGfxVmidDw=0x1e81u,kGfxBaseDw=0x1e86u,
                   kGfxBaseHiDw=0x1e87u,kGfxRptrDw=0x1e88u,kGfxCntlDw=0x1e8fu,
                   kGfxWptrDw=0x1e91u,kGfxWptrHiDw=0x1e92u;
constexpr uint64_t kG34Vram=250ull<<20;
constexpr uint32_t kMqdOff=0x4200u,kMqdDwords=512u;       // struct v11_gfx_mqd
constexpr uint32_t kAppendOff=mec_queue::kRingOff+64u,kEndWptr=32u;
constexpr uint32_t kMapFenceOff=mec_queue::kFenceOff+8u,kMapFence=0x0c0b1642u;
// Linux nbio_v4_3_set_reg_remap() and KFD's MMIO hole. The RX 7600 guest is
// x86_64 with the driver's 4 KiB GPU page contract; other page sizes refuse.
constexpr uint32_t kHdpMemFlushReg=0x012du,kHdpRegFlushReg=0x012eu;
constexpr uint32_t kNbioMemsizeReg=0x00c3u,kNbioBaseIndex=2u;
constexpr uint32_t kRemapHoleBytes=0x80000u,kSupportedPageBytes=4096u;
enum class HdpCode : uint8_t { Pass=0, UnsupportedPage, MissingNbif, UnsupportedNbif,
    MissingHdp, UnsupportedHdp, MissingBase, OutsideBar5 };
struct HdpPlan {
    HdpCode code=HdpCode::MissingNbif;
    uint32_t remapBytes=0;
    uint64_t remapMem=0,remapReg=0,flush=0,memsize=0;
    bool valid() const { return code==HdpCode::Pass; }
};
inline HdpPlan resolveHdp(const IPDiscovery &d,uint64_t bar5Bytes,uint32_t pageBytes) {
    HdpPlan p{};
    if(pageBytes!=kSupportedPageBytes || pageBytes>=kRemapHoleBytes) {
        p.code=HdpCode::UnsupportedPage;return p;
    }
    IPEntry nbif{},hdp{};
    if(!d.find(ipid::NBIF,0u,nbif))return p;
    if(nbif.major!=4u || nbif.minor!=3u || nbif.revision!=1u) {
        p.code=HdpCode::UnsupportedNbif;return p;
    }
    if(!d.find(ipid::HDP,0u,hdp)) {p.code=HdpCode::MissingHdp;return p;}
    if(hdp.major!=6u || hdp.minor!=0u || hdp.revision!=1u) {
        p.code=HdpCode::UnsupportedHdp;return p;
    }
    if(nbif.numBases<=kNbioBaseIndex || !nbif.bases[kNbioBaseIndex] || !hdp.numBases) {
        p.code=HdpCode::MissingBase;return p;
    }
    if(bar5Bytes<4u) {p.code=HdpCode::OutsideBar5;return p;}
    const uint64_t last=(bar5Bytes-4u)/4u,base=nbif.bases[kNbioBaseIndex];
    auto reg=[&](uint32_t dw,uint64_t &out) {
        if(base==UINT64_MAX || base==UINT32_MAX || base>last || dw>last-base)return false;
        out=(base+dw)*4u;return true;
    };
    const uint32_t aperture=kRemapHoleBytes-pageBytes;
    if(uint64_t(aperture)+pageBytes>bar5Bytes || !reg(kHdpMemFlushReg,p.remapMem) ||
       !reg(kHdpRegFlushReg,p.remapReg) || !reg(kNbioMemsizeReg,p.memsize)) {
        p.code=HdpCode::OutsideBar5;return p;
    }
    p.remapBytes=aperture;p.flush=aperture;
    p.code=HdpCode::Pass;return p;
}
static_assert(kMqdOff+4u*kMqdDwords<=gfx_ring::kMemBytes,"MQD inside the GFXR heap block");
// Extra CPU access is limited to these exact G34 words, not its whole pinned window.
constexpr bool stageRange(uint32_t off,uint32_t n,bool write) {
    return (off==kAppendOff && n==16u) || (off==mec_queue::kWptrOff && n==2u) ||
           (off==kMapFenceOff && n==1u) || (!write && off==mec_queue::kFenceOff && n==1u);
}
struct Snapshot {
    uint32_t scheduler=0,active=0,vmid=0,pqLo=0,pqHi=0,pqControl=0,rptr=0,wptrLo=0,wptrHi=0,fence=0,mapped=0;
};
inline bool ready(const Snapshot &s,uint64_t fb) {
    const uint64_t base=(uint64_t(s.pqHi)<<32)|s.pqLo;
    return (s.scheduler&0xffu)==0xa0u && s.active==1u && s.vmid==0u &&
        base==((fb+kG34Vram+mec_queue::kRingOff)>>8) &&
        (s.pqControl&0xc0000000u)==0xc0000000u && s.rptr==16u &&
        s.wptrLo==16u && !s.wptrHi && s.fence==mec_queue::kFenceValue && !s.mapped;
}
inline uint32_t schedulerWord(uint32_t old) { return (old&0xffffff00u)|0xa0u; } // me1 pipe0 q0, HIQ
// All fields not assigned remain zero, as upstream's memset before MQD init.
inline void mqd(uint64_t mc,uint32_t out[kMqdDwords]) {
    for(uint32_t i=0;i<kMqdDwords;++i)out[i]=0;
    const uint64_t addr=mc+kMqdOff,base=mc>>8,rptr=mc+gfx_ring::kRptrOff,wptr=mc+gfx_ring::kWptrOff;
    out[128]=static_cast<uint32_t>(addr);out[129]=static_cast<uint32_t>(addr>>32);
    out[130]=1u;out[134]=0u;out[135]=0x00000a01u;
    out[136]=static_cast<uint32_t>(base);out[137]=static_cast<uint32_t>(base>>32);
    out[139]=static_cast<uint32_t>(rptr)&0xfffffffcu;out[140]=static_cast<uint32_t>(rptr>>32)&0xffffu;
    out[141]=static_cast<uint32_t>(wptr)&0xfffffffcu;out[142]=static_cast<uint32_t>(wptr>>32)&0xffffu;
    out[143]=gfx_ring::kDoorbellEn|(gfx_ring::kDoorbellDword<<2);
    out[145]=0x00a00000u|gfx_ring::kRb0CntlValue; // gfx HQD reset MIN_AVAILSZ/MIN_IB_AVAILSZ
    out[162]=0x100u;                            // MQD PRIV_STATE=1, VMID0
}
inline void packets(uint64_t gfxMc,uint64_t fb,uint32_t out[16]) {
    namespace gd=gfx11_dispatch;
    const uint64_t addr=gfxMc+kMqdOff,wptr=gfxMc+gfx_ring::kWptrOff;
    out[0]=gd::packet3(0xa2u,5u);
    out[1]=(4u<<26)|(1u<<29);                   // gfx engine4, one queue, me/pipe/queue/vmid0
    out[2]=gfx_ring::kDoorbellDword<<2;
    out[3]=static_cast<uint32_t>(addr);out[4]=static_cast<uint32_t>(addr>>32);
    out[5]=static_cast<uint32_t>(wptr);out[6]=static_cast<uint32_t>(wptr>>32);
    // gfx_v11_0_ring_emit_fence_kiq: confirmed WRITE_DATA to memory, no IRQ.
    const uint64_t fence=fb+kG34Vram+kMapFenceOff;
    out[7]=gd::packet3(0x37u,3u);out[8]=(5u<<8)|(1u<<20);
    out[9]=static_cast<uint32_t>(fence);out[10]=static_cast<uint32_t>(fence>>32);out[11]=kMapFence;
    for(uint32_t i=12;i<16;++i)out[i]=mec_queue::kNop;
}
enum class State : uint32_t { Off=0,Refused,Attempted,MapTimeout,Mapped,HdpFailed };
enum class HdpState : uint32_t { NotTried=0,PlanUnavailable,PreflightFailed,UnexpectedRemap,
    RemapWriteFailed,RemapReadbackFailed,AlreadyConfigured,Remapped,FlushWriteFailed,
    FlushReadbackFailed,Complete };
struct HdpReport {
    HdpState state=HdpState::NotTried;
    HdpCode plan=HdpCode::MissingNbif;
    uint32_t remapBytes=0,remapMemBefore=0,remapRegBefore=0,
             remapMemAfter=0,remapRegAfter=0,memsize=0,writes=0;
};
struct GfxQueueSnapshot {
    bool valid=false;
    uint32_t active=0,vmid=0,baseLo=0,baseHi=0,rptr=0,cntl=0,wptrLo=0,wptrHi=0;
};
struct Report {
    State state=State::Off; Snapshot before{}; HdpReport hdp{};
    GfxQueueSnapshot gfxAfter{};
    uint32_t fence=0,mapped=0,kiqRptr=0;
};
} }
