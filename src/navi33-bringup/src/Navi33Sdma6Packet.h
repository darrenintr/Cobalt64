// Navi33 SDMA6 linear-copy PACKET ENCODER (not a GPU submission path).
// Reference: Linux amdgpu/sdma_v6_0.c::sdma_v6_0_emit_copy_buffer
// and amdgpu/vega10_sdma_pkt_open.h (AMD MIT license).
// Produces seven little-endian DWORDS for an OFFLINE command buffer.
// No ring, doorbell, GPU-visible memory allocation, PSP or MMIO.
#pragma once
#include <stdint.h>
#include <stddef.h>

namespace navi33 {
namespace sdma6 {

enum class CopyCode : uint8_t {
    Pass = 0, EmptyCopy, TooLarge, InvalidAddress, AddressOverflow,
    OverlappingMemory
};
constexpr uint32_t kSdmaCopyOp = 1u;       // SDMA_OP_COPY
constexpr uint32_t kSdmaLinearSubop = 0u;  // SDMA_SUBOP_COPY_LINEAR
constexpr uint32_t kMaxCopyBytes = 0x400000u; // conservative 22-bit count+1
constexpr size_t kCopyPacketDwords = 7u;

struct CopyPacket {
    CopyCode status = CopyCode::EmptyCopy;
    uint32_t words[kCopyPacketDwords]{};
};

constexpr bool rangesOverlap(uint64_t a, uint64_t b, uint64_t length) {
    // Valid inputs have no overflow. Use differences, not addition.
    return (a <= b && b - a < length) ||
           (b < a && a - b < length);
}

inline CopyPacket makeCopy(uint64_t sourceGpuVa, uint64_t targetGpuVa,
                           uint32_t byteCount) {
    CopyPacket p{};
    if (byteCount == 0u) {
        p.status=CopyCode::EmptyCopy; return p;
    }
    if (byteCount > kMaxCopyBytes) {
        p.status=CopyCode::TooLarge; return p;
    }
    // Deliberately limited to naturally aligned VRAM/GART copies until
    // the GFX11 VM/GART/PTE implementation is real and tested.
    if (!sourceGpuVa || !targetGpuVa ||
        (sourceGpuVa & 3u) != 0u || (targetGpuVa & 3u) != 0u) {
        p.status=CopyCode::InvalidAddress; return p;
    }
    if (sourceGpuVa > UINT64_MAX - byteCount ||
        targetGpuVa > UINT64_MAX - byteCount) {
        p.status=CopyCode::AddressOverflow; return p;
    }
    if (rangesOverlap(sourceGpuVa,targetGpuVa,byteCount)) {
        p.status=CopyCode::OverlappingMemory; return p;
    }
    // From sdma_v6_0_emit_copy_buffer(): HEADER, COUNT-1, SWAP,
    // SOURCE_LO/HI, DESTINATION_LO/HI. No protected TMZ bit set.
    p.words[0] = kSdmaCopyOp | (kSdmaLinearSubop << 8u);
    p.words[1] = byteCount - 1u;
    p.words[2] = 0u; // no byte-swap flags
    p.words[3] = static_cast<uint32_t>(sourceGpuVa);
    p.words[4] = static_cast<uint32_t>(sourceGpuVa >> 32u);
    p.words[5] = static_cast<uint32_t>(targetGpuVa);
    p.words[6] = static_cast<uint32_t>(targetGpuVa >> 32u);
    p.status = CopyCode::Pass;
    return p;
}

struct CopyDescription {
    CopyCode status = CopyCode::EmptyCopy;
    uint64_t source = 0, destination = 0;
    uint32_t bytes = 0;
};

inline CopyDescription inspectCopy(const uint32_t *packet,size_t dwordCount) {
    CopyDescription d{};
    if (!packet || dwordCount != kCopyPacketDwords ||
        packet[0] != kSdmaCopyOp || packet[2] != 0u)
        return d;
    if (packet[1] >= kMaxCopyBytes) {
        d.status=CopyCode::TooLarge; return d;
    }
    const uint64_t source = static_cast<uint64_t>(packet[3]) |
                            (static_cast<uint64_t>(packet[4]) << 32u);
    const uint64_t dest = static_cast<uint64_t>(packet[5]) |
                          (static_cast<uint64_t>(packet[6]) << 32u);
    const uint32_t bytes = packet[1] + 1u;
    const CopyPacket roundtrip = makeCopy(source,dest,bytes);
    d.status=roundtrip.status;
    if (roundtrip.status==CopyCode::Pass){
        d.source=source;d.destination=dest;d.bytes=bytes;
    }
    return d;
}

constexpr bool kCanSubmitPackets = false;
static_assert(!kCanSubmitPackets, "GPU queues, GART and fences not implemented");
} // namespace sdma6
} // namespace navi33
