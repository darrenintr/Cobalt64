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

// IOKit hands over an unwired user-task descriptor; readBytes() walks
// physical segments, so wire it for the copy.
static bool copyDescriptor(IOMemoryDescriptor *md,void *dst,size_t len) {
    if(!md || !dst || md->getLength()!=len ||
       md->prepare(kIODirectionOut)!=kIOReturnSuccess) return false;
    const bool ok=md->readBytes(0,dst,len)==len;
    md->complete(kIODirectionOut);
    return ok;
}

void Navi33NativeClient::releaseStageLocked() {
    // No live Navi33 code may currently submit GPU jobs. If a future
    // independently owned physical backend marks this Session as possibly
    // GPU-accessible, process death MUST NOT revoke the device IOVA or
    // permit another client to obtain the same provider stage lease.
    if(dmaStage_.requiresExternalReset()) {
        IOLog("Navi33NativeClient: retaining quarantined device IOMMU mapping and owner lease\n");
        return;
    }
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
    // Keep the attached provider alive until its exclusive DMA stage
    // lease has been released. Previous order cleared owner_ too early,
    // leaving a permanently held stage-owner epoch after client stop.
    releaseStageLocked(); // revoke IOVA before releasing provider lease
    owner_ = nullptr;
    if (dmaLock_) IOLockUnlock(dmaLock_);
    super::stop(provider);
}

IOReturn Navi33NativeClient::clientClose() {
    if (dmaLock_) IOLockLock(dmaLock_);
    active_ = false;
    releaseStageLocked(); // close/client death is equivalent to RELEASE
    owner_ = nullptr;
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
    // IOKit uses an out-of-line input descriptor above 4 KiB: real VBIOS
    // buffers and the PSP firmware. All other selectors refuse descriptors.
    const bool inputDescriptor=(selector==N33_SEL_HW_ROM_PREFLIGHT ||
                              selector==N33_SEL_PSP_LOAD) &&
                             args->structureInputDescriptor!=nullptr;
    const uint64_t inputBytes=inputDescriptor ?
        static_cast<uint64_t>(args->structureInputDescriptor->getLength()) :
        static_cast<uint64_t>(args->structureInputSize);
    if(inputBytes>UINT32_MAX || args->structureOutputSize>UINT32_MAX)
        return kIOReturnBadArgument;
    const bool illegalDescriptor=args->structureOutputDescriptor!=nullptr ||
        (args->structureInputDescriptor!=nullptr && !inputDescriptor);
    // is_io_connect_method always passes non-null MIG scalar arrays, even
    // for zero counts, so scalar presence must come from the counts.
    const navi33::native_policy::Shape result = navi33::native_policy::validate(
        selector, args->scalarInputCount, args->scalarOutputCount,
        static_cast<uint32_t>(inputBytes),
        static_cast<uint32_t>(args->structureOutputSize),
        args->scalarInputCount != 0u, args->scalarOutputCount != 0u,
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
    if (selector == N33_SEL_QUEUE_CENSUS) {
        if (!dmaLock_) return kIOReturnNotReady;
        IOLockLock(dmaLock_);
        if (!active_ || !owner_) {
            IOLockUnlock(dmaLock_); return kIOReturnNotReady;
        }
        const n33_queue_census_status status=owner_->queueCensus();
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
            if(inputDescriptor) {
                copied=IOMalloc(romLen);
                if(!copied) {
                    IOLockUnlock(dmaLock_);
                    return kIOReturnNoMemory;
                }
                if(!copyDescriptor(args->structureInputDescriptor,
                                   copied,romLen)) {
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
    if (selector == N33_SEL_MMIO_PEEK) {
        if (!dmaLock_) return kIOReturnNotReady;
        if (!args->structureInput) return kIOReturnBadArgument;
        n33_peek_request req{};
        bcopy(args->structureInput,&req,sizeof(req));
        if (req.abi_version!=N33_ABI_VERSION || req.size!=sizeof(req) ||
            req.reserved0 || req.rom_bytes!=inputBytes-sizeof(req))
            return kIOReturnBadArgument;
        IOLockLock(dmaLock_);
        if (!active_ || !owner_) {
            IOLockUnlock(dmaLock_);
            return kIOReturnNotReady;
        }
        const uint8_t *rom=static_cast<const uint8_t *>(
            args->structureInput)+sizeof(req);
        const n33_peek_result result=owner_->peekRegisters(
            req.window,req.first,req.count,rom,req.rom_bytes);
        bcopy(&result,args->structureOutput,sizeof(result));
        args->structureOutputSize=sizeof(result);
        IOLockUnlock(dmaLock_);
        return kIOReturnSuccess;
    }
    if (selector == N33_SEL_PSP_LOAD) {
        if (!dmaLock_) return kIOReturnNotReady;
        const size_t len=static_cast<size_t>(inputBytes);
        void *copied=IOMalloc(len);
        if (!copied) return kIOReturnNoMemory;
        if (inputDescriptor ? !copyDescriptor(args->structureInputDescriptor,
                                            copied,len)
                          : !args->structureInput) {
            IOFree(copied,len);
            return kIOReturnBadArgument;
        }
        if (!inputDescriptor) bcopy(args->structureInput,copied,len);
        n33_psp_load_request req{};
        bcopy(copied,&req,sizeof(req));
        const uint64_t payload=static_cast<uint64_t>(req.rom_bytes)+req.fw_bytes;
        if (req.abi_version!=N33_ABI_VERSION || req.size!=sizeof(req) ||
            
            (req.mode!=N33_PSP_LOAD_PLAN && req.mode!=N33_PSP_LOAD_RUN &&
             req.mode!=N33_PSP_LOAD_RING_PLAN && req.mode!=N33_PSP_LOAD_RING_RUN &&
             req.mode!=N33_PSP_LOAD_FW_PLAN && req.mode!=N33_PSP_LOAD_FW_RUN &&
             req.mode!=N33_SMU_ENABLE_PLAN && req.mode!=N33_SMU_ENABLE_RUN &&
             req.mode!=N33_SMU_SCPM_PLAN && req.mode!=N33_SMU_SCPM_RUN &&
             req.mode!=N33_MES_START_PLAN && req.mode!=N33_MES_START_RUN &&
             req.mode!=N33_MEC_QUEUE_PLAN && req.mode!=N33_MEC_QUEUE_RUN) ||
            ((req.mode==N33_PSP_LOAD_RING_PLAN || req.mode==N33_PSP_LOAD_RING_RUN) ?
                 (req.stop_after<N33_PSP_RING_STOP_RING ||
                  req.stop_after>N33_PSP_RING_STOP_TMR) :
             (req.mode==N33_PSP_LOAD_FW_PLAN || req.mode==N33_PSP_LOAD_FW_RUN) ?
                 (req.stop_after<1u || req.stop_after>N33_PSP_FW_STOP_AUTOLOAD) :
                 req.stop_after!=0u) ||
            ((req.mode==N33_PSP_LOAD_FW_PLAN || req.mode==N33_PSP_LOAD_FW_RUN) ?
                 (req.start_at>=req.stop_after) : req.start_at!=0u) ||
            req.rom_bytes<64u || !req.fw_bytes ||
            payload!=inputBytes-sizeof(req)) {
            IOFree(copied,len);
            return kIOReturnBadArgument;
        }
        if((req.mode==N33_SMU_ENABLE_PLAN || req.mode==N33_SMU_ENABLE_RUN ||
            req.mode==N33_SMU_SCPM_PLAN || req.mode==N33_SMU_SCPM_RUN ||
            req.mode==N33_MES_START_PLAN || req.mode==N33_MES_START_RUN ||
            req.mode==N33_MEC_QUEUE_PLAN || req.mode==N33_MEC_QUEUE_RUN) &&
           (req.fw_bytes!=1u || static_cast<const uint8_t *>(copied)[sizeof(req)+req.rom_bytes]!=0u)) {
            IOFree(copied,len);return kIOReturnBadArgument;
        }
        IOLockLock(dmaLock_);
        if (!active_ || !owner_) {
            IOLockUnlock(dmaLock_);
            IOFree(copied,len);
            return kIOReturnNotReady;
        }
        const uint8_t *rom=static_cast<const uint8_t *>(copied)+sizeof(req);
        const n33_psp_load_result result=owner_->pspLoad(
            req.mode,rom,req.rom_bytes,rom+req.rom_bytes,req.fw_bytes,
            req.stop_after,static_cast<uint32_t>(req.start_at));
        bcopy(&result,args->structureOutput,sizeof(result));
        args->structureOutputSize=sizeof(result);
        IOLockUnlock(dmaLock_);
        IOFree(copied,len);
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