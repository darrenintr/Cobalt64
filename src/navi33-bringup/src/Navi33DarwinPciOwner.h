// SPDX-License-Identifier: MIT
// IOKit evidence and the held PCI owner for the Navi33 owner probe. Config
// reads and IOService open/close only: no BAR mapping, no MMIO, no config
// writes.
#pragma once
#include "Navi33PciOwner.h"
#include "Navi33ProductionOwner.h"
#include <IOKit/IOService.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <stdint.h>

namespace navi33 { namespace darwin_owner {

// Probe only: gathers Evidence, opens and closes the device once, and logs
// the decision. The device is not held.
navi33::pci_owner::Decision runOwnerProbe(IOPCIDevice *pci,IOService *self,
                                          bool gateOn,bool killOn,
                                          bool recoveryAttested);

// Hold: when the decision is Owned, keeps IOService::open for the owner epoch
// and acquires the production owner. Any other outcome closes the device
// before returning. Returns true only when a live ticket is held.
bool holdOwner(IOPCIDevice *pci,IOService *self,bool killOn,bool recoveryAttested,
               production_owner::Owner &owner,production_owner::Ticket &ticket);

// Releases a held owner (no-op without a ticket) and closes the device.
void releaseOwner(IOPCIDevice *pci,IOService *self,
                  production_owner::Owner &owner,production_owner::Ticket &ticket);

} } // namespace navi33::darwin_owner
