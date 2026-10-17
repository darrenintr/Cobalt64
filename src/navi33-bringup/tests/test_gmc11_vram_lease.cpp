// GMC11 root BO binding: only mock VRAM, no GPU BAR0 access.
#include "../src/Navi33Gmc11VramLease.h"
#include <cassert>
#include <cstdio>
using namespace navi33;
int main() {
    vram_geometry::PhysicalInventory inventory{};
    inventory.realBytes=8ull*1024ull*1024ull*1024ull;
    inventory.assignedBar0Bytes=512ull*1024ull*1024ull;
    inventory.independentlyVerifiedMemsize=true;
    const auto observed=vram_geometry::decode(
        {0x6000u,0x61ffu,0x100u},
        {0x6000u,0x61ffu,0x100u},inventory);
    assert(observed.code==vram_geometry::Code::Pass);
    const auto geometry=observed.geometry;
    vram_bo::Authority owned{};
    owned.pciDeviceExclusive=true;
    owned.allDisplayClientsStopped=true;
    owned.firmwareAndDisplayRangesInventoried=true;
    owned.independentRecoveryAvailable=true;
    vram_bo::Arena arena{};
    assert(arena.configure(geometry,owned,{0x100000u,0x8000u},
                           nullptr,0u)==vram_bo::Code::Pass);
    int first=0,other=0;
    const auto allocation=arena.allocate(&first,4096u);
    assert(allocation.code==vram_bo::Code::Pass);
    const auto root=vram_binding::bindRoot(
        geometry,arena,&first,allocation.token);
    assert(root.valid);
    assert(root.rootGpuMcAddress==geometry.mcStart+0x100000u);
    assert(root.rootPhysicalAddress==geometry.physicalFbBase+0x100000u);
    assert(root.cpuBar0Offset==0x100000u);
    assert(root.reservation.rootMcAddress==root.rootPhysicalAddress);
    assert(root.reservation.mcVramStart==geometry.physicalFbBase);
    assert(root.reservation.ownedBoBytes==4096u);
    assert(!vram_binding::bindRoot(geometry,arena,&other,
                                  allocation.token).valid);
    auto mismatch=geometry;mismatch.physicalFbBase+=0x1000000ull;
    assert(!vram_binding::bindRoot(mismatch,arena,&first,
                                  allocation.token).valid);
    arena.quarantine();
    assert(!vram_binding::bindRoot(geometry,arena,&first,
                                  allocation.token).valid);
    assert(!arena.release(&first,allocation.token));
    std::puts("GMC11 nonzero-FB_OFFSET leased root / MC address binding OK (fake BO)");
}
