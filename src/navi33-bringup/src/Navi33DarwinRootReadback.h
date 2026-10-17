// SPDX-License-Identifier: MIT
// Real macOS BAR0 readback adapter for an externally reserved GMC11
// root page. This cannot allocate VRAM, reserve firmware ranges or
// authorize GPU writes. Kept separate from Navi33Probe.kext.
#pragma once
#include "Navi33Gmc11VramLease.h"
#include "Navi33Gmc11VramUpload.h"
#include <stdint.h>
class IOPCIDevice;
class IOMemoryMap;
namespace navi33 { namespace darwin_vram {
class RootReadback {
public:
    RootReadback()=default;
    ~RootReadback() { close(); }
    RootReadback(const RootReadback &)=delete;
    RootReadback &operator=(const RootReadback &)=delete;
    bool openReadOnly(IOPCIDevice *pci,
                      const vram_geometry::Geometry &geometry,
                      const vram_binding::Root &root);
    void close();
    bool ready() const {return bar0_!=nullptr;}
    // Signature consumed by vram_root::inspect, not an authorization
    // to write the GPU's actual page-table root.
    static bool readWord(void *context,uint64_t gpuPhysicalAddress,
                         uint64_t *value);
    // Bound to the full OS PCI BAR0 aperture. Required resource lifetime:
    // RootReadback must outlive every callback using this pointer.
    static constexpr bool kBar0WriteEnabled=false;
private:
    bool readPhysical(uint64_t gpuPhysicalAddress,uint64_t *value) const;
    IOPCIDevice *pci_=nullptr;
    IOMemoryMap *bar0_=nullptr;
    uint64_t rootPhysical_=0u;
    uint64_t rootBarOffset_=0u;
};
static_assert(!RootReadback::kBar0WriteEnabled,
              "A readback mapping is not VRAM ownership");
} } // namespace navi33::darwin_vram
