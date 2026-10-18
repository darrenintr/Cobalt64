// SPDX-License-Identifier: MIT
// Navi33's Metal principal-class scaffold. This file is intentionally not
// bundled, installed, or referenced by an IOAccelerator personality yet.
// The current N33N ABI has no BO/VM/context/submit/wait implementation, so
// even a future-positive capability query cannot produce a Metal device.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <IOKit/IOKitLib.h>
#import <os/log.h>
#include <stdint.h>
#include <string.h>
#include "Navi33MetalGate.h"

// MTLIOAccelDevice is a private Metal.framework superclass and is not
// declared by the public SDK. Keep the subclass ivar-free; the real class
// layout is owned by Metal.framework.
@interface MTLIOAccelDevice : NSObject
- (instancetype)initWithAcceleratorPort:(uint32_t)port;
@end

@interface Navi33Device : MTLIOAccelDevice
- (instancetype)initWithAcceleratorPort:(uint32_t)port;
@end

// Property contract expected from a future Navi33MetalNub. The present
// Navi33Probe publishes none of these, which is a deliberate fail-closed
// condition. These properties must be kernel-published from the same GPU
// owner's capability snapshot. N33N itself remains a root-only diagnostic
// interface and is never opened from a Metal process.
static const char *const kN33MetalNubClass = "Navi33MetalNub";
static const CFStringRef kN33MetalABIKey = CFSTR("Navi33,ABI");
static const CFStringRef kN33MetalVendorKey = CFSTR("Navi33,VendorID");
static const CFStringRef kN33MetalDeviceKey = CFSTR("Navi33,DeviceID");
static const CFStringRef kN33MetalGfxIPKey = CFSTR("Navi33,GfxIP");
static const CFStringRef kN33MetalFamilyKey = CFSTR("Navi33,Family");
static const CFStringRef kN33MetalCapsKey = CFSTR("Navi33,Capabilities");
static const CFStringRef kN33MetalReadyKey = CFSTR("Navi33,Ready");

static BOOL n33_read_u32(io_registry_entry_t entry, CFStringRef key,
                         uint32_t *out) {
    CFTypeRef value = IORegistryEntryCreateCFProperty(
        entry, key, kCFAllocatorDefault, 0);
    if (!value) return NO;
    int32_t signedValue = 0;
    const BOOL valid = CFGetTypeID(value) == CFNumberGetTypeID() &&
        CFNumberGetValue((CFNumberRef)value, kCFNumberSInt32Type,
                         &signedValue);
    CFRelease(value);
    if (!valid || signedValue < 0) return NO;
    *out = (uint32_t)signedValue;
    return YES;
}

static BOOL n33_provider_snapshot(uint32_t port,
                                  struct n33_capabilities *caps) {
    if (!caps) return NO;
    memset(caps, 0, sizeof(*caps));
    io_registry_entry_t parent = IO_OBJECT_NULL;
    if (IORegistryEntryGetParentEntry((io_registry_entry_t)port,
                                      kIOServicePlane, &parent) != KERN_SUCCESS ||
        !parent)
        return NO;

    const BOOL isNavi33Nub = IOObjectConformsTo(parent, kN33MetalNubClass);
    CFTypeRef readyValue = IORegistryEntryCreateCFProperty(
        parent, kN33MetalReadyKey, kCFAllocatorDefault, 0);
    const BOOL publishedReady = readyValue &&
        CFGetTypeID(readyValue) == CFBooleanGetTypeID() &&
        CFBooleanGetValue((CFBooleanRef)readyValue);
    if (readyValue) CFRelease(readyValue);
    uint32_t abi = 0, vendor = 0, device = 0, gfxIP = 0;
    uint32_t family = 0, capBits = 0;
    const BOOL propsValid =
        n33_read_u32(parent, kN33MetalABIKey, &abi) &&
        n33_read_u32(parent, kN33MetalVendorKey, &vendor) &&
        n33_read_u32(parent, kN33MetalDeviceKey, &device) &&
        n33_read_u32(parent, kN33MetalGfxIPKey, &gfxIP) &&
        n33_read_u32(parent, kN33MetalFamilyKey, &family) &&
        n33_read_u32(parent, kN33MetalCapsKey, &capBits);
    IOObjectRelease(parent);
    const uint32_t required = N33_CAP_HW_VM | N33_CAP_HW_GFX |
                              N33_CAP_HW_SDMA | N33_CAP_METAL;
    if (!isNavi33Nub || !publishedReady || !propsValid ||
        abi != N33_ABI_VERSION || vendor != N33_PCI_VENDOR ||
        device != N33_PCI_DEVICE || gfxIP != N33_GFX_IP_VERSION ||
        family != N33_FAMILY_GFX1102 ||
        (capBits & required) != required)
        return NO;

    caps->abi_version = abi;
    caps->size = sizeof(*caps);
    caps->vendor_id = vendor;
    caps->device_id = device;
    caps->gfx_ip_version = gfxIP;
    caps->family = family;
    caps->capabilities = capBits;
    return YES;
}

@implementation Navi33Device
- (instancetype)initWithAcceleratorPort:(uint32_t)port {
    // Initialize Metal's private base before declining so its normal object
    // teardown path sees a fully initialized base object.
    self = [super initWithAcceleratorPort:port];
    if (!self) return nil;

    struct n33_capabilities caps = { 0 };
    const BOOL providerReady = n33_provider_snapshot(port, &caps);

    // This tree intentionally has no GFX1102 command backend. Keep this a
    // literal false until the allocator, GPU VM, context, queue, submit and
    // fence/readback path all exist and have physical proof.
    const BOOL backendReady = NO;
    const enum n33m_gate_result gate = n33m_gate_device(
        providerReady ? &caps : NULL, providerReady, backendReady);
    os_log(OS_LOG_DEFAULT,
           "Navi33Metal: declining accelerator port 0x%x; gate=%{public}s",
           port, n33m_gate_result_name(gate));

    // Deliberate: no successful return path, Metal device selectors, caps,
    // resources, queues, command buffers, or GPU submission are implemented.
    return nil;
}
@end
