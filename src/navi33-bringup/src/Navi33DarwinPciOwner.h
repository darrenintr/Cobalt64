// SPDX-License-Identifier: MIT
// IOKit evidence for the Navi33 PCI owner probe. Config reads and one
// IOService open/close pair only: no BAR mapping, no MMIO, no config writes.
#pragma once
#include "Navi33PciOwner.h"
#include <IOKit/IOService.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <stdint.h>

namespace navi33 { namespace darwin_owner {

// Gathers Evidence for `pci`, with `self` as the forClient of the open.
// The gate, kill switch and recovery attestation are passed in from the
// boot arguments the caller already parsed.
navi33::pci_owner::Decision runOwnerProbe(IOPCIDevice *pci,IOService *self,
                                          bool gateOn,bool killOn,
                                          bool recoveryAttested);

} } // namespace navi33::darwin_owner
