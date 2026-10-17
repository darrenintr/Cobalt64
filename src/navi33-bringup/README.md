# Navi 33 / RX 7600 bring-up (experimental)

This is a **separate RDNA3 bring-up path** in the Cobalt64 fork.

- `Navi33Probe.kext`: opt-in read-only PCI and supplied-ROM IP inventory for
  Radeon RX 7600 (1002:7480). No display or GPU compute.
- `display/Navi33GOPFB.kext`: a separately gated, software-only framebuffer
  adopter intended to bridge UEFI GOP output to macOS WindowServer.
  **Untested on Navi33 hardware; screen output is NOT established.**
- Neither component submits GFX11 commands or implements RADV/Metal.

## Stage 0 behavior and safety boundary

- The kext's *only* PCI personality matches 0x74801002, and probe() further
  verifies the PCI vendor/device and display base class.
- It refuses to attach unless the exact OpenCore boot argument
  **navi33-probe=1** is present. Without this flag, it is inert.
- On attach it reads PCI configuration registers for identity, class,
  revision and subsystem, plus IOPCIFamily's existing BAR0/BAR2/BAR5
  **resource lengths**. It records these as IORegistry properties.
- It **never** enables PCI memory or bus mastering, maps BAR memory, reads
  MMIO registers, writes VRAM, issues GPU commands, loads firmware, touches
  PSP/SMU, changes display timings, or installs an accelerator/Metal bundle.
- Navi48Bringup remains unchanged and will not bind to this PCI device.
- IOMatchCategory is separate from the framebuffer/accelerator categories.
  Nevertheless, experimental kexts can still affect boot stability:
  back up your EFI and use a recovery boot entry.

The IOKit property 'Navi33,Stage' states that the driver is inventory-only.
No visible change to macOS graphics performance is expected. It is not a
GPU compute / render driver.

## Stage 1: read-only IP Discovery verification (implemented)

- Adds a new bounds-checked `Navi33Discovery.h` that scans supplied AMD
  flash/TMR snapshots. Checks binary and per-table checksums, offsets, die
  count, record lengths and 32/64-bit register bases. It never calls hardware.
- Looks for the optional `ATY,bin_image` OSData property on the PCI provider.
  If absent, the kext only reports that no ROM was supplied; it does not
  attempt to fetch it from PCI ROM, MM_INDEX/MM_DATA or VRAM.
- Reports discovered GC/SDMA/MP0/MP1/DMU versions in IORegistry. The target
  fingerprint is GC 11.0.2, SDMA 6.0.2, MP0/MP1 13.0.7, DMU 3.2.1. A version
  match identifies a supplied snapshot; it NEVER authorizes register writes.
- Host-side test fixtures include truncated/corrupt blobs, v1/v2 binary
  headers, v3/v4 IP structures, 32/64-bit bases and 4,000 mutations.

Offline snapshots can be inspected without booting macOS:

~~~sh
make -C src/navi33-bringup host-test
src/navi33-bringup/build/discovery-dump /path/to/full-rom-or-tmr-snapshot.bin
~~~

These are synthetic host tests, not measured results from a real RX 7600.
The driver must NOT load/execute Navi48 firmware or GPU commands.

## Host tests (Linux, macOS, CI)

Requires a C++17 compiler, Python 3, and make. Run:

~~~sh
make -C src/navi33-bringup host-test
~~~

These tests verify the PCI identity logic, manifest and read-only-source
guards without opening a PCI device.

## Optional macOS build (not an installation instruction)

On a **separate recoverable macOS installation** with Xcode Command Line Tools
and MacKernelSDK, compile the kext with:

~~~sh
make -C src/navi33-bringup MKSDK=/absolute/path/to/MacKernelSDK all
~~~

The output is an **uninstalled** build/Navi33Probe.kext. The code has not
been boot-tested on an RX 7600, and building it alone does not prove that the
kext will load on a particular Tahoe build. Do not add it to your only
bootable EFI until the build is reviewed and a recovery route is ready.


## Stage 2: GOP framebuffer handoff prototype (independent Kext)

Source, license and build instructions: [display/README.md](display/README.md).

CPU-only host tests:

~~~sh
make -C src/navi33-bringup/display host-test
~~~

Build the independent framebuffer Kext with the macOS SDK (without installing):

~~~sh
make -C src/navi33-bringup/display MKSDK=/path/to/MacKernelSDK all
~~~

It is **not** an OpenCore-injectable Kext because IOGraphicsFamily is a
System KC dependency. The macOS installer/Recovery display path is a
separate bootstrapping problem. The Kext cannot rescue a system that loses
UEFI GOP framebuffer access *before* the macOS Aux KC is loaded.

Boot argument \`navi33-fb=1\` opts in; \`navi33-fb-off=1\` disables it.
Until real Navi33 hardware tests and an installation/recovery strategy have
been demonstrated, the framebuffer remains experimental and should not
be installed on a single-GPU production machine.

## Stage 3: bootstrapping and recovery tooling (implemented offline)

Source: [bootstrap/README.md](bootstrap/README.md).

- `boot_profiles.py` creates **separate** OpenCore recovery,
  WhateverGreen `-radvesa` baseline, and GOP framebuffer trial configurations.
  It does not edit your active `config.plist` or any EFI partition.
- `auxkc_preflight.py` inspects the RX 7600 framebuffer Kext's Info.plist,
  x86_64 Mach-O and System KC dependency before any possible AuxKC staging.
- `auxkc_stage.py` implements **explicitly authorized** installation into
  the **currently running, recoverable test macOS** via `kmutil load -p`,
  with a dry-run default; it cannot magically stage a separate bare-metal
  target from an unrelated macOS VM.
- The GOP bridge now validates that firmware scanout lies within PCI BAR0
  and registers doze-only power states without touching DCN3.2.1 registers.
- macOS CI compiles and signs both independent Kexts and stores build
  artifacts for download. CI does **not** install, boot, or exercise RX 7600.

**Unsolved:** getting this IOGraphics-dependent Kext into the target macOS
AuxKC before a bare-metal first boot, and ensuring Tahoe's firmware/installer
keeps GOP pixels visible. Real GFX11, RADV and Metal are still pending.

## Next milestones (not implemented yet)

1. Provide safe runtime IP Discovery acquisition from real Navi33 TMR/flash,
   with strict bounds, isolation and recovery. Current only parses **supplied** snapshots.
2. Build separate RDNA3 firmware, GMC/GART and GFX11 queues, interrupts and
   power management. Never call Navi48 GC12 / PSP14 / SDMA7 routines.
3. Validate fences and a real gfx1102 compute dispatch/readback.
4. Adapt RADV Darwin winsys to Navi33; then Metal and WindowServer.
   Keep the GOP display for recovery.

Base: Almosst-DEV/Cobalt64 (MIT). Linux amdgpu/Mesa are implementation
references, not proofs that this port works. No firmware or Apple binaries
are copied into this directory.
