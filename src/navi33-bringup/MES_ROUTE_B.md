# Navi 33 / RX 7600 MES Graphics Queue — Route B

Status: **offline preparation only**; no new on-card experiment, no GFX
acceleration claim. Draft PR from `feature/navi33-dma-backend`.
This route is independent of the G34/G42 direct-MEC KIQ path.

## Why this route

The RX 7600 uses gfx1102 / GC 11.0.2. Linux
`amdgpu_discovery_set_mes_ip_blocks()` enables MES and MES KIQ for this IP.
With MES scheduler firmware >= 0x47, Linux's
`mes_v11_0_kiq_hw_init()` enables legacy queue mapping through MES.
Then `amdgpu_gfx_enable_kgq()` calls
`amdgpu_mes_map_legacy_queue()`, which encodes an
`MES_SCH_API_ADD_QUEUE` packet for GFX rather than submitting the legacy
`PACKET3_MAP_QUEUES` G42 path. The last RX 7600 reference Linux boot
logged scheduler version `0x92` (check again on a fresh boot).

Upstream source definitions:

- Linux `drivers/gpu/drm/amd/amdgpu/amdgpu_discovery.c`, `amdgpu_gfx.c`
- Linux `drivers/gpu/drm/amd/amdgpu/mes_v11_0.c`: queue init,
  scheduler packet submission, `SET_HW_RESOURCES`, `ADD_QUEUE`
- Linux `drivers/gpu/drm/amd/include/mes_v11_api_def.h`: packed ABI
- Linux `drivers/gpu/drm/amd/amdgpu/gfx_v11_0.c`: GFX MQD and ring
- nullarc `tools/phxctl/mesboot.c::gfx_via_mes`: reference design only.
  Do not copy nullarc's PolyForm Noncommercial modules into MIT files.

## Implemented, host-only

- `src/Navi33MesGfxRouteB.h`: Linux-shaped 256-byte MES API frames
  (`SET_HW_RESOURCES`, `SET_HW_RESOURCES_1`, `ADD_QUEUE(GFX)`,
  `QUERY_SCHEDULER_STATUS`), explicit status offsets, a separate
  API fence and MES scheduler sequence fence. Maps GFX pipe0 queue0,
  doorbell dword `0x116` (BAR2 byte `0x458`).
- `src/Navi33MesSchedulerTransport.h`: one command frame plus one query
  frame (128 dwords), monotonic 64-bit producer/consumer pointers, power-of-2
  wrap, capacity/overflow checks, pointer conflict refusals, and CPU-only
  staging for deterministic host tests. The MES Scheduler ring uses
  doorbell dword `0x16` (BAR2 byte `0x58`), distinct from the MES KIQ
  ring's dword `0x18` (byte `0x60`).
- `src/Navi33MesSchedulerBootstrap.h`: independently builds MES KIQ
  (ME3 pipe1) and MES Scheduler (ME3 pipe0) MQDs, register-write manifests
  and a KIQ-to-Scheduler MAP_QUEUES packet (ME2, engine5); validates
  non-overlapping, aligned, bounded GPU allocations. Reuses existing
  `Navi33GfxMes.h` and `Navi33GfxMesMqd.h` without invoking writes.
- `tests/test_mes_gfx_routeb.cpp`, `test_mes_scheduler_transport.cpp`,
  `test_mes_scheduler_bootstrap.cpp`: successful encodings and negative
  tests, included in `make host-test`. The Linux CI runs GCC, Clang and
  sanitizers; these tests do **not** represent real GPU execution.

All new files are pure C++17 helpers. None is linked into a new MMIO writer,
the shipping kext's `start()`, or the OpenCore USB autostart scripts.
No existing G42 code is removed or switched on.

## Required integration stages — NOT implemented

1. **Separate boot mode**: after pinned PSP/SMU/GC firmware and G33 MES
   firmware start, use a fresh card epoch and refrain from G34 direct MEC
   ownership, G40 client GFX initialization and G42. Current
   `navi33-autostart` starts MEC; do not reuse it unmodified.
2. **MES KIQ pipe 1**: allocate distinct MQD, EOP, ring, RPTR/WPTR WB
   storage from a reserved GPU memory lease, configure
   `RLC_CP_SCHEDULERS` with ME3 pipe1 and establish the ring's own
   doorbell/range. Verify hardware queue state and a KIQ fence before
   giving it additional work.
3. **MES Scheduler pipe 0**: allocate distinct MES ring resources,
   load its MQD to the correct GRBM-selected bank, map it from the MES
   KIQ via `MAP_QUEUES` (ME2 / engine5). Ensure KIQ has released the
   scheduler's queue; verify mapped state, real RPTR progress, and a
   simple scheduler query fence.
4. **Resource setup**: build `SET_HW_RESOURCES` from a real silicon/IP
   discovery snapshot, not hard-coded Phoenix bitmaps. Reserve GFX
   pipe0 queue0 for MES; preserve ownership of Compute HQDs/VMIDs.
   Provide separately pinned scheduler-context, fence, and (when required)
   cleaner-fence memory. For MES fw >= 0x52, also submit and acknowledge
   `SET_HW_RESOURCES_1`, then query scheduler status.
5. **Actual GFX mapping**: construct a Navi33 GFX MQD with GC11.0.2
   firmware/ring state, root ring buffers, 64-bit WPTR/RPTR and valid
   doorbell ranges. Submit `MES ADD_QUEUE(GFX, map_legacy_kq=1)`.
   Completion requires BOTH the API status fence and the scheduler
   sequence fence, plus a consistent hardware GFX HQD snapshot.
6. **Execution proof**: run a bounded GFX PM4 scratch/fence test;
   only then `n33-vkdraw` with offscreen pixel readback. `VkDevice`
   enumeration is not proof of graphics execution. Metal and WindowServer
   are later stages.

### Safety gates and limits

- All hardware writes need an **independent, newly approved** fixed
  allowlist, one-shot stage latch, read-before-write preconditions, exact
  VRAM span lease, watchdog, GRBM selector reset, and a recovery console.
- Existing G34/G42/direct GFX and new MES-owned GFX cannot share a power
  cycle, even if different doorbells are used. Firmware is not known to
  support arbitrary dual owners.
- On timeout, no free/reuse of GPU-visible memory or replay of an
  uncertain packet. Record raw HQD/queue status, both fence words and
  retired producer/consumer pointers; power off before a fresh attempt.
- Never change the VBIOS. Keep the sole GPU host bound to Linux amdgpu
  outside an explicitly authorized headless test. CI is compile/host-only.
- Run-path code MUST obtain owner/fence evidence from controlled kernel
  state and device snapshots. Never trust an application-provided
  `Owner` flag as hardware proof. All presently exposed booleans are for
  offline planning and simulated tests only.

## Gate review checklist

The next eligible patch is a **compile-only macOS bridge** that connects
these plans to a separately leased MES scheduler ring, without linking it
into `Navi33Probe.kext`. Verify all exact Navi33 register offsets against
IP discovery and upstream. Before a real test, audit
`RLC_CP_SCHEDULERS`, `CP_PQ_STATUS.DOORBELL_ENABLE`, NBIO aperture,
MES ring0/1 doorbell ranges, GFX MQD layout, and the transition from KIQ
ownership to MES-owned KIQ. Do not infer those states from cached ioreg
values collected before G40 changes the doorbell ranges.
