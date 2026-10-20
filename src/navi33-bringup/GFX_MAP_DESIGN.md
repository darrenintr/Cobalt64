# G42: map one gfx11 graphics queue through G34

The user requested continuing graphics queue mapping/activation on 2026-10-20
("please go"). G42 was attempted once on kext 0.0.57. It timed out and its
one-shot latch was consumed. The VM is still running on that artifact. No
retry, reboot, shutdown, artifact operation, or hardware action is authorized
by this design update. A retry needs another full power-off and renewed
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

The current 0.0.58 offline candidate tests a source-supported ordering
hypothesis: Linux marks the KIQ in RLC_CP_SCHEDULERS before loading and
activating its HQD, while our G34 queue had been activated as an ordinary
compute queue and was marked HIQ later by G42. When the existing
`navi33-gfx-map` gate and client-GPU gate are both open, the candidate now
sets the G34 scheduler selector to HIQ during G34 initialization, before
CP_HQD_ACTIVE, then G42 requires the low scheduler byte to be 0xa0 and no
longer writes the scheduler itself. This ordering mismatch is a plausible
hypothesis only; the G42 timeout does not establish its cause. The scheduler
write remains inside the existing exact register allowlist and is gated by
`navi33-gfx-map=1`, with `navi33-gfx-map-off`, `navi33-mec-off`, and
`navi33-psp-off` closing the path.

## Gate and plan

`navi33-gfx-map=1` plus the client GPU gate, no `navi33-gfx-map-off`,
`navi33-mec-off` or `navi33-psp-off`. VM builder option `--gfx-map` requires
`--client-gpu`. Off by default. The service starts once; a separate stage
latch is consumed before its first staging-window write. No retry after a
partial attempt, no reuse of G34's appended span, no alternate packet stream.

Before a stage write, read and require the original G34 ME1/pipe0/queue0:
ACTIVE=1, VMID=0, ring base exactly FB+250MiB+0x2000, PRIV_STATE and KMD_QUEUE
set, RPTR=16, WPTR memory=16/0, original fence 0x0c0b1601; scheduler HIQ bit
clear and graphics MAPPED=0. GRBM selects return to zero. The usual live
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
