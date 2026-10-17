// SPDX-License-Identifier: MIT
// Isolated Navi33 GMC11 GART-root VRAM upload *prototype*.
// Production Kext does not include or link this module. It cannot be
// enabled by a boot flag or an N33N selector. Only fake-VRAM host tests
// define COBALT_NAVI33_ENABLE_UNSAFE_VRAM_UPLOAD.
//
// No GPU/PCI ownership is acquired here. A future independent GPU owner
// must first establish PCI exclusivity, an owned/pinned VRAM BO, BAR0
// aperture mapping, firmware/display reservations, and recovery.
// NEVER use this on a sole active display GPU.
#pragma once
#include "Navi33Gmc11VramLease.h"
#include "Navi33Gmc11RootVram.h"

namespace navi33 { namespace vram_upload {
#if defined(COBALT_NAVI33_ENABLE_UNSAFE_VRAM_UPLOAD)
constexpr bool kWritePathCompiled=true;
#else
constexpr bool kWritePathCompiled=false;
#endif
struct Gate {
    bool pciExclusivelyOwned=false;
    bool noDisplayOrFirmwareVRAMClients=false;
    bool physicalVramBoIsOwnedAndPinned=false;
    bool bar0ResourceExclusivelyMapped=false;
    bool independentResetAndRecovery=false;
};
constexpr bool permitted(const Gate &g) {
    return g.pciExclusivelyOwned && g.noDisplayOrFirmwareVRAMClients &&
           g.physicalVramBoIsOwnedAndPinned &&
           g.bar0ResourceExclusivelyMapped &&
           g.independentResetAndRecovery;
}
struct IO {
    void *context=nullptr;
    bool (*read64)(void *,uint64_t byteOffset,uint64_t *out)=nullptr;
    bool (*write64)(void *,uint64_t byteOffset,uint64_t value)=nullptr;
    // Must fence posted CPU writes to the GPU VRAM CPU aperture,
    // NOT IODMACommand::synchronize for a system-RAM IOVA.
    bool (*syncBar0Writes)(void *)=nullptr;
};
enum class Code : uint8_t {
    Disabled=0, Refused, RootNotClean, ReadFailure,
    WriteFailure, SyncFailure, VerifyFailure,
    UploadedButGpuVmNotInitialized
};
struct Report {
    Code code=Code::Disabled;
    uint32_t reads=0u,writes=0u;
    uint64_t rootPhysical=0u;
    bool mustResetBeforeRelease=false;
    bool actualGpuExecutionProven=false;
};
inline Report uploadRoot(const Gate &gate,
                         const vram_geometry::Geometry &geometry,
                         vram_bo::Arena &arena,const void *client,
                         vram_bo::Token rootToken,
                         const mmhub11::Table &table,IO io) {
    Report result{};
#if !defined(COBALT_NAVI33_ENABLE_UNSAFE_VRAM_UPLOAD)
    (void)gate;(void)geometry;(void)arena;(void)client;
    (void)rootToken;(void)table;(void)io;
    return result;
#else
    result.code=Code::Refused;
    if(!permitted(gate) || !io.read64 || !io.write64 ||
       !io.syncBar0Writes || !mmhub11::validTable(table))
        return result;

    const auto root=vram_binding::bindRoot(
        geometry,arena,client,rootToken);
    if(!root.valid ||
       table.gpuPhysicalPageTable!=root.rootPhysicalAddress ||
       root.cpuBar0Offset>geometry.cpuVisibleBytes ||
       vram_root::kRootWords*sizeof(uint64_t)>
           geometry.cpuVisibleBytes-root.cpuBar0Offset)
        return result;
    result.rootPhysical=root.rootPhysicalAddress;

    // Before any write, require a clean, independently reserved VRAM
    // root page. Do not clobber a framebuffer or firmware allocation
    // merely because its bytes appear writable or uninitialized.
    for(size_t i=0u;i<vram_root::kRootWords;++i) {
        uint64_t value=0u;
        const uint64_t off=root.cpuBar0Offset+i*sizeof(uint64_t);
        if(!io.read64(io.context,off,&value)) {
            result.code=Code::ReadFailure;return result;
        }
        ++result.reads;
        if(value!=0u) {
            result.code=Code::RootNotClean;return result;
        }
    }

    // Even a transport returning "write failed" may have changed GPU
    // VRAM. Once the first BAR0 write is possible, never unpin or
    // reuse the page until an independently validated device reset.
    arena.quarantine();
    result.mustResetBeforeRelease=true;
    for(size_t i=0u;i<vram_root::kRootWords;++i) {
        const uint64_t expected=
            i<staging::kPages ? table.entries[i] : 0u;
        const uint64_t off=root.cpuBar0Offset+i*sizeof(uint64_t);
        if(!io.write64(io.context,off,expected)) {
            result.code=Code::WriteFailure;return result;
        }
        ++result.writes;
    }
    if(!io.syncBar0Writes(io.context)) {
        result.code=Code::SyncFailure;return result;
    }
    for(size_t i=0u;i<vram_root::kRootWords;++i) {
        uint64_t value=0u;
        const uint64_t off=root.cpuBar0Offset+i*sizeof(uint64_t);
        if(!io.read64(io.context,off,&value)) {
            result.code=Code::ReadFailure;return result;
        }
        ++result.reads;
        const uint64_t expected=
            i<staging::kPages ? table.entries[i] : 0u;
        if(value!=expected) {
            result.code=Code::VerifyFailure;return result;
        }
    }
    result.code=Code::UploadedButGpuVmNotInitialized;
    return result;
#endif
}
} } // namespace navi33::vram_upload
