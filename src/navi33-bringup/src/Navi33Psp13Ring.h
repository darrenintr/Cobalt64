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
} } // namespace navi33::psp13ring
