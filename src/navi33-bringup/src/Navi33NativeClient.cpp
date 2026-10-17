// SPDX-License-Identifier: MIT
#include "Navi33NativeClient.hpp"
#include "Navi33Probe.hpp"
#include "Navi33NativeABI.h"
#include "Navi33NativePolicy.h"
#include <libkern/libkern.h>

OSDefineMetaClassAndStructors(Navi33NativeClient, IOUserClient)

bool Navi33NativeClient::initWithTask(task_t owningTask, void *securityID,
                                       UInt32 type, OSDictionary *properties) {
    if (type != N33_UC_TYPE ||
        clientHasPrivilege(securityID, kIOClientPrivilegeAdministrator)
            != kIOReturnSuccess)
        return false;
    return super::initWithTask(owningTask, securityID, type, properties);
}

bool Navi33NativeClient::start(IOService *provider) {
    Navi33Probe *probe = OSDynamicCast(Navi33Probe, provider);
    if (!probe || !super::start(provider)) return false;
    owner_ = probe;
    active_ = true;
    return true;
}

void Navi33NativeClient::stop(IOService *provider) {
    active_ = false;
    owner_ = nullptr;
    super::stop(provider);
}

IOReturn Navi33NativeClient::clientClose() {
    active_ = false;
    owner_ = nullptr;
    terminate();
    return kIOReturnSuccess;
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
        const n33_capabilities caps = owner_->nativeCapabilities();
        bcopy(&caps, args->structureOutput, sizeof(caps));
        args->structureOutputSize = sizeof(caps);
        return kIOReturnSuccess;
    }
    if (selector == N33_SEL_PSP_STATUS) {
        const n33_psp_status status = owner_->pspStatus();
        bcopy(&status, args->structureOutput, sizeof(status));
        args->structureOutputSize = sizeof(status);
        return kIOReturnSuccess;
    }
    return kIOReturnUnsupported;
}
