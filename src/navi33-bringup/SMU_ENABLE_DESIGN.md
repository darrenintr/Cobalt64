# Navi33 SMU feature-enable experiments (kext 0.0.13, ABI 14)

The user approved one SCPM enable-only attempt on 2026-10-18 after the
stable Linux runtime-database capture. This approval covers preparing and
building the new mode, a full power-off, recreation of the PSP firmware
sequence in the VM, and one EnableAllSmuFeatures command. Execution is pending.
The earlier fixed-mask attempt (kext 0.0.12 / ABI 13) rejected HIGH with FD;
that approval is consumed.

This is a test of the post-AUTOLOAD_RLC power-domain hypothesis. Linux's
complete SMU hw_init also includes table addresses and calibration that these
experiments omit; a rejection leaves those prerequisites unresolved.

## SCPM enable-only mode

`n33psp --smu-scpm-plan --rom rx7600-ipd.bin` (mode 9) reads the same
prerequisites as the legacy plan, receives no write callback and does not
consume the latch. `--smu-scpm-enable --rom ... --i-have-ssh` (mode 10)
first runs the plan and sends only EnableAllSmuFeatures (6), argument 0.
It clears MP1 response, writes argument 0, then message 6: three register
writes total. No mask or table messages are submitted. Result
`smu_enable_only=1`, mask fields zero, completed count 0/1 or 1/1 and response
slot 0 identify this sequence; slots 1/2 remain zero. ABI 14 forces tools
and kexts to agree on these modes and the result-field meaning.

The real run requires both `navi33-smu-enable=1` and `navi33-smu-scpm=1`,
with neither SMU nor PSP kill switch set. Opening the SCPM gate closes the
legacy HIGH/LOW/ENABLE gate. Both modes share the provider-wide SMU latch
and lock; switching modes cannot grant another attempt. The isolated VM
OpenCore builder accepts `--smu-scpm` to set these gates. No autorun sends it.

Stop on the first rejected response, timeout, all-ones read or I/O failure.
After acknowledgement, sample IMU core/reset and RLC boot status for at most
one second with no further writes. SMU_ACKED means the command was accepted
but RLC boot-complete stayed clear; SMU_GC_READY means RLC bit 31 was observed.
Capture full before/after values even if GC does not start. This does not
establish queue or compute support.

## Upstream evidence

Studied torvalds/linux at `58161ead29e50d70a317ea26b472e0c7508e69da`:

- `drivers/gpu/drm/amd/pm/swsmu/smu13/smu_v13_0.c`,
  `smu_v13_0_set_allowed_mask`: sends HIGH before LOW;
  `smu_v13_0_system_features_control`: EnableAllSmuFeatures with argument 0.
- `pm/swsmu/smu13/smu_v13_0_7_ppt.c` (under the same `drivers/gpu/drm/amd/`):
  `smu_v13_0_7_init_allowed_features`, message map and mailbox selection.
- `pm/swsmu/inc/pmfw_if/smu13_driver_if_v13_0_7.h`: feature bit numbers.
- `pm/swsmu/inc/pmfw_if/smu_v13_0_7_ppsmc.h`: commands 5, 4, 6; OK=1,
  rejected-busy=FC, rejected-prerequisite=FD, unknown=FE, failed=FF.
- `pm/swsmu/smu_cmn.c`: clear response, write argument, write message,
  poll nonzero response. We stop on every non-OK answer, without retrying.
- `include/asic_reg/mp/mp_13_0_2_offset.h`: MP1 segment 0 C2PMSG_66=0x282,
  _82=0x292, _90=0x29a. With discovered MP1 base 0x16000, BAR5 byte offsets
  are 0x58a08, 0x58a48, 0x58a68. These are MP1, not MP0's PSP mailboxes.
- `include/asic_reg/gc/gc_11_0_0_offset.h`: GC segment 1 GFX_IMU_SCRATCH_0/1
  0x4068/69, GFX_IMU_CORE_STATUS 0x40b7, GFX_IMU_GFX_RESET_CTRL 0x40bc,
  RLC_RLCS_BOOTLOAD_STATUS 0x4e82.
- `amdgpu/amdgpu_drv.c` defaults `ppfeaturemask` to 0xfff7bfff;
  `include/amd_shared.h` puts all seven relevant policy flags in that mask.
- `amdgpu/soc21.c`, GC 11.0.2: ATHUB and MMHUB power gating are both supported.

[Upstream SMU13 source](https://github.com/torvalds/linux/blob/58161ead29e50d70a317ea26b472e0c7508e69da/drivers/gpu/drm/amd/pm/swsmu/smu13/smu_v13_0.c)

## Legacy fixed sequence and mask

Default Linux policy (`ppfeaturemask=0xfff7bfff`, all seven relevant options on) and Navi33's two hub
power-gating flags gives `0x0001a3a471fe7fff`. Bits: 0..14, 17..24, 28..30,
34, 37, 39..41, 45, 47, 48. In particular bit 37 enables GFX_IMU.
This is a source-derived baseline, **not a captured mask from this card's
Linux reference boot**. Runtime policy overrides could produce another mask.
There is no user-supplied mask or arbitrary-message interface.

1. SetAllowedFeaturesMaskHigh (5), argument 0x0001a3a4.
2. SetAllowedFeaturesMaskLow (4), argument 0x71fe7fff.
3. EnableAllSmuFeatures (6), argument 0.

Exactly nine successful register writes if all messages are accepted. Each
message waits up to 1000 ms for a response; all-ones/read failure stops
immediately. No resubmission, rollback, reset, or follow-up SMU messages.

## Plan, gates and observation

`n33psp --smu-plan --rom rx7600-ipd.bin` reads only BAR5. It has no write
callback, maps no VRAM, and needs no firmware payload (ABI carries one reserved
zero byte). IP Discovery must match Navi33, sOS must be alive, SMU response
must be 1, and the observed post-autoload state must be:

- IMU scratch 0/1 = 0x0b214700 / 0xbabeface.
- IMU core = 2, reset control = 0x10, RLC boot status = 0.

These checks establish the recorded stalled state; they cannot prove that
all firmware records were loaded. The operator must retain the successful
PSP sequence logs (SMU image first, other records, AUTOLOAD_RLC). The state
survives a VM restart; a provider-local autoload flag would reject the
existing workflow, so it is deliberately not used as proof.

`--smu-enable --rom ... --i-have-ssh` first runs that same plan. A real run
also needs `navi33-smu-enable=1`, no nonzero `navi33-smu-off`, no nonzero
`navi33-psp-off`, and the usual hw-probe/client gates. It has an independent
provider-wide one-shot latch, consumed before the first possible write,
under the same lock as the PSP stages. Rechecks occur inside the lock.
Neither run.sh nor boot autorun invokes it.

After three OK responses, read core/reset/RLC for at most one second.
State 38 (SMU_ACKED) means the messages were accepted **but bit 31 is still
clear**; the CLI exits nonzero. State 39 (SMU_GC_READY) means RLC bit 31 was
observed. Full raw before/after values and per-message responses are printed.
A set bit alone does not prove compute, queues, or every reset is released.

## Limits and recovery

Linux first sets driver/tools/log table addresses, transfers its pptable
(unless SCPM handles it), and runs boot-time calibration. This experiment
omits those steps. Firmware can reject the enable command, or acknowledge
it without starting GC; that does not refute the broader SMU-init hypothesis.
Power/clock changes can interrupt the only display GPU. No table allocations,
VRAM writes, calibration, power limits, VBIOS changes or additional commands
are included in this stage.

Before a separately approved real run: ensure host SSH recovery works, keep
serial capture running, power off fully to establish fresh card state, and
recreate the approved PSP sequence. After any SMU write attempt (even partial
or failed), close the SMU gate in the next boot configuration and **power off
fully again before another attempt**. Do not detach the card from amdgpu.

The read-only plan can run before the PSP sequence and report the missing
preconditions without changing the card. A PLAN_OK readiness result requires
the observed stalled post-autoload state.

## Hardware result and next diagnostic (2026-10-18)

After the user's confirmed full power-off, kext 0.0.12 / ABI 13 recreated
sOS, ring/TMR, SMU image first, remaining 26 records, and AUTOLOAD_RLC, with
successful responses and fences. The SMU plan passed with core 2, reset
0x10, RLC 0, scratch 0x0b214700/0xbabeface, and response 1.

`~/macos tahoe/navi33/runs/20261018-131339/smu-enable-run.txt` records
state 37: HIGH (message 5, argument 0x0001a3a4) returned 0xFD
(`PPSMC_Result_CmdRejectedPrereq`), after three register writes and 1 ms.
LOW and ENABLE were not sent. The full post-attempt snapshot shows no GC
window changed. Guest SSH survived. Guest shut down, PSP and SMU kill
switches restored; full power-off required before another attempt.

**Correction to the baseline assumption:** Linux's
`smu_init_driver_allowed_feature_mask()` explicitly says that allowed-mask
messages are not permitted under SCPM. `smu_smc_hw_setup()` skips both
allowed-mask messages and PPT transfer under that policy, while still
calling `smu_system_features_control(true)` later. The default policy mask
above is therefore conditional on SCPM being disabled. We did not measure
that precondition. FD alone does not prove SCPM is enabled, or identify
which prerequisite was rejected; enabling features was never tested.

Sources at the same pinned revision:
[SMU policy branches](https://github.com/torvalds/linux/blob/58161ead29e50d70a317ea26b472e0c7508e69da/drivers/gpu/drm/amd/pm/swsmu/amdgpu_smu.c),
[PSP database lookup](https://github.com/torvalds/linux/blob/58161ead29e50d70a317ea26b472e0c7508e69da/drivers/gpu/drm/amd/amdgpu/amdgpu_psp.c),
[database layout](https://github.com/torvalds/linux/blob/58161ead29e50d70a317ea26b472e0c7508e69da/drivers/gpu/drm/amd/amdgpu/amdgpu_psp.h).

Read-only runtime-database capture command:

```sh
sudo python3 -I src/navi33-bringup/tools/scpm-read.py --capture
```

This reads two 64-KiB snapshots of the PSP runtime DB at the driver's VRAM
size minus 1 MiB through a read-only BAR0 mapping, and saves them with a JSON
summary in `~/macos tahoe/navi33/host/scpm-<timestamp>/`. It requires the exact
RX 7600 on amdgpu, validates BAR visibility, version, bounds and directory
padding, and preserves undocumented SCPM status values in the summary and raw
capture. It refuses aperture changes. An absent database or missing entry is
reported as unknown, not proof that masks are permitted.
The initial offline tests cover padding, all documented SCPM states, absent
entries, truncation, invalid versions/counts, duplicates and overlap. No new
kext/ABI change or hardware writer was introduced for this diagnostic.

The capture completed at
`/home/darren/macos tahoe/navi33/host/scpm-20261018-132450` on boot
`a8a50688-4c92-4b5a-a228-7df4e4f6d464`. The two 64-KiB files are byte-for-byte
identical, with SHA-256
`efd6400e63139a9b70fc1345875fe69efb721c1df30302f73f34b82496741ba5`. The
database has a valid cookie and version, and one SCPM entry (type 6) at offset
`0x208`, size 4. Its bytes are `00 00 00 40`, the little-endian value
`0x40000000`.

The pinned Linux enum documents only 0, 1 and 2, so the exact meaning of this
value is unknown. Linux nevertheless sets `scpm_enabled` for any nonzero
status. Its SMU setup skips allowed-feature-mask messages under SCPM, making
this the leading explanation for the earlier HIGH response `FD`. The raw
status does not prove which prerequisite the SMU rejected. The subsequent
user approval above covers one enable-only attempt. The read-only capture
made no GPU writes.

Local verification: full GCC and Clang ASan/UBSan host suites, bootstrap and
display tests, kernel syntax checks with MacKernelSDK, and seven caught
mutants (plan writes, bypassed gate/latch, message order, missing IMU bit,
ignored rejection, wrong IMU precondition).

SCPM implementation checks: the complete GCC host suite, the SMU protocol
tests under Clang ASan/UBSan, and all 31 bootstrap tests passed locally. Tests
check the exact three-write trace (message 6, argument 0), error stops, shared
latch across modes, zero-write plans and mutually exclusive boot gates.
The macOS kext/tools build is performed by GitHub Actions.
