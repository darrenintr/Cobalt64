#!/usr/bin/env bash
# vm/stage-cont.sh: finish one stage in a VM that is already running.
# Continue one stage in the VM that is already running: copy tools, run the
# read-only plan, then (only if RUN is given) the write command, capture
# everything, and shut the guest down.
#   stage-cont.sh NAME "PLAN ARGS" ["RUN ARGS"]
set -euo pipefail
NAME=$1 PLAN=$2 RUN=${3:-}
VM="/home/darren/macos tahoe"; N="$VM/navi33"
ART="${ARTIFACT:-$N/cache/artifact-0.0.21}"
V=/home/darren/Cobalt64/src/navi33-bringup/vm
R="$(readlink -f "$N/runs/latest")"
for i in $(seq 1 60); do
  ssh -o ConnectTimeout=3 -o BatchMode=yes macos-vm true 2>/dev/null && break
  pgrep -f "^qemu-system-x86_64" >/dev/null || { echo "QEMU EXITED"; exit 1; }
  sleep 5
done
"$V/guest-run.sh" "$ART" > "$R/guest-run-before.txt" 2>&1 || true
ROM="--rom rx7600-ipd.bin"
ssh macos-vm "cd ~/navi33 && sudo ./n33psp $PLAN $ROM" > "$R/$NAME-plan.txt" 2>&1 || true
echo "== plan ($R/$NAME-plan.txt)"; grep -E "^state|^flags|ring|writes|fence|SMU|reset|RLC" "$R/$NAME-plan.txt" | head -12
if [ -n "$RUN" ]; then
  ssh macos-vm "cd ~/navi33 && sudo ./n33psp $RUN $ROM --i-have-ssh" > "$R/$NAME-run.txt" 2>&1 || true
  echo "== run ($R/$NAME-run.txt)"; head -40 "$R/$NAME-run.txt"
  "$V/guest-run.sh" "$ART" > "$R/guest-run-after.txt" 2>&1 || true
fi
grep -a "Navi33Probe" "$R/serial.log" | tail -6
ssh macos-vm 'sudo shutdown -h now' >/dev/null 2>&1 || true
for i in $(seq 1 60); do pgrep -f "^qemu-system-x86_64" >/dev/null || break; sleep 2; done
pgrep -f "^qemu-system-x86_64" >/dev/null && echo "QEMU STILL RUNNING" || echo "qemu exited; run dir $R"
tmux kill-session -t "n33-$NAME" 2>/dev/null || true
