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

The 2026-10-18 15:01 RX7600 VM run with Kext 0.0.14 exercised only the
read-only no-mapper fallback: it pinned 64 KiB of physically contiguous
system RAM, validated 1 segment / 16 pages in a CPU shadow PTE array, then
released it. The result was `PhysicalAddressOnly`, not a device IOVA or a
proof that VFIO/GPU can reach the pages. It did not change any of the
ownership or real-VRAM-root prerequisites below. macOS still exposed no
`IOAccelerator` service or Metal device, so no Metal command was attempted.
Full run evidence: `AGENTS.md`, run `~/macos tahoe/navi33/runs/20261018-150123/`.

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
  the no-mapper CPU-physical candidate path was exercised, but actual
  device-IOMMU staging and GPU reachability remain unproven.
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

## Stage G14: independent SDMA0 Queue1 + GMC11 VMID1 host transport

This branch replaces the previous **VMID1-with-Queue0** test mismatch
with Queue1's actual register bank and a matching two-hub VM context.

- SDMA0 Queue1 RB_CNTL=0x00d8, BASE=0x00d9, RPTR=0x00db,
  WPTR=0x00dd, DOORBELL=0x00ea, MINOR_PTR_UPDATE=0x010d.
  They are in the GFX11 GC BASE_IDX0 bank and differ from the Queue0
  registers by 0x58 DWORDs. The BAR5 adapter validates the selected
  bank and refuses Queue0 register writes by a Queue1-only adapter.
- GMC11 MMHUB v3.0.2 Context1: CNTL=0x06c1, TABLE_LO=0x072d,
  START_LO=0x074d, END_LO=0x076d. GFXHUB gfx1102 Context1:
  CNTL=0x1689, TABLE_LO=0x16f5, START_LO=0x1715,
  END_LO=0x1735. Context0 remains untouched in synthetic tests.
  Invalidation register **addresses are shared**; the requested/acked
  VMID bit is 2 for VMID1, not 1 for VMID0.
- The isolated COPY/FENCE orchestrator rejects mismatched VMIDs,
  Queue1 without an independent ownership attestation, or one
  incorrectly substituted Queue0 register bank. Host fake-GPU tests
  exercise MMHUB1 + GFXHUB1 + SDMA0 Queue1 and check the 4096-byte
  output. They also prove no Queue0 ownership flag is required by the
  *software* Queue1 path.
- Any attempted GMC11 or SDMA MMIO write permanently quarantines the
  related GPU-visible allocations in the controller and device ledger.
  Even a clean-looking abort cannot justify IOMMU/VRAM unpinning,
  reinitialization or reuse without externally validated GPU reset.
- Both experimental write-capable components remain compiled only
  with explicit unsafe build macros in the **offline host harness**.
  Neither writer is linked into the macOS Navi33Probe Kext.

This is NOT yet RX7600 hardware computation, 3D acceleration or Metal.
The booleans in the isolation contract are fake test evidence and cannot
replace actual PCI exclusion, PSP/SDMA firmware initialization,
per-VMID page-table depth/permissions review, GPU IOMMU/VRAM ownership,
silicon TLB/cache behavior, a physical fence, or a provable GPU reset.
Queue1 still shares SDMA0 silicon and firmware with Queue0: using a
different queue does NOT make the GPU independent of its display owner.

## Stage G15: offline Queue1 configuration manifest

`Navi33Sdma6Queue1ConfigPlan.h` flattens the planned one-page owned-VRAM
GART root and SDMA0 Queue1 / VMID1 COPY+FENCE sequence into ordered
register/value and readback steps. It uses the Navi33-only GMC11 and SDMA6
plans and exact-IP offsets; it has no hardware I/O, writer, Kext wiring or
capability publication. The manifest includes a descriptive field for the
already approved one-shot attempt; creating or inspecting it does not
execute or consume that approval.

Immediate physical blockers remain: no production exclusive PCI, both-hub
and Queue1 owner; no real pinned 4 KiB VRAM root with BAR0 write/readback;
no device-associated pinned DMA mapping and cache-maintenance path; and no
independent recovery proof. The current macOS display-GPU ownership also
prevents treating synthetic ownership flags as evidence. No physical attempt
has run. Host tests validate only the offline manifest and its refusal cases.

## Stage G16: proof-specific memory and GPU-VA contract

`Navi33Sdma6Queue1MemoryContract.h` binds a successful G15 manifest to
opaque owner/object generations, one pinned 64 KiB system staging object,
its sixteen unique device-DMA pages, the separately owned 4 KiB VRAM root,
and the exclusive 64 KiB GPU-VA reservation. It labels the source, target,
ring, fence, GART shadow and remaining mapped pages, and checks every role
against the planned PTE and Queue1 addresses. It only validates declarations;
opaque identities and booleans are not physical evidence, and the contract
cannot allocate, pin, map, access hardware, or publish capabilities.

This is scoped to the existing single COPY/FENCE proof. Its fixed flat GART
image and whole staging mapping do not define a general Metal BO lifecycle.
The graphics queue/draw path remains blocked by the unsupported BO/VM/CTX/
SUBMIT/WAIT ABI and the absence of a production GFX queue owner and verified
queue initialization path. No additional register offsets are introduced.

Authoritative register references: Linux AMDGPU headers
gc_11_0_0_offset.h, mmhub_3_0_2_offset.h and mmhub_3_0_0_offset.h.

## Stage G17: offline GFX11 MEC RS64 enable word

`Navi33Gfx11MecEnable.h` reproduces the value upstream
`gfx_v11_0_cp_compute_enable()` writes to `CP_MEC_RS64_CNTL` (gc_11_0_0
dword 0x2904, GC segment 1; the same offset as the N33N peek window). It
computes the word from the value already read, touching only the ICACHE,
pipe reset, pipe active and halt fields. Offsets and bit positions come
from `gc_11_0_0_offset.h` and `gc_11_0_0_sh_mask.h` on
torvalds/linux master (fetched 2026-10-18). The header performs no register
I/O. `test_gfx11_mec_enable` checks the enable and disable words, bit
preservation, and round trips. A mutant that drops the pipe-active bit fails
it.

Not done: the RS64 microcode load (`gfx_v11_0_cp_compute_load_microcode_rs64`),
the MQD and HQD programming (`gfx_v11_0_compute_mqd_init`,
`gfx_v11_0_kiq_init_queue`), and any ownership of the queue itself. Those
remain behind the Queue0/PCI owner and the physical approvals above.

## Stage G18: offline GFX11 KIQ HQD init register plan

`Navi33Gfx11KiqHqd.h` turns the KIQ's queue inputs into the ordered register
writes of `gfx_v11_0_kiq_init_register()`, with the mqd-derived values from
`gfx_v11_0_compute_mqd_init()`. The `_DEFAULT` values come from
`gc_11_0_0_default.h`, the masks and shifts from `gc_11_0_0_sh_mask.h`, and
the offsets from `gc_11_0_0_offset.h`. All of them are on torvalds/linux
master, fetched 2026-10-18. `amdgpu_ring.c` sets `hqd_active` only for the
KIQ and `kernel_queue` for every ring. The planner checks alignment, sizes,
address width, doorbell range and `hqd_active`. It refuses a queue that is
already active, because the dequeue path is not modelled. It does no I/O.
`test_gfx11_kiq_hqd` checks golden values derived by hand and the refusal
cases. A mutant that clears UNORD_DISPATCH fails it.

On the PSP load path, upstream does not run the driver-side RS64 MEC
microcode load (`gfx_v11_0_cp_compute_load_microcode_rs64`), because
`cp_resume` takes that branch only for `AMDGPU_FW_LOAD_DIRECT`. The PSP loads
MEC through the IP firmware records, so the load is not planned here.

Not done: the MQD memory image (the `v11_compute_mqd` layout), the KIQ
MAP_QUEUES packets that bring up the KCQs, and any GPU memory the
MQD/EOP/ring/rptr buffers live in. All of these need the GPU VM from G16.

## Stage G19: offline KIQ MQD memory image

`Navi33Gfx11Mqd.h` builds the 2 KiB `v11_compute_mqd` image for the KIQ. It
takes its values from the G18 register plan, so the register writes and the
image cannot drift apart. The field dword indices come from struct
`v11_compute_mqd` in upstream `v11_structs.h` (torvalds/linux master, fetched
2026-10-18). A script checked all 33 indices against the `offset:` comments
in that header, with no mismatches. The header does no memory I/O. The
caller memsets the image to zero first, as `gfx_v11_0_kiq_init_queue()` does.
`test_gfx11_mqd` checks hand-derived dwords, that the shared values match
the register plan, and refusal propagation. A mutant changing
MISC_RESERVED fails it.

Not done: the KCQ MQDs (they carry CU masks and priorities), the MAP_QUEUES
packets that the KIQ submits, and the GPU memory the image lives in (G16 is
SDMA-only).

**Correction (same day, found after G18 and G19):** upstream Linux does not
use this legacy KIQ path on Navi33. `amdgpu_discovery.c` (torvalds/linux
master, fetched 2026-10-18) sets `enable_mes` and `enable_mes_kiq` to true
for GC 11.0.0 through 11.0.4 and 11.5/11.7, which includes 11.0.2. Compute
queues are therefore scheduled by the MES firmware (`mes_v11_0.c`). The
legacy KIQ MAP_QUEUES path in `amdgpu_gfx_enable_kcq()` runs only when
`mes.enable_legacy_queue_map` is set, and that is set only when the MES
firmware version is at least 0x47. G18 and G19 are therefore the fallback
path, not the upstream path for this card. The next steps are the MES bring-up
sequence: `mes_v11_0_enable()`, KIQ-pipe queue init (`mes_v11_0_queue_init()`),
and then the MES API commands (`set_hw_resources`, `set_hw_resources_1`,
`query_sched_status`). They run through a ring in GPU memory and need fences,
so they are blocked on the same GPU memory and VM work as above.

## Stage G20: offline MES bring-up plan (the upstream compute path)

`Navi33GfxMes.h` plans the MES steps that upstream runs for Navi33 (see the
correction above). The sources are `mes_v11_0.c` (`mes_v11_0_enable()`,
`mes_v11_0_queue_init_register()`, `mes_v11_0_mqd_init()`), the
`kiq_map_queues` path for the MES ring, `soc15d.h` for PACKET3, and
`soc15_common.h`/`amdgpu.h` for `REG_SET_FIELD`, all from torvalds/linux
master, fetched 2026-10-18.

- Enable: the reset word, the per-pipe program counter (`uc_start >> 2`, under
  GRBM me 3 and that pipe), then the unhalt/activate word built from zero.
  Disable is a read-modify-write that halts the pipes.
- KIQ-pipe queue registers, in upstream order. The MES queue differs from the
  legacy KIQ path in four ways: NO_UPDATE_RPTR set, `CP_MQD_CONTROL` written
  as literal 0, the wptr-poll address masked to 8 bytes, and `CP_HQD_ACTIVE`
  always 1.
- **Quirk, mirrored on purpose:** `mes_v11_0_mqd_init()` passes
  `(order(4096/4)-1) << 8` to `REG_SET_FIELD` for RPTR_BLOCK_SIZE. That macro
  shifts the value again, so the field is written as 0. The legacy path writes
  9. The planner matches upstream, and a test pins the difference.
- MAP_QUEUES for the MES scheduler ring: ME 2, engine 5, one queue.

Firmware facts checked against the real files on this machine (decompressed
from `/lib/firmware/amdgpu`, linux-firmware 20260915; SHA-256 prefixes
`719f4468…` for `gc_11_0_2_mes_2.bin`, `5f8f0b50…` for `gc_11_0_2_mes1.bin`):
the common header is 32 bytes, then `mes_firmware_header_v1_0`. Both images
have `mes_uc_start_addr_lo = 0xf0005000` and `_hi = 0xffffffff`, read at
offsets 56 and 60. The start address is therefore `0xffffffff_f0005000`. Its
word address written to `CP_MES_PRGRM_CNTR_START` is `0xfc001400` (with
`_HI` = `0x3fffffff`). The planner test uses these values.

G17 still applies on the MES path. `gfx_v11_0_cp_resume()` calls
`gfx_v11_0_kcq_resume()` regardless of MES, and that enables the MEC
(`gfx_v11_0_cp_compute_enable(true)`). The KCQ registers are then programmed
by MES firmware, not by the driver.

Not done: the MES MQD memory image, the MES API ring (`set_hw_resources`,
`set_hw_resources_1`, `query_sched_status`, `add_queue`), and the fences for
them. The API ring lives in GPU memory, so it is blocked on the same GPU VM
work as before.

## Stage G21: kernel PCI owner probe (kext 0.0.15, code only, not run)

The first piece of the "production PCI/queue owner" that the SDMA Queue1
proof needs. Approved 2026-10-18 for CI compile only. Nothing was run on the
card, and no build went onto the stick.

- `src/Navi33PciOwner.h` is a pure decision: Evidence in, Decision out. An
  Owned decision grants no access (`kCanWriteGpu` and `kCanWriteConfig` are
  false, with a static_assert). It is tested by `test_pci_owner`: every
  refusal bit, the kill switch winning over everything, an empty-evidence
  refusal, and the closed gate being Disabled, not Refused.
- `src/Navi33DarwinPciOwner.cpp` gathers the evidence: the identity from config
  reads, one `IOService::open(self)` followed by `close` (an observation that
  nobody else held the device at that moment), and a one-level child scan for
  `IOFramebuffer` or `IOAccelerator` (fail closed if the scan cannot run). It
  logs the decision with IOLog.
- `Navi33Probe::start()` calls it only with `navi33-owner-probe=1`. The kill
  switch is `navi33-owner-off`, and `navi33-owner-recovery=1` is the operator's
  recovery attestation. None of these is on the stick.
- `tests/test_manifest.py` forbids `configWrite`, `setBusMasterEnable`,
  `setMemoryEnable`, `mapDeviceMemory`, `RREG32`/`WREG32` and `writeMmio` in
  the owner file.
- Checks run: host tests on g++ and clang++ with ASan/UBSan, the manifest, the
  bootstrap tests (kext version 0.0.15), and a kernel `-fsyntax-only` of
  `Navi33DarwinPciOwner.cpp` and `Navi33Probe.cpp` against the RDNA4FB
  MacKernelSDK headers, which pass.

Not done, and not claimed: the real open/close result, whether the child scan
finds the display stack on this machine, and the display-client detection
itself. Those are only observable on the card. The IOKit calls are checked by
the compiler, not by a run. Recovery stays the operator's attestation.

## Stage G22: production PCI/queue owner state machine (software only)

`Navi33ProductionOwner.h` is the software side of the owner that the SDMA
Queue1 proof needs. It has three phases. **Unowned** means acquire() may
succeed. **Owned** means one owner in one epoch, with SDMA0 Queue1 claimable
once. **Poisoned** means a hardware touch may have happened, so release() is
refused until an independent reset attests a power cycle, PCI function reset
and discarded firmware state. The next acquire() starts a new epoch, and every
older ticket goes stale.

- acquire() refuses unless the kext decision is Owned, the providerId and
  dmaDomainId are nonzero, the identity is 1002:7480 rev cf, and an independent
  reset path is attested. Each refusal sets its own bit.
- toDeviceOwner() fills the memory contract's `DeviceOwner` from a live owner
  only, so the contract's ownership check can use it.
- `test_production_owner` covers the happy path, each refusal, the poison rule,
  the reset rule, stale tickets across epochs, the exclusive Queue1 claim, and
  the device-owner handoff. A mutant that lets a poisoned owner release fails it.
- kCanWriteGpu, kCanWriteConfig and kCanClaimHardware are false, with
  static_asserts.

**Not evidence.** The Attestation is built by hand in the host tests, so those
tests prove the logic, not the card. The production owner is still not a
hardware owner. The SDMA Queue1 proof stays blocked until a live run shows it.

**Still missing for production (kext side, needs CI and then an on-card run):**
the kext must hold its `IOService::open` for the whole epoch instead of closing
it at once (today's 0.0.15 probe closes straight away), and it must feed the
state machine from real IOKit results. Until then this stays a state machine
without a live owner.

## Stage G23: kext holds the PCI owner for an epoch (kext 0.0.16, CI only)

The kext side of G22. With `navi33-owner-hold=1` (default off, never on the
stick), `holdOwner()` keeps `IOService::open` for one production-owner epoch.
It does this only when the decision is Owned, and any other outcome closes the
device before returning. `stop()` releases the owner and closes the device. A
poisoned owner is kept, so the open stays until an independent reset.

- Identity: `providerId` is the card's registry entry ID. `dmaDomainId` is the
  probe's own registry entry ID, not yet an `IODMACommand` domain, so that link
  is still open.
- Properties: `Navi33,OwnerHeld`, `Navi33,OwnerEpoch` and `Navi33,OwnerPhase`.
- No GPU or config write was added. The change is the PCI open and its close.
- Checks run: host suites on g++ and clang++ with sanitizers, the bootstrap
  tests, the manifest (which now requires the hold gate and `releaseOwner`), and
  a kernel `-fsyntax-only` of `Navi33DarwinPciOwner.cpp` and `Navi33Probe.cpp`
  against the RDNA4FB headers, which pass.

**Not run.** Holding the card open has not been run on the card. It blocks other
clients, including the display stack, while held. A run needs a fresh CI
artifact, a VM boot with the hold gate set, and a check that the guest's
probe and release log lines appear. It is on-card work, so it falls under the
standing card-run scope, and it must not touch the VBIOS.

**Kext 0.0.17 (release on shutdown, CI only so far).** The 0.0.16 VM run showed
that macOS shutdown does not reach `stop()`, so the held open was dropped only
when QEMU exited. 0.0.17 adds `systemWillShutdown()` and `willTerminate()`
overrides that call the same `releaseHeldOwner()` helper, which is idempotent.
Both are IOService notifications. `systemWillShutdown` reaches only power-plane
members, and this kext may not be one, so the VM run decides whether this works.
A new power-driver registration would be the next step if it does not.

**Result (0.0.17, VM run 2026-10-18 18:08):** the release line is still
absent after a clean guest shutdown, so neither override runs on this path.
The open is therefore held until the device goes away. The options are: register
as a power driver so `systemWillShutdown` is called (this changes power
management on the display card, so it needs a plan and approval), or accept
that the open lasts until the machine powers off, which is harmless in practice
but not clean.

**Decision (2026-10-18): option 2 is accepted.** The held open lasts until
power-off. Registering as a power driver is not being done for now. A restart
has not been tested, so the hold must not be relied on across a reboot until it
is.

## Stage G24: real DMA domain for the owner (kext 0.0.18, CI only so far)

The owner's `dmaDomainId` was the probe's own registry ID, which is not an
IODMACommand domain. 0.0.18 takes the domain from
`IOMapper::copyMapperForDevice()` for the card: the mapper's registry entry ID,
the same mapper the staging code already uses (`Navi33DarwinStage.cpp`).

- `pci_owner::Evidence.dmaMapperPresent` and the `NoDmaMapper` refusal:
  without a mapper the probe is Refused, because there is no DMA domain to own.
- `production_owner::domainFromMapper()`: zero when no mapper exists, and the
  owner refuses a zero domain (`MissingProvider`).
- The mapper is shared by every device behind the same IOMMU unit, so the
  domain identifies that unit, not the card alone. The memory contract needs the
  staging memory mapped through this domain, which is why it is the right id.
- Checks run: host suites on g++ and clang++ with sanitizers, the bootstrap
  tests, and a mutation that removes the `NoDmaMapper` refusal (the test fails).
  The kernel `-fsyntax-only` passes for the owner file and `Navi33Probe.cpp`.

**Expected in the VM:** the 14:47 VM run found no device mapper for this PCI
device (`NoDeviceMapper`). So the probe should now refuse with `NoDmaMapper`, not
report Owned. That is the correct fail-closed result, not a regression. Not run yet.

**Physical check status (2026-10-18):** the VM run has no mapper (`NoDmaMapper`).
The PC's host IOMMU is AMD-Vi (`ivhd0`, with active Linux IOMMU groups), not
Intel VT-d. Apple's macOS IOMapper support is for VT-d, so macOS on this board
probably has no mapper either. That is not verified. A bare-metal macOS run from
the stick is the only way to check it. The stick was not plugged in, and it
needs a physical reboot, so the check is pending. If the bare-metal run also
refuses with `NoDmaMapper`, the production owner's mapper gate cannot be met on
this hardware, and the SDMA Queue1 proof is blocked for this board.

## Kext 0.0.19 and the one-boot evidence run

The 0.0.18 stick run lost the owner decision. The kext's IOLog lines reach only
`dmesg`, the stick had no `msgbuf=` boot-arg, and the small default buffer had
rolled past the boot lines before `run.sh` ran. The VM disks set
`msgbuf=1048576` for this reason.

- 0.0.19 publishes the owner verdict as ioreg properties: `Navi33,OwnerProbe`
  (text), `OwnerDecision`, `OwnerRefusalMask`, `OwnerEvidence` (one bit per
  input, `pci_owner::evidenceBits`, host-tested) and `OwnerDmaDomain`.
- `run.sh` prints the owner and DMA-preflight properties, the IOMapper and
  AppleVTD instance counts, the ACPI table names (IVRS or DMAR), the
  framebuffer and accelerator instances, `system_profiler SPDisplaysDataType`,
  and 200 lines of kernel log. The manifest test keeps `run.sh` free of every
  write command.
- Boot-args for the one-boot evidence run on the stick: add `msgbuf=1048576`,
  swap `navi33-dma-off=1` for `navi33-dma-preflight=1` (the read-only mapper
  check, no GPU DMA), and keep `navi33-owner-probe=1` and every PSP, SMU, SDMA,
  FB and DMA-stage kill switch.

## Stage G25: offline MES KIQ-pipe MQD image

`Navi33GfxMesMqd.h` builds the 2 KiB MQD that `mes_v11_0_mqd_init()` writes for
the MES KIQ pipe. Values shared with the G20 register writes are copied from
the G20 plan. The MQD-only fields are computed from upstream: EOP base and
size (`MES_EOP_SIZE` 2048), `CP_MQD_CONTROL` default with VMID 0, IB control,
IQ timer and quantum defaults. Field indices are the v11_compute_mqd ones
checked for G19, plus `cp_hqd_iq_timer` at dword 150.

Differences from the legacy KIQ image (G19) are pinned by the test: quantum
0 (legacy `0x111`), the MES pq-control word `0xd8308009` (NO_UPDATE_RPTR,
RPTR_BLOCK_SIZE 0), and `CP_MQD_CONTROL` `0x100` in memory although the MES
register write uses a literal 0. A mutant giving MES the legacy quantum fails
`test_gfx11_mes_mqd`. No I/O; not hardware evidence.

## Stage G26: host-verified DMA buffer backend (no hardware)

Branch: feature/navi33-dma-backend. Software only; nothing runs on the card.

- `src/Navi33DmaBackend.h` connects page lists to the existing pieces: it
  attaches pinned bus pages, installs GMC11 system PTEs into a shadow
  aperture (`gmc11::mapSystemPages`), turns a buffer copy into an SDMA6
  COPY packet with GPU VAs (`sdma6::makeCopy`), and publishes it through
  the software `sdma6::Ring`, recording the fence on both buffers.
- Address validation: alignment, the 48-bit PTE range, zero pages,
  duplicates within a buffer and aliasing with any live or quarantined
  buffer. Refused operations leave no slot behind.
- Ownership: handles carry the owner epoch and a slot generation, so stale
  handles from another epoch or a released slot are refused.
- Synchronization: a buffer is busy while its last fence is above the
  completion horizon. Unmap and release are refused while busy.
- Faults: `quarantine()` freezes a buffer. Its pages and PTEs stay reserved,
  and it can never be unmapped, released or aliased again.
- Unmapped DMA is a separate mode, `Translation::PhysicalUnverified`. The
  absence of an IOMMU does not by itself make the GPU unable to reach
  CPU-physical pages. Reachability is unproven, though, so this mode is
  refused unless `allowPhysicalUnverified` is set, and GART installation
  needs `gpuGateOpen`. Production code sets neither.
- `tests/dma_mock_gpu.h` is a mock SDMA engine. It translates every byte
  through the shadow GART table and faults before consuming the packet. The
  mock models transactions only; it is not silicon evidence.
- `tests/test_dma_backend.cpp` passes under GCC and Clang with
  `-Werror -pedantic`, under ASan/UBSan, and in `make host-test`. Mutants
  that drop the in-flight check or the destination fence record abort.
- Still blocked for hardware, unchanged from G12-G25: a production
  PCI/IOMMU owner (on this board macOS has no IOMapper), real VRAM root,
  independent reset, and the Queue1 owner gate. The backend is not wired to
  any kext selector, and `kHardwareSubmissionEnabled` is `false`.

## Stage G27: host-verified SDMA6 copy engine (no hardware run)

- `src/Navi33DmaEngine.h` joins the G26 backend, the software ring and the
  SDMA6 register path into one engine: `start`, `attach/map/unmap/release`,
  `copy`, `ringDoorbell` and `pollCompletion`.
- Register writes need three independent gates: the build defines
  `COBALT_NAVI33_ENABLE_UNSAFE_MMIO`, `Config::hardwareSubmit` is set and a
  port is supplied. Production defines none of them. The engine never sets
  RB_CNTL or RB_BASE; the queue must already be configured and enabled by an
  owner (`sdma6_mmio::Queue`).
- The write pointer is ring-relative bytes, written high word then low
  (WPTR_HI=0, WPTR), the same order as `sdma6_mmio::Queue::submit()`. Upstream
  `sdma_v6_0.c` uses `wptr << 2` for the same field; the doorbell is not used.
- Completion comes only from the read pointer. Retire is modular, never claims
  work beyond what was published (`RetireMismatch`), and stops at whole items.
  An all-ones read is `ReadFailed`, never "done".
- Ring change (`Navi33Sdma6Ring.h`): SDMA rejects a packet that crosses the
  ring end, and the old ring wrapped silently. A batch now pads the gap with
  zero dwords (SDMA NOPs), and one dword stays free, because equal pointers
  cannot tell full from empty. `test_sdma6_ring` was updated for the new
  positions and padding.
- Host tests: `test_dma_engine` (default build, write path compiled out) and
  `test_dma_engine-unsafe` (same source, `COBALT_NAVI33_ENABLE_UNSAFE_MMIO`,
  against a simulated SDMA6 block). The unsafe build runs 120 copies through
  a 256-dword ring: wraps, padding, retire, fences 1..120. Mutants that remove
  the runtime gate or the padding abort.
- Still not verified against silicon: the RPTR/WPTR semantics on wrap, the
  SDMA NOP encoding on the card, and the hardware fence path (completion here
  is read-pointer based, not a fence write). None of this runs until the owner
  and VM gates in G21-G25 are met.

## Stage G28: unmapped DMA owner mode (kext 0.0.20, CI build only)

- The owner used to refuse every card with no IOMapper (`NoDmaMapper`). This
  board has no mapper, so every run stopped there. The owner can now accept
  the card through an explicit opt-in: `navi33-owner-physical=1`.
- The decision names its mode (`pci_owner::DmaMode`). `Mapper` needs an IOMMU
  domain. `PhysicalUnverified` needs the sentinel domain
  `production_owner::kUnmappedDomainId` and representable CPU-physical pages
  (`darwin_dma::physicalCandidateReady`, a 64 KiB allocate/release, no mapping).
- A mapper always wins over the opt-in. Kill switch, open, display, recovery
  and identity gates still apply to the physical path.
- The owner refuses a mode/domain mismatch (`DomainModeMismatch`), including a
  mapped owner that claims the sentinel.
- Host-verified: `test_pci_owner` and `test_production_owner` (new cases for
  every refusal above). Kext code is built and checked only in CI.
- NOT established: that the GPU can DMA to those physical pages. The owner
  says only that the decision and the pages are consistent. Any write stage
  still needs its own approval.

## Stage G29: Queue1 preflight windows (kext 0.0.21, read-only)

- The G15 manifest programs only the VMID1 context, the invalidation and the
  Queue1 ring registers. Linux does more before an SDMA ring runs. Each hub's
  `gart_enable` (`mmhub_v3_0_2.c`, `gfxhub_v3_0.c`) sets the system aperture,
  `MX_L1_TLB_CNTL` (ENABLE_L1_TLB, SYSTEM_ACCESS_MODE 3, advanced driver
  model), `VM_L2_CNTL`..`CNTL5` (ENABLE_L2_CACHE) and the identity aperture.
  `sdma_v6_0.c` sets UTCL1, the watchdog and clears `SDMA0_F32_CNTL.HALT`.
- After the 2026-10-18 SCPM sequence both hubs read `VM_L2_CNTL` =
  `0x00080602`: ENABLE_L2_CACHE (bit 0) is clear on MMHUB and GFXHUB, and every
  VM context is `0x01fffe00` (disabled). So the hubs have never been set up.
  The L1 TLB and F32 halt state were not readable before this stage.
- Kext 0.0.21 appends 20 `n33peek` windows (every dword checked against the
  upstream offset headers, no holes, the SEM/ACK/INV/UCODE/ERROR_LOG hazards
  excluded and asserted in `test_mmio_peek`): MMHUB and GFXHUB L2
  CNTL2..CNTL5, fault control/address, identity aperture, system aperture
  default and local ranges, AGP, `MX_L1_TLB_CNTL`; SDMA0 CNTL, watchdog, UTCL1,
  STATUS3 and `F32_CNTL` (GC BASE_IDX 1). No N33N ABI change: windows are
  appended, so earlier indices keep their meaning.
- `tools/queue1-census.py RESULTS` decodes those windows offline and lists the
  bits Linux sets that read clear. GC values count only after the RLC bootload
  has completed (hard rule 4). Tests: `bootstrap/tests/test_queue1_census.py`.
- Next: after the sOS -> SMU sequence, read the full set, then design the run
  mode from what is actually missing. Whether only GFXHUB (SDMA's hub) needs
  VMID1, or MMHUB too, is to be decided from upstream and the census; MMHUB
  also serves the display.
