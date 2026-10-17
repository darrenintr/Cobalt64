# Navi 33 / RX 7600 DCN 3.2.1 native modesetting — first integration stage

This is an **independent, fail-closed DCN321 register layer**, not a loadable RX 7600 kext and not a claim of working GOP-free output. It is intentionally **not included** in Navi48Bringup.kext: that driver targets Navi48/gfx12, uses different DCN 4.1 segment values, firmware, memory and power management.

## Verified code in this branch

- Extracted exact per-instance register offsets and masks from Linux amdgpu's `dcn_3_2_1_offset.h` and `dcn_3_2_1_sh_mask.h` (SHA identifiers in `dcn321_regs.h`). Offsets are DWORD units relative to **discovered DCN segment 2**, never hard-coded absolute BAR addresses.
- A host-portable MMIO abstraction with strict BAR bounds, Navi33 PCI ID checks, reserved scanout address bounds and default-off write gate.
- Timing register programming for a **previously disabled OTG**, with Linux OPTC's blank/sync formulas. Does not modify the active pipe or enable it.
- Read-only frame counter and active-pipe-only primary address latch (high DWORD before low DWORD), requiring already configured and pinned VRAM plus plane/format.
- Host MMIO mock tests and GCC/Clang-compatible C11 and C++17 CI targets.

## Remaining steps before GOP-independent display

1. A **separate Navi33 IOKit PCI kext** that attaches to a verified RX 7600 PCI function, maps its MMIO BAR and loads *Navi33-specific* PSP/SMU/DCN firmware.
2. IP discovery for DCN321 plus PSP/SMU/DMUB state validation and supported connector mapping via ATOM BIOS.
3. Pixel clock/DCCG and OPTC clock initialization, DPP, HUBP VM, watermarks, plane format, pitch and tiling, MPCC/OPP routing.
4. EDID/HPD and DP AUX/link training or HDMI encoder/TMDS PHY programming, safe blank/unblank.
5. A transaction manager that snapshots registers, performs disabled-pipe modesetting, checks scanout clock and vertical counters, and rolls back on timeout **before any GOP teardown**.
6. A macOS IOFramebuffer bridge and independent framebuffer allocation/lifetime handling; GOP independence must be tested at boot, not inferred from registers compiling.

**DO NOT** manually flip `armed` on a live single-GPU macOS system at this stage. The library exposes this gate for integration testing, but does not yet guarantee safe restoration of clocks or link training, and a failed modeset can leave a black screen. It is not enabled by OpenCore and no installer is provided.

## Host test

```sh
cc -std=c11 -Wall -Wextra -Werror -pedantic \
    src/dcn321/dcn321.c src/dcn321/tests/test_dcn321.c -o /tmp/test-dcn321
/tmp/test-dcn321
```

Linux reference: `drivers/gpu/drm/amd/display/dc/resource/dcn321/dcn321_resource.c`, `optc/dcn10/dcn10_optc.c`, `hubp/dcn20/dcn20_hubp.c`. The Linux source is not linked into this library.
