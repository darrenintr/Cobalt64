// SPDX-License-Identifier: MIT
// Compile-only Navi33 private-ABI feasibility scaffold.
//
// Build only against a local, target-derived header supplied through
// N33_ACCEL_PRIVATE_LAYOUT_HEADER. The generated Apple-derived header must
// remain outside the repository. This class has no personality, is not
// bundled or registered, does not open its provider, and cannot start.
#ifndef N33_ACCEL_PRIVATE_LAYOUT_HEADER
#error "Pass the local target-derived IOAccelFamily2 declaration header"
#endif

#include <stdint.h>

// The reconstructed KC vtable does not encode C++ return types. Static
// analysis of the concrete Tahoe provider shows these slots return pointer
// values (null is the refusal value), except populate/teardown (void) and
// configureDevice (bool, false refuses configuration). A runtime service is
// still blocked on exact source-level types and a real backend.
#define N33_R_IOGraphicsAccelerator2_271 void *
#define N33_R_IOGraphicsAccelerator2_273 void *
#define N33_R_IOGraphicsAccelerator2_307 void *
#define N33_R_IOGraphicsAccelerator2_310 void
#define N33_R_IOGraphicsAccelerator2_312 bool
#define N33_R_IOGraphicsAccelerator2_313 void
#define N33_R_IOGraphicsAccelerator2_314 void *
#define N33_R_IOGraphicsAccelerator2_320 void *
#define N33_R_IOGraphicsAccelerator2_324 void *
#define N33_R_IOGraphicsAccelerator2_325 void *
#define N33_R_IOGraphicsAccelerator2_326 void *
#define N33_R_IOGraphicsAccelerator2_327 void *
#define N33_R_IOGraphicsAccelerator2_328 void *
#define N33_R_IOGraphicsAccelerator2_332 void *
#define N33_R_IOGraphicsAccelerator2_333 void *
#define N33_R_IOGraphicsAccelerator2_334 void *
#define N33_R_IOGraphicsAccelerator2_337 void *

#include N33_ACCEL_PRIVATE_LAYOUT_HEADER

namespace {
bool isNavi33OwnerNub(IOService *provider) {
    return provider && provider->metaCast("Navi33MetalNub") != nullptr;
}
} // namespace

class Navi33AccelServiceSkeleton final : public IOGraphicsAccelerator2 {
    OSDeclareDefaultStructors(Navi33AccelServiceSkeleton)

public:
    IOService *probe(IOService *provider, SInt32 *score) override {
        if (score) *score = 0;
        return isNavi33OwnerNub(provider) ? this : nullptr;
    }

    bool start(IOService *provider) override {
        // Even a correctly matched nub cannot start this scaffold. There is
        // no BO/VM/context/queue/submit/wait backend or app-facing client.
        (void)provider;
        return false;
    }

    void stop(IOService *provider) override {
        // start() never succeeds and this skeleton owns no GPU-visible state.
        (void)provider;
    }

    void *getStampMemory(unsigned int *) override { return nullptr; }
    void *newEventMachine() override { return nullptr; }
    void *createUserGPUTask() override { return nullptr; }
    void populateAccelConfig(IOAccelConfig *) override {}
    bool configureDevice(IOPCIDevice *) override { return false; }
    void teardownDevice(IOPCIDevice *) override {}
    void *createKernelGPUTask() override { return nullptr; }
    void *newDisplayMachine() override { return nullptr; }
    void *newGLContext() override { return nullptr; }
    void *newCLContext() override { return nullptr; }
    void *newSurface() override { return nullptr; }
    void *new2DContext() override { return nullptr; }
    void *newVideoContext() override { return nullptr; }
    void *newSysMemory() override { return nullptr; }
    void *newVidMemory() override { return nullptr; }
    void *newResource() override { return nullptr; }
    void *newMemoryMap() override { return nullptr; }
};

OSDefineMetaClassAndStructors(Navi33AccelServiceSkeleton,
                              IOGraphicsAccelerator2)

static_assert(sizeof(Navi33AccelServiceSkeleton) ==
                  N33_IOGraphicsAccelerator2_SIZE,
              "the fail-closed Navi33 subclass must not add instance state");

