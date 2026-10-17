// SPDX-License-Identifier: MIT
// RX7600/GMC11 frame-buffer MC geometry, independent of a BAR mapping.
// Linux AMDGPU: gmc_v11_0_vram_gtt_location, amdgpu_gmc_vram_mc2pa,
// mmhub_v3_0_get_{fb_location,mc_fb_offset}, gfxhub_v3_0 equivalents.
//
// CPU-only arithmetic: NO MMIO reads/writes, PCI resource ownership,
// VRAM allocation, firmware changes, or GPU DMA are performed here.
#pragma once
#include <stdint.h>
#include <stddef.h>

namespace navi33 { namespace vram_geometry {
constexpr uint64_t kFbUnit = 1ull << 24u; // FB_LOCATION and FB_OFFSET: 16 MiB
constexpr uint64_t kPage = 4096ull;
constexpr uint64_t kGpuLimit = 1ull << 48u;
constexpr uint32_t kFbFieldMask = 0x00ffffffu;
struct Hub {
    uint32_t fbBase=0, fbTop=0, fbOffset=0;
};
struct PhysicalInventory {
    // NBIO-reported REAL VRAM capacity; never infer it from BAR0 size.
    uint64_t realBytes=0;
    // IOPCIFamily-assigned BAR0 length, NOT an MC-physical base.
    uint64_t assignedBar0Bytes=0;
    bool independentlyVerifiedMemsize=false;
};
enum class Code : uint8_t {
    BadInventory=0, InvalidHub, DisagreeingHubs, OutsideGpuAddress,
    InconsistentCapacity, Pass
};
struct Geometry {
    uint64_t mcStart=0;
    uint64_t mcWindowBytes=0;
    uint64_t physicalFbBase=0;
    uint64_t realBytes=0;
    uint64_t cpuVisibleBytes=0;
};
struct Report {
    Code code=Code::BadInventory;
    Geometry geometry{};
};
inline bool hubFieldsValid(const Hub &v) {
    return (v.fbBase & ~kFbFieldMask)==0u &&
           (v.fbTop & ~kFbFieldMask)==0u &&
           (v.fbOffset & ~kFbFieldMask)==0u &&
           v.fbTop>=v.fbBase;
}
inline Report decode(const Hub &mm,const Hub &gfx,
                     const PhysicalInventory &inv) {
    Report out{};
    if(!inv.independentlyVerifiedMemsize || !inv.realBytes ||
       !inv.assignedBar0Bytes || (inv.realBytes&(kPage-1u)) ||
       (inv.assignedBar0Bytes&(kPage-1u)))
        return out;
    if(!hubFieldsValid(mm) || !hubFieldsValid(gfx)) {
        out.code=Code::InvalidHub;return out;
    }
    if(mm.fbBase!=gfx.fbBase || mm.fbTop!=gfx.fbTop ||
       mm.fbOffset!=gfx.fbOffset) {
        out.code=Code::DisagreeingHubs;return out;
    }
    // TOP is inclusive in units of 16 MiB. Exclusive byte end fits 2^48.
    const uint64_t start=static_cast<uint64_t>(mm.fbBase)*kFbUnit;
    const uint64_t end=(static_cast<uint64_t>(mm.fbTop)+1ull)*kFbUnit;
    const uint64_t paBase=static_cast<uint64_t>(mm.fbOffset)*kFbUnit;
    if(end>kGpuLimit || end<=start || paBase>=kGpuLimit) {
        out.code=Code::OutsideGpuAddress;return out;
    }
    const uint64_t window=end-start;
    if(inv.realBytes>window || inv.realBytes>kGpuLimit-start ||
       inv.realBytes>kGpuLimit-paBase) {
        out.code=Code::InconsistentCapacity;return out;
    }
    out.geometry.mcStart=start;
    out.geometry.mcWindowBytes=window;
    out.geometry.physicalFbBase=paBase;
    out.geometry.realBytes=inv.realBytes;
    out.geometry.cpuVisibleBytes=inv.assignedBar0Bytes<inv.realBytes ?
                               inv.assignedBar0Bytes : inv.realBytes;
    out.code=Code::Pass;
    return out;
}
struct Location {
    bool valid=false;
    uint64_t vramOffset=0;
    uint64_t gpuMcAddress=0;
    uint64_t gpuPhysicalAddress=0;
    uint64_t bar0Offset=0;
    bool cpuBar0Visible=false;
};
inline Location locate(const Geometry &g,uint64_t offset,uint64_t bytes) {
    Location out{};
    if(!g.realBytes || !bytes || (offset&(kPage-1u)) ||
       (bytes&(kPage-1u)) || offset>g.realBytes ||
       bytes>g.realBytes-offset || g.mcStart>=kGpuLimit ||
       g.physicalFbBase>=kGpuLimit ||
       offset>kGpuLimit-g.mcStart ||
       offset>kGpuLimit-g.physicalFbBase ||
       bytes>kGpuLimit-g.mcStart-offset ||
       bytes>kGpuLimit-g.physicalFbBase-offset)
        return out;
    out.valid=true;
    out.vramOffset=offset;
    out.gpuMcAddress=g.mcStart+offset;
    // Equivalent to Linux: mc - vram_start + vram_base_offset.
    out.gpuPhysicalAddress=g.physicalFbBase+offset;
    out.bar0Offset=offset; // candidate BAR0 byte offset, NOT a CPU pointer
    out.cpuBar0Visible=(offset<=g.cpuVisibleBytes &&
                        bytes<=g.cpuVisibleBytes-offset);
    return out;
}
inline Location locateMc(const Geometry &g,uint64_t mc,uint64_t bytes) {
    if(mc<g.mcStart)return {};
    return locate(g,mc-g.mcStart,bytes);
}
} } // namespace navi33::vram_geometry
