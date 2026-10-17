// SPDX-License-Identifier: MIT
// Navi33 GFX1102/MMHUB3 framebuffer location observer. READ ONLY.
// Register DWORD indexes from Linux AMDGPU:
// gc_11_0_0_offset.h (GCMC FB_BASE=1678, TOP=1679, OFFSET=15a7);
// mmhub_3_0_0_offset.h (MMMC FB_BASE=08ec, TOP=08ed, OFFSET=08d7);
// mmhub_3_0_2_offset.h (MMMC FB_BASE=086c, TOP=086d, OFFSET=0857).
// IP Discovery base+reg uses Linux SOC15 BASE_IDX=0; *4 for BAR5 bytes.
// No MMIO write, DMA, BO allocation or flash operation exists here.
#pragma once
#include "Navi33HardwarePlan.h"
#include "Navi33NativeABI.h"
#include "Navi33Gmc11VramGeometry.h"
#include <stdint.h>
#include <stddef.h>

namespace navi33 { namespace vram_live {
enum class PlanCode : uint8_t {
    NoInventory=0, UnsupportedVersion, InvalidBar5, Pass
};
struct Plan {
    PlanCode code=PlanCode::NoInventory;
    uint64_t offsets[6u]{}; // MM BASE,TOP,OFFSET; GC BASE,TOP,OFFSET
    uint32_t mmRevision=0xffffffffu;
};
inline Plan fromDiscovery(const IPDiscovery &disc,uint64_t bar5Bytes) {
    Plan p{};
    if(inventory(disc).code!=InventoryCode::Pass ||
       !disc.isNavi33Candidate())return p;
    IPEntry mm{},gc{};
    if(!disc.find(ipid::MMHUB,0u,mm) ||
       !disc.find(ipid::GC,0u,gc))return p;
    if(mm.major!=3u || mm.minor!=0u ||
       (mm.revision!=0u && mm.revision!=2u) ||
       !mm.numBases || gc.major!=11u || gc.minor!=0u ||
       gc.revision!=2u || !gc.numBases) {
        p.code=PlanCode::UnsupportedVersion;return p;
    }
    const uint32_t mmRegs0[3u]={0x08ecu,0x08edu,0x08d7u};
    const uint32_t mmRegs2[3u]={0x086cu,0x086du,0x0857u};
    const uint32_t gcRegs[3u]={0x1678u,0x1679u,0x15a7u};
    const uint32_t *mmRegs=mm.revision==2u ? mmRegs2:mmRegs0;
    if(bar5Bytes<4u || !mm.bases[0u] || !gc.bases[0u] ||
       mm.bases[0u]==UINT64_MAX || gc.bases[0u]==UINT64_MAX ||
       mm.bases[0u]==UINT32_MAX || gc.bases[0u]==UINT32_MAX) {
        p.code=PlanCode::InvalidBar5;return p;
    }
    const uint64_t last=(bar5Bytes-4u)/4u;
    for(size_t i=0u;i<6u;++i) {
        const uint64_t base=i<3u?mm.bases[0u]:gc.bases[0u];
        const uint32_t reg=i<3u?mmRegs[i]:gcRegs[i-3u];
        if(base>last || reg>last-base) {
            p.code=PlanCode::InvalidBar5;return p;
        }
        p.offsets[i]=(base+reg)*4u;
        if(!p.offsets[i] || (p.offsets[i]&3u)) {
            p.code=PlanCode::InvalidBar5;return p;
        }
        for(size_t j=0u;j<i;++j)
            if(p.offsets[j]==p.offsets[i]) {
                p.code=PlanCode::InvalidBar5;return p;
            }
    }
    p.code=PlanCode::Pass;
    p.mmRevision=mm.revision;
    return p;
}
using Read=bool (*)(void*,uint64_t,uint32_t*);
struct Sample {
    uint32_t state=N33_VRAM_DISABLED;
    uint32_t flags=0u;
    uint32_t reads=0u;
    uint32_t raw[6u]{};
};
inline Sample inspect(const Plan &plan,Read read,void *context) {
    Sample out{};
    if(plan.code!=PlanCode::Pass) {
        out.state=N33_VRAM_BAD_OFFSETS;return out;
    }
    if(!read) {out.state=N33_VRAM_MMIO_FAILED;return out;}
    uint32_t first[6u]{},second[6u]{};
    for(size_t i=0u;i<6u;++i) {
        if(!read(context,plan.offsets[i],&first[i])) {
            out.state=N33_VRAM_MMIO_FAILED;return out;
        }
        ++out.reads;
        if(first[i]==UINT32_MAX) {
            out.state=N33_VRAM_MMIO_FAILED;return out;
        }
    }
    for(size_t i=0u;i<6u;++i) {
        if(!read(context,plan.offsets[i],&second[i])) {
            out.state=N33_VRAM_MMIO_FAILED;return out;
        }
        ++out.reads;
        if(first[i]!=second[i]) {
            out.state=N33_VRAM_UNSTABLE;return out;
        }
        out.raw[i]=second[i];
    }
    const vram_geometry::Hub mm{out.raw[0],out.raw[1],out.raw[2]};
    const vram_geometry::Hub gc{out.raw[3],out.raw[4],out.raw[5]};
    if(!vram_geometry::hubFieldsValid(mm) ||
       !vram_geometry::hubFieldsValid(gc) ||
       mm.fbBase!=gc.fbBase || mm.fbTop!=gc.fbTop ||
       mm.fbOffset!=gc.fbOffset) {
        out.state=N33_VRAM_DISAGREE;return out;
    }
    out.state=N33_VRAM_PAIRED_READ;
    out.flags=N33_VRAM_FLAG_PAIRED;
    return out;
}
inline n33_vram_geometry_status report(const Sample &s,uint64_t bar0Bytes,
                                        uint64_t bar5Bytes,uint32_t revision) {
    n33_vram_geometry_status r{};
    r.abi_version=N33_ABI_VERSION;
    r.size=sizeof(r);
    r.state=s.state;
    r.flags=s.flags;
    r.mm_fb_base=s.raw[0];r.mm_fb_top=s.raw[1];
    r.mm_fb_offset=s.raw[2];
    r.gfx_fb_base=s.raw[3];r.gfx_fb_top=s.raw[4];
    r.gfx_fb_offset=s.raw[5];
    r.bar0_mib=static_cast<uint32_t>(bar0Bytes/(1024u*1024u));
    r.bar5_kib=static_cast<uint32_t>(bar5Bytes/1024u);
    r.read_count=s.reads;
    r.mmhub_revision=revision;
    if(bar0Bytes)r.flags|=N33_VRAM_FLAG_BAR0_ASSIGNED;
    if(s.state==N33_VRAM_PAIRED_READ)
        r.candidate_physical_fb_base=
            static_cast<uint64_t>(s.raw[2])*vram_geometry::kFbUnit;
    // No NBIO physical capacity read: cannot assert actual VRAM size,
    // free GPU BO locations, or valid contents of BAR0 pages.
    return r;
}
static_assert(sizeof(n33_vram_geometry_status)==64,
              "N33N VRAM geometry must remain 64 bytes");
} } // namespace navi33::vram_live
