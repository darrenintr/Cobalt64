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

macOS CI checks only that the test program compiles. A green workflow
cannot claim any rendering. A future dedicated hardware pass of this
program, combined with a proven non-software Navi33 Metal device, is an
acceptance requirement before advertising native acceleration. The
compiled binary is NOT run by CI and not installed automatically.

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
