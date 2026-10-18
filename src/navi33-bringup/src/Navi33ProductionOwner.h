// SPDX-License-Identifier: MIT
// Software state machine for the production PCI/queue owner that the SDMA6
// Queue1 proof needs. It performs no I/O. The kext adapter must supply the
// Attestation from IOKit (exclusive open, identity, display-client scan); this
// header only decides whether that evidence is enough, and tracks the epoch.
//
// Phases:
//   Unowned  - no owner; acquire() may succeed.
//   Owned    - one owner for one epoch; SDMA0 Queue1 may be claimed once.
//   Poisoned - a hardware touch may have happened; release() is refused. Only
//              an independent reset (resetAttested) returns to Unowned, and
//              the next acquire() starts a new epoch.
//
// Host tests build Attestation values by hand. Those are not hardware evidence
// (AGENTS.md hard rule 6): this module can say "the evidence would be enough"
// but cannot prove the evidence is real.
#pragma once
#include "Navi33PciOwner.h"
#include "Navi33Sdma6Queue1MemoryContract.h"
#include <stdint.h>

namespace navi33 { namespace production_owner {

constexpr bool kCanWriteGpu=false;
constexpr bool kCanWriteConfig=false;
constexpr bool kCanClaimHardware=false;
static_assert(!kCanWriteGpu && !kCanWriteConfig && !kCanClaimHardware,
              "production owner state machine must not touch the card");

// The supported identity, from Navi33Identity.h and the RX 7600 facts.
constexpr uint16_t kVendorAmd=0x1002u;
constexpr uint16_t kDeviceNavi33=0x7480u;
constexpr uint8_t kRevisionNavi33=0xcfu;

enum Refusal : uint32_t {
    PciNotOwned      = 1u<<0,  // the kext decision was not Owned
    MissingProvider  = 1u<<1,  // providerId or dmaDomainId is zero
    WrongDevice      = 1u<<2,  // not 1002:7480 rev cf
    NoResetPath      = 1u<<3,  // no independent reset path was attested
    Busy             = 1u<<4,  // an owner exists in this epoch (Owned or Poisoned)
    EpochExhausted   = 1u<<5,
};

struct Attestation {
    pci_owner::Decision pciDecision{};   // from the kext owner probe
    uint64_t providerId=0u;              // kernel-assigned provider identity
    uint64_t dmaDomainId=0u;             // IODMACommand domain of this device
    uint16_t vendorId=0u;
    uint16_t deviceId=0u;
    uint8_t revisionId=0u;
    bool independentResetAvailable=false;
};

struct Ticket {
    uint64_t epoch=0u;   // 0 == refused
    uint64_t providerId=0u;
    constexpr explicit operator bool() const { return epoch!=0u; }
};

// What a reset must establish before the owner may start a new epoch. The
// software cannot prove a power cycle; the operator or kext attests it.
struct ResetAttestation {
    uint64_t poisonedEpoch=0u;
    bool gpuPowerCycled=false;
    bool pciFunctionReset=false;
    bool firmwareStateDiscarded=false;
};

enum class Phase : uint8_t { Unowned=0, Owned, Poisoned };

class Owner {
public:
    Ticket acquire(const Attestation &a) {
        uint32_t refused=0u;
        if(a.pciDecision.code!=pci_owner::Code::Owned) refused|=PciNotOwned;
        if(a.providerId==0u || a.dmaDomainId==0u) refused|=MissingProvider;
        if(a.vendorId!=kVendorAmd || a.deviceId!=kDeviceNavi33 ||
           a.revisionId!=kRevisionNavi33)
            refused|=WrongDevice;
        if(!a.independentResetAvailable) refused|=NoResetPath;
        if(phase_!=Phase::Unowned) refused|=Busy;
        if(epoch_==UINT64_MAX) refused|=EpochExhausted;
        lastRefusal_=refused;
        if(refused) return {};
        ++epoch_;
        phase_=Phase::Owned;
        providerId_=a.providerId;
        dmaDomainId_=a.dmaDomainId;
        vendorId_=a.vendorId;
        deviceId_=a.deviceId;
        revisionId_=a.revisionId;
        queue1Claimed_=false;
        return {epoch_,a.providerId};
    }

    bool holds(Ticket t) const {
        return t.epoch!=0u && phase_!=Phase::Unowned && t.epoch==epoch_ &&
               t.providerId==providerId_;
    }

    // Must be called before the first MMIO, DMA or firmware write of a
    // session. A timeout or failed callback does not undo it.
    bool markHardwareTouched(Ticket t) {
        if(!holds(t) || phase_!=Phase::Owned) return false;
        phase_=Phase::Poisoned;
        return true;
    }

    // Only an untouched owner can be released.
    bool release(Ticket t) {
        if(!holds(t) || phase_!=Phase::Owned) return false;
        phase_=Phase::Unowned;
        queue1Claimed_=false;
        return true;
    }

    // SDMA0 Queue1 (VMID1 proof) is exclusive within one owned epoch.
    bool claimSdma0Queue1(Ticket t) {
        if(!holds(t) || phase_!=Phase::Owned || queue1Claimed_) return false;
        queue1Claimed_=true;
        return true;
    }

    // Returns the owner to Unowned after an independent reset. The next
    // acquire() gets a new epoch, so every older ticket is stale.
    bool resetAttested(const ResetAttestation &r) {
        if(phase_!=Phase::Poisoned || r.poisonedEpoch!=epoch_) return false;
        if(!r.gpuPowerCycled || !r.pciFunctionReset || !r.firmwareStateDiscarded)
            return false;
        phase_=Phase::Unowned;
        queue1Claimed_=false;
        return true;
    }

    // Fills the memory contract's device owner. Only for a live owner.
    bool toDeviceOwner(Ticket t,queue1_memory::DeviceOwner &out) const {
        if(!holds(t)) return false;
        out.providerId=providerId_;
        out.dmaDomainId=dmaDomainId_;
        out.vendorId=vendorId_;
        out.deviceId=deviceId_;
        out.revisionId=revisionId_;
        return true;
    }

    Phase phase() const { return phase_; }
    uint64_t epoch() const { return epoch_; }
    uint32_t lastRefusal() const { return lastRefusal_; }

private:
    Phase phase_=Phase::Unowned;
    uint64_t epoch_=0u;
    uint64_t providerId_=0u;
    uint64_t dmaDomainId_=0u;
    uint16_t vendorId_=0u;
    uint16_t deviceId_=0u;
    uint8_t revisionId_=0u;
    bool queue1Claimed_=false;
    uint32_t lastRefusal_=0u;
};

} } // namespace navi33::production_owner
