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
`sfence; mfence`, but has no HDP flush; the flush is the strongest current
source-backed follow-up lead, not a proven cause. The generic HDP helper writes
zero to the remapped HDP memory-flush control register and reads memory size;
Navi33's exact device-specific mapping still needs to be resolved from matching
upstream IP discovery and register headers before designing any change.

`SET_RESOURCES` belongs to the separate `amdgpu_gfx_enable_kcq()` compute-queue
path; `amdgpu_gfx_enable_kgq()` does not emit it. Keep it as an indirect KIQ
setup question only, not a confirmed missing step in the GFX map path. The host
fake models neither HDP visibility nor these KIQ hardware effects. Do not add
operations or enlarge a stage-write span without a new bounded design and
renewed approval. Sources: [amdgpu_gfx.c](https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/amd/amdgpu/amdgpu_gfx.c), [gfx_v11_0.c](https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/amd/amdgpu/gfx_v11_0.c), [amdgpu_hdp.c](https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/amd/amdgpu/amdgpu_hdp.c), [amdgpu_device.c](https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/amd/amdgpu/amdgpu_device.c).

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
refusal of a consumed G34 ring before stage writes, timeout, and idempotent start.
CI builds the real kext and matched tools. Judge actual mapping, graphics fetch
and rendered output only from the live run, separately from host model results.
