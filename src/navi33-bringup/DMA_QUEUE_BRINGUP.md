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


## Stage G10: live MMHUB3 framebuffer census, N33N ABI v5

Navi33VramCensus.h uses exact Linux MMHUB offsets:
- MMHUB 3.0.0: FB_LOCATION_BASE 0x08ec, TOP 0x08ed, FB_OFFSET 0x08d7.
- MMHUB 3.0.2: FB_LOCATION_BASE 0x086c, TOP 0x086d, FB_OFFSET 0x0857.

All three values contain 24-bit fields in 16 MiB units. After checking
checksum-valid Navi33 IP Discovery and the assigned BAR5 bounds, the
Kext reads each register twice. It rejects unstable, overflowing or
implausibly large values. No GPU registers or VRAM are ever written,
no BAR0 mapping is attempted.

N33N root selector 15 returns a 64-byte VRAM geometry snapshot.
A recoverable, working EFI must explicitly opt in using the boot
arguments navi33-probe=1 navi33-client=1 navi33-vram-read=1.
The kill switch navi33-vram-off=1 always wins. The macOS n33vram
CLI prints the live FB window, the independent MC FB offset and
the PCI BAR0 assigned aperture. These are NOT free-VRAM estimates.

Navi33VramBoLedger.h implements page-aligned BO suballocation inside
an EXTERNALLY OWNED subpool, excluding firmware/GOP ranges, rejecting
double frees or stale epochs, and never unpinning memory after GPU
access could have started. Separate FB-location and FB-offset MC
address candidates are produced; they are not interchangeable and
must be verified before any GPU page-table register is programmed.
The ledger cannot guess a free BAR0 region and does not actually
write physical VRAM.

This branch also fixes an N33N client lifecycle bug: release the
provider-scoped DMA stage lease BEFORE clearing the provider pointer
on clientClose()/stop(). This prevents a stale owner from blocking
the next client.

Hardware blockers remain: independently proven exclusive PCI/VRAM
ownership, firmware/GOP allocation map, real pinned VRAM BO allocation,
physical GART upload, verified VMID0 ownership and independent reset.
The production Kext still has NO VRAM writing or GPU command path.
