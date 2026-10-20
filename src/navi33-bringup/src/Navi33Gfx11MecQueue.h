// SPDX-License-Identifier: MIT
// First compute queue on GC 11.0.2, without MES (G34). Upstream sources,
// torvalds/linux master fetched 2026-10-19:
//  - gfx_v11_0_config_gfx_rs64() (PSP path): MEC program counters per pipe,
//    pipe reset then clear. The PFP/ME (graphics) half is left out.
//  - gfx_v11_0_cp_compute_enable(true): unhalt the MEC (G17 word).
//  - hqd_load_v11() in amdgpu_amdkfd_gfx_v11.c: copy the MQD into
//    CP_MQD_BASE_ADDR..CP_HQD_PQ_WPTR_HI, doorbell logic on, one-shot WPTR
//    poll from memory, EOP fetcher, CP_HQD_ACTIVE. The MQD is the G19 image
//    (gfx_v11_0_compute_mqd_init). RLC_CP_SCHEDULERS (HIQ marking) is left out.
//  - gfx_v11_0_ring_test_ring(): SCRATCH_REG0 0xCAFEDEAD, then SET_UCONFIG_REG
//    0xDEADBEEF from the ring. A RELEASE_MEM fence follows, as
//    gfx_v11_0_ring_emit_fence() emits it, to prove GPU writes to VRAM.
// Queue me 1 pipe 0 queue 0, VMID 0. Every buffer sits in one 1 MiB VRAM window
// that the caller proved is BAR0 == VRAM (psp13load::planWindow). No hardware
// I/O except callbacks.
#pragma once
#include "Navi33Gfx11Dispatch.h"
#include "Navi33Gfx11MecEnable.h"
#include "Navi33Gfx11Mqd.h"
#include "Navi33HardwarePlan.h"
#include "Navi33NativeABI.h"
namespace navi33 { namespace mec_queue {
namespace kiq=gfx11_kiq;
// gc_11_0_0_offset.h. Segment 1: GRBM_GFX_CNTL, CP_MEC_RS64_*, SCRATCH_REG0,
// RLC_RLCS_BOOTLOAD_STATUS. Segment 0: the HQD block, CP_PQ_WPTR_POLL_CNTL1.
constexpr uint32_t kGrbmGfxCntl=0x0900u, kPrgrmCntrStart=0x2900u, kPrgrmCntrStartHi=0x2938u;
constexpr uint32_t kSchedulerSeg=1u, kSchedulerDw=0x098au; // RLC_CP_SCHEDULERS
constexpr uint32_t kScratchReg0=0x2040u, kRlcBootload=0x4e82u, kSosAlive=0x91u;
constexpr uint32_t kHqdFirst=0x1fa9u, kHqdLast=0x1fe0u, kHqdHole=0x1fb9u; // CP_MQD_BASE_ADDR..CP_HQD_PQ_WPTR_HI
constexpr uint32_t kPollCntl1=0x1e24u, kEopRptr=0x1fd1u;
// CP_HQD_EOP_RPTR.INIT_FETCHER is bit 31 (gc_11_0_0_sh_mask.h; bit 30 is the status bit
// RPTR_EQ_CSMD_WPTR). Until kext 0.0.46 this was bit 30, so the EOP fetcher was never
// restarted: a fresh HQD still worked (power-on fetcher state), but a reloaded one queued its
// end-of-pipe events and never processed them (client stalls, 2026-10-20 13:19-13:58).
constexpr uint32_t kEopInitFetcher=1u<<31;
static_assert(kEopInitFetcher==0x80000000u,"CP_HQD_EOP_RPTR__INIT_FETCHER_MASK");
constexpr uint32_t kDoorbellEn=1u<<30;          // CP_HQD_PQ_DOORBELL_CONTROL.DOORBELL_EN
// gc_11_0_2_mec.bin (pinned, SHA-256 19bf080d…): gfx_firmware_header_v2_0
// ucode_start_addr_lo 0x3000, _hi 0x70000. Upstream writes lo>>2 | hi<<30 (u32)
// and hi>>2.
constexpr uint32_t kMecStartLo=0x3000u, kMecStartHi=0x70000u;
constexpr uint32_t kPcStart=(kMecStartLo>>2)|(kMecStartHi<<30), kPcStartHi=kMecStartHi>>2;
static_assert(kPcStart==0xc00u && kPcStartHi==0x1c000u,"MEC program counter");
constexpr uint32_t kMeMec=1u, kPipes=4u, kPipe=0u, kQueue=0u;
// GRBM_GFX_CNTL: PIPEID [1:0], MEID [3:2], QUEUEID [10:8].
constexpr uint32_t grbmSelect(uint32_t me,uint32_t pipe,uint32_t queue) {
    return (pipe&3u)|((me&3u)<<2u)|((queue&7u)<<8u);
}
// nvd.h: SET_UCONFIG_REG 0x79 from 0xc000; NOP 0x10. SCRATCH_REG0 is
// 0xA000 + 0x2040 on this IP Discovery, 0x40 past the UCONFIG start.
constexpr uint32_t kSetUconfig=0x79u, kUconfigStart=0xc000u, kGcSeg1Base=0xa000u;
constexpr uint32_t kNop=gfx11_dispatch::packet3(0x10u,0x3fffu);
constexpr uint32_t kScratchPreset=0xcafedeadu, kScratchDone=0xdeadbeefu, kFenceValue=0x0c0b1601u;
// Window layout (bytes from the window start).
constexpr uint32_t kWindowBytes=1u<<20, kMqdOff=0x0000u, kEopOff=0x1000u, kRingOff=0x2000u;
constexpr uint32_t kRptrOff=0x3000u, kWptrOff=0x3008u, kFenceOff=0x4000u, kClearBytes=0x5000u;
constexpr uint32_t kRingBytes=4096u, kEopBytes=2048u, kRingDwords=16u;
constexpr uint32_t kTimeoutMs=1000u;
constexpr unsigned kMaxWrites=96u;

struct Regs {
    uint64_t grbm=0,pc=0,pcHi=0,cntl=0,scratch=0,rlc=0,sos=0,poll1=0,scheduler=0,hqd[kHqdLast-kHqdFirst+1u]{};
    bool valid=false;
};
inline Regs resolve(const IPDiscovery &d,uint64_t bytes) {
    Regs r{};
    struct One { uint64_t *dst; Engine e; uint8_t seg; uint32_t dw; };
    const One ones[]={{&r.grbm,Engine::GFX11,1,kGrbmGfxCntl},{&r.pc,Engine::GFX11,1,kPrgrmCntrStart},
        {&r.pcHi,Engine::GFX11,1,kPrgrmCntrStartHi},{&r.cntl,Engine::GFX11,1,gfx11_mec::kCpMecRs64CntlDword},
        {&r.scratch,Engine::GFX11,1,kScratchReg0},{&r.rlc,Engine::GFX11,1,kRlcBootload},
        {&r.sos,Engine::PSP13,0,kSosAlive},{&r.poll1,Engine::GFX11,0,kPollCntl1},
        {&r.scheduler,Engine::GFX11,kSchedulerSeg,kSchedulerDw}};
    for(const One &o:ones) {
        const auto a=resolveDword(d,o.e,o.seg,o.dw,bytes);
        if(a.code!=AddressCode::Pass)return r;
        *o.dst=a.bar5ByteOffset;
    }
    for(uint32_t dw=kHqdFirst;dw<=kHqdLast;++dw) {
        const auto a=resolveDword(d,Engine::GFX11,0,dw,bytes);
        if(a.code!=AddressCode::Pass)return r;
        r.hqd[dw-kHqdFirst]=a.bar5ByteOffset;
    }
    r.valid=true;return r;
}
inline uint64_t hqd(const Regs &r,uint32_t dw) { return r.hqd[dw-kHqdFirst]; }
// The only registers the run may write; the hole at 0x1fb9 is never one.
inline bool allowed(const Regs &r,uint64_t off,bool gfxMapGate=false) {
    if(!r.valid)return false;
    if(off==r.grbm || off==r.pc || off==r.pcHi || off==r.cntl || off==r.scratch || off==r.poll1 ||
       (gfxMapGate && off==r.scheduler))
        return true;
    for(uint32_t dw=kHqdFirst;dw<=kHqdLast;++dw)
        if(dw!=kHqdHole && off==hqd(r,dw))return true;
    return false;
}
struct Layout { uint64_t windowMc=0; };   // MC of the 1 MiB window (BAR0 proof by the caller)
struct Hw {
    void *ctx=nullptr;
    bool (*read)(void *,uint64_t,uint32_t *)=nullptr;
    bool (*write)(void *,uint64_t,uint32_t)=nullptr;
    // Offsets relative to the window start.
    bool (*vramWrite)(void *,uint32_t off,const uint32_t *src,uint32_t dwords)=nullptr;
    bool (*vramRead)(void *,uint32_t off,uint32_t *dst,uint32_t dwords)=nullptr;
    void (*sleep)(void *,uint32_t)=nullptr;
};
inline bool read(const Hw &h,uint64_t off,uint32_t &v) {
    return h.read && h.read(h.ctx,off,&v) && v!=0xffffffffu;
}
inline n33_psp_load_result blank() {
    n33_psp_load_result o{};o.abi_version=N33_ABI_VERSION;o.size=sizeof(o);
    o.mec_marker=N33_MEC_MARKER;return o;
}
// The queue as gfx_v11_0_compute_mqd_init() sees it: no doorbell in the MQD
// (hqd_load_v11 turns the logic on by register), WPTR polled from memory.
inline kiq::KiqQueue queueFor(const Layout &l) {
    kiq::KiqQueue q{};
    q.mqdGpuVa=l.windowMc+kMqdOff;q.eopGpuVa=l.windowMc+kEopOff;q.pqGpuVa=l.windowMc+kRingOff;
    q.rptrGpuVa=l.windowMc+kRptrOff;q.wptrGpuVa=l.windowMc+kWptrOff;
    q.queueBytes=kRingBytes;q.eopBytes=kEopBytes;q.useDoorbell=false;
    return q;
}
// Ring contents: Linux's compute ring test, then a 32-bit RELEASE_MEM fence,
// NOP-padded to 16 dwords.
inline void ringPackets(const Layout &l,uint32_t out[kRingDwords]) {
    namespace gd=gfx11_dispatch;
    const uint64_t fence=l.windowMc+kFenceOff;
    const uint32_t p[]={gd::packet3(kSetUconfig,1u),kGcSeg1Base+kScratchReg0-kUconfigStart,kScratchDone,
        gd::packet3(gd::kReleaseMem,6u),gd::kReleaseMemControl,gd::kReleaseMemDataSel32,
        static_cast<uint32_t>(fence),static_cast<uint32_t>(fence>>32),kFenceValue,0u,0u};
    for(uint32_t i=0;i<kRingDwords;++i)out[i]=i<sizeof(p)/4u ? p[i] : kNop;
}
inline uint32_t sample(const Regs &r,const Hw &h,n33_psp_load_result &o) {
    uint32_t sos=0,grbm=0;
    if(!read(h,r.sos,sos) || !read(h,r.rlc,o.mec_rlc) || !read(h,r.grbm,grbm) ||
       !read(h,r.cntl,o.mec_cntl_before) || !read(h,r.scratch,o.mec_scratch))
        return N33_PSPL_READ_FAILED;
    o.c2pmsg81_after=sos;o.mec_cntl_after=o.mec_cntl_before;
    if(!sos)return N33_PSPL_SOS_NOT_ALIVE;
    // RLC boot complete, no GRBM select held, MEC still halted (nobody runs it).
    if(!(o.mec_rlc&0x80000000u) || grbm || !(o.mec_cntl_before&gfx11_mec::kMecHalt))
        return N33_PSPL_MEC_BAD_PRECONDITION;
    return N33_PSPL_PLAN_OK;
}
struct Writer {
    const Regs &r; const Hw &h; n33_psp_load_result &o; bool gfxMapGate=false; uint32_t select=0u;
    bool put(uint64_t off,uint32_t v) {
        if(!allowed(r,off,gfxMapGate) || o.register_writes>=kMaxWrites || !h.write(h.ctx,off,v))return false;
        ++o.register_writes;o.flags|=N33_PSPL_FLAG_WROTE_REGISTERS;
        if(off==r.grbm)select=v;
        return true;
    }
    bool sel(uint32_t v) { return select==v || put(r.grbm,v); }
    n33_psp_load_result &stop(uint32_t state) {
        if(select)put(r.grbm,0u);
        o.state=state;return o;
    }
};
// Plan calls get no write callback. The run consumes the latch before its
// first write and never halts, dequeues or retries: recovery is a power-off.
inline n33_psp_load_result execute(const Regs &r,const Layout &l,const Hw &h,bool run,bool gate,
                                  bool &attempted,bool gfxMapGate=false,
                                  uint32_t *schedulerBefore=nullptr,uint32_t *schedulerAfter=nullptr) {
    auto o=blank();
    if(gate)o.flags|=N33_PSPL_FLAG_WRITE_GATE_OPEN;
    if(!r.valid){o.state=N33_PSPL_BAD_ROM;return o;}
    o.mec_window_mc=l.windowMc;
    if(!l.windowMc || (l.windowMc&(kWindowBytes-1u))){o.state=N33_PSPL_NO_WINDOW;return o;}
    const gfx11_mqd::Result mqd=gfx11_mqd::buildKiqMqd(queueFor(l));
    if(mqd.code!=kiq::Code::PlanOnly){o.state=N33_PSPL_BAD_REQUEST;return o;}
    o.state=sample(r,h,o);
    if(o.state!=N33_PSPL_PLAN_OK || !run)return o;
    if(!gate){o.state=N33_PSPL_WRITE_DISABLED;return o;}
    if(attempted){o.state=N33_PSPL_ALREADY_ATTEMPTED;return o;}
    if(!h.write || !h.sleep || !h.vramWrite || !h.vramRead){o.state=N33_PSPL_BAD_REQUEST;return o;}
    attempted=true;
    Writer w{r,h,o,gfxMapGate};
    // 1. VRAM first (no register touched yet): zeroed window head, MQD, ring,
    //    WPTR in memory = the ring's end. Read everything back.
    static uint32_t zero[kClearBytes/4u];
    uint32_t ring[kRingDwords],wptr[2]={kRingDwords,0u};
    ringPackets(l,ring);
    struct Blob { uint32_t off; const uint32_t *src; uint32_t dwords; };
    const Blob blobs[]={{0u,zero,kClearBytes/4u},{kMqdOff,mqd.image.dw,gfx11_mqd::kDwords},
                        {kRingOff,ring,kRingDwords},{kWptrOff,wptr,2u}};
    for(const Blob &b:blobs)
        if(!h.vramWrite(h.ctx,b.off,b.src,b.dwords)){o.state=N33_PSPL_STAGE_VERIFY_FAILED;return o;}
    for(const Blob &b:blobs) {
        if(b.src==zero)continue;
        for(uint32_t i=0;i<b.dwords;++i) {
            uint32_t v=0;
            if(!h.vramRead(h.ctx,b.off+4u*i,&v,1u) || v!=b.src[i])
                {o.state=N33_PSPL_STAGE_VERIFY_FAILED;return o;}
        }
    }
    o.flags|=N33_PSPL_FLAG_WROTE_VRAM;
    // 2. gfx_v11_0_config_gfx_rs64(), MEC half.
    for(uint32_t pipe=0;pipe<kPipes;++pipe)
        if(!w.sel(grbmSelect(kMeMec,pipe,0u)) || !w.put(r.pc,kPcStart) || !w.put(r.pcHi,kPcStartHi))
            return w.stop(N33_PSPL_MEC_WRITE_FAILED);
    if(!w.sel(0u))return w.stop(N33_PSPL_MEC_WRITE_FAILED);
    const uint32_t resets=gfx11_mec::kMecPipeResetAll;
    uint32_t cntl=o.mec_cntl_before|resets;
    if(!w.put(r.cntl,cntl) || !w.put(r.cntl,cntl&~resets))return w.stop(N33_PSPL_MEC_WRITE_FAILED);
    ++o.steps_done;
    // 3. gfx_v11_0_cp_compute_enable(true), then udelay(50).
    cntl=gfx11_mec::rs64CntlWord(cntl&~resets,true);
    if(!w.put(r.cntl,cntl))return w.stop(N33_PSPL_MEC_WRITE_FAILED);
    h.sleep(h.ctx,1u);++o.elapsed_ms;
    if(!read(h,r.cntl,o.mec_cntl_after))return w.stop(N33_PSPL_READ_FAILED);
    ++o.steps_done;
    // 4. Ring test preset, as upstream does before committing.
    if(!w.put(r.scratch,kScratchPreset))return w.stop(N33_PSPL_MEC_WRITE_FAILED);
    // 5. hqd_load_v11() for me 1 pipe 0 queue 0.
    if(!w.sel(grbmSelect(kMeMec,kPipe,kQueue)))return w.stop(N33_PSPL_MEC_WRITE_FAILED);
    if(gfxMapGate) {
        uint32_t before=0u;
        if(!read(h,r.scheduler,before))return w.stop(N33_PSPL_READ_FAILED);
        if(schedulerBefore)*schedulerBefore=before;
        // Linux marks the KIQ before loading/activating its HQD. Refuse if
        // this queue is already marked HIQ; only G34's exact selector changes.
        if(before&0x80u)return w.stop(N33_PSPL_MEC_BAD_PRECONDITION);
        const uint32_t target=(before&0xffffff00u)|0xa0u;
        if(!w.put(r.scheduler,target))return w.stop(N33_PSPL_MEC_WRITE_FAILED);
        uint32_t after=0u;
        if(!read(h,r.scheduler,after))return w.stop(N33_PSPL_READ_FAILED);
        if(schedulerAfter)*schedulerAfter=after;
        if(after!=target)return w.stop(N33_PSPL_READ_FAILED);
    }
    uint32_t active=0;
    if(!read(h,hqd(r,kiq::kCpHqdActive),active))return w.stop(N33_PSPL_READ_FAILED);
    o.mec_hqd_active_before=active;
    if(active&1u)return w.stop(N33_PSPL_MEC_BAD_PRECONDITION);
    const uint32_t *img=mqd.image.dw;
    for(uint32_t dw=kHqdFirst;dw<=kHqdLast;++dw)
        if(dw!=kHqdHole && !w.put(hqd(r,dw),img[gfx11_mqd::kCpMqdBaseAddrLo+(dw-kHqdFirst)]))
            return w.stop(N33_PSPL_MEC_WRITE_FAILED);
    const uint64_t wptrMc=l.windowMc+kWptrOff;
    const struct { uint32_t dw; uint32_t v; } tail[]={
        {kiq::kCpHqdPqDoorbellControl,img[gfx11_mqd::kCpHqdPqDoorbellControl]|kDoorbellEn},
        {kiq::kCpHqdPqWptrLo,0u},{kiq::kCpHqdPqWptrHi,0u},   // guessed WPTR = RPTR = 0
        {kiq::kCpHqdPqWptrPollAddr,static_cast<uint32_t>(wptrMc)},
        {kiq::kCpHqdPqWptrPollAddrHi,static_cast<uint32_t>(wptrMc>>32)}};
    for(const auto &t:tail)if(!w.put(hqd(r,t.dw),t.v))return w.stop(N33_PSPL_MEC_WRITE_FAILED);
    // get_queue_mask(pipe 0, queue 0) = bit 0: one-shot WPTR poll.
    if(!w.put(r.poll1,1u<<(kPipe*8u+kQueue)) ||
       !w.put(hqd(r,kEopRptr),img[gfx11_mqd::kCpMqdBaseAddrLo+(kEopRptr-kHqdFirst)]|kEopInitFetcher) ||
       !w.put(hqd(r,kiq::kCpHqdActive),img[gfx11_mqd::kCpHqdActive]|1u))
        return w.stop(N33_PSPL_MEC_WRITE_FAILED);
    ++o.steps_done;
    if(!w.sel(0u))return w.stop(N33_PSPL_MEC_WRITE_FAILED);
    // 6. Wait for the scratch register (ring fetched and executed) and the
    //    fence (GPU wrote VRAM), then read the queue's RPTR.
    for(uint32_t ms=0;;++ms) {
        uint32_t fence=0;
        if(!read(h,r.scratch,o.mec_scratch) || !h.vramRead(h.ctx,kFenceOff,&fence,1u))
            return w.stop(N33_PSPL_READ_FAILED);
        o.mec_fence=fence;
        if(o.mec_scratch==kScratchDone && fence==kFenceValue)break;
        if(ms==kTimeoutMs)break;
        h.sleep(h.ctx,1u);++o.elapsed_ms;
    }
    if(!w.sel(grbmSelect(kMeMec,kPipe,kQueue)) || !read(h,hqd(r,kiq::kCpHqdPqRptr),o.mec_rptr) ||
       !read(h,hqd(r,kiq::kCpHqdActive),o.mec_hqd_active_after))
        return w.stop(N33_PSPL_READ_FAILED);
    if(!w.sel(0u))return w.stop(N33_PSPL_MEC_WRITE_FAILED);
    if(o.mec_scratch!=kScratchDone){o.state=N33_PSPL_MEC_NO_FETCH;return o;}
    ++o.steps_done;
    if(o.mec_fence!=kFenceValue){o.state=N33_PSPL_MEC_NO_FENCE;return o;}
    ++o.steps_done;
    o.state=N33_PSPL_MEC_QUEUE_OK;return o;
}
} }
