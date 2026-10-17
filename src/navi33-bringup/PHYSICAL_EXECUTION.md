# Navi33 physical PCI / IOMMU / BAR transport milestone (PR #20)

**Status:** The macOS source now contains a real, compiler-verified
`IODMACommand`-backed staging session and isolated physical BAR0/BAR5
adapters. **This is NOT a functional RX7600 SDMA6/GFX11/Metal driver.**
No register write, hardware COPY or GPU fence has been submitted on a
physical Navi33 by Cobalt64.

## Physical-resource path now present

1. `Navi33DarwinStage::Session`: obtains this specific RX7600's
   `IOMapper::copyMapperForDevice`, retains a 64-KiB prepared
   `IOBufferMemoryDescriptor` and `IODMACommand`, and builds
   pinned IOVA-backed source, destination, ring and fence staging bytes.
   `readByte()` and `synchronizeForDevice()/synchronizeForHost()`
   use **the same retained mapping** instead of fake host arrays.
   This does **not** put a GPU page table in VRAM.
2. `Navi33DarwinQueue1IO`: takes a physical, identity-checked
   `IOPCIDevice`, validated gfx1102 IP Discovery, and the retained DMA
   session. It maps BAR5, converts GC Queue1 register indices through
   checked `Navi33Sdma6BarAdapter`, and returns callbacks compatible
   with the isolated `sdma6_mmio::IO` transport. CPU-side ring/fence
   access now uses the real pinned descriptor and device cache sync.
   All hardware register WRITE callbacks currently return **false**.
3. `Navi33DarwinRootReadback`: maps BAR0 only for readback of an
   externally leased 4-KiB GMC11 PTE root; it verifies the BAR mapping,
   GPU physical root, CPU-visible offset and root page bounds before
   returning 64-bit words for `vram_root::inspect`. It does not allocate
   VRAM or prove the lease belongs to this driver. No BAR0 writes.
4. `Navi33DmaLifetime`: a fail-closed pin lifecycle. Before a future
   real MMIO attempt, a hardware owner must mark the retained mapping
   Quarantined. A failed callback may still have reached silicon.
   Quarantined mappings are **not completed/unmapped even on user-client
   close**, and the provider-wide software DMA lease cannot be recycled.
   The only safe unpin path would require separately implemented,
   independently verified device reset. This code offers no reset or
   force-release method.
5. The production `Navi33Probe.kext` still includes only the
   read-only diagnostic and DMA staging. The BAR0/BAR5 sidecars are
   compiled to object files on Intel macOS CI with
   `make physical-io-check`, **never linked, loaded, or installed**.
   Linux GCC/Clang and sanitizer runs test the DMA lifetime policy.

## Actual hardware compute remains BLOCKED

A Queue1 ring and a ROM/register read do not confer ownership.
The following are required before a write-capable kernel backend can
safely bind to any physical RX7600:

- A genuine exclusive PCI/display/firmware authority and dedicated
  independently resettable recovery device. The sole GOP display GPU
  does not satisfy this. An `IOPCIDevice::open()` result alone is not
  proof that firmware, DCN scanout, PSP or other drivers have stopped.
- Firmware authenticated for Navi33 (MP0 PSP13, SDMA6.0.2, SMU13,
  GFX11), verified boot sequence, cache policy and clocks. Firmware
  images load into volatile GPU memory; this work does NOT call for
  flashing the GPU or changing VBIOS.
- A *real* reserved GPU-VRAM BO and a coherent BAR0 root page-table
  upload. An ordinary system-RAM IOVA is not an AMD GPU MC physical VRAM
  root. Confirm VMID1 page-table DEPTH/BLOCK/SYSTEM flags against the
  actual silicon and both MMHUB3/GFXHUB3 independently.
- Exclusive Queue1 ownership, confirmed SDMA0 engine status, correct
  Queue1 RB/doorbell/VMID registers, HDP and TLB invalidation, IOMMU
  unmap lifetime, IRQ/fence ordering, and an independently tested reset
  that can quiesce SDMA without corrupting display-owned Queue0.
- Actual hardware readback: copy a known 4096-byte pattern using
  **GPU-owned** SDMA6 packets, observe a GPU-written uncached FENCE
  cookie, verify Queue1 RPTR/WPTR, synchronize the device DMA mapping
  for CPU, check every destination byte matches, prove no CPU code
  performed the copy and record a hardware execution trace. No
  synthetic CI test can satisfy this requirement.
- Only **after** the above, implement GFX11 MEC/CP submission,
  shader compilation, gfx1102 RADV integration, Metal translations,
  IOSurface presentation and WindowServer acceleration.

## Build and safety

Host-only regression:
`make -C src/navi33-bringup clean host-test`

macOS isolated bridge syntax check with MacKernelSDK:
`make -C src/navi33-bringup MKSDK=/path/to/MacKernelSDK physical-io-check`

Neither command installs a driver or issues a GPU command. The
deliberate lack of a firmware/PCI/reset-aware physical writer is not a
CI bug and must not be bypassed by advertising `N33_CAP_HW_SDMA`,
`N33_CAP_HW_GFX` or `N33_CAP_METAL`.

**Do not load unsafe MMIO implementations into the only RX7600 that
currently drives the display.** Compile success is not macOS Metal
support and does not make any OpenCore Kext safe to install.
