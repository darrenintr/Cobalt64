# Navi33GOPFB — firmware framebuffer handoff experiment

An independent, opt-in `IOFramebuffer` Kext for RX 7600 (Navi33, `1002:7480`).
Developed within the Cobalt64 RDNA3 port alongside `Navi33Probe.kext`.

**Status:** untested on Navi33/Tahoe hardware. Successful Kext compilation is
NOT evidence that this enables the installer or desktop, and it has NO Metal
or GPU acceleration. This path exists to make future real driver testing
possible on a Ryzen desktop with **no backup iGPU**.

## Technical scope

Borrowing the architecture of
[RDNA4FB](https://github.com/somestupidgirl/RDNA4FB) (BSD-3-Clause; license
included), the Kext reads `IOPlatformExpert::getConsoleInfo` and validates
the firmware scanout physical address, stride, dimensions and depth. It
accepts firmware-reported 24/30/32-bit depths only when the buffer still
has a safe 4-byte/pixel stride. A 30-bit framebuffer is advertised as
32-bit, so colours may be wrong until the DCN format can be identified.
It offers
the **same existing pixel range** to IOGraphics as one fixed software 32bpp
mode. Its `isConsoleDevice()` reports true. No hardware mode setting occurs.

The Kext does **not** reinitialize the DCN display controller, query VRAM
through BAR5, alter PCI COMMAND, map GPU registers, initialize firmware,
submit shaders or enable hardware acceleration. Only the OS may write
software-composited pixels into the provided existing framebuffer aperture.

Requirements for any successful screen: the GOP framebuffer must still
exist, PCI memory decode must already be enabled, and the display engine must
continue scanning out from the same buffer. None can be proved by CI.

The Kext now **refuses** framebuffer handoff when the decoded GOP pixel
buffer falls outside the GPU's *assigned BAR0 resource* reported by
IOPCIFamily. The BAR0 check uses resource metadata only (no MMIO mapping
or hardware writes) and deliberately fails closed on missing/truncated BARs.
This will reject some configurations until memory assignment is fixed.
It is preferable to advertising arbitrary physical memory as GPU pixels.

It registers doze-only power states during `enableController`, matching
RDNA4FB's ordering, and prevents full system sleep until a true DCN3.2.1
resume path exists. This is not full macOS power-management support.

## Boot arguments and recovery

- `navi33-fb=1`: explicit opt-in (default is entirely inert).
- `navi33-fb-off=1`: kill switch, takes precedence even if enabled.
- `navi33-fb-cmap=0..5`: advertised channel order correction, default 0.
- Optional OpenCore GPU DeviceProperties `Navi33,EDID` (raw 128/256-byte
  monitor EDID, checked header, extension count and checksums). Exposes
  this through IOGraphics DDC without touching DP AUX or I2C registers.
  The driver can run with *no EDID*; it reports a generic display.

No **automatic hardware EDID reading**, hotplug, native modeset, VBL,
display sleep or hardware cursor support yet. The Kext now supports
`setStartupDisplayMode` without programming DCN. For 30bpp GOP formats,
colours may be inaccurate. Refresh metadata advertises 60Hz as a placeholder; it
does not measure the actual GOP mode.

This is a framebuffer **component of the larger GFX11 + RADV + Metal port**,
not a substitute for GPU acceleration.

## Offline OpenCore / AuxKC preparation

See [../bootstrap/README.md](../bootstrap/README.md) for generated recovery,
software-fallback and framebuffer-trial configurations, including two
**Tahoe 26 profiles** that disable WhateverGreen only in an alternate
config.plist, plus executable
preflight checks for Kext bundles. Profiles are **not automatically
selectable in the OpenCore picker**, and preflight does not install a Kext.

## Host-side verification (Ubuntu/macOS runners, no GPU)

~~~sh
make -C src/navi33-bringup/display host-test
~~~

## macOS Kext compile only (does not install)

~~~sh
make -C src/navi33-bringup/display MKSDK=/path/to/MacKernelSDK all
~~~

Output: `src/navi33-bringup/display/build/Navi33GOPFB.kext`.

### Bootstrap limitation

Because this Kext depends on `IOGraphicsFamily` in the macOS System KC, it
**cannot simply be injected through OpenCore's Kernel/Add**. Installing into
the macOS Auxiliary Kernel Collection requires an already accessible macOS
environment with an appropriate `kmutil` workflow. Bootstrapping a
completely blank, black-screen installer remains unresolved. For Tahoe,
the **first-choice fallback profile** is `tahoe-firmware`, which disables
WhateverGreen but uses macOS's built-in firmware/IOBootNDRV path: no
custom Kext required at installer time. This avoids a documented Tahoe
AMD connector-patching issue in WhateverGreen, but it is not guaranteed
to rescue a lost GOP or start WindowServer. The `tahoe-fb-trial`
profile requires an already installed and approved Kext in the actual
target installation's AuxKC.

Do NOT try this experimental Kext on the only macOS installation or change
`csr-active-config` without a verified recovery path. When there is a
recoverable Tahoe installation and a working display route, review the
loading method, installer compatibility and macOS security policy first.
`-x` Safe Boot or the kill switch are possible recovery tools, but are
**not guaranteed to work** if macOS itself loses GOP output earlier.

## Future integration

1. Share validated firmware GOP geometry with later RDNA3 display and
   scanout modules, then make the handoff from firmware to native DCN3.2.1.
2. Add real GFX11 firmware/queues and independently validate GPU compute.
3. Connect RADV gfx1102, then Metal and WindowServer GPU compositing.
4. Replace fixed software framebuffer with buffers produced by actual GPU
   rendering, once memory-coherence and scanout ownership are proven.

Nothing here calls Navi48 bring-up routines, links RDNA4 drivers or writes
RDNA4 display registers. The existing Navi48 path is unchanged.
