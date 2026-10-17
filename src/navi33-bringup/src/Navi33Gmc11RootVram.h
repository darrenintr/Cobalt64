// SPDX-License-Identifier: MIT
// Navi33 GMC11: check that a *separately allocated and pinned* VRAM root
// contains the intended GART PTEs before exposing it to the VM hubs.
//
// Linux amdgpu_gart_table_vram_alloc allocates the GART table in VRAM.
// An IODMACommand IOVA for system RAM is NOT a verified GPU-MC VRAM BO.
// This layer never allocates VRAM, writes BAR0/5, flushes a GPU TLB,
// configures VMID0, or grants queue ownership.
#pragma once
#include "Navi33Gmc11Mmhub.h"
#include <stdint.h>
#include <stddef.h>

namespace navi33 { namespace vram_root {
constexpr size_t kRootWords=staging::kBytePage/sizeof(uint64_t);
static_assert(kRootWords==512u,"4KiB GART root consists of 512 PTEs");

struct Reservation {
    uint64_t mcVramStart=0u;
    uint64_t mcVramBytes=0u;
    uint64_t ownedBoMcStart=0u;
    uint64_t ownedBoBytes=0u;
    uint64_t rootMcAddress=0u;
    // A separately implemented VRAM allocator MUST own/pin this range
    // and refuse all overlapping reservations. This struct does not do it.
};

enum class Code : uint8_t {
    Invalid=0, RootOutsideVram, RootOutsideOwnedBo, BadPteImage,
    NoReadback, ReadFailed, ContentMismatch, ImageMatches
};
struct Report {
    Code code=Code::Invalid;
    uint32_t checkedWords=0u;
    // ImageMatches proves only CPU-visible VRAM readback, not GPU execution.
    bool hardwareVmReady=false;
};
using ReadWord=bool (*)(void*,uint64_t,uint64_t*);

inline bool alignedRange(uint64_t start,uint64_t bytes) {
    return (start%staging::kBytePage)==0u && bytes &&
           (bytes%staging::kBytePage)==0u &&
           start<gmc11::kMaxConservativeDmaAddress &&
           bytes<=gmc11::kMaxConservativeDmaAddress-start;
}
inline bool contains(uint64_t base,uint64_t bytes,
                     uint64_t address,uint64_t needed) {
    return address>=base && needed<=bytes &&
           address-base<=bytes-needed;
}
inline Report inspect(const Reservation &r,const mmhub11::Table &table,
                      ReadWord readWord,void *readerContext) {
    Report out{};
    if(!alignedRange(r.mcVramStart,r.mcVramBytes) ||
       !alignedRange(r.ownedBoMcStart,r.ownedBoBytes) ||
       (r.rootMcAddress%staging::kBytePage) ||
       !r.rootMcAddress ||
       !mmhub11::validTable(table) ||
       table.gpuPhysicalPageTable!=r.rootMcAddress)
        {out.code=Code::Invalid;return out;}
    if(!contains(r.mcVramStart,r.mcVramBytes,
                 r.rootMcAddress,staging::kBytePage)) {
        out.code=Code::RootOutsideVram;return out;
    }
    if(!contains(r.ownedBoMcStart,r.ownedBoBytes,
                 r.rootMcAddress,staging::kBytePage) ||
       !contains(r.mcVramStart,r.mcVramBytes,
                 r.ownedBoMcStart,r.ownedBoBytes)) {
        out.code=Code::RootOutsideOwnedBo;return out;
    }
    if(!readWord){out.code=Code::NoReadback;return out;}
    // Check the *whole* 4 KiB root, not only 16 usable PTEs. Reject
    // stale mappings left in previously used VRAM pages.
    for(size_t i=0;i<kRootWords;++i) {
        uint64_t got=0u;
        if(!readWord(readerContext,r.rootMcAddress+i*sizeof(uint64_t),&got)) {
            out.code=Code::ReadFailed;return out;
        }
        ++out.checkedWords;
        const uint64_t expected=i<staging::kPages?table.entries[i]:0u;
        if(got!=expected){
            out.code=Code::ContentMismatch;return out;
        }
    }
    out.code=Code::ImageMatches;
    // VM hubs haven't been touched and there is no actual GPU read/fence.
    out.hardwareVmReady=false;
    return out;
}
} } // namespace navi33::vram_root
