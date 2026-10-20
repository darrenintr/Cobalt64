# G42: map one gfx11 graphics queue through G34

The user requested continuing graphics queue mapping/activation on 2026-10-20
("please go"). G42 ran once with kext 0.0.57 and once with 0.0.58 after the
user explicitly requested a warm host restart for the latter. Both attempts
timed out before the graphics queue mapped. The 0.0.58 one-shot latch is
consumed; the VM remains running on that artifact in
`runs/20261020-172609`. This was not a verified full power-off. Any further
GPU write attempt needs a full power-off and renewed explicit approval. This
work does not enable Metal publication.

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
`21868249-AAFB-3ACA-A72F-9E20F0557E70`). This is still only build and host
evidence: no 0.0.59 kext has run on the card. Any physical attempt still needs
a full host power-off and renewed explicit approval; the current 0.0.58 VM must
remain untouched until then.

## 0.0.60 read-only post-map capture

The follow-up captures the GFX ME0/pipe0/queue0 HQD after the MAP_QUEUES
poll: ACTIVE, VMID, BASE/BASE_HI, RPTR, CNTL and WPTR/WPTR_HI. These BAR5
offsets come from upstream Linux `gc_11_0_0_offset.h`. `Navi33,GfxMap` reports
the raw values and a validity bit. A failed read marks the snapshot invalid but
does not change the G42 result or cause another write. None of these registers
was added to either write allowlist. Host tests check the resolved offsets,
reject them as write targets, and show that a failed diagnostic read leaves a
MAP timeout intact. The values are raw evidence; zero is not interpreted as
idle if GC is not answering. This change has host-test evidence only and has
not run on the card.

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
