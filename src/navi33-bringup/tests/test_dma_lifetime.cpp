// SPDX-License-Identifier: MIT
// Device-IOMMU lifetime policy: no PCI device or physical GPU accessed.
#include "../src/Navi33DmaLifetime.h"
#include <cassert>
#include <cstdio>
using namespace navi33::dma_lifetime;
int main() {
    State stage{};
    assert(stage.phase()==Phase::Idle);
    assert(!stage.requiresExternalHardwareReset());
    assert(stage.mayCompleteIommuMapping());
    assert(!stage.isPinned());
    assert(!stage.quarantineBeforeFirstMmioAttempt());
    assert(stage.pin() && stage.isPinned());
    assert(!stage.pin());
    assert(stage.mayCompleteIommuMapping());
    assert(stage.unpin() && stage.phase()==Phase::Idle);
    assert(stage.pin());
    assert(stage.quarantineBeforeFirstMmioAttempt());
    assert(stage.phase()==Phase::Quarantined);
    assert(stage.requiresExternalHardwareReset());
    assert(!stage.mayCompleteIommuMapping());
    assert(!stage.unpin());
    assert(!stage.pin());
    assert(!stage.quarantineBeforeFirstMmioAttempt());
    assert(stage.isPinned());
    assert(!stage.mayCompleteIommuMapping());
    // No reset() API: a local software callback cannot release a
    // mapping while the physical GPU might be fetching from it.
    std::puts("Navi33 pinned device-IOMMU DMA lifetime and quarantine test passed (no hardware)");
}
