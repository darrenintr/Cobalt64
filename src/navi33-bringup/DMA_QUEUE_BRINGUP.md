# Navi33 RX 7600: true DMA/GMC11 mapping and SDMA6 ownership

STATUS: hardware bring-up only. Not a working accelerated macOS GPU driver.

SAFETY: Never enable experimental VMID0/SDMA6 register writes on the only
active display GPU. SSH, Moonlight and Sunshine do not provide an independent
hardware reset path.

## What is implemented in this branch

1. Navi33DarwinStage retains a real device-IOMMU IODMACommand mapping of
   pinned 64-KiB host memory. After preparing the source/PTE/ring CPU image,
   it explicitly calls synchronize(kIODirectionOut) and fails closed on error.
2. Navi33StageOwner provides an IOLock-protected, provider-scoped exclusive
   owner for N33N pinned staging sessions. Stale epochs cannot release
   another client. This is NOT GPU queue ownership.
3. Navi33Gmc11RootVram checks that a separately allocated, owned VRAM BO
   contains the expected 16 system PTEs and zeroes for the remainder of
   the entire 4-KiB GART root; it does not allocate any VRAM.
4. Navi33Sdma6QueueOwner grants a unique software Queue0 reservation
   and poisons the lease BEFORE a potentially irreversible MMIO write.
   Only an independent reset could establish a safe fresh owner.
5. Navi33Sdma6ProofRun requires a 512-entry VRAM readback and unique queue
   lease before invoking MMHUB, GFXHUB, and SDMA6. It is not linked into
   Navi33Probe.kext. All tested GPU behavior is simulated.

## Three addresses must not be mixed up

- CPU virtual pointer: host address of the pinned staging image.
- Device DMA IOVA: system-memory pages mapped through IODMACommand for
  this PCI device, potentially scattered. A valid IOVA is not a proof
  that the GPU's VM hub is correctly configured to reach the memory.
- GPU MC-physical VRAM address: base of a dedicated-GPU-VRAM GART root
  BO. Linux gmc_v11_0 uses amdgpu_gart_table_vram_alloc to allocate
  such a BO. It is not the system-RAM IOVA.

The current candidateTableDeviceIova() is a shadow PTE-page IOVA;
do not program VM_CONTEXT0_PAGE_TABLE_BASE_ADDR with it.

## Unfinished physical-hardware tasks, in dependency order

1. Exclusively open/control the PCI device in a separate test driver.
   Confirm the bootloader, macOS display stack, and PSP/firmware are not
   competing for Queue0/VMID0. An idle register is NOT an ownership
   guarantee. Refuse takeover if the card drives the active desktop.
2. Create a dedicated GPU VRAM allocator. Read/validate live VRAM MC
   location and aperture registers; reserve, pin and own 4KiB for the
   GART root without overlapping firmware/GOP/display memory. Never
   guess which BAR0 VRAM bytes are free. Translate VRAM BO offsets to
   verified GPU MC addresses.
3. Write the full 512-entry GART root into the owned VRAM BO, read
   it back from real BAR0-mapped VRAM and hold the BO for the entire
   device lifetime. Validate the system IOMMU IOVA pages and whether
   the GPU's system aperture can actually access those bus addresses.
4. Under exclusive ownership only, install GMC11 MMHUB/GFXHUB VMID0
   root/start/end registers, synchronize the GPU HDP, invalidate TLB,
   verify fresh acknowledgements and examine faults. Neither VM
   context may be owned by another display/compute client.
5. Verify PSP13/SDMA6 microcode, clocks, SDMA ring id, queue state,
   ring base, rptr/wptr and doorbell mode. Submit COPY + FENCE on an
   isolated card, prove GPU fence retirement and byte-for-byte 4KiB
   copy with no host CPU-side copy.
6. Implement independent reset and full queue/VM restoration. A
   timeout or IOKit client death must not unpin live GPU memory.
   Demonstrate recovery on real hardware before connecting the
   experimental backends to the shipping Kext.
7. Only after DMA succeeds: port gfx1102 GFX11 CP/MEC command
   submission, RADV transport, actual Metal bridge and native display.

## Acceptance checks

- Real macOS root N33N device IOVA with cache synchronize: code ready;
  physical RX7600 staging not exercised.
- Provider-wide DMA staging exclusivity: code ready; host-tested.
- Readback of a genuinely owned GPU VRAM page-table BO: helper ready;
  no allocator or BAR0 hardware readback yet.
- Exclusive hardware Queue0/PCI owner: NOT IMPLEMENTED; software
  reservation alone cannot establish this.
- Real MMHUB/GFXHUB TLB activation and live fault/ACK proof: NOT DONE.
- Real GPU-written SDMA6 fence and verified 4096-byte copy: NOT DONE.
- Metal 3D/compute: NOT DONE.

References: Linux AMDGPU gmc_v11_0.c, amdgpu_gart.c, sdma_v6_0.c;
Apple Kernel IODMACommand::synchronize and PCI device ownership APIs.

## Stage G10: VRAM MC address translation and read-only Kext census

Branch: feature/navi33-vram-allocator-mc-geometry (stacked on the G9
DMA/queue-lease prototype). This is NOT working GPU compute/Metal.

- Navi33Gmc11VramGeometry.h uses validated MMHUB3 + GFXHUB11 FB_BASE,
  FB_TOP (inclusive), and FB_OFFSET to translate three distinct spaces:
  (a) BO offset into dedicated VRAM, (b) GPU MC address = VRAM MC
  base + BO offset, (c) GPU physical address = FB_OFFSET + BO offset.
  BAR0 aperture offsets are reported only when the range is visible.
  This follows Linux amdgpu_gmc_vram_mc2pa, not CPU RAM IOVA reuse.
- Navi33VramBoAllocator.h manages a fixed-size interval ledger within a
  region ALREADY EXCLUSIVELY OWNED by some future, real device/firmware
  owner. It rejects unauthorized or overlapping reservations, supports
  generation-safe leases, and never reuses any page after a potentially
  live GPU transaction. It has no hardware allocation or VRAM writer.
- Navi33Gmc11VramLease.h binds an existing 4-KiB ledger lease to the
  complete 512-entry GART root readback acceptance gate. The isolated
  SDMA6 proof harness refuses fabricated root pointers and quarantines
  the lease before GPU register writes are even attempted.
- The shipping Navi33Probe.kext now exposes N33N ABI **v5**, root-only
  selector N33_SEL_VRAM_GEOMETRY=15. It checks a checksum-valid Navi33
  IP Discovery ROM, validates MMHUB 3.0.0 or 3.0.2 and GC 11.0.2
  BASE_IDX0 offsets against the assigned BAR5, reads six FB registers
  TWICE, and rejects inconsistent/unmapped reads. The status is read-only
  and DOES NOT prove physical VRAM size or ownership.
- The native macOS CLI **n33vram --read** is compiled by CI and can
  query the live MMHUB/GFXHUB raw values after loading the Kext with
  explicit boot flags:

      navi33-probe=1 navi33-client=1 navi33-vram-read=1

  Kill switch: navi33-vram-off=1. The normal Kext makes NO GPU writes,
  even if these flags are enabled. Do NOT copy experimental EFI/Kext
  into a single-GPU boot path without a separately tested recovery EFI.
- Physical NBIO-reported VRAM capacity, a trustworthy GPU VRAM owner,
  exact firmware/display reservations, BAR0 real read/write upload and
  safe GPUMMU/SDMA6 queue activation remain unimplemented. CI uses fake
  MMIO, fake VRAM BOs and synthetic register values. Do not claim an
  actual RX7600 copy, free VRAM page or Metal acceleration.
