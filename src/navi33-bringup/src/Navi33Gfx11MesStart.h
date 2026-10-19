// SPDX-License-Identifier: MIT
// MES start on GC 11.0.2: mes_v11_0_enable(adev, true) followed by
// mes_v11_0_get_fw_version(), torvalds/linux master fetched 2026-10-19. The
// writes come from the G20 planner (Navi33GfxMes.h); this runner only adds
// the GRBM selects soc21_grbm_select() makes around them, the preconditions
// and the readback. No hardware I/O except callbacks.
//
// Left out on purpose (gfx_v11_0_cp_resume() before amdgpu_mes_kiq_hw_init):
// the GUI-idle interrupt and the doorbell range; nothing here uses either.
// Not done here: kiq_setting, the KIQ-pipe queue and the scheduler ring.
#pragma once
#include "Navi33GfxMes.h"
#include "Navi33HardwarePlan.h"
#include "Navi33NativeABI.h"
namespace navi33 { namespace mes_start {
// gc_11_0_0_offset.h: GRBM_GFX_CNTL 0x0900, CP_MES_GP3_LO 0x2849,
// CP_MES_INSTR_PNTR 0x2813 and RLC_RLCS_BOOTLOAD_STATUS 0x4e82 are segment 1;
// CP_STAT 0x0f40 is segment 0.
constexpr uint32_t kGrbmGfxCntl=0x0900u, kCpMesGp3Lo=0x2849u, kCpMesInstrPntr=0x2813u;
constexpr uint32_t kRlcBootload=0x4e82u, kCpStat=0x0f40u, kSosAlive=0x91u;
// gc_11_0_0_sh_mask.h GRBM_GFX_CNTL: PIPEID [1:0], MEID [3:2].
constexpr uint32_t grbmSelect(uint32_t me,uint32_t pipe) { return (pipe&3u)|((me&3u)<<2u); }
// mes_firmware_header_v1_0 mes_uc_start_addr of the pinned gc_11_0_2_mes_2.bin
// and gc_11_0_2_mes1.bin (G20); the PSP stage loads exactly those files.
constexpr uint64_t kUcStart=0xfffffffff0005000ull;
constexpr uint32_t kActiveMask=gfx11_mes::kMesPipe0Active|gfx11_mes::kMesPipe1Active;
// Readback rounds after the enable, in ms after the last write (upstream reads
// once after 500 us; later rounds only give slow firmware time).
constexpr uint32_t kSampleAtMs[3]={1u,100u,1000u};
constexpr unsigned kMaxWrites=9u+3u*3u;

struct Regs { uint64_t grbm=0,cntl=0,start=0,startHi=0,gp3=0,pc=0,rlc=0,cpStat=0,sos=0; bool valid=false; };
inline Regs resolve(const IPDiscovery &d,uint64_t bytes) {
    Regs r{};
    uint64_t *dst[]={&r.grbm,&r.cntl,&r.start,&r.startHi,&r.gp3,&r.pc,&r.rlc,&r.cpStat,&r.sos};
    const Engine eng[]={Engine::GFX11,Engine::GFX11,Engine::GFX11,Engine::GFX11,Engine::GFX11,
                        Engine::GFX11,Engine::GFX11,Engine::GFX11,Engine::PSP13};
    const uint8_t seg[]={1,1,1,1,1,1,1,0,0};
    const uint32_t dw[]={kGrbmGfxCntl,gfx11_mes::kCpMesCntl,gfx11_mes::kCpMesPrgrmCntrStart,
                         gfx11_mes::kCpMesPrgrmCntrStartHi,kCpMesGp3Lo,kCpMesInstrPntr,
                         kRlcBootload,kCpStat,kSosAlive};
    for(unsigned i=0;i<9u;++i) {
        const auto a=resolveDword(d,eng[i],seg[i],dw[i],bytes);
        if(a.code!=AddressCode::Pass)return r;
        *dst[i]=a.bar5ByteOffset;
    }
    r.valid=true;return r;
}
// The only registers the run may write.
inline bool allowed(const Regs &r,uint64_t off) {
    return r.valid && (off==r.grbm || off==r.cntl || off==r.start || off==r.startHi);
}
struct Hw {
    void *ctx=nullptr;
    bool (*read)(void *,uint64_t,uint32_t *)=nullptr;
    bool (*write)(void *,uint64_t,uint32_t)=nullptr;
    void (*sleep)(void *,uint32_t)=nullptr;
};
inline bool read(const Hw &h,uint64_t off,uint32_t &v) {
    return h.read && h.read(h.ctx,off,&v) && v!=0xffffffffu;
}
inline n33_psp_load_result blank() {
    n33_psp_load_result o{};o.abi_version=N33_ABI_VERSION;o.size=sizeof(o);
    o.mes_marker=N33_MES_MARKER;return o;
}
inline uint32_t sample(const Regs &r,const Hw &h,n33_psp_load_result &o) {
    if(!read(h,r.sos,o.c2pmsg81_after) || !read(h,r.rlc,o.mes_rlc) ||
       !read(h,r.cpStat,o.mes_cp_stat) || !read(h,r.grbm,o.mes_grbm_before) ||
       !read(h,r.cntl,o.mes_cntl_before))return N33_PSPL_READ_FAILED;
    o.mes_cntl_after=o.mes_cntl_before;
    if(!o.c2pmsg81_after)return N33_PSPL_SOS_NOT_ALIVE;
    // RLC bootload complete (the SCPM stage's result), nobody holding a GRBM
    // select, and no MES pipe already active.
    if(!(o.mes_rlc&0x80000000u) || o.mes_grbm_before!=0u || (o.mes_cntl_before&kActiveMask))
        return N33_PSPL_MES_BAD_PRECONDITION;
    return N33_PSPL_PLAN_OK;
}
struct Writer {
    const Regs &r; const Hw &h; n33_psp_load_result &o; uint32_t select=0u;
    bool put(uint64_t off,uint32_t v) {
        if(!allowed(r,off) || o.register_writes>=kMaxWrites || !h.write(h.ctx,off,v))return false;
        ++o.register_writes;o.flags|=N33_PSPL_FLAG_WROTE_REGISTERS;
        if(off==r.grbm)select=v;
        return true;
    }
    bool selectFor(uint32_t want) { return select==want || put(r.grbm,want); }
    // Every stop leaves GRBM on me 0 pipe 0, as soc21_grbm_select(0,0,0,0).
    n33_psp_load_result &stop(uint32_t state) {
        if(select)put(r.grbm,0u);
        o.state=state;return o;
    }
};
// Plan calls never receive a write callback. Run rechecks the live state under
// the provider lock and consumes the one-shot latch before the first write.
inline n33_psp_load_result execute(const Regs &r,const Hw &h,bool run,bool gate,bool &attempted) {
    auto o=blank();
    if(gate)o.flags|=N33_PSPL_FLAG_WRITE_GATE_OPEN;
    if(!r.valid){o.state=N33_PSPL_BAD_ROM;return o;}
    gfx11_mes::EnableInput in{};
    in.ucStart[gfx11_mes::kSchedPipe]=in.ucStart[gfx11_mes::kKiqPipe]=kUcStart;
    o.state=sample(r,h,o);
    if(o.state!=N33_PSPL_PLAN_OK)return o;
    in.cntlCurrent=o.mes_cntl_before;
    const gfx11_mes::Plan plan=gfx11_mes::buildEnable(in);
    if(plan.code!=gfx11_mes::Code::PlanOnly || plan.count!=6u){o.state=N33_PSPL_BAD_REQUEST;return o;}
    if(!run)return o;
    if(!gate){o.state=N33_PSPL_WRITE_DISABLED;return o;}
    if(attempted){o.state=N33_PSPL_ALREADY_ATTEMPTED;return o;}
    if(!h.write || !h.sleep){o.state=N33_PSPL_BAD_REQUEST;return o;}
    attempted=true;
    Writer w{r,h,o};
    const uint64_t target[]={r.cntl,r.start,r.startHi};
    for(uint8_t i=0;i<plan.count;++i) {
        const gfx11_mes::Write &pw=plan.writes[i];
        const uint32_t want=pw.grbmPipe==gfx11_mes::kNoGrbmPipe ? 0u :
            grbmSelect(pw.grbmMe,pw.grbmPipe);
        const uint64_t off=pw.dword==gfx11_mes::kCpMesCntl ? target[0] :
            pw.dword==gfx11_mes::kCpMesPrgrmCntrStart ? target[1] :
            pw.dword==gfx11_mes::kCpMesPrgrmCntrStartHi ? target[2] : 0u;
        if(pw.seg!=gfx11_mes::kGc11SegMes || !off || !w.selectFor(want) || !w.put(off,pw.value))
            return w.stop(N33_PSPL_MES_WRITE_FAILED);
        ++o.steps_done;
    }
    // Firmware versions per pipe, as mes_v11_0_get_fw_version() reads them.
    uint32_t waited=0u;
    for(uint32_t at:kSampleAtMs) {
        h.sleep(h.ctx,at-waited);o.elapsed_ms+=at-waited;waited=at;
        ++o.mes_samples;
        for(uint32_t pipe=0;pipe<2u;++pipe) {
            if(!w.put(r.grbm,grbmSelect(gfx11_mes::kMesGrbmMe,pipe)))
                return w.stop(N33_PSPL_MES_WRITE_FAILED);
            if(!read(h,r.gp3,o.mes_version[pipe]) || !read(h,r.pc,o.mes_pc[pipe]))
                return w.stop(N33_PSPL_READ_FAILED);
        }
        if(!w.put(r.grbm,0u))return w.stop(N33_PSPL_MES_WRITE_FAILED);
        if(!read(h,r.cntl,o.mes_cntl_after) || !read(h,r.cpStat,o.mes_cp_stat))
            {o.state=N33_PSPL_READ_FAILED;return o;}
        if(o.mes_version[0] && o.mes_version[1]){o.state=N33_PSPL_MES_RUNNING;return o;}
    }
    // Never halt, retry or reset: a full power-off is the recovery.
    o.state=N33_PSPL_MES_NO_VERSION;return o;
}
} }
