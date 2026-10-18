# Navi33 SMU feature-enable experiment (kext 0.0.12, ABI 13)

Implementation and read-only planning approved on 2026-10-18. **No SMU write
run is approved.** This is a test of the post-AUTOLOAD_RLC power-domain
hypothesis, not an implementation of Linux's complete SMU hw_init.

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

## Fixed sequence and mask

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
the observed stalled post-autoload state. Hardware execution is still pending.

Local verification: full GCC and Clang ASan/UBSan host suites, bootstrap and
display tests, kernel syntax checks with MacKernelSDK, and seven caught
mutants (plan writes, bypassed gate/latch, message order, missing IMU bit,
ignored rejection, wrong IMU precondition).
