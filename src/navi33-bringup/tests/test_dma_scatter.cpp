// IODMACommand segment invariants represented by deterministic host fixtures.
// No macOS kernel, PCI IOMMU or physical RX7600 is touched.
#include "../src/Navi33DmaScatter.h"
#include <cassert>
#include <cstdint>
#include <cstdio>
using namespace navi33;
using namespace navi33::dma;

int main() {
    static_assert(!kCanProgramGpuPte && !kCanStartDma,
                  "validated IOVA list is not a usable hardware GPU mapping");
    const Segment good[]={
        {0x0000001000400000ull,4u*gmc11::kPageBytes},
        {0x0000002000800000ull,12u*gmc11::kPageBytes}
    };
    uint64_t pages[kMaxPages]{};
    const uint64_t expected[]={0x1000400000ull,0x1000401000ull,
                               0x1000402000ull,0x1000403000ull};
    auto r=flatten(good,2,kDiagnosticBytes,pages,kMaxPages);
    assert(r.code==Status::Pass&&r.pages==16u&&r.totalBytes==kDiagnosticBytes);
    for(size_t i=0;i<4u;++i)assert(pages[i]==expected[i]);
    assert(pages[4]==0x2000800000ull);
    assert(pages[15]==0x200080b000ull);

    const gmc11::Aperture a{0x100000000ull,kDiagnosticBytes};
    uint64_t shadow[kMaxPages]{};
    auto plan=mapIntoShadow(good,2,kDiagnosticBytes,a,a.gpuVaBase,
                            shadow,kMaxPages);
    assert(plan.code==Status::Pass&&plan.pages==16u);
    for(size_t i=0;i<kMaxPages;++i) {
        assert(gmc11::parseSystemPte(shadow[i]).code==gmc11::Code::Pass);
        assert((shadow[i]&gmc11::kPteWriteable)==0u);
    }
    assert((shadow[0]&gmc11::kAddressMask)==good[0].busAddress);
    assert((shadow[4]&gmc11::kAddressMask)==good[1].busAddress);
    assert(gmc11::unmapSystemPages(a,a.gpuVaBase,kMaxPages,shadow,kMaxPages).code==
           gmc11::Code::Pass);

    // Mapper-less x86 fallback supplies one CPU-physical segment per
    // pinned 4-KiB page. These remain address candidates only; the pure
    // planner may build a shadow PTE image but cannot submit GPU DMA.
    Segment physicalPages[kMaxPages]{};
    for(size_t i=0u;i<kMaxPages;++i)
        physicalPages[i]={0x30000000ull+i*gmc11::kPageBytes,
                          gmc11::kPageBytes};
    auto physical=flatten(physicalPages,kMaxPages,kDiagnosticBytes,
                          pages,kMaxPages);
    assert(physical.code==Status::Pass&&physical.pages==kMaxPages);
    assert(pages[0]==0x30000000ull&&pages[kMaxPages-1u]==0x3000f000ull);
    auto physicalShadow=mapIntoShadow(
        physicalPages,kMaxPages,kDiagnosticBytes,a,a.gpuVaBase,
        shadow,kMaxPages);
    assert(physicalShadow.code==Status::Pass&&
           physicalShadow.pages==kMaxPages);
    assert((shadow[0]&gmc11::kAddressMask)==0x30000000ull);
    assert((shadow[kMaxPages-1u]&gmc11::kAddressMask)==0x3000f000ull);
    assert(gmc11::unmapSystemPages(a,a.gpuVaBase,kMaxPages,shadow,kMaxPages).code==
           gmc11::Code::Pass);

    auto reject=[&](const Segment *s,size_t n,uint64_t expectedBytes, Status code) {
        for(size_t i=0;i<kMaxPages;++i){pages[i]=0xdeadbeefull;shadow[i]=0u;}
        auto res=flatten(s,n,expectedBytes,pages,kMaxPages);
        assert(res.code==code&&res.pages==0u);
        for(size_t i=0;i<kMaxPages;++i)assert(pages[i]==0xdeadbeefull);
    };
    reject(nullptr,1,kDiagnosticBytes,Status::EmptyList);
    reject(good,0,kDiagnosticBytes,Status::EmptyList);
    reject(good,2,0,Status::BadLength);
    reject(good,2,kDiagnosticBytes+gmc11::kPageBytes,Status::BadLength);
    reject(good,2,kDiagnosticBytes-4096u,Status::WrongTotal);
    reject(good,2,kDiagnosticBytes-1u,Status::BadLength);
    reject(good,2,kDiagnosticBytes+2u,Status::BadLength);

    const Segment unaligned[]={{good[0].busAddress+1u,kDiagnosticBytes}};
    reject(unaligned,1,kDiagnosticBytes,Status::BadAlignment);
    const Segment missing[]={{0,kDiagnosticBytes}};
    reject(missing,1,kDiagnosticBytes,Status::InvalidAddress);
    const Segment overflow[]={{(1ull<<48u)-gmc11::kPageBytes,
                               2*gmc11::kPageBytes}};
    reject(overflow,1,2*gmc11::kPageBytes,Status::AddressOverflow);
    const Segment shortEntry[]={{good[0].busAddress,kDiagnosticBytes-4095u}};
    reject(shortEntry,1,kDiagnosticBytes,Status::BadLength);
    const Segment dupe[]={{good[0].busAddress,2*gmc11::kPageBytes},
                          {good[0].busAddress+gmc11::kPageBytes,
                           2*gmc11::kPageBytes}};
    reject(dupe,2,4*gmc11::kPageBytes,Status::DuplicatePage);
    const Segment goodSmall[]={{0x400000ull,gmc11::kPageBytes}};
    reject(goodSmall,1,2*gmc11::kPageBytes,Status::WrongTotal);
    const Segment tooMany[]={good[0],good[1]};
    for(size_t i=0;i<kMaxPages;++i)pages[i]=0xdeadbeefull;
    assert(flatten(tooMany,2,kDiagnosticBytes,pages,4u).code==
           Status::InvalidOutput);
    assert(flatten(tooMany,2,kDiagnosticBytes,nullptr,kMaxPages).code==
           Status::InvalidOutput);
    // page-table is left unchanged when an input page fails validation.
    uint64_t dirty[kMaxPages]{};
    dirty[0]=0x1234ull;
    const auto denied=mapIntoShadow(good,2,kDiagnosticBytes,a,a.gpuVaBase,
                                    dirty,kMaxPages);
    assert(denied.code==Status::GartRejected&&dirty[0]==0x1234ull);
    for(size_t i=1;i<kMaxPages;++i)assert(dirty[i]==0u);
    std::puts("Navi33 DMA-address candidates -> GMC11 shadow PTE: PASS (host-only)");
}
