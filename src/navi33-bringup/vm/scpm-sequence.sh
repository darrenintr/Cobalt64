#!/usr/bin/env bash
# Stages 2-6 of the approved sequence, one VM boot each, in Linux's order:
# ring/TOC/TMR, SMU image alone, the other 26 records, AUTOLOAD_RLC, SCPM
# EnableAllSmuFeatures. Each stage runs its read-only plan first (the kext
# refuses the run unless it passes), and the script stops at the first stage
# whose run output lacks its success state. It ends with a closed-gate,
# read-only boot (kext 0.0.21 census) and leaves OpenCore-navi33.qcow2 closed.
set -uo pipefail
S="$(cd "$(dirname "$0")" && pwd)"
N="/home/darren/macos tahoe/navi33"
SOS="--fw psp_13_0_7_sos.bin"; FW="--fwdir fw"
stage() { # NAME GATES PLAN RUN EXPECT
  echo; echo "######## stage $1"
  pgrep -f "^qemu-system-x86_64" >/dev/null && { echo "STOP: a VM is still running"; exit 1; }
  bash "$S/stage.sh" "$1" "$2" "$3" "$4"
  local r; r="$(readlink -f "$N/runs/latest")/$1-run.txt"
  if ! grep -q "^state $5:" "$r" 2>/dev/null; then
    echo "STOP: stage $1 did not reach state $5 (see $r)"; exit 1
  fi
  echo "OK: stage $1 reached state $5"
}
stage ring     "--psp-ring"              "--ring-plan 3 $SOS"    "--ring 3 $SOS"     28
stage smu      "--psp-fw"                "--ip-fw-plan 26 27 $FW" "--ip-fw 26 27 $FW" 32
stage fwrest   "--psp-fw"                "--ip-fw-plan 0 26 $FW"  "--ip-fw 0 26 $FW"  32
stage autoload "--psp-fw --psp-autoload" "--ip-fw-plan 27 28 $FW" "--ip-fw 27 28 $FW" 32
stage scpm     "--smu-scpm"              "--smu-scpm-plan"        "--smu-scpm-enable" 39
echo; echo "######## read-only census boot (all gates closed)"
pgrep -f "^qemu-system-x86_64" >/dev/null && { echo "STOP: a VM is still running"; exit 1; }
bash "$S/stage.sh" census "" "--plan $SOS"
R="$(readlink -f "$N/runs/latest")"
F="$(ls "$R"/results-*-guest.txt | tail -1)"
python3 -I /home/darren/Cobalt64/src/navi33-bringup/tools/queue1-census.py "$F" | tail -25
echo "census results: $F"
