#!/usr/bin/env bash
# vm/stage.sh: one VM boot per write stage (used by vm/scpm-sequence.sh).
# ARTIFACT=DIR overrides the cached kext artifact (default 0.0.29).
# One VM boot per stage: build the OpenCore disk with exactly one gate set,
# boot, copy tools, run the read-only plan, then (only if a run command is
# given) the write command, capture everything, shut down.
#   stage.sh NAME "GATE FLAGS" "PLAN ARGS" ["RUN ARGS"]
set -euo pipefail
NAME=$1 GATES=$2 PLAN=$3 RUN=${4:-}
VM="/home/darren/macos tahoe"; N="$VM/navi33"
ART="${ARTIFACT:-$N/cache/artifact-0.0.29}"; OC="$N/cache/OpenCore-1.0.8-RELEASE"
V=/home/darren/Cobalt64/src/navi33-bringup/vm
# shellcheck disable=SC2086
"$V/make-opencore.sh" "$ART" --oc "$OC" --hostfb /var/lib/cobalt64-navi33/hostfb.txt $GATES | grep -E "UUID|boot-args"
tmux new-session -d -s "n33-$NAME" "cd '$V' && ./run-vm.sh; sleep 300"
sleep 3
R="$(readlink -f "$N/runs/latest")"
for i in $(seq 1 60); do
  ssh -o ConnectTimeout=3 -o BatchMode=yes macos-vm true 2>/dev/null && break
  pgrep -f "^qemu-system-x86_64" >/dev/null || { echo "QEMU EXITED"; exit 1; }
  sleep 5
done
"$V/guest-run.sh" "$ART" > "$R/guest-run-before.txt" 2>&1 || true
ROM="--rom rx7600-ipd.bin"
ssh macos-vm "cd ~/navi33 && sudo ./n33psp $PLAN $ROM" > "$R/$NAME-plan.txt" 2>&1 || true
echo "== plan ($R/$NAME-plan.txt)"; grep -E "^state|^flags|ring|writes|fence|SMU|reset|RLC" "$R/$NAME-plan.txt" | head -12 || true
if [ -n "$RUN" ]; then
  ssh macos-vm "cd ~/navi33 && sudo ./n33psp $RUN $ROM --i-have-ssh" > "$R/$NAME-run.txt" 2>&1 || true
  echo "== run ($R/$NAME-run.txt)"; cat "$R/$NAME-run.txt" | head -40
  "$V/guest-run.sh" "$ART" > "$R/guest-run-after.txt" 2>&1 || true
fi
grep -a "Navi33Probe" "$R/serial.log" | tail -6 || true
ssh macos-vm 'sudo shutdown -h now' >/dev/null 2>&1 || true
for i in $(seq 1 60); do pgrep -f "^qemu-system-x86_64" >/dev/null || break; sleep 2; done
pgrep -f "^qemu-system-x86_64" >/dev/null && echo "QEMU STILL RUNNING" || echo "qemu exited; run dir $R"
tmux kill-session -t "n33-$NAME" 2>/dev/null || true
