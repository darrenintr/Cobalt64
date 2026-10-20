#!/usr/bin/env bash
# vm/cold-run.sh: the whole card from a fresh power-on in one VM boot: the kext's autostart
# (sOS .. MEC), the client GPU service, and Navi33GOPFB on the host console. Then nine
# n33gpu clients (slot/VMID reuse with TLB flushes), a 200-submission client, n33n48n, the
# hub/HQD state, and the screen. The VM is left running (tmux n33-cold) so the monitor
# keeps the macOS screen. Needs Navi33GOPFB in the guest's Aux KC (vm/install-gopfb.sh).
set -euo pipefail
V=/home/darren/Cobalt64/src/navi33-bringup/vm
N="/home/darren/macos tahoe/navi33"
ART="${ARTIFACT:-$N/cache/artifact-0.0.49}"; OC="$N/cache/OpenCore-1.0.8-RELEASE"
pgrep -f "^qemu-system-x86_64" >/dev/null && { echo "a VM is already running" >&2; exit 1; }
"$V/make-opencore.sh" "$ART" --oc "$OC" --hostfb /var/lib/cobalt64-navi33/hostfb.txt --autostart --fb --client-gpu | grep -E "UUID|boot-args"
tmux kill-session -t n33-cold 2>/dev/null || true
tmux new-session -d -s n33-cold "cd '$V' && ./run-vm.sh; sleep 300"
sleep 3; R="$(readlink -f "$N/runs/latest")"
for i in $(seq 1 60); do
  ssh -o ConnectTimeout=3 -o BatchMode=yes macos-vm true 2>/dev/null && break
  pgrep -f "^qemu-system-x86_64" >/dev/null || { echo "QEMU EXITED"; exit 1; }
  sleep 5
done
sleep 20
"$V/guest-run.sh" "$ART" > "$R/guest-run-before.txt" 2>&1 || true
ssh macos-vm 'ioreg -l -w0 -c Navi33Probe | grep -E "Navi33,(Autostart|ClientGpu)\""; kmutil showloaded 2>/dev/null | grep -i navi33' > "$R/cold-before.txt" 2>&1 || true
ssh macos-vm 'cd ~/navi33 && for i in 1 2 3 4 5 6 7 8 9; do echo "== client $i"; sudo ./n33gpu --fw gradient.co --frame $((10+i)); done; sudo ./n33gpu --fw gradient.co --frame 200 --repeat 200; sudo ./n33n48n --fw gradient.co' > "$R/cold-clients.txt" 2>&1 || true
ssh macos-vm 'ioreg -l -w0 -c Navi33Probe | grep -E "Navi33,(Autostart|ClientGpu|ClientGpuStall|ClientGpuFault|Hqd)"; ioreg -l -w0 -c Navi33GOPFB | grep -E "Navi33FB,Stage|class Navi33GOPFB"; pgrep -lx WindowServer' > "$R/cold-after.txt" 2>&1 || true
printf 'pmemsave 0xa0000000 3145728 "%s/fb.raw"\n' "$R" | socat - UNIX-CONNECT:"$R/hmp.sock" >/dev/null 2>&1 || true
sleep 3
cat "$R/cold-before.txt"; grep -E "== client|context:|wrong pixels|WAIT|closed|N48N|HELLO|failed" "$R/cold-clients.txt"; grep -E "Autostart|ClientGpu\"|Stage" "$R/cold-after.txt"
echo "run dir $R (VM left running in tmux session n33-cold)"
