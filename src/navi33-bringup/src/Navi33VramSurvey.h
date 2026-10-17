// SPDX-License-Identifier: MIT
// CPU-only merger: two independent read-only physical GPU surveys.
// MMHUB/GFXHUB paired FB layout and NBIO4.3 real VRAM capacity must
// agree before any address range is labelled geometrically verified.
// Does NOT grant VRAM ownership, a BAR0 pointer, or GPU submission.
#pragma once
#include "Navi33Nbio43Capacity.h"
#include "Navi33Gmc11VramLive.h"

namespace navi33 { namespace vram_survey {
inline n33_vram_geometry_status combine(n33_vram_geometry_status r,
                                          const nbio4::Sample &capacity) {
    // Enum values intentionally parallel the v6 userspace ABI; no
    // speculative GPU/MC capacity may be inferred from BAR0 length.
    r.nbio_state=static_cast<uint32_t>(capacity.code);
    r.nbio_revision=capacity.version;
    r.nbio_reads=capacity.readCount;
    if(capacity.code!=nbio4::Code::Pass)return r;
    r.nbio_memsize_mib=capacity.capacityMiB;
    r.flags|=N33_VRAM_FLAG_CAPACITY_VALID;
    if(r.state!=N33_VRAM_PAIRED_READ || !(r.flags&N33_VRAM_FLAG_BAR0_ASSIGNED))
        return r;

    const vram_geometry::Hub mm={r.mm_fb_base,r.mm_fb_top,r.mm_fb_offset};
    const vram_geometry::Hub gc={r.gfx_fb_base,r.gfx_fb_top,r.gfx_fb_offset};
    vram_geometry::PhysicalInventory inventory{};
    inventory.independentlyVerifiedMemsize=true;
    inventory.realBytes=static_cast<uint64_t>(capacity.capacityMiB) *
                        nbio4::kMiB;
    inventory.assignedBar0Bytes=static_cast<uint64_t>(r.bar0_mib) *
                                nbio4::kMiB;
    const auto verified=vram_geometry::decode(mm,gc,inventory);
    if(verified.code!=vram_geometry::Code::Pass) {
        r.nbio_state=N33_NBIO_GEOMETRY_MISMATCH;
        return r;
    }
    r.verified_vram_bytes=verified.geometry.realBytes;
    r.verified_mc_start=verified.geometry.mcStart;
    r.flags|=N33_VRAM_FLAG_MC_GEOMETRY_VALID;
    return r;
}
static_assert(sizeof(n33_vram_geometry_status)==96,
              "Keep native and user-space survey ABI in lockstep");
} } // namespace navi33::vram_survey
