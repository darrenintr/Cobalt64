// SPDX-License-Identifier: MIT
// Root-only N33N read-only IOUserClient; never maps memory or submits a GPU job.
#pragma once
#include <IOKit/IOUserClient.h>
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
    IOReturn externalMethod(uint32_t selector,
                            IOExternalMethodArguments *args,
                            IOExternalMethodDispatch *dispatch,
                            OSObject *target, void *reference) override;
private:
    Navi33Probe *owner_ = nullptr; // IOKit attachment owns provider lifetime.
    bool active_ = false;
};
