# Navi33 boot bootstrap: recovery before experimentation

**Boot profile and recovery tools, NOT a verified GPU driver.** For
Tahoe 26 the highest-priority initial candidate is the existing macOS
firmware framebuffer with WhateverGreen disabled, not a third-party Kext
injected into an installer before IOGraphics is available.

The RX 7600 is the only GPU on a Ryzen 5 3600 test machine. It has no macOS
accelerated driver, and a blank-screen installer cannot be assumed usable.
The tools here let us audit a future **recoverable** test without touching
the current EFI or macOS installation.

## 1. Generate five OpenCore configurations (offline; safe)

~~~sh
python3 src/navi33-bringup/bootstrap/boot_profiles.py \
  --config /path/to/EFI/OC/config.plist \
  --output-dir /some/other/directory/navi33-profiles
~~~

Outputs:
- `recovery.plist`: original boot-args except experimental Navi33 flags,
  with `navi33-fb-off=1 navi33-dma-off=1`. This disables both the custom
  framebuffer and optional IOMMU preflight if they have been installed.
  It **cannot** recover a screen that died before a Kext loaded.
- `vesa-baseline.plist`: adds `-radvesa` and `navi33-fb-off=1`. It is a
  diagnostic for the WhateverGreen software-display path, **not** evidence
  that RX 7600 will show the installer or WindowServer.
- `fb-trial.plist`: adds `-radvesa navi33-fb=1`. This can only work
  after Navi33GOPFB has been added to the **target macOS AuxKC**.
- `tahoe-firmware.plist`: **first-choice software display experiment**
  for Tahoe 26. Disables only WhateverGreen in the generated `Kernel/Add`;
  strips `-radvesa`, sets `navi33-fb-off=1` and leaves the original macOS
  firmware fallback to drive the display. It does not require our Kext.
- `tahoe-fb-trial.plist`: same Tahoe/WEG-disabled fallback but enables
  Navi33GOPFB with `navi33-fb=1`, **only if** already approved and
  present in this target installation's AuxKC.

All profiles also set `navi33-dma-off=1`, including the trial profile.
They do **not** automatically invoke the `navi33-dma-preflight=1` IOMMU
mapping experiment. That experiment is separately opt-in and only
meaningful after a macOS kernel boot with a working, recoverable display.

The tool **never overwrites the input** and refuses output-name collisions.
Profiles preserve SMBIOS, AMD kernel patches, DeviceProperties, and existing
SIP settings. They also ensure OpenCore NVRAM/Delete includes `boot-args`,
so old persisted boot flags do not silently supersede profile flags.

**Important:** These five files are not independent entries in the OpenCore picker.
OpenCore reads one `EFI/OC/config.plist` at a time. To use alternatives,
prepare genuinely independent bootable EFI environments (or a way to restore
the previous configuration) first. Do not copy multiple named plists into
one EFI and expect the picker to switch between them.

The `-radvesa` boot arg comes from [WhateverGreen](https://github.com/
acidanthera/WhateverGreen); its effect requires an active compatible
WhateverGreen Kext. It disables AMD acceleration; it does not magically
create an RX 7600 graphics driver.


### Prepare three independent EFI directory copies without touching your EFI

On Windows, Linux or macOS, copy your working EFI folder elsewhere first,
then run:

~~~sh
python3 src/navi33-bringup/bootstrap/build_test_efis.py \
  --source-efi /path/to/your/copied/EFI \
  --output-dir /path/to/new/navi33-test-efis
~~~

Input must contain BOOT/BOOTx64.efi, OC/OpenCore.efi and OC/config.plist.
The script produces \`recovery/EFI\`, \`tahoe-firmware/EFI\` and
\`tahoe-fb-trial/EFI\` beneath the new output directory; it never edits
the original EFI or writes a USB partition. Each is **a separate boot
environment**, not a separate entry in a single OpenCore picker.
After inspecting the generated files, copy only one EFI folder to the
root of an independent FAT32 boot USB. Keep an unmodified recovery USB.

The first experiment should use \`tahoe-firmware\`: no custom framebuffer
Kext or AuxKC installation is required, and only the alternate config's
WhateverGreen entry is disabled. It relies on UEFI GOP surviving into
macOS's built-in framebuffer; whether RX 7600 actually outputs pixels is
still unknown until a physical boot with a connected monitor.


## 2. Check compiled Kext prior to any AuxKC operation

~~~sh
python3 src/navi33-bringup/bootstrap/auxkc_preflight.py \
  --kext src/navi33-bringup/display/build/Navi33GOPFB.kext
~~~

Checks the plist's sole RX 7600 PCI personality, IOGraphics dependency and
x86_64 Mach-O binary. It **does not install or sign anything**, or claim
the macOS Tahoe installer will display a UI.

Apple's post-Big-Sur model prelinks third-party Kexts in an Auxiliary Kernel
Collection and ordinarily involves approval + rebuilding + restarting.
`Navi33GOPFB` depends on `IOGraphicsFamily` in the **System** KC and is
not safely OpenCore-injectable in `Kernel/Add`. The installer framebuffer
handoff will remain unresolved until a **target macOS installation** can
load the AuxKC. A Kext built inside the user's existing macOS QEMU guest
will NOT appear automatically in a separate bare-metal installation.

Do not attempt to disable SIP, patch SSV / BootKC or move Kexts around in
Preboot just to make the installer start: those are high-risk changes with
no proven Navi33 display path yet.

## 3. Future live test-volume AuxKC staging (explicit opt-in)

`auxkc_stage.py` is executable code, but **defaults to dry-run**. It makes
no changes unless `--stage-live` and an explicit acknowledgement are both
supplied. On Linux CI, actual stage execution is refused.

Safe plan (Linux or macOS):

~~~sh
python3 src/navi33-bringup/bootstrap/auxkc_stage.py \
  --kext src/navi33-bringup/display/build/Navi33GOPFB.kext
~~~

For a future **booted, recoverable x86_64 TEST macOS installation only**,
after valid Kext-signing/approval prerequisites are established:

~~~sh
sudo python3 src/navi33-bringup/bootstrap/auxkc_stage.py \
  --kext src/navi33-bringup/display/build/Navi33GOPFB.kext \
  --stage-live --acknowledge-this-volume THIS_RUNNING_TEST_MACOS
~~~

It verifies the Kext's Info.plist, Mach-O CPU type, code signature, and
destination availability. It refuses to overwrite an existing installation.
It stages a root-owned Kext using a same-volume temporary directory and asks
`kmutil load -p` to initiate the running macOS approval/AuxKC process.
It **never** reboots, disables SIP, patches the System/Boot KC, touches EFI,
or claims that the driver is now loaded. macOS may reject the CI artifact's
ad-hoc code signature; an authorized Kext-signing certificate is normally
needed under standard security policy. `--allow-adhoc-test` only bypasses
this script's signature-type guard, NOT macOS security enforcement.

**VM limitation:** this command stages on the current running macOS volume
only. A Kext staged on your separate Tahoe VM does not get installed into
an unrelated future physical Hackintosh disk. Preparing the actual bare-metal
target using the *same* bootable macOS installation/disk is a separate step,
and must not be improvised by copying random AuxKC/Preboot files.

### Recovery

If a future test cannot display, the independently prepared rescue OpenCore
profile disables `Navi33GOPFB` via `navi33-fb-off=1`, if execution reached
the Kext at all. It does not make unsupported hardware gain a basic display.
To remove a staged Kext, boot that target macOS via a known-good recovery
route, remove the exact `/Library/Extensions/Navi33GOPFB.kext` bundle after
backup, and rebuild the AuxKC through `kmutil`. Avoid removing other Kexts.

## 4. Validation scope

- Linux/macOS CI: unit tests of NVRAM boot profiles, configuration
  preservation, profile file writes, Kext Info.plist and Mach-O preflight.
- macOS CI: compile both Kexts and run `auxkc_preflight.py` on
  `Navi33GOPFB.kext` **without installation**.
- No CI can validate an AMD DCN3.2.1 scanout, a GPU command submission or
  whether the black-screen Tahoe installer is fixed.

## References

- Apple: https://support.apple.com/guide/security/securely-extending-the-kernel-sec8e454101b/web
- Apple's kmutil manual: https://keith.github.io/xcode-man-pages/kmutil.8.html
- OpenCore NVRAM configuration: https://github.com/acidanthera/OpenCorePkg
- WhateverGreen -radvesa: https://github.com/acidanthera/WhateverGreen
