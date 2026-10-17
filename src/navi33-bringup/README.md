# Navi 33 / RX 7600 bring-up (experimental)

This is a **separate RDNA3 bring-up path** in the Cobalt64 fork. Stage 0
implements a real opt-in IOKit service for AMD PCI vendor 1002, device 7480
(Radeon RX 7600). It does **not** implement 2D display, GPU compute,
Vulkan, Metal, or firmware loading.

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

The IOKit property 'Navi33,Stage' must say 'PCI configuration inventory only'.
No visible change to macOS graphics performance is expected. Successful
Stage 0 merely gives a validated entry point for real RDNA3 work.

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

## Next milestones (not implemented yet)

1. Read and parse Navi 33 IP Discovery safely, validating discovered hardware
   versions against Linux amdgpu (GC11, PSP13, SMU13, SDMA6, DCN3.2.1).
   Keep unknown combinations fail-closed.
2. Create independent RDNA3 firmware, GART/GMC, queue/interrupt and GFX11
   implementations. Never call Navi 48 GC12 / PSP14 / SDMA7 register paths.
3. Validate command submission, fence completion and a small gfx1102
   compute shader with readback **before** enabling render/scanout.
4. Adapt Cobalt64's Darwin RADV winsys to Navi 33 device information, then
   Metal and WindowServer integration. Keep GOP fallback for diagnostics.

Base: Almosst-DEV/Cobalt64 (MIT). Linux amdgpu/Mesa are implementation
references, not proofs that this port works. No firmware or Apple binaries
are copied into this directory.
