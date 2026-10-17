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
  **separate, read-only** kext (`Navi33Probe`) with its own user-client
  ABI ("N33N"). No shared Navi48 sources. Never reuse RDNA4 constants
  (GC12, PSP14, DCN4); Navi33 is GC 11.0.2, PSP 13.0.7, DCN 3.2.1.
- `src/dcn321`, `src/dcn41`: display-engine helpers per generation.
- Key Navi33 docs: `src/navi33-bringup/PHYSICAL_EXECUTION.md` (what is
  proven on hardware, what is still blocked), `ACCELERATION.md`,
  `DMA_QUEUE_BRINGUP.md`.

## Navi33 hard rules

1. **The kext is read-only.** No GPU register writes, no firmware
   loading, no command submission. `Navi33HardwarePlan.h` enforces this
   with `static_assert`s; do not weaken them.
2. **The RX 7600 is the only display GPU in the test Mac.** A write
   stage (starting with the PSP sOS load) needs the user's explicit
   approval to write to that card, plus a recovery path (for example SSH
   into the Mac). Ask first; never assume.
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
  from `src/navi33-bringup/usb/`, and the results files.

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
  `n33compute --rom/--probe/--queue`, `n33vram`, `n33peek --all`, ioreg
  and this boot's kernel log.
- It writes `navi33/results-YYYYMMDD-HHMMSS-{manual|boot}.txt` (results
  before kext 0.0.5 have no suffix).
- Optional boot autorun: `sudo sh /Volumes/OPENCORE/navi33/install-autorun.sh`
  installs a LaunchDaemon that runs `run.sh` once per boot from the
  pinned stick, then chimes. Its log is
  `/var/log/cobalt64-navi33-autorun.log`. If macOS privacy blocks it, the
  log says `BLOCKED`; grant Full Disk Access to
  `/usr/local/libexec/cobalt64/n33autorun`.
- **The user hands results over by pasting only a results file path.**
  Read it, compare it with the previous results file, and interpret it.
  Then record any new hardware fact in the findings log below.

**Boot-args on the Mac** (in the OpenCore config): `navi33-probe=1
navi33-client=1 navi33-hw-probe=1`, plus kill switches `navi33-dma-off=1
navi33-dma-stage-off=1 navi33-fb-off=1 navi33-psp-off=1 navi33-sdma-off=1`.
- Only `n33compute --rom` and `n33peek` (both gated by `navi33-hw-probe=1`)
  return hardware data.
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
   - Precondition: `C2PMSG_35` = `0x80000000`. Met on 2026-10-17 (kext
     0.0.5), with no sOS loaded and the SMU already running.
   - Navi48 stages firmware in VRAM through BAR0, away from the GOP
     console (`stagePSP` in `Navi48Bringup.cpp`). On the RX 7600 the
     PSP sees that VRAM at MC `0x80_0000_0000` + offset.
3. SMU, IMU, RLC: power the GC block up (stages 9–11).
4. CP, MES, GFX, SDMA (stages 12–15). An SDMA copy is the first real
   GPU work.
5. PM4 fence and compute dispatch (stages 16–17).

**Gate before step 2:** the user's explicit approval for GPU writes on
the only display GPU, and a recovery path. Each write stage must be
opt-in by its own boot-arg, like Navi48.

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
