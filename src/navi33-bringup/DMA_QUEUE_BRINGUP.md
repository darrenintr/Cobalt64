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

## Stage G11: NBIO capacity census, DMA owner lifecycle, and mock-only BAR0 root upload

Branch: feature/navi33-nbio-vram-capacity-lifecycle (draft follow-up to
the GMC11 MC geometry/VRAM BO ledger). This stage does NOT enable 3D or
Metal, and is NOT a working RX7600 hardware-copy driver.

- Fix N33N user-client stop and clientClose: release the 64-KiB pinned
  DMA staging buffer AND its provider-wide lease while the Navi33Probe
  owner pointer is still valid. Before this, a dead root client could
  strand an epoch and make later DMA_STAGE requests report BUSY.
- Navi33Nbio43Capacity.h implements a bounded, read-only NBIO 4.3.0 or
  4.3.1 RCC_CONFIG_MEMSIZE register plan. The register is at DWORD
  0x00c3, IP Discovery NBIF BASE_IDX=2, as in Linux nbio_v4_3.c
  and nbio_4_3_0_offset.h. The capacity field is in MiB.
  Two identical reads, basic capacity plausibility, and valid ROM/IP
  revision/BAR5 bounds are mandatory.
- N33N ABI **v6** expands the root-only VRAM survey from 64 to 96 bytes.
  Besides the paired MMHUB/GFXHUB framebuffer registers, it separately
  reports stable NBIO capacity, reads, revision, and only when both
  pass a GMC11 capacity-fit check, the validated geometry:
  N33_VRAM_FLAG_MC_GEOMETRY_VALID.
  A stable NBIO result is NOT authorization to allocate any VRAM.
- The macOS n33vram --read CLI prints the additional NBIO capacity
  state, MiB size, and checked GPU MC base. Rebuild both N33N user
  client and diagnostic tool for ABI v6. It remains opt-in:
    navi33-probe=1 navi33-client=1 navi33-vram-read=1
  Kill switch: navi33-vram-off=1.
  BAR5/MMIO operations on this selector are strictly READ ONLY.
- Navi33Gmc11VramUpload.h is an isolated CPU BAR0 upload *backend*.
  It requires an independently owned 4-KiB VRAM BO lease in the
  existing software ledger, a verified physical root address,
  a clean VRAM destination, external PCI/firmware ownership, a
  transport-specific CPU-to-device BAR0 write sync, and an independent
  device recovery facility. It writes the 512-word diagnostic PTE
  image and verifies all words through callbacks only if the unsafe
  compile macro is defined. Before the first potentially live write
  it permanently quarantines the memory lease. It never activates
  GPU VM, never submits SDMA, and cannot claim GPU work.
- Production Navi33Probe.kext DOES NOT include or link this uploader,
  and no user-facing boot argument or N33N selector enables it.
  Only tests/test_gmc11_vram_upload.cpp defines the unsafe macro
  against a synthetic VRAM buffer. tests/test_gmc11_vram_upload_off.cpp
  confirms the default path is disabled. CI must not be mistaken
  for real RX7600 BAR0 readback or any physically allocated VRAM BO.

**Remaining first-hardware-copy blockers**:
1. A real OS/firmware-approved exclusive PCI + VRAM ownership handover
   that does not steal an active single-GPU GOP/macOS framebuffer.
2. Real VRAM BO reserve/pin/unpin and BAR0 aperture-mapped access,
   including firmware/display memory exclusion and live fault handling.
3. PSP/SMU firmware and GPU clocks, coherent root/IOVA mapping,
   fully initialized GMC11 TLB caches and system aperture registers.
4. Exclusive SDMA6 ring control and independent hardware reset/teardown.
5. Proof that silicon executed COPY/FENCE and wrote the result without
   CPU emulation; a graphics/Metal bridge is further work.

Nothing here flashes VBIOS, programs SPI flash, changes permanent
GPU parameters, or starts a real command on the user's RX 7600.

## Stage G12: Queue0 register-stability preflight and irreversible DMA quarantine

Branch: feature/navi33-sdma6-queue-quarantine-preflight (draft follow-up
to the NBIO capacity/VRAM uploader prototype). No real silicon compute.

- The isolated Navi33Sdma6MmioQueue.h now reads two complete Queue0
  register snapshots (control, doorbell, read/write pointers, ring base)
  and refuses to touch MMIO if they differ, are inaccessible, are all-ones,
  or suggest an already-initialized/owned queue. It rechecks the expected
  values immediately before the first write.
- These snapshots only detect conflicts; they are NOT proof of exclusive
  ownership. A future, independent OS/firmware-approved PCI owner must
  still attest exclusive access and hardware reset.
- The first attempted Queue0 register write now sets a sticky quarantine
  flag BEFORE calling the transport. A failed callback may have
  committed to silicon; there is deliberately no speculative write-back
  of "old" registers after a failure. Physical reset and verified VM
  teardown remain prerequisites before unpinning IOMMU/VRAM memory.
- A successful 4096-byte simulated COPY/FENCE, queue disarm, or
  abort-before-submit does NOT clear that sticky quarantine. These
  operations cannot prove that both GMC11 VM hubs released the backing
  pages. requiresIndependentResetBeforeUnmap() reports the state.
- The production macOS Kext still does not link this experimental
  backend and no boot argument enables it. GCC/Clang/ASan CI test a
  synthetic register and DMA model only. Fault tests include register
  changes between snapshots, a queue changing during pre-write validation,
  a write that reaches hardware but returns failure, and abort/stop.
- This stage intentionally does not acquire PCI ownership, initialize
  PSP/GMC11, map GPU-visible live VRAM, issue a real SDMA command or
  provide native Metal acceleration.

## Stage G13: N33N ABI v7 real read-only Queue0 BAR5 census

- Navi33Sdma6QueueCensus.h uses verified Navi33 IP Discovery (GFX11 GC
  BASE_IDX0, BAR5 bounds) to resolve eight SDMA0 Queue0 register offsets:
  control, doorbell, low/high RPTR, low/high WPTR and low/high ring base.
  The observer performs TWO full read-only snapshots, detects instability,
  rejects invalid all-ones reads, and never assumes a parked queue is free.
- New N33N root-only selector 16 returns a 64-byte v7 status structure,
  including the register image, 16-read count, stability state and flags.
  No acceleratation / GPU-owned-memory capabilities are set.
- Navi33Probe.kext hosts a root-gated query using its EXISTING read-only
  BAR5 mapping callback, opt-in with the existing boot arguments:

      navi33-probe=1 navi33-client=1 navi33-sdma-read=1

  Kill switch: navi33-sdma-off=1 (or navi33-client-off=1). No status
  polling is required at boot; only the explicit root client query
  reads physical registers.
- Compile the native CLI with this same Kext/ABI revision and invoke
  sudo n33compute --queue for a physical, *observation-only* report.
  An inactive ring with zero base/pointers is only 'apparently parked';
  firmware/driver PCI ownership is NOT proven by these observations.
- Host tests include a mocked 16-read snapshot, changes between reads,
  invalid register layouts/offsets and unavailable/unmapped BAR5 state.
  Hardware writes, ring enable and GPU execution remain absent.
- The diagnostic is not sufficient to enable unsafe GMC11 or SDMA writes.
  No real RX7600 SDMA COPY/FENCE, GPU BO ownership, or Metal compute proof
  exists at this stage.
