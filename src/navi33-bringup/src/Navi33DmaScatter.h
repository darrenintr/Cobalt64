// Navi33DmaScatter - reject malformed macOS IODMACommand scatter/gather lists.
// Pure C++17; no IOKit, MMIO, GPU firmware, DMA submission, or physical mapping.
// A platform adapter may supply OS-IOMMU IOVAs from IODMACommand, or an
// explicitly non-authoritative CPU-physical candidate when no mapper exists.
// A successful plan does NOT imply the GPU can reach those addresses or that
// GART/GFXHUB/MMHUB was configured.
#pragma once
#include "Navi33Gmc11Gart.h"
#include <stddef.h>
#include <stdint.h>

namespace navi33 {
namespace dma {

constexpr size_t kMaxPages = 16u;
constexpr uint64_t kDiagnosticBytes = kMaxPages * gmc11::kPageBytes;

struct Segment {
    uint64_t busAddress = 0;
    uint64_t length = 0;
};

enum class Status : uint8_t {
    Pass = 0, EmptyList, TooManySegments, BadLength, BadAlignment,
    InvalidAddress, AddressOverflow, DuplicatePage, WrongTotal,
    InvalidOutput, GartRejected
};

struct Plan {
    Status code = Status::EmptyList;
    size_t pages = 0;
    uint64_t totalBytes = 0;
};

// Fail-closed before mutating output. IODMACommand offsets correspond to
// consecutive logical bytes; a physical-candidate adapter supplies one page
// per segment. Device addresses may be discontinuous and must not be guessed
// from CPU pointers. Pagewise validation also disallows aliases.
inline Plan flatten(const Segment *segments, size_t count,
                    uint64_t expectedBytes,
                    uint64_t *pageBusAddresses, size_t pageCapacity) {
    if (!segments || count == 0u) return {Status::EmptyList, 0u, 0u};
    if (count > kMaxPages) return {Status::TooManySegments, 0u, 0u};
    if (!pageBusAddresses || pageCapacity == 0u)
        return {Status::InvalidOutput, 0u, 0u};
    if (!expectedBytes || expectedBytes > kDiagnosticBytes ||
        (expectedBytes & (gmc11::kPageBytes - 1u)))
        return {Status::BadLength, 0u, 0u};

    uint64_t scratch[kMaxPages]{};
    size_t out = 0;
    uint64_t total = 0;
    for (size_t i=0;i<count;++i) {
        const Segment &s = segments[i];
        if (!s.length || (s.length & (gmc11::kPageBytes-1u)))
            return {Status::BadLength, 0u, 0u};
        if ((s.busAddress & (gmc11::kPageBytes-1u)))
            return {Status::BadAlignment, 0u, 0u};
        if (!s.busAddress ||
            s.busAddress >= gmc11::kMaxConservativeDmaAddress)
            return {Status::InvalidAddress, 0u, 0u};
        if (s.length > gmc11::kMaxConservativeDmaAddress - s.busAddress)
            return {Status::AddressOverflow, 0u, 0u};
        if (s.length > expectedBytes-total) return {Status::WrongTotal, 0u, 0u};
        const size_t pages = static_cast<size_t>(s.length/gmc11::kPageBytes);
        if (pages > kMaxPages-out || pages > pageCapacity-out)
            return {Status::InvalidOutput, 0u, 0u};
        for (size_t j=0;j<pages;++j) {
            const uint64_t a=s.busAddress + j*gmc11::kPageBytes;
            for(size_t k=0;k<out;++k)
                if(scratch[k]==a)return {Status::DuplicatePage,0u,0u};
            scratch[out++]=a;
        }
        total+=s.length;
    }
    if(total!=expectedBytes || out!=expectedBytes/gmc11::kPageBytes)
        return {Status::WrongTotal,0u,0u};
    for(size_t i=0;i<out;++i)pageBusAddresses[i]=scratch[i];
    return {Status::Pass,out,total};
}

// Only produces PTEs in an *ordinary caller-owned CPU array*. The DMA
// translator, IOMMU mapping lifetime and GART hardware init remain separate.
// On failure, no PTE is installed in the shadow table.
inline Plan mapIntoShadow(const Segment *segments, size_t count,
                          uint64_t expectedBytes,
                          const gmc11::Aperture &aperture, uint64_t gpuVa,
                          uint64_t *table, size_t tableEntries) {
    uint64_t busPages[kMaxPages]{};
    const Plan plan=flatten(segments,count,expectedBytes,busPages,kMaxPages);
    if(plan.code!=Status::Pass) return plan;
    const gmc11::Result r=gmc11::mapSystemPages(
        aperture,gpuVa,busPages,plan.pages,false,table,tableEntries);
    if(r.code!=gmc11::Code::Pass) return {Status::GartRejected,0u,0u};
    return plan;
}

constexpr bool kCanProgramGpuPte = false;
constexpr bool kCanStartDma = false;
static_assert(!kCanProgramGpuPte && !kCanStartDma,
              "A DMA address list is NOT a functioning GPU GART mapping");
} // namespace dma
} // namespace navi33
