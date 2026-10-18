// Host-only checks for the PCI owner probe decision (no hardware).
#include "../src/Navi33PciOwner.h"
#include <assert.h>
#include <stdio.h>

namespace po=navi33::pci_owner;

static po::Evidence good() {
    po::Evidence e{};
    e.gateOn=true;
    e.identityOk=true;
    e.openSucceeded=true;
    e.displayClientAttached=false;
    e.recoveryAttested=true;
    e.dmaMapperPresent=true;
    return e;
}

static void allEvidenceOwns() {
    const po::Decision d=po::decide(good());
    assert(d.code==po::Code::Owned && d.refusalMask==0u);
}

static void gateClosedIsDisabledNotRefused() {
    po::Evidence e=good();
    e.gateOn=false;
    const po::Decision d=po::decide(e);
    assert(d.code==po::Code::Disabled && d.refusalMask==0u);
}

static void everyFailureRefusesWithItsBit() {
    po::Evidence e=good(); e.identityOk=false;
    assert(po::decide(e).refusalMask==po::WrongDevice);

    e=good(); e.openSucceeded=false;
    assert(po::decide(e).refusalMask==po::OpenFailed);

    e=good(); e.displayClientAttached=true;
    assert(po::decide(e).refusalMask==po::DisplayClient);

    e=good(); e.recoveryAttested=false;
    assert(po::decide(e).refusalMask==po::NoRecovery);

    e=good(); e.dmaMapperPresent=false;
    assert(po::decide(e).refusalMask==po::NoDmaMapper);
}

static void killSwitchWinsOverEverything() {
    po::Evidence e=good();
    e.killOn=true;
    const po::Decision d=po::decide(e);
    assert(d.code==po::Code::Refused);
    assert(d.refusalMask==po::KillSwitch);

    // Even with every other input bad, the kill switch is the only reason.
    po::Evidence worst{};
    worst.gateOn=true;
    worst.killOn=true;
    assert(po::decide(worst).refusalMask==po::KillSwitch);
}

static void emptyEvidenceRefusesWithGateOpen() {
    po::Evidence e{};
    e.gateOn=true;
    const po::Decision d=po::decide(e);
    assert(d.code==po::Code::Refused);
    assert(d.refusalMask==(po::WrongDevice|po::OpenFailed|po::NoRecovery|po::NoDmaMapper));
}

static void evidenceBitsNameEveryInput() {
    po::Evidence e{};
    assert(po::evidenceBits(e)==0u);
    e.gateOn=true;                 assert(po::evidenceBits(e)==po::kEvGate);
    e=po::Evidence{}; e.killOn=true;            assert(po::evidenceBits(e)==po::kEvKill);
    e=po::Evidence{}; e.identityOk=true;        assert(po::evidenceBits(e)==po::kEvIdentity);
    e=po::Evidence{}; e.openSucceeded=true;     assert(po::evidenceBits(e)==po::kEvOpen);
    e=po::Evidence{}; e.displayClientAttached=true; assert(po::evidenceBits(e)==po::kEvDisplay);
    e=po::Evidence{}; e.dmaMapperPresent=true;  assert(po::evidenceBits(e)==po::kEvMapper);
    e=po::Evidence{}; e.recoveryAttested=true;  assert(po::evidenceBits(e)==po::kEvRecovery);
    // The good evidence has every bit but kill and display.
    assert(po::evidenceBits(good())==(po::kEvGate|po::kEvIdentity|po::kEvOpen|
                                       po::kEvMapper|po::kEvRecovery));
}

static void ownedNeverGrantsWrites() {
    static_assert(!po::kCanWriteGpu && !po::kCanWriteConfig,
                  "owner probe must not write");
    assert(po::decide(good()).code==po::Code::Owned);
}

int main() {
    allEvidenceOwns();
    gateClosedIsDisabledNotRefused();
    everyFailureRefusesWithItsBit();
    killSwitchWinsOverEverything();
    emptyEvidenceRefusesWithGateOpen();
    ownedNeverGrantsWrites();
    evidenceBitsNameEveryInput();
    puts("Navi33 PCI owner probe decision: PASS (offline)");
}
