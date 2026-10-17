// SPDX-License-Identifier: MIT
// Navi33 MMHUB3 framebuffer geometry observer. READ-ONLY BAR5 access.
// Verified against Linux amdgpu mmhub_v3_0.c / mmhub_v3_0_2.c and the
// corresponding AMD mmhub_3_0_{0,2}_{offset,sh_mask}.h headers.
// This reads address WINDOWS. It does not allocate VRAM or prove BAR0
// ownership, VRAM size, GART availability, or GPU command capability.
#pragma once
#include "Navi33HardwarePlan.h"
#include "Navi33NativeABI.h"
#include <stddef.h>
#include <stdint.h>

namespace navi33 { namespace vram_census {
constexpr uint32_t kMmhub300Regs[3] = {0x08ecu, 0x08edu, 0x08d7u};
constexpr uint32_t kMmhub302Regs[3] = {0x086cu, 0x086du, 0x0857u};
constexpr uint32_t kFbFieldMask = 0x00ffffffu;
constexpr uint64_t kFbUnitBytes = 1ull << 24u;
constexpr uint64_t kMaxMcAddress = 1ull << 48u;
constexpr uint64_t kMaxNavi33Window = 64ull << 30u;
static_assert(kFbUnitBytes == 16ull * 1024ull * 1024ull,
              "MMHUB3 framebuffer registers are in 16MiB units");

struct Plan {
    n33_vram_state state=N33_VRAM_NO_INVENTORY;
    uint8_t revision=0xffu;
    uint64_t offsets[3]{};
    uint64_t bar5Bytes=0u;
};
inline Plan makePlan(const IPDiscovery &disc,uint64_t bar5Bytes) {
    Plan p{};
    p.bar5Bytes=bar5Bytes;
    if(inventory(disc).code!=InventoryCode::Pass ||
       !disc.isNavi33Candidate())return p;
    IPEntry mm{};
    if(!disc.find(ipid::MMHUB,0u,mm) || !mm.numBases)
        return p;
    if(mm.major!=3u || mm.minor!=0u ||
       (mm.revision!=0u && mm.revision!=2u)) {
        p.state=N33_VRAM_UNSUPPORTED_IP;
        return p;
    }
    p.revision=mm.revision;
    const uint32_t *regs=mm.revision==2u ?
        kMmhub302Regs:kMmhub300Regs;
    if(!bar5Bytes || bar5Bytes<4u) {
        p.state=N33_VRAM_NO_BAR5;return p;
    }
    const uint64_t base=mm.bases[0u];
    const uint64_t limit=(bar5Bytes-4u)/4u;
    if(!base || base==UINT64_MAX || base==UINT32_MAX) {
        p.state=N33_VRAM_BAD_OFFSETS;return p;
    }
    for(size_t i=0;i<3u;++i) {
        if(base>limit || regs[i]>limit-base) {
            p.state=N33_VRAM_BAD_OFFSETS;return p;
        }
        p.offsets[i]=(base+regs[i])*4u;
        for(size_t j=0;j<i;++j)
            if(p.offsets[i]==p.offsets[j]) {
                p.state=N33_VRAM_BAD_OFFSETS;return p;
            }
    }
    p.state=N33_VRAM_OBSERVED; // valid plan; no hardware observed yet
    return p;
}
using Read=bool (*)(void*,uint64_t,uint32_t*);
struct Observation {
    n33_vram_state state=N33_VRAM_DISABLED;
    uint32_t reads=0u;
    uint64_t fbBase=0u,fbTopInclusive=0u,mcFbOffset=0u;
    uint64_t windowBytes=0u;
};
inline Observation observe(const Plan &p,bool enabled,Read read,void *ctx) {
    Observation out{};
    if(!enabled)return out;
    if(p.state!=N33_VRAM_OBSERVED) {
        out.state=p.state;return out;
    }
    if(!read) {out.state=N33_VRAM_READ_ERROR;return out;}
    uint32_t first[3]{},second[3]{};
    for(size_t i=0;i<3u;++i) {
        if(!read(ctx,p.offsets[i],&first[i])) {
            out.state=N33_VRAM_READ_ERROR;return out;
        }
        ++out.reads;
    }
    for(size_t i=0;i<3u;++i) {
        if(!read(ctx,p.offsets[i],&second[i])) {
            out.state=N33_VRAM_READ_ERROR;return out;
        }
        ++out.reads;
        if(first[i]!=second[i]) {
            out.state=N33_VRAM_UNSTABLE;return out;
        }
    }
    // Only 24 bits are defined, and an all-ones PCI read is invalid.
    for(uint32_t word:first) {
        if((word & ~kFbFieldMask)!=0u) {
            out.state=N33_VRAM_INVALID_WINDOW;return out;
        }
    }
    if(first[1]<first[0]) {
        out.state=N33_VRAM_INVALID_WINDOW;return out;
    }
    const uint64_t pages=uint64_t(first[1])-first[0]+1ull;
    const uint64_t span=pages*kFbUnitBytes;
    const uint64_t base=uint64_t(first[0])*kFbUnitBytes;
    const uint64_t off=uint64_t(first[2])*kFbUnitBytes;
    // FB location is a hardware address window, not confirmed physical
    // VRAM capacity. Fail closed on implausible/out-of-range values.
    if(span==0u || span>kMaxNavi33Window ||
       base>=kMaxMcAddress ||
       span>kMaxMcAddress-base ||
       off>=kMaxMcAddress ||
       span>kMaxMcAddress-off) {
        out.state=N33_VRAM_INVALID_WINDOW;return out;
    }
    out.fbBase=base;
    out.fbTopInclusive=base+span-1u;
    out.mcFbOffset=off;
    out.windowBytes=span;
    out.state=N33_VRAM_OBSERVED;
    return out;
}
inline n33_vram_geometry report(const Observation &o,uint64_t bar0Bytes) {
    n33_vram_geometry r{};
    r.abi_version=N33_ABI_VERSION;r.size=sizeof(r);
    r.state=o.state;
    if(o.state==N33_VRAM_OBSERVED) {
        r.flags=N33_VRAM_FLAG_MMIO_STABLE;
        r.fb_base_bytes=o.fbBase;
        r.fb_top_inclusive_bytes=o.fbTopInclusive;
        r.mc_fb_offset_bytes=o.mcFbOffset;
        r.fb_window_bytes=o.windowBytes;
        // Assigned BAR0 size is *not* a free pool or a VRAM BO mapping.
        r.assigned_bar0_bytes=bar0Bytes;
        if(bar0Bytes)r.flags|=N33_VRAM_FLAG_BAR0_PRESENT;
    }
    return r;
}
} } // namespace navi33::vram_census
