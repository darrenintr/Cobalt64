# Navi33 boot bootstrap: recovery before experimentation

**Stage 3 development tools, NOT working GPU graphics or an installer patch.**

The RX 7600 is the only GPU on a Ryzen 5 3600 test machine. It has no macOS
accelerated driver, and a blank-screen installer cannot be assumed usable.
The tools here let us audit a future **recoverable** test without touching
the current EFI or macOS installation.

## 1. Generate three OpenCore configurations (offline; safe)

~~~sh
python3 src/navi33-bringup/bootstrap/boot_profiles.py \
  --config /path/to/EFI/OC/config.plist \
  --output-dir /some/other/directory/navi33-profiles
~~~

Outputs:
- \`recovery.plist\`: original boot-args except experimental Navi33 flags,
  with \`navi33-fb-off=1\`. This disables the custom framebuffer if it has been
  installed. It **cannot** recover a screen that died before the Kext loaded.
- \`vesa-baseline.plist\`: adds \`-radvesa\` and \`navi33-fb-off=1\`. It is a
  diagnostic for the WhateverGreen software-display path, **not** evidence
  that RX 7600 will show the installer or WindowServer.
- \`fb-trial.plist\`: adds \`-radvesa navi33-fb=1\`. It can only do anything
  after the separate \`Navi33GOPFB.kext\` has been added to the **target
  macOS** Auxiliary Kernel Collection (AuxKC).

The tool **never overwrites the input** and refuses output-name collisions.
Profiles preserve SMBIOS, AMD kernel patches, DeviceProperties, and existing
SIP settings. They also ensure OpenCore NVRAM/Delete includes \`boot-args\`,
so old persisted boot flags do not silently supersede profile flags.

**Important:** These three files are not three entries in the OpenCore picker.
OpenCore reads one \`EFI/OC/config.plist\` at a time. To use alternatives,
prepare genuinely independent bootable EFI environments (or a way to restore
the previous configuration) first. Do not copy multiple named plists into
one EFI and expect the picker to switch between them.

The \`-radvesa\` boot arg comes from [WhateverGreen](https://github.com/
acidanthera/WhateverGreen); its effect requires an active compatible
WhateverGreen Kext. It disables AMD acceleration; it does not magically
create an RX 7600 graphics driver.

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
\`Navi33GOPFB\` depends on \`IOGraphicsFamily\` in the **System** KC and is
not safely OpenCore-injectable in \`Kernel/Add\`. The installer framebuffer
handoff will remain unresolved until a **target macOS installation** can
load the AuxKC. A Kext built inside the user's existing macOS QEMU guest
will NOT appear automatically in a separate bare-metal installation.

Do not attempt to disable SIP, patch SSV / BootKC or move Kexts around in
Preboot just to make the installer start: those are high-risk changes with
no proven Navi33 display path yet.

## 3. Validation scope

- Linux/macOS CI: unit tests of NVRAM boot profiles, configuration
  preservation, profile file writes, Kext Info.plist and Mach-O preflight.
- macOS CI: compile both Kexts and run \`auxkc_preflight.py\` on
  \`Navi33GOPFB.kext\` **without installation**.
- No CI can validate an AMD DCN3.2.1 scanout, a GPU command submission or
  whether the black-screen Tahoe installer is fixed.

## References

- Apple: https://support.apple.com/guide/security/securely-extending-the-kernel-sec8e454101b/web
- Apple's kmutil manual: https://keith.github.io/xcode-man-pages/kmutil.8.html
- OpenCore NVRAM configuration: https://github.com/acidanthera/OpenCorePkg
- WhateverGreen -radvesa: https://github.com/acidanthera/WhateverGreen
