# RX 7600 IP firmware load (design, step 1 of "power up the graphics engine")

Status: **design only. Nothing here has been written to the card.** Written
2026-10-18 after the PSP ring stage (sOS, KM ring, TOC, TMR) was proven in the
Tahoe VM. Every fact below comes from the upstream Linux sources (amdgpu_psp.c,
amdgpu_ucode.c, amdgpu_gfx.c, amdgpu_rlc.c, amdgpu_sdma.c, amdgpu_mes.c,
gfx_v11_0.c, imu_v11_0.c, psp_gfx_if.h) and from decompressing the real
`/lib/firmware/amdgpu/*.bin.zst` files with `tools/fwplan.py`.

**Superseded 2026-10-18 (kext 0.0.11): the SMU image is now record 26, sent first and alone.** Original decision by the user: **the SMU image is left out.** Linux sends it
first (`psp_load_smu_fw`), but this card's SMU already runs from the VBIOS, and
re-sending it is a bigger commitment than the rest. Hard rule 5 applies: the
VBIOS is never modified; this plan never touches it.

## What Linux does after the TMR exists

For MP0 13.0.7 the load type is PSP (`amdgpu_ucode_get_load_type` returns PSP for
the default `-1`), with RLC autoload (`psp->autoload_supported`). In
`psp_load_non_psp_fw()`:

1. (SMU image: **skipped here**.)
2. For every registered ucode in `AMDGPU_UCODE_ID` order: one ring command
   `GFX_CMD_ID_LOAD_IP_FW` (0x6) with `fw_phy_addr` (MC address of the staged
   copy), `fw_size` and `fw_type`. The PSP checks and installs it; its reply
   carries the address inside the TMR.
3. Right after `RLC_G` has been loaded: one `GFX_CMD_ID_AUTOLOAD_RLC` (0x21).
   This is the command that makes the PSP start the RLC autoload, so the GC
   block is brought up. It is by far the biggest step of the sequence.
4. Linux then runs ASD/RL/TA loads; none are needed for compute.

Linux does this in `psp_hw_init`, which is before the SMU's and GFX's own
`hw_init` (IP block order: common, gmc, ih, **psp**, smu, dm, gfx, sdma, ...).
So RLC autoload before the SMU driver init is the normal order on this chip.

## The images (GC 11.0.2, header versions from the real files)

| File | Bytes | Header | Becomes |
|---|---|---|---|
| `sdma_6_0_2` | 34,560 | 2.0 | SDMA_UCODE_TH0, SDMA_UCODE_TH1 |
| `gc_11_0_2_pfp` | 234,944 | 2.0 (RS64) | CP_RS64_PFP + P0/P1 stacks |
| `gc_11_0_2_me` | 324,080 | 2.0 (RS64) | CP_RS64_ME + P0/P1 stacks |
| `gc_11_0_2_mec` | 406,528 | 2.0 (RS64) | CP_RS64_MEC + P0..P3 stacks |
| `gc_11_0_2_mes_2` | 263,696 | 1.0 | CP_MES, CP_MES_DATA (scheduler pipe) |
| `gc_11_0_2_mes1` | 221,408 | 1.0 | CP_MES1, CP_MES1_DATA (KIQ pipe) |
| `gc_11_0_2_imu` | 132,352 | 1.0 | IMU_I, IMU_D |
| `gc_11_0_2_rlc` | 178,848 | 2.3 | RLC restore lists, IRAM, DRAM, RLC_P, RLC_V, RLC_G |

`gc_11_0_2_mes.bin` is only Linux's fallback for the scheduler pipe and is not
used. The full list, 26 records, is `tests/fixtures/rx7600-fw-plan.json`
(offset, size, PSP type and SHA-256 of each record's bytes), produced by
`tools/fwplan.py` and sanity-checked against the headers: each CP image plus its
stack data equals the header's `ucode_size`; the RLC sub-images tile the file
end to end with no gap.

Per-record rules (all copied from `amdgpu_ucode_init_single_fw` and friends):
- RS64 CP: image = `ucode_size_bytes` at `ucode_array_offset`; **each stack is
  the same `data_size_bytes` (139,264 B) at `data_offset_bytes`**, sent twice
  (PFP, ME) or four times (MEC).
- SDMA v2.0: TH0 = `ctx_ucode_size_bytes` at the array offset; TH1 =
  `ctl_ucode_size_bytes` at `ctl_ucode_offset`.
- MES: image and data are separate (`mes_ucode_*`, `mes_ucode_data_*`).
- IMU: IRAM at the array offset, DRAM right after it.
- RLC 2.3: restore lists GPM/SRM (CNTL is empty here), IRAM/DRAM (v2.2),
  RLC_P/RLC_V (v2.3), all at offsets from the file start; RLC_G is the default
  ucode array.

## Staging

Linux copies all records into one buffer (`adev->firmware.fw_buf`, each record
page-aligned) and hands the PSP the MC address of each. Total for this chip:
**2,539,520 B = 2.42 MiB**. We stage the same way in VRAM through BAR0, in its
own writable window, kept clear of the console and every surface by the same
16 MiB rule. Proposed: a 4 MiB window at VRAM offset 244 MiB (the ring window
sits at 240-242 MiB; the TMR is far above, at 8128 MiB and written only by the
PSP). The TMR does not need to be mapped by us.

## What the kext needs (built 2026-10-18, kext 0.0.10, ABI 11; not run on the card)

- A parser (`Navi33Psp13IpFw.h`, pure and host-tested) that produces exactly the
  records in the golden JSON from the eight raw files; every offset and size
  bounds-checked; the eight files pinned by SHA-256 the way `psp_13_0_7_sos.bin`
  is. Test: it must reproduce `rx7600-fw-plan.json` record for record.
- The input limit `N33_PSP_LOAD_MAX_INPUT` was 2 MiB, too small for eight files
  (1,796,416 B); ABI 11 raises it to 4 MiB. The files are sent concatenated in
  `File` order and split by their pinned sizes, so no lengths travel.
- New modes of the same selector (`FW_PLAN` 5, `FW_RUN` 6), its own boot-arg
  (`navi33-psp-fw=1`), its own once-per-boot flag and its own writer file
  (`Navi33DarwinPspFw.cpp`). The allowlist is tighter than the ring stage's: **one
  register, `C2PMSG_67`** (the ring already exists), and two VRAM zones (ring
  pages, staging area) of an 8 MiB window. `navi33-psp-off` kills it too.
- A run sends records `[start_at, stop_after)` (0-based, Linux order), so the
  first run can be just the two SDMA images (`--ip-fw 0 2`). Each record has a
  fixed page-aligned slot in the staging area, whichever range a run covers.
- The plan refuses unless our ring exists (`RING_MISSING`), the files are the
  exact pinned set (`FW_BAD_IMAGES`), the sOS is alive, and the console /
  aperture / clear-window proof holds for the whole 8 MiB window.
- `AUTOLOAD_RLC` is **not** part of loading records: stop value 27 only, and the
  writer refuses it (`AUTOLOAD_DISABLED`, before any record is sent) unless
  `navi33-psp-autoload=1` is set in addition to `navi33-psp-fw=1`. It needs its
  own explicit approval.

## Order of real runs (each needs the user's yes)

1. Plan only (reads): staging window clear, all pins match, ring alive.
2. Load SDMA_UCODE_TH0 and TH1 only: smallest image, a PSP-side check of the
   whole path (staging, address, type, response status).
3. The CP, MES and IMU records.
4. The RLC records except RLC_G.
5. RLC_G.
6. **AUTOLOAD_RLC.** Afterwards the GC block should come out of reset:
   `IMU_GFX_RESET_CTRL` (now `0x10`), RLC and CP status registers become the
   observable result.

## Risks and open questions

- The PSP verifies every image's signature; our files are the unmodified
  linux-firmware ones, so a rejection would show as a non-zero response status
  and stop the run. Nothing is installed on failure.
- Whether the PSP accepts the images while the SMU has not been re-sent is
  unknown. Linux's order always sends the SMU first. If a record is refused for
  that reason, this decision is revisited with the user; nothing is assumed.
- `AUTOLOAD_RLC` powers up GC while the display runs on DCN. Linux does the same
  with the VBIOS-lit display, but we have not. The recovery path is unchanged:
  `sudo poweroff` from the laptop, then the power button; a full power-off also
  resets the PSP.
- The fence is read through the uncached BAR0 map. That worked for LOAD_TOC and
  SETUP_TMR (fence seen on the first poll).
- The MES firmware names assume Linux's choice for GC 11.0.2
  (`mes_2` + `mes1`); `tools/fwplan.py` encodes them.
- No MEC jump-table records appear because the PFP header is v2.0, which makes
  Linux choose the RS64 path (`gfx.rs64_enable`, `gfx_v11_0_init_microcode`); the
  `CP_MEC1/MEC1_JT` records belong to the older v1.0 layout. (Linux would skip
  them anyway when autoload is on, `fw_load_skip_check`.)
