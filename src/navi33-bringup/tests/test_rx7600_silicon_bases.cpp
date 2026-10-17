// SPDX-License-Identifier: MIT
// Every other plan test uses invented discovery bases (0x100, 0x500, ...).
// This one uses the IP Discovery table Linux amdgpu parsed from a physical
// RX 7600 (1002:7480 rev cf, 1 MiB BAR5); see tests/fixtures/
// rx7600-linux-ipd.txt and tools/capture-linux-ipd.sh. The expected byte
// offsets are written out literally, computed as Linux does:
// (reg_offset[ip][0][BASE_IDX] + reg) * 4, with reg/BASE_IDX taken from
// gc_11_0_0, mmhub_3_0_2, mp_13_0_2, nbio_4_3_0 and dcn_3_2_1 _offset.h.
// No GPU or PCI access; this only checks address planning.
#include "../src/Navi33HardwarePlan.h"
#include "../src/Navi33Dcn321.h"
#include "../src/Navi33LiveMmio.h"
#include "../src/Navi33Psp13Status.h"
#include "../src/Navi33Sdma6Status.h"
#include "../src/Navi33Sdma6QueueCensus.h"
#include "../src/Navi33Sdma6BarAdapter.h"
#include "../src/Navi33Gmc11Mmhub.h"
#include "../src/Navi33Gmc11VramLive.h"
#include "../src/Navi33Nbio43Capacity.h"
#include "../src/Navi33HwProbeLogic.h"
#include "rx7600_ipd_fixture.h"
#include <cassert>
#include <cstdio>
#include <vector>

using namespace navi33;

using rx7600_fixture::kBar5;
using rx7600_fixture::blob;

int main() {
    const std::vector<uint8_t> bytes=blob();
    IPDiscovery disc{};
    assert(disc.parse(bytes.data(),bytes.size()));
    assert(inventory(disc).code==InventoryCode::Pass);
    assert(disc.isNavi33Candidate());

    // SDMA0 status and Queue0 census: GC BASE_IDX 0 = 0x1260.
    const auto sdma=sdma6live::buildPlan(disc,kBar5);
    assert(sdma.state==N33_SDMA_OBSERVED);
    assert(sdma.offsets[0]==0x4a14u);          // SDMA0_STATUS_REG
    assert(sdma.offsets[2]==0x4b80u);          // SDMA0_QUEUE0_RB_CNTL
    assert(sdma.offsets[4]==0x4b94u);          // SDMA0_QUEUE0_RB_WPTR
    const auto census=queue_census::fromDiscovery(disc,kBar5);
    assert(census.state==N33_QUEUE_OBSERVED);
    const uint64_t q0Census[queue_census::kCount]={
        0x4b80u,0x4bc8u,0x4b8cu,0x4b90u,0x4b94u,0x4b98u,0x4b84u,0x4b88u};
    for(size_t i=0;i<queue_census::kCount;++i)
        assert(census.offsets[i]==q0Census[i]);

    // SDMA6 ring bank, Queue0 and Queue1 (RB_CNTL ... MINOR_PTR_UPDATE).
    const auto q0=sdma6_bar::buildPlan(disc,kBar5,0u);
    const auto q1=sdma6_bar::buildPlan(disc,kBar5,1u);
    assert(q0.code==AddressCode::Pass && sdma6_bar::validPlan(q0));
    assert(q1.code==AddressCode::Pass && sdma6_bar::validPlan(q1));
    const uint64_t q0Bank[sdma6_bar::kCount]={
        0x4b80u,0x4b84u,0x4b88u,0x4b8cu,0x4b90u,0x4b94u,0x4b98u,
        0x4bc8u,0x4c54u};
    const uint64_t q1Bank[sdma6_bar::kCount]={
        0x4ce0u,0x4ce4u,0x4ce8u,0x4cecu,0x4cf0u,0x4cf4u,0x4cf8u,
        0x4d28u,0x4db4u};
    for(size_t i=0;i<sdma6_bar::kCount;++i) {
        assert(q0.byteOffset[i]==q0Bank[i]);
        assert(q1.byteOffset[i]==q1Bank[i]);
    }

    // GFXHUB (GC BASE_IDX 0) and MMHUB 3.0.2 (MMHUB BASE_IDX 0 = 0x1A000).
    using mmhub11::Register;
    auto at=[](const mmhub11::Plan &p,Register r){
        return p.offsets[static_cast<size_t>(r)];
    };
    const auto gfx0=mmhub11::planGfxFromDiscovery(disc,kBar5,0u);
    const auto gfx1=mmhub11::planGfxFromDiscovery(disc,kBar5,1u);
    const auto mm0=mmhub11::planFromDiscovery(disc,kBar5,0u);
    const auto mm1=mmhub11::planFromDiscovery(disc,kBar5,1u);
    for(const auto *p:{&gfx0,&gfx1,&mm0,&mm1})
        assert(p->code==mmhub11::Code::Prepared && mmhub11::validPlan(*p));
    assert(mm0.revision==2u);
    assert(at(gfx0,Register::Context)==0xa3a0u);           // GCVM_CONTEXT0_CNTL
    assert(at(gfx1,Register::Context)==0xa3a4u);           // GCVM_CONTEXT1_CNTL
    assert(at(gfx1,Register::TableLo)==0xa554u);           // ..CONTEXT1_PT_BASE_LO32
    assert(at(gfx0,Register::InvalidateReq)==0xa470u);     // GCVM_INVALIDATE_ENG17_REQ
    assert(at(gfx0,Register::InvalidateAck)==0xa4b8u);
    assert(at(gfx0,Register::PrivateInvalidate)==0xa0dcu); // GCVM_L2_BANK_SELECT_RESERVED_CID2
    assert(at(mm0,Register::Context)==0x69b00u);           // MMVM_CONTEXT0_CNTL
    assert(at(mm1,Register::TableLo)==0x69cb4u);           // MMVM_CONTEXT1_PT_BASE_LO32
    assert(at(mm0,Register::InvalidateReq)==0x69bd0u);
    assert(at(mm0,Register::PrivateInvalidate)==0x69a6cu);
    const auto hubs=hwprobe::planHubs(disc,kBar5);
    assert(hubs.valid && hubs.mmhub==0x69b00u && hubs.gfxhub==0xa3a0u);

    // FB location: MMMC_VM_FB_* (3.0.2) then GCMC_VM_FB_*.
    const auto fb=vram_live::fromDiscovery(disc,kBar5);
    assert(fb.code==vram_live::PlanCode::Pass && fb.mmRevision==2u);
    const uint64_t fbRegs[6]={0x6a1b0u,0x6a1b4u,0x6a15cu,
                              0xa360u,0xa364u,0xa01cu};
    for(size_t i=0;i<6;++i)assert(fb.offsets[i]==fbRegs[i]);

    // PSP13 sOS mailbox: MP0 BASE_IDX 0 = 0x16000.
    const auto psp=psp13::buildPlan(disc,kBar5);
    assert(psp.state==N33_PSP_STOPPED);
    assert(psp.aliveOffset==0x58244u);                     // MP0_SMN_C2PMSG_81
    assert(psp.versionOffset==0x581e8u);                   // MP0_SMN_C2PMSG_58
    assert(psp.bootloaderOffset==0x5818cu);                // MP0_SMN_C2PMSG_35

    // NBIO 4.3.1 RCC_CONFIG_MEMSIZE: NBIF BASE_IDX 2 = 0xD20. NBIF
    // BASE_IDX 0 is 0 on real silicon, so the index really matters.
    const auto nbio=nbio4::fromDiscovery(disc,kBar5);
    assert(nbio.code==nbio4::Code::Pass);
    assert(nbio.byteOffset==0x378cu);
    assert(nbio.nbioRevision==0x040301u);

    // DCN 3.2.1: DMU BASE_IDX 2 = 0x34C0.
    const auto otg=live_mmio::buildPlan(disc,kBar5);
    assert(otg.code==live_mmio::Code::Pass);
    assert(otg.byteOffsets[0][0]==0x14004u);               // OTG0_OTG_CONTROL
    assert(otg.byteOffsets[0][1]==0x14114u);               // OTG0_OTG_CLOCK_CONTROL
    assert(otg.byteOffsets[0][2]==0x14030u);               // OTG0_OTG_STATUS_FRAME_COUNT
    assert(otg.byteOffsets[3][2]==0x14630u);               // OTG3_OTG_STATUS_FRAME_COUNT
    const auto dcn=dcn321::buildReadPlan(disc,kBar5);
    assert(dcn.code==AddressCode::Pass);
    assert(dcn.offsets[static_cast<size_t>(dcn321::Register::VmFbBase)]==0xe4d4u);
    assert(dcn.offsets[static_cast<size_t>(dcn321::Register::Pipe0SurfaceLo)]==0xeb28u);
    assert(dcn.offsets[static_cast<size_t>(dcn321::Register::Pipe0FrameCount)]==0x14030u);

    std::puts("RX 7600 silicon IP Discovery bases resolve to Linux amdgpu BAR5 offsets");
}
