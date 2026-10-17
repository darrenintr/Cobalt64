Cobalt64 Navi33 (RX 7600) read-only diagnostics
================================================

Everything in this folder only READS the GPU. Nothing loads firmware or
writes a GPU register.

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
    rx7600-ipd.bin  this card's IP Discovery table (from Linux amdgpu)

The Kext and these tools must come from the same build (same N33N ABI).
