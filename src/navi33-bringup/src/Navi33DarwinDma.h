// Optional, explicitly gated native macOS IOMMU/DMA-address preflight.
// No GPU programming. All DMA mappings are released before returning.
// Requires running macOS + real IOPCIFamily mapper; not executed in CI.
#pragma once
#include <stdint.h>
class IOPCIDevice;

namespace navi33 {
namespace darwin_dma {

enum class Code : uint8_t {
    NotRequested=0, RefusedDevice, NoDeviceMapper, NoMemory,
    MemoryPrepareFailed, CommandCreationFailed, CommandBindFailed,
    CommandPrepareFailed, SegmentQueryFailed, ScatterRejected,
    ShadowPageTableRejected, ShadowValidationFailed, Pass,
    PhysicalNoMemory, PhysicalPrepareFailed, PhysicalAddressInvalid,
    PhysicalAddressOnly
};

struct Report {
    Code code=Code::NotRequested;
    uint8_t segmentCount=0u;
    uint8_t pageCount=0u;
};

// Must be called only after BOTH navi33-probe=1 and
// navi33-dma-preflight=1 are present. If no device IOMapper exists, it may
// inspect pinned CPU-physical pages with kIOMemoryMapperNone and build a
// CPU-only shadow PTE image. That is only an address candidate, not proof
// that the PCI device can DMA to those pages. It never changes PCI COMMAND,
// maps BARs, programs GPU MMIO, submits GPU commands, writes a hardware GART,
// or exposes addresses through IORegistry.
Report runOnce(IOPCIDevice *rx7600);

// True when this host can represent ordinary pinned pages as CPU-physical
// addresses for the device-IOMMU-free owner mode. It allocates and releases
// one 64 KiB buffer and maps nothing for the GPU. It proves representability,
// not that the GPU can reach those addresses.
bool physicalCandidateReady();

constexpr bool kHardwareDmaSubmissionEnabled=false;
static_assert(!kHardwareDmaSubmissionEnabled, "IOMMU diagnostics cannot submit GPU DMA");

} // namespace darwin_dma
} // namespace navi33
