#!/usr/bin/env bash
# vm/fb-trial.sh: G37 trial, after install-gopfb.sh and the one Allow click.
# Shuts the guest down if it runs, rebuild the OpenCore disk with --autostart --fb and this
# boot's hostfb, boot, and save the framebuffer evidence in the run directory.
# A card that autostart already brought up is refused with zero writes.
set -euo pipefail
V=/home/darren/Cobalt64/src/navi33-bringup/vm
N="/home/darren/macos tahoe/navi33"
ART="${ARTIFACT:-$N/cache/artifact-0.0.45}"; OC="$N/cache/OpenCore-1.0.8-RELEASE"
up() { ssh -o ConnectTimeout=3 -o BatchMode=yes macos-vm true 2>/dev/null; }
if pgrep -f "^qemu-system-x86_64" >/dev/null; then
  ssh macos-vm 'sudo shutdown -h now' >/dev/null 2>&1 || true
  for i in $(seq 1 60); do pgrep -f "^qemu-system-x86_64" >/dev/null || break; sleep 2; done
  pgrep -f "^qemu-system-x86_64" >/dev/null && { echo "QEMU STILL RUNNING" >&2; exit 1; }
fi
tmux kill-session -t n33-gui 2>/dev/null || true
"$V/make-opencore.sh" "$ART" --oc "$OC" --hostfb /var/lib/cobalt64-navi33/hostfb.txt --autostart --fb | grep -E "UUID|boot-args"
tmux new-session -d -s n33-gui "cd '$V' && ./run-vm.sh; sleep 300"
sleep 3; R="$(readlink -f "$N/runs/latest")"
for i in $(seq 1 60); do up && break; sleep 5; done
sleep 20   # let WindowServer reach the login window
ssh macos-vm 'kmutil showloaded 2>/dev/null | grep -iE "navi33"; echo; ioreg -l -w0 -c Navi33GOPFB | grep -E "Navi33FB|IOFBCurrent|class Navi33GOPFB"; echo; system_profiler SPDisplaysDataType 2>/dev/null | head -40; echo; sudo dmesg 2>/dev/null | grep -E "Navi33GOPFB" | tail -10; pgrep -lx WindowServer' > "$R/fb-trial.txt" 2>&1 || true
cat "$R/fb-trial.txt"
echo "run dir $R (VM left running in tmux session n33-gui)"
