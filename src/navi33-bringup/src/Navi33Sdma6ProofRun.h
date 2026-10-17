// SPDX-License-Identifier: MIT
// EXPERIMENTAL Navi33/GMC11 + SDMA6 physical COPY/FENCE transaction.
// Not linked into Navi33Probe.kext. No macOS caller currently supplies
// authenticated, exclusive device/queue ownership or GPU-MC page-table memory.
//
// Unlike isolated component tests, this is the full ordering of TWO VM hubs
// and an SDMA ring. It is still not a driver: firmware/PSP, power management,
// device ownership and independent recovery must be solved on real hardware.
// DO NOT invoke this on the only active display GPU.
#pragma once
#include "Navi33Gmc11Mmhub.h"
#include "Navi33Sdma6MmioQueue.h"
#include "Navi33Gmc11RootVram.h"
#include "Navi33Gmc11VramLease.h"
#include "Navi33Sdma6QueueOwner.h"

namespace navi33 { namespace sdma6_proof {

#if defined(COBALT_NAVI33_ENABLE_UNSAFE_GMC11_MMIO) && \
    defined(COBALT_NAVI33_ENABLE_UNSAFE_MMIO)
constexpr bool kWritePathCompiled=true;
#else
constexpr bool kWritePathCompiled=false;
#endif

// These are NOT application-controlled permissions. A future isolated device
// owner must produce them from independently verified PCI, firmware, power,
// DMA and queue state. The production Kext does NOT construct this structure.
struct Isolation {
    bool pci1002_7480Owned=false;
    bool exclusiveGpuAndBothVmHubs=false;
    bool noActiveDisplayOrOtherGpuClients=false;
    bool pspAndSdmaFirmwareVerified=false;
    bool mmhubCacheAndHdpInitialized=false;
    bool ownedPhysicalGpuPageTablePinned=false;
    bool dmaIommuMappingHeld=false;
    bool deviceCacheMaintenanceAvailable=false;
    bool exclusiveSdma0Queue=false;
    bool independentPowerCycleAndRecovery=false;
};
constexpr bool isolated(const Isolation &g) {
    return g.pci1002_7480Owned && g.exclusiveGpuAndBothVmHubs &&
           g.noActiveDisplayOrOtherGpuClients &&
           g.pspAndSdmaFirmwareVerified &&
           g.mmhubCacheAndHdpInitialized &&
           g.ownedPhysicalGpuPageTablePinned &&
           g.dmaIommuMappingHeld &&
           g.deviceCacheMaintenanceAvailable &&
           g.exclusiveSdma0Queue &&
           g.independentPowerCycleAndRecovery;
}
enum class Verdict : uint8_t {
    Disabled=0, Refused, MmhubPrepareFailed, GfxhubPrepareFailed,
    MmhubActivateFailed, GfxhubActivateFailed, DualHubMismatch,
    QueueConfigureFailed, QueueSubmitFailed, FenceOrCopyFailed,
    QueueStopFailed, CopiedButVmStillActive, RecoveryRequired
};
struct Report {
    Verdict verdict=Verdict::Disabled;
    mmhub11::Code mmhub=mmhub11::Code::Disabled;
    mmhub11::Code gfxhub=mmhub11::Code::Disabled;
    sdma6_mmio::Code sdma=sdma6_mmio::Code::NotBuilt;
    uint32_t polls=0u;
    uint32_t observedFence=0u;
    uint32_t mmioWrites=0u;
    bool ringWasSubmitted=false;
    bool fourKiBCopyVerified=false;
    // NEVER free any pinned GPU-visible backing after a hub was activated:
    // the current GMC11 backend has no silicon-verified GART teardown.
    bool stagingMayBeReleased=false;
    bool physicalResetBeforeRelease=false;
};

inline bool boundPlans(const mmhub11::Plan &mm,
                       const mmhub11::Plan &gfx,
                       const mmhub11::Table &table,
                       const sdma6_mmio::Request &copy) {
    if(!mmhub11::validPlan(mm) || !mmhub11::validPlan(gfx) ||
       mm.hub!=mmhub11::Hub::Mmhub ||
       gfx.hub!=mmhub11::Hub::Gfxhub ||
       mm.barBytes!=gfx.barBytes || !mmhub11::validTable(table) ||
       !sdma6_mmio::validRequest(copy) ||
       copy.sourceGpuVa!=table.gpuVirtualBase ||
       copy.sourceGpuVa>UINT64_MAX-table.mappedBytes ||
       copy.sourceGpuVa+table.mappedBytes> (1ull<<48u))
        return false;
    // A malformed ROM must not direct both engines at the same register.
    for(size_t i=0u;i<mmhub11::kCount;++i)
        for(size_t j=0u;j<mmhub11::kCount;++j)
            if(mm.offsets[i]==gfx.offsets[j])return false;
    return true;
}

// Standalone transaction. Every MMIO write is performed by the callbacks
// supplied by an *external* isolated GPU owner; this code does not provide
// any adapter to Navi33Probe or an N33N user-client selector.
inline Report run(const Isolation &e,
                  const mmhub11::Plan &mmPlan,
                  const mmhub11::Plan &gfxPlan,
                  const mmhub11::Table &table,
                  const sdma6_mmio::Request &copy,
                  const vram_geometry::Geometry &geometry,
                  vram_bo::Arena &rootArena,vram_bo::Token rootToken,
                  vram_root::ReadWord readRootVram,void *rootReaderContext,
                  queue_owner::Registry &queueLeases,const void *client,
                  mmhub11::IO mmIo,mmhub11::IO gfxIo,
                  sdma6_mmio::IO sdmaIo,
                  unsigned hubPolls=1000u,
                  uint32_t fencePolls=1000u) {
    Report r{};
#if !defined(COBALT_NAVI33_ENABLE_UNSAFE_GMC11_MMIO) || \
    !defined(COBALT_NAVI33_ENABLE_UNSAFE_MMIO)
    (void)e;(void)mmPlan;(void)gfxPlan;(void)table;(void)copy;
    (void)mmIo;(void)gfxIo;(void)sdmaIo;(void)hubPolls;(void)fencePolls;
    (void)geometry;(void)rootArena;(void)rootToken;
    (void)readRootVram;(void)rootReaderContext;
    (void)queueLeases;(void)client;
    return r;
#else
    r.verdict=Verdict::Refused;
    // All validation is done BEFORE any MMIO write. The booleans below
    // are NOT a substitute for a hardware-derived proof on real silicon.
    if(!isolated(e) || !boundPlans(mmPlan,gfxPlan,table,copy) ||
       !hubPolls || hubPolls>100000u ||
       !fencePolls || fencePolls>100000u ||
       !mmIo.read || !mmIo.write || !mmIo.syncTableForDevice ||
       !mmIo.flushHdp || !mmIo.delayUs ||
       !gfxIo.read || !gfxIo.write || !gfxIo.syncTableForDevice ||
       !gfxIo.flushHdp || !gfxIo.delayUs ||
       !sdmaIo.read || !sdmaIo.write || !sdmaIo.beforeGpu ||
       !sdmaIo.afterGpu || !sdmaIo.readByte || !sdmaIo.delayUs)
        return r;

    // Do not feed an IODMACommand IOVA to the GPU-MC Context0 root.
    // Linux AMDGPU allocates the GART root in a pinned VRAM BO. Require
    // exact readback of the complete root (including unused zero PTEs)
    // from a SEPARATELY owned VRAM allocation before ANY GPU MMIO writes.
    // A caller cannot provide an arbitrary "owned" ROM/VRAM address:
    // the active per-device ledger MUST identify this exact BO lease.
    const auto binding=vram_binding::bindRoot(
        geometry,rootArena,client,rootToken);
    if(!binding.valid ||
       binding.rootPhysicalAddress!=table.gpuPhysicalPageTable)return r;
    const auto rootProof=vram_root::inspect(
        binding.reservation,table,readRootVram,rootReaderContext);
    if(rootProof.code!=vram_root::Code::ImageMatches)return r;

    // Software lease prevents two independent Cobalt64 Queue0 transactions.
    // It does NOT magically claim firmware/EFI/another macOS driver's queue.
    // The entire caller must be under a provider-wide lock.
    const auto lease=queueLeases.acquire(client);
    if(!lease.epoch)return r;
    // A failed write may still have reached silicon. Poison the lease
    // BEFORE first register-writing callback; never release on clientClose.
    if(!queueLeases.markHardwareTouched(client,lease)) {
        (void)queueLeases.releaseUnmodified(client,lease);
        return r;
    }
    // The GPU could fetch the root/packets as soon as registers change.
    // Never release/reuse this page after any possibly-live MMIO access,
    // regardless of the synthetic fence result.
    rootArena.quarantine();

    const mmhub11::Gate vmGate{
        e.exclusiveGpuAndBothVmHubs,
        e.noActiveDisplayOrOtherGpuClients,
        e.exclusiveGpuAndBothVmHubs,
        e.noActiveDisplayOrOtherGpuClients,
        e.pspAndSdmaFirmwareVerified && e.mmhubCacheAndHdpInitialized,
        e.mmhubCacheAndHdpInitialized,
        e.ownedPhysicalGpuPageTablePinned && e.dmaIommuMappingHeld,
        e.independentPowerCycleAndRecovery
    };
    mmhub11::Controller mm{},gfx{};
    const mmhub11::Result mp=mm.prepare(mmPlan,vmGate,table,mmIo);
    r.mmhub=mp.code;r.mmioWrites+=mp.writes;
    if(mp.code!=mmhub11::Code::Prepared) {
        r.verdict=mm.requiresRecovery()?Verdict::RecoveryRequired:
                  Verdict::MmhubPrepareFailed;
        // The provider-level allocation lease is already quarantined:
        // even a transport callback that reports failure might have sent
        // a register write to silicon. NO unpin until independent reset.
        r.stagingMayBeReleased=false;
        r.physicalResetBeforeRelease=true;
        return r;
    }
    const mmhub11::Result gp=gfx.prepare(gfxPlan,vmGate,table,gfxIo);
    r.gfxhub=gp.code;r.mmioWrites+=gp.writes;
    if(gp.code!=mmhub11::Code::Prepared) {
        const mmhub11::Result abort=mm.abortBeforeActivation();
        r.mmioWrites+=abort.writes;
        const bool clean=abort.code==mmhub11::Code::Aborted &&
                         gfx.mayReleaseMappedMemory();
        r.verdict=clean?Verdict::GfxhubPrepareFailed:
                        Verdict::RecoveryRequired;
        (void)clean; // still cannot reclaim quarantined hardware memory
        r.stagingMayBeReleased=false;
        r.physicalResetBeforeRelease=true;
        return r;
    }

    const mmhub11::Result ma=mm.invalidateAndActivate(hubPolls,10u);
    r.mmhub=ma.code;r.mmioWrites+=ma.writes;r.polls+=ma.polls;
    if(ma.code!=mmhub11::Code::Flushed) {
        r.verdict=Verdict::MmhubActivateFailed;
        // A Context0 enable may have reached GPU hardware even if the
        // transport returned false. An external reset is now required.
        r.physicalResetBeforeRelease=true;return r;
    }
    const mmhub11::Result ga=gfx.invalidateAndActivate(hubPolls,10u);
    r.gfxhub=ga.code;r.mmioWrites+=ga.writes;r.polls+=ga.polls;
    if(ga.code!=mmhub11::Code::Flushed) {
        r.verdict=Verdict::GfxhubActivateFailed;
        r.physicalResetBeforeRelease=true;return r;
    }
    r.physicalResetBeforeRelease=true; // no verified VMID0 teardown
    if(!mmhub11::dualHubReady(mm,gfx)) {
        r.verdict=Verdict::DualHubMismatch;return r;
    }

    sdma6_mmio::Preconditions sdmaGate{};
    sdmaGate.firmwareVerified=e.pspAndSdmaFirmwareVerified;
    sdmaGate.queueExclusivelyOwned=e.exclusiveSdma0Queue;
    sdmaGate.vmid0MappedByThisDriver=mmhub11::dualHubReady(mm,gfx);
    sdmaGate.tlbFlushedAfterMap=mmhub11::dualHubReady(mm,gfx);
    sdmaGate.ringGpuVaResolved=sdma6_mmio::validRequest(copy);
    sdmaGate.dmaOwnershipHeld=e.dmaIommuMappingHeld;
    sdmaGate.independentGpuRecovery=e.independentPowerCycleAndRecovery;
    sdmaGate.cpuGpuCacheMaintenance=e.deviceCacheMaintenanceAvailable;
    sdma6_mmio::Queue queue{};
    const auto qc=queue.configure(sdmaGate,copy,sdmaIo);
    r.sdma=qc.status;r.mmioWrites+=qc.mmioWrites;
    if(qc.status!=sdma6_mmio::Code::Ready) {
        r.verdict=Verdict::QueueConfigureFailed;return r;
    }
    const auto qs=queue.submit();
    r.sdma=qs.status;r.mmioWrites+=qs.mmioWrites;
    if(qs.status!=sdma6_mmio::Code::Submitted) {
        // The queue may still be recoverable before submit if cache sync
        // failed. VM hubs nevertheless remain active: keep all RAM pinned.
        if(!queue.requiresRecovery())(void)queue.abortBeforeSubmit();
        r.verdict=Verdict::QueueSubmitFailed;return r;
    }
    r.ringWasSubmitted=true;
    const auto qr=queue.waitAndVerify(fencePolls,100u);
    r.sdma=qr.status;r.mmioWrites+=qr.mmioWrites;r.polls+=qr.polls;
    r.observedFence=qr.fence;
    if(qr.status!=sdma6_mmio::Code::Completed) {
        r.verdict=Verdict::FenceOrCopyFailed;return r;
    }
    r.fourKiBCopyVerified=true;
    const auto stop=queue.stopAfterCompleted();
    r.sdma=stop.status;r.mmioWrites+=stop.mmioWrites;
    if(stop.status!=sdma6_mmio::Code::Completed) {
        r.verdict=Verdict::QueueStopFailed;return r;
    }
    // COPY+FENCE is proven only when these callbacks run against real,
    // independently owned hardware, not in fake-MMIO CI.
    r.verdict=Verdict::CopiedButVmStillActive;
    return r;
#endif
}
} } // namespace navi33::sdma6_proof
