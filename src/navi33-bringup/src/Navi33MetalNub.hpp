// SPDX-License-Identifier: MIT
// The Metal nub (G41): the IOService the aux accelerator kext (tools/native/navi48accel, IOProviderClass
// "Navi48MetalNub") matches, with the Navi48MetalOps.h table it calls back through. Navi33Probe
// publishes it itself, once, when the client GPU core has the live evidence hard rule 6 asks for
// (Navi33MetalPure.h Evidence) and boot-arg navi33-metal=1 is set. No hardware access here: one 4 KiB
// stamp page, the nub and its properties; native_open hands the accelerator's 'N48N' opens to
// Navi33N48nClient with Navi33Probe as owner.
#pragma once
#include <IOKit/IOService.h>
#include "Navi33MetalPure.h"
class Navi33Probe;

class Navi48MetalNub : public IOService {             // the class name is the aux kext's match key
    OSDeclareDefaultStructors(Navi48MetalNub)
    using super = IOService;
public:
    // 0 = published now; otherwise the verdict (kNotReady while evidence is missing, ...).
    static uint32_t publish(Navi33Probe *owner, const navi33::metal::Evidence &e, uint64_t vramBytes);
    static bool published();
    static void shutdown();                              // Navi33Probe::stop
    IOReturn callPlatformFunction(const OSSymbol *functionName, bool waitForFunction, void *param1, void *param2,
                                  void *param3, void *param4) override;
    void free() override;
};
