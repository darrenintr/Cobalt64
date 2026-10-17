// SPDX-License-Identifier: MIT
// Host-testable lifecycle for real device-IOMMU-backed pinned memory.
// A successful MMIO callback is NOT evidence that GPU work completed;
// a failed callback may still have reached silicon.
// This policy never performs hardware reset and intentionally cannot
// unpin/recycle after any possibly-live GPU/MMIO interaction.
#pragma once
#include <stdint.h>
namespace navi33 { namespace dma_lifetime {
enum class Phase : uint8_t {
    Idle=0, Pinned=1, Quarantined=2
};
class State {
public:
    bool pin() {
        if(phase_!=Phase::Idle)return false;
        phase_=Phase::Pinned;
        return true;
    }
    bool quarantineBeforeFirstMmioAttempt() {
        if(phase_!=Phase::Pinned)return false;
        phase_=Phase::Quarantined;
        return true;
    }
    bool mayCompleteIommuMapping() const {
        return phase_!=Phase::Quarantined;
    }
    bool unpin() {
        if(phase_==Phase::Quarantined)return false;
        phase_=Phase::Idle;
        return true;
    }
    bool isPinned() const {
        return phase_==Phase::Pinned || phase_==Phase::Quarantined;
    }
    bool requiresExternalHardwareReset() const {
        return phase_==Phase::Quarantined;
    }
    Phase phase() const {return phase_;}
private:
    Phase phase_=Phase::Idle;
};
} } // namespace navi33::dma_lifetime
