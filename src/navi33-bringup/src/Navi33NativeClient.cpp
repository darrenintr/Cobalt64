// SPDX-License-Identifier: MIT
#include "Navi33NativeClient.hpp"
#include "Navi33Probe.hpp"
#include "Navi33NativeABI.h"
#include "Navi33NativePolicy.h"
#include <libkern/libkern.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <pexpert/pexpert.h>

OSDefineMetaClassAndStructors(Navi33NativeClient, IOUserClient)

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
    dmaStage_.release(); // revoke the device IOVA mapping before detach
    if (dmaLock_) IOLockUnlock(dmaLock_);
    super::stop(provider);
}

IOReturn Navi33NativeClient::clientClose() {
    if (dmaLock_) IOLockLock(dmaLock_);
    active_ = false;
    owner_ = nullptr;
    dmaStage_.release(); // close/client death is equivalent to RELEASE
    if (dmaLock_) IOLockUnlock(dmaLock_);
    terminate();
    return kIOReturnSuccess;
}

IOReturn Navi33NativeClient::clientDied() {
    return clientClose(); // reclaim pinned IOMMU mappings on process crash
}

void Navi33NativeClient::free() {
    dmaStage_.release(); // idempotent last-resort revoke
    if (dmaLock_) { IOLockFree(dmaLock_); dmaLock_ = nullptr; }
    super::free();
}

IOReturn Navi33NativeClient::externalMethod(
    uint32_t selector, IOExternalMethodArguments *args,
    IOExternalMethodDispatch *dispatch, OSObject *target, void *reference) {
    (void)dispatch; (void)target; (void)reference;
    if (!active_ || !owner_) return kIOReturnNotReady;
    if (!args) return kIOReturnBadArgument;
    const navi33::native_policy::Shape result = navi33::native_policy::validate(
        selector, args->scalarInputCount, args->scalarOutputCount,
        args->structureInputSize, args->structureOutputSize,
        args->scalarInput != nullptr, args->scalarOutput != nullptr,
        args->structureOutput != nullptr,
        args->structureInputDescriptor != nullptr ||
        args->structureOutputDescriptor != nullptr);
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
            dmaStage_.release(); // always permitted even with kill switch
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
                IOPCIDevice *pci=OSDynamicCast(IOPCIDevice,
                                               owner_->getProvider());
                status=dmaStage_.prepare(pci);
            }
        }
        bcopy(&status,args->structureOutput,sizeof(status));
        args->structureOutputSize=sizeof(status);
        IOLockUnlock(dmaLock_);
        return kIOReturnSuccess;
    }
    return kIOReturnUnsupported;
}