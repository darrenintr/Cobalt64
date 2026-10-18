# Navi33 Metal plug-in scaffold

This directory builds an x86_64 `Navi33Metal.bundle` with the macOS 26 SDK:

```sh
make -C tools/native/navi33metal
```

The outputs are `build/Navi33Metal.bundle` and
`build/Navi33Metal.bundle.tar`. The target only compiles and packages the
bundle. It does not install it, register an `IOAccelerator` personality,
publish `MetalPluginName`/`MetalPluginClassName`, or claim hardware readiness.
`Info.plist` names `Navi33Device` as the bundle principal class, but no
accelerator service points to this bundle. The Navi33 workflow runs the
host-only admission tests and builds this bundle with the macOS 26 SDK; its
artifact is explicitly unregistered and not boot-tested.

The Makefile requires an Xcode-selected macOS 26 SDK and targets x86_64 with
a macOS 26.0 deployment target. This Linux checkout has no `xcrun`, Apple
frameworks, or Metal SDK, so the Objective-C bundle cannot be compiled here.
Run `make -C tools/native/navi33metal host-test` on Linux or macOS for the
hardware-free C11 admission-policy tests; they do not instantiate a Metal
device or establish hardware readiness.

## Fail-closed state

`Navi33Device.m` declares the private `MTLIOAccelDevice` superclass and its
`initWithAcceleratorPort:` entry point. This is a private interface, inferred
from the local native plug-in pattern; Apple does not provide a public SDK
contract for it, and this class has not been loaded or runtime-checked on
macOS. The full Metal device protocol remains unknown here.

Initialization accepts only a `Navi33MetalNub` associated with the supplied
accelerator port and requires the kernel-published properties
`Navi33,ABI`, `Navi33,VendorID`, `Navi33,DeviceID`, `Navi33,GfxIP`,
`Navi33,Family`, `Navi33,Capabilities`, and `Navi33,Ready`. The capabilities
must include `N33_CAP_HW_VM`, `N33_CAP_HW_SDMA`, `N33_CAP_HW_GFX`, and
`N33_CAP_METAL`. The provider must copy this snapshot from the same live
hardware owner; registry discovery by itself is not evidence. The plugin
does not open N33N, which remains a root-only diagnostic interface.

Even if those properties pass, the literal `backendReady = NO` and
unconditional `return nil` keep Metal from enumerating a Navi33 device. The
class advertises no Metal features, creates no resources or queues, and
submits no GPU work. No Metal plugin metadata is published by the kext.

N33N ABI 14 currently supports HELLO, capability/status/preflight reads,
system-memory DMA staging and release, and the separately gated PSP stage.
Selectors 2–7 are reserved for BO create/free, VM map, context create,
submit, and wait, but the native client refuses them. DMA staging is not
VRAM allocation, GPU page-table ownership, a GFX queue, or GPU submission.

## Interfaces still required

1. **Accelerator nub:** implement a Navi33-owned provider attached to the
   same PCI owner as Navi33Probe. It must supply the private IOAccelFamily2
   service interface, per-client user-client creation, and the registry
   snapshot above. `Navi33,Ready` can be true only after the complete owner,
   memory, VM, queue, submit, readback, recovery, and client-transport proof
   is live for that device epoch.
2. **Per-client submit ABI:** define a separate versioned user-client
   contract for open/close, capability snapshot and epoch, resource handles,
   VM/context/queue lifetime, command submission, fence wait, timeout/error
   reporting, cancellation, and device-loss cleanup. It must bind every
   handle to the owning task and provider. Do not route application submits
   through the current root-only N33N diagnostic client.
3. **Memory backend:** implement owned BO allocation/free, CPU mapping and
   cache synchronization, GPU VA assignment, VM map/unmap, page-table-root
   ownership, installed-root readback, and TLB invalidation/acknowledgement.
   The provider must retain object ownership and never expose physical
   addresses in its registry snapshot.
4. **Queue backend:** prove and own the SDMA Queue1 COPY/FENCE/readback,
   queue park and memory retention, plus a separate GFX context/queue and
   GFX FENCE/readback path. Define ring/doorbell ownership, completion and
   error handling, teardown, and independent recovery before publishing
   readiness. Hardware writes remain subject to the project approval gates.
5. **Metal lowering:** implement Metal buffers, textures, command queues,
   command buffers, encoders and pipelines over a real gfx1102 backend, with
   shader translation and an explicit feature/format policy. Keep feature
   queries conservative until each advertised path has passed hardware
   readback.

The accelerator provider's readiness contract is tracked in
`tools/native/navi33accel/README.md`. No Navi48 source is included, linked,
copied, or edited by this scaffold.
