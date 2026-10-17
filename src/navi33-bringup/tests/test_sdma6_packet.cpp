// CPU-side SDMA6 packet round-trip and boundary tests. NO GPU.
#include "../src/Navi33Sdma6Packet.h"
#include <cassert>
#include <cstdio>
#include <cstdint>
using namespace navi33::sdma6;

int main(){
    static_assert(!kCanSubmitPackets, "This is only an offline encoder");
    const uint64_t src=0x00000001abcd0000ull;
    const uint64_t dst=0x00000002cdef0000ull;
    const auto packet=makeCopy(src,dst,4096u);
    assert(packet.status==CopyCode::Pass);
    assert(packet.words[0]==1u);
    assert(packet.words[1]==4095u);
    assert(packet.words[2]==0u);
    assert(packet.words[3]==0xabcd0000u);
    assert(packet.words[4]==1u);
    assert(packet.words[5]==0xcdef0000u);
    assert(packet.words[6]==2u);
    auto decoded=inspectCopy(packet.words,kCopyPacketDwords);
    assert(decoded.status==CopyCode::Pass);
    assert(decoded.source==src&&decoded.destination==dst&&decoded.bytes==4096u);

    assert(makeCopy(src,dst,0u).status==CopyCode::EmptyCopy);
    assert(makeCopy(src,dst,kMaxCopyBytes+1u).status==CopyCode::TooLarge);
    assert(makeCopy(src,dst,kMaxCopyBytes).status==CopyCode::Pass);
    assert(makeCopy(0,dst,4096u).status==CopyCode::InvalidAddress);
    assert(makeCopy(src,dst+1u,4096u).status==CopyCode::InvalidAddress);
    assert(makeCopy(UINT64_MAX-3u,dst,4096u).status==CopyCode::AddressOverflow);
    assert(makeCopy(src,src,4096u).status==CopyCode::OverlappingMemory);
    assert(makeCopy(src,src+4092u,4096u).status==CopyCode::OverlappingMemory);
    assert(makeCopy(src,src+4096u,4096u).status==CopyCode::Pass);
    assert(inspectCopy(nullptr,kCopyPacketDwords).status==CopyCode::EmptyCopy);
    assert(inspectCopy(packet.words,kCopyPacketDwords-1u).status==CopyCode::EmptyCopy);
    uint32_t corrupted[kCopyPacketDwords]{};
    for(size_t i=0;i<kCopyPacketDwords;++i)corrupted[i]=packet.words[i];
    corrupted[0]=(1u<<18u)|1u; // Protected/TMZ copy must not be silently allowed.
    assert(inspectCopy(corrupted,kCopyPacketDwords).status==CopyCode::EmptyCopy);
    corrupted[0]=packet.words[0]; corrupted[1]=kMaxCopyBytes;
    assert(inspectCopy(corrupted,kCopyPacketDwords).status==CopyCode::TooLarge);
    std::puts("Navi33 SDMA6 linear-copy packet encoding and round-trip guards: PASS (offline)");
}
