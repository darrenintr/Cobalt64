// SPDX-License-Identifier: MIT
// Software arbitration for a proposed isolated Navi33 SDMA6 Queue0 owner.
// This does NOT obtain PCI ownership or take over firmware/display queues.
// Caller MUST hold a provider-wide lock and independently prove exclusivity.
//
// Once a register-write transaction MAY have started, the lease is poisoned
// until an independent GPU reset; clientClose or a timeout cannot unpin DMA.
#pragma once
#include <stdint.h>
namespace navi33 { namespace queue_owner {
enum class Phase : uint8_t { Available=0, Reserved, GpuMayHaveFetched };
struct Ticket {uint64_t epoch=0u;};
class Registry {
public:
    Ticket acquire(const void *client) {
        if(!client || phase_!=Phase::Available || epoch_==UINT64_MAX)
            return {};
        ++epoch_; owner_=client; phase_=Phase::Reserved;
        return {epoch_};
    }
    bool holds(const void *client,Ticket token) const {
        return client && token.epoch!=0u && owner_==client &&
               token.epoch==epoch_ && phase_!=Phase::Available;
    }
    // Must precede FIRST attempted GPU register write. Even a failed MMIO
    // callback may have changed hardware; do not release on a timeout.
    bool markHardwareTouched(const void *client,Ticket token) {
        if(!holds(client,token) || phase_!=Phase::Reserved)return false;
        phase_=Phase::GpuMayHaveFetched;
        return true;
    }
    bool releaseUnmodified(const void *client,Ticket token) {
        if(!holds(client,token) || phase_!=Phase::Reserved)return false;
        owner_=nullptr;phase_=Phase::Available;return true;
    }
    Phase phase() const {return phase_;}
    // Intentionally no reset() / forceRelease(): a software callback cannot
    // prove a hardware reset. Reinitialize a registry only after a *real*
    // externally validated GPU power-cycle and PCI device ownership reset.
private:
    const void *owner_=nullptr;
    uint64_t epoch_=0u;
    Phase phase_=Phase::Available;
};
} } // namespace navi33::queue_owner
