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

## Stage 4: separate GFX11/DCN3.2.1 backend plan (host tested)

These NEW modules are compiled by both GCC/Clang Linux CI and the macOS
`Navi33Probe.kext`. They are deliberately **metadata-only**, not a
partially-working GPU driver:

- `src/Navi33HardwarePlan.h`: strict per-engine fingerprint for
  GC 11.0.2 (GFX11/GMC11), MP0 13.0.7 (PSP13), MP1 13.0.7 (SMU13),
  SDMA 6.0.2, DMU 3.2.1 (DCN3.2.1). Rejects missing/duplicate/older
  or RDNA4 blocks. Every BAR5-relative register offset is derived from
  supplied IP Discovery and bounds-checked against **assigned BAR5 size**.
- `src/Navi33Dcn321.h`: **18 exact, source-pinned DCN3.2.1 register
  definitions** (DCN VRAM base/top, all four HUBPREQ pitch/surface pairs,
  and OTG frame counters), all using IP Discovery DMU BASE_IDX 2. These
  are potential read offsets only; a Kext does not access these registers.
  Each pipe may be inactive; the plan never assumes pipe0 drives the GOP.
- `src/Navi33Sdma6Packet.h`: offline seven-DWORD SDMA6 linear copy packet
  encoder, following Linux `sdma_v6_0_emit_copy_buffer`. Includes
  alignment, byte-count, overflow, overlap, TMZ-disabled and parser
  round-trip guards. It produces only CPU-owned command bytes; it cannot
  submit any GPU work or touch a doorbell.
- `src/Navi33FirmwareGuard.h`: Linux AMD common firmware-header metadata
  parser for families `gc_11_0_2`, `psp_13_0_7`, `smu_13_0_7`,
  `sdma_6_0_2`. Refuses wrong IP family, malformed header and out-of-range
  payloads. CRC is reported as an **unverified field**, not a trust proof.
  No firmware binaries bundled and no firmware execution path.
- `tools/backend_report.cpp`: read-only offline CLI accepting a full ROM/TMR
  snapshot and the PCI BAR5 resource **size in bytes**:

~~~sh
make -C src/navi33-bringup host-test
src/navi33-bringup/build/backend-report /path/to/full-rom.bin 0x100000
~~~

If input is missing or incompatible, it prints a rejection rather than
guessing register addresses. `Navi33Probe.kext` exposes the inventory
and offset-plan statuses in IORegistry when a full `ATY,bin_image`
snapshot and BAR5 metadata are available, without mapping/readback.

**Important:** None of these modules initializes PSP, SMU, SDMA,
memory translation, interrupts or GFX11 command rings. Their
`kRegisterWritesImplemented` and `kGpuCommandSubmissionImplemented`
guards remain compile-time false. They cannot enable macOS display
or Metal without new hardware-backed code and a recoverable test path.

Sources: Linux `amdgpu_discovery.c`, `amdgpu_ucode.h`, and
`dcn_3_2_1_offset.h`, all under the AMD permissive MIT-style license.

## Stage 6: GMC11/GART software page tables and SDMA6 ring/fences (host tested)

**No real DMA/GPU allocation, no register writes, and no Kext hardware
submission have been added.** These are bounded, self-contained CPU-only
components designed to make the eventual physical backend testable:

- \`src/Navi33Gmc11Gart.h\` implements 64-bit 4 KiB system-memory PTE
  formatting for RDNA3/GMC11: VALID, SYSTEM, SNOOPED, READABLE,
  optionally WRITEABLE, and **MTYPE_UC = 3 at bits 50:48** (not GFX12's
  bit shift). Bus/DMA addresses must be 4 KiB-aligned, already IOMMU
  mapped/pinned by the OS, and within a conservative 48-bit range.
  Pages are written only to a caller-provided **CPU buffer**, transactionally:
  a bad DMA address or already-occupied slot rejects the whole batch.
  Unmapping similarly rejects unrecognized PTEs and leaves the CPU buffer
  unchanged. **GFXHUB/MMHUB setup and TLB flush are not implemented**.
- \`src/Navi33GpuVaArena.h\` is a 64-slot fixed-capacity, first-fit
  GPU virtual-address reservation ledger with power-of-two alignment,
  fragmentation reuse and duplicate-free checks. It does not reserve
  system RAM, pin VRAM, or install page tables into a GPU.
- \`src/Navi33Sdma6Ring.h\` models a power-of-two, caller-owned
  **software** queue of seven-DWORD SDMA6 linear-copy packets. It
  implements atomic batch validation, ring wraparound, backpressure,
  32 outstanding logical fence records, consume/peek and fence
  completion. CPU-only tests copy bytes in test memory after decoding
  packets. No doorbell, GPU-visible command buffer, SDMA microcode,
  interrupts or hardware fence polling exist.

Test on Linux/macOS with GCC/Clang:

~~~sh
make -C src/navi33-bringup clean host-test HOST_CXX=clang++
~~~

For a meaningful GPU execution path we still need an OS DMA mapping
adapter, pinned buffer objects, firmware/PSP authentication and SMU,
GFXHUB/MMHUB VM page table ownership, hardware TLB invalidation,
SDMA6 MQD/context/ring setup, coherency handling and measured GPU
fence/readback. **Do not feed generated PTEs/packets to a GPU yet.**

Source details: Linux \`amdgpu_vm.h\`, \`amdgpu_gart.c\`,
\`gmc_v11_0.c\`, \`amdgpu_ring.c\`, \`sdma_v6_0.c\`, and
\`soc_v1_0_enum.h\`; AMD permissive/MIT-style licenses.


## Stage 7: opt-in macOS IODMACommand preflight (implemented; untested on RX7600)

This is the first macOS **kernel IOMMU / DMA-mapping adapter** (as opposed
to only CPU-simulated bus addresses). No Navi33 hardware registers are
touched, and it remains opt-in because the user's machine has one GPU:

- `src/Navi33DmaScatter.h`: portable bounds-checked, non-aliasing
  scatter/gather IOVA-to-4KiB-page planner. It only accepts OS-provided
  48-bit-aligned 4KiB bus-address segments, and refuses malformed lists
  **without modifying** the target page list or shadow page table.
- `src/Navi33DarwinDma.cpp`: allocates up to **64 KiB of ordinary wired
  system memory** using `IOBufferMemoryDescriptor`, pins it, obtains the
  **device-associated** `IOMapper::copyMapperForDevice`, and creates a
  mapped `IODMACommand`. It enumerates IOMMU I/O bus segments, validates
  and transiently builds GMC11 PTEs in a CPU-only shadow array, tests
  unmap, and releases mappings, memory, and the mapper on all exit paths.
  It **never** writes a GPU MMIO register or publishes a live GART page.
- `Navi33Probe.kext` exposes one-shot status, number of validated bus
  segments/pages, but **not** the actual IOVA addresses.

By default, this adapter is **not invoked**. Even with `navi33-probe=1`
the new code does nothing unless `navi33-dma-preflight=1` is ALSO
present. `navi33-dma-off=1` takes precedence. Do not add these
experimental flags until you have an observable, recoverable macOS boot.

A CI pass demonstrates Kext compile/link and CPU-only scatter-page test
fixtures, **not** that macOS IOMMU mappings are usable by a real RX7600.
A macOS IOMMU mapper may refuse the request, and this stage intentionally
does not bypass it or fall back to unsafe physical addresses.

Hardware GART page tables, GPU VMID initialization, TLB invalidations,
firmware authentication, SDMA queue/doorbell and GFX11 compute remain
disabled. The preflight's `fakeGpuVa` is a constant used ONLY inside
a CPU-owned shadow page-table array: it is NOT assigned to the GPU.


## Next milestones (not implemented yet)

1. Establish a genuinely observable, recoverable native macOS boot on
   the user's single RX7600. OpenCore GOP and AuxKC remain unverified.
2. Validate device-associated IOMMU/IOVA mapping on actual macOS and keep
   the OS bus mapping lifetime valid through GPU execution. Stage 7's
   `IODMACommand` adapter only preflights in software and cleans up.
3. Implement separate RDNA3 firmware/PSP13/SMU13, real GMC11 GART page
   tables, GFXHUB/MMHUB state/TLB updates, SDMA6 rings/doorbells,
   interrupts, PCI DMA-safe synchronization and measured hardware fences.
   Never mix Navi48 GC12/PSP14/SDMA7 routines.
4. Validate actual SDMA6 readback then GFX11 compute; adapt Darwin
   RADV gfx1102 and finally Metal/WindowServer GPU acceleration.
   Preserve GOP output and a boot recovery path.

Base: Almosst-DEV/Cobalt64 (MIT). Linux amdgpu/Mesa are implementation
references, not proofs that this port works. No firmware or Apple binaries
are copied into this directory.

## Stage G2: opt-in N33N macOS diagnostic client (uninstalled)

The separate `Navi33NativeClient` Kext class exposes only HELLO and
QUERY_CAPS (64 bytes) and is gated by `navi33-probe=1` plus
`navi33-client=1`; `navi33-client-off=1` overrides. Root is required.
No GPU engine is started and all mutation/submit selectors refuse requests.
The read-only `tools/n33query.c` example compiles on macOS but is never
executed in CI. This is not a RADV/Metal GPU execution path.
