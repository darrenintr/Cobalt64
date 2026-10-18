# Navi33 physical PCI / IOMMU / BAR transport milestone (PR #20)

**Status:** The macOS source now contains a real, compiler-verified
`IODMACommand`-backed staging session and isolated physical BAR0/BAR5
adapters. **This is NOT a functional RX7600 SDMA6/GFX11/Metal driver.**
No register write, hardware COPY or GPU fence has been submitted on a
physical Navi33 by Cobalt64.

## Physical-resource path now present

1. `Navi33DarwinStage::Session`: obtains this specific RX7600's
   `IOMapper::copyMapperForDevice`, retains a 64-KiB prepared
   `IOBufferMemoryDescriptor` and `IODMACommand`, and builds
   pinned IOVA-backed source, destination, ring and fence staging bytes.
   `readByte()` and `synchronizeForDevice()/synchronizeForHost()`
   use **the same retained mapping** instead of fake host arrays.
   This does **not** put a GPU page table in VRAM.
2. `Navi33DarwinQueue1IO`: takes a physical, identity-checked
   `IOPCIDevice`, validated gfx1102 IP Discovery, and the retained DMA
   session. It maps BAR5, converts GC Queue1 register indices through
   checked `Navi33Sdma6BarAdapter`, and returns callbacks compatible
   with the isolated `sdma6_mmio::IO` transport. CPU-side ring/fence
   access now uses the real pinned descriptor and device cache sync.
   All hardware register WRITE callbacks currently return **false**.
3. `Navi33DarwinRootReadback`: maps BAR0 only for readback of an
   externally leased 4-KiB GMC11 PTE root; it verifies the BAR mapping,
   GPU physical root, CPU-visible offset and root page bounds before
   returning 64-bit words for `vram_root::inspect`. It does not allocate
   VRAM or prove the lease belongs to this driver. No BAR0 writes.
4. `Navi33DmaLifetime`: a fail-closed pin lifecycle. Before a future
   real MMIO attempt, a hardware owner must mark the retained mapping
   Quarantined. A failed callback may still have reached silicon.
   Quarantined mappings are **not completed/unmapped even on user-client
   close**, and the provider-wide software DMA lease cannot be recycled.
   The only safe unpin path would require separately implemented,
   independently verified device reset. This code offers no reset or
   force-release method.
5. The production `Navi33Probe.kext` still includes only the
   read-only diagnostic and DMA staging. The BAR0/BAR5 sidecars are
   compiled to object files on Intel macOS CI with
   `make physical-io-check`, **never linked, loaded, or installed**.
   Linux GCC/Clang and sanitizer runs test the DMA lifetime policy.

## Register addresses checked against a physical RX 7600

`tests/fixtures/rx7600-linux-ipd.txt` is the IP Discovery table that
Linux amdgpu parsed from a real RX 7600 (1002:7480 rev cf, 1 MiB BAR5),
captured read-only from sysfs by `tools/capture-linux-ipd.sh`. The IP
versions match what the Kext accepts (GC 11.0.2, SDMA 6.0.2, MP0/MP1
13.0.7, DCN 3.2.1, MMHUB 3.0.2, NBIF 4.3.1).
`tests/test_rx7600_silicon_bases.cpp` feeds those bases to every plan
builder (SDMA0 status, Queue0 census, Queue0/Queue1 ring bank, GFXHUB
and MMHUB VMID0/VMID1, FB location, PSP13, NBIO memsize, DCN321) and
checks the exact BAR5 byte offsets Linux would use. On real silicon
NBIF BASE_IDX 0 is 0, so BASE_IDX 2 for `RCC_CONFIG_MEMSIZE` matters.

Every register index and bitfield used by `src/navi33-bringup` and
`src/dcn321` was also compared with upstream `gc_11_0_0`,
`mmhub_3_0_{0,2}`, `mp_13_0_2`, `nbio_4_3_0` and `dcn_3_2_1` headers;
all match. That proves the addresses, not that writing them is safe.

### First live read-only preflight (2026-10-17, Navi33Probe 0.0.3)

`n33compute --rom rx7600-ipd.bin` (built by `tools/ipd-from-capture.py`)
on macOS 26.7.1 made 8 BAR5 reads, none all-ones. PSP13 C2PMSG_81 = 0
(reason 8, sOS not loaded: expected with no OS driver for 1002:7480).
MMHUB VM_CONTEXT0_CNTL = `0x01fffe00`, a plausible power-on value, the
first live sign that the Discovery-derived offsets land on real registers.
GFXHUB VM_CONTEXT0_CNTL and SDMA0 RB_CNTL/RPTR/WPTR all read 0, which
suggests a GC block that isn't answering rather than an idle queue. 0.0.4
reports SDMA0_STATUS_REG, UCODE_CHECKSUM and the PSP bootloader mailbox
(C2PMSG_35) in `Navi33,LastHwPreflight`, and no longer classifies a zero
SDMA0_STATUS_REG as an observed SDMA.

### Physical test loop (Navi33Probe 0.0.5, N33N ABI 8)

CI assembles the OPENCORE stick layout as the `navi33-usb-<runner>`
artifact: `EFI/OC/Kexts/Navi33Probe.kext` plus `navi33/` with the tools,
`rx7600-ipd.bin` and the `usb/` scripts. `run.sh` runs every diagnostic
in one pass. `install-autorun.sh` adds a LaunchDaemon that runs it once
per boot from the pinned stick, so a test cycle is boot, wait, reboot.

`N33_SEL_MMIO_PEEK` reads up to 64 dwords from one window in
`src/Navi33PeekWindows.h`, under the same `navi33-hw-probe=1` gate as the
preflight. Each window was checked against the upstream offset headers:
only defined registers, and none with read side effects (VM invalidation
semaphores, ACKs, DATA/INDEX pairs, SDMA scratch RAM, INT_STATUS, EDC
counters). A new read-only check is a new `n33peek` invocation; only a
new window needs a Kext.

### First MMIO peek (2026-10-17, Navi33Probe 0.0.5)

`n33peek --all` read every window with no writes. Each value below was
decoded against upstream `psp_v13_0.c`, `smu_v13_0.c`, `imu_v11_0.c`,
`mmhub_v3_0_2.c`, `nbio_v4_3.c` and the matching offset/sh_mask headers.

- PSP13: `C2PMSG_35` = `0x80000000`, the bootloader-ready value Linux
  waits for before loading sOS. `C2PMSG_81` and `C2PMSG_58` (sOS
  version) are 0, so no sOS is loaded. Many mailbox dwords read
  `0xffffffff` next to real values, which looks like PSP filtering rather
  than a dead block.
- SMU13: the driver response register `C2PMSG_90` and the DCN response
  register `C2PMSG_52` both read 1 (`SMU_RESP_OK`), so the SMU firmware is
  running.
- GC: `IMU_GFX_RESET_CTRL` = `0x10`. GRBM is out of reset, but hard, EA,
  UTCL2 and SDMA resets are still asserted; Linux waits for `0x1f`. All
  other GC, SDMA, RLC and GFXHUB registers read 0. The GC seg1 base is
  right, and GC is held in reset because no IMU firmware has run.
- MMHUB: the VBIOS put VRAM at MC `0x80_0000_0000`–`0x81_FFFF_FFFF`
  (`FB_LOCATION` `0x8000/0x81ff`), with the system aperture over the same
  range. NBIO memsize reads 8176 MiB.

### PSP13 sOS load stage (Navi33Probe 0.0.8, N33N ABI 9; proven 2026-10-18)

Proven in the Tahoe VM on 2026-10-18 at 10:37 (see the AGENTS.md
findings log). The plan gave state 14 with a host console. The load gave
state 20 (`sOS ALIVE`): 4 parts, 8 register writes, 25 ms. `C2PMSG_81`
then runs as a heartbeat, and `C2PMSG_58` holds `0x00330024`. DCN kept
scanning out throughout.

The first GPU write, approved by the user on 2026-10-18 with SSH as the
recovery path. `N33_SEL_PSP_LOAD` has two modes:

- **PLAN** (`n33psp --plan`, also in `run.sh`). It reads, checks and
  reports, and writes nothing. Gate: `navi33-hw-probe=1`.
- **RUN** (`n33psp --load ... --i-have-ssh`, by hand only). It runs the
  same plan, then the bootloader chain. Gates: `navi33-psp-load=1`, no
  `navi33-psp-off`, and no earlier attempt this boot.

The plan refuses unless all of these hold:

1. The firmware is byte-for-byte the pinned `psp_13_0_7_sos.bin`
   (SHA-256), and it holds exactly KDB, SPL, SYS_DRV, TOC and sOS.
2. `C2PMSG_81` = 0 (no sOS) and `C2PMSG_35` = `0x80000000` exactly.
3. MMHUB `FB_LOCATION` is posted. If DCN's copy is set, it agrees.
4. The GOP console lies inside BAR0, and an active HUBP primary surface
   sits at MC base + the console's BAR0 offset. This proves BAR0 offset
   == VRAM offset without writing anything. The console's physical base
   comes from `boot_args->Video`. 0.0.6 used `getConsoleInfo()`, which
   on the Mac returned the kernel's virtual mapping
   (`0xffffffa06a800000`), so its plan refused with state 11.
   - **No boot console (0.0.8):** in the Tahoe VM the guest's AMD GOP
     switched DCN off, so the guest boots without it (`rombar=0`) and
     `boot_args->Video` is empty. The user chose on 2026-10-18 to take
     the console from the host instead. `vm/prepare-card.sh` records the
     PC firmware's framebuffer from Linux's `boot_params` as a BAR0
     offset (1024×768×32 at offset 0, 3 MiB). It reaches the kext as
     `navi33-hostfb-offset` and `navi33-hostfb-bytes`, through
     `psp13load::hostConsole()`.
   - The host range is used only when there is no boot console and both
     boot-args are set. It must be page-aligned and inside BAR0, and
     the HUBP check above still applies to it unchanged.
   - `LastPspPlan` and the kext log say which source was used: `console
     boot`, `host` or `none`.
5. `fw_pri` at VRAM offset 240 MiB keeps 16 MiB clear of the console
   and of every active surface (each counted as at least 32 MiB). It
   must also lie inside VRAM.

A second write stage, the PSP KM ring (kext 0.0.9, N33N ABI 10), follows
this one; see "PSP13 KM ring stage" below. It is built and host-tested but
has not been run on the card.

RUN maps only the 1 MiB `fw_pri` sub-range of BAR0, uncached. It then
follows `psp_v13_0.c` step by step:

- For each of KDB, SPL (upstream sends the KDB bytes here) and SYS_DRV:
  1. Wait for `C2PMSG_35` == `0x80000000`.
  2. Zero the 1 MiB window, copy the part in, and read all of it back.
  3. Write `C2PMSG_36` = `0x800f0`, then the command to `C2PMSG_35`.
  4. Wait for `0x80000000` again.
- sOS: the same staging, then a 20 ms wait, then poll `C2PMSG_81`
  until it is non-zero, then read `C2PMSG_58`.

The writer refuses any register but `C2PMSG_35`/`36`. A read-back
mismatch stops the load before that part's command is sent.

Expected on success: state 20 (sOS ALIVE), 8 register writes, and a
non-zero `C2PMSG_81`. `C2PMSG_58` is probably `0x00330024`, the
container's sOS version, but that is not verified.

If the bootloader does not answer, or the display or SSH stop
responding, power the Mac off fully. The PSP can keep its state across
a warm restart: Linux's `soc21_need_reset_on_init()` checks `C2PMSG_81`
for exactly that reason. Only volatile GPU memory and registers are
written; the VBIOS flash is never touched.

## Actual hardware compute remains BLOCKED

A Queue1 ring and a ROM/register read do not confer ownership.
The following are required before a write-capable kernel backend can
safely bind to any physical RX7600:

- A genuine exclusive PCI/display/firmware authority and dedicated
  independently resettable recovery device. The sole GOP display GPU
  does not satisfy this. An `IOPCIDevice::open()` result alone is not
  proof that firmware, DCN scanout, PSP or other drivers have stopped.
- Firmware authenticated for Navi33 (MP0 PSP13, SDMA6.0.2, SMU13,
  GFX11), verified boot sequence, cache policy and clocks. Firmware
  images load into volatile GPU memory; this work does NOT call for
  flashing the GPU or changing VBIOS.
- A *real* reserved GPU-VRAM BO and a coherent BAR0 root page-table
  upload. An ordinary system-RAM IOVA is not an AMD GPU MC physical VRAM
  root. Confirm VMID1 page-table DEPTH/BLOCK/SYSTEM flags against the
  actual silicon and both MMHUB3/GFXHUB3 independently.
- Exclusive Queue1 ownership, confirmed SDMA0 engine status, correct
  Queue1 RB/doorbell/VMID registers, HDP and TLB invalidation, IOMMU
  unmap lifetime, IRQ/fence ordering, and an independently tested reset
  that can quiesce SDMA without corrupting display-owned Queue0.
- Actual hardware readback: copy a known 4096-byte pattern using
  **GPU-owned** SDMA6 packets, observe a GPU-written uncached FENCE
  cookie, verify Queue1 RPTR/WPTR, synchronize the device DMA mapping
  for CPU, check every destination byte matches, prove no CPU code
  performed the copy and record a hardware execution trace. No
  synthetic CI test can satisfy this requirement.
- Only **after** the above, implement GFX11 MEC/CP submission,
  shader compilation, gfx1102 RADV integration, Metal translations,
  IOSurface presentation and WindowServer acceleration.

## Build and safety

Host-only regression:
`make -C src/navi33-bringup clean host-test`

macOS isolated bridge syntax check with MacKernelSDK:
`make -C src/navi33-bringup MKSDK=/path/to/MacKernelSDK physical-io-check`

Neither command installs a driver or issues a GPU command. The
deliberate lack of a firmware/PCI/reset-aware physical writer is not a
CI bug and must not be bypassed by advertising `N33_CAP_HW_SDMA`,
`N33_CAP_HW_GFX` or `N33_CAP_METAL`.

**Do not load unsafe MMIO implementations into the only RX7600 that
currently drives the display.** Compile success is not macOS Metal
support and does not make any OpenCore Kext safe to install.

### PSP13 KM ring stage (Navi33Probe 0.0.9, N33N ABI 10; not yet run)

Runs after the sOS is alive. Modes `RING_PLAN` and `RING_RUN` of
`N33_SEL_PSP_LOAD`; `n33psp --ring-plan N` / `--ring N --i-have-ssh`.
`stop_after` N: 1 creates the KM ring, 2 also sends `LOAD_TOC`, 3 also
`SETUP_TMR`. Gate: `navi33-psp-ring=1`, no `navi33-psp-off`, once per boot;
`navi33-psp-off` kills both PSP stages.

The plan refuses unless the sOS is alive (`C2PMSG_81` != 0), `C2PMSG_64`
shows the response flag with a zero status, the ring registers
(`C2PMSG_67/69/70/71`) are all zero or already hold this ring (then the run
resumes it), and everything the sOS-load plan checks still holds: FB
location, console inside BAR0, a live HUBP scanout at the console's offset,
a window clear of the console and every surface by 16 MiB. The window is
2 MiB wide here: `fw_pri` at 240 MiB, then the 4 KiB ring, 4 KiB command
page and fence word at 241 MiB. The TMR address (FB base + 8128 MiB) must
fit in the framebuffer.

RUN zeroes the ring pages and reads them back before any register write,
then follows `psp_v13_0_ring_create()` (69, 70, 71, then 64, 20 ms, wait for
the response), then for each command writes the command buffer and a ring
frame, reads both back, advances `C2PMSG_67` and waits for the fence. A
non-zero response status or a missing fence stops the run. The register
writer accepts only `C2PMSG_64/67/69/70/71`.

### PSP13 IP firmware stage (Navi33Probe 0.0.10, N33N ABI 11; not yet run)

Runs after the ring and TMR exist. Modes `FW_PLAN` / `FW_RUN` of
`N33_SEL_PSP_LOAD`; `n33psp --ip-fw-plan START STOP` / `--ip-fw START STOP
--i-have-ssh`, with `--fwdir` pointing at the eight decompressed
linux-firmware files. It sends `LOAD_IP_FW` for records `[START, STOP)` of the
26-record list in `FIRMWARE_LOAD_DESIGN.md` through our ring; STOP 27 also
sends `AUTOLOAD_RLC`. The SMU image is never sent. Gate: `navi33-psp-fw=1`,
no `navi33-psp-off`, once per boot; `AUTOLOAD_RLC` also needs
`navi33-psp-autoload=1`. The register writer accepts only `C2PMSG_67`.
