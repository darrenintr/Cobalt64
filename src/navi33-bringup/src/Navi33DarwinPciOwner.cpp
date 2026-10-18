// SPDX-License-Identifier: MIT
#include "Navi33DarwinPciOwner.h"
#include "Navi33Discovery.h"
#include "Navi33Identity.h"
#include <IOKit/IOLib.h>
#include <IOKit/IORegistryEntry.h>
#include <libkern/c++/OSIterator.h>
#include <libkern/c++/OSObject.h>

namespace navi33 { namespace darwin_owner {

// A framebuffer or accelerator attached under the PCI device means macOS is
// driving this card. The check looks one level down, which is where the
// framebuffer and accelerator nubs appear for a PCI GPU.
static bool displayClientAttached(IOPCIDevice *pci) {
    OSIterator *it=pci->getChildIterator(gIOServicePlane);
    if(!it) return true;   // cannot enumerate: treat as attached (fail closed)
    bool found=false;
    while(OSObject *child=it->getNextObject()) {
        if(child->metaCast("IOFramebuffer") || child->metaCast("IOAccelerator")) {
            found=true;
            break;
        }
    }
    it->release();
    return found;
}

pci_owner::Decision runOwnerProbe(IOPCIDevice *pci,IOService *self,
                                  bool gateOn,bool killOn,
                                  bool recoveryAttested) {
    pci_owner::Evidence e{};
    e.gateOn=gateOn;
    e.killOn=killOn;
    e.recoveryAttested=recoveryAttested;
    if(pci && self) {
        const navi33::Identity id=navi33::inspectPci(
            pci->configRead32(kIOPCIConfigVendorID),
            pci->configRead32(kIOPCIConfigRevisionID));
        e.identityOk=navi33::isSupported(id);
        // IOService::open fails while another client holds the device, so a
        // true result is an observation that nobody else held it right now.
        // The matching close returns it at once.
        e.openSucceeded=pci->open(self);
        if(e.openSucceeded) pci->close(self);
        e.displayClientAttached=displayClientAttached(pci);
    }
    const pci_owner::Decision d=pci_owner::decide(e);
    IOLog("Navi33Probe: owner probe gate=%d kill=%d identity=%d open=%d display=%d recovery=%d -> code %u mask 0x%x\n",
          int(e.gateOn), int(e.killOn), int(e.identityOk), int(e.openSucceeded),
          int(e.displayClientAttached), int(e.recoveryAttested),
          static_cast<unsigned>(d.code), d.refusalMask);
    return d;
}

} } // namespace navi33::darwin_owner
