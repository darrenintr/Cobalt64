// SPDX-License-Identifier: MIT
// GFX11 SDMA6 hardware-readiness gate. Deliberately no MMIO writes.
// Before a REAL fence can be trusted, the device must exclusively own
// a queue, have hardware-visible page tables, TLB sync, live firmware,
// and a bounded reset/recovery path. No flag may be inferred from
// a host-only staging buffer or the PSP/SDMA read-only snapshots.
#pragma once
#include <stdint.h>
#include <stddef.h>

namespace navi33 { namespace sdma_hw {
enum class Stage : uint8_t {
    Refused=0, Identity, Firmware, Dma, Vm, Tlb, Ring, Recovery, Ready
};
enum class Failure : uint8_t {
    None=0, WrongDevice, FirmwareMissing, DmaNotPinned,
    HardwareVmMissing, TlbNotFlushed, QueueNotOwned,
    NoRecovery, FenceNotVerified, UntrustedCapability
};
struct Evidence {
    uint16_t vendor=0,device=0;
    bool firmwareVerified=false;
    bool dmaPinned=false;
    bool hwVmInstalled=false;
    bool tlbSynchronized=false;
    bool exclusiveRing=false;
    bool independentRecovery=false;
    bool hardwareFenceVerified=false;
    bool sourceDataVerified=false;
    bool outputDataVerified=false;
};
struct Result {
    Stage stage=Stage::Refused;
    Failure failure=Failure::WrongDevice;
    bool permitsSubmission=false;
};
constexpr Result evaluate(const Evidence &e) {
    if(e.vendor!=0x1002u || e.device!=0x7480u)
        return {Stage::Refused,Failure::WrongDevice,false};
    if(!e.firmwareVerified)
        return {Stage::Identity,Failure::FirmwareMissing,false};
    if(!e.dmaPinned)
        return {Stage::Firmware,Failure::DmaNotPinned,false};
    if(!e.hwVmInstalled)
        return {Stage::Dma,Failure::HardwareVmMissing,false};
    if(!e.tlbSynchronized)
        return {Stage::Vm,Failure::TlbNotFlushed,false};
    if(!e.exclusiveRing)
        return {Stage::Tlb,Failure::QueueNotOwned,false};
    if(!e.independentRecovery)
        return {Stage::Ring,Failure::NoRecovery,false};
    if(!e.hardwareFenceVerified || !e.sourceDataVerified ||
       !e.outputDataVerified)
        return {Stage::Recovery,Failure::FenceNotVerified,false};
    return {Stage::Ready,Failure::None,true};
}
// Current Navi33 cannot prove these from hardware: never enable commands.
// In particular do not use evaluate(...).permitsSubmission in a write path
// until evidence collection is bound to the same real device/queue/session.
constexpr bool kDeviceSubmissionImplemented=false;
static_assert(!kDeviceSubmissionImplemented,"No live SDMA queue driver exists");
} }
