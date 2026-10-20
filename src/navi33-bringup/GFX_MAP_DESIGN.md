# G42: map one gfx11 graphics queue through G34

The user requested continuing graphics queue mapping/activation on 2026-10-20
("please go"). G42 ran with kext 0.0.57 and 0.0.58; both timed out before the
graphics queue mapped. After the user reported a complete reboot and approved
one attempt, kext 0.0.60 ran in `runs/20261020-191448`. It refused at the HDP
preflight because the remap registers contained unexpected values; no G42
stage-window write or MAP_QUEUES followed. Subsequently 0.0.61 reached the
flush readback and 0.0.62 passed HDP but timed out mapping (details below).
At this handoff the VM may still be running on 0.0.62; it was not contacted.
Any further GPU write attempt needs another full host power-off and renewed
explicit approval. This work does not enable Metal publication.

The 0.0.56 run `20261020-155810` has live PFP/ME firmware, a programmed RB0,
and GFX_HQD_ACTIVE=1, but MAPPED=0 and no WPTR/RPTR movement through either
kick method. Upstream's async graphics path builds a v11_gfx_mqd and sends
MAP_QUEUES through a privileged queue before submitting the clear-state
preamble. G42 tests that missing mapping step, with the original, proven G34
VMID0 compute queue as the transport. It does not start or reconfigure MES.

Sources fetched 2026-10-20: torvalds/linux `gfx_v11_0.c`
(`gfx_v11_0_kiq_setting`, `gfx11_kiq_map_queues`, `gfx_v11_0_gfx_mqd_init`),
`include/v11_structs.h`, `nvd.h`, `gc_11_0_0_offset.h` and `gc_11_0_0_sh_mask.h`.
The existing RLC_CP_SCHEDULERS read was 0x3038 at guest BAR5+0x2a628.
G42's QMP read-only evidence showed its value change to 0x30a0, G34 memory
WPTR reach 32, but the KIQ fence remained zero, graphics MAPPED remained zero,
and the GFX HQD remained inactive. `Navi33,ClientGfx` ended in state 5
(Unhalt). The graphics ring was never kicked, so this says nothing about
whether a graphics ring submission or rendered output works.

The 0.0.58 candidate moved the G34 scheduler HIQ write before HQD activation.
The live run confirmed `RLC_CP_SCHEDULERS` changed `0x3038->0x30a0`. The G34
snapshot began with RPTR/WPTR=16; after the staged WPTR advanced to 32, the
reported final RPTR readback was 0 rather than 32. The new map fence and
graphics MAPPED bit remained zero. Thus the ordering change did not resolve
the failure, and the cause remains unknown. The captured
16-dword MAP_QUEUES plus WRITE_DATA fence matches upstream packet encoding.
The post-stage BAR5-only `n33peek` capture is
`runs/20261020-172609/gfxmap-post-n33peek.txt`; it reports no GPU writes and
does not include the G34 HQD block.

Read-only source comparison, corrected 2026-10-20 17:42 HKT: the direct GFX
path in `amdgpu_gfx_enable_kgq()` calls `amdgpu_device_flush_hdp()` before it
commits MAP_QUEUES. G42 stages the ring, WPTR and fence through BAR0 using
`sfence; mfence`, but has no HDP flush; this is the strongest current
source-backed follow-up lead, not a proven cause.

Resolved against this card's captured IP Discovery (`NBIF 4.3.1`, `HDP 6.0.1`)
and upstream Linux on 2026-10-20: `amdgpu_discovery.c` selects NBIO v4.3 and HDP
v6.0 for these versions; HDP v6.0 uses `amdgpu_hdp_generic_flush()`. For the
non-SR-IOV 4 KiB-page mapping, `nbio_v4_3_set_reg_remap()` chooses
`MMIO_REG_HOLE_OFFSET = 0x80000 - PAGE_SIZE`, or `0x7f000` at 4 KiB. It maps
the memory-flush control at that BAR5 aperture offset. The v4.3 register header
defines `REMAP_HDP_MEM_FLUSH_CNTL` and `REMAP_HDP_REG_FLUSH_CNTL` at dwords
`0x12d` and `0x12e`, BASE_IDX 2. This card's NBIF BASE_IDX 2 is dword `0xd20`,
so the corresponding BAR5 byte offsets are `0x3934` and `0x3938`. Upstream
programs those remap registers with `remapOffset + 0` and `remapOffset + 4`.
The generic flush then writes zero to BAR5 + `0x7f000` and reads NBIO memory
size (`NBIO_MEMSIZE`, dword `0xc3`) as its ordering/readback operation.

Before 0.0.59, G42 performed none of the remap setup or flush. The bounded
candidate therefore permits at most three exact GPU writes
beyond the existing G42 writes: if both remap registers are reset to zero, set
them to the selected aperture offset and offset+4; if already set to those
exact values, leave them alone; refuse any other values. Then write zero to the
remapped HDP memory-flush register after the G34 MQD/ring/fence/WPTR have been
staged and before the poll trigger. Read the two remap registers back after
setup and read NBIO memory size after the flush. Compute the remap offset from
the guest kernel page size using the upstream `0x80000 - PAGE_SIZE` rule, and
refuse if the resulting aperture is outside BAR5. The G42 one-shot latch must
move before the first possible remap write, and a failed setup or readback must
stop before MAP_QUEUES. This adds no packet types or VRAM span, but it is a new
MMIO write set.

## 0.0.59 bounded implementation

The user asked to continue the G42 investigation. Kext 0.0.59 implements only
the candidate above. The Darwin path passes the current XNU `PAGE_SIZE` into
the host-independent resolver; it refuses unless that is 4096 bytes and the
captured NBIF/HDP versions are exactly 4.3.1 / 6.0.1. The remap writes accept
only `0x7f000` and `0x7f004`; the aperture accepts only a zero flush write.
Unexpected old remap values refuse before the latch. The latch is consumed
before the first possible remap, MQD, or staging-window write. After staging,
the code writes the flush, then requires the two-read NBIO capacity check to
pass before it triggers G34. Every failure stops before MAP_QUEUES. Ioreg
`Navi33,GfxMap` now reports the HDP plan, state, old/new remap values, number
of successful HDP writes, aperture, and capacity readback.

Host coverage verifies the RX 7600 offsets, exact values, reset and already-set
remaps, unexpected remap refusal, partial remap write failure, remap readback
failure, flush write/readback failure, strict operation order, and that all
failure cases stop before the poll trigger. CI run `39406581881` passed all
jobs, including both macOS Intel kext builds and GCC/Clang/ASan/UBSan tests.
The macos-26-intel artifact is cached at
`~/macos tahoe/navi33/cache/artifact-0.0.59-ci` (Mach-O UUID
`21868249-AAFB-3ACA-A72F-9E20F0557E70`). At the time of that artifact handoff,
this was only build and host evidence; no 0.0.59 kext had run on the card.

## 0.0.60 read-only post-map capture

The follow-up captures the GFX ME0/pipe0/queue0 HQD after the MAP_QUEUES
poll: ACTIVE, VMID, BASE/BASE_HI, RPTR, CNTL and WPTR/WPTR_HI. These BAR5
offsets come from upstream Linux `gc_11_0_0_offset.h`. `Navi33,GfxMap` reports
the raw values and a validity bit. A failed read marks the snapshot invalid but
does not change the G42 result or cause another write. None of these registers
was added to either write allowlist. Host tests check the resolved offsets,
reject them as write targets, and show that a failed diagnostic read leaves a
MAP timeout intact. The values are raw evidence; zero is not interpreted as
idle if GC is not answering. CI run `39408266841` passed GCC, Clang,
ASan/UBSan and both macOS Intel kext builds. Its macos-26-intel artifact is
cached at `~/macos tahoe/navi33/cache/artifact-0.0.60-ci`, plist version
0.0.60, Mach-O UUID `13B8975F-2F6C-399A-8C9C-27CC7E938E53`.

## 0.0.60 physical preflight result

The user reported a full reboot, confirmed by Linux boot ID changing from
`10ce1af7-f9c2-444e-9388-ca13750bb4c4` to
`b0d22cb8-c1f8-470e-b27c-285ebbaee125`, with the RX 7600 bound to `vfio-pci`.
The loaded kext UUID matches the CI artifact above. Autostart reached 8/8. The
first VMID 8 client open started G40 and reached G42 preflight. `Navi33,GfxMap`
reports Refused / `UnexpectedRemap`, aperture `0x7f000`, remap values
`0x385c`/`0x3858`, and zero HDP writes. These values matched neither the reset
pair nor the configured pair, so G42 stopped before its stage-window write and
MAP_QUEUES. The reads were at BAR5 offsets `0x3934` and `0x3938`, resolved as
NBIF BASE_IDX 2 (`0xd20`) plus dword offsets `0x12d` and `0x12e`. The HDP
flush and capacity readback were not reached; reported memsize 0 is therefore
unpopulated. G40 pipe setup had already run; `Navi33,ClientGfx` ended in state
5 with RB0 WPTR/RPTR/fence zero, and the GFX ring test did not run. A VMID 8
compute NOP retired its fence. No gradient, GFX map, rendered output, or Metal
output was attempted. Captures are in
`~/macos tahoe/navi33/runs/20261020-191448/`: `results-20261020-191940-guest.txt`,
`g42-after-client.txt`, `g42-client-trigger.txt`, and `n33peek-post-g42.txt`.
The VM remains running on 0.0.60; any further GPU write attempt needs another
full host power-off and renewed explicit approval.

This result does not establish what owns or programs the unexpected remap
values. Offline check (2026-10-20, upstream `torvalds/linux` headers fetched
from raw.githubusercontent.com; git.kernel.org returned 502):
- The register names are confirmed: dword `0x12d` is
  `regBIF_BX0_REMAP_HDP_MEM_FLUSH_CNTL` and `0x12e` is
  `regBIF_BX0_REMAP_HDP_REG_FLUSH_CNTL`, both BASE_IDX 2 in
  `nbio/nbio_4_3_0_offset.h`. The `kHdpMemFlushReg`/`kHdpRegFlushReg`
  constants are correct for that header.
- Their only field, `ADDRESS` (`nbio_4_3_0_sh_mask.h`), is bits 2..18
  (`0x7FFFC`). The live `0x385c`/`0x3858` are therefore byte addresses
  inside BAR5, not a status bit pattern. Upstream's accepted pair `0x7f000`
  and `0x7f004` is also a byte address in the same field.
- `0x385c` (dword `0xe17`) and `0x3858` (dword `0xe16`) do not match any
  register in `nbio_4_3_0_offset.h` or `hdp_6_0_0_offset.h`. The HDP header
  names `0x12d`/`0x12e` differently (`HDP_XDP_HDP_MBX_MC_CFG` and
  `HDP_XDP_HDP_MC_CFG`) and is not the remap block, so those names are not
  the source of the value.
- Upstream writer, `nbio_v4_3_remap_hdp_registers()` in `amdgpu/nbio_v4_3.c`
  (fetched 2026-10-20): it writes `rmmio_remap.reg_offset + KFD_MMIO_REMAP_*`
  to the two registers, with `reg_offset = MMIO_REG_HOLE_OFFSET = 0x80000 -
  PAGE_SIZE`. `KFD_MMIO_REMAP_HDP_MEM_FLUSH_CNTL = 0` and
  `KFD_MMIO_REMAP_HDP_REG_FLUSH_CNTL = 4` (`include/uapi/linux/kfd_ioctl.h`).
  So upstream's pair is `0x7f000`/`0x7f004`, which matches our constants.
- Upstream's writer does not read the registers first. It writes the pair
  unconditionally. Our G42 gate instead refuses any existing pair that is
  neither the reset value nor ours. That is stricter than upstream: the live
  `0x385c`/`0x3858` would be overwritten by the driver's normal init. Changing
  that rule is a new write-policy decision, so it needs its own bounded design
  and renewed approval; it is not made here.
- `nbio_v4_3.c` includes the `nbio_4_3_0` headers, so the repo's NBIF 4.3.1
  label does not change the register names used here. I did not verify which
  IP version maps to `nbio_v4_3` in discovery, and the `nbio_4_3_1` header path
  returned 404 on the mirror.
- The call site is `soc21_common_hw_init()` in `amdgpu/soc21.c` (the SoC
  common IP block, which runs before GFX init), gated only on
  `!sriov`. Upstream therefore writes the pair on every driver init. A power-on
  value that no driver has written should be visible on a boot where amdgpu
  and our kext have not touched the card. A read on the 2026-10-20 19:38 boot
  (`d04f59d2-…`, card on vfio-pci, no VM) is the next check; the script is
  `$HOME/cobalt-scratch/remap-peek.py` (read-only, BAR5 PROT_READ). Until its
  Result, boot `d04f59d2-…` (fresh boot, card on vfio-pci, no amdgpu, no VM):
  REMAP_HDP_MEM_FLUSH_CNTL `0x385c`, REMAP_HDP_REG_FLUSH_CNTL `0x3858`, NBIO
  MEMSIZE `0x1ff0` (8176 MiB), C2PMSG_35 `0x80000000`, C2PMSG_81 0. So the pair
  is present before any driver here touched it. This is most likely the
  power-on or VBIOS value, not verified: a VBIOS POST write cannot be ruled out
  from a read. Upstream's init overwrites it, and our gate refuses it. Do not
  accept, overwrite, or clear these values without a new bounded design and
  renewed approval.

`SET_RESOURCES` belongs to the separate `amdgpu_gfx_enable_kcq()` compute-queue
path; `amdgpu_gfx_enable_kgq()` does not emit it. Keep it as an indirect KIQ
setup question only, not a confirmed missing step in the GFX map path. The host
fake models neither HDP visibility nor these KIQ hardware effects. Do not add
operations or enlarge a stage-write span without a new bounded design and
renewed approval. Sources: [amdgpu_gfx.c](https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/amd/amdgpu/amdgpu_gfx.c), [amdgpu_discovery.c](https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/amd/amdgpu/amdgpu_discovery.c), [nbio_v4_3.c](https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/amd/amdgpu/nbio_v4_3.c), [nbio_4_3_0_offset.h](https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/amd/include/asic_reg/nbio/nbio_4_3_0_offset.h), [amdgpu_hdp.c](https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/amd/amdgpu/amdgpu_hdp.c), [hdp_v6_0.c](https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/amd/amdgpu/hdp_v6_0.c), [kfd_ioctl.h](https://github.com/torvalds/linux/blob/master/include/uapi/linux/kfd_ioctl.h).

## Gate and plan

`navi33-gfx-map=1` plus the client GPU gate, no `navi33-gfx-map-off`,
`navi33-mec-off` or `navi33-psp-off`. VM builder option `--gfx-map` requires
`--client-gpu`. Off by default. The service starts once; a separate stage
latch is consumed before its first staging-window write. No retry after a
partial attempt, no reuse of G34's appended span, no alternate packet stream.

Before a stage write, read and require the original G34 ME1/pipe0/queue0:
ACTIVE=1, VMID=0, ring base exactly FB+250MiB+0x2000, PRIV_STATE and KMD_QUEUE
set, RPTR=16, WPTR memory=16/0, original fence 0x0c0b1601; scheduler HIQ bit
clear before gated G34 initialization, scheduler low byte 0xa0 after it, and
graphics MAPPED=0. GRBM selects return to zero. The usual live
console/BAR0/VRAM proof and heap reservations precede this plan.

## Exact additional writes

- RLC_CP_SCHEDULERS (GC segment1 dword0x98a), preserving bits31:8 and setting
  the low byte to 0xa0 (ME1/pipe0/queue0, HIQ), now written during gated G34
  initialization before HQD activation. It is the only added MMIO register.
  Existing CP_PQ_WPTR_POLL_CNTL1 bit0 triggers G34; existing G40 CP_RB_ACTIVE
  stays zero until the firmware maps the graphics queue.
- Inside the existing G34 window at VRAM250MiB: exactly 16 dwords at +0x2040
  (MAP_QUEUES, confirmed WRITE_DATA fence, NOPs), 2 dwords at +0x3008 (WPTR32/0), and 1 dword
  at +0x4008 (clear a new fence). Total76 bytes. Original ring-test bytes and
  fence at +0x4000 remain intact. Separate callbacks enforce these exact spans;
  they cannot access the rest of the pinned window. All CPU writes read back.
- A 512-dword v11_gfx_mqd at +0x4200 inside the live 20KiB GFXR heap allocation.
  MQD addresses name only that allocation's ring, RPTR and WPTR; VMID0, normal
  gfx queue, ME0/pipe0/queue0, engine4, doorbell0x116. The command is fixed in
  kernel code and cannot be supplied by a user client. It permits firmware
  to program that one graphics HQD, not arbitrary register writes.

Wait for G34's new fence 0x0c0b1642 and graphics MAPPED=1. Then kick the same
G40 preamble/test ring and require its fence. A timeout leaves both allocations
reserved and the graphics path unavailable; compute remains available. No
legacy MMIO fallback follows a G42 attempt. Ioreg `Navi33,GfxMap` carries the
plan inputs, stage state, fence, MAPPED and final G34 RPTR. These verdicts survive
missing boot logs. A later attempt after any stage write needs a full power-off.

## Verification

The host fake requires a mapped gfx queue for G42. It checks the fixed packet
header/engine selection, privileged MQD, G34 pointer/fence updates and resulting
clear-state/fence execution. Tests cover default-off, exact window bounds,
refusal of a consumed G34 ring before stage writes, HDP ordering and failure
stops, timeout, and idempotent start. CI builds the real kext and matched tools.
Judge actual mapping, graphics fetch and rendered output only from the live run,
separately from host model results.

## 0.0.61 candidate: accept the power-on remap pair (historical design)

The fresh-boot read (`d04f59d2`) showed the remap pair `0x385c`/`0x3858` before
any driver touched the card, and upstream overwrites it on every init. The gate
now accepts exactly that pair as safe to overwrite, with the same write, readback
and flush sequence as a reset pair. Every other non-reset value still refuses
with `UnexpectedRemap`, and a near miss (`0x385c`/`0x3854`) is tested. Host
tests pass, and removing the power-on branch fails `test_client_gpu`. This
candidate subsequently ran as recorded below. Any further physical run needs
a full host power-off and renewed explicit approval.

## 0.0.61 run: why the flush readback failed (offline fix)

The 0.0.61 run (`runs/20261020-200423`) wrote the remap pair, then the flush
readback stopped at `FlushReadbackFailed`. The NBIO memsize register read
`0x1ff0` (8176 MiB, as in the guest's own dump), but `nbio4::inspect()` refused
it: it required a multiple of 256 MiB, and 8176 is not one. The reported
capacity is 16 MiB below 8 GiB, so the granularity is now 16 MiB and
`test_nbio43_capacity` checks that 8176 passes and 8200 still refuses. The
host suite passed before the 0.0.62 run below. Any new G42 attempt needs a
full host power-off and renewed approval. The register value establishes
8176 MiB reported capacity; attributing the 16 MiB difference to a particular
reservation remains an inference.

## 0.0.62 run: MAP_QUEUES still times out (offline analysis)

Run `runs/20261020-202558`: the remap pair and flush now pass (HDP Complete,
memsize 8176 MiB), but `GfxMap` is state 3 (MapTimeout): `mapped` stays 0, the
gfx HQD shows `valid 1 active 0`, and the G34 KIQ RPTR reads 0 after the run.
Completion of the packets appended at offset 64 was not established. The
earlier inference that they were never fetched is too strong: a final RPTR
alone cannot exclude a stalled command, reset/reload, or changed queue state.
Checked against
upstream `gfx_v11_0.c` (`gfx11_kiq_map_queues`, `gfx_v11_0_ring_emit_fence_kiq`,
fetched 2026-10-20): the 16-dword G42 packet matches on header 0xa2 with 5
body dwords, engine 4, one queue, ME 0 (the gfx ring is `me = 0`), doorbell
offset, MQD and WPTR addresses, and the WRITE_DATA fence (dst sel 5, confirm).
No encoding bug was found. The open question is why this KIQ stream does not
complete; the next check is the KIQ HQD and MQD fields
against upstream. The analysis below performed no further card run.

## 0.0.62 analysis: the KIQ is never kicked by doorbell (offline)

Upstream `gfx_v11_0.c`: the compute/KIQ rings use doorbells (`use_doorbell =
true`), the KIQ doorbell is index 0 (`AMDGPU_NAVI10_DOORBELL_KIQ = 0x000`,
`amdgpu_doorbell.h`), and `gfx_v11_0_cp_set_doorbell_range()` sets
`CP_MEC_DOORBELL_RANGE_LOWER = (kiq*2)<<2 = 0` and `UPPER = (userqueue_end*2)<<2`
(0x450). Our G34 KIQ is kicked only by the one-shot WPTR poll (`poll1`).
However, the initial interpretation of the saved range string and zero MQD
WPTR was incorrect. The audit below supersedes it: the range string is stale
after G40, and zero is upstream's initial MQD WPTR too. Doorbell submission
remains a candidate, not a demonstrated fix.

## 0.0.62 source and saved-evidence audit (2026-10-20, no hardware access)

The failure is localized to completion of G42's KIQ stream, before the GFX
ring test. **The root cause is not proven.** HDP's software checks passed;
the map fence did not land and MAPPED stayed zero. That does not establish
whether MEC failed to fetch, fetched stale bytes, or stalled processing MAP.
`GFXHQD valid 1` is the software snapshot-read success flag, not a hardware
queue-valid bit. ACTIVE=0 is expected before successful mapping because G40
deliberately writes RB_ACTIVE=0 in G42 mode.

The audit used repository revision `67017fd` (driver unchanged from 0.0.62)
and upstream Linux revision `f47d223627f80cb7a1eb14c585fe6cb35de5e984`,
fetched on 2026-10-20. The fetched `gfx_v11_0.c` is byte-identical to the
previous local upstream copy. Primary references:

- [gfx_v11_0.c](https://github.com/torvalds/linux/blob/f47d223627f80cb7a1eb14c585fe6cb35de5e984/drivers/gpu/drm/amd/amdgpu/gfx_v11_0.c):
  `gfx_v11_0_compute_mqd_init`, `gfx_v11_0_kiq_init_register`,
  `gfx_v11_0_kiq_init_queue`, `gfx_v11_0_cp_resume`,
  `gfx_v11_0_ring_set_wptr_compute`, and the map/fence emitters.
- [amdgpu_gfx.c](https://github.com/torvalds/linux/blob/f47d223627f80cb7a1eb14c585fe6cb35de5e984/drivers/gpu/drm/amd/amdgpu/amdgpu_gfx.c),
  [amdgpu_ring.c](https://github.com/torvalds/linux/blob/f47d223627f80cb7a1eb14c585fe6cb35de5e984/drivers/gpu/drm/amd/amdgpu/amdgpu_ring.c):
  KIQ allocation, MQD properties, KCQ/KGQ initialization.
- [soc21.c](https://github.com/torvalds/linux/blob/f47d223627f80cb7a1eb14c585fe6cb35de5e984/drivers/gpu/drm/amd/amdgpu/soc21.c),
  [amdgpu_doorbell.h](https://github.com/torvalds/linux/blob/f47d223627f80cb7a1eb14c585fe6cb35de5e984/drivers/gpu/drm/amd/amdgpu/amdgpu_doorbell.h),
  [amdgpu_doorbell_mgr.c](https://github.com/torvalds/linux/blob/f47d223627f80cb7a1eb14c585fe6cb35de5e984/drivers/gpu/drm/amd/amdgpu/amdgpu_doorbell_mgr.c):
  index assignment and 64-bit doorbell write.
- [amdgpu_amdkfd_gfx_v11.c](https://github.com/torvalds/linux/blob/f47d223627f80cb7a1eb14c585fe6cb35de5e984/drivers/gpu/drm/amd/amdgpu/amdgpu_amdkfd_gfx_v11.c):
  `hqd_load_v11`, the origin of G34's one-shot poll.
- [v11_structs.h](https://github.com/torvalds/linux/blob/f47d223627f80cb7a1eb14c585fe6cb35de5e984/drivers/gpu/drm/amd/include/v11_structs.h),
  [gc_11_0_0_offset.h](https://github.com/torvalds/linux/blob/f47d223627f80cb7a1eb14c585fe6cb35de5e984/drivers/gpu/drm/amd/include/asic_reg/gc/gc_11_0_0_offset.h),
  [gc_11_0_0_default.h](https://github.com/torvalds/linux/blob/f47d223627f80cb7a1eb14c585fe6cb35de5e984/drivers/gpu/drm/amd/include/asic_reg/gc/gc_11_0_0_default.h):
  descriptor layout, exact GC11 offsets and defaults. No new register
  constants or runtime writes were introduced by this audit.

### Doorbell lead: confirmed difference, incorrect range inference

Upstream `amdgpu_gfx_kiq_init_ring()` sets `use_doorbell=true`.
`soc21_init_doorbell_index()` selects NAVI10 KIQ index 0 and userqueue end
0x8a. The logical 64-bit index becomes a dword index by multiplying by two;
the range registers use byte offsets: `0*8=0` through `0x8a*8=0x450`.
`gfx_v11_0_ring_set_wptr_compute()` updates the memory WPTR and writes
`WDOORBELL64`; its non-doorbell path only warns. G34 uses no BAR2 kick,
and G42 updates memory WPTR to 32 then writes POLL_CNTL1 bit 0. This is a
real deviation from the upstream KIQ submission path.

But `Navi33,ClientGpu`'s `range 110..ffc->110..ffc` **does not measure the
range at G42**. In `Navi33ClientGpu.h`, `start()` fills `db_.lower/upper`
before `gfxStart()`. G40 then reads MEC upper and, if above 0x450, writes
lower=0 and upper=0x450 before reaching state Unhalt and `gfxMapQueue()`.
It never refreshes those cached report fields afterward. Reaching G42
proves that this branch did not report a callback failure, not that hardware
retained the values: no immediate post-write range readback is recorded.
The source therefore contradicts the claim that the cached string proves a
doorbell-0 exclusion during G42. There was no doorbell-0 write to drop in
any case. The attribution of the earlier range specifically to MES is also
unverified by these snapshots.

Another distinction: `CP_PQ_STATUS.DOORBELL_ENABLE` is initially zero in
both runs. G34 does not set it. The client loader sets it after G42, when it
loads the first client queue; the `pq 0->0` line after `gfxStart()` and later
`pq 0->2` support that order. Upstream KIQ init sets it before submissions.
A future doorbell experiment must establish this bit and NBIO aperture
enable before the kick, as well as the queue's own DOORBELL_EN and range.
Whether the global bit also affects this one-shot poll is a hypothesis.

### G34 descriptor and HQD comparison

Values below are the generated G34 image and code writes, except where
explicitly marked live. The saved 0.0.62 run contains only a partial G34 HQD
snapshot, not a full MQD/HQD dump. All addresses are GPU MC addresses.

| Field | G34 | Upstream KIQ comparison |
| --- | --- | --- |
| MQD layout | 512 dwords; header `c0310800`, pipeline stats 1, SE masks all ones, misc 7 | Matches `v11_compute_mqd` and initializer; CU-mask override is inactive for ordinary KIQ properties |
| MQD base / control | `0x800fa00000`, control `0x100`, VMID 0 | Matches raw aligned base, privileged default and MQD VMID 0 |
| Ring base / size | `0x800fa02000`, 4096 bytes; encoded base `0:800fa020` live | Matches shift-by-8 encoding and upstream 1024-dword KIQ ring |
| PQ control | `0xd0308909` live | Queue size 9, RPTR block size 9, UNORD_DISPATCH, PRIV_STATE, KMD_QUEUE; tunneling off, matching ordinary KIQ |
| RPTR report / WPTR poll addresses | `0x800fa03000` / `0x800fa03008` | Same low/high encoding; local VRAM placement differs from Linux's writeback allocation but G34's first test succeeded |
| RPTR / WPTR initialization | MQD RPTR=0, WPTR=0; memory WPTR=16 | Upstream also initializes descriptor pointers to zero. KFD's guessed WPTR is zero for this fresh empty queue. This alone is not a bug |
| EOP | base `0x800fa01000`, 2048 bytes, size field 8 | Matches upstream `GFX11_MEC_HPD_SIZE`; local bulk loader additionally writes EOP state and INIT_FETCHER bit31 |
| Persistent / IB / quantum / priority | `0x0be05501` / `0x00300000` / `0x111` / 0,0 | Matches defaults and ordinary KIQ properties |
| Queue doorbell | MQD control 0; HQD tail sets `0x40000000` (index0 enabled) | Upstream uses `0x40000000` in the MQD as well; local memory and hardware descriptors disagree |
| ACTIVE | MQD=1; G34 writes it as the third HQD-block word, then again at end | Upstream KIQ initializer activates only after configuring the queue |
| Scheduler | `0x3038->0x30a0` live, low byte HIQ ME1/pipe0/queue0 | Matches upstream marking for this chosen queue; Linux chooses a free queue0 from its ownership bitmap, not a universal fixed pipe0 |

The most concrete additional initialization defect is **early ACTIVE**:
`Navi33Gfx11MecQueue.h::execute()` copies MQD dword130=1 at register 0x1fab
before writing VMID, base, pointers, controls and EOP. The MQD was built with
KIQ semantics but loaded using a KFD-style bulk loop. Upstream's KIQ ordered
writer avoids this; this repository's client `loadQueue()` already forces
ACTIVE=0 during its bulk copy (0.0.46 fix), then activates last. G34 lacks
that fix. This creates a real initialization race; it is **not established
as the G42 cause**, since G34 subsequently reached RPTR16 and its original
fence on both runs.

Other differences to retain in the investigation:

- Upstream KIQ ring functions specify `align_mask=0xff`;
  `amdgpu_ring_commit()` NOP-pads the producer to a **256-dword boundary**
  before setting WPTR. G34 commits 16 dwords and G42 appends through32.
  The NOP word itself matches upstream, but the commit alignment does not.
  The initial 16-dword test succeeded, so misalignment is not a demonstrated
  universal fetch blocker. A fetch/preload/cache boundary interaction on
  the second append remains possible. Testing upstream alignment requires
  an explicit revised ring/span budget, not extra writes outside today's
  16-dword G42 append allowlist.
- G34's initial test uses SET_UCONFIG_REG plus RELEASE_MEM. Upstream's KIQ
  scratch test instead calls `gfx_v11_0_ring_emit_wreg()`; its KIQ memory
  fence uses confirmed WRITE_DATA. Passing G34 establishes initial fetch
  and a memory write, not successful privileged MAP processing or repeated
  KIQ commits.
- G34 does not disable periodic `CP_PQ_WPTR_POLL_CNTL.EN` as upstream KIQ
  init does. The offline `buildKiqInit()` contains this step, but G34 only
  copies its MQD fields, not its register plan. POLL_CNTL and POLL_CNTL1 are
  different registers (GC segment0 dwords 0x1e23 and 0x1e24). Current EN,
  one-shot behavior and hardware WPTR after the G42 kick were not captured.
- The bulk loop writes additional zero MQD state (including EOP and dequeue
  fields) that upstream's KIQ writer leaves alone. It skips the undefined
  0x1fb9 hole correctly. Memory MQD WPTR remaining zero after submission is
  normal unless something reloads that stale descriptor; no such reload is
  proven. Do not "fix" it by resetting a live ring's pointers.
- Upstream configures doorbell ranges before queue setup, establishes KIQ,
  then initializes KCQ and KGQ. G34 is a directly loaded queue, alongside
  started MES firmware, followed by hub/G40 initialization. KIQ ownership,
  descriptor reload or a pipe-level stall remain candidates. MES firmware
  start alone does not prove that Linux's MES queue/resource setup ran.
- `SET_RESOURCES` precedes KGQ through the upstream KCQ path; it is absent
  here. The direct KGQ helper does not itself emit it. This is a resource
  initialization question, not proof that adding that packet fixes fetch.
  The current Linux helpers also have MES mapping branches; the direct KIQ
  comparison does not establish this machine's complete Linux runtime path.
- HDP Complete proves exact remap readback, a flush write and two acceptable
  capacity reads. It is not a GPU-side checksum of the appended stream/MQD.
  HDP absence no longer explains the 0.0.62 failure by itself.

The source explicitly selects ME1/pipe0/queue0 for both G42's initial and
final RPTR reads; no obvious wrong-bank expression was found. However, a
post-map tuple of selector readback, ACTIVE, base, VMID, hardware WPTR,
RPTR and memory RPTR is missing. RPTR changed **16 to 0**, not simply
"stayed at zero." This unexplained regression makes an assertion of
"never fetched" particularly unsafe. The map/fence packet encoding matches
upstream; packet correctness does not establish firmware execution.

### VMID8 stall and what would distinguish the hypotheses

In `20261020-202558/client-gpu.txt`, the first VMID8 NOP times out. The
saved stall reports ACTIVE1, PQ_WPTR=0xc, PQ_RPTR=0, IB_BASE/RPTR=0, memory
fence=0. Its producer pointer was accepted, but no consumption is observed.
VMID8 uses ME1/pipe0/queue1, sharing **pipe0** with G34. Subsequent contexts
use VMID9 / pipe1/queue0 because VMID8 remains quarantined. Their gradient,
200 submissions, and N48N gradient are exact. This is not successful VMID8
reuse, nor evidence that VMIDs10..13 were exercised in this run. No RADV
vkfill result exists in these two run directories.

0.0.61 stopped before the G42 poll and its VMID8 NOP/gradient/repeat/N48N
all passed. 0.0.62 reached the poll and then pipe0's other queue stalled.
That correlation supports investigating a pipe0 stall during MAP processing
as well as a failed KIQ kick. Different boots and the capacity-check change
prevent a causal conclusion. L2 stayed 0 and flush misses stayed zero;
neither alone proves complete cache correctness.

Before another experiment, design event-stamped ioreg snapshots at G34
completion, after G40 setup, immediately before the G42 kick and on timeout.
Record the exact selected HQD identity and pointers, memory pointers, scheduler,
MEC control, range, NBIO aperture, PQ_STATUS, poll control and approved error
status. Audit every read against upstream hazards; bank-select writes are
still writes even if the diagnostic is called a "read." Do not acquire SEM,
read FIFO/clear-on-read status, or scan arbitrary MMIO.

The first bounded candidate should establish a coherent KIQ initialization
and commit path, with ACTIVE last, matching descriptor/HQD doorbell settings,
verified range and enables, HDP ordering, and one exact 64-bit BAR2 doorbell
at byte0. Its review must decide whether to isolate these changes in separate
cold experiments or evaluate the corrected initialization as one whole;
a bundled success would not identify which difference caused the failure.
An independent second KIQ fence before MAP would distinguish transport from
MAP processing, but needs its own exact packet/span/write-budget design:
the current 16-dword append window is not authorized for arbitrary probes.
No implementation or new approval is implied by this plan.

### Saved evidence identity

Paths are relative to `~/macos tahoe/navi33/runs/`. Both loaded UUIDs were
compared with the cached artifact's Mach-O LC_UUID during this offline audit:
0.0.61 `7CE1D94A-FD2C-39B0-9285-163AA3578AF5`, 0.0.62
`D94995B6-AFCC-3150-A504-5312E5A33F08`.

| File | SHA-256 |
| --- | --- |
| `20261020-200423/client-ioreg-after.txt` | `4d3649cd6f3fba6440e33345a5b240dfa125530e9c302adc09ac2a4922abfa67` |
| `20261020-200423/client-gpu.txt` | `31b20adfbd7f189980c12297df3c0e395391e1a89f4cce0f15a29585afa382fe` |
| `20261020-202558/client-ioreg-after.txt` | `4fc34e224c925cb8941a669697c48ddead700b8792f0e97256b04af015978ed3` |
| `20261020-202558/client-gpu.txt` | `3b973f1b2a82d64c447364fbef49d998091ec810daf92d5f4b6df939f598696a` |

Full diagnostics: `20261020-200423/results-20261020-200539-guest.txt`
and `20261020-202558/results-20261020-202755-guest.txt`. These are archived
observations, not live reads made for this analysis. Guest boot times and
successful autostart are not independent proof of full host power removal.
The route from these blockers to bare-metal acceleration is in
[BARE_METAL_ACCELERATION_PLAN.md](BARE_METAL_ACCELERATION_PLAN.md).
