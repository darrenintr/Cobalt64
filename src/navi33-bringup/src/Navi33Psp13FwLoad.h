// SPDX-License-Identifier: MIT
// PSP13 IP firmware load stage: planner and runner, host-tested, no I/O.
// Sends LOAD_IP_FW for a range of the 27 records of Navi33Psp13IpFw.h through
// the KM ring that Navi33Psp13Ring.h created, optionally followed by
// AUTOLOAD_RLC. All hardware access goes through the callbacks of
// psp13ring::Hw; Navi33DarwinPspFw.cpp supplies them. Physical use is gated
// by AGENTS.md; the approved sequence ran on 2026-10-18.
//
// Linux (psp_load_non_psp_fw): one LOAD_IP_FW per ucode in AMDGPU_UCODE_ID
// order, AUTOLOAD_RLC right after RLC_G. The SMU is record 26 but Linux sends
// it first, so our approved sequence sends [26,27), then [0,26), then
// AUTOLOAD_RLC alone as [27,28).
//
// Window (VRAM offsets from fw_pri, 8 MiB at 240 MiB): fw_pri 0..1 MiB is left
// alone here, the ring pages sit at 1 MiB, the staged images at 4..8 MiB.
// Each record has its own page-aligned slot in list order, exactly like
// Linux's fw_buf, so a record always lands at the same address whichever
// range a run covers.
#pragma once
#include "Navi33Psp13IpFw.h"
#include "Navi33Psp13Ring.h"

namespace navi33 { namespace psp13fwload {
namespace pl=psp13load;
namespace rg=psp13ring;
namespace fw=psp13ipfw;

constexpr uint64_t kWindowBytes=8u*1024u*1024u;
constexpr uint32_t kStageOff=4u*1024u*1024u;
constexpr uint32_t kStageBytes=4u*1024u*1024u;
constexpr uint32_t kStopAutoload=N33_PSP_FW_STOP_AUTOLOAD;
// The SMU image is the last record (26) and Linux sends it alone, before the
// others (2026-10-18). On bare metal a restart resets the PSP (2026-10-20), so
// sOS, ring, SMU image and the other records must all fit in one boot: the
// SMU-only range gets its own once-per-boot latch; every other range keeps the
// stage's single latch.
constexpr uint32_t kSmuRecord=kStopAutoload-2u;
constexpr bool smuOnlyRange(uint32_t start,uint32_t stop) {
    return start==kSmuRecord && stop==kSmuRecord+1u;
}

struct Plan {
    n33_psp_load_state state=N33_PSPL_BAD_REQUEST;
    rg::Plan ring;                // addresses of the ring pages (cmd, fence) and the proof
    fw::List list;
    uint32_t start=0u, stop=0u;   // records [start, min(stop, count)); stop > count: autoload
    bool autoload=false;
    uint64_t stageMc=0u;          // MC address of the staging area
    uint64_t stagingTotal=0u;
};
// Whether stop_after asks for AUTOLOAD_RLC is separate from whether it may
// run: the writer refuses it unless navi33-psp-autoload=1.
inline bool wantsAutoload(const Plan &p) { return p.autoload; }
inline uint32_t firstRecord(const Plan &p) { return p.start; }
inline uint32_t endRecord(const Plan &p) {
    return p.stop<p.list.count ? p.stop : p.list.count;
}
inline uint32_t recordCount(const Plan &p) {
    return endRecord(p)>p.start ? endRecord(p)-p.start : 0u;
}

// requirePins=false exists for host tests with synthetic images only; the kext
// never passes it (tests/test_manifest.py checks the call).
inline Plan plan(const rg::Snapshot &s,const pl::Range &console,const pl::Range &bar0,
                 const fw::Images &in,uint32_t start,uint32_t stop,
                 bool requirePins=true) {
    Plan p{};
    fw::List list;
    if(!fw::build(in,list) || list.count!=N33_PSP_FW_RECORDS ||
       (requirePins && !fw::allPinned(in))) {
        p.state=N33_PSPL_FW_BAD_IMAGES;return p;
    }
    if(stop<1u || stop>kStopAutoload || start>=stop || start>list.count)return p;
    p.list=list;p.start=start;p.stop=stop;p.autoload=stop==kStopAutoload;
    if(!s.base.c2p81) {p.state=N33_PSPL_SOS_NOT_ALIVE;return p;}
    if(!rg::respOk(s.c64)) {p.state=N33_PSPL_RING_NOT_READY;return p;}
    p.ring.win=pl::planWindow(s.base,console,bar0,true,kWindowBytes);
    if(p.ring.win.state!=N33_PSPL_PLAN_OK) {p.state=p.ring.win.state;return p;}
    p.ring.ringMc=p.ring.win.fwPriMc+rg::kRingOff;
    p.ring.cmdMc=p.ring.win.fwPriMc+rg::kCmdOff;
    p.ring.fenceMc=p.ring.win.fwPriMc+rg::kFenceOff;
    p.stageMc=p.ring.win.fwPriMc+kStageOff;
    // The ring must already exist at our address (the ring stage made it);
    // creating it is not this stage's job.
    const bool exists=s.c69==static_cast<uint32_t>(p.ring.ringMc) &&
                      s.c70==static_cast<uint32_t>(p.ring.ringMc>>32u) &&
                      s.c71==rg::kRingBytes &&
                      (s.c67%(rg::kFrameBytes/4u))==0u && s.c67<rg::kRingBytes/4u;
    if(!exists) {p.state=N33_PSPL_RING_MISSING;return p;}
    p.ring.resume=true;
    p.stagingTotal=fw::stagingTotal(list);
    if(p.stagingTotal>kStageBytes) {p.state=N33_PSPL_NO_WINDOW;return p;}
    p.state=N33_PSPL_PLAN_OK;
    return p;
}

struct Outcome {
    rg::Outcome r;                 // state, step, writes, fence, status ... (rg::submit fills it)
    uint32_t recordsDone=0u, lastRecord=0u;
    uint64_t lastFwAddr=0u;        // resp.fw_addr of the last LOAD_IP_FW
    bool autoloadSent=false;
};
// Writes the image of record i into its staging slot (zeroing the whole slot
// first) and reads it back. The slot lies inside [kStageOff, kStageOff+kStageBytes).
inline bool stage(const rg::Hw &hw,const Plan &p,const fw::Images &in,uint32_t i) {
    const fw::Record &r=p.list.rec[i];
    const uint64_t slot=kStageOff+fw::stagingOffset(p.list,i);
    const fw::Image &im=in.f[static_cast<size_t>(r.file)];
    if(slot+fw::stagedBytes(r)>kStageOff+kStageBytes ||
       static_cast<uint64_t>(r.offset)+r.size>im.size)return false;
    const uint8_t *src=im.data+r.offset;
    return hw.vramZero(hw.ctx,static_cast<uint32_t>(slot),fw::stagedBytes(r)) &&
           hw.vramWrite(hw.ctx,static_cast<uint32_t>(slot),src,r.size) &&
           rg::vramEquals(hw,static_cast<uint32_t>(slot),src,r.size);
}
inline Outcome run(const Plan &p,const rg::Regs &regs,const fw::Images &in,
                   const rg::Hw &hw) {
    Outcome o{};
    if(p.state!=N33_PSPL_PLAN_OK || !regs.valid || !hw.read32 || !hw.write32 ||
       !hw.vramZero || !hw.vramWrite || !hw.vramRead || !hw.sleepMs ||
       !p.ring.win.fwPriMc || (p.ring.win.fwPriMc&(pl::kFwPriBytes-1u)))
        return o;
    // The ring pages start clean, as in the ring stage; the ring keeps working
    // because the PSP only follows the write pointer.
    o.r.wroteVram=true;
    if(!hw.vramZero(hw.ctx,rg::kRingOff,rg::kPagesBytes) ||
       !rg::vramEquals(hw,rg::kRingOff,nullptr,rg::kPagesBytes)) {
        o.r.state=N33_PSPL_STAGE_VERIFY_FAILED;return o;
    }
    uint8_t cmd[rg::kCmdBytes];
    const rg::Plan &rp=p.ring;
    for(uint32_t i=firstRecord(p);i<endRecord(p);++i) {
        const fw::Record &rec=p.list.rec[i];
        o.r.step=N33_PSPL_STEP_FW_LOAD;
        if(!stage(hw,p,in,i)) {o.r.state=N33_PSPL_STAGE_VERIFY_FAILED;return o;}
        const uint64_t mc=p.stageMc+fw::stagingOffset(p.list,i);
        if(!rg::prepLoadIpFw(cmd,mc,rec.size,rec.type)) {
            o.r.state=N33_PSPL_BAD_REQUEST;return o;
        }
        if(!rg::submit(regs,hw,rp,o.r,cmd))return o;
        o.lastRecord=i;
        o.lastFwAddr=(static_cast<uint64_t>(o.r.fwAddrHi)<<32u)|o.r.fwAddrLo;
        o.r.flags|=N33_PSPR_FLAG_FW_LOADED;
        ++o.recordsDone;++o.r.stepsDone;
    }
    if(wantsAutoload(p)) {
        o.r.step=N33_PSPL_STEP_AUTOLOAD;
        rg::prepAutoloadRlc(cmd);
        if(!rg::submit(regs,hw,rp,o.r,cmd))return o;
        o.autoloadSent=true;
        o.r.flags|=N33_PSPR_FLAG_AUTOLOAD_SENT;++o.r.stepsDone;
    }
    o.r.state=N33_PSPL_FW_DONE;
    return o;
}

inline void report(n33_psp_load_result &out,const rg::Snapshot &s,const Plan &p) {
    rg::report(out,s,p.ring);
    out.fw_record_count=p.list.count;
    out.fw_last_record=p.start;
    out.toc_bytes=0u;out.tmr_mc=0u;           // not used by this stage
    out.fw_tmr_addr=p.stageMc;                 // in a plan: where the staging area is
}
inline void report(n33_psp_load_result &out,const Outcome &o) {
    rg::report(out,o.r);
    out.fw_last_record=o.lastRecord;
    out.fw_tmr_addr=o.lastFwAddr;
    out.steps_done=o.recordsDone+(o.autoloadSent ? 1u : 0u);
}
} } // namespace navi33::psp13fwload
