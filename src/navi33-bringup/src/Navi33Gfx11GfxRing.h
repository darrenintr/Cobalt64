// SPDX-License-Identifier: MIT
// The graphics pipe of GC 11.0.2 (G40): the kernel's gfx ring 0, the queue Metal's
// rendering needs. Upstream sources, torvalds/linux master fetched 2026-10-20, PSP-load
// path of gfx_v11_0.c:
//  - gfx_v11_0_config_gfx_rs64(): PFP and ME program counters per gfx pipe (0, 1), each
//    pipe pair reset then released (the MEC half is G34's and is not repeated).
//  - gfx_v11_0_cp_gfx_resume(): RB0 of pipe 0 in VMID 0 (RB_BUFSZ, RB_BLKSZ, KMD_QUEUE),
//    WPTR 0, RPTR report and WPTR poll addresses, BASE, RB_ACTIVE. The ring is kicked by
//    writing CP_RB0_WPTR (gfx_v11_0_ring_set_wptr_gfx without a doorbell), so
//    CP_RB_DOORBELL_CONTROL.DOORBELL_EN is cleared; pipe 1 (RB1) is left alone.
//  - gfx_v11_0_cp_gfx_start(): CP_MAX_CONTEXT = max_hw_contexts - 1 (8 - 1),
//    CP_DEVICE_ID 1, gfx_v11_0_cp_gfx_enable(true) (PFP/ME unhalted, CP_STAT waited for 0),
//    then the clear-state preamble (Navi33Gfx11ClearState.h) and CLEAR_STATE.
//  - gfx_v11_0_ring_emit_fence(): RELEASE_MEM, the ring test's proof.
// No hardware I/O except callbacks; the caller owns the ring memory (VMID 0, MC addresses).
#pragma once
#include "Navi33Gfx11ClearState.h"
#include "Navi33Gfx11Dispatch.h"
#include "Navi33Gfx11MecQueue.h"
#include "Navi33HardwarePlan.h"
namespace navi33 { namespace gfx_ring {
// gc_11_0_0_offset.h (segment, dword).
struct Reg { uint8_t seg; uint32_t dw; };
constexpr Reg kMeCntl{1,0x0803u}, kPfpStart{0,0x1e44u}, kPfpStartHi{0,0x1e59u}, kMeStart{0,0x1e45u},
              kMeStartHi{0,0x1e79u}, kCpStat{0,0x0f40u}, kMaxContext{0,0x1e4eu}, kDeviceId{0,0x1debu},
              kWptrDelay{0,0x0f61u}, kRbVmid{0,0x1df1u}, kRb0Cntl{0,0x1de1u}, kRb0Wptr{0,0x1df4u},
              kRb0WptrHi{0,0x1df5u}, kRb0Rptr{0,0x0f60u}, kRb0RptrAddr{0,0x1de3u}, kRb0RptrAddrHi{0,0x1de4u},
              kWptrPollLo{0,0x1e8bu}, kWptrPollHi{0,0x1e8cu}, kRb0Base{0,0x1de0u}, kRb0BaseHi{0,0x1e51u},
              kRbActive{0,0x1f40u}, kRbDoorbellCntl{0,0x1e8du}, kRbDbLower{0,0x1dfau}, kRbDbUpper{0,0x1dfbu};
// gc_11_0_0_sh_mask.h CP_ME_CNTL: PFP_PIPE0/1_RESET 18/19, ME_PIPE0/1_RESET 20/21, PFP_HALT 26,
// ME_HALT 28 (CE_HALT 24 is left as it is: gfx11 has no CE). Default 0x15000000.
constexpr uint32_t kPfpReset=(1u<<18)|(1u<<19), kMeReset=(1u<<20)|(1u<<21), kPfpHalt=1u<<26, kMeHalt=1u<<28;
constexpr uint32_t kMeCntlDefault=0x15000000u;
// CP_RB0_CNTL: RB_BUFSZ [5:0] = order_base_2(ring bytes / 8), RB_BLKSZ [13:8] = RB_BUFSZ - 2,
// KMD_QUEUE bit 29. CP_RB_RPTR_ADDR_HI keeps RB_RPTR_ADDR_HI [15:0]. CP_RB_DOORBELL_CONTROL
// DOORBELL_EN bit 30.
constexpr uint32_t kRingBytes=16384u, kRingDwords=kRingBytes/4u;
constexpr uint32_t order2(uint32_t v) { uint32_t o=0; while((1u<<o)<v)++o; return o; }
constexpr uint32_t kRbBufsz=order2(kRingBytes/8u);
constexpr uint32_t kRb0CntlValue=kRbBufsz|((kRbBufsz-2u)<<8)|(1u<<29);
static_assert(kRbBufsz==11u && kRb0CntlValue==0x2000090bu,"CP_RB0_CNTL for a 16 KiB ring");
constexpr uint32_t kRptrAddrHiMask=0xffffu, kDoorbellEn=1u<<30, kDoorbellOffsetMask=0x0ffffffcu;
// The ring is kicked by doorbell, as upstream's gfx ring (use_doorbell) and Navi48's RB0: the RS64 ME
// never fetched RB0 after a CP_RB0_WPTR register write on the card (2026-10-20, RPTR stayed 0).
// amdgpu_doorbell.h AMDGPU_NAVI10_DOORBELL_GFX_RING0 0x8b, GFX_USERQUEUE_END 0xff (64-bit doorbells):
// ring->doorbell_index = 0x8b << 1 (dword 0x116, BAR2 byte 0x458); gfx_v11_0_cp_set_doorbell_range
// puts the RB range at (0x8b * 2) << 2 .. (0xff * 2) << 2 and the MEC range at 0 .. (0x8a * 2) << 2.
constexpr uint32_t kDoorbellDword=0x8bu<<1, kDoorbellByte=4u*kDoorbellDword;
constexpr uint32_t kRbRangeLower=(0x8bu*2u)<<2, kRbRangeUpper=(0xffu*2u)<<2, kMecRangeUpper=(0x8au*2u)<<2;
static_assert(kDoorbellByte==0x458u && kRbRangeLower==0x458u && kRbRangeUpper==0x7f8u && kMecRangeUpper==0x450u,"gfx doorbells");
// gc_11_0_2_pfp.bin / gc_11_0_2_me.bin (linux-firmware 1522c78a; SHA-256 9020f537.. / f5fe48f9..):
// gfx_firmware_header_v2_0 ucode_start_addr_lo 0x3000, _hi 0x70000 for both, as the MEC's.
constexpr uint32_t kStartLo=0x3000u, kStartHi=0x70000u;
constexpr uint32_t kPcStart=(kStartHi<<30)|(kStartLo>>2), kPcStartHi=kStartHi>>2;
static_assert(kPcStart==0xc00u && kPcStartHi==0x1c000u,"PFP/ME program counter");
constexpr uint32_t kMaxHwContexts=8u, kGfxPipes=2u;
// nvd.h packets of gfx_v11_0_cp_gfx_start().
constexpr uint32_t kPreambleCntl=0x4au, kPreambleBegin=2u<<28, kPreambleEnd=3u<<28, kContextControl=0x28u;
constexpr uint32_t kSetContextReg=0x69u, kSetContextStart=0xa000u, kClearState=0x12u;
constexpr uint32_t kTileSteeringOverride=0xa000u+0x00d7u;   // PA_SC_TILE_STEERING_OVERRIDE (seg 1 0xd7)
constexpr uint32_t kTileSteeringValue=0u;                   // gfx.config.pa_sc_tile_steering_override
// Ring memory (bytes from its start): ring, RPTR report, WPTR poll word, ring-test fence.
constexpr uint32_t kRingOff=0u, kRptrOff=kRingBytes, kWptrOff=kRingBytes+8u, kFenceOff=kRingBytes+0x100u;
constexpr uint32_t kMemBytes=kRingBytes+0x1000u, kFenceValue=0x0c0b1640u;
constexpr uint32_t kStatTimeoutUs=100000u, kFenceTimeoutUs=1000000u;

struct Regs {
    uint64_t meCntl=0,pfpStart=0,pfpStartHi=0,meStart=0,meStartHi=0,cpStat=0,maxContext=0,deviceId=0,
             wptrDelay=0,rbVmid=0,rb0Cntl=0,rb0Wptr=0,rb0WptrHi=0,rb0Rptr=0,rb0RptrAddr=0,rb0RptrAddrHi=0,
             wptrPollLo=0,wptrPollHi=0,rb0Base=0,rb0BaseHi=0,rbActive=0,rbDoorbellCntl=0,rbDbLower=0,rbDbUpper=0;
    bool valid=false;
};
inline Regs resolve(const IPDiscovery &d,uint64_t bytes) {
    Regs r{};
    const struct { uint64_t *dst; Reg reg; } all[]={
        {&r.meCntl,kMeCntl},{&r.pfpStart,kPfpStart},{&r.pfpStartHi,kPfpStartHi},{&r.meStart,kMeStart},
        {&r.meStartHi,kMeStartHi},{&r.cpStat,kCpStat},{&r.maxContext,kMaxContext},{&r.deviceId,kDeviceId},
        {&r.wptrDelay,kWptrDelay},{&r.rbVmid,kRbVmid},{&r.rb0Cntl,kRb0Cntl},{&r.rb0Wptr,kRb0Wptr},
        {&r.rb0WptrHi,kRb0WptrHi},{&r.rb0Rptr,kRb0Rptr},{&r.rb0RptrAddr,kRb0RptrAddr},
        {&r.rb0RptrAddrHi,kRb0RptrAddrHi},{&r.wptrPollLo,kWptrPollLo},{&r.wptrPollHi,kWptrPollHi},
        {&r.rb0Base,kRb0Base},{&r.rb0BaseHi,kRb0BaseHi},{&r.rbActive,kRbActive},{&r.rbDoorbellCntl,kRbDoorbellCntl},
        {&r.rbDbLower,kRbDbLower},{&r.rbDbUpper,kRbDbUpper}};
    for(const auto &x:all) {
        const auto a=resolveDword(d,Engine::GFX11,x.reg.seg,x.reg.dw,bytes);
        if(a.code!=AddressCode::Pass)return r;
        *x.dst=a.bar5ByteOffset;
    }
    r.valid=true;return r;
}
// Every register the gfx ring code writes (CP_STAT and CP_RB0_RPTR are read only).
inline bool allowed(const Regs &r,uint64_t off) {
    if(!r.valid)return false;
    const uint64_t w[]={r.meCntl,r.pfpStart,r.pfpStartHi,r.meStart,r.meStartHi,r.maxContext,r.deviceId,r.wptrDelay,
                        r.rbVmid,r.rb0Cntl,r.rb0Wptr,r.rb0WptrHi,r.rb0RptrAddr,r.rb0RptrAddrHi,r.wptrPollLo,
                        r.wptrPollHi,r.rb0Base,r.rb0BaseHi,r.rbActive,r.rbDoorbellCntl,r.rbDbLower,r.rbDbUpper};
    for(uint64_t o:w)if(off==o)return true;
    return false;
}
// The preamble gfx_v11_0_cp_gfx_start() puts on the ring: kCsbDwords dwords.
inline uint32_t buildCsb(uint32_t *p) {
    namespace cs=gfx11_clear_state;
    uint32_t k=0;
    p[k++]=gfx11_dispatch::packet3(kPreambleCntl,0u);p[k++]=kPreambleBegin;
    p[k++]=gfx11_dispatch::packet3(kContextControl,1u);p[k++]=0x80000000u;p[k++]=0x80000000u;
    for(const cs::Extent &e:cs::kExtents) {
        p[k++]=gfx11_dispatch::packet3(kSetContextReg,e.count);p[k++]=e.regIndex-kSetContextStart;
        for(uint32_t i=0;i<e.count;++i)p[k++]=e.values[i];
    }
    p[k++]=gfx11_dispatch::packet3(kSetContextReg,1u);p[k++]=kTileSteeringOverride-kSetContextStart;p[k++]=kTileSteeringValue;
    p[k++]=gfx11_dispatch::packet3(kPreambleCntl,0u);p[k++]=kPreambleEnd;
    p[k++]=gfx11_dispatch::packet3(kClearState,0u);p[k++]=0u;
    return k;
}
// gfx_v11_0_ring_emit_fence() for a 32-bit sequence number at an MC address.
inline uint32_t buildFence(uint32_t *p,uint64_t mc,uint32_t value) {
    const uint32_t f[8]={gfx11_dispatch::packet3(gfx11_dispatch::kReleaseMem,6u),gfx11_dispatch::kReleaseMemControl,
                         gfx11_dispatch::kReleaseMemDataSel32,static_cast<uint32_t>(mc),static_cast<uint32_t>(mc>>32),
                         value,0u,0u};
    for(uint32_t i=0;i<8u;++i)p[i]=f[i];
    return 8u;
}
}}
