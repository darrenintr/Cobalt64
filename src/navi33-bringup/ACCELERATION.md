# Navi33 gfx1102 → RADV → Metal development contract

This is NOT a working macOS acceleration driver. The Navi48 Metal bundle and
Mesa patches are hard-wired to gfx1201, the N48N user-client ABI and the Navi48
bringup classes. Pointing that bridge at an RX7600 without a real gfx1102
kernel backend would fabricate GPU capabilities and may hang the single GPU.

Stage G0 (this branch): isolate the N33N ABI and offline GFX11 PM4 framing.
The ABI is generation-specific, 64-byte query shape, zeroed reserved fields,
and explicitly advertises **no** hardware VM, SDMA, GFX, DCN or Metal. The
portable code can build on Linux and macOS but does not touch a GPU.

## Real blockers, in dependency order

1. Validated device ownership / BAR mapping and IP Discovery on the live RX7600,
   preserving GOP and a known-good boot path. Build on Navi33Probe with an
   independent, opt-in N33N IOKit user client, not the Navi48's N48N.
2. Load the correct Navi33 PSP13/SMU13/GFX11/SDMA6 firmware, clocks and queues;
   keep Navi48's PSP14, SDMA7, GFX12 routines entirely separate.
3. Live device-associated DMA ownership, GMC11/GFXHUB/MMHUB page tables, TLB
   invalidation and an SDMA6 copy + fence completion verified by readback.
4. GFX11 MEC/CP queue allocation, packet/IB submission and kernel fence
   retirement; only then advertise N33_CAP_HW_VM | N33_CAP_HW_GFX.
5. Make RADV's Darwin backend enumerate the real gfx1102 with queried chip
   properties and the N33N transport; do not set RADV_DARWIN_FAKE or import
   the gfx1201 canned GPU metadata.
6. Adapt Metal AIR→SPIR-V translation and the Metal device bundle to the
   validated Vulkan device; test actual GPU draw, compute, IOSurface and
   display composition. Never advertise N33_CAP_METAL prematurely.
7. Native DCN3.2.1 modesetting/link encoder, IOFramebuffer and WindowServer
   integration; the current independent DCN321 PR remains under review.

The order matters. A Metal bundle without a GPU-backed Vulkan device is not
hardware acceleration. Host CI is only syntax/contract verification.

## Offline GFX11 Metal render-target formats

`src/Navi33Gfx11Formats.h` contains the first two render-target metadata rows:
Metal `RGBA8Unorm` / `BGRA8Unorm` map to their Vulkan formats, GFX11 color
buffer number/swap fields, and texture-format/channel selectors. The focused
host test checks those enum relationships and rejects unknown formats. This
does not query RX 7600 format features, create a Vulkan device, enable a Metal
capability, or execute rendering; a future RADV backend must query the actual
gfx1102 feature support before exposing either format.

## Stage G2: macOS read-only N33N user client (this PR)

A compiled IOUserClient now responds to `IOServiceOpen("Navi33Probe", N33N)`
and two selectors: HELLO (version/selector count) and QUERY_CAPS (64-byte
struct). It is *disabled* without `navi33-probe=1 navi33-client=1`,
`navi33-client-off=1` takes priority, and administrator privileges are
required. BO_CREATE, BO_FREE, VM_MAP, CTX_CREATE, SUBMIT and WAIT all return
`kIOReturnUnsupported`; the driver does NOT expose any memory mapping,
packet parser, framebuffer takeover or GPU write to clients.

The returned capability flags are computed from actual probed PCI identity
and, if available, validated IP Discovery ROM metadata. HW_VM, HW_GFX,
HW_SDMA, DCN and METAL stay *off*, and VRAM/GTT sizes are reported as 0
until independently measured. An `n33query` userland diagnostic can be
compiled on macOS, but CI does not run it on hardware.

This is a real kernel/userspace communication path, **not** hardware
acceleration or firmware bring-up. Before opening it on the only GPU,
verify a recoverable macOS/EFI boot and ensure no unintended Kext conflict.

## Stage G3: PSP13 live-state query and offline firmware validation

- `N33_SEL_PSP_STATUS` is a new 64-byte, root-only, read-only N33N call.
  It returns `DISABLED` unless both `navi33-probe=1` and
  `navi33-client=1` are set as before, plus `navi33-psp-read=1`
  for live PSP mailbox reads; `navi33-psp-off=1` overrides.
- No unsafe fixed MMIO base guesses: parses the validated
  `ATY,bin_image` IP Discovery metadata for MP0 version 13.0.7 and
  computes bounded BAR5 offsets for MP0 C2PMSG_81 and _58.
- Reads sign-of-life twice with the version between; rejects an unmapped
  all-ones value and any changing or unreadable state. Sign-of-life
  observed does **not** imply GPU command rings are initialized or
  Metal acceleration works.
- Host-only `Navi33Psp13Firmware.h` checks the common header and
  PSP v2.0/v2.1 descriptor table, component bounds, duplicate types,
  overlap, and CRC32-IEEE of the advertised payload.
  This checksum detects accidental corruption, **not** AMD signatures
  or trusted firmware authenticity. Firmware blobs are NOT distributed,
  staged or executed. This is still not PSP bootloader initialization.
- Native macOS diagnostic `n33query` includes the PSP status result.
  The live query has not been exercised on RX 7600 hardware.

## Stage G4: SDMA6 queue-0 readback

N33N selector 9 (64-byte SDMA status) is a **read-only**, root-only
snapshot of GC11 SDMA0 status, firmware checksum, ring control, ring
read pointer and ring write pointer. It is inert unless
`navi33-probe=1 navi33-client=1 navi33-sdma-read=1` and a checksum-valid
Navi33 IP Discovery snapshot are available. `navi33-sdma-off=1`
overrides the opt-in. Uses only validated GC11 BASE_IDX=0 DWORD offsets
from upstream Linux gc_11_0_0_offset.h. No doorbell, firmware reset,
VRAM writes, fences, or actual GPU jobs. Counter movement and queue
configuration have **not** been tested on Navi33 silicon.

## Acceptance gate: first actual RX7600 GPU-rendered Metal frame

`src/navi33-bringup/tools/metal_frame_proof.m` is a native macOS test
that must **not** silently pass on Apple's software framebuffer, the Navi48
bridge, or another GPU. It opens root-gated N33N, demands real Navi33
identification and the HW_VM+HW_GFX+METAL capability bits, enumerates a
Metal device named for RX7600/Navi33, submits a Metal triangle, copies the
GPU-rendered 64x64 texture back to host memory, verifies center/background
pixels, and reports a frame hash/GPU timing. **Current Navi33 Kext
intentionally fails the capability gate.**

The 2026-10-18 SCPM enable command was accepted and RLC boot-complete became
visible, but the combined post-run diagnostic still reports reason 11
(GPU memory/queue ownership not established) and
`GFX11 hardware command submission: DISABLED; Metal: DISABLED`. No Metal
frame was attempted. On 2026-10-18, `metal_frame_proof` compiled and ran in
the RX7600 VM, then exited at the N33N capability gate (`caps 0x00000001`)
before Metal device enumeration or submission. See the physical run evidence
indexed in `AGENTS.md`.

The proof tool now also accepts `--enumerate-only`: it lists the Metal
`IOAccelerator` services and Metal devices macOS exposes even when N33N is
unavailable or advertises no acceleration. It prints the accelerator's
`MetalPluginName` / `MetalPluginClassName` when present. This mode does not
create a command queue, pipeline, texture or GPU command buffer. It is
diagnostic output and cannot pass the frame acceptance gate.

macOS CI checks only that the test program compiles. A green workflow
cannot claim any rendering. A future dedicated hardware pass of this
program, combined with a proven non-software Navi33 Metal device, is an
acceptance requirement before advertising native acceleration. The
compiled binary is NOT run by CI and not installed automatically.

In the 2026-10-18 14:09–14:10 RX7600 VM run, the SDK enum check passed
(`RGBA8=70`, `BGRA8=80`). `--enumerate-only` reported zero Metal devices;
the normal acceptance invocation still stopped at caps `0x00000001`. The
same boot's full diagnostic remained at reason 11, with both context0 values
`0x01fffe00` and Queue0/Queue1 pointers zero. This confirms that macOS has no
Metal device to route to the RX7600 in this setup, and no GPU rendering was
attempted. Full paths are in `AGENTS.md`.

The current Navi33 kext matches only `Navi33Probe` as a diagnostic
`IOService`; it does not publish an `IOAccelerator` service or Metal plugin.
The local Navi48 stack has a separate `Navi48Accelerator` personality with
`MetalPluginName` / `MetalPluginClassName` and a `Navi48Device` subclass of
Metal's `MTLIOAccelDevice`, backed by its RADV/N48N path. Navi33 has none of
those device-discovery or submit components yet. Matching the two pixel
formats is therefore only a small output-format slice of the port.

In the 14:36–14:37 RX7600 passthrough run, `--enumerate-only` found zero
`IOAccelerator` services as well as zero Metal devices. The standard proof
again stopped at caps `0x00000001`; the same boot kept reason 11, an alive
PSP sOS, idle SDMA Queue0/Queue1, and disabled MMHUB/GFXHUB Context0. The
extra registry census narrows the immediate macOS discovery gap: there is no
accelerator personality or plugin to load. It does not change the lower-level
GPU memory/queue blocker or prove any rendering. See `AGENTS.md` for the run
files.

In the 14:47–14:48 combined diagnostic, the one-shot macOS DMA preflight
returned `NoDeviceMapper` (status 2), with zero mapped segments/pages. The
guest IOKit adapter therefore cannot produce a device-IOMMU address for this
VFIO PCI function; this says nothing about a possible host VFIO IOVA mapping.
The RGBA8/BGRA8 Metal SDK enum check passed, but root-run discovery again
found no `IOAccelerator` service or Metal device, and the real-frame proof
stopped at caps `0x00000001`. Reason 11 and disabled MMHUB/GFXHUB Context0
remained unchanged. The run submitted no GPU DMA or command and made no GPU
register writes. Its outputs are recorded in `AGENTS.md`.

The 14:53 RX7600 passthrough run added QEMU virtual Intel VT-d with VFIO
caching mode while still passing through PCI `09:00.0`; it did not use the
`--no-gpu` mode. macOS saw the ACPI DMAR table but logged that `AppleVTD`
was not installed as the system mapper. Kext 0.0.13 therefore returned the
same `NoDeviceMapper` status, with zero segments/pages. The Metal enum check
passed, but root-run enumeration again found zero `IOAccelerator` services
and zero Metal devices; the frame proof stopped at caps `0x00000001` before
creating or submitting GPU work.

Kext 0.0.14 now adds a separate pinned CPU-physical candidate check for the
no-mapper case. It returns `PhysicalAddressOnly` only after the 64-KiB
physical span and CPU shadow PTE image validate. This does not prove PCI DMA
reachability and leaves all GPU, GART, queue and Metal capability bits off;
CI run 39106005328 passed, and the 15:01 VM run returned 1 segment / 16
pages before releasing the allocation. The frame check still found no Metal
device and refused before creating resources or submitting GPU work; the
first rendered frame remains unproven. Full output paths are in `AGENTS.md`.

At 15:16–15:18, a second actual GPU-passthrough VM run (QEMU `vfio-pci`
host `0000:09:00.0`, `rombar=0`; no `--no-gpu`) compiled and ran the current
frame proof on macOS 26.7.1. The VM's existing OpenCore disk had an older
0.0.13 / ABI 14 kext, while the 15:01 run used 0.0.14. The root census still
found zero `IOAccelerator` services and zero Metal devices; the proof exited
at caps `0x00000001`, before the new BGRA/GFX11 guard or any GPU command.
The full read-only run again reported reason 11 and disabled VM contexts.
Run files are indexed in `AGENTS.md`.

## Stage G5: Persistent OS-IOMMU GMC11 / SDMA6 staging (no GPU execution)

A new `Navi33DarwinStage::Session` owns a device-associated macOS
`IOMapper`, a 64-KiB pinned `IOBufferMemoryDescriptor`, and its
`IODMACommand` until the root-only N33N client explicitly releases it
or closes/dies. Mapping and staging require the separate
`navi33-dma-stage=1` boot argument; `navi33-dma-stage-off=1` overrides.
No process can obtain DMA addresses, mapped RAM pointers, or a memory
descriptor through the API.

`N33_SEL_DMA_STAGE` returns a 64-byte status:
- `N33_DMA_STAGED` means device-IOMMU pages are **pinned and retained**,
  a 16-PTE GMC11 GART **CPU image** was generated (not installed in HW),
  and SDMA6 COPY + uncached FENCE + NOP commands were encoded in mapped
  ordinary system RAM (not uploaded to a GPU ring).
- The 64-KiB image reserves 4-KiB source, destination, ring, fence and
  PTE pages. Remaining pages are reserved/zeroed. It stores initial source
  and destination FNV-1a hashes and expected fence cookie, never claims
  the cookie was written by GPU.
- `N33_SEL_DMA_STATUS` is a cached read; `N33_SEL_DMA_RELEASE` always
  revokes mappings and unpins RAM even if the opt-in flag is disabled.
- `clientClose`, `stop` and `free` provide idempotent, serialized
  cleanup. A failed allocation or IOVA lookup drops every partial resource.
- ABI version is **2** so previous N33N userland callers can detect the
  change. All GPU/Metal capability bits remain disabled.

**Cannot yet submit** because GC/GFXHUB/MMHUB VMID, hardware page tables,
HDP flush, TLB invalidation, firmware/engine ownership, SDMA ring base,
wptr/doorbell and GPU fence retirement have not been implemented or
validated. A host test may validate the buffer image but can NEVER prove
that the RX7600 ran it. Do not set `N33_CAP_HW_SDMA` or
`N33_CAP_HW_VM` based on the staging status. No code writes a GPU
register in this branch.

## Stage G6: SDMA6 hardware acceptance gate

`Navi33Sdma6HardwareGate.h` specifies an explicit per-RX7600
`Evidence` chain: PCI identity, PSP/firmware validation, pinned DMA
pages, **installed** GMC11 GPU hardware VM mappings, completed GPU
TLB sync, exclusive SDMA ring ownership, independently tested reset
recovery, actual fence retirement, and matching source/target bytes.

This is intentionally *not* a GPU submit implementation. All inputs to
the host-only test are synthetic. The existing root-gated N33N client
does not populate, trust or consume a fabricated `Evidence` object;
the flag `kDeviceSubmissionImplemented` stays `false`.
Nothing here can claim a successful GPU render or hardware copy.

### Required next concrete hardware work

- Obtain live Navi33 firmware/IP Discovery/PSP status from a safely
  recoverable device, and verify memory-mapped register offsets on silicon.
- Implement device/queue ownership arbitration separate from the
  existing GOP/display driver, with restoration on initialization failure.
- Install *owned* GMC11 VM context and page-table mapping, verify HDP/TLB
  flush and fault reporting on an independent test queue.
- Initialize an SDMA6 ring with exact firmware ABI, wptr/rptr/doorbell
  configuration and an explicit bounded timeout/recovery.
- Use a GPU-written fence in mapped CPU-readable memory and read back
  the 4-KiB destination, proving no CPU side copy occurred.

Do **not** use a single display GPU for the first destructive MMIO
write without an independent headless/remote recovery console and a
known rollback environment. CI and software host tests cannot replace
hardware observations.

## Stage G7: actual SDMA6 MMIO write backend (isolated, NOT run on GPU)

`src/Navi33Sdma6MmioQueue.h` implements the **write-capable** SDMA6
GC11 Queue0 register protocol, as opposed to the earlier CPU ring
simulator and software readiness gate. It has a checked
`configure → submit → waitAndVerify → stopAfterCompleted` path:

1. Require separately proven firmware, exclusive queue ownership,
   device-installed GMC11 VMID0, TLB synchronization, GPU-accessible
   addresses, DMA pin lifetime, cache maintenance and independent
   GPU recovery. None of these is inferred from a mock test.
2. Validate a full 4KiB source pattern, poisoned 4KiB destination,
   16-DWORD COPY+FENCE+NOP ring, zero-initialized fence memory, and
   exact ring/source/destination/fence GPU VA relationships.
3. Refuse a busy/doorbell-enabled ring or nonzero RPTR/WPTR. Snapshot
   Queue0 register state, program `RB_BASE[_HI]`, RPTR/WPTR,
   RB size 1024 DWORDS (4KiB), VMID0, privileged ring, then
   verify disabled-state readback.
4. After a device-specific cache-sync callback succeeds, enable RB
   and write `RB_WPTR=64` **bytes** through GC MMIO (not a made-up
   16-DWORD pointer). The tail is a UC `SDMA_OP_FENCE` packet.
5. Poll CPU-visible fence under cache invalidation, compare **all
   4096 destination bytes** against their source and expected pattern,
   then verify RPTR/WPTR have reached 64 bytes. Only then disable RB
   and permit its IOMMU mapping to be reclaimed.
6. On a failed readback/timeout after submission, require external
   recovery; **never release pinned buffers** simply because a
   host-side timeout has elapsed. A queue configured but not kicked
   may use `abortBeforeSubmit`.

The hardware write implementation is under
`COBALT_NAVI33_ENABLE_UNSAFE_MMIO`. **The shipping Kext does not
define it, import this module, or expose its callbacks to N33N.**
Host tests compile with this macro and use a fake MMIO bus and
synthetic GPU response, plus a separate default-disabled test.
No physical RX7600 instruction has been submitted or retired by this PR.

**Why it remains isolated:** The existing `Navi33DarwinStage`
holds OS-IOMMU IOVAs and constructs an offline GART PTE image. The
GPU addresses in that image are candidate addresses, not proven
installed GC VM mappings. A real macOS GMC11 MMHUB/GFXHUB root page
table, TLB flush, firmware queue ownership and recovery path are
still needed before the backend may receive a real MMIO callback.
Never simply pass an OS DMA IOVA into `ringGpuVa` or turn on the
compile gate on your only display GPU.

## Stage G8 — GMC11 MMHUB3 real VMID0 + TLB invalidate backend

`Navi33Gmc11Mmhub.h` implements a separate, **write-capable** GMC11
VMID0/GART MMIO transaction for Navi33. It is compiled only when
`COBALT_NAVI33_ENABLE_UNSAFE_GMC11_MMIO` is defined in an isolated
test backend. It is **not included, linked or called** by the
`Navi33Probe.kext` production build.

The register plan uses a validated Navi33 IP Discovery ROM to locate
MMHUB instance 0. Two Linux-verified MMHUB layouts are selected *only
when the real IP revision matches*: 3.0.0 (`mmhub_3_0_0_offset.h`)
and 3.0.2 (`mmhub_3_0_2_offset.h`). Unsupported versions and
out-of-range/duplicate BAR5 offsets fail closed.

The experimental transaction does the following:

1. Require independent ownership of the **entire MMHUB** and absence
   of active display users/other GPU jobs, verified firmware/PSP/HDP
   and pre-initialized MMHUB cache, trusted GPU-physical page-table
   memory and an independently recoverable GPU.
2. Pre-read Context0, page-table base/start/end, invalidate request/
   acknowledgement and protected invalidation range. Reject active
   Context0, outstanding requests and stale VMID0 ACK.
3. Validate all 16 GMC11 system-memory PTEs, including a read-only
   source page and writable destination/ring/fence; synchronize the
   pinned table for GPU access. Write base/start/end and the
   engine-17 invalidation range **while Context0 is still disabled**.
   Read back values and compensate/re-verify failures.
4. Activate Context0 at depth zero, flush HDP, acquire MMHUB
   invalidate semaphore, issue Linux-style request
   `0x00f80001` for VMID0, poll bounded ACK, release semaphore,
   then perform the required MMHUB private cache invalidation.
5. On partial activation, ACK timeout or ambiguous MMIO: report
   `RecoveryRequired`, NEVER authorize DMA unmapping. There is a
   rollback method only **before** activation, when Context0
   remains disabled; no unsafe post-submission blind rollback.

The existing macOS `Navi33DarwinStage` now retains the page-table
backing **OS IOMMU address** privately. This is intentionally named a
*device IOVA candidate*, **not** a proven GPU MC-physical page-table
address. The missing verified IOVA→GPU-MC translation and ownership
prevents any real activation on the current single-GPU macOS host.

Tested in Linux/GCC/Clang/ASan/UBSan using a fake BAR5 MMIO model,
including missing/incorrect ROM MMHUB revisions, corrupt page-table
entries, busy contexts, partial writes, rollback failure, semaphore
timeout, invalidate ACK timeout, and the production-default
write-disabled build. macOS CI compiles the existing Kext but does
not instantiate MMHUB MMIO code or access a physical GPU.

**Not yet a real RX7600 workload:** native GPU context memory mappings
are unverified, there is no safe MMHUB ownership on a GOP-backed
single-GPU display, and the SDMA6 write backend remains isolated.
Do not enable either unsafe compile flag on the primary video card.

## Stage G8b — GFXHUB3 companion mapping and paired flush

The same isolated GMC11 transaction backend now supports the **GC11
GFXHUB3 (gfx1102)** register map (GC IP Discovery BASE_IDX 0).
Linux `gfxhub_v3_0.c`, `gc_11_0_0_offset.h`, and its field-mask
header define the Context0 base/start/end, engine-17 invalidate
request/ACK and range. GFXHUB deliberately **does not acquire the
MMHUB semaphore or issue the MMHUB-only private invalidate**.
Independent Host MMIO simulations verify both layouts and the
different invalidate paths.

`dualHubReady(mmhub,gfxhub)` requires both backends to have passed
their *own* TLB ACK, the correct Hub identities, and the **same verified
GPU-physical page-table root address**. One valid Hub alone is not
accepted as ready for SDMA6.

This remains opt-in **compile-time isolated and not linked to the
macOS Kext**. A simulated `dualHubReady` pass is not proof of GPU
execution. The existing pinned-memory staging yields a device-IOMMU
address, which still needs independent validation as a GPU MC address
before supplying it to either Hub. GPU graphics/rendering is not yet
available.
