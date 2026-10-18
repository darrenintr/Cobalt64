# Navi33 accelerator readiness contract

This directory contains Navi33-only, host-checkable policy for a future
Metal-facing accelerator. It does **not** implement or publish an
`IOAccelerator` service, a Metal device, a capability bit, a memory manager,
a GPU VM, or a command queue. It performs no hardware access and no writes.

## Fail-closed contract

`n33_accel_backend_ready()` returns true only when the canonical N33N
capability snapshot identifies ABI 14 / RX 7600 / gfx1102 and reports all of
`HW_VM`, `HW_SDMA`, `HW_GFX`, and `METAL`, with nonzero validated VRAM/GTT
sizes. It also requires a same-epoch evidence record covering exclusive PCI
ownership, the owned and read-back GMC root, installed VM context and TLB
acknowledgement, SDMA6 Queue1 COPY/FENCE/readback/park with memory retained,
GFX context and queue/FENCE/readback, independent recovery, and a per-client
submission transport. It additionally requires a validated Metal shader
translation path, command-buffer encoding into Navi33 work, and an actual
Metal draw whose output was read back and matched. `N33_CAP_METAL` is only an
input capability claim; it does not stand in for these runtime proofs.
Physical addresses never enter the registry evidence; all resource and
recovery generations must equal the current device-owner generation.

The evidence must come from live kernel-owned objects and verified hardware
results. A host-generated plan, read-only register census, IODMA mapping test,
PSP/RLC state, a boolean copied from userspace, or successful plugin discovery
cannot set it. Missing fields, unknown evidence bits, stale ABI, zero owner
epochs, or reserved bits all refuse readiness. This helper never sets caps or
publishes properties itself.

The property names shared with the Navi33 Metal plugin are `Navi33,ABI`,
`Navi33,VendorID`, `Navi33,DeviceID`, `Navi33,GfxIP`, `Navi33,Family`,
`Navi33,Capabilities`, and `Navi33,Ready`. A future Navi33-owned nub may
publish them only from the canonical hardware owner snapshot, and may set
`Navi33,Ready=true` only after this predicate passes. Registry properties
are device discovery only; rendering calls must use an app-facing,
per-client accelerator user-client connection. The existing root-only N33N
diagnostic client is not that transport.

Today `Navi33Probe` publishes PCI inventory (and optional ROM discovery),
keeps the acceleration bits clear, has no BO/VM/context/submit/wait selectors,
and does not publish `Navi33,Ready` or a Metal plugin name. Keep it that way
until the independent owner and queue work has a reviewed runtime path and
hardware evidence. The SDMA Queue1 manifest in
`src/navi33-bringup/src/Navi33Sdma6Queue1ConfigPlan.h` is offline input only;
it explicitly cannot signal hardware or Metal readiness. The memory/queue
proof also does not replace the separate GFX queue/context and user-client
submission implementation needed for a rendered Metal frame.

## Kernel-service ABI and build boundary

The workflow-fetched MacKernelSDK has public
`IOKit/graphics/IOAccelerator.h`, whose `IOAccelerator` base is an
`IOService` with accel-ID helpers. It does not declare the private
`IOGraphicsAccelerator2` / IOAccelFamily2 vtable interface needed for the
existing Metal personality path. Do not register a placeholder
`IOAccelerator` or add `MetalPluginName`/`MetalPluginClassName` based only on
that public header.

The target private ABI is available as local, non-repository evidence from
the macOS 26.7.1 (25G241) x86_64 guest. Its source KCs, sparse private kext
views, generated model, and compile/layout result are stored outside the repo
at `~/macos tahoe/navi33/host/kc-25G241-20261018-072638/`. The layout tool
matched 16 relevant class vtables and 3150 slots with that target KC. These
Apple-derived inputs and outputs must remain private and must not be checked
into the repository.

The layout comparison proves vtable slot order and object size; it does not
prove private C++ return types or a complete Navi33 service method contract.
The reconstructed declarations use placeholder return types where Mach-O
mangling does not encode them. That is insufficient to ship a concrete
`IOGraphicsAccelerator2` subclass safely: an ambiguous zero return could mean
either a null factory result or a successful status. The current target
therefore remains a host-checked readiness/ownership boundary, not a
registered IOAccelerator personality. Do not add an `IOAccelerator`/Metal
personality until exact method signatures are validated against the target KC
and a per-client backend is implemented.

The accelerator's future owner must exclusively hold the PCI function, GPU
VM root and mappings, GMC context, SDMA Queue1 and GFX queues, and all memory
reachable by the GPU. It must prevent another driver or client from touching
those resources. Every client owns isolated BOs, mappings, contexts and queue
state through a dedicated IOUserClient transport; the N33N diagnostic client
cannot be reused as an app submission channel.

On stop, the owner must reject new work, stop submissions, wait for verified
queue fences, park queues, and retain every GPU-visible page table, ring,
command and BO page until DMA retirement is proven. If retirement or reset
cannot be proven, keep those pages pinned and require the independent recovery
path (including a full power removal where needed) before releasing them. A
driver stop callback alone is not evidence that the GPU stopped fetching.

The current supported ownership chain is still missing the app-facing
accelerator client, real BO lifetime, VM mapping/unmapping, context creation,
GFX queue ownership, submit/wait transport, Metal translation and command
encoding, and rendered-output readback. The new memory/queue plan is still
offline metadata, and its approved bounded hardware attempt has not run. The
accelerator remains unpublished until those gaps are closed and each write
stage passes its separate authorization and recovery gates.

## Host policy check

Run `make test` here to verify that inventory-only caps, any missing capability
or evidence bit, stale epochs, and malformed ABI/reserved fields all refuse
readiness. This test does not access Metal or a GPU.
