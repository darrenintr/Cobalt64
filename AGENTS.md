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
  **separate** kext (`Navi33Probe`), read-only except for the gated PSP
  stages and the opt-in SMU enable experiment, with its own user-client ABI ("N33N"). No
  shared Navi48 sources. Never reuse RDNA4 constants (GC12, PSP14, DCN4);
  Navi33 is GC 11.0.2, PSP 13.0.7, DCN 3.2.1.
- `tools/native/navi33accel` and `tools/native/navi33metal`: separate Navi33
  readiness policy and unregistered Metal bundle scaffold. Neither currently
  publishes an IOAccelerator or hardware capabilities. The Queue1 memory/VM
  configurator in `src/navi33-bringup/src/Navi33Sdma6Queue1ConfigPlan.h` is
  an offline manifest only.
- `src/dcn321`, `src/dcn41`: display-engine helpers per generation.
- Key Navi33 docs: `src/navi33-bringup/PHYSICAL_EXECUTION.md` (what is
  proven on hardware, what is still blocked), `ACCELERATION.md`,
  `DMA_QUEUE_BRINGUP.md`.
- The PSP sOS load: protocol and plan in `src/Navi33Psp13Loader.h`
  (host-tested), the only GPU writer in `src/Navi33DarwinPspLoad.cpp`,
  the tool in `tools/n33psp.c`.

## Navi33 hard rules

1. **The kext is read-only except three gated PSP stages and the SMU experiment.**
   - The sOS load (approved and run 2026-10-18) may write only
     `C2PMSG_35`/`C2PMSG_36` and its 1 MiB `fw_pri` window in VRAM. It
     needs `navi33-psp-load=1`, is killed by `navi33-psp-off`, and runs at
     most once per boot.
   - The ring stage (code since kext 0.0.9; approved and run in the VM on
     2026-10-18 as part of the SCPM sequence)
     may write only `C2PMSG_64/67/69/70/71` and a 2 MiB window in VRAM.
     It needs `navi33-psp-ring=1`, is killed by `navi33-psp-off`, and runs
     at most once per boot. Its first real run was separately approved as
     part of the SCPM sequence and succeeded on 2026-10-18.
   - The IP firmware stage (code since kext 0.0.10, ABI 11; approved and
     run in the VM on 2026-10-18 as part of the SCPM sequence) may write
     only `C2PMSG_67` (the ring write pointer)
     and two zones of an 8 MiB window in VRAM (ring pages, staging area). It
     needs `navi33-psp-fw=1`, is killed by `navi33-psp-off`, and runs at most
     once per boot. It sends `LOAD_IP_FW` for records of the 27-record list
     (record 26 is the SMU image, added in kext 0.0.11 at the user's choice
     on 2026-10-18; Linux sends it first, so it goes alone as range 26..27
     before the others). `AUTOLOAD_RLC` (stop value 28) is refused
     unless `navi33-psp-autoload=1` is also set. The SCPM sequence approval
     explicitly covered and ran AUTOLOAD_RLC on 2026-10-18. Further real
     runs require separate approval (rule 2).
   - The SMU enable experiment (kext 0.0.12, ABI 13; **one approved attempt
     executed 2026-10-18 at 13:14, rejected HIGH with FD; no retry approved**)
     may write only MP1 `C2PMSG_66/82/90`, with the fixed high-mask,
     low-mask, EnableAllSmuFeatures sequence. It needs `navi33-smu-enable=1`,
     is killed by either `navi33-smu-off` or `navi33-psp-off`, and consumes
     its own one-shot latch before the first write. No VRAM/table writes.
     Design/build and read-only planning approved on 2026-10-18; the user
     subsequently approved recreating the PSP sequence and one SMU attempt.
     Later attempts need renewed approval and a full power-off. See
     `src/navi33-bringup/SMU_ENABLE_DESIGN.md` for prerequisites and limits.
   - The SCPM enable-only experiment (kext 0.0.13, ABI 14; one approved
     attempt executed 2026-10-18 at 13:44 and accepted, state 39) was explicitly
     approved by the user on 2026-10-18 after the stable Linux SCPM capture.
     Approval covers building the new mode, recreating sOS/ring/TMR/SMU-first
     IP firmware/AUTOLOAD_RLC in the VM after full power-off, and one
     EnableAllSmuFeatures attempt (message 6, argument 0). It writes only MP1
     C2PMSG_66/82/90, exactly three writes on success, no HIGH/LOW mask messages,
     no VRAM/table/ROM writes in the SMU stage. Requires both
     `navi33-smu-enable=1` and `navi33-smu-scpm=1`; either SMU/PSP kill switch
     stops it. The SCPM gate closes the legacy mask-sequence gate. Both SMU
     modes share the same one-shot latch. The command returned OK, set
     `IMU_GFX_RESET_CTRL` to `0x1f` and `RLC_BOOTLOAD_STATUS` to `0xc000001f`.
     This approval is consumed; further attempts require renewed approval
     and full power-off. The follow-up preflight still reports reason 11
     (GPU memory/queue ownership not established), and Metal remains disabled.
   No other register write, firmware load or command submission.
5. **Never modify the VBIOS, permanently (user's rule, 2026-10-18).** No
   flashing, no writing, no firmware-update or vBIOS-flash tools, nothing
   that alters the ROM image or its flash chip, in any situation and under
   any circumstances. **Reading is allowed** (for example copying the ROM
   image for inspection); the user confirmed this the same day. If a task
   seems to need a change to the VBIOS, stop: the answer is no.
   `Navi33HardwarePlan.h` and `tests/test_manifest.py` enforce this; do
   not weaken them.
2. **The RX 7600 is the only display GPU in the test Mac.** Every new
   write stage (SMU, IMU, RLC, and so on) needs the user's explicit
   approval to write to that card, plus a recovery path (SSH into the
   Mac, and a full power-off to reset the PSP). Ask first; never assume.
   The PSP sOS load was approved on 2026-10-18 with SSH as the recovery
   path. That approval covers that stage only.
   - The user approved one bounded GMC11 VMID1 + SDMA6 Queue1 COPY/FENCE
     proof attempt on 2026-10-18, with SSH and full power-off recovery.
     It has not run and does not authorize unrelated writes. It remains
     gated on a production PCI/queue owner, a real owned VRAM page-table
     root and readback, independent recovery, a read-only plan, and a fresh
     CI kext artifact. Synthetic ownership flags are not hardware evidence.
3. **Every register offset comes from upstream Linux amdgpu headers**
   for the exact IP version, never from memory. See "Adding a register
   read".
4. **Never rely on a result you have not verified.** A zero read from a
   GC register is not "idle": the GC block reads 0 when powered down.
   All-ones means the block is not answering.
6. **Do not register or advertise Navi33 Metal acceleration until the
   complete per-device owner, memory/VM, SDMA and GFX queue, per-client
   submit/wait, shader translation, rendered-output readback and recovery
   path have matching live hardware evidence for one owner epoch.** Host
   policy tests, offline manifests and a buildable bundle do not count.

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
- PCI owner probe (kext 0.0.15, default off, never on the stick): `navi33-owner-probe=1`
  runs one `IOService::open`/`close` on the provider and a framebuffer/accelerator
  child scan, and logs the decision. `navi33-owner-off` kills it,
  `navi33-owner-recovery=1` is the operator's recovery attestation. Config reads only.
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
  - `make-opencore.sh ARTIFACT_DIR [--oc RELEASE_DIR] [--hostfb FILE]
    [--dma-preflight] [--output FILE.qcow2] [write-stage options]`
    builds `navi33/OpenCore-navi33.qcow2` by default from the VM's OpenCore disk
    plus the artifact's kext, and writes `OpenCore-navi33.txt` (kext UUID,
    OpenCore version, boot-args). It sets `-v debug=0x108 serial=3`, the
    same `navi33-*` flags as the stick, and `navi33-psp-off=1` unless
    `--psp-load` is given. It also sends OpenCore's log to serial.
    `--hostfb /var/lib/cobalt64-navi33/hostfb.txt` adds the host console
    boot-args (kext 0.0.8+) and records that boot's ID in the manifest.
    `--dma-preflight` removes `navi33-dma-off=1` and adds
    `navi33-dma-preflight=1`; this one-shot IODMACommand mapper check maps
    and unmaps system RAM and issues no GPU DMA. `--output` writes a separate
    `.qcow2` filename under `navi33/` for an isolated diagnostic boot disk.
    **Always pass `--oc`** with an unpacked OpenCorePkg release (1.0.8 in
    use). The VM's own OpenCore 1.0.4 cannot inject kexts on macOS 26.
  - `run-vm.sh [--no-gpu] [--snapshot] [--intel-iommu] [--check]` boots the VM. Each run
    gets `navi33/runs/<stamp>/` (with `runs/latest` pointing at it):
    `serial.log`, `qemu.log`, `cmdline.txt`, `qmp.sock` and `hmp.sock`.
    gdb attaches on 127.0.0.1:1234. A guest reboot stops the VM.
    It hides the card's ROM (`rombar=0`) unless `ROMBAR=1`: the guest's
    AMD GOP switches DCN off. It refuses an OpenCore disk whose hostfb
    record is from another boot. `--intel-iommu` exposes QEMU's virtual
    Intel VT-d with VFIO caching mode for read-only `IODMACommand` mapper
    experiments; it does not itself submit GPU DMA.
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
- **2026-10-18, kext 0.0.9 (N33N ABI 10) in the VM: the ring stage's
  first plan, read-only** (`runs/` latest at 11:0x, `ring-plan-{1,2,3}.txt`).
  - Build: CI run 39092196781, commit `36e8ede`. Loaded UUID
    `780E89DB-2315-3D25-A407-A205947F5AE4` matches the artifact (cached in
    `navi33/cache/artifact-0.0.9`). Gates closed (`navi33-psp-off=1`, no
    `navi33-psp-ring`); the sOS from the earlier load was still alive.
  - `n33psp --ring-plan 1`, `2` and `3`: state 14, flags `0x0f`, console
    from the host, `C2PMSG_64` `80000000`, `C2PMSG_67/69/70/71` all 0
    ("fresh", not resumed). Layout: fw_pri MC `0x800f000000`, ring
    `0x800f100000`, command page `0x800f101000`, fence `0x800f102000`,
    TMR MC `0x81fc000000`. The kext's IOLog line reached `serial.log`.
  - Afterwards the host read the card: sOS heartbeat still counting, ring
    registers still 0, DCN scanning. Nothing was written.
  - **Next:** the user's explicit yes for `--ring 1` (create the KM ring and
    nothing else): needs an OpenCore disk with `navi33-psp-ring=1` and no
    `navi33-psp-off`, then restore the closed disk afterwards.
- **2026-10-18, kext 0.0.9 in the VM: the PSP KM ring was created.
  Second GPU write stage, step 1 only.** Run `runs/20261018-110538` (`ring-*.txt`,
  `serial.log`; host reads in `navi33/host/ringregs-after-ring.txt`). The
  user approved it ("yes, create the ring") after the read-only plan.
  - Setup: OpenCore disk built with `--psp-ring` (`navi33-psp-ring=1`, no
    `navi33-psp-off`, the sOS-load gate still closed, other kill switches
    unchanged). The plan was rerun with the gate open: state 14, flags
    `0x0f`, ring flags `0x10` (gate open), ring registers fresh.
  - `n33psp --ring 1`: state 28 (`ring stage DONE`), flags `0x6f`, **4
    register writes** (`C2PMSG_69`, `70`, `71`, then `64`), 20 ms, the
    fence and command status unused (0). `C2PMSG_64` went from
    `0x80000000` to `0x80020000`: the response flag with a zero status and
    the KM ring type echoed. The kext logged `RUN starts` and `RUN state
    28`.
  - Read from the host after the VM stopped: `C2PMSG_69` `0x0f100000`,
    `C2PMSG_70` `0x80`, `C2PMSG_71` `0x1000` (our ring at MC
    `0x80_0F10_0000`), `C2PMSG_67` 0 (no frame yet), `C2PMSG_58`
    `0x00330024`, and the sOS heartbeat still counting. DCN kept scanning.
  - The gate was closed again afterwards (`navi33-psp-off=1` is back in
    `OpenCore-navi33.qcow2`). The ring lives in the PSP until a full
    power-off, so the next ring step resumes it: the plan sees the ring
    registers equal to our ring and sets "resume".
  - **Next:** `--ring 2` (copy the TOC, send LOAD_TOC through the ring,
    read the TMR size from the response). It is the first command through
    the ring and the first use of the fence; it needs its own yes.
- **2026-10-18, kext 0.0.9 in the VM: LOAD_TOC went through the PSP ring.
  First command through the ring.** Run `runs/20261018-110734` (`ring2-*.txt`,
  `serial.log`; host reads in `navi33/host/ringregs-after-toc.txt`). The
  user approved it ("yes, load the TOC") after the read-only plan.
  - The plan (`--ring-plan 2`) was OK and said `resumed`: the registers
    held our ring from the earlier step (`80020000`, `_67` 0, `_69/70/71`
    `0f100000/80/1000`).
  - `n33psp --ring 2`: state 28, flags `0x6f`, ring flags `0x15` (resumed,
    toc-loaded, gate open), **1 register write** (`C2PMSG_67` 0 -> 16), 2
    ms. The 2304 B TOC was copied into `fw_pri` and read back; the frame,
    command buffer and fence were written and read back. **The fence
    arrived (value 1) on the first poll and the response status was 0**,
    so the feared stale-HDP-read problem did not appear on this card.
  - **The PSP asked for a TMR of 19,922,944 B = 0x1300000 (19 MiB)**,
    exactly the size in this card's Linux journal. The runner accepted it
    (4 KiB multiple, within 32 MiB, address 1 MiB aligned at MC
    `0x81_FC00_0000`). The TMR itself is not set up yet.
  - Host reads afterwards: `C2PMSG_67` `0x10` (one frame consumed),
    `C2PMSG_64` `0x80020000`, ring registers unchanged, sOS heartbeat
    counting, DCN scanning. The gate was closed again.
  - Known reporting quirk (to fix with the next kext build): a resumed run
    never re-reads `C2PMSG_64`, so `ring regs: C2PMSG_64` in the run
    summary prints 0; the host read shows the real `80020000`.
  - **Next:** `--ring 3`: SETUP_TMR with MC `0x81_FC00_0000` and 19 MiB.
    After it the PSP owns that VRAM region. It needs its own yes.
- **2026-10-18, kext 0.0.9 in the VM: SETUP_TMR succeeded. The PSP ring
  stage is complete (ring, LOAD_TOC, TMR).** Run `runs/20261018-110904` (`ring3-*.txt`,
  `serial.log`; host reads in `navi33/host/ringregs-after-tmr.txt`). The
  user approved it ("yes, set up the TMR") after the read-only plan.
  - `n33psp --ring 3` on the resumed ring: state 28, flags `0x6f`, ring
    flags `0x1d` (resumed, toc-loaded, tmr-set, gate open), **2 register
    writes** (`C2PMSG_67` 16 -> 32 -> 48: LOAD_TOC repeated, then
    SETUP_TMR; an earlier message wrongly said 1), 5 ms, response status
    0, fence 2 (the SETUP_TMR frame).
  - The TMR is MC `0x81_FC00_0000`, 19,922,944 B (0x1300000), the same
    place and size as in this card's Linux journal. It was written by the
    PSP alone; nothing of ours was mapped there.
  - Host reads afterwards: `C2PMSG_67` `0x30` (three frames consumed),
    `C2PMSG_64` `0x80020000`, ring registers unchanged, sOS heartbeat
    counting, DCN scanning at 60 Hz. The gate was closed again.
  - In all, the PSP is now where Linux's `psp_hw_start()` leaves it for
    this ASIC (sOS, KM ring, TMR). **Next on the ladder:** loading the other
    firmware through the ring (`GFX_CMD_ID_LOAD_IP_FW`: RLC, MES, CP, SDMA
    and the rest) and the SMU/IMU/RLC power-up that follows; each of those
    is a new write stage that needs the user's approval and its own gate
    (hard rule 2).
- **2026-10-18, kext 0.0.10 (N33N ABI 11) in the VM: the IP firmware stage's
  first plans, read-only.** Run `runs/20261018-113125` (`ipfw-plan-*.txt`, `serial.log`;
  host read in `navi33/host/ringregs-after-fwplan.txt`).
  - Build: CI run 39093804518, commit `770e160`. Loaded UUID
    `527282A6-5B3B-39CE-8DA8-CFB8249F55D4` matches the artifact (cached in
    `navi33/cache/artifact-0.0.10`). All gates closed (`navi33-psp-off=1`).
    `guest-run.sh` put the eight decompressed firmware files in the guest's
    `~/navi33/fw`.
  - `n33psp --ip-fw-plan` for `0 2`, `2 25`, `25 26`, `0 26`, `26 27` and
    `0 27`: **state 14, flags `0x0f`** every time. So the kext's SHA-256 pins
    accept the real files in the guest, the 26-record list builds, our ring
    is found (`resumed`, `C2PMSG_67` `0x30`), and the staging area is at MC
    `0x800f400000` (2,539,520 B). The kext's IOLog line reached `serial.log`.
  - With the gate closed `n33psp --ip-fw 0 2 ... --i-have-ssh` and `--ip-fw 0
    27 ...` both refused ("Write gate closed", exit 6) before any run call.
  - Host reads afterwards: `C2PMSG_67` `0x30`, `C2PMSG_64` `0x80020000`, ring
    registers unchanged, sOS heartbeat counting. Nothing was written.
  - Known cosmetic issue: the tool's last line says "GPU writes: ... (see
    flags)" even when the gate refused the run; the flags are what count.
    To fix with the next build.
  - **Next:** the user's explicit yes for `--ip-fw 0 2` (the two SDMA images:
    2 register writes, 2 staged pages-sets, 2 `LOAD_IP_FW` commands), which
    needs an OpenCore disk built with `--psp-fw`. `AUTOLOAD_RLC` (stop 27) is a
    separate approval and needs `--psp-autoload` as well.
- **2026-10-18, kext 0.0.10 in the VM: the first IP firmware was loaded - the
  two SDMA images (records 0 and 1).** Third GPU write stage, first run.
  Run `runs/20261018-113712` (`ipfw-run-0-2-*.txt`, `serial.log`; host reads in
  `navi33/host/ringregs-before-fw.txt`, `ringregs-after-sdma-fw.txt`,
  `gcregs-after-sdma-fw.txt`). The user approved it ("yes, load the SDMA
  firmware") after the read-only plan.
  - Setup: OpenCore disk built with `--psp-fw` (`navi33-psp-fw=1`, no
    `navi33-psp-off`, no `navi33-psp-autoload`, other switches unchanged).
    The plan rerun with the gate open: state 14, ring flags `0x11`.
  - `n33psp --ip-fw 0 2`: state 32 (`firmware stage DONE`), flags `0x6f`,
    ring flags `0x31` (resumed, fw-loaded, gate open), **2 register writes**
    (`C2PMSG_67` `0x30` -> `0x40` -> `0x50`), 4 ms, response status 0 and
    fence 2. The PSP returned TMR address `0x81_FC93_2100` for the last
    record, inside the TMR (`0x81_FC00_0000`..`0x81_FD30_0000`).
  - Host reads afterwards: `C2PMSG_67` `0x50`, `C2PMSG_64` `0x80020000`, ring
    registers unchanged, sOS heartbeat counting, DCN scanning at 60 Hz. GC is
    **unchanged**: `IMU_GFX_RESET_CTRL` `0x10`, `RLC_CNTL`,
    `RLC_BOOTLOAD_STATUS`, `SDMA0_ID` and `GRBM_STATUS` all 0. The images are
    only stored; nothing starts them until `AUTOLOAD_RLC`.
  - The gate was closed again (`navi33-psp-off=1` is back in
    `OpenCore-navi33.qcow2`).
  - **Next:** records 2..25 (CP, MES, IMU, RLC except RLC_G), in ranges the
    user approves, then `RLC_G` and `AUTOLOAD_RLC` (stop 27, needs
    `--psp-autoload` and its own approval). Re-sending records 0-1 is not
    needed: the PSP keeps them until a full power-off.
- **2026-10-18, kext 0.0.10 in the VM: IP firmware records 2..24 loaded
  (CP, MES, IMU and the RLC parts except RLC_G).** Run `runs/20261018-113949`
  (`ipfw-run-2-25-*.txt`, `serial.log`; host reads in
  `navi33/host/ringregs-before-fw-2-24.txt`, `ringregs-after-fw-2-24.txt`,
  `gcregs-after-fw-2-24.txt`). The user approved it ("yes, load records 2 to
  24") with a one-hour standing approval for the routine steps; I read that
  as covering these 23 records and the steps around them only, **not**
  `RLC_G` or `AUTOLOAD_RLC`, which keep their own explicit approval.
  - Setup: OpenCore disk built with `--psp-fw` only (no autoload, no
    `navi33-psp-off`). The plan for records [2,25) was OK first (state 14,
    ring flags `0x11`, `C2PMSG_67` `0x50`).
  - `n33psp --ip-fw 2 25`: state 32, flags `0x6f`, **23 register writes**
    (`C2PMSG_67` `0x50` -> `0x1c0`, one per record), 60 ms, **response status
    0 and fence 23**; the PSP gave `0x81_FC91_2F00` for record 24 (inside the
    TMR). One run per boot, so records 0-1 (earlier run) and 2-24 are now
    stored; 24 of 26 records are in.
  - Host reads afterwards: `C2PMSG_67` `0x1c0`, `C2PMSG_64` `0x80020000`, ring
    registers unchanged, sOS heartbeat counting, DCN scanning. **GC still
    unchanged** (`IMU_GFX_RESET_CTRL` `0x10`, RLC and SDMA registers 0). The
    gate was closed again.
  - **Next (needs the user's yes):** record 25 (`RLC_G`), then
    `AUTOLOAD_RLC`: `--ip-fw 25 27` in a single run sends both, with
    `--psp-fw --psp-autoload`; or `--ip-fw 25 26` first to store `RLC_G`
    alone and `--ip-fw 26 27` for the autoload in a second VM boot.
- **2026-10-18, kext 0.0.10 in the VM: RLC_G (record 25) loaded alone. All 26
  IP firmware records are now stored in the PSP.** Run `runs/20261018-115107`
  (`ipfw-run-25-26-*.txt`, `serial.log`; host reads in
  `navi33/host/ringregs-before-rlcg.txt`, `ringregs-after-rlcg.txt`,
  `gcregs-after-rlcg.txt`). The user approved it ("yes, load RLC_G alone").
  - Setup: `--psp-fw` only (no autoload). The plan for [25,26) was OK first.
  - `n33psp --ip-fw 25 26`: state 32, flags `0x6f`, **1 register write**
    (`C2PMSG_67` `0x1c0` -> `0x1d0`), 2 ms, response status 0, fence 1; the PSP
    gave `0x81_FC90_0000` for RLC_G (inside the TMR).
  - Host reads afterwards: `C2PMSG_67` `0x1d0`, `C2PMSG_64` `0x80020000`, ring
    registers unchanged, sOS heartbeat counting, DCN scanning. GC
    **unchanged**: `IMU_GFX_RESET_CTRL` `0x10`, `RLC_CNTL`,
    `RLC_BOOTLOAD_STATUS`, `SDMA0_ID`, `GRBM_STATUS` all 0. Gate closed again.
  - **Next (needs the user's explicit yes, and the user present):**
    `AUTOLOAD_RLC` = `--ip-fw 26 27` with `--psp-fw --psp-autoload`. It sends
    one command and no record. Expected to change, if it works:
    `IMU_GFX_RESET_CTRL` (from `0x10`), `RLC_BOOTLOAD_STATUS`, `RLC_CNTL`,
    `GRBM_STATUS`, `SDMA0_ID`. If the card hangs, recover with `sudo poweroff`
    then the power button (a full power-off also resets the PSP, so the whole
    sOS/ring/TMR/firmware sequence must be redone afterwards).
- **2026-10-18, kext 0.0.10 in the VM: AUTOLOAD_RLC was sent and accepted. The
  IMU firmware is running.** Run `runs/20261018-115241` (`ipfw-run-26-27-*.txt`,
  `results-20261018-115324-guest.txt`, `serial.log`; host reads in
  `navi33/host/*-before-autoload.txt` and `*-after-autoload.txt`,
  `imuregs-after-autoload.txt`). The user approved it ("yes, run
  AUTOLOAD_RLC").
  - Setup: OpenCore disk built with `--psp-fw --psp-autoload` (both gates
    open, no `navi33-psp-off`). The plan for [26,27) was OK first: zero
    records, autoload 1, ring flags `0x91`.
  - `n33psp --ip-fw 26 27`: state 32, flags `0x6f`, ring flags `0xd1`
    (autoload gate open, autoload sent), **1 register write** (`C2PMSG_67`
    `0x1d0` -> `0x1e0`), 1 ms, **response status 0 and fence 1**. The VM,
    the PC and the display all survived.
  - **What changed on the card** (full `n33peek --all` diff against the
    snapshot taken right after the sOS load; everything else is identical
    apart from the ring bookkeeping and the sOS heartbeat):
    - `IMU_SCRATCH[0]` 0 -> `0x0b214700`: equal to the IMU firmware header's
      `ucode_version` (`0xb214700`).
    - `IMU_SCRATCH[1]` 0 -> `0xbabeface`.
    - `IMU_CORE_STATUS` (GC seg1 `0x40b7`) 0 -> `0x00000002`.
    Read from the host after the VM stopped, the same values persist.
  - What did **not** change: `IMU_GFX_RESET_CTRL` is still `0x10` (Linux
    waits for `0x1f` after starting the IMU), and `RLC_CNTL`, `RLC_STAT`,
    `RLC_GPM_STAT`, `RLC_BOOTLOAD_STATUS`, `GRBM_STATUS`, `CP_*`, `SDMA0_*`
    and all GFXHUB/GCVM registers still read 0. The preflight is still
    reason 9 (GC not answering). DCN kept scanning at 60 Hz.
  - Interpretation (**not verified beyond the register values**): the
    scratch dword equal to the IMU image's own version strongly suggests the
    IMU core is executing the firmware the PSP installed. The matching
    upstream step is that, after the PSP load, the *driver* runs
    `imu_v11_0_setup()` ... **CORRECTED the same day, see the entry
    "IMU start is not a driver stage in the PSP path": that reading was
    wrong for this chip.**
  - The gate was closed again (`navi33-psp-off=1` is back in
    `OpenCore-navi33.qcow2`). The firmware stays installed until a full
    power-off.
  - **Next:** superseded by the correction entry below.
- **2026-10-18, correction: the IMU start is not a driver stage in the PSP
  path, and the RLC bootload has not progressed.** Found while starting the
  "IMU start" design from upstream (`gfx_v11_0.c`, `imu_v11_0.c`).
  - `gfx_v11_0_hw_init()` calls the IMU `load_microcode`/`setup_imu`/`start_imu`
    functions only for the DIRECT and RLC-backdoor load types. For
    `AMDGPU_FW_LOAD_PSP` (ours: `amdgpu_ucode_get_load_type` returns PSP for the
    default) it goes straight to `gfx_v11_0_wait_for_rlc_autoload_complete()`.
    So the PSP's `AUTOLOAD_RLC` is meant to boot the IMU and RLC by itself, and
    the driver then only **waits** for `CP_STAT == 0` and
    `RLC_RLCS_BOOTLOAD_STATUS.BOOTLOAD_COMPLETE` (bit 31; GC seg1 `0x4e82`,
    the register our windows call `RLC_BOOTLOAD_STATUS`).
  - Observed, host reads at 11:56 (about 4 minutes after the command, and again
    unchanged in the guest 1 minute after it): `IMU_SCRATCH[0]` `0x0b214700`
    (the IMU image's version), `[1]` `0xbabeface`, `IMU_CORE_STATUS` 2, but
    `RLC_RLCS_BOOTLOAD_STATUS` **0** (not even `GFX_INIT_DONE`),
    `IMU_GFX_RESET_CTRL` `0x10` (Linux's own IMU start waits for `0x1f`),
    `RLC_CNTL`/`RLC_STAT`/`GRBM_STATUS` 0. The state did not change with time.
  - **Hypothesis, not verified:** the RLC autoload depends on the SMU. Linux
    sends the SMU image (`GFX_FW_TYPE_SMU`, `smu_13_0_7.bin`) *first* in
    `psp_load_non_psp_fw()` (`psp_load_smu_fw`), and `gfx_v11_0_hw_init` says
    "for gfx 11, rlc firmware loading relies on smu firmware is loaded
    firstly". We left the SMU image out by the user's decision (2026-10-18),
    and our order differs from Linux's in any case (SMU first, autoload last).
    The earlier `AUTOLOAD_RLC` is therefore not the Linux sequence.
  - Options (need the user): (a) reproduce Linux's order exactly, including the
    SMU image, after a full power-off (everything is scripted, about an hour);
    (b) first collect more read-only evidence (SMU mailbox state, what the VBIOS
    SMU reports) and decide after. Re-sending `AUTOLOAD_RLC` alone is not
    expected to help and is not proposed.
- **2026-10-18, option 2 (read-only evidence), part 1.** The user chose to
  collect read-only evidence before deciding whether to re-run the sequence
  with the SMU image (option (a) above). No card access beyond reads.
  - **The SMU mailbox block (`SMU_C2PMSG`, MP1 seg0 `0x260` x72) is
    byte-identical** in the guest snapshots taken after the first VM run, after
    the sOS load and after `AUTOLOAD_RLC`. Nothing we sent reached the SMU, and
    the autoload did not visibly involve it. (We never write the SMU mailbox.)
  - **Linux reference timeline** (kernel journal of a normal amdgpu boot of this
    card, 2026-10-18 08:24:54): TMR reserved at `0x81fc000000` (+0.134 s), then
    SMU `hw_init` reports `smu fw version = 0x00526100 (82.97.0)`, `smu driver
    if 0x35, fw if 0x40` (+0.240 s), `SMU is initialized successfully` (+0.285
    s), display core (+0.285 s), and the GFX/ring summary at +0.816 s. 82.97.0
    equals the `ucode_version` of the `smu_13_0_7.bin` Linux loads, so the SMU
    Linux talks to is running the PSP-loaded copy; what the VBIOS ran before
    that is not known to us (it cannot be read passively).
  - Open question the evidence cannot settle: why `RLC_RLCS_BOOTLOAD_STATUS`
    stays 0. The SMU image (hypothesis in the entry above) is only one
    candidate; the working end state of these registers on this very card is
    not yet recorded.
  - **Proposed next read-only step (needs the user's physical action):** a
    reference boot. Full power-off, then a normal Ubuntu boot where amdgpu
    brings the card up itself; dump the same registers from the host
    (`IMU_*`, `RLC_*`, `GRBM_STATUS`, `CP_STAT`, `SMU_C2PMSG`, `PSP_C2PMSG`) and
    the amdgpu debugfs firmware/pm info, then power off again. That gives the
    exact target values to compare with. A warm restart is not enough: our sOS
    and ring are still in the PSP, which amdgpu does not expect.
  - **Procedure for the reference boot** (so a fresh session can run it): 1) the
    user does a FULL power-off and boots normal Ubuntu; 2) check `lspci -k -s
    09:00.0` shows `amdgpu`; 3) `sudo python3 -I
    src/navi33-bringup/tools/ref-dump.py` (read-only, saves
    `~/macos tahoe/navi33/host/reference-*.txt`; it turns the driver's GFXOFF
    switch off while reading the graphics registers and restores it, because
    those reads can hang the machine in GFXOFF); 4) compare with
    `runs/20261018-115241/results-20261018-115324-guest.txt`; 5) the user does
    another FULL power-off before any of our own experiments, since amdgpu
    leaves its own sOS in the PSP. The password for `sudo` is the user's own;
    ask if it is not in the conversation.
- **2026-10-18, decision: reproduce Linux's order including the SMU image.**
  After the correction entry the user chose to skip the reference boot and to
  go straight to option (a). Full power-off done at 12:00 (PSP verified fresh:
  `C2PMSG_81` 0, `C2PMSG_35` `0x80000000`). Kext 0.0.11, N33N ABI 12: the
  firmware list has 27 records, record 26 = `SMC` (GFX_FW_TYPE_SMU 18,
  `smu_13_0_7.bin`, 261,632 B at offset 256, header v2.1, firmware 82.97.0), a
  ninth pinned file; `AUTOLOAD_RLC` is now stop 28. Sequence for the next
  attempt, each its own VM boot with its own explicit yes: sOS load, ring
  1/2/3, `--ip-fw 26 27` (SMU **first**, as Linux), `--ip-fw 0 26`, then
  `--ip-fw 27 28` with `--psp-autoload`. Expected if the hypothesis holds:
  `RLC_RLCS_BOOTLOAD_STATUS` bit 31 and `IMU_GFX_RESET_CTRL` 0x1f.
  Open risk: the SMU is already running the VBIOS' copy; re-loading may change
  clocks or briefly disturb the display.
- **2026-10-18 12:2x, kext 0.0.11 (ABI 12) in the VM: sOS and ring redone on
  the freshly power-cycled card, SMU plans OK.** The user approved ("yes, redo
  sOS and ring") the repeats of stages already proven.
  - sOS load: state 20, 8 writes, sOS `00330024`, heartbeat counting.
  - `--ring 3` from a fresh ring (create, LOAD_TOC, SETUP_TMR in one run): state
    28, flags `0x6f`, ring flags `0x1e`, 6 register writes, status 0, fence 2,
    TOC 2304 B, TMR 19,922,944 B; host: `C2PMSG_67` `0x20`, `C2PMSG_64`
    `0x80020000`, ring `0x0f100000`/`0x80`/`0x1000`.
  - With all gates closed: `--ip-fw-plan 26 27` (SMU), `0 26` and `27 28`
    (autoload) all state 14, ring found and resumed. Nothing else written.
  - **Next, needs its own yes:** `--ip-fw 26 27` (the SMU image alone,
    261,632 B, 1 register write), then `0 26`, then autoload `27 28`.
- **2026-10-18, kext 0.0.11 in the VM: the SMU image (record 26) was loaded
  first and alone, as Linux does.** Run `runs/20261018-121614`
  (`ipfw-run-26-27-smu-*.txt`; host reads `navi33/host/smuregs-before-smu.txt`
  and `smuregs-after-smu.txt`). The user approved it ("yes load that SMU").
  - `--ip-fw 26 27`: state 32, **1 register write** (`C2PMSG_67` `0x20` ->
    `0x30`), 7 ms, response status 0, fence 1; the PSP returned TMR address 0
    for it (Linux ignores it for SMC). No flicker: DCN kept scanning at 60 Hz,
    no MCE/AER lines in the kernel log.
  - The SMU mailbox block (`0x260..0x2a7`) is **identical** before and after
    (only the sOS heartbeat differs). So the PSP accepted the SMU image without
    any visible change to the running SMU; whether it was applied is not
    shown by these registers.
  - Gate closed again. **Next, needs a yes:** `--ip-fw 0 26` (the other 26
    records), then `--ip-fw 27 28` with `--psp-autoload`.
- **2026-10-18, kext 0.0.11 in the VM: Linux's order WITH the SMU image did not
  change the outcome. The SMU hypothesis is refuted as stated.** Runs on the
  card power-cycled at 12:00 (sOS, ring, TMR redone; SMU first; then records
  0..25; then autoload). The user approved ("yes, load the rest and run
  autoload"). Run `runs/20261018-122534` (`ipfw-run-0-26-*`, `ipfw-run-27-28-*`,
  `results-20261018-122607-guest.txt`; host `navi33/host/smuregs-*autoload2.txt`).
  - `--ip-fw 0 26`: state 32, **26 writes**, status 0, fence 26, 68 ms (RLC_G
    at `0x81_FC90_0000`). `--ip-fw 27 28` (autoload): state 32, 1 write,
    status 0, fence 1, 1 ms.
  - After it, identical to the run without the SMU: `IMU_SCRATCH[0]`
    `0x0b214700`, `[1]` `0xbabeface`, `IMU_CORE_STATUS` 2, but
    `RLC_BOOTLOAD_STATUS` 0, `IMU_GFX_RESET_CTRL` `0x10`, `RLC_*`, `GRBM_STATUS`,
    `CP_STAT`, `SDMA0_ID` all 0; unchanged one minute later from the host. The
    SMU mailbox block did not change either; display fine, no errors.
  - So the SMU image was not the missing piece (it may still matter, but is not
    sufficient). Remaining differences from Linux that we know of: Linux also
    loads ASD, runs `psp_rl_load`, loads `DMCUB` through the PSP in the display
    init, does SMU `hw_init` (feature enablement, tables) between the PSP and
    GFX init, and runs with the real driver's HDP/doorbell/NBIO setup; none of
    these is tested. The reference boot (declined for now) would show the
    working end state to compare with.

- **2026-10-18 12:41, reference boot (normal Ubuntu, amdgpu, after the full
  power-off; read-only `tools/ref-dump.py`)**: the working end state of this
  card, saved as `navi33/host/reference-20261018-124119.txt`.
  - Same as ours after AUTOLOAD_RLC: `IMU_SCRATCH[0]` `0x0b214700`, `[1]`
    `0xbabeface`. So our IMU boots exactly as Linux's does.
  - Different (Linux): `IMU_CORE_STATUS` 3 (ours 2), `IMU_RLC_STATUS` 2,
    `IMU_GFX_RESET_CTRL` `0x1f`, `RLC_CNTL` 1, `RLC_GPM_STAT` `0x00ac001a`,
    `RLC_BOOTLOAD_STATUS` `0xc000001f` (bit 31 complete), `GRBM_STATUS`
    `0x302c`, `CP_STAT` `0x8200`, `SDMA0_ID` 1. IMU_SCRATCH[11] = `0x13`.
  - Linux's PSP ring registers: `C2PMSG_64` `0x80020000`, `_67` `0x2b0`
    (nothing odd); amdgpu's firmware list also shows ASD `0x21000118`, TA
    HDCP/DTM, DMCUB `0x07003300`, TOC `0xc`, MES_KIQ `0x113`, MES `0x92`.
  - Fix: `ref-dump.py` reads/writes `amdgpu_gfxoff` as a raw u32 (text I/O
    gave EINVAL) and writes to the sudo user's home.
  - Conclusion: IMU start is identical, the RLC does not follow. Next:
    compare what Linux does between AUTOLOAD_RLC and the first GC read
    (e.g. the SMU/IMU handshake) using the kernel source and a
    `drm.debug` boot log; then update the sequence.

- **2026-10-18, upstream study (read-only): what Linux does after
  AUTOLOAD_RLC.** Sources: `amdgpu_psp.c`, `gfx_v11_0.c`, `amdgpu_smu.c`,
  `smu_v13_0*.c`, `smu_v13_0_7_ppsmc.h` (torvalds/linux).
  - Our PSP sequence matches `psp_load_fw()` up to the autoload: sOS, KM ring,
    TMR, SMU image first, the GC records, `AUTOLOAD_RLC` right after RLC_G
    (Linux then loads ASD, RL and the TAs, which only touch the PSP).
  - IP block order is common, gmc, ih, **psp, smu, dm, gfx**. So between the
    autoload and the driver's wait for `BOOTLOAD_COMPLETE` (in
    `gfx_v11_0_hw_init`), Linux runs `smu_hw_init()` -> `smu_smc_hw_setup()`.
    We never did. Its messages, in order: SetDriverDramAddr, SetToolsDramAddr,
    SetSystemVirtualDramAddr/DramLogSetDramAddr, TransferTableDram2Smu
    (pptable; skipped when `scpm_enabled`), RunBtc, **SetAllowedFeaturesMask
    (0x4/0x5), EnableAllSmuFeatures (0x6)**, PCIe parameters, then DPM table,
    thermal, display-change and deep-sleep messages.
  - Strong hint in the reference dump: the SMU mailbox's last message there is
    `0x28` = AllowGfxOff; ours has never carried a driver message.
  - **Hypothesis (not verified):** the PMFW keeps the GC power domain off until
    the driver enables its features, so the RLC cannot finish bootload.
    Candidate minimal test: the SMU mailbox (`MP1 C2PMSG_66` message, `_82`
    argument, `_90` response; offsets to be taken from `mp_13_0_*_offset.h`)
    with `SetAllowedFeaturesMask` low/high and `EnableAllSmuFeatures`, maybe
    preceded by the pptable/driver-table messages. This is a **new write stage**
    (SMU mailbox, and DPM features on the only display GPU): needs a design, host
    tests, its own boot-arg and gate, and the user's explicit approval (rule 2).

- **2026-10-18, SMU stage implementation decision (no hardware run):** user
  approved completing Claude's SMU design/build and read-only plan. Kext
  0.0.12 / ABI 13 adds `n33psp --smu-plan` / `--smu-enable`, a fixed
  three-message experiment, with independent gating/latch and a three-register
  allowlist. Corrected the proposed order from upstream: HIGH (5), LOW (4),
  ENABLE (6). Source-derived Linux default mask: `0x0001a3a471fe7fff`;
  this is not a measurement of the reference boot's allowed mask. Plan
  checks the recorded stalled IMU hardware signature (survives VM restarts),
  not a provider-local history flag. Linux's earlier tables/calibration are
  deliberately absent: acknowledgement alone does not prove GC boot.
  The real SMU write remains unapproved. Full power-off required after
  any SMU attempt and before retry; never take the GPU from live amdgpu.
  - Build verified: CI run `39098725048`, commit `7fc020b`, all host and both
    macOS jobs passed. The macos-26-intel artifact is cached in
    `~/macos tahoe/navi33/cache/artifact-0.0.12`; kext UUID
    `95481710-1E06-3B93-AA71-93878A2B209F`, version 0.0.12 / ABI 13.
    Staged into our isolated `OpenCore-navi33.qcow2` (not booted), with
    `navi33-psp-off=1` and `navi33-smu-off=1`. Its previous disk/manifest are
    in `navi33/backups/pre-0.0.12/`. No hostfb is embedded yet: rebuild with
    the new boot's `/var/lib/cobalt64-navi33/hostfb.txt` before the next VM
    run. The user is ready for the headless reboot; host sudo requires an
    interactive password, so the user must run `vm/reboot-to-vm.sh`.
    SSH socket is enabled and listening. The real read-only `--smu-plan`
    awaits that boot. No SMU write has been performed or approved.

- **2026-10-18 13:01, kext 0.0.12 / ABI 13, headless VM read-only run.**
  Run `runs/20261018-130015`, results `results-20261018-130145-guest.txt`
  and `smu-plan.txt`. Loaded UUID `95481710-1E06-3B93-AA71-93878A2B209F`
  matches the cached artifact. Rebuilt our OpenCore disk with this boot's
  hostfb record; PSP and SMU kill switches remain set. VM and guest SSH are
  running in tmux session `navi33-readonly`; serial capture is active.
  - The card is fresh: PSP `_35` `80000000`, `_58`/`_81` and ring
    registers zero. Compared with the 12:26 post-autoload results, only
    the PSP window, IMU scratch window and IMU core window differ; every
    other allowlisted register window matches. IMU scratch/core are now
    zero; reset control remains `0x10`, RLC boot status zero, SMU response 1.
  - sOS plan passes (state 14, flags `0x0f`, host console at BAR0+0,
    3 MiB, fw_pri MC `0x800f000000`). SMU plan safely refuses at state 21
    (sOS not alive), completed 0/3, zero writes. The prerequisite PSP
    sequence must be recreated before an SMU readiness plan can pass;
    the real SMU enable run remains unapproved.
  - Optional host-peek was skipped because host sudo requires interactive
    authentication. Guest diagnostics succeeded with passwordless guest sudo.

- **2026-10-18, approval and pre-attempt stop:** the user explicitly approved
  one sequence: sOS, ring/TMR, SMU image first, remaining IP firmware,
  AUTOLOAD_RLC, then the fixed SMU HIGH/LOW/ENABLE experiment. This approval
  remains pending execution; later new write stages need separate approval.
  The sOS-gated VM boot `runs/20261018-130354` (`sos-plan-before.txt`) reached a
  read-only PLAN_OK with flags `0x1f`; no write command was invoked.
  - The user confirmed this headless boot came from **Restart**, not full
    power-off. Stopped before any GPU write to meet the SMU design's fresh
    power-cycle prerequisite. Guest shut down and OpenCore rebuilt with
    PSP/SMU kill switches. Next: full host power-off, power on into the
    headless GRUB entry, rebuild hostfb for that boot, then execute the
    already approved sequence. Host sudo requires the user's password.

- **2026-10-18 13:08, approved SMU attempt preparation after full power-off.**
  User confirmed full power-off; headless host boot ID
  `9d8f8c98-18cf-4c4e-b1aa-8a33345b8bf1`, GPU on VFIO, BAR0 256 MiB,
  reset methods disabled, host SSH listening. Kext 0.0.12 UUID matches
  `95481710-1E06-3B93-AA71-93878A2B209F`, ABI 13.
  - `runs/20261018-130733/sos-load.txt`: fresh sOS plan state 14, then
    load state 20, four parts, eight register writes, 27 ms; heartbeat
    `0821ebf8`, sOS `00330024`. Host console and aperture checks passed.
    Results before load: `results-20261018-130829-guest.txt`.
  - `runs/20261018-130920/ring3-run.txt`: ring creation, LOAD_TOC and
    SETUP_TMR state 28, six register writes, status 0, fence 2, 25 ms;
    TMR 19,922,944 B, `_64` `80020000`, `_67` `20`. Console checks passed.
  - `runs/20261018-131034/ipfw-smu-run.txt`: record 26 (SMU) loaded
    first and alone, state 32, one register write, status 0, fence 1,
    7 ms, `_67` `20` -> `30`, returned TMR address 0 (as before).
  - `runs/20261018-131131/ipfw-rest-run.txt`: records 0..25 loaded,
    state 32, 26 register writes, status 0, fence 26, 67 ms; `_67` `30`
    -> `1d0`, RLC_G returned address `0x81fc900000`.
  - `runs/20261018-131233/autoload-run.txt`: AUTOLOAD_RLC state 32,
    one register write, status 0, fence 1, 1 ms; `_67` `1d0` -> `1e0`.
    `smu-plan-after-autoload.txt` passes state 14 with the SMU gate closed:
    scratch `0b214700/babeface`, core 2, reset `10`, RLC 0, SMU response 1.
  - `runs/20261018-131339/smu-enable-run.txt`: the approved SMU attempt
    stopped at the first message. HIGH mask message 5, argument `0001a3a4`,
    response `fd` (CmdRejectedPrereq), state 37, three register writes,
    completed 0/3, 1 ms. LOW and ENABLE were never sent. Post-attempt
    `results-20261018-131433-guest.txt` matches every pre-attempt GC window;
    only PSP heartbeat and the three SMU mailbox values differ. Core remains
    2, reset `10`, RLC 0. Guest SSH survived; serial trace confirms rejection.
  - Guest shut down; OpenCore rebuilt with `navi33-psp-off=1` and
    `navi33-smu-off=1`. A full power-off is required before another attempt;
    the one-attempt approval is consumed. No retry or later new write stage
    is authorized.
  - Upstream follow-up (same pinned Linux revision): `amdgpu_smu.c`
    explicitly forbids allowed-feature-mask messages under SCPM and skips
    them in `smu_smc_hw_setup`. SCPM is read from the PSP runtime database
    (`amdgpu_psp.c`), not inferred from SMU response FD. This is a candidate
    explanation, not proof that SCPM is enabled on this card. Next: read its
    runtime database during a normal Linux reference boot, after power-off.
  - Added `tools/scpm-read.py --capture`: read-only BAR0 capture of the
    64-KiB runtime DB, twice, using driver-reported VRAM size minus 1 MiB.
    Only normal amdgpu on 1002:7480 with DB visible in BAR0 is accepted;
    no aperture/register/ROM writes. Raw copies and JSON go to
    `navi33/host/scpm-<timestamp>/`. Initial offline parser tests passed;
    capture correctly refuses the current VFIO boot. No kext/ABI change.
    After full power-off, boot normal Ubuntu and run `sudo python3 -I
    src/navi33-bringup/tools/scpm-read.py --capture`; hand over its saved
    directory. This capture is the next prerequisite to designing a retry.

- **2026-10-18, first SCPM runtime-database capture:** the user ran the
  read-only capture on the normal amdgpu boot. It read the database header,
  directory and SCPM entry, then reported `unknown SCPM status 0x40000000`.
  The old parser stopped on that first snapshot, before its second read or
  writing either raw database file, so the value is not yet confirmed stable.
  Upstream documents SCPM values 0, 1 and 2, while `amdgpu` treats every
  nonzero value as SCPM enabled. The exact meaning of this value remains
  unknown. Updated the parser to preserve unknown values and save both raw
  snapshots; rerun the same read-only capture and provide its output directory
  before drawing a hardware conclusion. No GPU write was performed.

- **2026-10-18 13:24, SCPM runtime database captured on amdgpu.** The saved
  directory is `/home/darren/macos tahoe/navi33/host/scpm-20261018-132450`,
  boot ID `a8a50688-4c92-4b5a-a228-7df4e4f6d464`. Both 64-KiB snapshots are
  byte-for-byte identical (SHA-256
  `efd6400e63139a9b70fc1345875fe69efb721c1df30302f73f34b82496741ba5`).
  The valid DB has cookie `0x0ed5`, version `0x0100`, and one entry: type 6
  (SCPM), offset `0x208`, size 4. The entry bytes are `00 00 00 40`, or
  little-endian status `0x40000000`. The enum's exact meaning remains
  undocumented, but the pinned Linux `amdgpu` code treats any nonzero value
  as SCPM enabled and skips allowed-feature-mask messages under SCPM. This is
  now the leading explanation for the earlier HIGH `FD` rejection, not proof
  of the specific rejected prerequisite. No retry is approved; no GPU write
  was performed during this capture.

- **2026-10-18, SCPM enable-only experiment approved.** The user approved
  the proposed separate mode, CI build and one physical VM attempt following
  full power-off and recreation of the PSP sequence. Kext 0.0.13 / ABI 14
  adds `--smu-scpm-plan` and `--smu-scpm-enable` (new modes 9/10). The latter
  sends only message 6, argument 0, using the same allowlist, lock, shared
  SMU latch, stop-on-error behavior and one-second read-only GC observation.
  At approval time the Linux host was on amdgpu (boot `a8a50688...`); sudo
  required interactive authentication. Do not detach it from amdgpu. Build and stage
  first, then arrange a full power-off and fresh headless VFIO boot with
  hostfb from that boot. Approval is not yet consumed.

- **2026-10-18 13:36, kext 0.0.13 / ABI 14 built and staged.**
  Source commit `8ebbb72` on `feature/navi33-scpm-enable-only`; GitHub Actions
  run `39100866159` passed host tests and both macOS builds. The macOS 26
  artifact is cached at `~/macos tahoe/navi33/cache/artifact-0.0.13` (paired
  tools included), UUID `EB123CF7-DBF5-3860-9CB7-87B6C597B58A`, executable
  SHA-256 `08e42aae0e1ec6e61de692c91182c47b4c41fdca7168080c480953bb55ff3839`.
  Local full host tests, SMU Clang ASan/UBSan tests and 31 bootstrap tests
  passed. Tests prove the three-write SCPM trace, zero-write plans, error
  stops, the shared latch and the mutually exclusive gates.
  - Our isolated OpenCore disk now contains the artifact with both kill
    switches set; no hostfb is embedded until a fresh headless boot. Every
    extracted kext file matches the artifact. Previous disk and manifest are
    in `navi33/backups/pre-0.0.13/`. User VM files and the USB stick were not
    changed.
  - At staging time the host remained on amdgpu at `192.168.100.24`; SSH
    was active and the card-preparation service enabled. Host sudo required
    interactive authentication. The user ran `sudo grub-reboot cobalt64-navi33-vm`
    followed by `sudo poweroff`, then switch the PC on. This provides a full
    power-off and a one-time headless VFIO boot. Do not use the warm-reboot
    helper for this attempt. After that boot, rebuild our OpenCore disk with
    its new `/var/lib/cobalt64-navi33/hostfb.txt`, verify UUID/plan and execute
    the already approved stages one gate at a time. At this point the SCPM
    attempt remained approved and unconsumed; no GPU write had occurred.

- **2026-10-18 13:39–13:45, kext 0.0.13 / ABI 14 physical VM sequence.**
  The user confirmed a full reboot. Headless host boot ID and hostfb record:
  `0c611479-fc04-4009-a9bc-e7eae92b70df`; RX 7600 remained on `vfio-pci`,
  BAR0 256 MiB, guest SSH recovered, and QEMU could not reset the card.
  OpenCore 1.0.8 loaded UUID `EB123CF7-DBF5-3860-9CB7-87B6C597B58A`.
  - `runs/20261018-133917/sos-load.txt`: PLAN_OK, sOS alive, four parts,
    eight register writes, 25 ms; heartbeat `0x0b855224`, version `0x00330024`.
  - `runs/20261018-134026/ring-run.txt`: ring, TOC and TMR succeeded,
    six register writes, fence 2; TMR 19,922,944 B.
  - `runs/20261018-134128/ipfw-smu-first-run.txt`: SMU image (record 26)
    loaded first, one command/write, fence 1. `runs/20261018-134223/ipfw-rest-run.txt`:
    the other 26 records including RLC_G loaded, 26 commands/writes, fence 26.
  - `runs/20261018-134316/autoload-run.txt`: AUTOLOAD_RLC-only plan and run
    succeeded, response 0, fence 1. `--smu-scpm-plan` then passed with core 2,
    reset `0x10`, RLC 0, response 1, and IMU scratch `0x0b214700/0xbabeface`.
  - `runs/20261018-134419/smu-scpm-enable-run.txt`: the single approved
    EnableAllSmuFeatures command (message 6, argument 0) returned OK. Exactly
    three MP1 writes, 32 ms, state 39; reset changed `0x10` → `0x1f`, and
    `RLC_BOOTLOAD_STATUS` changed 0 → `0xc000001f`. No mask messages, VRAM,
    table or ROM writes in the SMU stage.
  - Combined read-only diagnostics in
    `runs/20261018-134419/results-20261018-134504-guest.txt` show live GC/SDMA
    registers, `SDMA0_ID=0x00000001/0x00000600`, and MMHUB/GFXHUB context 0
    `0x01fffe00`. The preflight is now reason 11 (GPU memory/queue ownership
    not established); N33N still reports hardware command submission and Metal
    disabled. No SDMA copy, shader, or Metal frame ran.
  - The guest was shut down and `OpenCore-navi33.qcow2` rebuilt with both
    `navi33-psp-off=1` and `navi33-smu-off=1`; the VM remains off. The host
    is still on the same headless VFIO boot. The one SCPM attempt is consumed.
    Any new GPU write stage needs its own
    explicit approval and recovery plan; another SMU attempt also needs a
    full power-off.

- **2026-10-18 13:57–13:58, read-only repeat after the SCPM sequence.** The
  VM booted kext 0.0.13 / ABI 14 (UUID
  `EB123CF7-DBF5-3860-9CB7-87B6C597B58A`) with both PSP and SMU kill
  switches set. Results:
  `~/macos tahoe/navi33/runs/20261018-135743/results-20261018-135823-guest.txt`.
  The sOS is still alive (`C2PMSG_81=0x0b88b410`, version `0x00330024`);
  `n33psp --plan` reports state 8 and made zero writes. SDMA6 is observable
  (`SDMA0_ID=1/0x600`, checksum `0x7a75a926`), but Queue0 and Queue1 both
  have zero RPTR/WPTR and are not running a Cobalt64 job. MMHUB/GFXHUB
  context 0 remains `0x01fffe00`; preflight remains reason 11. The N33N
  client reports hardware submission and Metal disabled, and the VM
  diagnostic submitted no copy or shader. This confirms the state persisted
  after the prior firmware/SMU sequence; it does not establish queue
  ownership or a usable GPU page table. QEMU was shut down after collection;
  no GPU write occurred in this run.

- **2026-10-18 14:00, Metal frame acceptance gate on the RX 7600 VM.** With
  kext 0.0.13 / ABI 14 and both PSP/SMU kill switches set, the macOS
  `metal_frame_proof` program compiled and ran as root. It exited 2 at the
  N33N capability check with `caps 0x00000001`; required HW_VM, HW_GFX and
  METAL bits are absent. The test stopped before Metal device enumeration,
  shader compilation or GPU submission, as designed. QEMU was then stopped.
  Full output: `~/macos tahoe/navi33/runs/20261018-140039/metal-frame-proof.txt`.

- **2026-10-18 14:09–14:10, Metal SDK and device enumeration plus combined
  read-only diagnostics.** A fresh GPU VM boot (run
  `~/macos tahoe/navi33/runs/20261018-140937/`) used kext 0.0.13 / ABI 14,
  the same N33N caps `0x00000001`, and both PSP/SMU kill switches. The new
  macOS SDK check passed (`RGBA8=70`, `BGRA8=80`). The new
  `metal_frame_proof --enumerate-only` mode found **0 Metal devices**; it
  created no queue or command buffer. Normal `metal_frame_proof` still exited
  2 at the caps gate. The same boot's combined diagnostic
  (`results-20261018-141036-guest.txt`) reports reason 11, sOS alive,
  Queue0/Queue1 pointers zero, and both MMHUB/GFXHUB Context0 at
  `0x01fffe00`; no SDMA copy, shader, Metal command or GPU write occurred.
  QEMU was shut down after collection. Tool and outputs:
  `metal-format-sdk-check.txt`, `metal-device-census.txt`, and
  `metal-frame-gate.txt` in that run directory.

- **2026-10-18 14:36–14:37, read-only IOKit accelerator census** (run
  `~/macos tahoe/navi33/runs/20261018-143634/`). The VM again used kext
  0.0.13 / ABI 14, the RX7600 via VFIO with `rombar=0`, and both PSP/SMU
  kill switches. `metal_frame_proof --enumerate-only` reported N33N caps
  `0x00000001`, **zero `IOAccelerator` services**, and **zero Metal devices**.
  The normal proof exited 2 at the same capability gate, before queue,
  pipeline, texture or command-buffer creation. The same boot's full
  diagnostic again reported reason 11, live PSP sOS version `0x00330024`,
  SDMA6 observable with Queue0/Queue1 pointers zero, and both MMHUB/GFXHUB
  Context0 at `0x01fffe00`; all GPU-write counters remained zero. No GPU
  command or register write ran. QEMU logged that VFIO could not reset the
  card, and the guest was shut down cleanly. Evidence:
  `metal-ioaccel-census.txt`, `metal-frame-gate.txt` (inside the guest), and
  `results-20261018-143717-guest.txt` in the run directory.

- **2026-10-18 14:47–14:48, read-only DMA-mapper and Metal repeat** (run
  `~/macos tahoe/navi33/runs/20261018-144711/`). Built a separate
  `OpenCore-navi33-dma-preflight.qcow2` with `navi33-dma-preflight=1`, while
  PSP/SMU/GPU-write and DMA-stage kill switches stayed enabled. The one-shot
  `IODMACommand` preflight returned status 2 (`NoDeviceMapper`), with zero
  segments and pages: macOS has no `IOMapper` for this PCI device in the
  current VM, so the existing device-IOMMU path cannot provide a GPU DMA
  address. This does not rule out a host VFIO mapping; it means this
  guest-side adapter did not obtain one. The macOS SDK format check passed
  (`RGBA8=70`, `BGRA8=80`), while the root-run frame census again found zero
  `IOAccelerator` services and zero Metal devices. `metal_frame_proof`
  exited 2 at caps `0x00000001`; diagnostics remained at reason 11, disabled
  MMHUB/GFXHUB Context0 `0x01fffe00`, and idle Queue0/Queue1. No GPU DMA,
  register write, command, shader or frame was submitted. VFIO could not
  reset the card; the guest shut down cleanly over SSH. Evidence:
  `results-20261018-144752-guest.txt`, `metal-ioaccel-census.txt`, and
  `metal-frame-gate.txt` in the run directory.

- **2026-10-18 14:53–14:57, RX7600 passthrough with guest Intel VT-d**
  (run `~/macos tahoe/navi33/runs/20261018-145308/`). This used
  `run-vm.sh --intel-iommu` with the DMA-preflight OpenCore disk. It did not
  use `--no-gpu`: QEMU passed through `0000:09:00.0` as `vfio-pci` with
  `rombar=0`; the ROM BAR alone was hidden. PSP/SMU and DMA-stage gates
  remained closed. macOS saw QEMU's ACPI DMAR table, but the serial log said
  `AppleVTD is not yet installed as gSystem`; kext 0.0.13 returned status 2
  (`NoDeviceMapper`), zero segments/pages. The read-only diagnostic still
  reported reason 11, Context0 `0x01fffe00`, and idle SDMA Queue0/Queue1.
  The Metal SDK enum check passed; root-run enumeration found zero
  `IOAccelerator` services and zero Metal devices, while the frame proof
  exited 2 at caps `0x00000001` before creating a queue or submitting work.
  No GPU DMA or GPU register write occurred; the guest shut down cleanly
  over SSH. Evidence: `results-20261018-145339-guest.txt` and
  `serial.log` in the run directory.

- **2026-10-18 15:01–15:03, kext 0.0.14 read-only physical-address and
  Metal checks** (run `~/macos tahoe/navi33/runs/20261018-150123/`). CI
  run 39106005328 passed all host tests and both macOS kext builds; its
  macOS 26 artifact loaded with UUID
  `A0D15677-D867-3712-8A80-BA8F50E2ED7D`. The VM used `--intel-iommu`
  and passed the RX 7600 through as `vfio-pci`; it did not use `--no-gpu`.
  The read-only no-mapper preflight returned status 16
  (`PhysicalAddressOnly`), 1 segment and 16 pages, then released the pinned
  64-KiB allocation. Its addresses are CPU-physical candidates only; the
  result does not establish PCIe/GPU reachability. The full diagnostic was
  still reason 11, with PSP sOS `0x00330024` alive, SDMA Queue0/Queue1
  pointers zero, MMHUB/GFXHUB Context0 `0x01fffe00`, and GPU write/queue
  capabilities disabled. The macOS Metal enum check passed (RGBA8 70,
  BGRA8 80); the root-run census found zero `IOAccelerator` services and
  zero Metal devices. `metal_frame_proof` exited 2 at caps `0x00000001`,
  before creating Metal resources or submitting work. No GPU DMA, command,
  shader or register write occurred. The guest shut down cleanly. Evidence:
  `results-20261018-150147-guest.txt`, `metal-proof-checks.txt`, and
  `serial.log` in the run directory.

- **Kext 0.0.14 CPU-physical candidate path:** the opt-in no-mapper check
  pins a 64-KiB physically contiguous descriptor with `kIOMemoryMapperNone`,
  validates CPU-physical page addresses, and builds
  only a temporary CPU shadow PTE image. The result is `PhysicalAddressOnly`;
  macOS CI compiled it and the 15:01 VM run confirmed the status and page
  count. It is not GPU-DMA reachability evidence and does not satisfy the
  Queue1 write gate's owned-VRAM-root prerequisite.

- **2026-10-18 15:16–15:18, RX7600 GPU-passthrough Metal census**
  (run `~/macos tahoe/navi33/runs/20261018-151646/`). This run did **not**
  use `--no-gpu`: `run-vm.sh` reported `gpu=1`, and QEMU's saved command
  line attaches `0000:09:00.0` as `vfio-pci` with `rombar=0` (only the ROM
  BAR is hidden to keep the GOP from switching DCN off). The existing
  OpenCore disk loaded Navi33Probe **0.0.13 / ABI 14**, UUID
  `EB123CF7-DBF5-3860-9CB7-87B6C597B58A`; this was stale relative to the
  0.0.14 artifact used at 15:01, and no kext was changed for this run. The
  proof source at commit `186b82a` compiled in the guest. Root enumeration
  again found zero `IOAccelerator` services and zero Metal devices; the
  strict proof exited 2 at caps `0x00000001`, before its BGRA/GFX11 mapping
  guard, Metal resource creation or GPU submission. The complete read-only
  `run.sh` result remained reason 11, with live PSP sOS `0x00330024`,
  `MMHUB_CTX0=GFXHUB_CTX0=0x01fffe00`, Queue0/Queue1 RPTR/WPTR zero and
  `GpuWritesEnabled=0`. No GPU DMA, command or register write occurred.
  Evidence: `results-20261018-151818-manual.txt`,
  `metal-proof-checks.txt`, `cmdline.txt` and `serial.log` in the run
  directory. The VM was shut down cleanly over SSH.

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
     VBIOS asks).
   - Recovery if the PSP wedges: a full power-off (a warm restart may
     leave the PSP state in place).
3. SMU, IMU, RLC: power the GC block up (stages 9–11). **Progress: all 27
   firmware records were sent with the SMU first, `AUTOLOAD_RLC` succeeded,
   and the SCPM EnableAllSmuFeatures command set RLC boot complete on
   2026-10-18. GC registers now answer, but preflight is reason 11
   (GPU memory/queue ownership not established), and Metal remains disabled.**
   Earlier note: **design
   written** (`src/navi33-bringup/FIRMWARE_LOAD_DESIGN.md`, 2026-10-18, no
   code on the card until 0.0.10): 26 `LOAD_IP_FW` records (SDMA, RS64 CP, MES, IMU, RLC)
   then `AUTOLOAD_RLC`, **SMU image left out by the user's decision**;
   `tools/fwplan.py` computes the records from the real firmware files and
   `tests/fixtures/rx7600-fw-plan.{json,tsv}` is the golden list.
   **Built 2026-10-18 (kext 0.0.10, N33N ABI 11), then physically run with
   kext 0.0.13 / ABI 14 on 2026-10-18:**
   pure parser `src/Navi33Psp13IpFw.h` (tests `test_psp13_ipfw`, which
   compares the real files with the golden list record for record),
   planner and runner `src/Navi33Psp13FwLoad.h` (tests `test_psp13_fwload`,
   fake PSP, seven mutants caught), writer `src/Navi33DarwinPspFw.cpp`.
   Modes `N33_PSP_LOAD_FW_PLAN` (5) / `FW_RUN` (6): records `[start_at,
   stop_after)`, stop 28 adds `AUTOLOAD_RLC`; tool `n33psp --ip-fw-plan
   START STOP --rom .. --fwdir DIR` and `--ip-fw START STOP .. --i-have-ssh`;
   `vm/make-opencore.sh --psp-fw [--psp-autoload]`; `vm/guest-run.sh` now
   copies the eight decompressed files to the guest's `~/navi33/fw`.
   Window: 8 MiB at VRAM 240 MiB: ring pages at +1 MiB, one page-aligned
   slot per record in list order at +4 MiB (2.42 MiB used). The plan needs
   our ring to exist (otherwise `RING_MISSING`), the exact pinned files
   (otherwise `FW_BAD_IMAGES`), and the same console/aperture proof as the
   other stages. This stage ran under the SCPM-sequence approval on
   2026-10-18; further runs need separate approval and its own boot-arg
   (hard rule 2). The KM ring
   (`C2PMSG_64` is already `0x80000000`) and TMR come first, as in
   upstream `psp_hw_start`.
   - **Built 2026-10-18 (kext 0.0.9, N33N ABI 10); ring/TMR run succeeded
     in the VM with kext 0.0.13 / ABI 14 on 2026-10-18.** Pure protocol,
     planner and runner: `src/Navi33Psp13Ring.h`
     (tests `test_psp13_ring.cpp` and `test_psp13_ring_run.cpp`, a fake
     PSP with failure paths and mutation checks). Writer:
     `src/Navi33DarwinPspRing.cpp`. Modes `N33_PSP_LOAD_RING_PLAN` (3) and
     `RING_RUN` (4) of the existing `N33_SEL_PSP_LOAD` selector, with
     `stop_after` 1 (create the KM ring), 2 (+LOAD_TOC) or 3 (+SETUP_TMR).
     Tool: `n33psp --ring-plan N` and `n33psp --ring N --i-have-ssh`.
     Layout (VRAM offsets): fw_pri at 240 MiB holds the TOC copy; the ring
     (4 KiB), command page and fence word sit at 241 MiB; only that 2 MiB
     window is mapped writable. The TMR is written by the PSP alone, at
     FB base + 8128 MiB (the address Linux used on this card), sized by
     the PSP's own LOAD_TOC answer (19 MiB here, not 4 MiB, and not a
     power of two; an earlier `tmrValid` assumed otherwise and was
     corrected before any run). Sequence from upstream: wait
     `C2PMSG_64` ready, write 69, 70, 71, then 64 = KM type << 16, wait
     20 ms and the response flag, then frames through `C2PMSG_67`.
     `psp_update_fw_reservation` does nothing for MP0 13.0.7.
   - The plan needs the sOS alive, `C2PMSG_64` ready, and the ring
     registers either all zero (fresh) or already holding our ring
     (resume). The run refuses to write a register unless the ring pages
     read back as zero first. Open risk: the fence is polled through the
     uncached BAR0 map, and Linux invalidates the HDP read cache there; if
     a fence ever looks stale the symptom is `CMD_TIMEOUT` after the PSP
     did run the command (a rerun then resumes the ring).
   - Command buffers are raw offsets (cmd id +8, body +28, response
     +864; Linux leaves `buf_size`/`buf_version` 0 for ring commands).
     TMR rule: size a non-zero 4 KiB multiple up to 32 MiB, address 1 MiB
     aligned.
   - **Status:** `--ring-plan 3` passed and `--ring 1` (create the KM ring)
     succeeded in the VM on 2026-10-18, then `--ring 2` (LOAD_TOC) and `--ring
     3` (SETUP_TMR) too (findings log). The ring stage is complete.
4. CP, MES, GFX, SDMA (stages 12–15). An SDMA copy is the first real
   GPU work. Upstream Linux drives compute queues through MES on this
   card (`enable_mes` is set for GC 11.0.2, `amdgpu_discovery.c`), not
   the legacy KIQ MAP_QUEUES path. Offline KIQ/MQD planners G18/G19 are
   the fallback. See `DMA_QUEUE_BRINGUP.md` for the MES sequence.
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
