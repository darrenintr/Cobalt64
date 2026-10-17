// SPDX-License-Identifier: MIT
// Root-only N33N query and opt-in OS IOMMU staging; NO GPU submission.
#pragma once
#include <IOKit/IOUserClient.h>
#include <IOKit/IOLib.h>
#include "Navi33DarwinStage.h"
class Navi33Probe;

class Navi33NativeClient : public IOUserClient {
    OSDeclareDefaultStructors(Navi33NativeClient)
    using super = IOUserClient;
public:
    bool initWithTask(task_t owningTask, void *securityID, UInt32 type,
                      OSDictionary *properties) override;
    bool start(IOService *provider) override;
    void stop(IOService *provider) override;
    IOReturn clientClose() override;
    IOReturn clientDied() override;
    void free() override;
    IOReturn externalMethod(uint32_t selector,
                            IOExternalMethodArguments *args,
                            IOExternalMethodDispatch *dispatch,
                            OSObject *target, void *reference) override;
private:
    Navi33Probe *owner_ = nullptr; // IOKit attachment owns provider lifetime.
    bool active_ = false;
    IOLock *dmaLock_ = nullptr; // serializes stage/snapshot/release and stop
    navi33::darwin_stage::Session dmaStage_{};
    uint64_t dmaLeaseEpoch_=0u;
    // Called under dmaLock_ where present; safe with idempotent release.
    void releaseStageLocked();
};
