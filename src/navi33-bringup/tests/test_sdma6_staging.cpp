// Host-only GMC11+SDMA6 staging tests (all buffers synthetic CPU memory).
#include "../src/Navi33Sdma6Staging.h"
#include <cassert>
#include <cstdio>
#include <vector>
#include <cstring>
using namespace navi33;
using namespace navi33::staging;
int main() {
    std::vector<uint8_t> buffer(kBytes,0xaau);
    const dma::Segment fragments[]={
        {0x1000000ull,4u*gmc11::kPageBytes},
        {0x2000000ull,12u*gmc11::kPageBytes}
    };
    const Report valid=prepare(fragments,2u,buffer.data(),buffer.size());
    assert(valid.code==Code::Pass);
    assert(valid.pages==kPages && valid.packetDwords==16u);
    assert(valid.candidatePageTableDmaIova==0x2000000ull);
    assert(verifyInitial(buffer.data(),buffer.size(),valid));
    assert(!gpuCompleted(valid));
    const uint8_t *ring=buffer.data()+kRingPage*kBytePage;
    assert(read32(ring)==sdma6::kSdmaCopyOp);
    assert(read32(ring+4u)==kBytePage-1u);
    assert(read32(ring+12u)==static_cast<uint32_t>(kGuestGpuVa));
    assert(read32(ring+20u)==static_cast<uint32_t>(kGuestGpuVa+kBytePage));
    assert(read32(ring+28u)==(5u|(3u<<16u)));
    assert(read32(ring+32u)==static_cast<uint32_t>(kGuestGpuVa+kFencePage*kBytePage));
    assert(read32(ring+40u)==kFenceCookie);
    assert(read32(buffer.data()+kFencePage*kBytePage)==0u);
    for(size_t i=0;i<kPages;++i){
        const uint64_t lo=read32(buffer.data()+kPageTablePage*kBytePage+i*8u);
        const uint64_t hi=read32(buffer.data()+kPageTablePage*kBytePage+i*8u+4u);
        const uint64_t pte=lo|(hi<<32u);
        assert(gmc11::parseSystemPte(pte).code==gmc11::Code::Pass);
        assert(((pte & gmc11::kPteWriteable)!=0u)==(i!=kSourcePage));
    }
    // Malformed target address must never be treated as a valid image.
    write32(buffer.data()+kRingPage*kBytePage+20u,0xdeadbee0u);
    assert(!verifyInitial(buffer.data(),buffer.size(),valid));
    write32(buffer.data()+kRingPage*kBytePage+20u,
            static_cast<uint32_t>(kGuestGpuVa+kTargetPage*kBytePage));
    assert(verifyInitial(buffer.data(),buffer.size(),valid));
    buffer[0]^=0x80u;
    assert(!verifyInitial(buffer.data(),buffer.size(),valid));
    buffer[0]^=0x80u;
    buffer[kTargetPage*kBytePage]=0u;
    assert(!verifyInitial(buffer.data(),buffer.size(),valid));
    // Not even a CPU-forged completed fence can be counted as GPU execution.
    write32(buffer.data()+kFencePage*kBytePage,kFenceCookie);
    assert(!gpuCompleted(valid));
    std::vector<uint8_t> sentinel(kBytes,0xaau);
    const dma::Segment malformed[]={
        {0x1000000ull,4u*gmc11::kPageBytes},
        {0x1000000ull,12u*gmc11::kPageBytes}
    };
    assert(prepare(malformed,2u,sentinel.data(),sentinel.size()).code==
           Code::RejectedScatter);
    for(uint8_t c:sentinel) assert(c==0xaau);
    assert(prepare(fragments,2u,nullptr,kBytes).code==Code::BadMemory);
    assert(prepare(fragments,2u,sentinel.data(),kBytes-1u).code==Code::BadMemory);
    const dma::Segment notAligned[]={{0x1000001ull,kBytes}};
    assert(prepare(notAligned,1u,sentinel.data(),kBytes).code==Code::RejectedScatter);
    const dma::Segment contiguous[]={{0x5000000ull,kBytes}};
    assert(prepare(contiguous,1u,sentinel.data(),kBytes).code==Code::Pass);
    assert(!kGpuRingSubmissionEnabled && !kGartHardwareEnabled);
    std::puts("GMC11 IOMMU scatter / 16 PTE shadow + SDMA6 copy/fence staging: PASS (NO HARDWARE SUBMIT)");
}
