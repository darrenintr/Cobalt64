// SPDX-License-Identifier: MIT
// Pure plan for the first SDMA work on Navi33: Linux's own ring bring-up and
// ring test, on SDMA0 Queue0 in VMID0, with every buffer in VRAM.
//
// Sources (torvalds/linux): sdma_v6_0.c sdma_v6_0_gfx_resume_instance(),
// sdma_v6_0_ring_test_ring(), sdma_v6_0_emit_copy_buffer(),
// sdma_v6_0_ring_emit_fence(), sdma_v6_0_gfx_stop()/enable(false);
// sdma_common.h (L2 cache policy defaults); gc_11_0_0_offset.h and
// gc_11_0_0_sh_mask.h (offsets, fields); sdma_v6_0_0_pkt_open.h (packets).
//
// Why VMID0 and VRAM only: the Linux kernel SDMA ring runs in VMID0, and the
// census (2026-10-19 14:39) shows GCMC_VM_MX_L1_TLB_CNTL in
// SYSTEM_ACCESS_MODE 0, which radeon cikd.h names PA_ONLY. If that holds for
// GC11, VMID0 accesses to framebuffer MC addresses need no page table, no hub
// write and no system-memory DMA. That is a hypothesis this test checks; it is
// not established.
//
// This header has no hardware access. It returns the write list; a gated
// writer would execute it later, only after the user approves that list.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace navi33 { namespace sdma6_test {

constexpr bool kCanAccessHardware=false;
static_assert(!kCanAccessHardware,"the ring-test plan is pure");

// GC BASE_IDX 0 dword offsets unless noted (gc_11_0_0_offset.h).
enum Reg : uint16_t {
    SemWaitFailTimer=0x0022, WatchdogCntl=0x002e, Utcl1Cntl=0x003c,
    Utcl1Page=0x003f, Q0RbCntl=0x0080, Q0RbBase=0x0081, Q0RbBaseHi=0x0082,
    Q0Rptr=0x0083, Q0RptrHi=0x0084, Q0Wptr=0x0085, Q0WptrHi=0x0086,
    Q0RptrAddrHi=0x0088, Q0RptrAddrLo=0x0089, Q0IbCntl=0x008a,
    Q0Doorbell=0x0092, Q0WptrPollAddrHi=0x00b2, Q0WptrPollAddrLo=0x00b3,
    Q0MinorPtrUpdate=0x00b5,
    F32Cntl=0x589a  // GC BASE_IDX 1
};
constexpr uint8_t segmentOf(uint16_t reg) { return reg==F32Cntl ? 1u : 0u; }

// gc_11_0_0_sh_mask.h
constexpr uint32_t kRbEnable=0x1u, kRbSizeMask=0x3eu, kWptrPollEnable=0x100u,
    kF32WptrPollEnable=0x800u, kRptrWritebackEnable=0x1000u,
    kRbPriv=0x00800000u, kRbVmidMask=0x0f000000u;
constexpr uint32_t kIbEnable=0x1u, kDoorbellEnable=0x10000000u;
constexpr uint32_t kHalt=0x1u, kTh1Reset=0x2000u;
constexpr uint32_t kHangCountMask=0xffu;
constexpr uint32_t kRedoDelayMask=0x1fu, kRespModeMask=0x600u;
constexpr uint32_t kLlcNoalloc=0x01000000u;
constexpr uint32_t kL1TlbEnable=0x1u, kSystemAccessModeMask=0x18u;
// sdma_common.h: CACHE_READ_POLICY_L2__DEFAULT = NOA (2),
// CACHE_WRITE_POLICY_L2__DEFAULT = BYPASS (3).
constexpr uint32_t kReadPolicyDefault=2u, kWritePolicyDefault=3u;

// Packets (sdma_v6_0_0_pkt_open.h).
constexpr uint32_t kOpNop=0u, kOpCopy=1u, kOpWrite=2u, kOpFence=5u;
constexpr uint32_t kFenceMtypeUc=3u<<16u;
constexpr uint32_t kTestInit=0xcafedeadu, kTestValue=0xdeadbeefu;
constexpr uint32_t kFenceCookie=0xc0b1606au;

// One 1 MiB window just above the PSP stages' 8 MiB at 240 MiB, inside the
// 256 MiB BAR0, far from the console at VRAM offset 0.
constexpr uint64_t kWindowOffset=248ull<<20u, kWindowBytes=1ull<<20u;
constexpr uint64_t kRingOff=0x0000u, kRptrWbOff=0x1000u, kWptrPollOff=0x1008u,
    kTestOff=0x2000u, kFenceOff=0x3000u, kSourceOff=0x4000u, kTargetOff=0x5000u;
constexpr uint32_t kRingBytes=4096u, kRingDwords=kRingBytes/4u;
constexpr uint32_t kRbSizeLog2=10u;          // order_base_2(1024 dwords)
constexpr uint32_t kAlignDwords=16u;         // sdma_v6_0 align_mask 0xf
constexpr uint32_t kCopyBytes=4096u;
static_assert((1u<<kRbSizeLog2)==kRingDwords,"ring size field");
static_assert(kTargetOff+kCopyBytes<=kWindowBytes,"layout fits the window");

struct Snapshot {        // live reads, all from the census windows
    uint32_t rlcBootload=0u, psp81=0u, sdmaId=0u, sdmaStatus=0u;
    uint32_t gcL1TlbCntl=0u, f32Cntl=0u, utcl1Cntl=0u, utcl1Page=0u,
             watchdog=0u;
    uint32_t q0[7]{};    // RB_CNTL, BASE, BASE_HI, RPTR, RPTR_HI, WPTR, WPTR_HI
    uint32_t ibCntl=0u, doorbell=0u;
    uint32_t fbBase=0u, fbTop=0u; // GCMC_VM_FB_LOCATION_BASE/TOP (16 MiB units)
};

enum Refusal : uint32_t {
    GcNotUp=1u<<0, SosNotAlive=1u<<1, NoSdma=1u<<2, ReadFailed=1u<<3,
    QueueBusy=1u<<4, NotHalted=1u<<5, SdmaNotIdle=1u<<6,
    NotPhysicalMode=1u<<7, BadFbLocation=1u<<8
};

struct Write { uint16_t reg=0u; uint32_t value=0u; bool readback=true; };
constexpr size_t kMaxWrites=32u;

struct Plan {
    uint32_t refusal=0u;
    uint64_t windowMc=0u, ringMc=0u, rptrWbMc=0u, wptrPollMc=0u, testMc=0u,
             fenceMc=0u, sourceMc=0u, targetMc=0u;
    Write setup[kMaxWrites]{}; size_t setupCount=0u;
    uint32_t ringA[kAlignDwords]{};     // WRITE_LINEAR 0xdeadbeef + NOPs
    uint32_t ringB[kAlignDwords]{};     // COPY 4 KiB + FENCE + NOPs
    uint32_t wptrAfterA=0u, wptrAfterB=0u; // bytes, as Linux writes wptr<<2
    Write park[3]{};                    // RB off, IB off, HALT
    bool ok() const { return refusal==0u; }
};

inline bool anyAllOnes(const Snapshot &s) {
    const uint32_t v[]={s.rlcBootload,s.psp81,s.sdmaId,s.sdmaStatus,
        s.gcL1TlbCntl,s.f32Cntl,s.utcl1Cntl,s.utcl1Page,s.watchdog,
        s.q0[0],s.q0[1],s.q0[2],s.q0[3],s.q0[4],s.q0[5],s.q0[6],
        s.ibCntl,s.doorbell,s.fbBase,s.fbTop};
    for(uint32_t x:v) if(x==UINT32_MAX) return true;
    return false;
}

inline uint32_t refusals(const Snapshot &s) {
    uint32_t r=0u;
    if(anyAllOnes(s)) return ReadFailed;
    if(!(s.rlcBootload&0x80000000u)) r|=GcNotUp;   // BOOTLOAD_COMPLETE
    if(!s.psp81) r|=SosNotAlive;
    if(s.sdmaId!=1u) r|=NoSdma;
    if((s.q0[0]&kRbEnable) || s.q0[1] || s.q0[2] || s.q0[3] || s.q0[4] ||
       s.q0[5] || s.q0[6] || (s.ibCntl&kIbEnable) ||
       (s.doorbell&kDoorbellEnable)) r|=QueueBusy;
    if(!(s.f32Cntl&kHalt)) r|=NotHalted;           // nobody else runs SDMA
    if(!(s.sdmaStatus&0x1u)) r|=SdmaNotIdle;       // SDMA0_STATUS_REG.IDLE
    if(!(s.gcL1TlbCntl&kL1TlbEnable) || (s.gcL1TlbCntl&kSystemAccessModeMask))
        r|=NotPhysicalMode;
    const uint64_t fbBytes=(static_cast<uint64_t>(s.fbTop)-s.fbBase+1u)<<24u;
    if(!s.fbBase || s.fbTop<s.fbBase || fbBytes<kWindowOffset+kWindowBytes)
        r|=BadFbLocation;
    return r;
}

inline void put(Write *list,size_t &n,uint16_t reg,uint32_t value,bool rb=true) {
    if(n<kMaxWrites) list[n++]={reg,value,rb};
}

inline Plan build(const Snapshot &s) {
    Plan p{};
    p.refusal=refusals(s);
    if(p.refusal) return p;
    p.windowMc=(static_cast<uint64_t>(s.fbBase)<<24u)+kWindowOffset;
    p.ringMc=p.windowMc+kRingOff;     p.rptrWbMc=p.windowMc+kRptrWbOff;
    p.wptrPollMc=p.windowMc+kWptrPollOff; p.testMc=p.windowMc+kTestOff;
    p.fenceMc=p.windowMc+kFenceOff;   p.sourceMc=p.windowMc+kSourceOff;
    p.targetMc=p.windowMc+kTargetOff;

    // sdma_v6_0_gfx_resume_instance(), restore=false, no doorbell, in order.
    Write *w=p.setup; size_t &n=p.setupCount;
    put(w,n,SemWaitFailTimer,0u,false);
    uint32_t rb=(s.q0[0]&~(kRbSizeMask|kRbVmidMask))|(kRbSizeLog2<<1u)|kRbPriv;
    put(w,n,Q0RbCntl,rb);
    put(w,n,Q0Rptr,0u); put(w,n,Q0RptrHi,0u);
    put(w,n,Q0Wptr,0u); put(w,n,Q0WptrHi,0u);
    put(w,n,Q0WptrPollAddrLo,static_cast<uint32_t>(p.wptrPollMc));
    put(w,n,Q0WptrPollAddrHi,static_cast<uint32_t>(p.wptrPollMc>>32u));
    put(w,n,Q0RptrAddrHi,static_cast<uint32_t>(p.rptrWbMc>>32u));
    put(w,n,Q0RptrAddrLo,static_cast<uint32_t>(p.rptrWbMc)&~3u);
    rb=(rb|kRptrWritebackEnable|kF32WptrPollEnable)&~kWptrPollEnable;
    put(w,n,Q0RbBase,static_cast<uint32_t>(p.ringMc>>8u));
    put(w,n,Q0RbBaseHi,static_cast<uint32_t>(p.ringMc>>40u));
    put(w,n,Q0MinorPtrUpdate,1u);
    put(w,n,Q0Wptr,0u); put(w,n,Q0WptrHi,0u);
    put(w,n,Q0Doorbell,s.doorbell&~kDoorbellEnable);
    put(w,n,Q0MinorPtrUpdate,0u);
    put(w,n,WatchdogCntl,(s.watchdog&~kHangCountMask)|1u); // max(100000/100000,1)
    put(w,n,Utcl1Cntl,(s.utcl1Cntl&~(kRespModeMask|kRedoDelayMask))|(3u<<9u)|9u);
    put(w,n,Utcl1Page,(s.utcl1Page&0xff0fffu)|(kReadPolicyDefault<<12u)|
                      (kWritePolicyDefault<<14u)|kLlcNoalloc);
    put(w,n,F32Cntl,s.f32Cntl&~(kHalt|kTh1Reset));          // unhalt engine
    put(w,n,Q0RbCntl,rb|kRbEnable);                         // enable DMA RB
    put(w,n,Q0IbCntl,s.ibCntl|kIbEnable);                   // enable DMA IBs

    // Ring test (sdma_v6_0_ring_test_ring), padded with NOPs to 16 dwords.
    p.ringA[0]=kOpWrite;  // SUB_OP WRITE_LINEAR = 0
    p.ringA[1]=static_cast<uint32_t>(p.testMc);
    p.ringA[2]=static_cast<uint32_t>(p.testMc>>32u);
    p.ringA[3]=0u;        // DW_3 COUNT(0): one dword
    p.ringA[4]=kTestValue;
    p.wptrAfterA=kAlignDwords*4u;
    // 4 KiB copy (sdma_v6_0_emit_copy_buffer) then fence (emit_fence, UC).
    p.ringB[0]=kOpCopy;   // SUB_OP COPY_LINEAR = 0
    p.ringB[1]=kCopyBytes-1u;
    p.ringB[2]=0u;
    p.ringB[3]=static_cast<uint32_t>(p.sourceMc);
    p.ringB[4]=static_cast<uint32_t>(p.sourceMc>>32u);
    p.ringB[5]=static_cast<uint32_t>(p.targetMc);
    p.ringB[6]=static_cast<uint32_t>(p.targetMc>>32u);
    p.ringB[7]=kOpFence|kFenceMtypeUc;
    p.ringB[8]=static_cast<uint32_t>(p.fenceMc);
    p.ringB[9]=static_cast<uint32_t>(p.fenceMc>>32u);
    p.ringB[10]=kFenceCookie;
    p.wptrAfterB=2u*kAlignDwords*4u;

    // sdma_v6_0_gfx_stop() then sdma_v6_0_enable(false).
    p.park[0]={Q0RbCntl,rb&~kRbEnable,true};
    p.park[1]={Q0IbCntl,s.ibCntl&~kIbEnable,true};
    p.park[2]={F32Cntl,s.f32Cntl|kHalt,true};
    return p;
}

// The only registers a writer for this plan may touch.
inline bool allowed(uint16_t reg) {
    switch(reg) {
    case SemWaitFailTimer: case WatchdogCntl: case Utcl1Cntl: case Utcl1Page:
    case Q0RbCntl: case Q0RbBase: case Q0RbBaseHi: case Q0Rptr: case Q0RptrHi:
    case Q0Wptr: case Q0WptrHi: case Q0RptrAddrHi: case Q0RptrAddrLo:
    case Q0IbCntl: case Q0Doorbell: case Q0WptrPollAddrHi:
    case Q0WptrPollAddrLo: case Q0MinorPtrUpdate: case F32Cntl: return true;
    default: return false;
    }
}

} } // namespace navi33::sdma6_test
