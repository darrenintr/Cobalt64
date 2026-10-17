// SPDX-License-Identifier: MIT
// macOS kernel-only physical SDMA6 Queue1 IO bridge. NOT linked into
// Navi33Probe.kext and never enabled by an OpenCore boot argument.
// Unlike host-fake callbacks, BAR5 reads and CPU DMA bytes would come
// from an actual IOPCIDevice and its retained IODMACommand mapping.
//
// Hardware writes intentionally remain impossible: opening a PCI BAR
// and obtaining an IOVA do not confer firmware, VRAM or queue ownership.
// The future isolated PCI owner must supply an independently audited,
// reset-capable authority before this transport can permit submission.
#pragma once
#include "Navi33DarwinStage.h"
#include "Navi33Sdma6BarAdapter.h"
#include "Navi33Sdma6MmioQueue.h"
#include <stdint.h>
#include <stddef.h>

class IOPCIDevice;
class IOMemoryMap;

namespace navi33 { namespace darwin_queue1 {
class PhysicalIO {
public:
    PhysicalIO()=default;
    ~PhysicalIO() { close(); }
    PhysicalIO(const PhysicalIO &)=delete;
    PhysicalIO &operator=(const PhysicalIO &)=delete;

    // Creates a REAL physical BAR5 read mapping for Queue1. No firmware
    // or PCI COMMAND writes, bus master enable or VRAM allocation occurs.
    bool openReadOnly(IOPCIDevice *pci,darwin_stage::Session *stage,
                      const IPDiscovery &discovery);
    void close();
    bool isBound() const {return bar5_!=nullptr && stage_!=nullptr;}
    // Callback packet is bound to this object; it cannot outlive close().
    // A real GPU command submission is NOT authorized by this accessor.
    sdma6_mmio::IO callbacks();

    static constexpr bool kProductionWritesEnabled=false;
    static constexpr bool kExclusivePciOwnerImplemented=false;

private:
    static bool readQueueRegister(void*,uint32_t,uint32_t*);
    static bool denyQueueRegisterWrite(void*,uint32_t,uint32_t);
    static bool beforeGpu(void*);
    static bool afterGpu(void*);
    static bool readStagedByte(void*,size_t,uint8_t*);
    static void delayUs(void*,uint32_t);
    bool readBar5(uint64_t byteOffset,uint32_t *out) const;

    IOPCIDevice *pci_=nullptr;
    IOMemoryMap *bar5_=nullptr;
    darwin_stage::Session *stage_=nullptr; // borrowed under provider lock
    sdma6_bar::Plan queue1Plan_{};
};
static_assert(!PhysicalIO::kProductionWritesEnabled &&
              !PhysicalIO::kExclusivePciOwnerImplemented,
              "Read-only transport is not a physical GPU owner");
} } // namespace navi33::darwin_queue1
