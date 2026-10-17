// Navi33 VRAM interval ledger: pure host model; no PCI or BAR0 access.
#include "../src/Navi33VramBoAllocator.h"
#include <cassert>
#include <cstdio>
using namespace navi33;
namespace m=vram_geometry;
namespace bo=vram_bo;
static m::Geometry geometry() {
    m::PhysicalInventory p{};
    p.realBytes=8ull*1024ull*1024ull*1024ull;
    p.assignedBar0Bytes=256ull*1024ull*1024ull;
    p.independentlyVerifiedMemsize=true;
    const auto r=m::decode({0x6000u,0x61ffu,0x100u},
                           {0x6000u,0x61ffu,0x100u},p);
    assert(r.code==m::Code::Pass);return r.geometry;
}
static bo::Authority authority() {
    bo::Authority a{};
    a.pciDeviceExclusive=true;a.allDisplayClientsStopped=true;
    a.firmwareAndDisplayRangesInventoried=true;
    a.independentRecoveryAvailable=true;return a;
}
int main() {
    int user1=0,user2=0;
    {
        bo::Arena denied{};
        auto a=authority();a.allDisplayClientsStopped=false;
        assert(denied.configure(geometry(),a,{0,4096u},nullptr,0u)==
               bo::Code::NoAuthority);
        assert(denied.liveLeases()==0u);
        assert(denied.allocate(&user1,4096u).code==
               bo::Code::NotConfigured);
    }
    {
        bo::Arena ledger{};
        const bo::Range reserved[]={{0u,8192u},{32768u,32768u}};
        assert(ledger.configure(geometry(),authority(),{0u,131072u},
                                reserved,2u)==bo::Code::Pass);
        const auto a=ledger.allocate(&user1,4096u);
        assert(a.code==bo::Code::Pass);
        assert(a.range.offset==8192u);
        assert(a.address.cpuBar0Visible);
        assert(a.address.gpuMcAddress==geometry().mcStart+8192u);
        assert(a.address.gpuPhysicalAddress==geometry().physicalFbBase+8192u);
        assert(a.address.bar0Offset==8192u);
        assert(ledger.holds(&user1,a.token));
        assert(!ledger.holds(&user2,a.token));
        const auto b=ledger.allocate(&user2,16384u);
        assert(b.code==bo::Code::Pass);
        assert(b.range.offset==12288u); // adjacent, no collision
        const auto c=ledger.allocate(&user1,8192u);
        assert(c.code==bo::Code::Pass);
        assert(c.range.offset==65536u); // excludes 32..64KiB
        assert(!ledger.release(&user1,b.token)); // foreign token
        assert(!ledger.release(&user1,{a.token.generation+1u,a.token.slot}));
        assert(ledger.release(&user1,a.token));
        assert(!ledger.release(&user1,a.token)); // double-free
        const auto replacement=ledger.allocate(&user2,4096u);
        assert(replacement.code==bo::Code::Pass);
        assert(replacement.range.offset==8192u);
        assert(replacement.token.generation>a.token.generation);
        assert(!ledger.release(&user2,a.token)); // ABA defense
        assert(ledger.liveLeases()==3u);
        ledger.quarantine(); // may have reached live GPU MMIO
        assert(ledger.isQuarantined());
        assert(!ledger.release(&user2,replacement.token));
        assert(ledger.allocate(&user2,4096u).code==bo::Code::Quarantined);
        assert(ledger.liveLeases()==3u); // must remain pinned until reset
    }
    {
        bo::Arena ledger{};
        const bo::Range reserved[]={{4096u,8192u},{8192u,4096u}};
        assert(ledger.configure(geometry(),authority(),{0u,65536u},
                                reserved,2u)==bo::Code::Overlap);
        const bo::Range bad[]={{geometry().realBytes,8192u}};
        assert(ledger.configure(geometry(),authority(),{0u,65536u},
                                bad,1u)==bo::Code::BadReservation);
        assert(ledger.configure(geometry(),authority(),
                                {geometry().realBytes,4096u},
                                nullptr,0u)==bo::Code::InvalidSlice);
    }
    {
        bo::Arena ledger{};
        const uint64_t high=geometry().cpuVisibleBytes;
        assert(ledger.configure(geometry(),authority(),{high,8192u},
                                nullptr,0u)==bo::Code::Pass);
        assert(ledger.allocate(&user1,4096u).code==bo::Code::NoSpace);
        const auto a=ledger.allocate(&user1,4096u,false);
        assert(a.code==bo::Code::Pass && !a.address.cpuBar0Visible);
        assert(ledger.release(&user1,a.token));
    }
    {
        bo::Arena ledger{};
        assert(ledger.configure(geometry(),authority(),{0u,8192u},
                                nullptr,0u)==bo::Code::Pass);
        assert(ledger.allocate(&user1,4096u).code==bo::Code::Pass);
        assert(ledger.allocate(&user1,4096u).code==bo::Code::Pass);
        assert(ledger.allocate(&user1,4096u).code==bo::Code::NoSpace);
        assert(ledger.allocate(nullptr,4096u).code==bo::Code::InvalidSlice);
    }
    std::puts("VRAM BO allocator exclusions/leases/PA translation/quarantine OK (offline)");
}
