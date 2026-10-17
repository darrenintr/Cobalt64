// SPDX-License-Identifier: MIT
// RX 7600 native probe service. Read-only N33N query contract.
#pragma once
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include "Navi33NativeABI.h"

class Navi33Probe : public IOService {
    OSDeclareDefaultStructors(Navi33Probe)
    using super = IOService;

public:
    IOService *probe(IOService *provider, SInt32 *score) override;
    bool start(IOService *provider) override;
    void stop(IOService *provider) override;
    IOReturn newUserClient(task_t owningTask, void *securityID, UInt32 type,
                           OSDictionary *properties, IOUserClient **handler) override;
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
    // Written in start() before registerService(), read thereafter.
    uint32_t nativeFlags_ = N33_CAP_PCI_INVENTORY;
};
