#include "../src/Navi33Sdma6HardwareGate.h"
#include <cassert>
#include <cstdio>
using namespace navi33::sdma_hw;
int main() {
    Evidence e{};
    assert(evaluate(e).failure==Failure::WrongDevice);
    e.vendor=0x1002u;e.device=0x7480u;
    assert(evaluate(e).failure==Failure::FirmwareMissing);
    e.firmwareVerified=true;
    assert(evaluate(e).failure==Failure::DmaNotPinned);
    e.dmaPinned=true;
    assert(evaluate(e).failure==Failure::HardwareVmMissing);
    e.hwVmInstalled=true;
    assert(evaluate(e).failure==Failure::TlbNotFlushed);
    e.tlbSynchronized=true;
    assert(evaluate(e).failure==Failure::QueueNotOwned);
    e.exclusiveRing=true;
    assert(evaluate(e).failure==Failure::NoRecovery);
    e.independentRecovery=true;
    assert(evaluate(e).failure==Failure::FenceNotVerified);
    e.hardwareFenceVerified=true;e.sourceDataVerified=true;
    assert(evaluate(e).failure==Failure::FenceNotVerified);
    e.outputDataVerified=true;
    assert(evaluate(e).stage==Stage::Ready);
    assert(evaluate(e).permitsSubmission);
    e.vendor=0xffffu;
    assert(!evaluate(e).permitsSubmission);
    static_assert(!kDeviceSubmissionImplemented,"never submit from host gate");
    std::puts("GFX11 SDMA6 evidence gate passed (software test only)");
}
