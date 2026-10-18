// SPDX-License-Identifier: MIT
// Navi33-only, side-effect-free manifest for a bounded GMC11 + SDMA6 proof.
// It describes proposed values and required readbacks; it has no MMIO, BAR,
// DMA, VRAM, callback, or execution interface and is not part of the Kext.
// Sources: Linux amdgpu gmc_v11_0.c, gfxhub_v3_0.c, mmhub_v3_0_2.c,
// gc_11_0_0_offset.h, mmhub_3_0_2_offset.h and sdma_v6_0.c.
#pragma once

#include "Navi33Gmc11VramLease.h"
#include "Navi33Gmc11RootVram.h"
#include "Navi33Sdma6MmioQueue.h"
#include "Navi33Sdma6Packet.h"
#include <stddef.h>
#include <stdint.h>

namespace navi33 { namespace queue1_plan {

// This means only that a host-readable manifest was produced. It is never a
// GPU/Metal readiness result and must not feed an IOAccelerator capability.
enum class Code : uint8_t { Refused=0, ManifestOnly, StepOverflow };
enum class Target : uint8_t { Cpu=0, Mmhub, Gfxhub, Sdma0Queue1, Host };
enum class Action : uint8_t {
    VerifyStagingImage=0,
    VerifyOwnedVramRoot,
    RequireSnapshot,
    QuarantineBackingBeforeFirstWrite,
    SyncGartForDevice,
    ProposedWrite,
    RequireReadback,
    // Provider-owned HDP/cache operation; offset 0 means no register
    // address or value is guessed by this host-side manifest.
    FlushHdp,
    AcquireMmhubInvalidateSemaphore,
    PollRegisterBits,
    ReleaseMmhubInvalidateSemaphore,
    RequireDualHubReady,
    RecheckQueue1Snapshot,
    SyncQueueForGpu,
    PollFence,
    VerifyCopyReadback,
    ProposedQueuePark,
    RetainMemoryUntilIndependentReset
};

enum Refusal : uint32_t {
    MissingPciOwner          = 1u<<0u,
    DisplayOrOtherGpuClient  = 1u<<1u,
    FirmwareNotVerified      = 1u<<2u,
    HubCacheOrHdpNotReady    = 1u<<3u,
    DmaOwnershipMissing      = 1u<<4u,
    CacheSyncMissing         = 1u<<5u,
    GmcGeometryMissing       = 1u<<6u,
    RootOwnershipMissing     = 1u<<7u,
    Queue1OwnershipMissing   = 1u<<8u,
    RecoveryMissing          = 1u<<9u,
    WrongPciIdentity         = 1u<<10u,
    ApprovalMissing          = 1u<<11u,
    BadDiscovery             = 1u<<12u,
    BadRootReservation       = 1u<<13u,
    RootReadbackIncomplete   = 1u<<14u,
    BadGartImage             = 1u<<15u,
    BadQueueRequest          = 1u<<16u,
    MissingHubSnapshot       = 1u<<17u,
    HubBusy                  = 1u<<18u,
    QueueSnapshotMissing     = 1u<<19u,
    QueueSnapshotUnstable    = 1u<<20u,
    QueueBusy                = 1u<<21u,
    BadBarRange              = 1u<<22u,
    InvalidPollBudget        = 1u<<23u
};

struct Evidence {
    // These are requirements for a future independent owner, not permissions
    // or facts inferred by this manifest. Tests may set them synthetically.
    bool exactPci1002_7480RevisionCf=false;
    bool exclusivePciAndBothVmHubs=false;
    bool noDisplayOrOtherGpuClients=false;
    bool psp13AndSdma6FirmwareVerified=false;
    bool mmhubCacheAndHdpInitialized=false;
    bool allDmaPagesPinnedAndMappedForThisPciDevice=false;
    bool deviceCacheMaintenanceAvailable=false;
    bool pairedGmcGeometryAndBar0Verified=false;
    bool separatelyOwnedPinnedVramRoot=false;
    bool exclusiveSdma0Queue1Owner=false;
    bool independentGpuRecovery=false;
    // Descriptive record of the user's one-shot VMID1 + Queue1 COPY/FENCE
    // approval in AGENTS.md. This planner never executes or consumes it,
    // and this bit cannot authorize another run.
    bool explicitApprovalForThisBoundedAttempt=false;
};

struct HubSnapshot {
    bool complete=false;
    uint32_t contextControl=0u;
    uint32_t invalidateRequest=0u;
    uint32_t invalidateAck=0u;
    uint32_t privateInvalidate=0u;
};

struct Input {
    const IPDiscovery *discovery=nullptr;
    vram_geometry::Geometry geometry{};
    uint64_t bar0Bytes=0u;
    uint64_t bar5Bytes=0u;
    vram_binding::Root rootLease{};
    uint64_t rootReadbackMcAddress=0u;
    mmhub11::Table gart{};
    vram_root::Report rootReadback{};
    sdma6_mmio::Request queue{};
    HubSnapshot mmhub{};
    HubSnapshot gfxhub{};
    bool queueSnapshotsComplete=false;
    sdma6_mmio::QueueSnapshot queueFirst{};
    sdma6_mmio::QueueSnapshot queueSecond{};
    Evidence evidence{};
    uint32_t hubPollBudget=1000u;
    uint32_t queuePollBudget=1000u;
};

struct Step {
    Action action=Action::VerifyStagingImage;
    Target target=Target::Cpu;
    uint64_t bar5ByteOffset=0u;
    uint64_t value=0u;
    uint32_t mask=0u;
    uint32_t pollBudget=0u;
};

constexpr size_t kMaxSteps=112u;
constexpr size_t kGartRootBytes=4096u;
constexpr size_t kGartRootWords=kGartRootBytes/sizeof(uint64_t);
constexpr uint32_t kDefaultHubPollBudget=1000u;
constexpr uint32_t kDefaultQueuePollBudget=1000u;
constexpr uint32_t kMaxPollBudget=100000u;
constexpr uint32_t kHubDelayUs=10u;
constexpr uint32_t kQueueDelayUs=100u;
constexpr uint32_t kQueue1Vmid=1u;
constexpr uint32_t kQueue1Index=1u;
constexpr uint32_t kVmid1Bit=1u<<1u;
constexpr uint32_t kQueue1AckMask=1u<<1u;

struct Manifest {
    Code code=Code::Refused;
    uint32_t refusalMask=0u;
    uint16_t stepCount=0u;
    uint64_t rootGpuMcAddress=0u;
    uint64_t gpuVirtualBase=0u;
    uint64_t mappedBytes=0u;
    uint64_t sourceGpuVa=0u;
    uint64_t targetGpuVa=0u;
    uint64_t ringGpuVa=0u;
    uint64_t fenceGpuVa=0u;
    uint32_t queueVmid=0u;
    uint32_t queueIndex=0u;
    uint16_t rootPteCount=0u;
    uint16_t unusedRootPteCount=0u;
    uint64_t rootPtes[staging::kPages]{};
    uint64_t mmhubOffsets[mmhub11::kCount]{};
    uint64_t gfxhubOffsets[mmhub11::kCount]{};
    uint64_t queue1Offsets[9u]{}; // CNTL, BASE, BASE_HI, RPTR, RPTR_HI, WPTR, WPTR_HI, DOORBELL, MINOR
    uint32_t expectedFence=0u;
    uint32_t copyBytes=0u;
    uint32_t ringDwords=sdma6_mmio::kHwRingDwords;
    bool stopAtFirstFailure=true;
    bool quarantineBeforeFirstWrite=true;
    bool releaseMemoryPermitted=false;
    Step steps[kMaxSteps]{};

    bool hardwareReady() const { return false; }
    bool wroteHardware() const { return false; }
};

constexpr bool kCanAccessHardware=false;
constexpr bool kCanWriteHardware=false;
constexpr bool kCanPublishCapabilities=false;
static_assert(!kCanAccessHardware && !kCanWriteHardware &&
              !kCanPublishCapabilities,
              "Queue1 config plan must remain a pure offline manifest");

constexpr uint32_t kQueueRegisters[9u]={
    sdma6_mmio::kRbCntl,sdma6_mmio::kRbBase,sdma6_mmio::kRbBaseHi,
    sdma6_mmio::kRptr,sdma6_mmio::kRptrHi,sdma6_mmio::kWptr,
    sdma6_mmio::kWptrHi,sdma6_mmio::kDoorbell,sdma6_mmio::kMinorPtr
};
constexpr size_t kQueueCntl=0u;
constexpr size_t kQueueBase=1u;
constexpr size_t kQueueBaseHi=2u;
constexpr size_t kQueueRptr=3u;
constexpr size_t kQueueRptrHi=4u;
constexpr size_t kQueueWptr=5u;
constexpr size_t kQueueWptrHi=6u;
constexpr size_t kQueueDoorbell=7u;

inline uint32_t addRefusal(uint32_t mask,bool condition,Refusal bit) {
    return condition ? (mask|static_cast<uint32_t>(bit)) : mask;
}
inline bool allNotOnes(const HubSnapshot &s,bool usesPrivateInvalidate) {
    return s.contextControl!=UINT32_MAX &&
           s.invalidateRequest!=UINT32_MAX &&
           s.invalidateAck!=UINT32_MAX &&
           (!usesPrivateInvalidate || s.privateInvalidate!=UINT32_MAX);
}
inline bool noAllOnesQueueValues(const sdma6_mmio::QueueSnapshot &s) {
    for(size_t i=0u;i<8u;++i)
        if(s.values[i]==UINT32_MAX)return false;
    return true;
}
inline bool emptyQueue1(const sdma6_mmio::QueueSnapshot &s) {
    return !(s.values[0u]&sdma6_mmio::kEnable) &&
           s.values[1u]==0u && s.values[2u]==0u && s.values[3u]==0u &&
           s.values[4u]==0u && s.values[5u]==0u && s.values[6u]==0u &&
           s.values[7u]==0u;
}
inline bool validRootReservation(const vram_binding::Root &root,
                                 const vram_geometry::Geometry &geometry,
                                 uint64_t bar0Bytes,
                                 const mmhub11::Table &t) {
    const vram_root::Reservation &r=root.reservation;
    const auto location=vram_geometry::locate(
        geometry,root.cpuBar0Offset,kGartRootBytes);
    if(!root.valid || bar0Bytes<kGartRootBytes ||
       geometry.realBytes==0u || geometry.cpuVisibleBytes>bar0Bytes ||
       !location.valid || !location.cpuBar0Visible ||
       (root.cpuBar0Offset&(kGartRootBytes-1u)) ||
       root.cpuBar0Offset>bar0Bytes-kGartRootBytes ||
       root.rootGpuMcAddress!=location.gpuMcAddress ||
       root.rootPhysicalAddress!=location.gpuPhysicalAddress ||
       root.cpuBar0Offset!=location.bar0Offset ||
       root.rootPhysicalAddress!=r.rootMcAddress ||
       r.mcVramStart!=geometry.physicalFbBase ||
       r.mcVramBytes!=geometry.realBytes)
        return false;
    return vram_root::alignedRange(r.mcVramStart,r.mcVramBytes) &&
           vram_root::alignedRange(r.ownedBoMcStart,r.ownedBoBytes) &&
           r.ownedBoBytes==kGartRootBytes &&
           r.ownedBoMcStart==r.rootMcAddress &&
           r.rootMcAddress==t.gpuPhysicalPageTable &&
           vram_root::contains(r.mcVramStart,r.mcVramBytes,
                               r.rootMcAddress,kGartRootBytes) &&
           vram_root::contains(r.ownedBoMcStart,r.ownedBoBytes,
                               r.rootMcAddress,kGartRootBytes) &&
           vram_root::contains(r.mcVramStart,r.mcVramBytes,
                               r.ownedBoMcStart,r.ownedBoBytes);
}
inline bool uniqueGartPages(const mmhub11::Table &t) {
    for(size_t i=0u;i<staging::kPages;++i) {
        const auto pte=gmc11::parseSystemPte(t.entries[i]);
        if(pte.code!=gmc11::Code::Pass ||
           (((t.entries[i]&gmc11::kPteWriteable)!=0u) !=
             (i!=staging::kSourcePage)))return false;
        const uint64_t page=t.entries[i]&gmc11::kAddressMask;
        for(size_t j=0u;j<i;++j)
            if((t.entries[j]&gmc11::kAddressMask)==page)return false;
    }
    return true;
}
inline bool supportedDiscovery(const Input &in) {
    return in.discovery && in.discovery->valid() &&
           in.discovery->isNavi33Candidate() &&
           inventory(*in.discovery).code==InventoryCode::Pass;
}
inline uint64_t queueByteOffset(const Input &in,size_t queueRegister) {
    const uint32_t qreg=kQueueRegisters[queueRegister]+
                        sdma6_mmio::kQueue1DwordDelta;
    const RegisterAddress a=resolveDword(*in.discovery,Engine::GFX11,0u,
                                         qreg,in.bar5Bytes);
    return a.code==AddressCode::Pass ? a.bar5ByteOffset : 0u;
}
inline bool append(Manifest &m,Action action,Target target=Target::Cpu,
                   uint64_t offset=0u,uint64_t value=0u,uint32_t mask=0u,
                   uint32_t polls=0u) {
    if(m.stepCount>=kMaxSteps) {
        m.code=Code::StepOverflow;
        return false;
    }
    m.steps[m.stepCount++]={action,target,offset,value,mask,polls};
    return true;
}
inline bool addWriteAndReadback(Manifest &m,Target target,uint64_t offset,
                                uint32_t value) {
    return append(m,Action::ProposedWrite,target,offset,value) &&
           append(m,Action::RequireReadback,target,offset,value);
}
inline uint32_t activeContextValue(uint32_t original) {
    return (original&~mmhub11::kContextDepth)|mmhub11::kContextEnable;
}
inline uint32_t parkedQueueControl(uint32_t original) {
    return (original&~sdma6_mmio::kCtlUpdateMask)|
           ((10u<<1u)&sdma6_mmio::kRbSizeMask)|sdma6_mmio::kRbPriv|
           ((kQueue1Vmid<<24u)&sdma6_mmio::kVmidMask);
}
inline uint32_t activeQueueControl(uint32_t original) {
    return parkedQueueControl(original)|sdma6_mmio::kEnable;
}

// Generate a structured, read-only execution manifest. `rootReadback` must
// come from a future separately owned BAR0 readback path and cover all 512
// DWORD pairs; synthetic evidence is useful only for host tests. No function
// here has access to the device or can execute the proposed steps.
inline Manifest build(const Input &in) {
    Manifest m{};
    uint32_t refused=0u;
    const Evidence &e=in.evidence;
    refused=addRefusal(refused,!e.exactPci1002_7480RevisionCf,
                       WrongPciIdentity);
    refused=addRefusal(refused,!e.exclusivePciAndBothVmHubs,MissingPciOwner);
    refused=addRefusal(refused,!e.noDisplayOrOtherGpuClients,
                       DisplayOrOtherGpuClient);
    refused=addRefusal(refused,!e.psp13AndSdma6FirmwareVerified,
                       FirmwareNotVerified);
    refused=addRefusal(refused,!e.mmhubCacheAndHdpInitialized,
                       HubCacheOrHdpNotReady);
    refused=addRefusal(refused,!e.allDmaPagesPinnedAndMappedForThisPciDevice,
                       DmaOwnershipMissing);
    refused=addRefusal(refused,!e.deviceCacheMaintenanceAvailable,
                       CacheSyncMissing);
    refused=addRefusal(refused,!e.pairedGmcGeometryAndBar0Verified,
                       GmcGeometryMissing);
    refused=addRefusal(refused,!e.separatelyOwnedPinnedVramRoot,
                       RootOwnershipMissing);
    refused=addRefusal(refused,!e.exclusiveSdma0Queue1Owner,
                       Queue1OwnershipMissing);
    refused=addRefusal(refused,!e.independentGpuRecovery,RecoveryMissing);
    refused=addRefusal(refused,!e.explicitApprovalForThisBoundedAttempt,
                       ApprovalMissing);
    if(!supportedDiscovery(in))refused|=BadDiscovery;
    if(in.bar5Bytes<4u)refused|=BadBarRange;
    if(in.hubPollBudget==0u || in.hubPollBudget>kMaxPollBudget ||
       in.queuePollBudget==0u || in.queuePollBudget>kMaxPollBudget)
        refused|=InvalidPollBudget;
    if(!validRootReservation(in.rootLease,in.geometry,in.bar0Bytes,in.gart))
        refused|=BadRootReservation;
    if(in.rootReadbackMcAddress!=in.rootLease.reservation.rootMcAddress ||
       in.rootReadback.code!=vram_root::Code::ImageMatches ||
       in.rootReadback.checkedWords!=kGartRootWords ||
       in.rootReadback.hardwareVmReady)
        refused|=RootReadbackIncomplete;
    if(!mmhub11::validTable(in.gart) || !uniqueGartPages(in.gart))
        refused|=BadGartImage;
    if(!sdma6_mmio::validRequest(in.queue) ||
       in.queue.vmid!=kQueue1Vmid || in.queue.queueIndex!=kQueue1Index)
        refused|=BadQueueRequest;
    if(!in.mmhub.complete || !in.gfxhub.complete ||
       !allNotOnes(in.mmhub,true) || !allNotOnes(in.gfxhub,false))
        refused|=MissingHubSnapshot;
    else if((in.mmhub.contextControl&mmhub11::kContextEnable) ||
            (in.gfxhub.contextControl&mmhub11::kContextEnable) ||
            in.mmhub.invalidateRequest!=0u ||
            in.gfxhub.invalidateRequest!=0u ||
            (in.mmhub.invalidateAck&kVmid1Bit) ||
            (in.gfxhub.invalidateAck&kVmid1Bit) ||
            (in.mmhub.privateInvalidate&mmhub11::kPrivateInvalidateBit))
        refused|=HubBusy;
    if(!in.queueSnapshotsComplete ||
       !noAllOnesQueueValues(in.queueFirst) ||
       !noAllOnesQueueValues(in.queueSecond))
        refused|=QueueSnapshotMissing;
    else if(!sdma6_mmio::sameQueueSnapshot(in.queueFirst,in.queueSecond))
        refused|=QueueSnapshotUnstable;
    else if(!emptyQueue1(in.queueFirst))
        refused|=QueueBusy;
    if(refused) {
        m.refusalMask=refused;
        return m;
    }

    const mmhub11::Plan mm=mmhub11::planFromDiscovery(
        *in.discovery,in.bar5Bytes,kQueue1Vmid);
    const mmhub11::Plan gfx=mmhub11::planGfxFromDiscovery(
        *in.discovery,in.bar5Bytes,kQueue1Vmid);
    if(!mmhub11::validPlan(mm) || !mmhub11::validPlan(gfx) ||
       mm.hub!=mmhub11::Hub::Mmhub || gfx.hub!=mmhub11::Hub::Gfxhub ||
       mm.vmid!=kQueue1Vmid || gfx.vmid!=kQueue1Vmid) {
        m.refusalMask=(mm.code==mmhub11::Code::InvalidBar ||
                       gfx.code==mmhub11::Code::InvalidBar)
                      ? BadBarRange : BadDiscovery;
        return m;
    }
    for(size_t i=0u;i<9u;++i) {
        m.queue1Offsets[i]=queueByteOffset(in,i);
        if(!m.queue1Offsets[i]) {
            m.refusalMask=BadBarRange;
            return m;
        }
        for(size_t j=0u;j<i;++j)
            if(m.queue1Offsets[i]==m.queue1Offsets[j]) {
                m.refusalMask=BadBarRange;
                return m;
            }
    }
    for(size_t i=0u;i<mmhub11::kCount;++i)
        for(size_t j=0u;j<mmhub11::kCount;++j)
            if(mm.offsets[i]==gfx.offsets[j]) {
                m.refusalMask=BadBarRange;
                return m;
            }
    m.rootGpuMcAddress=in.rootLease.reservation.rootMcAddress;
    m.gpuVirtualBase=in.gart.gpuVirtualBase;
    m.mappedBytes=in.gart.mappedBytes;
    m.sourceGpuVa=in.queue.sourceGpuVa;
    m.targetGpuVa=in.queue.targetGpuVa;
    m.ringGpuVa=in.queue.ringGpuVa;
    m.fenceGpuVa=in.queue.fenceGpuVa;
    m.queueVmid=in.queue.vmid;
    m.queueIndex=in.queue.queueIndex;
    m.rootPteCount=static_cast<uint16_t>(staging::kPages);
    m.unusedRootPteCount=static_cast<uint16_t>(
        kGartRootWords-staging::kPages);
    m.expectedFence=in.queue.cookie;
    m.copyBytes=static_cast<uint32_t>(staging::kBytePage);
    for(size_t i=0u;i<staging::kPages;++i)m.rootPtes[i]=in.gart.entries[i];
    for(size_t i=0u;i<mmhub11::kCount;++i) {
        m.mmhubOffsets[i]=mm.offsets[i];
        m.gfxhubOffsets[i]=gfx.offsets[i];
    }

    const size_t tableLo=static_cast<size_t>(mmhub11::Register::TableLo);
    const size_t tableHi=static_cast<size_t>(mmhub11::Register::TableHi);
    const size_t startLo=static_cast<size_t>(mmhub11::Register::StartLo);
    const size_t startHi=static_cast<size_t>(mmhub11::Register::StartHi);
    const size_t endLo=static_cast<size_t>(mmhub11::Register::EndLo);
    const size_t endHi=static_cast<size_t>(mmhub11::Register::EndHi);
    const size_t reqIndex=static_cast<size_t>(mmhub11::Register::InvalidateReq);
    const size_t ackIndex=static_cast<size_t>(mmhub11::Register::InvalidateAck);
    const size_t semIndex=static_cast<size_t>(mmhub11::Register::InvalidateSem);
    const size_t rangeLo=static_cast<size_t>(mmhub11::Register::InvalidateRangeLo);
    const size_t rangeHi=static_cast<size_t>(mmhub11::Register::InvalidateRangeHi);
    const size_t privateIndex=static_cast<size_t>(mmhub11::Register::PrivateInvalidate);
    const uint64_t encodedRoot=in.gart.gpuPhysicalPageTable|mmhub11::kPdeValid;
    const uint64_t start=in.gart.gpuVirtualBase>>12u;
    const uint64_t end=(in.gart.gpuVirtualBase+in.gart.mappedBytes-1u)>>12u;
    const uint32_t values[8u]={
        static_cast<uint32_t>(encodedRoot),static_cast<uint32_t>(encodedRoot>>32u),
        static_cast<uint32_t>(start),static_cast<uint32_t>(start>>32u),
        static_cast<uint32_t>(end),static_cast<uint32_t>(end>>32u),
        mmhub11::kRangeLoAll,mmhub11::kRangeHiAll
    };
    const size_t registerIndex[8u]={tableLo,tableHi,startLo,startHi,
                                    endLo,endHi,rangeLo,rangeHi};
    if(!append(m,Action::VerifyStagingImage,Target::Cpu) ||
       !append(m,Action::VerifyOwnedVramRoot,Target::Cpu,0u,
               static_cast<uint32_t>(kGartRootWords)))return m;
    const HubSnapshot *snapshots[2u]={&in.mmhub,&in.gfxhub};
    const mmhub11::Plan *plans[2u]={&mm,&gfx};
    const Target targets[2u]={Target::Mmhub,Target::Gfxhub};
    for(size_t hubIndex=0u;hubIndex<2u;++hubIndex) {
        const HubSnapshot &s=*snapshots[hubIndex];
        const Target target=targets[hubIndex];
        const mmhub11::Plan &hubPlan=*plans[hubIndex];
        if(!append(m,Action::RequireSnapshot,target,
                   hubPlan.offsets[static_cast<size_t>(mmhub11::Register::Context)],
                   s.contextControl) ||
           !append(m,Action::RequireSnapshot,target,
                   hubPlan.offsets[reqIndex],s.invalidateRequest) ||
           !append(m,Action::RequireSnapshot,target,
                   hubPlan.offsets[ackIndex],s.invalidateAck))return m;
        if(target==Target::Mmhub &&
           !append(m,Action::RequireSnapshot,target,
                   hubPlan.offsets[privateIndex],s.privateInvalidate))return m;
    }
    // Any ambiguous MMIO callback can reach silicon even if it reports a
    // failure. The backing must be quarantined before the first proposal.
    if(!append(m,Action::QuarantineBackingBeforeFirstWrite,Target::Host))
        return m;
    if(!append(m,Action::SyncGartForDevice,Target::Cpu))return m;

    // Stage both hubs disabled, install the exact same root/range, and
    // require every write to be read back before either context is enabled.
    for(size_t i=0u;i<8u;++i)
        if(!addWriteAndReadback(m,Target::Mmhub,mm.offsets[registerIndex[i]],
                                values[i]))return m;
    if(!append(m,Action::SyncGartForDevice,Target::Cpu))return m;
    for(size_t i=0u;i<8u;++i)
        if(!addWriteAndReadback(m,Target::Gfxhub,gfx.offsets[registerIndex[i]],
                                values[i]))return m;

    const uint32_t mmActive=activeContextValue(in.mmhub.contextControl);
    if(!addWriteAndReadback(m,Target::Mmhub,
           mm.offsets[static_cast<size_t>(mmhub11::Register::Context)],
           mmActive) ||
       !append(m,Action::FlushHdp,Target::Mmhub) ||
       !append(m,Action::AcquireMmhubInvalidateSemaphore,Target::Mmhub,
               mm.offsets[semIndex],1u,1u,in.hubPollBudget) ||
       !append(m,Action::ProposedWrite,Target::Mmhub,mm.offsets[reqIndex],
               mmhub11::invalidateForVmid(kQueue1Vmid)) ||
       !append(m,Action::PollRegisterBits,Target::Mmhub,
               mm.offsets[ackIndex],kQueue1AckMask,kQueue1AckMask,
               in.hubPollBudget) ||
       !append(m,Action::ReleaseMmhubInvalidateSemaphore,Target::Mmhub,
               mm.offsets[semIndex],0u) ||
       !append(m,Action::RequireReadback,Target::Mmhub,
               mm.offsets[privateIndex],in.mmhub.privateInvalidate) ||
       !append(m,Action::ProposedWrite,Target::Mmhub,
               mm.offsets[privateIndex],
               in.mmhub.privateInvalidate|mmhub11::kPrivateInvalidateBit,
               mmhub11::kPrivateInvalidateBit) ||
       !append(m,Action::RequireReadback,Target::Mmhub,
               mm.offsets[privateIndex],
               in.mmhub.privateInvalidate|mmhub11::kPrivateInvalidateBit,
               mmhub11::kPrivateInvalidateBit))return m;

    const uint32_t gfxActive=activeContextValue(in.gfxhub.contextControl);
    if(!addWriteAndReadback(m,Target::Gfxhub,
           gfx.offsets[static_cast<size_t>(mmhub11::Register::Context)],
           gfxActive) ||
       !append(m,Action::FlushHdp,Target::Gfxhub) ||
       !append(m,Action::ProposedWrite,Target::Gfxhub,gfx.offsets[reqIndex],
               mmhub11::invalidateForVmid(kQueue1Vmid)) ||
       !append(m,Action::PollRegisterBits,Target::Gfxhub,
               gfx.offsets[ackIndex],kQueue1AckMask,kQueue1AckMask,
               in.hubPollBudget) ||
       !append(m,Action::RequireDualHubReady,Target::Host,0u,
               in.gart.gpuPhysicalPageTable))return m;

    const uint64_t encodedRing=in.queue.ringGpuVa>>8u;
    const uint32_t parked=parkedQueueControl(
        in.queueFirst.values[0u]);
    const size_t configReg[7u]={
        kQueueBase,kQueueBaseHi,kQueueRptr,kQueueRptrHi,
        kQueueWptr,kQueueWptrHi,kQueueCntl
    };
    const uint32_t configValue[7u]={
        static_cast<uint32_t>(encodedRing),static_cast<uint32_t>(encodedRing>>32u),
        0u,0u,0u,0u,parked
    };
    if(!append(m,Action::RecheckQueue1Snapshot,Target::Sdma0Queue1))return m;
    const size_t queueSnapshotOffsets[8u]={
        kQueueCntl,kQueueDoorbell,kQueueRptr,kQueueRptrHi,
        kQueueWptr,kQueueWptrHi,kQueueBase,kQueueBaseHi
    };
    for(size_t i=0u;i<8u;++i)
        if(!append(m,Action::RequireSnapshot,Target::Sdma0Queue1,
                   m.queue1Offsets[queueSnapshotOffsets[i]],
                   in.queueFirst.values[i]))return m;
    for(size_t i=0u;i<7u;++i) {
        if(!addWriteAndReadback(m,Target::Sdma0Queue1,
                                m.queue1Offsets[configReg[i]],configValue[i]))
            return m;
    }
    const uint32_t active=activeQueueControl(in.queueFirst.values[0u]);
    if(!append(m,Action::SyncQueueForGpu,Target::Cpu) ||
       !addWriteAndReadback(m,Target::Sdma0Queue1,
                            m.queue1Offsets[kQueueCntl],active) ||
       !append(m,Action::ProposedWrite,Target::Sdma0Queue1,
               m.queue1Offsets[kQueueWptrHi],0u) ||
       !append(m,Action::RequireReadback,Target::Sdma0Queue1,
               m.queue1Offsets[kQueueWptrHi],0u) ||
       !append(m,Action::ProposedWrite,Target::Sdma0Queue1,
               m.queue1Offsets[kQueueWptr],sdma6_mmio::kCommandsBytes) ||
       !append(m,Action::PollFence,Target::Cpu,0u,in.queue.cookie,0u,
               in.queuePollBudget) ||
       !append(m,Action::VerifyCopyReadback,Target::Cpu,0u,
               static_cast<uint32_t>(staging::kBytePage)) ||
       !append(m,Action::RequireReadback,Target::Sdma0Queue1,
               m.queue1Offsets[kQueueRptr],sdma6_mmio::kCommandsBytes) ||
       !append(m,Action::RequireReadback,Target::Sdma0Queue1,
               m.queue1Offsets[kQueueWptr],sdma6_mmio::kCommandsBytes))return m;

    if(!append(m,Action::ProposedQueuePark,Target::Sdma0Queue1,
               m.queue1Offsets[kQueueCntl],
               in.queueFirst.values[0u]&~sdma6_mmio::kEnable) ||
       !append(m,Action::RequireReadback,Target::Sdma0Queue1,
               m.queue1Offsets[kQueueCntl],
               in.queueFirst.values[0u]&~sdma6_mmio::kEnable))return m;
    const size_t restoreReg[6u]={kQueueBase,kQueueBaseHi,kQueueRptr,
                                 kQueueRptrHi,kQueueWptr,kQueueWptrHi};
    const size_t snapshotValue[6u]={6u,7u,2u,3u,4u,5u};
    for(size_t i=0u;i<6u;++i)
        if(!addWriteAndReadback(m,Target::Sdma0Queue1,
               m.queue1Offsets[restoreReg[i]],
               in.queueFirst.values[snapshotValue[i]]))return m;
    if(!append(m,Action::RetainMemoryUntilIndependentReset,Target::Host))
        return m;

    m.code=Code::ManifestOnly;
    return m;
}

} } // namespace navi33::queue1_plan
