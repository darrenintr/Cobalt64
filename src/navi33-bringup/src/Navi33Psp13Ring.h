// SPDX-License-Identifier: MIT
// PSP13 (MP0 13.0.7) kernel-mode (KM/GPCOM) ring: protocol only, host-tested.
// NOT wired into the kext and NOT approved to write the GPU yet (hard rule 2
// in AGENTS.md: the PSP sOS load approval covers that stage only). All I/O
// goes through callbacks, so nothing here can touch hardware by itself.
//
// Protocol: Linux psp_v13_0.c psp_v13_0_ring_create() (non-SR-IOV branch),
// ring_get_wptr/set_wptr and amdgpu_psp.c psp_ring_cmd_submit():
//   wait C2PMSG_64 & 0x8000FFFF == 0x80000000 (sOS ready for ring creation);
//   C2PMSG_69/70 = ring MC lo/hi, C2PMSG_71 = ring size,
//   C2PMSG_64 = PSP_RING_TYPE__KM << 16; mdelay(20);
//   wait C2PMSG_64 & 0x8000FFFF == 0x80000000 again.
// A frame is a 64 byte psp_gfx_rb_frame; wptr (C2PMSG_67) counts dwords.
#pragma once
#include "Navi33NativeABI.h"
#include "Navi33Psp13Loader.h"
#include <stddef.h>
#include <stdint.h>

namespace navi33 { namespace psp13ring {
// mp_13_0_2_offset.h, MP0 BASE_IDX 0 (checked 2026-10-18).
constexpr uint32_t kC2pmsg64=0x0080u; // ring command / sOS ready
constexpr uint32_t kC2pmsg67=0x0083u; // ring write pointer (dwords)
constexpr uint32_t kC2pmsg69=0x0085u; // ring MC low
constexpr uint32_t kC2pmsg70=0x0086u; // ring MC high
constexpr uint32_t kC2pmsg71=0x0087u; // ring size (bytes)
// The only registers a ring writer may touch.
constexpr uint32_t kWritableRegisters[5]={kC2pmsg64,kC2pmsg67,kC2pmsg69,
                                          kC2pmsg70,kC2pmsg71};
// psp_gfx_if.h / amdgpu_psp.h.
constexpr uint32_t kRespFlag=0x80000000u;         // GFX_FLAG_RESPONSE
constexpr uint32_t kRespMask=0x8000ffffu;         // RESPONSE | STATUS masks
constexpr uint32_t kRingTypeKm=2u;                // PSP_RING_TYPE__KM
constexpr uint32_t kCtrlDestroyRings=0x00030000u; // GFX_CTRL_CMD_ID_DESTROY_RINGS
constexpr uint32_t kRingBytes=0x1000u;            // psp_ring_init()
constexpr uint32_t kFrameBytes=64u;               // sizeof(psp_gfx_rb_frame)
constexpr uint32_t kCreateSettleMs=20u;           // mdelay(20)

struct Frame {                                     // psp_gfx_rb_frame, 64 B
    uint32_t cmd_buf_lo, cmd_buf_hi, cmd_buf_size;
    uint32_t fence_lo, fence_hi, fence_value;
    uint32_t sid_lo, sid_hi;
    uint8_t vmid, frame_type, reserved1[2];
    uint32_t reserved2[7];
};
static_assert(sizeof(Frame)==kFrameBytes,"psp_gfx_rb_frame is 64 bytes");

// Ring buffer in the card's memory; the caller owns it. 4 KiB aligned and
// kRingBytes long, so the size field and the wrap arithmetic below hold.
struct Ring { uint64_t mc=0u; uint32_t bytes=0u; };
inline bool ringValid(const Ring &r) {
    return r.bytes==kRingBytes && r.mc && !(r.mc&0xfffull) &&
           r.mc+r.bytes>r.mc;
}
// Register values for creation, in the order they are written.
struct CreatePlan { uint32_t r69, r70, r71, r64; bool ok=false; };
inline CreatePlan planCreate(const Ring &r) {
    CreatePlan p{};
    if(!ringValid(r))return p;
    p.r69=static_cast<uint32_t>(r.mc);
    p.r70=static_cast<uint32_t>(r.mc>>32u);
    p.r71=r.bytes;
    p.r64=kRingTypeKm<<16u;
    p.ok=true;
    return p;
}
inline bool respOk(uint32_t v) { return (v&kRespMask)==kRespFlag; }

// Next frame slot from the write pointer register (dwords), or false if the
// pointer is not frame-aligned or points past the ring. Upstream wraps at the
// ring size and checks that the frame lies inside the buffer.
struct Slot { uint32_t offsetBytes=0u; uint32_t nextWptr=0u; };
inline bool nextSlot(uint32_t wptrDwords,uint32_t ringBytes,Slot &out) {
    const uint32_t ringDw=ringBytes/4u, frameDw=kFrameBytes/4u;
    if(ringBytes!=kRingBytes || (wptrDwords%frameDw)!=0u || wptrDwords>=ringDw)
        return false;
    out.offsetBytes=wptrDwords*4u;
    out.nextWptr=(wptrDwords+frameDw)%ringDw;
    return true;
}
inline Frame makeFrame(uint64_t cmdMc,uint32_t cmdBytes,uint64_t fenceMc,
                       uint32_t fenceValue) {
    Frame f{};
    f.cmd_buf_lo=static_cast<uint32_t>(cmdMc);
    f.cmd_buf_hi=static_cast<uint32_t>(cmdMc>>32u);
    f.cmd_buf_size=cmdBytes;
    f.fence_lo=static_cast<uint32_t>(fenceMc);
    f.fence_hi=static_cast<uint32_t>(fenceMc>>32u);
    f.fence_value=fenceValue;
    return f;
}

// --- Command buffer (psp_gfx_cmd_resp, 1024 B) ---------------------------
// Kept as raw offsets: the command union's size depends on members this
// kext never builds. Linux zeroes the buffer and fills only cmd_id and the
// command body; buf_size and buf_version stay 0 for ring (GPCOM) commands.
constexpr uint32_t kCmdBytes=1024u;               // PSP_CMD_BUFFER_SIZE is 4 KiB
constexpr uint32_t kCmdIdOff=8u, kCmdBodyOff=28u;
constexpr uint32_t kRespOff=864u;                  // psp_gfx_resp, 96 B
constexpr uint32_t kRespStatusOff=kRespOff+0u;
constexpr uint32_t kRespTmrSizeOff=kRespOff+16u;
constexpr uint32_t kCmdSetupTmr=0x00000005u;       // GFX_CMD_ID_SETUP_TMR
constexpr uint32_t kCmdLoadToc=0x00000020u;        // GFX_CMD_ID_LOAD_TOC
constexpr uint32_t kCmdLoadIpFw=0x00000006u;       // GFX_CMD_ID_LOAD_IP_FW
constexpr uint32_t kCmdAutoloadRlc=0x00000021u;    // GFX_CMD_ID_AUTOLOAD_RLC
constexpr uint32_t kRespFwAddrLoOff=kRespOff+8u;   // resp.fw_addr_lo/hi (LOAD_IP_FW)
constexpr uint64_t kTmrAlignment=0x100000u;        // PSP_TMR_ALIGNMENT
// MP0 13.0.7 has boot_time_tmr = false, so Linux runs LOAD_TOC, takes the
// TMR size from resp.tmr_size (this card, 2026-10-18 journal: 0x1300000 =
// 19 MiB, placed at MC 0x81fc000000 = FB base + 8128 MiB) and SETUP_TMR with
// the buffer. PSP_TMR_SIZE (4 MiB) is only the fallback without a TOC.
constexpr uint64_t kTmrVramOffset=8128ull<<20u;
constexpr uint64_t kTmrMaxBytes=32ull<<20u;        // refuse anything larger

inline void put32(uint8_t *buf,uint32_t off,uint32_t v) {
    for(unsigned i=0;i<4u;++i)buf[off+i]=static_cast<uint8_t>(v>>(8u*i));
}
inline uint32_t get32(const uint8_t *buf,uint32_t off) {
    return static_cast<uint32_t>(buf[off])|static_cast<uint32_t>(buf[off+1])<<8u|
           static_cast<uint32_t>(buf[off+2])<<16u|
           static_cast<uint32_t>(buf[off+3])<<24u;
}
inline void clear(uint8_t (&buf)[kCmdBytes]) {
    for(uint8_t &b:buf)b=0u;
}
// psp_prep_load_toc_cmd_buf(): toc address (the fw_pri MC address), size.
inline bool prepLoadToc(uint8_t (&buf)[kCmdBytes],uint64_t fwPriMc,
                        uint32_t tocBytes) {
    if(!tocBytes || (fwPriMc&0xfffull))return false;
    clear(buf);
    put32(buf,kCmdIdOff,kCmdLoadToc);
    put32(buf,kCmdBodyOff+0u,static_cast<uint32_t>(fwPriMc));
    put32(buf,kCmdBodyOff+4u,static_cast<uint32_t>(fwPriMc>>32u));
    put32(buf,kCmdBodyOff+8u,tocBytes);
    return true;
}
// psp_prep_load_ip_fw_cmd_buf(): body = {fw address lo, hi, size, fw_type}.
// The address is the MC address of the staged copy (4 KiB aligned).
inline bool prepLoadIpFw(uint8_t (&buf)[kCmdBytes],uint64_t stageMc,uint32_t bytes,
                         uint32_t fwType) {
    if(!bytes || !fwType || (stageMc&0xfffull))return false;
    clear(buf);
    put32(buf,kCmdIdOff,kCmdLoadIpFw);
    put32(buf,kCmdBodyOff+0u,static_cast<uint32_t>(stageMc));
    put32(buf,kCmdBodyOff+4u,static_cast<uint32_t>(stageMc>>32u));
    put32(buf,kCmdBodyOff+8u,bytes);
    put32(buf,kCmdBodyOff+12u,fwType);
    return true;
}
// psp_rlc_autoload_start(): no body, just the command id.
inline void prepAutoloadRlc(uint8_t (&buf)[kCmdBytes]) {
    clear(buf);
    put32(buf,kCmdIdOff,kCmdAutoloadRlc);
}
// Size: non-zero 4 KiB multiple, at most kTmrMaxBytes. Start: PSP_TMR_ALIGNMENT.
// (The TMR is not a power of two: 19 MiB on this card.)
inline bool tmrValid(uint64_t mc,uint64_t bytes) {
    return bytes && !(bytes&0xfffull) && bytes<=kTmrMaxBytes &&
           !(mc&(kTmrAlignment-1u)) && mc+bytes>mc;
}
// psp_prep_tmr_cmd_buf(), non-SR-IOV: virt_phy_addr = 1 (bit 1 of tmr_flags),
// and the "system physical" address is the VRAM physical address (the
// TMR's offset in the aperture, filled by the caller).
inline bool prepSetupTmr(uint8_t (&buf)[kCmdBytes],uint64_t tmrMc,
                         uint64_t tmrBytes,uint64_t tmrVramPa) {
    if(!tmrValid(tmrMc,tmrBytes) || tmrBytes>0xffffffffull)return false;
    clear(buf);
    put32(buf,kCmdIdOff,kCmdSetupTmr);
    put32(buf,kCmdBodyOff+0u,static_cast<uint32_t>(tmrMc));
    put32(buf,kCmdBodyOff+4u,static_cast<uint32_t>(tmrMc>>32u));
    put32(buf,kCmdBodyOff+8u,static_cast<uint32_t>(tmrBytes));
    put32(buf,kCmdBodyOff+12u,2u);                  // tmr_flags: virt_phy_addr
    put32(buf,kCmdBodyOff+16u,static_cast<uint32_t>(tmrVramPa));
    put32(buf,kCmdBodyOff+20u,static_cast<uint32_t>(tmrVramPa>>32u));
    return true;
}
// The fence the PSP writes when the frame is done must equal the frame's
// fence_value (the running index); the response status must then be 0.
inline bool fenceDone(uint32_t fence,uint32_t index) { return fence==index; }
inline bool cmdOk(const uint8_t *buf) { return get32(buf,kRespStatusOff)==0u; }

// --- Memory layout (offsets in VRAM; all inside BAR0 except the TMR) -------
// [fw_pri, +1 MiB)      TOC copy (psp_copy_fw), as in the sOS load
// [fw_pri+1 MiB, +4 KiB) ring   |  +4 KiB cmd buffer  |  +8 KiB fence word
// Only the 2 MiB window is ever mapped writable. The TMR is written by the
// PSP alone; we only send its address.
constexpr uint64_t kWindowBytes=2u*psp13load::kFwPriBytes;
constexpr uint32_t kRingOff=static_cast<uint32_t>(psp13load::kFwPriBytes);
constexpr uint32_t kCmdOff=kRingOff+0x1000u;
constexpr uint32_t kFenceOff=kRingOff+0x2000u;
constexpr uint32_t kPagesBytes=0x3000u;
constexpr uint32_t kCmdWaitMs=2000u;               // Linux: psp_timeout polls
constexpr uint32_t kRingWaitMs=2000u;

// BAR5 byte offsets, resolved through checksum-valid IP Discovery.
struct Regs {
    bool valid=false;
    uint64_t c64=0u, c67=0u, c69=0u, c70=0u, c71=0u;
};
inline Regs resolve(const IPDiscovery &disc,uint64_t bar5Bytes) {
    Regs r{};
    const uint32_t reg[5]={kC2pmsg64,kC2pmsg67,kC2pmsg69,kC2pmsg70,kC2pmsg71};
    uint64_t *out[5]={&r.c64,&r.c67,&r.c69,&r.c70,&r.c71};
    for(unsigned i=0;i<5u;++i) {
        const RegisterAddress a=resolveDword(disc,Engine::PSP13,0u,reg[i],bar5Bytes);
        if(a.code!=AddressCode::Pass)return Regs{};
        *out[i]=a.bar5ByteOffset;
    }
    r.valid=true;
    return r;
}
struct Snapshot {
    psp13load::Snapshot base;
    uint32_t c64=0u, c67=0u, c69=0u, c70=0u, c71=0u;
};
// The ring registers may legitimately read zero, so only all-ones is a
// failed read.
inline bool readSnapshot(const psp13load::Registers &pr,const Regs &r,
                         psp13load::Read read,void *ctx,Snapshot &s) {
    s=Snapshot{};
    if(!r.valid || !read || !psp13load::readSnapshot(pr,read,ctx,s.base))
        return false;
    struct Item { uint64_t off; uint32_t *dst; };
    const Item items[]={{r.c64,&s.c64},{r.c67,&s.c67},{r.c69,&s.c69},
                        {r.c70,&s.c70},{r.c71,&s.c71}};
    for(const Item &it:items)
        if(!read(ctx,it.off,it.dst) || *it.dst==UINT32_MAX)return false;
    return true;
}

struct Plan {
    n33_psp_load_state state=N33_PSPL_BAD_REQUEST;
    psp13load::Plan win;           // fw_pri window and the console/aperture proof
    bool resume=false;             // ring already exists at our address
    uint32_t stopAfter=0u;
    uint64_t ringMc=0u, cmdMc=0u, fenceMc=0u, tmrMc=0u;
};
inline Plan plan(const Snapshot &s,const psp13load::Range &console,
                 const psp13load::Range &bar0,uint32_t stopAfter) {
    Plan p{};
    if(stopAfter<N33_PSP_RING_STOP_RING || stopAfter>N33_PSP_RING_STOP_TMR)return p;
    p.stopAfter=stopAfter;
    if(!s.base.c2p81) {p.state=N33_PSPL_SOS_NOT_ALIVE;return p;}
    if(!respOk(s.c64)) {p.state=N33_PSPL_RING_NOT_READY;return p;}
    p.win=psp13load::planWindow(s.base,console,bar0,true,kWindowBytes);
    if(p.win.state!=N33_PSPL_PLAN_OK) {p.state=p.win.state;return p;}
    p.ringMc=p.win.fwPriMc+kRingOff;
    p.cmdMc=p.win.fwPriMc+kCmdOff;
    p.fenceMc=p.win.fwPriMc+kFenceOff;
    p.tmrMc=p.win.fbBaseMc+kTmrVramOffset;
    if(p.tmrMc+kTmrMaxBytes-1u>p.win.fbTopMc) {p.state=N33_PSPL_NO_WINDOW;return p;}
    const bool fresh=!s.c67 && !s.c69 && !s.c70 && !s.c71;
    const bool exists=s.c69==static_cast<uint32_t>(p.ringMc) &&
                      s.c70==static_cast<uint32_t>(p.ringMc>>32u) &&
                      s.c71==kRingBytes &&
                      (s.c67%(kFrameBytes/4u))==0u && s.c67<kRingBytes/4u;
    if(!fresh && !exists) {p.state=N33_PSPL_RING_REGS_UNEXPECTED;return p;}
    p.resume=exists;
    p.state=N33_PSPL_PLAN_OK;
    return p;
}

struct Hw {
    void *ctx=nullptr;
    psp13load::Read read32=nullptr;
    bool (*write32)(void *ctx,uint64_t bar5Offset,uint32_t value)=nullptr;
    // Offsets are relative to the start of the 2 MiB window.
    bool (*vramZero)(void *ctx,uint32_t off,uint32_t len)=nullptr;
    bool (*vramWrite)(void *ctx,uint32_t off,const uint8_t *src,uint32_t len)=nullptr;
    bool (*vramRead)(void *ctx,uint32_t off,uint8_t *dst,uint32_t len)=nullptr;
    void (*sleepMs)(void *ctx,uint32_t ms)=nullptr;
};
struct Outcome {
    n33_psp_load_state state=N33_PSPL_BAD_REQUEST;
    n33_psp_load_step step=N33_PSPL_STEP_NONE;
    uint32_t stepsDone=0u, writes=0u, elapsedMs=0u;
    uint32_t c64Last=0u, c67Last=0u, cmdStatus=0u, fenceSeen=0u;
    uint32_t tocBytes=0u, tmrBytes=0u, flags=0u, fenceIndex=0u;
    uint32_t fwAddrLo=0u, fwAddrHi=0u;   // resp.fw_addr of the last command
    bool wroteVram=false;
};
inline bool vramEquals(const Hw &hw,uint32_t off,const uint8_t *want,uint32_t len) {
    uint8_t chunk[256];
    for(uint32_t at=0;at<len;at+=sizeof(chunk)) {
        const uint32_t n=len-at<sizeof(chunk) ? len-at : sizeof(chunk);
        if(!hw.vramRead(hw.ctx,off+at,chunk,n))return false;
        for(uint32_t i=0;i<n;++i)
            if(chunk[i]!=(want ? want[at+i] : 0u))return false;
    }
    return true;
}
// Poll C2PMSG_64 for the response flag, as psp_wait_for(MBOX_TOS_RESP_*).
inline bool waitResp(const Regs &r,const Hw &hw,Outcome &o,uint32_t ms) {
    for(uint32_t t=0;;++t) {
        uint32_t v=0u;
        if(!hw.read32(hw.ctx,r.c64,&v) || v==UINT32_MAX)return false;
        o.c64Last=v;
        if(respOk(v))return true;
        if(t>=ms)return false;
        hw.sleepMs(hw.ctx,1u);++o.elapsedMs;
    }
}
// psp_cmd_submit_buf(): command buffer, frame, write pointer, then the fence.
// The command stays in the cmd page afterwards so its response can be read.
inline bool submit(const Regs &r,const Hw &hw,const Plan &p,Outcome &o,
                   const uint8_t (&cmd)[kCmdBytes]) {
    uint32_t wptr=0u;
    if(!hw.read32(hw.ctx,r.c67,&wptr) || wptr==UINT32_MAX) {
        o.state=N33_PSPL_READ_FAILED;return false;
    }
    Slot slot{};
    if(!nextSlot(wptr,kRingBytes,slot)) {
        o.state=N33_PSPL_RING_REGS_UNEXPECTED;return false;
    }
    ++o.fenceIndex;
    const Frame f=makeFrame(p.cmdMc,kCmdBytes,p.fenceMc,o.fenceIndex);
    uint8_t fb[kFrameBytes];
    const uint8_t *src=reinterpret_cast<const uint8_t *>(&f);
    for(uint32_t i=0;i<kFrameBytes;++i)fb[i]=src[i];
    const uint8_t zero4[4]={0,0,0,0};
    o.wroteVram=true;
    if(!hw.vramZero(hw.ctx,kCmdOff,0x1000u) ||
       !hw.vramWrite(hw.ctx,kCmdOff,cmd,kCmdBytes) ||
       !vramEquals(hw,kCmdOff,cmd,kCmdBytes) ||
       !hw.vramWrite(hw.ctx,kFenceOff,zero4,4u) ||
       !hw.vramWrite(hw.ctx,kRingOff+slot.offsetBytes,fb,kFrameBytes) ||
       !vramEquals(hw,kRingOff+slot.offsetBytes,fb,kFrameBytes)) {
        o.state=N33_PSPL_STAGE_VERIFY_FAILED;return false;
    }
    if(!hw.write32(hw.ctx,r.c67,slot.nextWptr)) {
        o.state=N33_PSPL_MAP_FAILED;return false;
    }
    ++o.writes;o.c67Last=slot.nextWptr;
    for(uint32_t t=0;;++t) {
        uint8_t w[4];
        if(!hw.vramRead(hw.ctx,kFenceOff,w,4u)) {
            o.state=N33_PSPL_READ_FAILED;return false;
        }
        o.fenceSeen=get32(w,0u);
        if(fenceDone(o.fenceSeen,o.fenceIndex))break;
        if(t>=kCmdWaitMs) {o.state=N33_PSPL_CMD_TIMEOUT;return false;}
        hw.sleepMs(hw.ctx,1u);++o.elapsedMs;
    }
    uint8_t resp[kCmdBytes];
    if(!hw.vramRead(hw.ctx,kCmdOff,resp,kCmdBytes)) {
        o.state=N33_PSPL_READ_FAILED;return false;
    }
    o.cmdStatus=get32(resp,kRespStatusOff);
    if(!cmdOk(resp)) {o.state=N33_PSPL_CMD_FAILED;return false;}
    o.tmrBytes=get32(resp,kRespTmrSizeOff);   // only meaningful after LOAD_TOC
    o.fwAddrLo=get32(resp,kRespFwAddrLoOff);  // only meaningful after LOAD_IP_FW
    o.fwAddrHi=get32(resp,kRespFwAddrLoOff+4u);
    return true;
}
inline Outcome run(const Plan &p,const Regs &r,const psp13load::Part &toc,
                   const Hw &hw) {
    Outcome o{};
    if(p.state!=N33_PSPL_PLAN_OK || !r.valid || !hw.read32 || !hw.write32 ||
       !hw.vramZero || !hw.vramWrite || !hw.vramRead || !hw.sleepMs ||
       !p.win.fwPriMc || (p.win.fwPriMc&(psp13load::kFwPriBytes-1u)) ||
       p.stopAfter<N33_PSP_RING_STOP_RING || p.stopAfter>N33_PSP_RING_STOP_TMR ||
       (p.stopAfter>=N33_PSP_RING_STOP_TOC && (!toc.data || !toc.size)))
        return o;
    // Fresh, verified-zero ring pages before the PSP is told about them.
    o.wroteVram=true;
    if(!hw.vramZero(hw.ctx,kRingOff,kPagesBytes) ||
       !vramEquals(hw,kRingOff,nullptr,kPagesBytes)) {
        o.state=N33_PSPL_STAGE_VERIFY_FAILED;return o;
    }
    o.step=N33_PSPL_STEP_RING_CREATE;
    if(p.resume) {
        o.flags|=N33_PSPR_FLAG_RESUMED;
    } else {
        const Ring ring{p.ringMc,kRingBytes};
        const CreatePlan c=planCreate(ring);
        if(!c.ok)return o;
        uint32_t v=0u;
        if(!hw.read32(hw.ctx,r.c64,&v) || v==UINT32_MAX) {
            o.state=N33_PSPL_READ_FAILED;return o;
        }
        o.c64Last=v;
        if(!respOk(v)) {o.state=N33_PSPL_RING_NOT_READY;return o;}
        // psp_v13_0_ring_create(): 69, 70, 71, then the command into 64.
        const struct { uint64_t reg; uint32_t val; } w[4]={
            {r.c69,c.r69},{r.c70,c.r70},{r.c71,c.r71},{r.c64,c.r64}};
        for(const auto &x:w) {
            if(!hw.write32(hw.ctx,x.reg,x.val)) {
                o.state=N33_PSPL_MAP_FAILED;return o;
            }
            ++o.writes;
        }
        hw.sleepMs(hw.ctx,kCreateSettleMs);o.elapsedMs+=kCreateSettleMs;
        if(!waitResp(r,hw,o,kRingWaitMs)) {
            o.state=N33_PSPL_RING_CREATE_TIMEOUT;return o;
        }
        o.flags|=N33_PSPR_FLAG_RING_CREATED;
    }
    ++o.stepsDone;
    if(p.stopAfter==N33_PSP_RING_STOP_RING) {o.state=N33_PSPL_RING_DONE;return o;}

    // LOAD_TOC: copy the TOC into fw_pri (zeroed first), then ask for the TMR size.
    o.step=N33_PSPL_STEP_LOAD_TOC;
    o.tocBytes=toc.size;
    uint8_t cmd[kCmdBytes];
    if(!prepLoadToc(cmd,p.win.fwPriMc,toc.size)) {o.state=N33_PSPL_TOC_BAD;return o;}
    if(!hw.vramZero(hw.ctx,0u,static_cast<uint32_t>(psp13load::kFwPriBytes)) ||
       !hw.vramWrite(hw.ctx,0u,toc.data,toc.size) ||
       !vramEquals(hw,0u,toc.data,toc.size)) {
        o.state=N33_PSPL_STAGE_VERIFY_FAILED;return o;
    }
    if(!submit(r,hw,p,o,cmd))return o;
    const uint32_t tmrBytes=o.tmrBytes;
    if(!tmrValid(p.tmrMc,tmrBytes)) {o.state=N33_PSPL_TOC_BAD;return o;}
    o.flags|=N33_PSPR_FLAG_TOC_LOADED;++o.stepsDone;
    if(p.stopAfter==N33_PSP_RING_STOP_TOC) {o.state=N33_PSPL_RING_DONE;return o;}

    // SETUP_TMR at FB base + 8128 MiB with the size the PSP asked for. The
    // "system physical" address of a dGPU buffer is its VRAM offset.
    o.step=N33_PSPL_STEP_SETUP_TMR;
    if(!prepSetupTmr(cmd,p.tmrMc,tmrBytes,p.tmrMc-p.win.fbBaseMc)) {
        o.state=N33_PSPL_TOC_BAD;return o;
    }
    if(!submit(r,hw,p,o,cmd))return o;
    o.tmrBytes=tmrBytes;
    o.flags|=N33_PSPR_FLAG_TMR_SET;++o.stepsDone;
    o.state=N33_PSPL_RING_DONE;
    return o;
}

// Result helpers (the load result struct carries both stages).
inline void report(n33_psp_load_result &out,const Snapshot &s,const Plan &p) {
    psp13load::report(out,s.base,p.win);
    out.fw_pri_vram_offset=p.win.fwPriOffset;
    out.ring_mc=p.ringMc;out.cmd_mc=p.cmdMc;out.fence_mc=p.fenceMc;
    out.tmr_mc=p.tmrMc;
    out.c2pmsg64_last=s.c64;out.c2pmsg67_last=s.c67;
    if(p.resume)out.ring_flags|=N33_PSPR_FLAG_RESUMED;
}
inline void report(n33_psp_load_result &out,const Outcome &o) {
    out.state=o.state;out.step=o.step;out.steps_done=o.stepsDone;
    out.register_writes=o.writes;out.elapsed_ms=o.elapsedMs;
    out.c2pmsg64_last=o.c64Last;out.c2pmsg67_last=o.c67Last;
    out.cmd_status=o.cmdStatus;out.fence_seen=o.fenceSeen;
    out.toc_bytes=o.tocBytes;out.tmr_bytes=o.tmrBytes;
    out.ring_flags|=o.flags;
    if(o.wroteVram)out.flags|=N33_PSPL_FLAG_WROTE_VRAM;
    if(o.writes)out.flags|=N33_PSPL_FLAG_WROTE_REGISTERS;
}
} } // namespace navi33::psp13ring
