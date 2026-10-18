// Simulated SDMA copy flow: CPU-side descriptor -> ring -> fence.
// GPU addresses here are IDs into an artificial host memory sandbox.
// No real PCI, GART, SDMA firmware, doorbells or GPU-facing allocations.
#include "../src/Navi33Sdma6Ring.h"
#include <cassert>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>

using namespace navi33::sdma6;

int main() {
    static_assert(!kCanRingDoorbell && !kCanSubmitToHardware,
                  "This ring must remain a CPU-only simulator");
    uint32_t words[16]{};
    Ring ring;
    assert(!ring.reset(nullptr,16u));
    assert(!ring.reset(words,12u));
    assert(ring.reset(words,16u));

    const uint64_t a=0x100000ull,b=0x200000ull,c=0x300000ull,d=0x400000ull;
    const CopyPacket copies[2]{makeCopy(a,b,16u),makeCopy(c,d,16u)};
    const auto batch=ring.publish(copies,2u);
    assert(batch.code==RingCode::Pass&&batch.fence==1u);
    assert(ring.publishedDwords()==14u&&ring.inFlightBatches()==1u);
    assert(!ring.fenceComplete(batch.fence)&&ring.fenceKnown(batch.fence));
    assert(!ring.fenceKnown(batch.fence+1u));
    assert(ring.publish(copies,1u).code==RingCode::NoSpace);
    assert(ring.publishedDwords()==14u);

    CopyDescription desc{};
    assert(ring.peek(desc)==RingCode::Pass);
    assert(desc.source==a&&desc.destination==b&&desc.bytes==16u);
    assert(ring.consume()==RingCode::Pass);
    assert(!ring.fenceComplete(batch.fence));
    assert(ring.consumedDwords()==7u);
    // Wrap: next packet spans the ring's last 2 DWORDs and first 5.
    // Batch 2 would straddle the ring end at dword 14, so it is padded with
    // two zero dwords (SDMA NOPs) and starts at dword 16 (ring position 0).
    assert(ring.peek(desc)==RingCode::Pass&&desc.source==c);
    assert(ring.consume()==RingCode::Pass);
    assert(ring.fenceComplete(batch.fence));
    const auto batch2=ring.publish(copies,1u);
    assert(batch2.code==RingCode::Pass&&batch2.fence==2u);
    assert(ring.publishedDwords()==23u);
    assert(!ring.fenceComplete(batch2.fence));
    assert(ring.nextItemDwords()==2u);
    assert(ring.peek(desc)==RingCode::InvalidPacket);
    assert(ring.consume()==RingCode::Pass);
    assert(ring.consumedDwords()==16u);
    assert(ring.peek(desc)==RingCode::Pass&&desc.source==a);
    assert(ring.consume()==RingCode::Pass);
    assert(ring.fenceComplete(batch2.fence)&&ring.inFlightBatches()==0u);
    assert(ring.peek(desc)==RingCode::NoPublishedPacket);
    assert(ring.consume()==RingCode::NoPublishedPacket);
    assert(!ring.fenceComplete(0));

    assert(ring.reset(words,16u));
    CopyPacket invalid=makeCopy(a,b,16u);
    invalid.words[0] = (1u<<18u)|1u; // TMZ forbidden by parser
    assert(ring.publish(&invalid,1u).code==RingCode::InvalidPacket);
    assert(ring.publishedDwords()==0u&&ring.lastIssuedFence()==0u);

    // End-to-end CPU model: submit SDMA copy and apply the decoded
    // descriptor to host-backed memory only, then signal a software fence.
    assert(ring.reset(words,16u));
    uint8_t hostSrc[64]{},hostDst[64]{};
    for(size_t i=0;i<64u;++i) hostSrc[i]=static_cast<uint8_t>(i^0x5au);
    const CopyPacket hostCopy=makeCopy(a,b,64u);
    assert(ring.publish(&hostCopy,1u).code==RingCode::Pass);
    assert(ring.peek(desc)==RingCode::Pass);
    assert(desc.source==a&&desc.destination==b&&desc.bytes==64u);
    std::memcpy(hostDst,hostSrc,desc.bytes);
    assert(!ring.fenceComplete(1u));
    assert(ring.consume()==RingCode::Pass);
    assert(ring.fenceComplete(1u));
    for(size_t i=0;i<64u;++i)assert(hostDst[i]==hostSrc[i]);

    // A bounded ledger of outstanding host-fence IDs prevents unbounded
    // accumulation, even when enough ring capacity remains.
    std::vector<uint32_t> big(512u);
    assert(ring.reset(big.data(),big.size()));
    const CopyPacket p=makeCopy(a,b,16u);
    for(size_t i=0;i<kMaxPendingFences;++i) {
        const auto r=ring.publish(&p,1u);
        assert(r.code==RingCode::Pass&&r.fence==i+1u);
    }
    assert(ring.publish(&p,1u).code==RingCode::TooManyBatches);
    assert(ring.consume()==RingCode::Pass);
    assert(ring.fenceComplete(1u));
    assert(ring.publish(&p,1u).code==RingCode::Pass);
    while(ring.consume()==RingCode::Pass) {}
    assert(ring.lastCompletedFence()==kMaxPendingFences+1u);
    assert(ring.inFlightBatches()==0u);

    std::puts("Navi33 SDMA6 CPU ring / wrap / backpressure / fence simulation: PASS (no GPU)");
}
