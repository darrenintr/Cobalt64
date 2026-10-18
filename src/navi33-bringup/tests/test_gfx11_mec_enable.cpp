// Host-only checks for the RS64 MEC enable word (no hardware).
#include "../src/Navi33Gfx11MecEnable.h"
#include <assert.h>
#include <stdio.h>

namespace mec = navi33::gfx11_mec;

static void offsetsMatchUpstream() {
    // gc_11_0_0_offset.h: regCP_MEC_RS64_CNTL 0x2904, BASE_IDX 1.
    static_assert(mec::kCpMecRs64CntlDword==0x2904u, "dword offset");
    static_assert(mec::kCpMecRs64CntlSegment==1u, "segment");
}

static void enableFromReset() {
    // Fresh enable from an all-zero register sets all four pipes active.
    const uint32_t word=mec::rs64CntlWord(0u,true);
    assert(word==0x3c000000u);
    assert((word&mec::kMecHalt)==0u);
    assert((word&mec::kMecPipeResetAll)==0u);
    assert((word&mec::kMecInvalidateIcache)==0u);
}

static void disableSetsHaltAndResets() {
    const uint32_t word=mec::rs64CntlWord(0x3c000000u,false);
    assert(word==0x400f0010u);
    assert((word&mec::kMecPipeActiveAll)==0u);
}

static void keepsUnrelatedBits() {
    // Bits outside the four touched fields survive both directions. Disable
    // sets ICACHE (bit 4), so compare only the untouched bits.
    const uint32_t touched=mec::kMecInvalidateIcache|mec::kMecPipeResetAll|
                           mec::kMecPipeActiveAll|mec::kMecHalt;
    const uint32_t untouched=~touched;
    const uint32_t inputs[]={0x0000ff01u,0x3c00ff01u,0xa5a5a5a5u};
    for(uint32_t in : inputs) {
        assert((mec::rs64CntlWord(in,true)&untouched)==(in&untouched));
        assert((mec::rs64CntlWord(in,false)&untouched)==(in&untouched));
    }
}

static void roundTripIsStable() {
    uint32_t reg=0x12345678u;
    reg=mec::rs64CntlWord(reg,false);
    reg=mec::rs64CntlWord(reg,true);
    assert(mec::rs64CntlWord(reg,true)==reg);
    assert((reg&mec::kMecHalt)==0u);
    assert((reg&mec::kMecPipeActiveAll)==mec::kMecPipeActiveAll);
}

int main() {
    offsetsMatchUpstream();
    enableFromReset();
    disableSetsHaltAndResets();
    keepsUnrelatedBits();
    roundTripIsStable();
    puts("Navi33 GFX11 MEC RS64 enable word: PASS (offline)");
}
