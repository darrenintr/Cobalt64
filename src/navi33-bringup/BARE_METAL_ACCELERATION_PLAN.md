# RX 7600: from G42 to a GPU-rendered bare-metal macOS desktop

Planning baseline: 2026-10-20, kext 0.0.62. The goal is GPU composition of
the desktop, wallpaper and applications on the RX 7600, followed by moving
daily development to that macOS installation. A working firmware framebuffer
or a computed image copied to it is not sufficient evidence of that goal.

This document authorizes no execution. This task was limited to saved-file
and source analysis. The host is reported to be on the headless VFIO boot;
the VM may still be running in `n33-g42-0.0.62`. Leave it and the card alone.
Every future card-write experiment below requires explicit user approval,
a full host power-off beforehand, and independently working SSH recovery.
Do not flash or modify the VBIOS, including as a proposed workaround.

## 1. Establish the bare-metal console/aperture proof

The 2026-10-19 08:47 finding in [AGENTS.md](../../AGENTS.md) identifies a
specific mismatch: `boot_args->Video` still names firmware console
`0x7c00000000`, whereas macOS assigns BAR0 `0x830000000`, size 256 MiB.
`Navi33Probe::fillConsole()` takes the stale boot-args value. The PSP plan
rejects it as NO_CONSOLE before the HUBP aperture proof. It is not evidence
that the actual display surface has disappeared. The plan's zero console
offset on this refusal is unpopulated, not a measured offset.

First design a read-only capture of both boot-args Video and the current
platform console, BAR0 resource metadata, geometry/pitch, active DCN3.2.1
HUBP surfaces and relevant relocation logs. The existing
`Navi33GOPFB::captureConsole()` uses
`IOPlatformExpert::getConsoleInfo()` and validates the result against BAR0;
that is a repository precedent for investigating the current console.
Do not hard-code either physical base, assume offset0, or substitute a
user-provided address merely to make the plan pass.

The candidate resolver should prefer a validated current console, retain
both raw values and their provenance in ioreg, and refuse disagreement it
cannot explain. If current platform information is insufficient, a
same-boot firmware BAR/surface record and verified relocation relationship
would be needed. Linux's hostfb record from a different boot is not that
proof. The identity of the component that moved BAR0 is still unverified.
Keep `planWindow()`'s live HUBP/MC-to-BAR-offset checks, geometry bounds,
overflow checks, scanout exclusion, and firmware-window margins intact.

**Required evidence:** a kill-switch-closed bare-metal boot reaches PLAN_OK
using current console and active HUBP data; the firmware/staging windows
are demonstrably outside every live surface. Negative offline cases include
stale/out-of-range console, a 16 MiB offset error, bad geometry, overflow,
ambiguous surfaces, and a console resource belonging to another device.
Only afterward propose the bounded bare-metal PSP/autostart sequence.

**Risk:** selecting the wrong VRAM offset can overwrite the only display
surface or firmware. macOS/IOPCIFamily relocation and the firmware display
handoff may differ from the VM. A host simulation returning PLAN_OK is
insufficient to authorize the physical load.

**Approval:** source work, host checks and saved-file analysis need no
card-write approval. A future read-only boot still needs coordination to
stop the current VM/change the host state. PSP/autostart on bare metal needs
explicit card-write approval; prior VM approvals do not transfer.

**Progress (2026-10-20, kext 0.0.63, offline):** the read-only capture is
built. `src/Navi33ConsoleSources.h` classifies a console source against the
live BAR0 (empty, virtual, bad geometry, outside, inside with its offset),
host-tested in `tests/test_console_sources.cpp` with the bare-metal values.
The kext publishes `Navi33,ConsoleSourcesStart` (at `start()`, before a
framebuffer can replace the platform console) and `Navi33,ConsoleSourcesPlan`
(on each PSP-selector call): BAR0 base/size plus `boot_args->Video` and
`PE_state.video`, each raw and classified. `usb/run.sh` prints them. Nothing
feeds the plan yet: `fillConsole()` is unchanged, so bare metal still reports
NO_CONSOLE. Next: one kill-switch-closed bare-metal boot to capture these
values, then design the resolver from them.

## 2. Establish reliable KIQ transport, then the graphics queue

See the pinned upstream comparison and raw-run identities in
[GFX_MAP_DESIGN.md](GFX_MAP_DESIGN.md). 0.0.62 passed HDP but did not complete
MAP_QUEUES. Its cached range string does not show the range after G40.
G34's bulk loader activates too early, leaves the MQD doorbell disabled
while enabling the HQD, and uses a KFD-style poll instead of upstream's
KIQ doorbell. Initial MQD WPTR0 is normal. None of these differences has
been proved to cause this timeout.

Before implementation, review a bounded candidate with these requirements:

1. One named queue, ME1/pipe0/queue0, with ownership distinct from MES,
   clients, G36–G39 and every other experiment. One named graphics target,
   ME0/pipe0/queue0. Keep allocations pinned after uncertain completion.
2. Default-off gate and all applicable kill switches; latch before the
   first changed register or staging write. Define exact expected old
   states, allowed values, order, maximum writes and timeout at each phase.
   A selector change counts toward the MMIO budget.
3. ACTIVE stays clear during HQD configuration and becomes set only after
   base, VMID, controls, pointers and EOP are ready. MQD and HQD must agree
   on doorbell index0/enabled. Do not reload or reset pointers on a live
   partially consumed ring.
4. Read and verify MEC range0..0x450 at the point of use; G40 already has
   writes for this range, so avoid redundant changes based on stale
   telemetry. Review ownership of every affected doorbell, including MES.
   The exact GC11 range registers are segment0 dwords0x1dfc/0x1dfd in the
   cited upstream header. Resolve through this card's IP Discovery.
5. Verify NBIO doorbell aperture, queue DOORBELL_EN and global
   `CP_PQ_STATUS.DOORBELL_ENABLE` before submission. Review whether to adopt
   upstream's periodic-poll disable step; its register is not POLL_CNTL1.
   Any added register/mask must be sourced to the exact upstream IP header.
6. Review upstream's 256-dword KIQ commit alignment against G34/G42's
   16/32-dword producer values. Adopting it requires a new exact span and
   write budget; the current append allowlist cannot accommodate it.
   Stage/read back only the reviewed ring/MQD/WPTR/fence spans, retain the
   HDP flush, then issue at most one exact 64-bit BAR2 write at byte0 with
   the planned producer value. The Darwin callback currently admits client
   doorbells and GFX byte0x458, so adding byte0 needs an explicit narrow
   allowlist change. It must not become an arbitrary BAR2 writer.
7. Report actual before/after values and the selected HQD identity at each
   boundary. Require KIQ completion, graphics MAPPED/ACTIVE, and then the
   G40 clear-state/ring fence as separate verdicts. No automatic retry,
   fallback kick, pointer rewind or replay after partial/unknown progress.

A transport-only second fence before MAP is a useful discriminating
experiment. Its packet sequence, fence address and space budget must be
designed separately; current G42 authorization covers only its fixed append.
If transport succeeds but MAP stalls, investigate resource setup, ownership,
target MQD and MES/KIQ initialization ordering. Do not add SET_RESOURCES
on speculation. If multiple initialization changes are bundled, report that
a success validates the bundle rather than proving one particular cause.

**Required evidence:** fresh-cycle provenance and matching artifact UUID;
G34 initial and subsequent producer/consumer progress with exact fences;
map fence and MAPPED=1; GFX preamble fence; first VMID8 client succeeds;
reused and simultaneous client queues, including pipe0, remain exact;
TLB ACKs without misses, no faults/quarantine, and verified teardown or
retention. Then run a small RADV graphics draw with pixel readback through
N48N, followed by context reuse and interleaved compute/graphics work.
Compute success and Vulkan device enumeration cannot replace a graphics draw.

**Risk:** KIQ packets can program queues, global ranges affect other engines,
and a stalled privileged queue may block its pipe. The 0.0.62 VMID8 stall
must be resolved rather than worked around by silently using VMID9.

**Approval:** documentation/source/offline checks do not write the card.
Every proposed initialization, doorbell, packet submission, and physical
regression run needs explicit approval for that bounded run and a full
power-off. These pages are not approval and no candidate was implemented here.

## 3. Complete the Metal evidence and adapt the reusable Navi48 stack

Hard rule6 remains the acceptance contract: one device-owner epoch must
connect ownership, memory/VM, required queues, per-client submit/wait,
shader translation, output readback and recovery. Its documented G41
exception permits an opt-in experimental Metal nub after the narrower live
core checks. `Navi33MetalPure.h::publish_verdict()` requires service start,
a retired compute submission, the G40 fence, acknowledged TLB flushing with
no misses, no hang/quarantine, and `navi33-metal=1`. It does not establish
shader correctness, presentation or a production-ready desktop.
0.0.62 fails the GFX and no-quarantine conditions; it is not eligible.

The current path intentionally exposes no transfer ring to RADV, so SDMA
is not used. Do not claim SDMA is proven. Keep this explicit exception when
using compute/GFX transfers; if any ported feature requires SDMA, first
design and approve a real SDMA6 queue/copy/fence/readback/park proof.
The older Navi33 scaffold README files still describe ABI14-era missing
BO/context support. They are not the current implementation status:
N48N selectors0..8 and the kernel heap/client core now exist. Conversely,
those selectors do not imply that the later Navi48 interfaces exist.

| Reusable part | Existing foothold | Work and evidence still needed |
| --- | --- | --- |
| `tools/native/navi48accel` | IOAccelFamily2 adapter, Metal/display ops interfaces; Navi33 already has the compatible `Navi48MetalNub` provider | Validate private ABI against target macOS26.7.1; bind the accelerator to this owner and generation; audit app access, lifecycle and failure handling |
| `tools/native/navi48metal` and `third-party/metal2vulkan-n48` | Metal objects, AIR-to-SPIR-V translation, Vulkan-backed encoding and IOSurface/presentation machinery | Audit gfx1201 identity/features/assumptions; compile gfx1102 via RADV; exercise shader and format coverage; validate memory import and synchronization |
| `mesa-patches/0001..0011` | Darwin N48N winsys; 0010 selects Navi33 device data and service; 0011 handles the absent high VA range | Vulkan graphics readback; honest hardware limits; unsupported paths fail explicitly; full graphics submission/lifetime checks |
| Navi33 N48N core | Selectors0..8, BO/VM/context/compute submit/wait; gated GFX IB path | Graphics evidence, app-facing access and isolation, loss/teardown; later Navi48 scanout/host-import selectors9..26 remain absent |
| `src/dcn321` and Navi33 display code | Correct-generation display groundwork and firmware scanout handoff | Native display ownership, flips/vblank, mode/link behavior and recovery as needed; never copy DCN4.1 register programming |

**Required evidence:** under the narrow experimental nub gate, an approved
Metal test translates and runs a known shader on this RX7600, renders an
offscreen triangle/texture into a known format, waits, copies it back and
matches pixels. Repeat with resource/context reuse and multiple clients.
Verify IOSurface import/export, CPU/GPU synchronization, allocation failures,
device loss, and memory retention after timeout. Tie every result to the
same loaded build and owner epoch, and advertise only tested capabilities.

**Risk:** private Apple ABI errors can panic the system; bad resource lifetime
can corrupt memory; inherited feature claims can send unsupported workloads.
The repository reports a Navi48 GPU-composited desktop, but that does not
prove the RX7600 route. Keep kernel hardware support separate and specific to
GC11/PSP13/DCN3.2.1.

**Approval:** offline porting needs no card-write approval. Installation,
enabling the experimental nub and actual rendering submissions must be
explicitly approved as a bounded physical experiment after full power-off.
Do not publish production readiness from host tests or bundle discovery.

## 4. Prove bare-metal desktop composition and presentation

Once steps1–3 pass, reproduce the working owner/queue/render path on bare
metal, not just in the VM. Start with the proven firmware mode and one display
if its scanout can be safely owned. GPU rendering can initially feed that
mode; full mode switching need not precede the first accelerated desktop.
However, a CPU memcpy of one shader image to the framebuffer is not evidence
that WindowServer uses Metal.

Connect the accelerator and framebuffer through coherent IOSurface ownership,
present/flip completion and vblank semantics. Audit which Navi48 display ops,
N48N selectors and system-memory imports WindowServer actually needs.
Implement only the required DCN3.2.1 path with separately bounded write
designs. System RAM imports need real GPU reachability/VM and lifetime
evidence; `PhysicalUnverified` or CPU physical allocation alone is not enough.
Polling fences may suffice for a first proof, but sustained operation likely
needs IH interrupts and a reviewed scheduling/recovery policy.

**Required evidence:** WindowServer demonstrably opens this accelerator,
issues GPU work and presents its completed surfaces; screenshot/readback
matches visible output during window movement, wallpaper and representative
apps. Preserve submission/fence and presentation evidence together, distinguish
software fallback, and repeat across cold boots. Test login/logout, client
exit, load, memory pressure and display reconnect before calling it usable.
Native modeset, multiple monitors and sleep/wake are separate milestones,
not implied by a successful fixed-mode desktop.

**Risk:** loss of the only display, WindowServer restart loops, scanout/render
ownership conflicts and private presentation ABI gaps. Guest success does
not guarantee the bare-metal PCI/console or recovery path.

**Approval:** physical rendering, display/interrupt programming and reboot
experiments require explicit approval and the defined full-power-off recovery
procedure. Keep a known-good OpenCore entry and diagnostics available.

## 5. Move development to macOS after a stable baseline

Archive the last working CI artifact, matching tools, UUID, boot-args, source
revision and results on persistent storage. Keep Linux/headless SSH recovery
and the original user VM files intact. Copy the repository and configure the
macOS SDK/Clang/Python/Mesa toolchain; begin with editing, offline tests and
user-space builds on macOS while retaining the known-good boot image.

**Required evidence:** repeatable cold boot to the accelerated desktop,
working recovery from another machine, and reproducible build/artifact
provenance. This repository currently requires **GitHub Actions only for kext
builds**. Moving the editor to macOS does not change that rule. A later move
to local kext builds needs an explicit workflow decision, documented toolchain
and matching validation; update AGENTS.md with that decision.

**Risk:** developing and testing on the only accelerated display makes a
driver fault disrupt the entire session. Preserve remote access and a
separate working boot configuration before replacing the baseline.

**Approval:** copying sources and configuring user-space development does not
write the GPU. Installing/booting a changed GPU-writing driver remains a
separately approved physical experiment.

## Effort and uncertainty

The console resolver and better diagnostics are bounded source tasks,
plausibly days of work plus an approved boot to validate them. G42 may yield
to a small KIQ initialization correction, or require several cold experiments
on firmware/resource interaction; there is no defensible completion date.
After graphics works, app access, Metal/IOSurface adaptation and bare-metal
presentation are likely weeks to months of integration, even with Navi48
code available. Robust recovery, interrupts, native modesetting and sleep/wake
can take longer. These are planning estimates, not measured schedules.
Moving development depends on demonstrated stability, not elapsed time.
