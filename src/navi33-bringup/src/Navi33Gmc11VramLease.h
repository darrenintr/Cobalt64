// SPDX-License-Identifier: MIT
// Bridge a live, exclusive *software* VRAM BO lease to GMC11 GART
// root image inspection. Absolutely no BAR0 or GPU writes here.
// A real PCI VRAM allocator and readback callback are NOT implemented.
#pragma once
#include "Navi33VramBoAllocator.h"
#include "Navi33Gmc11RootVram.h"
namespace navi33 { namespace vram_binding {
struct Root {
    bool valid=false;
    uint64_t rootGpuMcAddress=0u;
    uint64_t rootPhysicalAddress=0u;
    uint64_t cpuBar0Offset=0u;
    vram_root::Reservation reservation{};
};
inline Root bindRoot(const vram_geometry::Geometry &geometry,
                     const vram_bo::Arena &arena,
                     const void *client,vram_bo::Token token) {
    Root out{};
    const auto lease=arena.describe(client,token);
    if(lease.code!=vram_bo::Code::Pass ||
       lease.range.bytes!=vram_geometry::kPage ||
       !lease.address.cpuBar0Visible)
        return out;
    const auto expected=vram_geometry::locate(
        geometry,lease.range.offset,lease.range.bytes);
    if(!expected.valid || !expected.cpuBar0Visible ||
       lease.address.gpuMcAddress!=expected.gpuMcAddress ||
       lease.address.gpuPhysicalAddress!=expected.gpuPhysicalAddress ||
       lease.address.bar0Offset!=expected.bar0Offset)
        return out;
    // This is a CPU-visible, leased region; not evidence the VRAM contents
    // are correct. vram_root::inspect must independently read every PTE.
    out.reservation.mcVramStart=geometry.physicalFbBase;
    out.reservation.mcVramBytes=geometry.realBytes;
    out.reservation.ownedBoMcStart=expected.gpuPhysicalAddress;
    out.reservation.ownedBoBytes=lease.range.bytes;
    out.reservation.rootMcAddress=expected.gpuPhysicalAddress;
    out.rootGpuMcAddress=expected.gpuMcAddress;
    out.rootPhysicalAddress=expected.gpuPhysicalAddress;
    out.cpuBar0Offset=expected.bar0Offset;
    out.valid=true;
    return out;
}
constexpr bool kMacKernelVramBOAllocationReady=false;
static_assert(!kMacKernelVramBOAllocationReady,
              "VRAM lease bookkeeping must not be mistaken for a real BO");
} } // namespace navi33::vram_binding
