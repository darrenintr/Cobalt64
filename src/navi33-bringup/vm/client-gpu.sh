#!/usr/bin/env bash
# vm/client-gpu.sh: Phase B4 on the card. Builds the OpenCore disk with
# navi33-client-gpu=1 (plus --autostart unless NO_AUTOSTART=1: a fresh card is
# brought up at boot, an already-up card is refused by the autostart with zero
# writes and simply stays up), boots the VM, copies the tools, runs n33gpu (a user
# program on its own GPU context) twice and n33n48n (the same through the N48N ABI
# RADV uses), saves everything and shuts down.
# ARTIFACT=DIR overrides the cached kext artifact. The user runs it with `!`.
set -euo pipefail
VM="/home/darren/macos tahoe"; N="$VM/navi33"
ART="${ARTIFACT:-$N/cache/artifact-0.0.49}"; OC="$N/cache/OpenCore-1.0.8-RELEASE"
V=/home/darren/Cobalt64/src/navi33-bringup/vm
GATES="--client-gpu"; [ "${NO_AUTOSTART:-0}" = 1 ] || GATES="--autostart --client-gpu"
pgrep -f "^qemu-system-x86_64" >/dev/null && { echo "a VM is already running" >&2; exit 1; }
# shellcheck disable=SC2086
"$V/make-opencore.sh" "$ART" --oc "$OC" --hostfb /var/lib/cobalt64-navi33/hostfb.txt $GATES | grep -E "UUID|boot-args"
tmux new-session -d -s n33-client "cd '$V' && ./run-vm.sh; sleep 300"
sleep 3; R="$(readlink -f "$N/runs/latest")"
for i in $(seq 1 60); do
  ssh -o ConnectTimeout=3 -o BatchMode=yes macos-vm true 2>/dev/null && break
  pgrep -f "^qemu-system-x86_64" >/dev/null || { echo "QEMU EXITED"; exit 1; }
  sleep 5
done
"$V/guest-run.sh" "$ART" > "$R/guest-run-before.txt" 2>&1 || true
ssh macos-vm 'ioreg -l -w0 -c Navi33Probe | grep -E "Navi33,(Autostart|ClientGpu|VramHeap)"' > "$R/client-ioreg-before.txt" 2>&1 || true
# CMDS=... replaces the guest command list (run in ~/navi33 as darren).
CMDS="${CMDS:-sudo ./n33gpu --fw gradient.co --nop; sudo ./n33gpu --fw gradient.co --frame 11 && sudo ./n33gpu --fw gradient.co --frame 200 --repeat 200 && sudo ./n33n48n --fw gradient.co}"
ssh macos-vm "cd ~/navi33 && $CMDS" \
  > "$R/client-gpu.txt" 2>&1 || true
cat "$R/client-gpu.txt"
ssh macos-vm 'ioreg -l -w0 -c Navi33Probe | grep -E "Navi33,(Autostart|ClientGpu|ClientGpuStall|ClientGpuFault|VramHeap)"; sudo dmesg | grep -E "Navi33Probe: client GPU" | tail -8' \
  > "$R/client-ioreg-after.txt" 2>&1 || true
cat "$R/client-ioreg-after.txt"
"$V/guest-run.sh" "$ART" > "$R/guest-run-after.txt" 2>&1 || true
ssh macos-vm 'sudo shutdown -h now' >/dev/null 2>&1 || true
for i in $(seq 1 60); do pgrep -f "^qemu-system-x86_64" >/dev/null || break; sleep 2; done
pgrep -f "^qemu-system-x86_64" >/dev/null && echo "QEMU STILL RUNNING" || echo "qemu exited; run dir $R"
tmux kill-session -t n33-client 2>/dev/null || true
