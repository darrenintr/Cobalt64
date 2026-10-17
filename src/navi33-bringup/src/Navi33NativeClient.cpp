// SPDX-License-Identifier: MIT
#include "Navi33NativeClient.hpp"
#include "Navi33Probe.hpp"
#include "Navi33NativeABI.h"
#include "Navi33NativePolicy.h"
#include <libkern/libkern.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <pexpert/pexpert.h>

OSDefineMetaClassAndStructors(Navi33NativeClient, IOUserClient)

void Navi33NativeClient::releaseStageLocked() {
    // The current Kext NEVER enables a physical GPU queue, so this unpins
    // only a CPU image with a live device-IOMMU mapping, not GPU work.
    dmaStage_.release();
    if(owner_ && dmaLeaseEpoch_)
        owner_->releaseStageOwner(this,dmaLeaseEpoch_);
    dmaLeaseEpoch_=0u;
}

bool Navi33NativeClient::initWithTask(task_t owningTask, void *securityID,
                                       UInt32 type, OSDictionary *properties) {
    if (type != N33_UC_TYPE ||
        clientHasPrivilege(securityID, kIOClientPrivilegeAdministrator)
            != kIOReturnSuccess)
        return false;
    if (!super::initWithTask(owningTask, securityID, type, properties))
        return false;
    dmaLock_ = IOLockAlloc();
    return dmaLock_ != nullptr;
}

bool Navi33NativeClient::start(IOService *provider) {
    Navi33Probe *probe = OSDynamicCast(Navi33Probe, provider);
    if (!probe || !super::start(provider)) return false;
    owner_ = probe;
    active_ = true;
    return true;
}

void Navi33NativeClient::stop(IOService *provider) {
    if (dmaLock_) IOLockLock(dmaLock_);
    active_ = false;
    owner_ = nullptr;
    releaseStageLocked(); // revoke IOVA before releasing provider lease
    if (dmaLock_) IOLockUnlock(dmaLock_);
    super::stop(provider);
}

IOReturn Navi33NativeClient::clientClose() {
    if (dmaLock_) IOLockLock(dmaLock_);
    active_ = false;
    owner_ = nullptr;
    releaseStageLocked(); // close/client death is equivalent to RELEASE
    if (dmaLock_) IOLockUnlock(dmaLock_);
    terminate();
    return kIOReturnSuccess;
}

IOReturn Navi33NativeClient::clientDied() {
    return clientClose(); // reclaim pinned IOMMU mappings on process crash
}

void Navi33NativeClient::free() {
    if(dmaLock_)IOLockLock(dmaLock_);
    releaseStageLocked(); // idempotent last-resort revoke
    if(dmaLock_) {
        IOLockUnlock(dmaLock_);
        IOLockFree(dmaLock_);
        dmaLock_=nullptr;
    }
    super::free();
}

IOReturn Navi33NativeClient::externalMethod(
    uint32_t selector, IOExternalMethodArguments *args,
    IOExternalMethodDispatch *dispatch, OSObject *target, void *reference) {
    (void)dispatch; (void)target; (void)reference;
    if (!active_ || !owner_) return kIOReturnNotReady;
    if (!args) return kIOReturnBadArgument;
    // IOKit may use an out-of-line input descriptor for real 128KiB+
    // VBIOS buffers. All other selectors continue to refuse descriptors.
    const bool romDescriptor=selector==N33_SEL_HW_ROM_PREFLIGHT &&
                             args->structureInputDescriptor!=nullptr;
    const uint64_t inputBytes=romDescriptor ?
        static_cast<uint64_t>(args->structureInputDescriptor->getLength()) :
        static_cast<uint64_t>(args->structureInputSize);
    if(inputBytes>UINT32_MAX || args->structureOutputSize>UINT32_MAX)
        return kIOReturnBadArgument;
    const bool illegalDescriptor=args->structureOutputDescriptor!=nullptr ||
        (args->structureInputDescriptor!=nullptr && !romDescriptor);
    const navi33::native_policy::Shape result = navi33::native_policy::validate(
        selector, args->scalarInputCount, args->scalarOutputCount,
        static_cast<uint32_t>(inputBytes),
        static_cast<uint32_t>(args->structureOutputSize),
        args->scalarInput != nullptr, args->scalarOutput != nullptr,
        args->structureOutput != nullptr, illegalDescriptor);
    if (result == navi33::native_policy::Shape::Unsupported)
        return kIOReturnUnsupported; // Cannot submit or allocate GPU memory.
    if (result != navi33::native_policy::Shape::Valid)
        return kIOReturnBadArgument;

    if (selector == N33_SEL_HELLO) {
        if (args->scalarInput[0] != N33_ABI_VERSION ||
            args->scalarInput[1] != 0u)
            return kIOReturnBadArgument;
        args->scalarOutput[0] = N33_ABI_VERSION;
        args->scalarOutput[1] = N33_UC_TYPE;
        args->scalarOutput[2] = N33_SEL_COUNT;
        return kIOReturnSuccess;
    }
    if (selector == N33_SEL_QUERY_CAPS) {
        if (!dmaLock_) return kIOReturnNotReady;
        IOLockLock(dmaLock_);
        if (!active_ || !owner_) {
            IOLockUnlock(dmaLock_); return kIOReturnNotReady;
        }
        const n33_capabilities caps = owner_->nativeCapabilities();
        bcopy(&caps, args->structureOutput, sizeof(caps));
        args->structureOutputSize = sizeof(caps);
        IOLockUnlock(dmaLock_);
        return kIOReturnSuccess;
    }
    if (selector == N33_SEL_VRAM_GEOMETRY) {
        if (!dmaLock_) return kIOReturnNotReady;
        IOLockLock(dmaLock_);
        if (!active_ || !owner_) {
            IOLockUnlock(dmaLock_); return kIOReturnNotReady;
        }
        const n33_vram_geometry_status status=owner_->vramGeometryStatus();
        bcopy(&status,args->structureOutput,sizeof(status));
        args->structureOutputSize=sizeof(status);
        IOLockUnlock(dmaLock_);
        return kIOReturnSuccess;
    }
    if (selector == N33_SEL_PSP_STATUS) {
        if (!dmaLock_) return kIOReturnNotReady;
        IOLockLock(dmaLock_);
        if (!active_ || !owner_) {
            IOLockUnlock(dmaLock_); return kIOReturnNotReady;
        }
        const n33_psp_status status = owner_->pspStatus();
        bcopy(&status, args->structureOutput, sizeof(status));
        args->structureOutputSize = sizeof(status);
        IOLockUnlock(dmaLock_);
        return kIOReturnSuccess;
    }
    if (selector == N33_SEL_SDMA_STATUS) {
        if (!dmaLock_) return kIOReturnNotReady;
        IOLockLock(dmaLock_);
        if (!active_ || !owner_) {
            IOLockUnlock(dmaLock_); return kIOReturnNotReady;
        }
        const n33_sdma_status status = owner_->sdmaStatus();
        bcopy(&status, args->structureOutput, sizeof(status));
        args->structureOutputSize = sizeof(status);
        IOLockUnlock(dmaLock_);
        return kIOReturnSuccess;
    }
    if (selector == N33_SEL_HW_PREFLIGHT ||
        selector == N33_SEL_HW_ROM_PREFLIGHT) {
        if (!dmaLock_) return kIOReturnNotReady;
        IOLockLock(dmaLock_);
        if (!active_ || !owner_) {
            IOLockUnlock(dmaLock_);
            return kIOReturnNotReady;
        }
        // The second selector provides an ephemeral VBIOS blob; the
        // Kext parses only IP Discovery and never uploads that firmware.
        const uint8_t *rom=nullptr;
        void *copied=nullptr;
        size_t romLen=0u;
        if(selector==N33_SEL_HW_ROM_PREFLIGHT) {
            if(inputBytes<64u || inputBytes>4u*1024u*1024u) {
                IOLockUnlock(dmaLock_);
                return kIOReturnBadArgument;
            }
            romLen=static_cast<size_t>(inputBytes);
            if(romDescriptor) {
                copied=IOMalloc(romLen);
                if(!copied) {
                    IOLockUnlock(dmaLock_);
                    return kIOReturnNoMemory;
                }
                if(args->structureInputDescriptor->readBytes(
                       0,copied,romLen)!=romLen) {
                    IOFree(copied,romLen);
                    IOLockUnlock(dmaLock_);
                    return kIOReturnBadArgument;
                }
                rom=static_cast<const uint8_t *>(copied);
            } else {
                if(!args->structureInput) {
                    IOLockUnlock(dmaLock_);
                    return kIOReturnBadArgument;
                }
                rom=static_cast<const uint8_t *>(args->structureInput);
            }
        }
        const n33_hw_preflight report = owner_->hardwarePreflight(rom,romLen);
        if(copied)IOFree(copied,romLen);
        bcopy(&report,args->structureOutput,sizeof(report));
        args->structureOutputSize=sizeof(report);
        IOLockUnlock(dmaLock_);
        return kIOReturnSuccess;
    }
    if (selector == N33_SEL_DMA_STAGE ||
        selector == N33_SEL_DMA_STATUS ||
        selector == N33_SEL_DMA_RELEASE) {
        if (!dmaLock_) return kIOReturnNotReady;
        IOLockLock(dmaLock_);
        if (!active_ || !owner_) {
            IOLockUnlock(dmaLock_);
            return kIOReturnNotReady;
        }
        if (selector == N33_SEL_DMA_RELEASE) {
            releaseStageLocked(); // always permitted even with kill switch
            IOLockUnlock(dmaLock_);
            return kIOReturnSuccess;
        }
        n33_dma_stage_status status{};
        if (selector == N33_SEL_DMA_STATUS) {
            status = dmaStage_.snapshot(); // cached: no I/O
        } else {
            uint32_t enabled=0u, kill=0u;
            const bool on=PE_parse_boot_argn("navi33-dma-stage",
                                  &enabled,sizeof(enabled)) && enabled==1u;
            const bool off=PE_parse_boot_argn("navi33-dma-stage-off",
                                  &kill,sizeof(kill)) && kill!=0u;
            if (!on || off) {
                status.abi_version=N33_ABI_VERSION;
                status.size=sizeof(status);
                status.state=N33_DMA_DISABLED;
            } else {
                // Per-client IOLock is not enough: only ONE user client
                // may pin diagnostic mappings for this provider at a time.
                if(!dmaLeaseEpoch_)
                    dmaLeaseEpoch_=owner_->claimStageOwner(this);
                if(!dmaLeaseEpoch_) {
                    status.abi_version=N33_ABI_VERSION;
                    status.size=sizeof(status);
                    status.state=N33_DMA_BUSY;
                } else {
                    IOPCIDevice *pci=OSDynamicCast(IOPCIDevice,
                                                   owner_->getProvider());
                    status=dmaStage_.prepare(pci);
                    if(status.state!=N33_DMA_STAGED)
                        releaseStageLocked();
                }
            }
        }
        bcopy(&status,args->structureOutput,sizeof(status));
        args->structureOutputSize=sizeof(status);
        IOLockUnlock(dmaLock_);
        return kIOReturnSuccess;
    }
    return kIOReturnUnsupported;
}