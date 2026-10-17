// Navi33 GMC11/GART metadata-only page-table builder.
// References (AMD MIT): Linux drivers/gpu/drm/amd/amdgpu/{amdgpu_vm.h,
// amdgpu_gart.c,gmc_v11_0.c}, amd/include/soc_v1_0_enum.h.
// CPU-memory table writes only. NO DMA mappings, GPU memory, MMIO,
// TLB invalidation, allocation of pinned pages, PSP or DCN operations.
// A successful page-table image is not a working GPU virtual memory system.
#pragma once
#include <stdint.h>
#include <stddef.h>

namespace navi33 {
namespace gmc11 {

constexpr uint64_t kPageBytes = 4096ull; // AMDGPU_GPU_PAGE_SIZE
constexpr uint64_t kPteBytes = 8ull;
constexpr uint64_t kMaxConservativeDmaAddress = (1ull << 48u);
constexpr size_t kMaxBatchPages = 256u;

constexpr uint64_t kPteValid = 1ull << 0u;
constexpr uint64_t kPteSystem = 1ull << 1u;
constexpr uint64_t kPteSnooped = 1ull << 2u;
constexpr uint64_t kPteReadable = 1ull << 5u;
constexpr uint64_t kPteWriteable = 1ull << 6u;
// gfx11 uses gfx10 NV10 MTYPE bits 50:48; MTYPE_UC=3. Do not use GFX12 shift 54.
constexpr uint64_t kMtypeUC = 3ull << 48u;
constexpr uint64_t kAllowedPteFlags =
    kPteValid | kPteSystem | kPteSnooped |
    kPteReadable | kPteWriteable | kMtypeUC;
constexpr uint64_t kAddressMask = (1ull << 48u) - kPageBytes;

enum class Code : uint8_t {
    Pass = 0, NullBuffer, NoPages, TooManyPages, BadAperture,
    OutsideAperture, InvalidDma, PermissionDenied, SlotOccupied,
    SlotUnmapped, UnknownFlags
};

struct Result {
    Code code = Code::NullBuffer;
    size_t changed = 0u;
};

struct Pte {
    Code code = Code::InvalidDma;
    uint64_t bits = 0u;
};

inline Pte makeSystemPte(uint64_t dmaBusAddress, bool writeable) {
    // DMA/IOMMU bus address must already be mapped and pinned by an OS
    // provider. A CPU virtual or physical pointer is not a DMA address.
    if (dmaBusAddress == 0u ||
        (dmaBusAddress & (kPageBytes - 1u)) != 0u ||
        dmaBusAddress >= kMaxConservativeDmaAddress ||
        dmaBusAddress > kMaxConservativeDmaAddress - kPageBytes)
        return {Code::InvalidDma, 0u};
    const uint64_t flags = kPteValid | kPteSystem | kPteSnooped |
                           kPteReadable | (writeable ? kPteWriteable : 0ull) |
                           kMtypeUC;
    return {Code::Pass, dmaBusAddress | flags};
}

inline Pte parseSystemPte(uint64_t bits) {
    // Only accept our own conservative system-memory PTE encoding.
    if ((bits & ~ (kAddressMask | kAllowedPteFlags)) != 0u ||
        (bits & (kPteValid | kPteSystem | kPteSnooped |
                 kPteReadable | kMtypeUC)) !=
        (kPteValid | kPteSystem | kPteSnooped | kPteReadable | kMtypeUC))
        return {Code::UnknownFlags, 0u};
    const uint64_t dma = bits & kAddressMask;
    if (dma == 0u)
        return {Code::InvalidDma, 0u};
    return {Code::Pass, bits};
}

struct Aperture {
    uint64_t gpuVaBase;
    uint64_t lengthBytes;
};

inline Code validateRange(const Aperture &a, uint64_t va, size_t pages,
                          size_t tableEntries, size_t &index) {
    index = 0u;
    if (!a.gpuVaBase || a.lengthBytes < kPageBytes ||
        (a.gpuVaBase & (kPageBytes - 1u)) ||
        (a.lengthBytes & (kPageBytes - 1u)) ||
        a.gpuVaBase >= kMaxConservativeDmaAddress ||
        a.lengthBytes > kMaxConservativeDmaAddress - a.gpuVaBase ||
        a.lengthBytes / kPageBytes > tableEntries)
        return Code::BadAperture;
    if (!pages) return Code::NoPages;
    if (pages > kMaxBatchPages) return Code::TooManyPages;
    if (va < a.gpuVaBase || (va & (kPageBytes - 1u)))
        return Code::OutsideAperture;
    const uint64_t diff = va - a.gpuVaBase;
    const uint64_t count = a.lengthBytes / kPageBytes;
    if (diff / kPageBytes >= count ||
        static_cast<uint64_t>(pages) > count - diff / kPageBytes)
        return Code::OutsideAperture;
    index = static_cast<size_t>(diff / kPageBytes);
    return Code::Pass;
}

inline Result mapSystemPages(const Aperture &a, uint64_t gpuVa,
                            const uint64_t *pinnedBusPages, size_t pages,
                            bool writeable, uint64_t *table,
                            size_t tableEntries) {
    if (!pinnedBusPages || !table) return {Code::NullBuffer, 0u};
    size_t start = 0;
    const Code range = validateRange(a,gpuVa,pages,tableEntries,start);
    if (range != Code::Pass) return {range,0u};
    // Transactional preflight: NO PTE is changed unless every input is good.
    for (size_t i=0;i<pages;++i) {
        if (table[start+i] != 0u)
            return {Code::SlotOccupied,0u};
        if (makeSystemPte(pinnedBusPages[i],writeable).code != Code::Pass)
            return {Code::InvalidDma,0u};
    }
    for (size_t i=0;i<pages;++i)
        table[start+i] = makeSystemPte(pinnedBusPages[i],writeable).bits;
    return {Code::Pass,pages};
}

inline Result unmapSystemPages(const Aperture &a, uint64_t gpuVa,
                              size_t pages, uint64_t *table,
                              size_t tableEntries) {
    if (!table) return {Code::NullBuffer,0u};
    size_t start=0;
    const Code range=validateRange(a,gpuVa,pages,tableEntries,start);
    if (range!=Code::Pass) return {range,0u};
    // Avoid partial unmaps on missing/unrecognized entries.
    for (size_t i=0;i<pages;++i) {
        if (table[start+i] == 0u) return {Code::SlotUnmapped,0u};
        if (parseSystemPte(table[start+i]).code != Code::Pass)
            return {Code::UnknownFlags,0u};
    }
    for (size_t i=0;i<pages;++i) table[start+i]=0u;
    return {Code::Pass,pages};
}

// No direct submission path exists. Real VM operations will need pinned DMA
// addresses, a mapped VRAM page-table BO, GFXHUB/MMHUB programming, TLB
// invalidation, fences, and a testable failure recovery path.
constexpr bool kCanProgramGfxhub = false;
constexpr bool kCanInvalidateTlb = false;
static_assert(!kCanProgramGfxhub && !kCanInvalidateTlb,
              "This stage cannot touch GPU memory-controller registers");

} // namespace gmc11
} // namespace navi33
