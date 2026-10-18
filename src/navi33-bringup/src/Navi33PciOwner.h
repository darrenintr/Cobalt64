// SPDX-License-Identifier: MIT
// Pure decision for the Navi33 PCI owner probe. The kext fills Evidence from
// IOKit (Navi33DarwinPciOwner.cpp); this header only combines it. Nothing
// here touches hardware, and an Owned decision grants no GPU access: the
// write gates stay closed (kCanWriteGpu is false).
#pragma once
#include <stdint.h>

namespace navi33 { namespace pci_owner {

constexpr bool kCanWriteGpu=false;
constexpr bool kCanWriteConfig=false;
static_assert(!kCanWriteGpu && !kCanWriteConfig,
              "PCI owner probe must not write the card");

enum class Code : uint8_t { Disabled=0, Refused, Owned };

enum Refusal : uint32_t {
    KillSwitch     = 1u<<0,  // navi33-owner-off set
    WrongDevice    = 1u<<1,  // not the supported RX 7600 identity
    OpenFailed     = 1u<<2,  // IOService::open refused: someone else holds it
    DisplayClient  = 1u<<3,  // a framebuffer or accelerator child is attached
    NoRecovery     = 1u<<4,  // operator did not attest recovery (navi33-owner-recovery)
    NoDmaMapper    = 1u<<5,  // no IOMapper for this device: no DMA domain to own
};

// Filled by the kext. Every field is an observation, not a claim: the open
// result is what IOService::open returned, and the child scan is what the
// registry showed at that moment.
struct Evidence {
    bool gateOn=false;               // navi33-owner-probe=1
    bool killOn=false;               // navi33-owner-off nonzero
    bool identityOk=false;           // inspectPci() and isSupported() passed
    bool openSucceeded=false;        // IOService::open(self) returned true
    bool displayClientAttached=false;// IOFramebuffer/IOAccelerator child found
    bool dmaMapperPresent=false;     // IOMapper::copyMapperForDevice() returned one
    bool recoveryAttested=false;     // navi33-owner-recovery=1
};

// One bit per Evidence field, for the Navi33,OwnerEvidence ioreg property, so a
// results file shows every input even when the kernel log has rolled over.
constexpr uint32_t kEvGate=1u<<0, kEvKill=1u<<1, kEvIdentity=1u<<2, kEvOpen=1u<<3,
                   kEvDisplay=1u<<4, kEvMapper=1u<<5, kEvRecovery=1u<<6;

inline uint32_t evidenceBits(const Evidence &e) {
    return (e.gateOn ? kEvGate : 0u) | (e.killOn ? kEvKill : 0u) |
           (e.identityOk ? kEvIdentity : 0u) | (e.openSucceeded ? kEvOpen : 0u) |
           (e.displayClientAttached ? kEvDisplay : 0u) |
           (e.dmaMapperPresent ? kEvMapper : 0u) |
           (e.recoveryAttested ? kEvRecovery : 0u);
}

struct Decision {
    Code code=Code::Disabled;
    uint32_t refusalMask=0u;
};

// A closed gate is Disabled, not Refused, so the default boot is quiet.
// Kill switch wins over every other input.
inline Decision decide(const Evidence &e) {
    Decision d{};
    if(!e.gateOn) return d;
    if(e.killOn) {
        d.code=Code::Refused;
        d.refusalMask=KillSwitch;
        return d;
    }
    uint32_t refused=0u;
    if(!e.identityOk) refused|=WrongDevice;
    if(!e.openSucceeded) refused|=OpenFailed;
    if(e.displayClientAttached) refused|=DisplayClient;
    if(!e.recoveryAttested) refused|=NoRecovery;
    if(!e.dmaMapperPresent) refused|=NoDmaMapper;
    d.refusalMask=refused;
    d.code=refused ? Code::Refused : Code::Owned;
    return d;
}

} } // namespace navi33::pci_owner
