# Navi33 Metal plug-in scaffold

## Experimental bridge artifact

`build-bridge.sh` builds a separate experimental bundle using the shared
Navi48Metal Objective-C bridge, the RADV Darwin backend, and the in-process
AIR-to-SPIR-V translator. The bridge talks through N48N to the existing G41
`Navi48MetalNub`/`Navi48Accelerator` path. RADV must include Mesa patches
0001–0011: in particular, patch 10 selects the `navi33` device table from the
live GC 11.0.2 QueryInfo response. The wrapper requires the RADV build's Mesa
commit manifest to identify patch 10, checks the N48N service selector and
Vulkan entry point, verifies x86_64 architecture, and rejects unbundled
non-system dylib dependencies before packaging.

On macOS 26 with Xcode, `vulkan-headers`, and Rust 1.87 or newer, build it with
the RADV dylib from the `radv-darwin-x86_64` workflow artifact:

```sh
N33_RADV_LIB=/path/to/libvulkan_radeon.dylib \
N33_VK_INC="-I$(brew --prefix vulkan-headers)/include" \
tools/native/navi33metal/build-bridge.sh
```

The wrapper builds the vendored `n48xlate` adapter from its pinned upstream
base when `N33_XLATE_LIB` is unset. It writes
`build/experimental-bridge/Navi33ExperimentalMetal-x86_64.tar` (or the path in
`N33_BRIDGE_OUTPUT_DIR`). The archive contains the loader-facing
`Navi48Metal.bundle` name and class expected by the auxiliary accelerator,
with a distinct experimental bundle identifier. It is an artifact only: the
script does not install it, load a kext, register an accelerator, or set any
readiness property. The G41 nub and accelerator still require their own live
kernel evidence gate.

This is a userspace packaging route, not proof of Navi33 feature parity. The
Navi48 Metal implementation carries broad format, feature, and display logic;
RADV's Navi33 table and hardware feature queries remain the authority for the
actual gfx1102 device. Do not use Navi48's fixed-format evidence as a Navi33
hardware result. The bundle still needs the separate rendered-output proof on
the RX 7600 before it can be treated as usable acceleration.

## Historical standalone Navi33 plug-in scaffold

The `make` target below builds the original x86_64 `Navi33Metal.bundle` with
the macOS 26 SDK. It is separate from the experimental bridge artifact above:
the auxiliary accelerator does not select this bundle, and this target is not
part of the Navi33 rendering path.

```sh
make -C tools/native/navi33metal
```

The outputs are `build/Navi33Metal.bundle` and
`build/Navi33Metal.bundle.tar`. The target only compiles and packages the
bundle. It does not install it, register an `IOAccelerator` personality,
publish `MetalPluginName`/`MetalPluginClassName`, or claim hardware readiness.
`Info.plist` names `Navi33Device` as the principal class. Its provider lookup
expects a `Navi33MetalNub`, while the active G41 path publishes
`Navi48MetalNub` for the existing N48N accelerator and RADV bridge. This
standalone bundle remains unregistered and untested on macOS hardware.

The Makefile requires an Xcode-selected macOS 26 SDK and targets x86_64 with
a macOS 26.0 deployment target. This Linux checkout has no `xcrun`, Apple
frameworks, or Metal SDK, so the Objective-C bundle cannot be compiled here.
Run `make -C tools/native/navi33metal host-test` on Linux or macOS for the
hardware-free C11 admission-policy tests; they do not instantiate a Metal
device or establish hardware readiness.

### Why the standalone scaffold is inert

`Navi33Device.m` declares the private `MTLIOAccelDevice` superclass and its
`initWithAcceleratorPort:` entry point. This is a private interface, inferred
from the local native plug-in pattern; Apple does not provide a public SDK
contract for it, and this class has not been loaded or runtime-checked on
macOS. The full Metal device protocol remains unknown here.

Initialization expects a `Navi33MetalNub` associated with the supplied
accelerator port and requires kernel-published Navi33 capability properties.
The active G41 route instead uses the existing Navi48 accelerator personality
and its `Navi48MetalNub`, which forwards N48N calls to Navi33Probe after G41's
live-evidence gate passes.

This standalone class keeps `backendReady = NO` and always returns `nil`.
It advertises no Metal features, creates no resources or queues, and submits
no GPU work.

The original scaffold documentation described N33N ABI 14 from October 2026.
That ABI statement is historical; the active rendering bridge uses the
versioned N48N ABI implemented by `Navi33N48nClient` and RADV's Darwin backend.

## Interfaces still required

The old scaffold remains only as a fail-closed policy/build experiment. The
active bridge's compile-time and runtime feature behavior comes from RADV's
live gfx1102 device queries plus the shared Metal implementation. Its
offscreen first-frame proof does not establish IOSurface composition or a
usable macOS desktop. The separate Navi33 accelerator readiness contract is
documented in `tools/native/navi33accel/README.md`.
