// SPDX-License-Identifier: MIT
// Navi33 GMC11 + SDMA6 PREPARATION ONLY. These bytes are written into
// CPU-visible, device-IOMMU-mapped system RAM. GPU VMID/PDE/PTE programming,
// HDP/TLB flush, SDMA ring ownership and HW fence retirement are NOT done.
// Kernel clients MUST keep the IODMACommand + pinned memory alive while using
// this image and release them together; do not use IOVA as a GPU VA directly.
#pragma once
#include "Navi33DmaScatter.h"
#include "Navi33Sdma6Packet.h"
#include <stddef.h>
#include <stdint.h>

namespace navi33 { namespace staging {
constexpr size_t kPages=dma::kMaxPages;
constexpr size_t kBytes=static_cast<size_t>(dma::kDiagnosticBytes);
constexpr size_t kSourcePage=0u;
constexpr size_t kTargetPage=1u;
constexpr size_t kRingPage=2u;
constexpr size_t kFencePage=3u;
constexpr size_t kPageTablePage=4u;
constexpr uint64_t kGuestGpuVa=0x100000000ull; // UNMAPPED until GMC11 HW ready
constexpr uint32_t kFenceCookie=0xc0b1606au;
constexpr size_t kCopyWords=sdma6::kCopyPacketDwords;
constexpr size_t kFenceWords=4u;
constexpr size_t kRingWords=16u; // one 7dw copy, one 4dw fence, five SDMA NOPs
constexpr size_t kBytePage=static_cast<size_t>(gmc11::kPageBytes);
static_assert(kBytes==kPages*kBytePage && kPages>=5u,"staging memory budget");
static_assert(kCopyWords+kFenceWords<=kRingWords,"SDMA staging ring overflow");
constexpr bool kGpuRingSubmissionEnabled=false;
constexpr bool kGartHardwareEnabled=false;
static_assert(!kGpuRingSubmissionEnabled && !kGartHardwareEnabled,
              "An IOMMU mapping is not an initialized RX7600 GPU VMID");

enum class Code : uint8_t {
    Pass=0, BadMemory, RejectedScatter, RejectedGart,
    InvalidPacket, InvalidLayout
};
struct Report {
    Code code=Code::BadMemory;
    uint32_t segments=0u;
    uint32_t pages=0u;
    uint32_t packetDwords=0u;
    uint64_t sourceHash=0u, destinationHash=0u;
};

// Never rely on host endianness or pointer aliasing for GPU-visible data.
inline void write32(uint8_t *p,uint32_t value) {
    for(unsigned i=0;i<4u;++i)p[i]=static_cast<uint8_t>(value>>(i*8u));
}
inline uint32_t read32(const uint8_t *p) {
    uint32_t word=0;
    for(unsigned i=0;i<4u;++i)
        word|=static_cast<uint32_t>(p[i])<<(i*8u);
    return word;
}
inline void write64(uint8_t *p,uint64_t value) {
    write32(p,static_cast<uint32_t>(value));
    write32(p+4,static_cast<uint32_t>(value>>32u));
}
inline uint64_t hash(const uint8_t *p,size_t bytes) {
    uint64_t h=1469598103934665603ull;
    for(size_t i=0;i<bytes;++i)
        h=(h ^ p[i])*1099511628211ull;
    return h;
}
inline uint8_t pattern(size_t offset) {
    return static_cast<uint8_t>(((offset*131u)^(offset>>5u)^0x5au)&255u);
}
// Caller owns 64KiB mapped memory for the complete IOMMU mapping lifetime.
// Two preflight stages operate only on stack; buffer isn't touched on error.
inline Report prepare(const dma::Segment *segments,size_t segmentCount,
                      uint8_t *memory,size_t memoryBytes) {
    Report out{};
    if(!memory || memoryBytes<kBytes || !segments) return out;
    // DMA/IOMMU addresses are provided by the OS for this exact PCI device.
    uint64_t busPages[kPages]{};
    const dma::Plan flatten=dma::flatten(segments,segmentCount,kBytes,
                                        busPages,kPages);
    if(flatten.code!=dma::Status::Pass || flatten.pages!=kPages) {
        out.code=Code::RejectedScatter;return out;
    }
    // This is a SOFTWARE SHADOW of a GPU page table, not a live GART.
    uint64_t ptes[kPages]{};
    const gmc11::Aperture aperture{kGuestGpuVa,kBytes};
    const gmc11::Result mapped=gmc11::mapSystemPages(
        aperture,kGuestGpuVa,busPages,kPages,true,ptes,kPages);
    if(mapped.code!=gmc11::Code::Pass) {
        out.code=Code::RejectedGart;return out;
    }
    for(size_t i=0;i<kPages;++i)
        if(gmc11::parseSystemPte(ptes[i]).code!=gmc11::Code::Pass) {
            out.code=Code::RejectedGart;return out;
        }

    const sdma6::CopyPacket copy=sdma6::makeCopy(
        kGuestGpuVa+kSourcePage*kBytePage,
        kGuestGpuVa+kTargetPage*kBytePage,
        static_cast<uint32_t>(kBytePage));
    if(copy.status!=sdma6::CopyCode::Pass ||
       sdma6::inspectCopy(copy.words,kCopyWords).status!=sdma6::CopyCode::Pass) {
        out.code=Code::InvalidPacket;return out;
    }
    const uint64_t fenceVa=kGuestGpuVa+kFencePage*kBytePage;
    if((fenceVa & 3u)!=0u || fenceVa > UINT64_MAX-4u) {
        out.code=Code::InvalidLayout;return out;
    }

    // AFTER full preflight, write only CPU-owned, pinned staging memory.
    // No MMIO, no doorbell, no hardware GART programming.
    for(size_t i=0;i<kBytePage;++i)
        memory[kSourcePage*kBytePage+i]=pattern(i);
    for(size_t i=0;i<kBytePage;++i)
        memory[kTargetPage*kBytePage+i]=0xcdu;
    for(size_t page=kRingPage;page<kPages;++page)
        for(size_t i=0;i<kBytePage;++i)
            memory[page*kBytePage+i]=0u;

    uint8_t *ring=memory+kRingPage*kBytePage;
    for(size_t i=0;i<kCopyWords;++i)
        write32(ring+i*4u,copy.words[i]);
    // Linux amdgpu sdma_v6_0_ring_emit_fence:
    // SDMA_OP_FENCE=5; MTYPE_UC=3 at bits [18:16].
    const uint32_t fence[]={
        5u | (3u<<16u),static_cast<uint32_t>(fenceVa),
        static_cast<uint32_t>(fenceVa>>32u),kFenceCookie
    };
    for(size_t i=0;i<kFenceWords;++i)
        write32(ring+(kCopyWords+i)*4u,fence[i]);
    for(size_t i=kCopyWords+kFenceWords;i<kRingWords;++i)
        write32(ring+i*4u,0u); // SDMA_NOP
    uint8_t *pteImage=memory+kPageTablePage*kBytePage;
    for(size_t i=0;i<kPages;++i)
        write64(pteImage+i*8u,ptes[i]);
    out.code=Code::Pass;
    out.segments=static_cast<uint32_t>(segmentCount);
    out.pages=static_cast<uint32_t>(kPages);
    out.packetDwords=static_cast<uint32_t>(kRingWords);
    out.sourceHash=hash(memory+kSourcePage*kBytePage,kBytePage);
    out.destinationHash=hash(memory+kTargetPage*kBytePage,kBytePage);
    return out;
}
inline bool verifyInitial(const uint8_t *memory,size_t length,
                          const Report &r) {
    if(!memory || length<kBytes || r.code!=Code::Pass ||
       r.pages!=kPages || r.packetDwords!=kRingWords) return false;
    for(size_t i=0;i<kBytePage;++i) {
        if(memory[kSourcePage*kBytePage+i]!=pattern(i) ||
           memory[kTargetPage*kBytePage+i]!=0xcdu) return false;
    }
    const uint8_t *ring=memory+kRingPage*kBytePage;
    const uint32_t packet[kCopyWords]={
        read32(ring),read32(ring+4),read32(ring+8),read32(ring+12),
        read32(ring+16),read32(ring+20),read32(ring+24)
    };
    if(sdma6::inspectCopy(packet,kCopyWords).status!=sdma6::CopyCode::Pass ||
       read32(ring+kCopyWords*4u)!=(5u | (3u<<16u)) ||
       read32(ring+(kCopyWords+3u)*4u)!=kFenceCookie ||
       read32(memory+kFencePage*kBytePage)!=0u) return false;
    return hash(memory,kBytePage)==r.sourceHash &&
           hash(memory+kTargetPage*kBytePage,kBytePage)==r.destinationHash;
}
// A valid initial staging image is NEVER evidence of GPU completion.
// A future hardware completion path MUST own the ring and confirm fence
// memory *after* an actual doorbell + GPU write, then validate copy readback.
inline bool gpuCompleted(const Report &) { return false; }

} } // namespace navi33::staging
