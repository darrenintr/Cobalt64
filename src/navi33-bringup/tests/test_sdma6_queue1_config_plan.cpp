// Host-only review of the read-only Queue1 configuration manifest.
// No fake MMIO callbacks, hardware writes, adapter, or GPU execution.
#include "../src/Navi33Sdma6Queue1ConfigPlan.h"
#include "rx7600_ipd_fixture.h"
#include <cassert>
#include <cstdio>
#include <vector>

using namespace navi33;
namespace q1=queue1_plan;

struct Fixture {
    std::vector<uint8_t> ipdBytes=rx7600_fixture::blob();
    IPDiscovery discovery{};
    q1::Input input{};

    Fixture() {
        assert(discovery.parse(ipdBytes.data(),ipdBytes.size()));
        input.discovery=&discovery;
        input.geometry.mcStart=0x8000000000ull;
        input.geometry.mcWindowBytes=8ull*1024ull*1024ull*1024ull;
        input.geometry.physicalFbBase=0x8000000000ull;
        input.geometry.realBytes=8ull*1024ull*1024ull*1024ull;
        input.geometry.cpuVisibleBytes=256u*1024u*1024u;
        input.bar0Bytes=256u*1024u*1024u;
        input.bar5Bytes=rx7600_fixture::kBar5;
        input.rootLease.valid=true;
        input.rootLease.rootGpuMcAddress=0x8000004000ull;
        input.rootLease.rootPhysicalAddress=0x8000004000ull;
        input.rootLease.cpuBar0Offset=0x4000u;
        input.rootLease.reservation.mcVramStart=0x8000000000ull;
        input.rootLease.reservation.mcVramBytes=8ull*1024ull*1024ull*1024ull;
        input.rootLease.reservation.ownedBoMcStart=0x8000004000ull;
        input.rootLease.reservation.ownedBoBytes=q1::kGartRootBytes;
        input.rootLease.reservation.rootMcAddress=
            input.rootLease.reservation.ownedBoMcStart;
        input.gart.gpuPhysicalPageTable=
            input.rootLease.reservation.rootMcAddress;
        input.rootReadbackMcAddress=
            input.rootLease.reservation.rootMcAddress;
        input.gart.gpuVirtualBase=staging::kGuestGpuVa;
        input.gart.mappedBytes=staging::kBytes;
        for(size_t i=0u;i<staging::kPages;++i) {
            const auto pte=gmc11::makeSystemPte(
                0x10000000ull+i*staging::kBytePage,
                i!=staging::kSourcePage);
            assert(pte.code==gmc11::Code::Pass);
            input.gart.entries[i]=pte.bits;
        }
        input.rootReadback.code=vram_root::Code::ImageMatches;
        input.rootReadback.checkedWords=static_cast<uint32_t>(q1::kGartRootWords);
        input.rootReadback.hardwareVmReady=false;
        input.queue.sourceGpuVa=staging::kGuestGpuVa;
        input.queue.targetGpuVa=input.queue.sourceGpuVa+staging::kBytePage;
        input.queue.ringGpuVa=input.queue.sourceGpuVa+2u*staging::kBytePage;
        input.queue.fenceGpuVa=input.queue.sourceGpuVa+3u*staging::kBytePage;
        input.queue.cookie=staging::kFenceCookie;
        input.queue.vmid=1u;
        input.queue.queueIndex=1u;
        input.mmhub.complete=true;
        input.mmhub.contextControl=0x01fffe00u;
        input.gfxhub.complete=true;
        input.gfxhub.contextControl=0x01fffe00u;
        input.queueSnapshotsComplete=true;
        input.evidence.exclusivePciAndBothVmHubs=true;
        input.evidence.exactPci1002_7480RevisionCf=true;
        input.evidence.noDisplayOrOtherGpuClients=true;
        input.evidence.psp13AndSdma6FirmwareVerified=true;
        input.evidence.mmhubCacheAndHdpInitialized=true;
        input.evidence.allDmaPagesPinnedAndMappedForThisPciDevice=true;
        input.evidence.deviceCacheMaintenanceAvailable=true;
        input.evidence.pairedGmcGeometryAndBar0Verified=true;
        input.evidence.separatelyOwnedPinnedVramRoot=true;
        input.evidence.exclusiveSdma0Queue1Owner=true;
        input.evidence.independentGpuRecovery=true;
        input.evidence.explicitApprovalForThisBoundedAttempt=true;
    }
};

static size_t findStep(const q1::Manifest &m,q1::Action action,
                       q1::Target target,uint64_t offset=0u) {
    for(size_t i=0u;i<m.stepCount;++i)
        if(m.steps[i].action==action && m.steps[i].target==target &&
           (!offset || m.steps[i].bar5ByteOffset==offset))return i;
    return m.stepCount;
}

static bool hasWrite(const q1::Manifest &m,q1::Target target,
                     uint64_t offset,uint32_t value) {
    for(size_t i=0u;i<m.stepCount;++i)
        if(m.steps[i].action==q1::Action::ProposedWrite &&
           m.steps[i].target==target &&
           m.steps[i].bar5ByteOffset==offset && m.steps[i].value==value)
            return true;
    return false;
}
static size_t findWrite(const q1::Manifest &m,q1::Target target,
                        uint64_t offset,uint32_t value) {
    for(size_t i=0u;i<m.stepCount;++i)
        if(m.steps[i].action==q1::Action::ProposedWrite &&
           m.steps[i].target==target &&
           m.steps[i].bar5ByteOffset==offset && m.steps[i].value==value)
            return i;
    return m.stepCount;
}

static void successfulOfflineManifest() {
    Fixture f{};
    const auto m=q1::build(f.input);
    assert(m.code==q1::Code::ManifestOnly && m.refusalMask==0u);
    assert(!m.hardwareReady() && !m.wroteHardware());
    assert(m.rootGpuMcAddress==0x8000004000ull);
    assert(m.mappedBytes==64u*1024u && m.copyBytes==4096u);
    assert(m.rootPteCount==16u && m.unusedRootPteCount==496u);
    assert(m.ringDwords==1024u && m.stopAtFirstFailure &&
           m.quarantineBeforeFirstWrite && !m.releaseMemoryPermitted);
    assert(m.expectedFence==0xc0b1606au && m.stepCount<q1::kMaxSteps);
    assert(m.queue1Offsets[0u]==
           (0x1260ull+sdma6_mmio::kRbCntl+
            sdma6_mmio::kQueue1DwordDelta)*4ull);
    assert(m.queue1Offsets[7u]==
           (0x1260ull+sdma6_mmio::kDoorbell+
            sdma6_mmio::kQueue1DwordDelta)*4ull);
    assert(m.queue1Offsets[0u]!=
           (0x1260ull+sdma6_mmio::kRbCntl)*4ull); // Queue0 excluded

    const size_t mmTableLo=static_cast<size_t>(mmhub11::Register::TableLo);
    const size_t mmStartLo=static_cast<size_t>(mmhub11::Register::StartLo);
    const size_t mmEndLo=static_cast<size_t>(mmhub11::Register::EndLo);
    const size_t mmReq=static_cast<size_t>(mmhub11::Register::InvalidateReq);
    assert(m.mmhubOffsets[mmTableLo]==(0x1a000ull+0x072dull)*4ull);
    assert(m.gfxhubOffsets[mmTableLo]==(0x1260ull+0x16f5ull)*4ull);
    assert(hasWrite(m,q1::Target::Mmhub,m.mmhubOffsets[mmTableLo],
                    0x00004001u));
    assert(hasWrite(m,q1::Target::Mmhub,
        m.mmhubOffsets[static_cast<size_t>(mmhub11::Register::TableHi)],
        0x80u));
    assert(hasWrite(m,q1::Target::Gfxhub,m.gfxhubOffsets[mmTableLo],
                    0x00004001u));
    assert(hasWrite(m,q1::Target::Mmhub,m.mmhubOffsets[mmStartLo],0x100000u));
    assert(hasWrite(m,q1::Target::Mmhub,m.mmhubOffsets[mmEndLo],0x10000fu));
    assert(hasWrite(m,q1::Target::Mmhub,m.mmhubOffsets[mmReq],0x00f80002u));
    assert(hasWrite(m,q1::Target::Gfxhub,m.gfxhubOffsets[mmReq],0x00f80002u));
    assert(hasWrite(m,q1::Target::Mmhub,
        m.mmhubOffsets[static_cast<size_t>(mmhub11::Register::Context)],
        0x01fffe01u));
    assert(hasWrite(m,q1::Target::Gfxhub,
        m.gfxhubOffsets[static_cast<size_t>(mmhub11::Register::Context)],
        0x01fffe01u));
    assert(hasWrite(m,q1::Target::Sdma0Queue1,m.queue1Offsets[1u],
                    0x01000020u)); // Queue1 RB_BASE is in 256-byte units.
    assert(hasWrite(m,q1::Target::Sdma0Queue1,m.queue1Offsets[0u],
                    0x01800014u)); // parked, VMID1, 1024 DWORD ring.
    assert(hasWrite(m,q1::Target::Sdma0Queue1,m.queue1Offsets[0u],
                    0x01800015u)); // enabled only after both TLB ACKs.
    assert(hasWrite(m,q1::Target::Sdma0Queue1,m.queue1Offsets[5u],64u));
    assert(m.rootPtes[0u]==f.input.gart.entries[0u]);

    const size_t dualReady=findStep(m,q1::Action::RequireDualHubReady,
                                    q1::Target::Host);
    const size_t queueRecheck=findStep(m,q1::Action::RecheckQueue1Snapshot,
                                       q1::Target::Sdma0Queue1);
    const size_t queueEnable=findWrite(m,q1::Target::Sdma0Queue1,
        m.queue1Offsets[0u],0x01800015u);
    const size_t queueSubmit=findWrite(m,q1::Target::Sdma0Queue1,
        m.queue1Offsets[5u],sdma6_mmio::kCommandsBytes);
    const size_t memoryRetain=findStep(m,
        q1::Action::RetainMemoryUntilIndependentReset,q1::Target::Host);
    assert(dualReady<queueRecheck && queueRecheck<queueEnable &&
           queueEnable<queueSubmit && queueSubmit<memoryRetain);
    assert(findStep(m,q1::Action::VerifyOwnedVramRoot,q1::Target::Cpu)<
           findStep(m,q1::Action::ProposedWrite,q1::Target::Mmhub));
    assert(findStep(m,q1::Action::PollFence,q1::Target::Cpu)<memoryRetain);
}

static void refusalsAreExplicit() {
    Fixture f{};
    auto input=f.input;
    input.evidence.exclusivePciAndBothVmHubs=false;
    input.evidence.noDisplayOrOtherGpuClients=false;
    input.evidence.independentGpuRecovery=false;
    auto m=q1::build(input);
    assert(m.code==q1::Code::Refused && m.stepCount==0u);
    assert((m.refusalMask&q1::MissingPciOwner)!=0u);
    assert((m.refusalMask&q1::DisplayOrOtherGpuClient)!=0u);
    assert((m.refusalMask&q1::RecoveryMissing)!=0u);

    input=f.input;input.evidence.exactPci1002_7480RevisionCf=false;
    assert(q1::build(input).refusalMask&q1::WrongPciIdentity);
    input=f.input;input.evidence.pairedGmcGeometryAndBar0Verified=false;
    assert(q1::build(input).refusalMask&q1::GmcGeometryMissing);
    input=f.input;input.evidence.explicitApprovalForThisBoundedAttempt=false;
    assert(q1::build(input).refusalMask&q1::ApprovalMissing);

    input=f.input;input.queue.queueIndex=0u;
    assert(q1::build(input).refusalMask&q1::BadQueueRequest);
    input=f.input;input.queue.vmid=0u;
    assert(q1::build(input).refusalMask&q1::BadQueueRequest);
    input=f.input;input.rootLease.reservation.ownedBoBytes=
        2u*q1::kGartRootBytes;
    assert(q1::build(input).refusalMask&q1::BadRootReservation);
    input=f.input;input.rootReadback.checkedWords=511u;
    assert(q1::build(input).refusalMask&q1::RootReadbackIncomplete);
    input=f.input;input.rootReadbackMcAddress+=staging::kBytePage;
    assert(q1::build(input).refusalMask&q1::RootReadbackIncomplete);
    input=f.input;input.geometry.physicalFbBase+=16u*1024u*1024u;
    assert(q1::build(input).refusalMask&q1::BadRootReservation);
    input=f.input;input.gart.entries[2u]=input.gart.entries[1u];
    assert(q1::build(input).refusalMask&q1::BadGartImage);
    input=f.input;input.mmhub.invalidateRequest=1u;
    assert(q1::build(input).refusalMask&q1::HubBusy);
    input=f.input;input.gfxhub.invalidateAck=q1::kQueue1AckMask;
    assert(q1::build(input).refusalMask&q1::HubBusy);
    input=f.input;input.queueSnapshotsComplete=false;
    assert(q1::build(input).refusalMask&q1::QueueSnapshotMissing);
    input=f.input;input.queueSecond.values[4u]=4u;
    assert(q1::build(input).refusalMask&q1::QueueSnapshotUnstable);
    input=f.input;input.queueFirst.values[1u]=1u;
    input.queueSecond=input.queueFirst;
    assert(q1::build(input).refusalMask&q1::QueueBusy);
    input=f.input;input.bar5Bytes=4u;
    assert(q1::build(input).refusalMask&q1::BadBarRange);
    input=f.input;input.queuePollBudget=0u;
    assert(q1::build(input).refusalMask&q1::InvalidPollBudget);
}

int main() {
    static_assert(!q1::kCanAccessHardware && !q1::kCanWriteHardware &&
                  !q1::kCanPublishCapabilities,
                  "Manifest must never touch a device or advertise readiness");
    successfulOfflineManifest();
    refusalsAreExplicit();
    std::puts("Navi33 VMID1/Queue1 read-only config manifest: PASS (offline)");
}
