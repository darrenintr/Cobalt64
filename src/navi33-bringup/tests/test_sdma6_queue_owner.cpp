// Software-only Queue0 exclusivity, no PCI device or register access.
#include "../src/Navi33Sdma6QueueOwner.h"
#include <cassert>
#include <cstdio>
using namespace navi33::queue_owner;
int main() {
    Registry r{};
    int first=0, second=0;
    assert(r.phase()==Phase::Available);
    assert(!r.acquire(nullptr).epoch);
    const Ticket a=r.acquire(&first);
    assert(a.epoch && r.holds(&first,a));
    assert(!r.acquire(&second).epoch);
    assert(!r.releaseUnmodified(&second,a));
    assert(!r.releaseUnmodified(&first,Ticket{a.epoch+1u}));
    assert(r.releaseUnmodified(&first,a));
    assert(r.phase()==Phase::Available);
    const Ticket b=r.acquire(&second);
    assert(b.epoch>a.epoch);
    assert(!r.markHardwareTouched(&second,a)); // stale ticket
    assert(r.markHardwareTouched(&second,b));
    assert(r.phase()==Phase::GpuMayHaveFetched);
    assert(!r.releaseUnmodified(&second,b)); // client death != GPU reset
    assert(!r.acquire(&first).epoch);
    assert(!r.markHardwareTouched(&second,b)); // cannot arm twice
    std::puts("SDMA6 Queue0 per-provider lease poison/recovery guard OK (host only)");
}
