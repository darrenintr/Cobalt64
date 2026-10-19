// SPDX-License-Identifier: MIT
// RX 7600 native probe service. Read-only N33N queries plus the gated
// PSP13 sOS load (Navi33DarwinPspLoad.h).
#pragma once
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <stddef.h>
#include "Navi33NativeABI.h"
#include "Navi33StageOwner.h"
#include "Navi33ProductionOwner.h"
#include <IOKit/IOLib.h>

namespace navi33 { namespace darwin_owner { struct ProbeReport; } }

class Navi33Probe : public IOService {
    OSDeclareDefaultStructors(Navi33Probe)
    using super = IOService;

public:
    IOService *probe(IOService *provider, SInt32 *score) override;
    bool start(IOService *provider) override;
    void stop(IOService *provider) override;
    // Shutdown and termination paths also release a held PCI owner (G23).
    void systemWillShutdown(IOOptionBits specifier) override;
    bool willTerminate(IOService *provider, IOOptionBits options) override;
    void free() override;
    IOReturn newUserClient(task_t owningTask, void *securityID, UInt32 type,
                           OSDictionary *properties, IOUserClient **handler) override;
    // Provider-scoped root-client DMA staging lease, NOT PCI/SDMA ownership.
    uint64_t claimStageOwner(const void *client);
    void releaseStageOwner(const void *client,uint64_t epoch);
    n33_psp_status pspStatus(); // opt-in, read-only MP0 sOS observation
    n33_sdma_status sdmaStatus(); // opt-in, read-only SDMA6 ring observation
    n33_queue_census_status queueCensus(); // root-only physical Queue0 READ-ONLY
    n33_vram_geometry_status vramGeometryStatus(); // root-only READ-ONLY paired FB registers
    n33_hw_preflight hardwarePreflight(const uint8_t *romBytes=nullptr,
                                     size_t romLength=0u); // no writes
    n33_peek_result peekRegisters(uint32_t window,uint32_t first,
                                  uint32_t count,const uint8_t *romBytes,
                                  size_t romLength); // allowlisted reads
    // PLAN modes read only. RUN writes the GPU: the sOS load (navi33-psp-load=1)
    // or the ring stage (navi33-psp-ring=1); stopAfter is for the ring modes.
    n33_psp_load_result pspLoad(uint32_t mode,const uint8_t *romBytes,
                                size_t romLength,const uint8_t *fwBytes,
                                size_t fwLength,uint32_t stopAfter=0u,
                                uint32_t startAt=0u);
    n33_capabilities nativeCapabilities() const {
        n33_capabilities result{};
        result.abi_version = N33_ABI_VERSION;
        result.size = sizeof(result);
        result.vendor_id = N33_PCI_VENDOR;
        result.device_id = N33_PCI_DEVICE;
        result.gfx_ip_version = N33_GFX_IP_VERSION;
        result.family = N33_FAMILY_GFX1102;
        // No acceleration, VRAM size or GTT size is inferred from PCI BARs.
        result.capabilities = nativeFlags_ &
            (N33_CAP_PCI_INVENTORY | N33_CAP_ROM_DISCOVERY);
        return result;
    }

private:
    void releaseHeldOwner(IOService *provider);
    void publishOwnerReport(const navi33::darwin_owner::ProbeReport &r);
    // Written in start() before registerService(), read thereafter.
    uint32_t nativeFlags_ = N33_CAP_PCI_INVENTORY;
    IOLock *stageOwnerLock_ = nullptr;
    navi33::stage_owner::Registry stageOwner_{};
    IOLock *pspLock_ = nullptr; // serializes PSP plans and the one load
    bool pspLoadAttempted_ = false; // set before the first write; never reset
    bool pspRingAttempted_ = false; // same for the ring stage
    bool smuAttempted_ = false;
    bool mesAttempted_ = false;   // MES start: its own one-shot latch
    bool pspFwAttempted_ = false;   // same for the IP firmware stage
    // Held PCI owner (navi33-owner-hold=1 only). The ticket is nonzero while
    // IOService::open is held; stop() releases it.
    navi33::production_owner::Owner pciOwner_{};
    navi33::production_owner::Ticket pciOwnerTicket_{};
};
