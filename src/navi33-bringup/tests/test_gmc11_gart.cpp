// GMC11 CPU-only AMD GART system PTE model. No DMA, no kernel/PCI access.
#include "../src/Navi33Gmc11Gart.h"
#include "../src/Navi33GpuVaArena.h"
#include <cassert>
#include <cstdio>
#include <cstdint>

using namespace navi33;
using namespace navi33::gmc11;

int main() {
    static_assert(!kCanProgramGfxhub && !kCanInvalidateTlb,
                  "Do not enable hardware address translation");
    assert(makeSystemPte(0x00200000ull,false).code==Code::Pass);
    const Pte readOnly=makeSystemPte(0x00200000ull,false);
    const Pte readWrite=makeSystemPte(0x00200000ull,true);
    assert(readOnly.bits==((3ull<<48u)|0x00200000ull|0x27ull));
    assert(readWrite.bits==((3ull<<48u)|0x00200000ull|0x67ull));
    assert(parseSystemPte(readOnly.bits).code==Code::Pass);
    assert(parseSystemPte(readWrite.bits).code==Code::Pass);
    assert(parseSystemPte(readOnly.bits|(1ull<<54u)).code==Code::UnknownFlags);
    assert(parseSystemPte(readOnly.bits&~kPteSystem).code==Code::UnknownFlags);
    assert(parseSystemPte(readOnly.bits&~kPteSnooped).code==Code::UnknownFlags);
    assert(makeSystemPte(0,true).code==Code::InvalidDma);
    assert(makeSystemPte(0x201001,true).code==Code::InvalidDma);
    assert(makeSystemPte(kMaxConservativeDmaAddress,true).code==Code::InvalidDma);
    assert(makeSystemPte(kMaxConservativeDmaAddress-kPageBytes,true).code==Code::Pass);

    const Aperture aperture{0x100000000ull,16u*kPageBytes};
    uint64_t table[16]{};
    const uint64_t pages[3]{0x200000ull,0x400000ull,0x500000ull};
    const uint64_t va=aperture.gpuVaBase+3u*kPageBytes;
    const Result mapped=mapSystemPages(aperture,va,pages,3,true,table,16);
    assert(mapped.code==Code::Pass&&mapped.changed==3u);
    assert(table[0]==0u);
    assert(table[3]==makeSystemPte(pages[0],true).bits);
    assert(table[4]==makeSystemPte(pages[1],true).bits);
    assert(table[5]==makeSystemPte(pages[2],true).bits);
    const uint64_t before=table[3];

    assert(mapSystemPages(aperture,va,pages,3,false,table,16).code==Code::SlotOccupied);
    assert(table[3]==before);
    assert(mapSystemPages(aperture,va,nullptr,3,true,table,16).code==Code::NullBuffer);
    assert(mapSystemPages(aperture,va,pages,0,true,table,16).code==Code::NoPages);
    assert(mapSystemPages(aperture,va,pages,kMaxBatchPages+1,true,table,16).code==Code::TooManyPages);
    assert(mapSystemPages(aperture,va+1u,pages,3,true,table,16).code==Code::OutsideAperture);
    assert(mapSystemPages(aperture,aperture.gpuVaBase-4096u,pages,3,true,table,16).code==Code::OutsideAperture);
    assert(mapSystemPages(aperture,aperture.gpuVaBase+15u*kPageBytes,pages,3,true,table,16).code==Code::OutsideAperture);
    assert(mapSystemPages({0u,aperture.lengthBytes},va,pages,3,true,table,16).code==Code::BadAperture);
    assert(mapSystemPages(aperture,va,pages,3,true,table,15).code==Code::BadAperture);
    assert(table[3]==before&&table[4]!=0u&&table[5]!=0u);

    assert(unmapSystemPages(aperture,va,3,table,16).code==Code::Pass);
    assert(table[3]==0u&&table[4]==0u&&table[5]==0u);
    assert(unmapSystemPages(aperture,va,3,table,16).code==Code::SlotUnmapped);

    // Transactional failure: first valid PTE must not be installed when
    // a later DMA page address is malformed.
    const uint64_t badPages[3]{0x200000ull,0x300001ull,0x400000ull};
    assert(mapSystemPages(aperture,va,badPages,3,true,table,16).code==Code::InvalidDma);
    assert(table[3]==0u&&table[4]==0u&&table[5]==0u);

    uint64_t goodPages[3]{0x200000ull,0x300000ull,0x400000ull};
    assert(mapSystemPages(aperture,va,goodPages,3,false,table,16).code==Code::Pass);
    table[4]|=1ull<<54u; // GFX12-style flag injection
    assert(unmapSystemPages(aperture,va,3,table,16).code==Code::UnknownFlags);
    assert(table[3]==makeSystemPte(goodPages[0],false).bits);
    assert(table[5]==makeSystemPte(goodPages[2],false).bits);

    va::Arena arena;
    assert(arena.reset(0x100000000ull,256ull*kPageBytes));
    auto a=arena.reserve(8u*kPageBytes);
    auto b=arena.reserve(4u*kPageBytes);
    auto c=arena.reserve(4u*kPageBytes,65536u);
    assert(a.code==va::Code::Pass && a.address==0x100000000ull);
    assert(b.code==va::Code::Pass && b.address==a.address+8u*kPageBytes);
    assert(c.code==va::Code::Pass && c.address==0x100010000ull);
    assert(arena.contains(a.address+4096u,2u*kPageBytes));
    assert(!arena.contains(a.address+7u*kPageBytes,2u*kPageBytes));
    assert(arena.count()==3u);
    assert(arena.release(b.address)==va::Code::Pass);
    assert(arena.release(b.address)==va::Code::NotReserved);
    auto reused=arena.reserve(4u*kPageBytes);
    assert(reused.code==va::Code::Pass&&reused.address==b.address);
    assert(arena.reserve(1u).code==va::Code::InvalidRange);
    assert(arena.reserve(kPageBytes,3000u).code==va::Code::InvalidRange);
    assert(arena.reserve(257ull*kPageBytes).code==va::Code::InvalidRange);

    va::Arena small;
    assert(small.reset(0x200000000ull,256u*kPageBytes));
    for(size_t i=0;i<va::kMaxReservations;++i)
        assert(small.reserve(kPageBytes).code==va::Code::Pass);
    assert(small.reserve(kPageBytes).code==va::Code::NoSlots);
    assert(small.count()==va::kMaxReservations);
    assert(!small.reset(0u,kPageBytes));
    assert(small.count()==0u);

    std::puts("Navi33 GMC11/GART PTE, atomic map/unmap and GPU VA arena: PASS (host-only)");
}
