// SPDX-License-Identifier: MIT
// Offline GFX11 MEC (compute micro-engine) enable word for the RS64 firmware
// path. Mirrors gfx_v11_0_cp_compute_enable() in upstream amdgpu gfx_v11_0.c
// for gc_11_0_0. No register access here: the caller owns every read and
// write, and this header is not a stage and not hardware evidence.
#pragma once
#include <stdint.h>

namespace navi33 { namespace gfx11_mec {

// gc_11_0_0_offset.h: regCP_MEC_RS64_CNTL = 0x2904, BASE_IDX 1 (GC seg 1).
// Matches the N33N peek window "CP_MEC_RS64_CNTL" in Navi33PeekWindows.h.
constexpr uint32_t kCpMecRs64CntlDword=0x2904u;
constexpr uint32_t kCpMecRs64CntlSegment=1u;

// gc_11_0_0_sh_mask.h, CP_MEC_RS64_CNTL__*_MASK.
constexpr uint32_t kMecInvalidateIcache=0x00000010u;
constexpr uint32_t kMecPipeResetAll=0x000f0000u;   // PIPE0..3_RESET
constexpr uint32_t kMecPipeActiveAll=0x3c000000u;  // PIPE0..3_ACTIVE
constexpr uint32_t kMecHalt=0x40000000u;

// Field positions the masks above must match (sh_mask __SHIFT values).
constexpr uint32_t kShiftInvalidateIcache=4u;
constexpr uint32_t kShiftPipe0Reset=16u;
constexpr uint32_t kShiftPipe0Active=26u;
constexpr uint32_t kShiftHalt=30u;
static_assert((1u<<kShiftInvalidateIcache)==kMecInvalidateIcache,
              "MEC_INVALIDATE_ICACHE mask/shift mismatch");
static_assert((0xfu<<kShiftPipe0Reset)==kMecPipeResetAll,
              "MEC_PIPE*_RESET mask/shift mismatch");
static_assert((0xfu<<kShiftPipe0Active)==kMecPipeActiveAll,
              "MEC_PIPE*_ACTIVE mask/shift mismatch");
static_assert((1u<<kShiftHalt)==kMecHalt, "MEC_HALT mask/shift mismatch");

// The value gfx_v11_0_cp_compute_enable() writes to CP_MEC_RS64_CNTL, given
// the value read first. Upstream's REG_SET_FIELD keeps every bit not named
// here, so only the fields it touches are replaced.
//   enable:  ICACHE 0, PIPE resets 0, PIPE active 1, HALT 0
//   disable: ICACHE 1, PIPE resets 1, PIPE active 0, HALT 1
constexpr uint32_t rs64CntlWord(uint32_t current,bool enable) {
    const uint32_t touched=kMecInvalidateIcache|kMecPipeResetAll|
                           kMecPipeActiveAll|kMecHalt;
    const uint32_t kept=current&~touched;
    return enable ? (kept|kMecPipeActiveAll)
                  : (kept|kMecInvalidateIcache|kMecPipeResetAll|kMecHalt);
}

} } // namespace navi33::gfx11_mec
