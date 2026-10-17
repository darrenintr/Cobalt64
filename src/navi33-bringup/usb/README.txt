Cobalt64 Navi33 (RX 7600) diagnostics
=====================================

run.sh and the boot autorun only READ the GPU. The one tool that can
write it is n33psp --load (the PSP sOS firmware load), and only by hand.

Run once by hand (asks for your password):
    sh /Volumes/OPENCORE/navi33/run.sh

Or let every boot do it (install once):
    sudo sh /Volumes/OPENCORE/navi33/install-autorun.sh
The installer does a test run right away. After that: boot macOS, wait
for the chime (or about 2 minutes), then reboot. Each run leaves a
results-<date>-<time>-boot.txt file here, readable from Linux.
If the installer reports BLOCKED, give Full Disk Access to
/usr/local/libexec/cobalt64/n33autorun (System Settings > Privacy &
Security > Full Disk Access, "+", Cmd-Shift-G to type the path), then
run the installer again.
Remove it with:
    sudo sh /Volumes/OPENCORE/navi33/uninstall-autorun.sh
Autorun log on the Mac: /var/log/cobalt64-navi33-autorun.log

Tools
    n33query     HELLO and capabilities
    n33compute   --rom FILE / --probe / --queue hardware preflights
    n33vram      framebuffer geometry (needs navi33-vram-read=1)
    n33peek      allowlisted register windows; --list shows them
    n33psp       PSP13 sOS load: --plan reads only; --load writes (below)
    rx7600-ipd.bin  this card's IP Discovery table (from Linux amdgpu)
    psp_13_0_7_sos.bin  PSP firmware, NOT from CI: copy it from Linux
                    (zstd -d /lib/firmware/amdgpu/psp_13_0_7_sos.bin.zst).
                    The Kext accepts only the file whose SHA-256 it pins.

PSP sOS load (the first GPU write; the RX 7600 is the only display)
    1. Have SSH into the Mac working. If the card stops answering, the
       recovery is a full power-off (not a restart), then boot again.
    2. In the OpenCore boot-args, remove navi33-psp-off=1 and add
       navi33-psp-load=1. Boot.
    3. Over SSH: cd /Volumes/OPENCORE/navi33 && sudo ./n33psp --load \
         --rom rx7600-ipd.bin --fw psp_13_0_7_sos.bin --i-have-ssh
       It prints the plan, refuses unless the plan is clean, then loads.
       One attempt per boot. Then run sh run.sh to save the results.
    4. Put navi33-psp-off=1 back (and drop navi33-psp-load=1) afterwards.

The Kext and these tools must come from the same build (same N33N ABI).
