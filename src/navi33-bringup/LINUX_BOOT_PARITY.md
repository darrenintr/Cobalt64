# RX 7600 / Navi33: Linux amdgpu boot-order parity

**Status (2026-11-09):** source-backed offline order contract, NOT a macOS
hardware bootstrap. The Navi33Probe.kext, OpenCore configuration and any
hardware write allowlist are unchanged.

**Baseline:** Linux torvalds/linux commit
[7475ea8b58616b33c04afb976cf0fbaad9dd63fb](https://github.com/torvalds/linux/tree/7475ea8b58616b33c04afb976cf0fbaad9dd63fb).
Pin this SHA when comparing behaviour. Do not compare a mix of kernel revisions.
Target silicon: GC 11.0.2 / gfx1102 Navi33, PSP13.0.7, GMC11, SDMA6,
MES v11. Route B is based on feature/navi33-mes-gfx-routeb (PR #22).

## Why the original G34 / G42 design is not Linux-equivalent

The old path initialized a dedicated ME1/Pipe0 G34 compute/KIQ and attempted
G42 direct PACKET3_MAP_QUEUES for Graphics. The saved 0.0.62 VM run passed
HDP flush but the map fence and MAPPED bit stayed zero; the same MEC Pipe0's
VMID8 queue stalled while Pipe1 still executed. The 0.0.67 bare-metal run
proved GPU compute and firmware autostart, but did NOT run G42; GFX ring state 6
remained a timeout. See GFX_MAP_DESIGN.md and AGENTS.md.

Linux's GC11.0.2 discovery sets enable_mes=true and enable_mes_kiq=true.
The Linux default amdgpu_async_gfx_ring is 1. With MES scheduler firmware
>=0x47, gfx_v11_0_cp_resume activates MES KIQ and MES Scheduler *before*
the compute and graphics queue mapping. It maps the latter with MES ADD_QUEUE,
not G42's direct legacy PACKET3_MAP_QUEUES. Firmware merely reporting
MES RUNNING is NOT the same as an initialized scheduler.

## Source-backed call order (PSP firmware path, MES KIQ, async GFX)

Linux IP discovery adds blocks in order COMMON, GMC, IH, PSP, SMU,
DISPLAY, GC, SDMA, other blocks, MES. This is a list of callback owners,
**not** itself the execution order of every hardware write.

1. amdgpu_device_ip_early_init: IP Discovery and each block early_init.
2. amdgpu_device_ip_init: all sw_init, with COMMON.hw_init and GMC.hw_init
   performed early as exceptions (GMC must be operational before GPU BOs,
   GPU writeback and kernel allocations). amdgpu_ib_pool_init and
   amdgpu_ucode_create_bo happen after sw_init.
3. amdgpu_device_ip_hw_init_phase1: COMMON/IH hardware initialization if
   not already done, as applicable.
4. amdgpu_device_fw_loading: PSP-managed IP firmware loading and other
   firmware-path decisions, followed by phase2 hw_init for remaining blocks.
5. The GFX11 block's gfx_v11_0_hw_init: RLC autoload completion,
   gfxhub_enable, golden registers, RLC resume, and cp_resume. Include the
   NBIO GC doorbell setup, CP firmware and necessary clocks/SMU gates.
   The exact order of these substeps must be compared inside that function.
6. gfx_v11_0_cp_resume: set CP doorbell ranges; enable compute and GFX
   for async mode; call amdgpu_mes_kiq_hw_init (NOT legacy G34 KIQ).
7. mes_v11_0_kiq_hw_init: start/read MES firmware, set KIQ identity
   (ME3/Pipe1), initialize MES KIQ MQD/EOP/ring/RPTR/WPTR/doorbell, and
   prove that the KIQ queue can execute.
8. With firmware >=0x47, mes_v11_0_hw_init: initialize Scheduler MQD/ring
   (ME3/Pipe0), map Scheduler queue through MES KIQ, SET_HW_RESOURCES,
   optional RS64 result buffer, SET_HW_RESOURCES_1 when MES firmware >=0x52,
   QUERY_SCHEDULER_STATUS, and hand off KIQ ownership to MES. Once MES is
   active the ordinary driver must NOT keep submitting to its KIQ ring.
9. gfx_v11_0_kcq_resume: prepare compute MQDs then
   amdgpu_gfx_enable_kcq -> MES mapping of the legacy compute rings.
10. gfx_v11_0_cp_async_gfx_ring_resume: prepare GFX MQD(s) then
    amdgpu_gfx_enable_kgq -> MES ADD_QUEUE(GFX) via Scheduler; finally
    gfx_v11_0_cp_gfx_start.
11. Test actual GFX and compute ring fences with independently read-back
    GPU results. Only then attempt an offscreen draw with pixel readback.
    Metal/IOAccelerator/WindowServer are separate, later integration.

**Important conditional paths:** the above is the PSP-loaded GC11.0.2,
MES-KIQ-enabled, async-GFX variant. Linux has branches for firmware direct
load, SR-IOV, resets/suspend, async_gfx_ring=0 and older MES firmware. Do
not claim a universal hardware initialization order from one branch.

## Offline order guard

src/Navi33LinuxBootParity.h defines a *host-only* step ledger and rejects
incorrect silicon, incompatible firmware path, absent exclusive owner,
missing retained memory, G34/G42 conflict, MES version <0x47, and
out-of-order/missing event names. Versions >=0x52 require
SET_HW_RESOURCES_1. Run via make -C src/navi33-bringup host-test.

This guard **does not** perform any MMIO, verify the truth of recorded
events, or authorize a stage on physical silicon. Recording an event does
NOT demonstrate that hardware completed it.

## Required per-stage silicon evidence before enabling writes

| Linux stage | Comparable Cobalt64 component | Acceptance evidence |
|---|---|---|
| PCI/IP/common | Navi33Discovery, NBIO/HDP | IP base + BAR sizes, remap register snapshots, exclusive PCI function |
| GMC before BOs | Navi33Gmc11*, VRAM allocator | Fully owned/pinned VRAM roots, VM ranges and IOMMU/physical reachability; no overlapping GOP allocations |
| PSP/SMU/RLC | existing gated PSP, SCPM | firmware version/hash, completed PSP fences, RLC boot status, clocks and recovery |
| MES KIQ | Navi33GfxMesMqd, bootstrap | live ME3/Pipe1 HQD, doorbell and independent KIQ ring fence |
| MES Scheduler | Route B bootstrap | ME3/Pipe0 mapped by KIQ, independent Scheduler ring/progress/fence |
| SET_HW_RESOURCES | Navi33MesGfxRouteB | masks from actual GC/MMHUB/OSSSYS discovery; API + scheduler fences |
| MES ADD_QUEUE | Route B transport | correct VM root, MQD, ring, doorbell, firmware API version and two fences |
| GFX ring | Navi33Gfx11GfxRing | GFX MAPPED/ACTIVE, RPTR progress, GPU-origin fence then actual pixel readback |
| Renderer | gfx1102 RADV + Metal port | offscreen draw pixel verification before any desktop publication |

## Corrections to make in the NEXT isolated code implementation

- Create a **separate boot configuration** that does not run existing G34,
  G40 or G42 ownership before MES KIQ/Scheduler bootstrap; do not merely
  add steps after the old autostart. Use a single owner epoch.
- Allocate and retain real, separate MES KIQ and Scheduler MQDs, EOP,
  ring and writeback buffers. Linux's GPU BO memory lifetime cannot be
  simulated by a host pointer or synthetic ownership flag.
- Port the **behaviour**, NOT verbatim GPL-licensed Linux source, into
  independently written MIT-side code using exact gc11.0.2 definitions.
- Confirm firmware API version when preparing MES ADD_QUEUE: Linux
  mes_v11_0_add_hw_queue selects wptr_mc_addr for API version >= 2,
  otherwise wptr_addr. A scheduler version 0x92 does not by itself establish
  that API version. Do not blindly choose a field.
- Send exact SET_HW_RESOURCES masks from live IP Discovery and owned HQD
  slots; reject placeholders and Phoenix-derived masks.
- Place bounded, CPU-visible **two-fence** checkpoints after KIQ,
  Scheduler bootstrap, resource setup and ADD_QUEUE. Record exact GPU
  values, selected GRBM bank, RPTR/WPTR, doorbell range and ownership.
- Fail closed on partial progress; do not unmap/recycle GPU-visible
  buffers or replay uncertain submissions after timeout. Recovery must
  be independently tested. Never modify VBIOS.

**Live tests require a new, explicit hardware-write authorization, a cold
boot, an independently operable recovery path, version-pinned artifacts,
and exclusive control of the GPU.** This branch is only for source parity
planning and host tests. It must not be linked to Navi33Probe.kext or
automatically deployed to EFI.

## Pinned source references

- [amdgpu_discovery.c](https://github.com/torvalds/linux/blob/7475ea8b58616b33c04afb976cf0fbaad9dd63fb/drivers/gpu/drm/amd/amdgpu/amdgpu_discovery.c)
- [amdgpu_device.c](https://github.com/torvalds/linux/blob/7475ea8b58616b33c04afb976cf0fbaad9dd63fb/drivers/gpu/drm/amd/amdgpu/amdgpu_device.c)
- [gfx_v11_0.c](https://github.com/torvalds/linux/blob/7475ea8b58616b33c04afb976cf0fbaad9dd63fb/drivers/gpu/drm/amd/amdgpu/gfx_v11_0.c)
- [mes_v11_0.c](https://github.com/torvalds/linux/blob/7475ea8b58616b33c04afb976cf0fbaad9dd63fb/drivers/gpu/drm/amd/amdgpu/mes_v11_0.c)
- [amdgpu_gfx.c](https://github.com/torvalds/linux/blob/7475ea8b58616b33c04afb976cf0fbaad9dd63fb/drivers/gpu/drm/amd/amdgpu/amdgpu_gfx.c)
- [amdgpu_drv.c](https://github.com/torvalds/linux/blob/7475ea8b58616b33c04afb976cf0fbaad9dd63fb/drivers/gpu/drm/amd/amdgpu/amdgpu_drv.c)
