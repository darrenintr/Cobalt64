// SPDX-License-Identifier: MIT
// A real IODMACommand mapping lives throughout the root-only N33N session;
// GMC11 PTE and SDMA6 packet images remain CPU staging only.
#include "Navi33DarwinStage.h"
#include "Navi33Identity.h"
#include "Navi33Sdma6Staging.h"
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IODMACommand.h>
#include <IOKit/IOMapper.h>
#include <IOKit/IOLib.h>
#include <IOKit/pci/IOPCIDevice.h>

namespace navi33 { namespace darwin_stage {

static n33_dma_stage_status state(n33_dma_stage_state code) {
    n33_dma_stage_status s{};
    s.abi_version=N33_ABI_VERSION;
    s.size=sizeof(s);
    s.state=code;
    // Physical GPU GPU-VM/HDP/TLB/rings are never programmed. This is
    // not proof of GPU command submission, regardless of IOMMU success.
    return s;
}

void Session::release() {
    // Reverse ownership order: first revoke live DMA mapping, then unpin.
    if(command_) {
        if(dmaPrepared_) (void)command_->complete();
        if(descriptorBound_) (void)command_->clearMemoryDescriptor(false);
        command_->release();
        command_=nullptr;
    }
    dmaPrepared_=false;
    hostSynced_=false;
    descriptorBound_=false;
    candidatePtIova_=0u;
    if(memory_) {
        if(memoryPrepared_) (void)memory_->complete();
        memory_->release();
        memory_=nullptr;
    }
    memoryPrepared_=false;
    if(mapper_) { mapper_->release(); mapper_=nullptr; }
    cached_=state(N33_DMA_NOT_PREPARED);
}

n33_dma_stage_status Session::failure(n33_dma_stage_state code) {
    release(); // Always drop potentially partial mapping on any failure.
    cached_=state(code);
    return cached_;
}

n33_dma_stage_status Session::snapshot() const {
    return cached_.size==sizeof(cached_)
           ? cached_ : state(N33_DMA_NOT_PREPARED);
}

bool Session::pinnedAndMapped() const {
    return dmaPrepared_ && descriptorBound_ && memoryPrepared_ &&
           memory_ && command_ && mapper_ &&
           cached_.state==N33_DMA_STAGED;
}

bool Session::readByte(size_t offset,uint8_t *out) const {
    if(!hostSynced_ || !pinnedAndMapped() || !out ||
       offset>=staging::kBytes || offset>=memory_->getLength())
        return false;
    const auto *data=static_cast<const uint8_t *>(memory_->getBytesNoCopy());
    if(!data)return false;
    *out=data[offset];
    return true;
}

bool Session::synchronizeForDevice() {
    if(!pinnedAndMapped() || !hostSynced_)return false;
    // User memory is already prepared and pinned to this exact PCI
    // IOMapper domain. Synchronization does not install a GPU VM root.
    if(command_->synchronize(kIODirectionOut)!=kIOReturnSuccess)
        return false;
    hostSynced_=false; // require explicit invalidate before CPU readback
    return true;
}

bool Session::synchronizeForHost() {
    if(!pinnedAndMapped())return false;
    if(command_->synchronize(kIODirectionIn)!=kIOReturnSuccess)
        return false;
    hostSynced_=true; // coherent CPU reads may now inspect fence/destination
    return true;
}

n33_dma_stage_status Session::prepare(IOPCIDevice *pci) {
    if(cached_.state==N33_DMA_STAGED && memory_ && command_ &&
       dmaPrepared_ && descriptorBound_ && memoryPrepared_)
        return cached_; // one pinned map per root client
    release();
    if(!pci) return failure(N33_DMA_BAD_PROVIDER);
    const Identity id=inspectPci(
        pci->configRead32(kIOPCIConfigVendorID),
        pci->configRead32(kIOPCIConfigRevisionID));
    if(!isSupported(id)) return failure(N33_DMA_BAD_PROVIDER);

    // No physical-address fallback. IOVA belongs to this PCI device domain.
    mapper_=IOMapper::copyMapperForDevice(pci);
    if(!mapper_) return failure(N33_DMA_NO_MAPPER);
    memory_=IOBufferMemoryDescriptor::withOptions(
        kIODirectionInOut,
        static_cast<vm_size_t>(staging::kBytes),
        static_cast<vm_offset_t>(gmc11::kPageBytes));
    if(!memory_ || !memory_->getBytesNoCopy() ||
       memory_->getLength()<staging::kBytes)
        return failure(N33_DMA_NO_MEMORY);

    if(memory_->prepare(kIODirectionInOut)!=kIOReturnSuccess)
        return failure(N33_DMA_PREPARE_FAILED);
    memoryPrepared_=true;
    command_=IODMACommand::withSpecification(
        kIODMACommandOutputHost64,48u,staging::kBytes,
        IODMACommand::kMapped,staging::kBytes,
        static_cast<UInt32>(gmc11::kPageBytes),mapper_,nullptr);
    if(!command_) return failure(N33_DMA_COMMAND_FAILED);
    if(command_->setMemoryDescriptor(memory_,false)!=kIOReturnSuccess)
        return failure(N33_DMA_COMMAND_FAILED);
    descriptorBound_=true;
    if(command_->prepare(0,staging::kBytes)!=kIOReturnSuccess)
        return failure(N33_DMA_COMMAND_FAILED);
    dmaPrepared_=true;

    dma::Segment segments[dma::kMaxPages]{};
    size_t n=0u;
    UInt64 offset=0u;
    while(offset<staging::kBytes) {
        if(n>=dma::kMaxPages) return failure(N33_DMA_SCATTER_FAILED);
        IODMACommand::Segment64 segment{};
        UInt32 count=1u;
        const UInt64 old=offset;
        const IOReturn err=command_->gen64IOVMSegments(
            &offset,&segment,&count);
        if((err!=kIOReturnSuccess && err!=kIOReturnOverrun) ||
           count!=1u || offset<=old || offset>staging::kBytes ||
           segment.fLength!=offset-old)
            return failure(N33_DMA_SCATTER_FAILED);
        segments[n++]={static_cast<uint64_t>(segment.fIOVMAddr),
                       static_cast<uint64_t>(segment.fLength)};
    }

    uint8_t *host=static_cast<uint8_t *>(memory_->getBytesNoCopy());
    const staging::Report report=staging::prepare(
        segments,n,host,static_cast<size_t>(memory_->getLength()));
    if(report.code!=staging::Code::Pass ||
       !staging::verifyInitial(host,staging::kBytes,report))
        return failure(N33_DMA_IMAGE_FAILED);

    // CPU filled the backing memory AFTER IODMACommand::prepare().
    // For double-buffered/noncoherent DMA this MUST be synchronized so the
    // actual PCI device DMA mapping sees the PTE, ring and source contents.
    // This is necessary for DMA, but not sufficient for GMC11 VM mapping.
    if(command_->synchronize(kIODirectionOut)!=kIOReturnSuccess)
        return failure(N33_DMA_CACHE_SYNC_FAILED);

    // Retain the *candidate* OS device-IOMMU address for future
    // hardware verification, not as a claim of GPU physical mapping.
    candidatePtIova_=report.candidatePageTableDmaIova;
    cached_=state(N33_DMA_STAGED);
    hostSynced_=true; // no GPU operation has begun
    cached_.flags=N33_DMA_FLAG_IOMMU_MAPPED|N33_DMA_FLAG_CPU_IMAGE_READY|
                  N33_DMA_FLAG_DEVICE_SYNCED;
    cached_.page_count=report.pages;
    cached_.segment_count=report.segments;
    cached_.ring_dwords=report.packetDwords;
    cached_.source_hash=report.sourceHash;
    cached_.destination_hash=report.destinationHash;
    cached_.fence_cookie=staging::kFenceCookie;
    // Neither GPU VM mappings nor physical GPU fences are created.
    return cached_;
}

} } // namespace navi33::darwin_stage
