# Cobalt64: notes for coding agents

Read this before working here. Keep it current: when a physical run,
decision or rule changes something below, update this file in the same
change. `CLAUDE.md` imports this file, so there is one source of truth.

## Repository map

- `src/navi48-bringup`: the working driver. RX 9070 XT (Navi 48, RDNA4,
  gfx1201): PSP/SMU/GMC/GFX/SDMA/MES bring-up, DCN 4.1 display, Metal
  through RADV. Its staged bring-up ladder is the template for every
  other GPU (see "Road to compute").
- `src/navi33-bringup`: RX 7600 (Navi 33, RDNA3, gfx1102) bring-up. A
  **separate** kext (`Navi33Probe`), read-only except for one gated write
  stage (the PSP sOS load), with its own user-client ABI ("N33N"). No
  shared Navi48 sources. Never reuse RDNA4 constants (GC12, PSP14, DCN4);
  Navi33 is GC 11.0.2, PSP 13.0.7, DCN 3.2.1.
- `src/dcn321`, `src/dcn41`: display-engine helpers per generation.
- Key Navi33 docs: `src/navi33-bringup/PHYSICAL_EXECUTION.md` (what is
  proven on hardware, what is still blocked), `ACCELERATION.md`,
  `DMA_QUEUE_BRINGUP.md`.
- The PSP sOS load: protocol and plan in `src/Navi33Psp13Loader.h`
  (host-tested), the only GPU writer in `src/Navi33DarwinPspLoad.cpp`,
  the tool in `tools/n33psp.c`.

## Navi33 hard rules

1. **The kext is read-only except the PSP sOS load.** That one stage
   (approved 2026-10-18) may write only `C2PMSG_35`/`C2PMSG_36` and its
   1 MiB `fw_pri` window in VRAM. It needs `navi33-psp-load=1`, is killed
   by `navi33-psp-off`, and runs at most once per boot. No other register
   write, firmware load or command submission. `Navi33HardwarePlan.h` and
   `tests/test_manifest.py` enforce this; do not weaken them.
2. **The RX 7600 is the only display GPU in the test Mac.** Every new
   write stage (SMU, IMU, RLC, and so on) needs the user's explicit
   approval to write to that card, plus a recovery path (SSH into the
   Mac, and a full power-off to reset the PSP). Ask first; never assume.
   The PSP sOS load was approved on 2026-10-18 with SSH as the recovery
   path. That approval covers that stage only.
3. **Every register offset comes from upstream Linux amdgpu headers**
   for the exact IP version, never from memory. See "Adding a register
   read".
4. **Never rely on a result you have not verified.** A zero read from a
   GC register is not "idle": the GC block reads 0 when powered down.
   All-ones means the block is not answering.

## Physical test loop

The Mac boots macOS 26.7.1 through OpenCore from a USB stick labelled
`OPENCORE`. That stick carries both the kext and the diagnostics.

- On Linux the stick is `/run/media/darren/OPENCORE/`; on the Mac,
  `/Volumes/OPENCORE/`.
- OpenCore injects `EFI/OC/Kexts/Navi33Probe.kext` from the stick. The
  `config.plist` entry is by path, so replacing the kext needs no config
  edit.
- `navi33/` on the stick holds the tools, `rx7600-ipd.bin`, the scripts
  from `src/navi33-bringup/usb/`, and the results files. It also holds
  `psp_13_0_7_sos.bin`, which CI does not ship: it is decompressed from
  this Linux machine's `/lib/firmware/amdgpu/psp_13_0_7_sos.bin.zst`, and
  the kext accepts only the file whose SHA-256 it pins.

**Build: GitHub Actions only.** The kext cannot be built on Linux.
1. Push to a `feature/navi33-*` branch; `.github/workflows/navi33-host.yml`
   runs host tests and builds on `macos-15-intel` and `macos-26-intel`.
2. The Mac uses the **macos-26-intel** build. Its artifact
   `navi33-usb-macos-26-intel` is the ready-to-copy stick layout:
   `EFI/OC/Kexts/Navi33Probe.kext` plus `navi33/`.
3. Before replacing anything on the stick, copy the old kext, `run.sh`
   and tools into `navi33/backup-<oldversion>/`. Then copy the artifact,
   `sync`, and check the copy matches the artifact (`diff -r`/`cmp`).
4. To prove which build the Mac loaded, compare the kext UUID that
   `kmutil showloaded` prints in the results file with the artifact's
   Mach-O `LC_UUID`.

**Run and results.**
- `run.sh` runs every diagnostic in one pass: `n33query`,
  `n33compute --rom/--probe/--queue`, `n33vram`, `n33peek --all`,
  `n33psp --plan` (read-only), ioreg and this boot's kernel log. It never
  runs the PSP load; neither does the boot autorun.
- It writes `navi33/results-YYYYMMDD-HHMMSS-{manual|boot}.txt` (results
  before kext 0.0.5 have no suffix).
- Optional boot autorun: `sudo sh /Volumes/OPENCORE/navi33/install-autorun.sh`
  installs a LaunchDaemon that runs `run.sh` once per boot from the
  pinned stick, then chimes. Its log is
  `/var/log/cobalt64-navi33-autorun.log`. If macOS privacy blocks it, the
  log says `BLOCKED`; grant Full Disk Access to
  `/usr/local/libexec/cobalt64/n33autorun`.
- **A boot that panics leaves no results file.** The Linux install on the
  same PC can still read the panic: macOS keeps it in NVRAM
  (`/sys/firmware/efi/efivars/AAPL,PanicInfo0000`, `0001`, ...) until the
  next good macOS boot. Drop each file's 4-byte attribute header,
  concatenate them, and unpack every 7 bytes (little-endian) into eight
  7-bit ASCII characters.
- **The user hands results over by pasting only a results file path.**
  Read it, compare it with the previous results file, and interpret it.
  Then record any new hardware fact in the findings log below.

**Boot-args on the Mac** (in the OpenCore config): `navi33-probe=1
navi33-client=1 navi33-hw-probe=1`, plus kill switches `navi33-dma-off=1
navi33-dma-stage-off=1 navi33-fb-off=1 navi33-psp-off=1 navi33-sdma-off=1`.
- Only `n33compute --rom`, `n33peek` and `n33psp --plan` (all gated by
  `navi33-hw-probe=1`) return hardware data.
- The PSP load (`n33psp --load ... --i-have-ssh`, run by hand over SSH)
  also needs `navi33-psp-load=1` **and** `navi33-psp-off=1` removed.
  Restore the kill switch after the attempt.
- `--probe` reports "no ROM": the Mac has no `ATY,bin_image` property.
  That is why `rx7600-ipd.bin` (built by `tools/ipd-from-capture.py`) is
  passed explicitly.
- `--queue` and `n33vram` report "disabled".

**Every kext change bumps the version** in `src/navi33-bringup/Info.plist`,
`bootstrap/build_hw_probe_efi.py` and `bootstrap/tests/test_hw_probe_efi.py`,
so a results file shows which build actually loaded.

**Every N33N ABI change** bumps `N33_ABI_VERSION` and the pinned values
in `tests/test_native_client.cpp` and `tests/test_sdma6_queue_census.cpp`.
Old tools and a new kext refuse each other at HELLO, so ship all tools
from the same CI run.

## VM test loop (being set up, 2026-10-18)

The "Mac" is this same PC (Ryzen 5 3600, no integrated GPU, 30 GiB RAM),
booted from the stick instead of Ubuntu. The user also runs macOS in a VM
on Ubuntu. The goal is to pass the RX 7600 into that VM, so every byte of
output survives a hang and Linux can inspect a stuck guest.

- **The VM:** the user's ultimate-macOS-KVM install at `~/macos tahoe`
  (`HDD.qcow2`). It runs macOS 26.7.1 (25G241) on the same kernel build
  as bare metal. Guest SSH is the `macos-vm` alias (127.0.0.1:2222, user
  `darren`, key login). Guest sudo is passwordless
  (`/etc/sudoers.d/cobalt64`, set by the user on 2026-10-18), which
  `guest-run.sh` relies on.
- **Do not touch the user's own VM files:** `boot/OpenCore.qcow2`,
  `ovmf/OVMF_VARS.fd`, `boot.sh` and `boot-reims-vgpu.sh` (their separate
  reims-vgpu project). Ours live in `~/macos tahoe/navi33/`.
- **Scripts** (in `src/navi33-bringup/vm/`):
  - `make-opencore.sh ARTIFACT_DIR [--oc RELEASE_DIR] [--psp-load]`
    builds `navi33/OpenCore-navi33.qcow2` from the VM's OpenCore disk
    plus the artifact's kext, and writes `OpenCore-navi33.txt` (kext UUID,
    OpenCore version, boot-args). It sets `-v debug=0x108 serial=3`, the
    same `navi33-*` flags as the stick, and `navi33-psp-off=1` unless
    `--psp-load` is given. It also sends OpenCore's log to serial.
    `--hostfb /var/lib/cobalt64-navi33/hostfb.txt` adds the host console
    boot-args (kext 0.0.8+) and records that boot's ID in the manifest.
    **Always pass `--oc`** with an unpacked OpenCorePkg release (1.0.8 in
    use). The VM's own OpenCore 1.0.4 cannot inject kexts on macOS 26.
  - `run-vm.sh [--no-gpu] [--snapshot] [--check]` boots the VM. Each run
    gets `navi33/runs/<stamp>/` (with `runs/latest` pointing at it):
    `serial.log`, `qemu.log`, `cmdline.txt`, `qmp.sock` and `hmp.sock`.
    gdb attaches on 127.0.0.1:1234. A guest reboot stops the VM.
    It hides the card's ROM (`rombar=0`) unless `ROMBAR=1`: the guest's
    AMD GOP switches DCN off. It refuses an OpenCore disk whose hostfb
    record is from another boot.
  - `guest-run.sh ARTIFACT_DIR` copies the tools and the pinned firmware
    to the guest's `~/navi33`, runs `run.sh` there, and copies the results
    file into the run directory.
  - `host-setup.sh` is a one-time sudo setup, undone with `--remove`. It
    installs openssh-server and tmux, adds the GRUB entry
    `cobalt64-navi33-vm`, an unlimited memlock limit and a vfio udev rule.
  - `reboot-to-vm.sh` boots that GRUB entry once. The entry is headless:
    it blacklists amdgpu and snd_hda_intel and gives 1002:7480 and
    1002:ab30 to vfio-pci.
  - `host-peek.py` (root, no VM running) reads the PSP mailbox and the
    plan's DCN registers straight from BAR5 on the host, read-only.
  - `prepare-card.sh` (root) is installed by `host-setup.sh` as
    `cobalt64-navi33-card.service`. It runs only in the VM boot. It shrinks
    BAR0 to 256 MiB (the firmware gives 8 GiB, which macOS cannot place)
    and copies the ROM to `/var/lib/cobalt64-navi33/rx7600-rom.bin`. It
    records the PC firmware's console (`boot_params` screen_info) as a
    BAR0 offset in `/var/lib/cobalt64-navi33/hostfb.txt`, stamped with
    the boot ID, before the resize. It also stops VFIO from resetting the
    card: it clears `reset_method` and
    takes 09:00.1 off vfio-pci, so the VM runs without the HDMI audio
    function. It touches the card only while it is on vfio-pci and no VM
    holds it.
- **Cache that survives restarts:** `~/macos tahoe/navi33/cache/` holds
  `artifact-0.0.7` (the kext, recovered from the OpenCore disk, plus the
  guest's tools) and `OpenCore-1.0.8-RELEASE` (zip SHA-256
  `2011e8b7…be6d27b`). `/tmp` is cleared at every boot.
- **`gh` cannot download artifacts over SSH:** its token sits in the
  desktop keyring. Download while logged in at the desktop, or use
  `GH_TOKEN`.
- **Never take the card away from a running amdgpu.** amdgpu loads the
  PSP sOS itself, so the card would no longer be in the bootloader-ready
  state. `run-vm.sh` refuses unless 09:00.0 is already on vfio-pci.
- **Headless means the user drives it from their Arch Linux laptop:**
  `ssh darren@192.168.100.24`, inside tmux. Recovery: `sudo poweroff`
  over SSH, then power on, which boots normal Ubuntu.
- **Status (2026-10-18):**
  - The `--no-gpu` tests passed: guest SSH, the serial capture, and
    `guest-run.sh` from tools in to results out.
  - The first passthrough run (see the findings log) showed the PSP still
    bootloader-ready after the VFIO reset. It also showed three blockers:
    OpenCore 1.0.4 could not inject the kext, BAR0 was 8 GiB, and the
    guest had no GOP.
  - Fixed so far: OpenCore 1.0.8 and the BAR0 service. With both, the
    second run matched bare metal in every `n33peek` value.
  - The guest's AMD GOP switches DCN off (fourth run), so the VM runs
    with `rombar=0`. The PSP plan then reaches `NO_CONSOLE`, exactly as
    on bare metal. Kext 0.0.8 (host console) fixed that: PLAN_OK, then
    the PSP sOS load succeeded in the VM on 2026-10-18.
  - `run-vm.sh` takes `BOOT_DISK=` (for example `navi33/gop-diag.qcow2`),
    `VFIO_OPTS=` (for example `,x-vga=on`), and `OVMF_CODE=` with
    `OVMF_VARS=` (a writable copy).
  - Every run is read-only (`navi33-psp-off=1`) unless the user approves
    a write stage. The PSP load was approved and done on 2026-10-18.

## RX 7600 hardware facts

PCI 1002:7480 rev cf. BAR0 256 MiB (VRAM window), BAR2 2 MiB, BAR5 1 MiB
(registers). IP Discovery from Linux: `tests/fixtures/rx7600-linux-ipd.txt`.

| IP | Version | Register bases (dwords) |
|---|---|---|
| GC (incl. SDMA0/1 6.0.2) | 11.0.2 | seg0 0x1260, seg1 0xA000 |
| MP0 (PSP), MP1 (SMU) | 13.0.7 | seg0 0x16000 |
| MMHUB | 3.0.2 | seg0 0x1A000 |
| NBIF | 4.3.1 | seg2 0xD20 (seg0 is 0) |
| DCN | 3.2.1 | seg0 0x12, seg2 0x34C0 |

BAR5 byte offset = (base + register dword) × 4. Example: PSP
`C2PMSG_35` (0x63) is at 0x5818C.

VRAM: 8 GiB at GPU MC address `0x80_0000_0000`. The VBIOS programs this
in MMHUB `FB_LOCATION` before macOS starts. NBIO reports 8176 MiB.

PSP firmware (`psp_13_0_7_sos.bin`, linux-firmware 20260915 git
1522c78a, 248496 bytes, SHA-256 `94db505f…a26fa8`, sOS `0x00330024`).
These facts were checked against the real file, and the old offline
parser got two of them wrong:
- It is a v2.0 container. `header_size_bytes` is 36 (the fixed header
  only); the descriptors follow it, and the ucode array starts at 256.
- Descriptor offsets are relative to `ucode_array_offset`, as in upstream
  `parse_sos_bin_descriptor`.
- It holds SYS_DRV 141200 B, sOS 99184 B, TOC 2304 B, KDB 4880 B and
  SPL 672 B. There is no SOC/INTF/DBG/RAS/IPKEYMGR/SPDM driver.
- The header `crc32` (`0xe28dd196`) is not a CRC-32 of the payload
  (`0xcbad41a4`). Linux never checks it.
- Upstream `psp_v13_0_bootloader_load_spl()` sends the **KDB** bytes with
  `PSP_BL__LOAD_TOS_SPL_TABLE`; PSP 14 sends the SPL. Every Linux RX 7600
  boot does this, so the Navi33 chain copies it on purpose.

## Findings log (append after every physical run)

- **2026-10-17, kext 0.0.2:** HELLO worked, but every struct selector
  returned `0xe00002c2`. IOKit always passes non-null scalar arrays;
  scalar presence must come from the counts. Fixed in 0.0.3.
- **2026-10-17, kext 0.0.3:** first live read-only preflight.
  - Reason 8: PSP sOS not loaded (`C2PMSG_81` = 0). Expected, because
    macOS has no driver for 1002:7480 and only the VBIOS/GOP has run.
  - 8 BAR5 reads, none all-ones.
  - MMHUB `VM_CONTEXT0_CNTL` = `0x01fffe00`, a plausible power-on value.
    This was the first live proof that the IP Discovery offsets land on
    real registers.
  - GFXHUB `VM_CONTEXT0_CNTL` and SDMA0 `RB_CNTL/RPTR/WPTR` all read 0:
    the GC block is most likely powered down.
- **Kext 0.0.4** (adds `C2PMSG_35`, `SDMA0_STATUS_REG` and
  `UCODE_CHECKSUM` to the `Navi33,LastHwPreflight` ioreg string) was
  staged but never booted.
- **2026-10-17, kext 0.0.5 (N33N ABI 8):** first `n33peek --all`
  (`results-20261017-221114-manual.txt`). The loaded UUID
  `6AFC2D34-0228-3079-8F8B-AD71FADB9B5F` matches the artifact. This was a
  manual run, so whether the boot autorun can read the stick is still
  unknown. The preflight is unchanged from 0.0.3 (reason 8, now 9 reads).
  - **PSP bootloader ready:** `C2PMSG_35` = `0x80000000` exactly, which is
    the value `psp_v13_0_wait_for_bootloader` waits for. `C2PMSG_33` is
    also `0x80000000`. No sOS: `C2PMSG_81` = 0, `C2PMSG_58` (sOS
    version) = 0, and the ring registers `C2PMSG_64/67/69-71` = 0.
  - About half the PSP `C2PMSG_32..127` dwords read `0xffffffff`,
    interleaved with real values, so the block is answering; those words
    are probably filtered by the PSP (not verified). Non-zero words with
    no known meaning: `C2PMSG_59..62` = `00330f1c 00000004 83f00f80
    0d1102f4`, `C2PMSG_89..91` = `88f36431 139bed48 0d330088`,
    `C2PMSG_115` and `C2PMSG_118` = `0x80000000`.
  - **SMU firmware running:** the driver mailbox response `C2PMSG_90` = 1
    (`SMU_RESP_OK`). The DCN clock-manager response `C2PMSG_52`
    (`DAL_RESP_REG` 0x16274) = 1. Message and argument registers are 0.
  - **GC held in reset, not mis-addressed:** `IMU_GFX_RESET_CTRL` (GC
    seg1 0x40bc) = `0x10`, the first non-zero GC read. Only `GRBM_RESETB`
    is released. `HARD/EA/UTCL2/SDMA_RESETB` are asserted; Linux waits for
    `0x1f` once the IMU has started. Everything else reads 0: `SDMA0_ID`
    and `VERSION`, GRBM, CP, RLC (including `RLC_BOOTLOAD_STATUS`), the
    other IMU registers and all of GFXHUB.
  - **Memory map set by the VBIOS:** `MMMC_VM_FB_LOCATION_BASE/TOP` =
    `0x8000/0x81ff`, so VRAM is at GPU MC `0x80_0000_0000`–`0x81_FFFF_FFFF`.
    The system aperture covers the same range and `FB_OFFSET` is 0.
    `RCC_CONFIG_MEMSIZE` = `0x1ff0` (8176 MiB). All 16 MMHUB contexts read
    `0x01fffe00` (none enabled) and `MMVM_L2_CNTL` = `0x00080602`.
  - `run.sh` bugs, fixed in `usb/run.sh` (they need a CI build to reach
    the stick):
    - `n33vram` ran without `--read`, so it only printed usage.
    - `log show` missed the kext's own IOLog lines. The `dmesg` used by
      0.0.3 caught them.
  - **`panic.txt` on the stick:**
    - The panic: `"The global payload bytes pointer is NULL"
      @apfs_vfsops.c:2893` in `apfs_vfsop_mountroot`, 3.3 s into a boot
      with the same boot-args. No Navi33Probe frame.
    - At boot the kext reads only PCI config space and maps no BAR.
    - The same message and backtrace was reported on a Ryzen + OpenCore
      machine without this kext (forum.amd-osx.com, Monterey 12.2).
    - Which kext build that boot loaded is unknown. The 22:04:47 boot
      was clean.
    - Treat it as unrelated unless it recurs. If it does, boot once with
      Navi33Probe disabled.
- **2026-10-18:** the fixed `run.sh` (`n33vram --read`, `dmesg`) was
  copied onto the stick next to the 0.0.5 kext.
- **Kext 0.0.6 (N33N ABI 9, PSP sOS load stage)** is staged on the
  stick, from CI run 39081053858. Expected UUID:
  `4B6769B0-5973-3370-A0D4-6E979CB327C5`.
  - The stick also has the pinned `psp_13_0_7_sos.bin`. The 0.0.5 kext,
    tools and scripts are in `navi33/backup-0.0.5/`.
  - The user chose to skip a separate plan-only boot. On 2026-10-18 the
    stick's `config.plist` boot-args got `navi33-psp-load=1` in place of
    `navi33-psp-off=1`; the original is `config.plist.bak-20261018-psp-off`.
    `n33psp --load` still runs the plan first and refuses unless it
    passes. Restore the backup after the attempt.
  - Plan output to check before any load: state 14 (`PLAN_OK`), console
    inside BAR0, an active HUBP surface at the console's offset, and
    `fw_pri` MC `0x80_0F00_0000`.
- **2026-10-18, kext 0.0.6 (N33N ABI 9): first `n33psp --load`, refused
  by the plan, no write** (`psp-load-20261018-081128.txt`,
  `results-20261018-081136-manual.txt`). The loaded UUID `4B6769B0…`
  matches the artifact. The boot-args had `navi33-psp-load=1` and no
  `navi33-psp-off`, so the write gate was open (flags `0x19`:
  fw-pinned, dcn-fb-match, gate open).
  - State 11 (`NO_CONSOLE`): `getConsoleInfo()` returned
    `0xffffffa06a800000`, a kernel virtual address, not a physical one.
    Most likely the framebuffer that owns the console by then has
    replaced `PE_state.video` with its own mapping (not verified).
    Navi48 reads it from `stagePSP()` at boot and saw a physical value.
    The console size, 8100 KiB (1920×1080×4), was right. "pipes 0x0"
    only means the plan stopped before the HUBP loop.
  - The other inputs looked right, but the checks after the console
    check never ran: `C2PMSG_35` = `0x80000000`, `C2PMSG_81` = 0, MMHUB
    and DCN `FB_LOCATION` `0x8000-0x81ff`, HUBP0 surface MC
    `0x80_0000_0000` (VRAM offset 0). BAR0 is at `0x8_3000_0000`.
  - `n33peek --all` is byte-for-byte identical to the 0.0.5 run, so the
    PSP is in the same bootloader-ready state as on 2026-10-17.
  - Fixed in 0.0.7: the console comes from `PE_state.bootArgs->Video`,
    which keeps the physical base, through `psp13load::bootConsole()`.
    That helper rejects anything at or above 2^52.
- **Kext 0.0.7 (N33N ABI 9)** is staged on the stick, from CI run
  39082377048. Expected UUID: `6CC36689-A8D7-33A4-B0FA-9638079370EA`.
  - The 0.0.6 kext and tools are in `navi33/backup-0.0.6/`.
  - The boot-args still have `navi33-psp-load=1`. Restore
    `config.plist.bak-20261018-psp-off` after the next load attempt.
  - The plan's console line should now show a physical address inside
    BAR0 and `= vram+0x0`.
- **2026-10-18, kext 0.0.7 staged: macOS did not boot.** No results
  file. The panic, decoded from NVRAM on Linux, is the same one as the
  stick's `panic.txt`: `"The global payload bytes pointer is NULL"
  @apfs_vfsops.c:2893` in `apfs_vfsop_mountroot`, the same kernel UUID
  `1291D44A…`, 3.2 s after boot, and no Navi33Probe frame.
  - The two panics had different boot-args (`navi33-psp-off=1` the first
    time, `navi33-psp-load=1` this time). The 0.0.6 boot between them,
    with `navi33-psp-load=1`, was clean. 0.0.7 changes only `pspLoad()`,
    which runs only when `n33psp --load` is called.
  - Linux on this PC has no APFS driver, so it cannot have changed the
    macOS disk (`sda2`).
  - Web reports tie this panic to booting Tahoe through OpenCore (macOS
    expecting a root hash the boot path did not hand over). The cause is
    not verified here.
  - Next: power off fully and boot again. If it panics twice in a row,
    boot once with Navi33Probe disabled in `config.plist`.
- **2026-10-18, from the Linux journal: a warm restart probably resets the
  PSP.** The Ubuntu boot before the 08:11 macOS run ended with a restart
  (`systemd-reboot`) at 08:05:49, not a power-off. amdgpu had loaded the
  sOS during that boot. Yet macOS saw `C2PMSG_35` = `0x80000000` and
  `C2PMSG_81` = 0. Unless the PC was also switched off in those 6
  minutes, a warm restart from Ubuntu leaves the PSP bootloader-ready.
  This was seen only after amdgpu's own clean shutdown, so after a PSP
  write attempt still power off.
- **2026-10-18, first RX 7600 passthrough to the Tahoe VM** (run
  `~/macos tahoe/navi33/runs/20261018-090741`, kext 0.0.7, gate closed).
  The headless boot came from a warm restart of normal Ubuntu. Both
  functions were on vfio-pci, and amdgpu never loaded. VFIO bus-reset the
  card when QEMU started (and again when it stopped).
  - **The kext never loaded.** OpenCore 1.0.4 refused Navi33Probe,
    WhateverGreen, AppleALC and BrcmPatchRAM3 with `Prelinked injection
    ... - Invalid Parameter`; only Lilu loaded. Upstream fixed kext
    injection for macOS 26 in 1.0.5. The earlier `--no-gpu` "NOT loaded"
    was the same failure: there OpenCore logged to the QXL screen, not
    to serial.
  - **The PSP survives the VFIO reset.** The read below went through
    QEMU's monitor (`xp`) on the guest's BAR5 (`0x8000_0000`), reading
    the same registers `n33peek` reads (saved as `hmp-reads.txt` in the
    run directory).
    - Same as bare metal: `C2PMSG_33`/`35` = `0x80000000`,
      `C2PMSG_58`/`64`/`81` = 0, `C2PMSG_59` = `0x00330f1c`,
      `C2PMSG_89..91`, `C2PMSG_115`/`118`, and `IMU_GFX_RESET_CTRL` =
      `0x10`.
    - Different: `C2PMSG_60..62` = `3200000f 8001b000 0000da5c` (bare
      metal: `00000004 83f00f80 0d1102f4`). Their meaning is unknown.
  - **BAR0 is 8 GiB here.** The PC's firmware assigns 8 GiB
    (`0x7c00000000`); bare-metal macOS saw 256 MiB. OVMF put BAR0 at
    `0x8_0000_0000` and BAR5 at `0x8000_0000`. macOS then moved BAR0 to
    `0x8012400000000000`, an address it cannot use, and BAR2 to
    `0x9000_0000`. The card supports 256 MiB to 8 GiB
    (`resource0_resize` = `0x3f00`).
  - **No GOP in the guest:** `OCC: Missing compatible GOP`, then the
    kernel printed `initialize_screen: No video - forcing serial mode`.
    So `boot_args->Video` is empty and the PSP plan would refuse with
    `NO_CONSOLE`.
- **2026-10-18, second passthrough run** (`runs/20261018-093427`), with
  OpenCore 1.0.8 and BAR0 shrunk to 256 MiB by
  `cobalt64-navi33-card.service`. The host now has BAR0 at
  `0x8_3000_0000`, the same address bare-metal macOS reported; the guest
  put it at `0xA000_0000`.
  - **The kext loaded** (UUID `6CC36689…`), and `guest-run.sh` produced
    `results-20261018-093615-guest.txt`.
  - **`n33peek --all` matches every value recorded for bare metal:**
    - PSP: `C2PMSG_33/35` = `0x80000000`, `C2PMSG_58/64-71/81` = 0,
      `C2PMSG_59`, `89..91`, `115`, `118`.
    - SMU: `C2PMSG_90` = 1 and DAL `C2PMSG_52` = 1.
    - GC: everything 0 except `IMU_GFX_RESET_CTRL` = `0x10`.
    - MMHUB: `FB_LOCATION` `0x8000/0x81ff`, the system aperture over the
      same range, all 16 contexts `0x01fffe00`, `MMVM_L2_CNTL`
      `0x00080602`.
    - NBIO `MEMSIZE` `0x1ff0`.
    - Only `C2PMSG_60..62` differ, and `C2PMSG_60` also changed between
      two VM boots (`3200000f`, then `9800000f`), so they look like a
      live status word.
  - **`n33psp --plan` stopped at state 7** (`READ_FAILED`: one of its 14
    reads returned all-ones). Its PSP and MMHUB registers read fine in
    `n33peek`, so the all-ones read is most likely a DCN/HUBP register:
    no GOP ran after the VFIO reset, so the display engine was never set
    up. The zeros it printed are only an unfilled report.
  - **The ROM is complete:** a 57856 B x86 VBIOS, then a 54272 B
    EFI-compressed x64 boot-service driver (the AMD GOP), 112128 B in
    all.
  - **What the UEFI shell showed** (`vm/make-gop-diag.sh`): the firmware
    loads `AMD GOP X64 Rel Driver Rev.3.7.13` from that ROM, and it
    manages a controller named `NAVI33`. It creates no child and no
    `GraphicsOutput`, with or without `x-vga=on`. Loading a second copy
    with `loadpcirom` hung `connect -r`.
    - It is not the monitor: with the HDMI monitor awake it said "no
      HDMI input detected". Ubuntu's OVMF (`OVMF_CODE_4M.fd`) gave the
      same result.
    - The suspected cause: the RX 7600 is the PC's boot GPU. The PC
      firmware sets it up at power-on, and VFIO bus-resets it before the
      guest starts; after that the AMD GOP cannot light any output.
      Passthrough setups that work boot the host on another GPU. Not
      verified.
    - Next test: no VFIO reset at all. `prepare-card.sh` now clears
      09:00.0's `reset_method` (the kernel logs "All device reset
      methods disabled by user") and takes 09:00.1 off vfio-pci, so no
      bus reset can happen. This needs a fresh boot, because the
      firmware's setup of the card is lost after the first reset.
  - After the GOP had run, the PSP was still bootloader-ready, so the
    GOP does not load an sOS.
- **2026-10-18, third passthrough run, no VFIO reset**
  (`runs/20261018-101022`, fresh boot at 10:08 from a warm restart).
  - At boot `cobalt64-navi33-card.service` resized BAR0 (8192 to 256
    MiB), disabled the reset methods and took 09:00.1 off vfio-pci.
    QEMU logged `vfio: Cannot reset device 0000:09:00.0, depends on group
    19 which is not owned`, and the host logged no `resetting`. So the
    guest got the card with no reset since power-on.
  - **Still no GOP** (`OCC: Missing compatible GOP`, `No video - forcing
    serial mode`). So the reset was not the cause, or not the whole of
    it.
  - `n33peek` matches the second run, and the PSP is bootloader-ready.
    `n33psp --plan` is again state 7.
  - **The display engine is not answering.** All 18 DCN registers the
    plan reads (`Navi33Dcn321.h`: `DCN_VM_FB_LOCATION_BASE/TOP`,
    `HUBPREQ0-3` pitch and surface address, `OTG0-3_STATUS_FRAME_COUNT`)
    read `0xffffffff` through the QEMU monitor (`hmp-dcn-reads.txt`). On
    bare metal (0.0.6) they read `0x8000/0x81ff`, with HUBP0 at VRAM
    offset 0.
  - **The monitor lost its picture exactly when the VM started** (the
    user watched it). So the host's vfio-pci bind and BAR0 resize leave
    the display running, and the guest kills it. QEMU does not reset the
    card, so the most likely culprit is the guest's AMD GOP failing
    part-way; this is not verified.
  - After that VM, `vm/host-peek.py` (root, read-only `resource5` mmap,
    no VM running) read the same PSP values from the host as the guest
    did, and all-ones for every DCN register.
- **2026-10-18, fourth passthrough run, guest GOP disabled**
  (`runs/20261018-101934`, `VFIO_OPTS=,rombar=0`, fresh boot at 10:17
  from a warm restart, no VFIO reset).
  - **Before the VM, the display was alive** (`host-peek.py` from the
    host, `navi33/host/host-peek-*-before-vm.txt`):
    - `DCN_VM_FB_LOCATION` = `0x8000/0x81ff`.
    - HUBP0 surface MC = `0x80_0000_0000` (VRAM offset 0); its pitch
      register reads `0x3ff`.
    - `OTG0_STATUS_FRAME_COUNT` went from `0x1c7d` to `0x1cb9` in 1 s
      (60 Hz). Pipes 1 to 3 read 0. The PSP was bootloader-ready.
  - **With `rombar=0` it stays alive inside the guest:** same FB
    location and HUBP0 surface, and OTG0 still counts. So the guest's
    AMD GOP, run from the ROM BAR, is what switched DCN off in the
    earlier runs. Without the ROM the guest has no GOP at all.
  - **`n33psp --plan` reached state 11 (`NO_CONSOLE`)**, flags `0x09`
    (fw-pinned, dcn-fb-match). That is the same state as bare-metal
    0.0.6. All other inputs match bare metal:
    - `C2PMSG_35` = `0x80000000`, `C2PMSG_81` = 0.
    - MMHUB `FB_LOCATION` `0x8000-0x81ff`, DCN FB base `0x8000`.
    - HUBP0 surface MC `0x80_0000_0000`.
    - BAR0 is at guest `0xA000_0000` + 256 MiB.
  - The only input missing in the VM is a GOP console inside BAR0.
- **2026-10-18, decision:** the user chose to do the PSP load in the VM,
  with the console taken from the host. Kext 0.0.8 adds this
  (`psp13load::hostConsole()`, boot-args `navi33-hostfb-offset` and
  `-bytes`).
  - The host tests show that a boot console always wins, that both
    boot-args are needed, and that the range must be page-aligned and
    inside BAR0.
  - A wrong offset still fails the HUBP check (`APERTURE_MISMATCH`).
  - A mutant without the "boot console wins" rule fails
    `test_psp13_loader`.
  - This boot's record is BAR0+0x0, 3 MiB, 1024×768×32. It was made by
    hand from the boot-time BAR0 (`0x7c00000000`) after the service had
    already resized it.
- **2026-10-18, kext 0.0.8 (N33N ABI 9) in the VM: the first `PLAN_OK`**
  (`runs/20261018-103317`, `results-20261018-103339-guest.txt`).
  - Build: CI run 39090397732, commit `0877148`. The loaded UUID
    `6E159736-37E7-3574-9EAE-7BB2324806BA` matches the artifact (cached
    in `navi33/cache/artifact-0.0.8`).
  - The gate was closed (`navi33-psp-off=1`). The boot-args had
    `navi33-hostfb-offset=0x0 navi33-hostfb-bytes=0x300000`, and the VM
    ran with `rombar=0`.
  - `n33psp --plan`: state 14, flags `0x0f` (fw-pinned, console-found,
    aperture-match, dcn-fb-match), `LastPspPlan` ... `console host`.
    - PSP: `C2PMSG_35` `80000000`, `C2PMSG_81` 0.
    - MMHUB FB `8000-81ff`, DCN FB base `8000`.
    - HUBP0 surface MC `0x80_0000_0000` (active, the only pipe).
    - BAR0 is at guest `0xa0000000` + 256 MiB. The console is
      `0xa0000000` (3072 KiB) = vram+0x0.
    - `fw_pri` is vram+0xf000000 = MC `0x80_0F00_0000` (`C2PMSG_36`
      `0x800f0`), exactly the values to check before a load.
  - The kext's IOLog line (`Navi33Probe: psp: plan state 14 ...`)
    reached `serial.log`.
- **2026-10-18 10:37, kext 0.0.8 in the VM: the PSP sOS load
  succeeded. This was the first GPU write.** Run `runs/20261018-103646`
  (`psp-load-20261018-103729.txt`, `results-20261018-103756-guest.txt`,
  host reads in `navi33/host/`). The user approved it after the PLAN_OK
  run.
  - Setup: the gate was open (`navi33-psp-load=1`, no `navi33-psp-off`),
    with the host console boot-args and `rombar=0`. The firmware's
    SHA-256 was `94db505f…a26fa8`. The plan rerun just before the load
    gave state 14, flags `0x1f`.
  - The load gave state 20 (`sOS ALIVE`), flags `0x7f`: 4 parts (KDB,
    SPL, SYS_DRV, sOS), **8 register writes**, 25 ms. `C2PMSG_35` was
    `0x80000000` after every part, `C2PMSG_81` = `433bbc95`, and the sOS
    version is `00330024`, matching the firmware file. The kext logged
    `LOAD starts` and `LOAD state 20` to `serial.log`.
  - After the load:
    - `C2PMSG_81` keeps counting (`433bcb6a`, `433bcbb7`, ... `433bd6d9`
      read from the host after the VM stopped), so it is a live
      heartbeat.
    - `C2PMSG_58` = `00330024`, and `C2PMSG_64` = `0x80000000`, the bit
      Linux waits for before it creates the KM ring.
    - `C2PMSG_33` went from `0x80000000` to 0, and `C2PMSG_60..63` =
      `0 0 0 2`.
    - The preflight now gives PSP state 6 (running), and its reason
      moved from 8 to 9 (GC not answering). GC is still in reset
      (`IMU_GFX_RESET_CTRL` = `0x10`), as expected.
    - `n33psp --plan` now refuses with state 8 (sOS already alive).
    - DCN kept scanning out the whole time (OTG0 at 60 Hz).
  - The gate was closed again right after (`navi33-psp-off=1` is back in
    `OpenCore-navi33.qcow2`). The sOS stays loaded until a full
    power-off, so every later run starts with the sOS alive.

## Adding a register read

Prefer a new `n33peek` invocation over a new kext.
- `N33_SEL_MMIO_PEEK` reads up to 64 dwords from one window in
  `src/navi33-bringup/src/Navi33PeekWindows.h`.
- If a register is already inside a window, no kext change is needed:
  `n33peek --rom rx7600-ipd.bin NAME[:first[:count]]`.

To add a window (needs a kext rebuild):
1. Fetch the upstream header for the exact IP version. Use
   `git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/plain/drivers/gpu/drm/amd/include/asic_reg/...`;
   raw.githubusercontent.com rate-limits large files. The headers in use
   are `gc_11_0_0_offset.h`, `mp_13_0_2_offset.h`, `mmhub_3_0_2_offset.h`
   and `nbio_4_3_0_offset.h`.
2. Check every dword of the window: it must be a defined register at the
   right `BASE_IDX`, with no holes.
3. Exclude registers whose read has a side effect:
   - `*_INVALIDATE_ENG*_SEM`: a read **acquires** a VM invalidation
     semaphore.
   - `*_ACK`.
   - DATA/INDEX pairs (including MM_INDEX/MM_DATA at the start of BAR5).
   - `SCRATCH_RAM_*`, `UCODE_ADDR/DATA`.
   - `INT_STATUS`, `EDC_COUNTER*`, `QUEUE_RESET_REQ`, FIFOs.
4. Offsets are version-specific. MMHUB 3.0.0 differs from 3.0.2 (for
   example `VM_CONTEXT0_CNTL` 0x0740 vs 0x06c0); the kext refuses any
   other version.
5. Add the window to `tests/test_mmio_peek.cpp`: an expected RX 7600
   offset, plus a hazard assertion if a dangerous register sits nearby.

## Verification

```sh
make -C src/navi33-bringup clean host-test HOST_CXX=g++
make -C src/navi33-bringup clean host-test HOST_CXX=clang++ \
  HOST_FLAGS="-std=c++17 -Wall -Wextra -Werror -pedantic -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer"
make -C src/navi33-bringup/display clean host-test HOST_CXX=g++
python3 -W error::ResourceWarning -m unittest discover -s src/navi33-bringup/bootstrap/tests -p 'test_*.py'
```

Kernel sources can be syntax-checked on Linux:
1. Clone `https://github.com/somestupidgirl/RDNA4FB` with
   `--recurse-submodules` into a scratch directory.
2. Run `clang++ -target x86_64-apple-macos11.0 -I<RDNA4FB>/MacKernelSDK/Headers -Isrc
   -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT -nostdinc -fno-builtin
   -mkernel -fapple-kext -Wall -std=c++17 -fno-exceptions -fno-rtti -fsyntax-only src/Navi33Probe.cpp`
   (from `src/navi33-bringup`).

Real kext builds happen only in CI. When a decision rule changes, check
that a test fails without it, for example by temporarily removing the
rule.

## Road to compute (the user's goal)

The user wants the RX 7600 to run real compute on macOS. The finish line
is a gfx1102 shader writing a value that the CPU reads back. The plan is
to port the Navi48 ladder (`src/navi48-bringup/src/amd/amdgpu_init.h`,
`BringupStage`) to the RDNA3 IP versions:

1. IP discovery and read-only probe (done).
2. PSP: load sOS (stages 4–8). This is the **first GPU write**.
   **Done on 2026-10-18, in the VM with kext 0.0.8:** sOS `00330024`
   alive, 8 register writes (see the findings log). Implemented in
   kext 0.0.6 (N33N ABI 9). Its plan refused on bare metal (console
   address was virtual, see the log); fixed in 0.0.7.
   - **To be run in the VM** (user's choice, 2026-10-18). The guest has
     no GOP console (see "VM test loop"), so kext 0.0.8 can take the
     console from the host: `navi33-hostfb-offset`/`-bytes`, recorded by
     `prepare-card.sh` from Linux's `boot_params`. This is used only
     when `boot_args->Video` is empty, and the HUBP check is unchanged.
   - Precondition: `C2PMSG_35` = `0x80000000`. Met on 2026-10-17 (kext
     0.0.5), with no sOS loaded and the SMU already running.
   - Sequence (upstream `psp_v13_0.c`): KDB, SPL (KDB bytes), SYS_DRV,
     then sOS. Each part goes into a zeroed 1 MiB `fw_pri` window with
     `C2PMSG_36` = MC >> 20 and the command in `C2PMSG_35`, then the
     bootloader is polled until it reads exactly `0x80000000` again. The
     sOS is done when `C2PMSG_81` goes non-zero, and `C2PMSG_58` then
     holds its version. Expect 8 register writes in total.
   - `fw_pri` is at VRAM offset 240 MiB (MC `0x80_0F00_0000`,
     `C2PMSG_36` = `0x800f0`). The plan refuses unless:
     - the GOP console is inside BAR0;
     - an active HUBP scanout surface sits at the console's BAR0 offset,
       which proves BAR0 offset == VRAM offset without a write;
     - the window keeps 16 MiB clear of every surface.
     Only that 1 MiB sub-range of BAR0 is ever mapped, and every write is
     read back before the command goes out.
   - Left out on purpose: DRAM long training (Linux runs it only when the
     VBIOS asks), TOC/TMR, the KM ring.
   - Recovery if the PSP wedges: a full power-off (a warm restart may
     leave the PSP state in place).
3. SMU, IMU, RLC: power the GC block up (stages 9–11). **Next.** This
   needs its own approval and its own boot-arg (hard rule 2). The KM ring
   (`C2PMSG_64` is already `0x80000000`) and TMR come first, as in
   upstream `psp_hw_start`.
4. CP, MES, GFX, SDMA (stages 12–15). An SDMA copy is the first real
   GPU work.
5. PM4 fence and compute dispatch (stages 16–17).

**Gate before each write stage:** the user's explicit approval for GPU
writes on the only display GPU, and a recovery path. Given for step 2
on 2026-10-18. Each write stage must be opt-in by its own boot-arg, like
Navi48 (step 2: `navi33-psp-load=1`).

## Conventions

- Branch `feature/navi33-*`; merge to `main` through a PR. Commit
  subjects start with the area (`navi33: ...`, `docs: ...`). Bodies cite
  the physical evidence or test that motivated the change.
- Navi33 code style: compact C++17 / C11, few spaces. Comments explain
  why, not what. Safety constants are `static_assert`ed (`kMayWriteMmio`,
  `kCanLoadFirmware` and so on).
- Each N33N struct has a fixed size checked by `N33_STATIC_ASSERT`, with
  reserved fields that must be zero. Selector shapes live in
  `Navi33NativePolicy.h` and are tested in `tests/test_native_client.cpp`.
