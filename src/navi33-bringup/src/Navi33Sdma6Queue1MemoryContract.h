// SPDX-License-Identifier: MIT
// Offline, proof-specific binding of a Navi33 Queue1 plan to its backing
// objects and GPU-VA reservation. This is not an allocator, pin/map API,
// owner attestation, or hardware execution path.
#pragma once

#include "Navi33Sdma6Queue1ConfigPlan.h"
#include <stddef.h>
#include <stdint.h>

namespace navi33 { namespace queue1_memory {

enum class Code : uint8_t { Refused=0, ContractOnly };
enum class Role : uint8_t {
    SourceReadOnly=0, TargetReadWrite, RingReadWrite, FenceReadWrite,
    GartShadowReadWrite, MappedPaddingReadWrite
};

enum Refusal : uint32_t {
    MissingPlan             = 1u<<0u,
    MissingOwnerIdentity    = 1u<<1u,
    WrongPciIdentity        = 1u<<2u,
    OwnerMismatch           = 1u<<3u,
    BadStagingObject        = 1u<<4u,
    BadVramRootObject       = 1u<<5u,
    BadGpuVaReservation     = 1u<<6u,
    BadPageMapping          = 1u<<7u,
    BadQueueBinding         = 1u<<8u,
    MissingRetention        = 1u<<9u
};

// Opaque identities are supplied by a future production owner. Non-zero
// values and matching identities check the declaration's shape only; they
// do not prove PCI, IOMMU, VRAM, or queue ownership.
struct DeviceOwner {
    uint64_t providerId=0u;
    uint64_t dmaDomainId=0u;
    uint16_t vendorId=0u;
    uint16_t deviceId=0u;
    uint8_t revisionId=0u;
};

struct StagingObject {
    uint64_t objectId=0u;
    uint64_t generation=0u;
    uint64_t ownerProviderId=0u;
    uint64_t dmaDomainId=0u;
    uint64_t bytes=0u;
    uint64_t dmaPageAddresses[staging::kPages]{};
    bool pinned=false;
    bool mappedToThisDevice=false;
    bool cacheMaintenanceAvailable=false;
    bool retainedUntilIndependentReset=false;
};

struct VramRootObject {
    uint64_t objectId=0u;
    uint64_t generation=0u;
    uint64_t ownerProviderId=0u;
    uint64_t gpuPhysicalAddress=0u;
    uint64_t bytes=0u;
    bool ownedVramBo=false;
    bool pinned=false;
    bool exclusive=false;
    bool retainedUntilIndependentReset=false;
};

struct GpuVaReservation {
    uint64_t reservationId=0u;
    uint64_t generation=0u;
    uint64_t ownerProviderId=0u;
    uint64_t base=0u;
    uint64_t bytes=0u;
    bool exclusive=false;
    bool retainedUntilIndependentReset=false;
};

struct Input {
    const queue1_plan::Manifest *plan=nullptr;
    DeviceOwner device{};
    StagingObject stagingObject{};
    VramRootObject rootObject{};
    GpuVaReservation gpuVa{};
};

struct PageBinding {
    uint64_t gpuVa=0u;
    uint64_t dmaAddress=0u;
    uint64_t pte=0u;
    Role role=Role::MappedPaddingReadWrite;
    bool gpuReadable=false;
    bool gpuWritable=false;
};

struct Contract {
    Code code=Code::Refused;
    uint32_t refusalMask=0u;
    uint64_t providerId=0u;
    uint64_t dmaDomainId=0u;
    uint64_t stagingObjectId=0u;
    uint64_t stagingGeneration=0u;
    uint64_t rootObjectId=0u;
    uint64_t rootGeneration=0u;
    uint64_t gpuVaReservationId=0u;
    uint64_t gpuVaGeneration=0u;
    uint64_t rootGpuPhysicalAddress=0u;
    uint64_t gpuVaBase=0u;
    uint64_t mappedBytes=0u;
    uint16_t mappedPageCount=0u;
    PageBinding pages[staging::kPages]{};
    bool isHardwareEvidence=false;

    bool hardwareReady() const { return false; }
    bool mayReleaseBacking() const { return false; }
};

constexpr bool kCanAllocate=false;
constexpr bool kCanPinOrMap=false;
constexpr bool kCanAccessHardware=false;
constexpr bool kCanPublishCapabilities=false;
static_assert(!kCanAllocate && !kCanPinOrMap && !kCanAccessHardware &&
              !kCanPublishCapabilities,
              "Queue1 memory contract must remain offline-only");

constexpr uint64_t kPageBytes=staging::kBytePage;
constexpr uint64_t kExpectedStagingBytes=staging::kBytes;
constexpr uint64_t kExpectedGpuVaBase=staging::kGuestGpuVa;
constexpr uint16_t kExpectedPteCount=static_cast<uint16_t>(staging::kPages);

inline Role roleForPage(size_t page) {
    if(page==staging::kSourcePage)return Role::SourceReadOnly;
    if(page==staging::kTargetPage)return Role::TargetReadWrite;
    if(page==staging::kRingPage)return Role::RingReadWrite;
    if(page==staging::kFencePage)return Role::FenceReadWrite;
    if(page==staging::kPageTablePage)return Role::GartShadowReadWrite;
    return Role::MappedPaddingReadWrite;
}

inline bool writableRole(Role role) {
    return role!=Role::SourceReadOnly;
}

inline void refuse(uint32_t &mask,bool condition,Refusal bit) {
    if(condition)mask|=static_cast<uint32_t>(bit);
}

// Convert a successful read-only Queue1 plan plus future-owner descriptors
// into a typed memory/GPU-VA mapping contract. This never invokes the named
// owner, allocator, IODMACommand, BAR, or MMIO operation.
inline Contract build(const Input &in) {
    Contract out{};
    uint32_t refused=0u;
    if(!in.plan ||
       in.plan->code!=queue1_plan::Code::ManifestOnly ||
       in.plan->refusalMask!=0u || in.plan->hardwareReady() ||
       in.plan->wroteHardware())
        refused|=MissingPlan;

    const DeviceOwner &device=in.device;
    const StagingObject &stagingObject=in.stagingObject;
    const VramRootObject &root=in.rootObject;
    const GpuVaReservation &va=in.gpuVa;
    if(!device.providerId || !device.dmaDomainId ||
       !stagingObject.objectId || !stagingObject.generation ||
       !root.objectId || !root.generation ||
       !va.reservationId || !va.generation)
        refused|=MissingOwnerIdentity;
    if(device.vendorId!=0x1002u || device.deviceId!=0x7480u ||
       device.revisionId!=0xcfu)
        refused|=WrongPciIdentity;
    if(stagingObject.ownerProviderId!=device.providerId ||
       root.ownerProviderId!=device.providerId ||
       va.ownerProviderId!=device.providerId ||
       stagingObject.dmaDomainId!=device.dmaDomainId ||
       stagingObject.objectId==root.objectId)
        refused|=OwnerMismatch;

    if(stagingObject.bytes!=kExpectedStagingBytes ||
       !stagingObject.pinned || !stagingObject.mappedToThisDevice ||
       !stagingObject.cacheMaintenanceAvailable)
        refused|=BadStagingObject;
    if(!root.ownedVramBo || !root.pinned || !root.exclusive ||
       root.bytes!=queue1_plan::kGartRootBytes || !in.plan ||
       root.gpuPhysicalAddress!=in.plan->rootGpuMcAddress ||
       root.gpuPhysicalAddress==0u)
        refused|=BadVramRootObject;
    if(!in.plan || va.base!=kExpectedGpuVaBase ||
       va.bytes!=kExpectedStagingBytes || !va.exclusive ||
       va.base!=in.plan->gpuVirtualBase ||
       va.bytes!=in.plan->mappedBytes)
        refused|=BadGpuVaReservation;
    if(!stagingObject.retainedUntilIndependentReset ||
       !root.retainedUntilIndependentReset ||
       !va.retainedUntilIndependentReset)
        refused|=MissingRetention;

    if(!in.plan || stagingObject.bytes!=kExpectedStagingBytes ||
       in.plan->gpuVirtualBase!=kExpectedGpuVaBase ||
       in.plan->mappedBytes!=kExpectedStagingBytes ||
       in.plan->rootPteCount!=kExpectedPteCount ||
       in.plan->unusedRootPteCount!=
           queue1_plan::kGartRootWords-kExpectedPteCount ||
       va.base>UINT64_MAX-va.bytes || va.bytes!=kExpectedStagingBytes) {
        refused|=BadPageMapping;
    } else {
        for(size_t i=0u;i<staging::kPages;++i) {
            const uint64_t pte=in.plan->rootPtes[i];
            const auto parsed=gmc11::parseSystemPte(pte);
            const Role role=roleForPage(i);
            const bool writeable=writableRole(role);
            const uint64_t dmaAddress=pte&gmc11::kAddressMask;
            if(parsed.code!=gmc11::Code::Pass ||
               dmaAddress!=stagingObject.dmaPageAddresses[i] ||
               (pte&gmc11::kPteWriteable)!=(writeable ?
                    gmc11::kPteWriteable : 0u) ||
               va.base+i*kPageBytes!=
                    in.plan->gpuVirtualBase+i*kPageBytes)
                refused|=BadPageMapping;
            for(size_t j=0u;j<i;++j)
                if(stagingObject.dmaPageAddresses[i]==
                   stagingObject.dmaPageAddresses[j])
                    refused|=BadPageMapping;
            out.pages[i]={va.base+i*kPageBytes,dmaAddress,pte,role,
                          parsed.code==gmc11::Code::Pass,writeable};
        }
    }

    if(!in.plan || in.plan->queueVmid!=queue1_plan::kQueue1Vmid ||
       in.plan->queueIndex!=queue1_plan::kQueue1Index ||
       in.plan->sourceGpuVa!=va.base ||
       in.plan->targetGpuVa!=va.base+staging::kTargetPage*kPageBytes ||
       in.plan->ringGpuVa!=va.base+staging::kRingPage*kPageBytes ||
       in.plan->fenceGpuVa!=va.base+staging::kFencePage*kPageBytes)
        refused|=BadQueueBinding;

    if(refused) {
        out.refusalMask=refused;
        return out;
    }
    out.code=Code::ContractOnly;
    out.providerId=device.providerId;
    out.dmaDomainId=device.dmaDomainId;
    out.stagingObjectId=stagingObject.objectId;
    out.stagingGeneration=stagingObject.generation;
    out.rootObjectId=root.objectId;
    out.rootGeneration=root.generation;
    out.gpuVaReservationId=va.reservationId;
    out.gpuVaGeneration=va.generation;
    out.rootGpuPhysicalAddress=root.gpuPhysicalAddress;
    out.gpuVaBase=va.base;
    out.mappedBytes=va.bytes;
    out.mappedPageCount=kExpectedPteCount;
    out.isHardwareEvidence=false;
    return out;
}

} } // namespace navi33::queue1_memory
