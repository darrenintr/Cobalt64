// Host-only checks for the production owner state machine (no hardware).
// Attestation values here are synthetic; they prove the logic, not the card.
#include "../src/Navi33ProductionOwner.h"
#include <assert.h>
#include <stdio.h>

namespace po=navi33::production_owner;
namespace pci=navi33::pci_owner;
namespace q1=navi33::queue1_memory;

static po::Attestation good() {
    po::Attestation a{};
    a.pciDecision.code=pci::Code::Owned;
    a.providerId=0x11u;
    a.dmaDomainId=0x22u;
    a.vendorId=po::kVendorAmd;
    a.deviceId=po::kDeviceNavi33;
    a.revisionId=po::kRevisionNavi33;
    a.independentResetAvailable=true;
    return a;
}

static po::ResetAttestation resetFor(uint64_t epoch) {
    po::ResetAttestation r{};
    r.poisonedEpoch=epoch;
    r.gpuPowerCycled=true;
    r.pciFunctionReset=true;
    r.firmwareStateDiscarded=true;
    return r;
}

static void acquireReleaseHappyPath() {
    po::Owner o;
    po::Ticket t=o.acquire(good());
    assert(t && t.epoch==1u);
    assert(o.phase()==po::Phase::Owned);
    assert(o.holds(t));
    assert(o.release(t));
    assert(o.phase()==po::Phase::Unowned);
    assert(!o.holds(t));
}

static void eachMissingEvidenceRefuses() {
    po::Attestation a=good(); a.pciDecision.code=pci::Code::Refused;
    po::Owner o1; assert(!o1.acquire(a) && (o1.lastRefusal()&po::PciNotOwned));

    a=good(); a.providerId=0u;
    po::Owner o2; assert(!o2.acquire(a) && (o2.lastRefusal()&po::MissingProvider));

    a=good(); a.dmaDomainId=0u;
    po::Owner o3; assert(!o3.acquire(a) && (o3.lastRefusal()&po::MissingProvider));

    a=good(); a.deviceId=0x7440u;
    po::Owner o4; assert(!o4.acquire(a) && (o4.lastRefusal()&po::WrongDevice));

    a=good(); a.revisionId=0xc0u;
    po::Owner o5; assert(!o5.acquire(a) && (o5.lastRefusal()&po::WrongDevice));

    a=good(); a.independentResetAvailable=false;
    po::Owner o6; assert(!o6.acquire(a) && (o6.lastRefusal()&po::NoResetPath));
}

static void secondAcquireIsBusy() {
    po::Owner o;
    assert(o.acquire(good()));
    assert(!o.acquire(good()) && (o.lastRefusal()&po::Busy));
}

static void poisonedOwnerCannotBeReleased() {
    po::Owner o;
    po::Ticket t=o.acquire(good());
    assert(o.markHardwareTouched(t));
    assert(o.phase()==po::Phase::Poisoned);
    assert(!o.release(t));                    // a timeout cannot unpin DMA
    assert(o.phase()==po::Phase::Poisoned);
    assert(!o.acquire(good()) && (o.lastRefusal()&po::Busy));
}

static void resetStartsNewEpochAndStalesOldTickets() {
    po::Owner o;
    po::Ticket t1=o.acquire(good());
    assert(o.markHardwareTouched(t1));
    assert(!o.resetAttested(resetFor(99u)));   // wrong epoch
    po::ResetAttestation partial=resetFor(t1.epoch);
    partial.gpuPowerCycled=false;
    assert(!o.resetAttested(partial));         // power cycle is required
    assert(o.resetAttested(resetFor(t1.epoch)));
    assert(o.phase()==po::Phase::Unowned);

    po::Ticket t2=o.acquire(good());
    assert(t2 && t2.epoch==2u);
    assert(!o.holds(t1));                      // stale ticket from epoch 1
    assert(o.holds(t2));
}

static void queueClaimIsExclusivePerEpoch() {
    po::Owner o;
    po::Ticket t=o.acquire(good());
    assert(o.claimSdma0Queue1(t));
    assert(!o.claimSdma0Queue1(t));            // exclusive
    assert(o.release(t));
    po::Ticket t2=o.acquire(good());
    assert(o.claimSdma0Queue1(t2));            // a new epoch may claim again
}

static void queueClaimNeedsLiveOwner() {
    po::Owner o;
    po::Ticket dead{};
    assert(!o.claimSdma0Queue1(dead));
    po::Ticket t=o.acquire(good());
    assert(o.markHardwareTouched(t));
    assert(!o.claimSdma0Queue1(t));            // poisoned owner cannot claim
}

static void domainComesFromTheMapperOnly() {
    // No mapper means no domain, whatever id the caller holds.
    assert(po::domainFromMapper(false,0x77u)==0u);
    assert(po::domainFromMapper(true,0x77u)==0x77u);
    // A zero registry id is refused by the owner, not silently accepted.
    po::Owner o;
    po::Attestation a=good();
    a.dmaDomainId=po::domainFromMapper(true,0u);
    assert(!o.acquire(a) && (o.lastRefusal()&po::MissingProvider));
}

static void deviceOwnerFeedsTheMemoryContract() {
    po::Owner o;
    po::Ticket t=o.acquire(good());
    q1::DeviceOwner d{};
    assert(o.toDeviceOwner(t,d));
    assert(d.providerId==0x11u && d.dmaDomainId==0x22u);
    assert(d.vendorId==0x1002u && d.deviceId==0x7480u && d.revisionId==0xcfu);
    po::Ticket stale{};
    assert(!o.toDeviceOwner(stale,d));
}

int main() {
    acquireReleaseHappyPath();
    eachMissingEvidenceRefuses();
    secondAcquireIsBusy();
    poisonedOwnerCannotBeReleased();
    resetStartsNewEpochAndStalesOldTickets();
    queueClaimIsExclusivePerEpoch();
    queueClaimNeedsLiveOwner();
    deviceOwnerFeedsTheMemoryContract();
    domainComesFromTheMapperOnly();
    puts("Navi33 production PCI/queue owner state machine: PASS (offline)");
}
