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
    ShadowPageTableRejected, ShadowValidationFailed, Pass
};

struct Report {
    Code code=Code::NotRequested;
    uint8_t segmentCount=0u;
    uint8_t pageCount=0u;
};

// Must be called only after BOTH navi33-probe=1 and
// navi33-dma-preflight=1 are present. It does not change PCI COMMAND,
// map BARs, program GPU MMIO, open a user client, submit GPU commands,
// write a hardware GART/PTE or expose bus addresses through IORegistry.
Report runOnce(IOPCIDevice *rx7600);

constexpr bool kHardwareDmaSubmissionEnabled=false;
static_assert(!kHardwareDmaSubmissionEnabled, "IOMMU diagnostics cannot submit GPU DMA");

} // namespace darwin_dma
} // namespace navi33
