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
constexpr uint64_t kTmrAlignment=0x100000u;        // PSP_TMR_ALIGNMENT
constexpr uint64_t kTmrDefaultBytes=0x400000u;     // PSP_TMR_SIZE (non-Aldebaran)

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
// TMR "naturally aligned": the start is a multiple of the size (a power of
// two here) and of PSP_TMR_ALIGNMENT; the size is a multiple of 4 KiB.
inline bool tmrValid(uint64_t mc,uint64_t bytes) {
    return bytes && !(bytes&0xfffull) && !(bytes&(bytes-1u)) &&
           !(mc&(kTmrAlignment-1u)) && !(mc&(bytes-1u)) && mc+bytes>mc;
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
} } // namespace navi33::psp13ring
