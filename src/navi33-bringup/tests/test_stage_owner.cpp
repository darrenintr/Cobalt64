// Pure host test of provider-wide N33N DMA staging ownership.
// This does not grant exclusive GPU, PCI or SDMA Queue0 ownership.
#include "../src/Navi33StageOwner.h"
#include <cassert>
#include <cstdio>
using namespace navi33::stage_owner;

int main() {
    Registry registry{};
    int first=0,second=0;
    assert(registry.available());
    assert(!registry.claim(nullptr));
    const Ticket a=registry.claim(&first);
    assert(a && a.epoch!=0u && !registry.available());
    assert(registry.holds(&first,a));
    assert(registry.claim(&first).epoch==a.epoch); // idempotent
    assert(!registry.claim(&second)); // cannot map two diagnostic sessions
    assert(!registry.release(&second,a)); // no foreign release
    assert(!registry.release(&first,Ticket{a.epoch+1u})); // stale token
    assert(registry.holds(&first,a));
    assert(registry.release(&first,a));
    assert(registry.available());
    assert(!registry.release(&first,a)); // double release cannot succeed
    const Ticket b=registry.claim(&second);
    assert(b && b.epoch>a.epoch);
    assert(!registry.release(&second,a)); // old epoch never unlocks new session
    assert(registry.holds(&second,b));
    registry.shutdown();
    assert(!registry.available());
    assert(!registry.holds(&second,b));
    assert(!registry.claim(&first)); // provider stop permanently closes staging
    assert(!registry.release(&second,b));
    std::puts("Provider-scoped DMA stage lease epochs and crash/stop refusal OK (host only)");
}
