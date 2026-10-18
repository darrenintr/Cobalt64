// SPDX-License-Identifier: MIT
#include "Navi33DarwinPciOwner.h"
#include "Navi33Discovery.h"
#include "Navi33Identity.h"
#include <IOKit/IOLib.h>
#include <IOKit/IORegistryEntry.h>
#include <IOKit/IOMapper.h>
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

// The card's DMA domain: the IOMapper for this device, identified by its
// registry entry. Absent when macOS has no mapper for the device (the VM).
static bool dmaMapperFor(IOPCIDevice *pci,uint64_t *domainOut) {
    IOMapper *mapper=IOMapper::copyMapperForDevice(pci);
    if(!mapper) {
        *domainOut=0u;
        return false;
    }
    *domainOut=production_owner::domainFromMapper(true,mapper->getRegistryEntryID());
    mapper->release();
    return true;
}

// Gathers Evidence. keepOpen leaves IOService::open in place on success, so
// the caller owns the close.
static pci_owner::Evidence gather(IOPCIDevice *pci,IOService *self,bool gateOn,
                                  bool killOn,bool recoveryAttested,bool keepOpen,
                                  navi33::Identity *idOut,uint64_t *domainOut) {
    pci_owner::Evidence e{};
    e.gateOn=gateOn;
    e.killOn=killOn;
    e.recoveryAttested=recoveryAttested;
    if(pci && self) {
        const navi33::Identity id=navi33::inspectPci(
            pci->configRead32(kIOPCIConfigVendorID),
            pci->configRead32(kIOPCIConfigRevisionID));
        e.identityOk=navi33::isSupported(id);
        if(idOut) *idOut=id;
        // IOService::open fails while another client holds the device, so a
        // true result is an observation that nobody else held it right now.
        e.openSucceeded=pci->open(self);
        if(e.openSucceeded && !keepOpen) pci->close(self);
        e.displayClientAttached=displayClientAttached(pci);
        e.dmaMapperPresent=dmaMapperFor(pci,domainOut);
    }
    return e;
}

ProbeReport runOwnerProbe(IOPCIDevice *pci,IOService *self,bool gateOn,bool killOn,
                          bool recoveryAttested) {
    uint64_t domain=0u;
    const pci_owner::Evidence e=gather(pci,self,gateOn,killOn,recoveryAttested,false,nullptr,&domain);
    const pci_owner::Decision d=pci_owner::decide(e);
    IOLog("Navi33Probe: owner probe gate=%d kill=%d identity=%d open=%d display=%d recovery=%d -> code %u mask 0x%x\n",
          int(e.gateOn), int(e.killOn), int(e.identityOk), int(e.openSucceeded),
          int(e.displayClientAttached), int(e.recoveryAttested),
          static_cast<unsigned>(d.code), d.refusalMask);
    ProbeReport r{};
    r.evidence=e;
    r.decision=d;
    r.dmaDomain=domain;
    return r;
}

bool holdOwner(IOPCIDevice *pci,IOService *self,bool killOn,bool recoveryAttested,
               production_owner::Owner &owner,production_owner::Ticket &ticket,
               ProbeReport *report) {
    navi33::Identity id{};
    uint64_t domain=0u;
    const pci_owner::Evidence e=gather(pci,self,true,killOn,recoveryAttested,true,&id,&domain);
    const pci_owner::Decision d=pci_owner::decide(e);
    if(report) { report->evidence=e; report->decision=d; report->dmaDomain=domain; }
    IOLog("Navi33Probe: owner hold gate=%d kill=%d identity=%d open=%d display=%d recovery=%d -> code %u mask 0x%x\n",
          int(e.gateOn), int(e.killOn), int(e.identityOk), int(e.openSucceeded),
          int(e.displayClientAttached), int(e.recoveryAttested),
          static_cast<unsigned>(d.code), d.refusalMask);
    if(d.code!=pci_owner::Code::Owned) {
        if(e.openSucceeded && pci) pci->close(self);
        return false;
    }
    // providerId: the kernel's registry identity for the card. dmaDomainId: the
    // IOMapper for this device (the IOMMU unit's domain, possibly shared).
    production_owner::Attestation a{};
    a.pciDecision=d;
    a.providerId=pci->getRegistryEntryID();
    a.dmaDomainId=domain;
    a.vendorId=id.vendorId;
    a.deviceId=id.deviceId;
    a.revisionId=id.revisionId;
    a.independentResetAvailable=recoveryAttested;
    ticket=owner.acquire(a);
    if(!ticket) {
        IOLog("Navi33Probe: owner acquire refused, mask 0x%x\n", owner.lastRefusal());
        pci->close(self);
        return false;
    }
    IOLog("Navi33Probe: owner held, epoch %llu\n", static_cast<unsigned long long>(ticket.epoch));
    return true;
}

void releaseOwner(IOPCIDevice *pci,IOService *self,
                  production_owner::Owner &owner,production_owner::Ticket &ticket) {
    if(!ticket) return;
    const bool released=owner.release(ticket);
    IOLog("Navi33Probe: owner release epoch %llu -> %s\n", static_cast<unsigned long long>(ticket.epoch),
          released ? "released" : "kept (poisoned)");
    ticket=production_owner::Ticket{};
    if(pci && self) pci->close(self);
}

} } // namespace navi33::darwin_owner
