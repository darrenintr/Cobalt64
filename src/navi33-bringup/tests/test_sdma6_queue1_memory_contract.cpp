// Offline typed memory/VM contract review. No allocator, BAR, MMIO or GPU.
#include "../src/Navi33Sdma6Queue1MemoryContract.h"
#include "rx7600_ipd_fixture.h"
#include <cassert>
#include <cstdio>
#include <vector>

using namespace navi33;
namespace q1=queue1_plan;
namespace mem=queue1_memory;

struct Fixture {
    std::vector<uint8_t> ipdBytes=rx7600_fixture::blob();
    IPDiscovery discovery{};
    q1::Input planInput{};
    q1::Manifest plan{};
    mem::Input input{};

    Fixture() {
        assert(discovery.parse(ipdBytes.data(),ipdBytes.size()));
        planInput.discovery=&discovery;
        planInput.geometry.mcStart=0x8000000000ull;
        planInput.geometry.mcWindowBytes=8ull*1024ull*1024ull*1024ull;
        planInput.geometry.physicalFbBase=0x8000000000ull;
        planInput.geometry.realBytes=8ull*1024ull*1024ull*1024ull;
        planInput.geometry.cpuVisibleBytes=256u*1024u*1024u;
        planInput.bar0Bytes=256u*1024u*1024u;
        planInput.bar5Bytes=rx7600_fixture::kBar5;
        planInput.rootLease.valid=true;
        planInput.rootLease.rootGpuMcAddress=0x8000004000ull;
        planInput.rootLease.rootPhysicalAddress=0x8000004000ull;
        planInput.rootLease.cpuBar0Offset=0x4000u;
        planInput.rootLease.reservation.mcVramStart=0x8000000000ull;
        planInput.rootLease.reservation.mcVramBytes=
            8ull*1024ull*1024ull*1024ull;
        planInput.rootLease.reservation.ownedBoMcStart=0x8000004000ull;
        planInput.rootLease.reservation.ownedBoBytes=q1::kGartRootBytes;
        planInput.rootLease.reservation.rootMcAddress=0x8000004000ull;
        planInput.gart.gpuPhysicalPageTable=0x8000004000ull;
        planInput.gart.gpuVirtualBase=staging::kGuestGpuVa;
        planInput.gart.mappedBytes=staging::kBytes;
        for(size_t i=0u;i<staging::kPages;++i) {
            const auto pte=gmc11::makeSystemPte(
                0x10000000ull+i*staging::kBytePage,
                i!=staging::kSourcePage);
            assert(pte.code==gmc11::Code::Pass);
            planInput.gart.entries[i]=pte.bits;
        }
        planInput.rootReadbackMcAddress=0x8000004000ull;
        planInput.rootReadback.code=vram_root::Code::ImageMatches;
        planInput.rootReadback.checkedWords=
            static_cast<uint32_t>(q1::kGartRootWords);
        planInput.queue.sourceGpuVa=staging::kGuestGpuVa;
        planInput.queue.targetGpuVa=staging::kGuestGpuVa+staging::kBytePage;
        planInput.queue.ringGpuVa=staging::kGuestGpuVa+2u*staging::kBytePage;
        planInput.queue.fenceGpuVa=staging::kGuestGpuVa+3u*staging::kBytePage;
        planInput.queue.cookie=staging::kFenceCookie;
        planInput.queue.vmid=q1::kQueue1Vmid;
        planInput.queue.queueIndex=q1::kQueue1Index;
        planInput.mmhub.complete=true;
        planInput.mmhub.contextControl=0x01fffe00u;
        planInput.gfxhub.complete=true;
        planInput.gfxhub.contextControl=0x01fffe00u;
        planInput.queueSnapshotsComplete=true;
        planInput.evidence.exactPci1002_7480RevisionCf=true;
        planInput.evidence.exclusivePciAndBothVmHubs=true;
        planInput.evidence.noDisplayOrOtherGpuClients=true;
        planInput.evidence.psp13AndSdma6FirmwareVerified=true;
        planInput.evidence.mmhubCacheAndHdpInitialized=true;
        planInput.evidence.allDmaPagesPinnedAndMappedForThisPciDevice=true;
        planInput.evidence.deviceCacheMaintenanceAvailable=true;
        planInput.evidence.pairedGmcGeometryAndBar0Verified=true;
        planInput.evidence.separatelyOwnedPinnedVramRoot=true;
        planInput.evidence.exclusiveSdma0Queue1Owner=true;
        planInput.evidence.independentGpuRecovery=true;
        planInput.evidence.explicitApprovalForThisBoundedAttempt=true;
        plan=q1::build(planInput);
        assert(plan.code==q1::Code::ManifestOnly);

        input.plan=&plan;
        input.device={0x11u,0x22u,0x1002u,0x7480u,0xcfu};
        input.stagingObject.objectId=0x100u;
        input.stagingObject.generation=1u;
        input.stagingObject.ownerProviderId=0x11u;
        input.stagingObject.dmaDomainId=0x22u;
        input.stagingObject.bytes=staging::kBytes;
        input.stagingObject.pinned=true;
        input.stagingObject.mappedToThisDevice=true;
        input.stagingObject.cacheMaintenanceAvailable=true;
        input.stagingObject.retainedUntilIndependentReset=true;
        for(size_t i=0u;i<staging::kPages;++i)
            input.stagingObject.dmaPageAddresses[i]=
                plan.rootPtes[i]&gmc11::kAddressMask;
        input.rootObject.objectId=0x200u;
        input.rootObject.generation=2u;
        input.rootObject.ownerProviderId=0x11u;
        input.rootObject.gpuPhysicalAddress=0x8000004000ull;
        input.rootObject.bytes=q1::kGartRootBytes;
        input.rootObject.ownedVramBo=true;
        input.rootObject.pinned=true;
        input.rootObject.exclusive=true;
        input.rootObject.retainedUntilIndependentReset=true;
        input.gpuVa={0x300u,3u,0x11u,staging::kGuestGpuVa,
                     staging::kBytes,true,true};
    }
};

static void contractBindsAllProofMemory() {
    Fixture f{};
    const auto c=mem::build(f.input);
    assert(c.code==mem::Code::ContractOnly && c.refusalMask==0u);
    assert(!c.hardwareReady() && !c.mayReleaseBacking() &&
           !c.isHardwareEvidence);
    assert(c.providerId==0x11u && c.dmaDomainId==0x22u);
    assert(c.stagingObjectId==0x100u && c.stagingGeneration==1u);
    assert(c.rootObjectId==0x200u && c.rootGeneration==2u);
    assert(c.gpuVaReservationId==0x300u && c.gpuVaGeneration==3u);
    assert(c.rootGpuPhysicalAddress==0x8000004000ull);
    assert(c.gpuVaBase==staging::kGuestGpuVa &&
           c.mappedBytes==staging::kBytes &&
           c.mappedPageCount==staging::kPages);
    for(size_t i=0u;i<staging::kPages;++i) {
        const auto &page=c.pages[i];
        assert(page.gpuVa==staging::kGuestGpuVa+i*staging::kBytePage);
        assert(page.dmaAddress==f.input.stagingObject.dmaPageAddresses[i]);
        assert(page.role==mem::roleForPage(i));
        assert(page.gpuReadable);
        assert(page.gpuWritable==(i!=staging::kSourcePage));
    }
    static_assert(!mem::kCanAllocate && !mem::kCanPinOrMap &&
                  !mem::kCanAccessHardware &&
                  !mem::kCanPublishCapabilities,
                  "Contract must not create a backend");
}

static void refusalCases() {
    Fixture f{};
    auto in=f.input;
    in.device.providerId=0u;
    assert(mem::build(in).refusalMask&mem::MissingOwnerIdentity);
    in=f.input;in.device.deviceId=0x7440u;
    assert(mem::build(in).refusalMask&mem::WrongPciIdentity);
    in=f.input;in.stagingObject.dmaDomainId++;
    assert(mem::build(in).refusalMask&mem::OwnerMismatch);
    in=f.input;in.stagingObject.pinned=false;
    assert(mem::build(in).refusalMask&mem::BadStagingObject);
    in=f.input;in.stagingObject.bytes-=staging::kBytePage;
    assert(mem::build(in).refusalMask&mem::BadStagingObject);
    in=f.input;in.stagingObject.dmaPageAddresses[4u]+=
        staging::kBytePage;
    assert(mem::build(in).refusalMask&mem::BadPageMapping);
    in=f.input;in.stagingObject.dmaPageAddresses[7u]=
        in.stagingObject.dmaPageAddresses[6u];
    assert(mem::build(in).refusalMask&mem::BadPageMapping);
    in=f.input;in.rootObject.ownedVramBo=false;
    assert(mem::build(in).refusalMask&mem::BadVramRootObject);
    in=f.input;in.rootObject.gpuPhysicalAddress+=staging::kBytePage;
    assert(mem::build(in).refusalMask&mem::BadVramRootObject);
    in=f.input;in.gpuVa.exclusive=false;
    assert(mem::build(in).refusalMask&mem::BadGpuVaReservation);
    in=f.input;in.gpuVa.bytes-=staging::kBytePage;
    assert(mem::build(in).refusalMask&mem::BadGpuVaReservation);
    in=f.input;in.gpuVa.base+=staging::kBytePage;
    assert(mem::build(in).refusalMask&mem::BadGpuVaReservation);
    f.plan.ringGpuVa+=staging::kBytePage;
    in=f.input;
    // A manifest whose ring address drifted from the reservation is refused.
    assert(mem::build(in).refusalMask&mem::BadQueueBinding);
    f.plan.ringGpuVa-=staging::kBytePage;
    in=f.input;in.rootObject.retainedUntilIndependentReset=false;
    assert(mem::build(in).refusalMask&mem::MissingRetention);
    f.plan.code=q1::Code::Refused;
    in=f.input;
    assert(mem::build(in).refusalMask&mem::MissingPlan);
}

int main() {
    contractBindsAllProofMemory();
    refusalCases();
    std::puts("Navi33 Queue1 memory/GPU-VA contract: PASS (offline)");
}
