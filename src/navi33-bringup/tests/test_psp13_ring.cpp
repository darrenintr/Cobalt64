// Host test for the PSP13 KM ring protocol helpers (Navi33Psp13Ring.h).
#include "../src/Navi33Psp13Ring.h"
#include <assert.h>
#include <stdio.h>
namespace pr = navi33::psp13ring;

int main() {
    // Offsets are upstream mp_13_0_2_offset.h values; C2PMSG_n = 0x40 + n.
    assert(pr::kC2pmsg64==0x40u+64u && pr::kC2pmsg67==0x40u+67u);
    assert(pr::kC2pmsg69==0x40u+69u && pr::kC2pmsg70==0x40u+70u);
    assert(pr::kC2pmsg71==0x40u+71u);
    // The ring writer may never touch the bootloader registers.
    for(uint32_t r:pr::kWritableRegisters) assert(r!=0x63u && r!=0x64u);

    // Creation values: PSP_RING_TYPE__KM << 16 = 0x00020000.
    pr::CreatePlan p=pr::planCreate({0x80'00f0'1000ull,pr::kRingBytes});
    assert(p.ok && p.r69==0x00f01000u && p.r70==0x80u && p.r71==0x1000u &&
           p.r64==0x00020000u);
    // Bad rings are refused: unaligned, wrong size, null, address wrap.
    assert(!pr::planCreate({0x80'00f0'1800ull,pr::kRingBytes}).ok);
    assert(!pr::planCreate({0x80'00f0'1000ull,0x2000u}).ok);
    assert(!pr::planCreate({0u,pr::kRingBytes}).ok);
    assert(!pr::planCreate({0xffff'ffff'ffff'f000ull,pr::kRingBytes}).ok);

    // Response check: bit 31 set and the status half zero. An error status
    // (low half non-zero) or a missing flag is never a success.
    assert(pr::respOk(0x80000000u));
    assert(!pr::respOk(0x80000001u) && !pr::respOk(0x00000000u));
    assert(!pr::respOk(0x8000ffffu) && pr::respOk(0xffff0000u|0x80000000u));

    // Write pointer: dwords, frame = 16 dwords, ring = 1024 dwords, wraps.
    pr::Slot s{};
    assert(pr::nextSlot(0u,pr::kRingBytes,s) && s.offsetBytes==0u &&
           s.nextWptr==16u);
    assert(pr::nextSlot(16u,pr::kRingBytes,s) && s.offsetBytes==64u &&
           s.nextWptr==32u);
    assert(pr::nextSlot(1008u,pr::kRingBytes,s) && s.offsetBytes==4032u &&
           s.nextWptr==0u);                       // last frame wraps to 0
    assert(!pr::nextSlot(8u,pr::kRingBytes,s));   // not frame aligned
    assert(!pr::nextSlot(1024u,pr::kRingBytes,s)); // past the end
    assert(!pr::nextSlot(0u,0x2000u,s));

    // Frame layout: 64 bytes, fields at the upstream offsets.
    pr::Frame f=pr::makeFrame(0x80'0f10'0000ull,0x1000u,0x80'0f11'0000ull,7u);
    const uint32_t *w=reinterpret_cast<const uint32_t *>(&f);
    assert(w[0]==0x0f100000u && w[1]==0x80u && w[2]==0x1000u);
    assert(w[3]==0x0f110000u && w[4]==0x80u && w[5]==7u);
    assert(w[6]==0u && w[7]==0u && f.vmid==0u && f.frame_type==0u);
    for(unsigned i=9;i<16;++i) assert(w[i]==0u);

    // --- command buffers (upstream psp_gfx_if.h layout) ---
    uint8_t cmd[pr::kCmdBytes];
    assert(pr::prepLoadToc(cmd,0x80'00f0'0000ull,2304u));
    assert(pr::get32(cmd,0)==0u && pr::get32(cmd,4)==0u);   // buf_size/version unset
    assert(pr::get32(cmd,8)==0x20u && pr::get32(cmd,28)==0x00f00000u &&
           pr::get32(cmd,32)==0x80u && pr::get32(cmd,36)==2304u);
    assert(pr::get32(cmd,40)==0u && pr::get32(cmd,pr::kRespOff)==0u);
    assert(!pr::prepLoadToc(cmd,0x80'00f0'0800ull,2304u));  // not 4 KiB aligned
    assert(!pr::prepLoadToc(cmd,0x80'00f0'0000ull,0u));
    // TMR: 4 MiB, naturally aligned; offsets +28.. in the command body.
    assert(pr::tmrValid(0x80'0040'0000ull,0x400000u));
    assert(!pr::tmrValid(0x80'0010'0000ull,0x400000u));      // not size-aligned
    assert(!pr::tmrValid(0x80'0040'0000ull,0x401000u));      // not a power of 2
    assert(!pr::tmrValid(0x80'0040'0000ull,0xc00u));         // not 4 KiB multiple
    assert(pr::prepSetupTmr(cmd,0x80'0040'0000ull,0x400000u,0x40'0000ull));
    assert(pr::get32(cmd,8)==5u && pr::get32(cmd,28)==0x00400000u &&
           pr::get32(cmd,32)==0x80u && pr::get32(cmd,36)==0x400000u &&
           pr::get32(cmd,40)==2u && pr::get32(cmd,44)==0x400000u &&
           pr::get32(cmd,48)==0u);
    assert(!pr::prepSetupTmr(cmd,0x80'0040'0000ull,0u,0u));
    assert(!pr::prepSetupTmr(cmd,0x80'0040'0000ull,1ull<<32u,0u));
    // Fence and status.
    assert(pr::fenceDone(3u,3u) && !pr::fenceDone(2u,3u));
    pr::put32(cmd,pr::kRespStatusOff,0u);
    assert(pr::cmdOk(cmd));
    pr::put32(cmd,pr::kRespStatusOff,0xffff000cu);
    assert(!pr::cmdOk(cmd));
    pr::put32(cmd,pr::kRespTmrSizeOff,0x400000u);
    assert(pr::get32(cmd,880)==0x400000u);                  // resp.tmr_size

    puts("PSP13 KM ring protocol tests passed");
    return 0;
}
