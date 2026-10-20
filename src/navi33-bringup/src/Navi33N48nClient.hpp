// SPDX-License-Identifier: MIT
// The N48N v1.0 user client (selectors 0..8, Navi33N48nAbi.h) on Navi33Probe, so RADV's
// Darwin backend can drive the RX 7600 (Phase B4). Root only, behind navi33-client-gpu=1.
// HELLO opens the client's GPU context; close drains and frees it. Everything else is
// Navi33N48nFront.h under the provider's lock.
#pragma once
#include <IOKit/IOUserClient.h>
#include <IOKit/IOLib.h>
#include "Navi33N48nFront.h"
class Navi33Probe;

class Navi33N48nClient : public IOUserClient {
    OSDeclareDefaultStructors(Navi33N48nClient)
    using super = IOUserClient;
public:
    bool initWithTask(task_t owningTask, void *securityID, UInt32 type, OSDictionary *properties) override;
    bool start(IOService *provider) override;
    IOReturn clientClose() override;
    IOReturn clientDied() override;
    void free() override;
    IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args, IOExternalMethodDispatch *dispatch,
                            OSObject *target, void *reference) override;
    IOReturn clientMemoryForType(UInt32 type, IOOptionBits *options, IOMemoryDescriptor **memory) override;
    // The accelerator route (G41): the aux accelerator's 'N48N' open, attached to `provider` (the
    // accelerator) with `owner` holding all GPU state, exactly as an open on Navi33Probe.
    static IOReturn create(Navi33Probe *owner, IOService *provider, task_t task, void *securityID, UInt32 type,
                           IOUserClient **out);
private:
    Navi33Probe *owner_ = nullptr;
    IOLock *lock_ = nullptr;                  // this connection's session and mapping table
    navi33::n48n_front::Session session_{};
    static constexpr uint32_t kMaxBos = navi33::client_gpu::kMaxBos;
    IOMemoryDescriptor *boMd_[kMaxBos] = {};
    void revokeCpuMappings(uint32_t handle);  // under lock_
};
