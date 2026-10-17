# Navi33 gfx1102 → RADV → Metal development contract

This is NOT a working macOS acceleration driver. The Navi48 Metal bundle and
Mesa patches are hard-wired to gfx1201, the N48N user-client ABI and the Navi48
bringup classes. Pointing that bridge at an RX7600 without a real gfx1102
kernel backend would fabricate GPU capabilities and may hang the single GPU.

Stage G0 (this branch): isolate the N33N ABI and offline GFX11 PM4 framing.
The ABI is generation-specific, 64-byte query shape, zeroed reserved fields,
and explicitly advertises **no** hardware VM, SDMA, GFX, DCN or Metal. The
portable code can build on Linux and macOS but does not touch a GPU.

## Real blockers, in dependency order

1. Validated device ownership / BAR mapping and IP Discovery on the live RX7600,
   preserving GOP and a known-good boot path. Build on Navi33Probe with an
   independent, opt-in N33N IOKit user client, not the Navi48's N48N.
2. Load the correct Navi33 PSP13/SMU13/GFX11/SDMA6 firmware, clocks and queues;
   keep Navi48's PSP14, SDMA7, GFX12 routines entirely separate.
3. Live device-associated DMA ownership, GMC11/GFXHUB/MMHUB page tables, TLB
   invalidation and an SDMA6 copy + fence completion verified by readback.
4. GFX11 MEC/CP queue allocation, packet/IB submission and kernel fence
   retirement; only then advertise N33_CAP_HW_VM | N33_CAP_HW_GFX.
5. Make RADV's Darwin backend enumerate the real gfx1102 with queried chip
   properties and the N33N transport; do not set RADV_DARWIN_FAKE or import
   the gfx1201 canned GPU metadata.
6. Adapt Metal AIR→SPIR-V translation and the Metal device bundle to the
   validated Vulkan device; test actual GPU draw, compute, IOSurface and
   display composition. Never advertise N33_CAP_METAL prematurely.
7. Native DCN3.2.1 modesetting/link encoder, IOFramebuffer and WindowServer
   integration; the current independent DCN321 PR remains under review.

The order matters. A Metal bundle without a GPU-backed Vulkan device is not
hardware acceleration. Host CI is only syntax/contract verification.
