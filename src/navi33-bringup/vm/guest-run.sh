#!/usr/bin/env bash
# Run the read-only diagnostics in the running VM and bring the results home.
#   vm/guest-run.sh ARTIFACT_DIR
# Copies the artifact's navi33/ tools and the pinned PSP firmware to ~/navi33
# in the guest, runs run.sh there (sudo in the guest), and copies the results
# file into this run's directory next to serial.log. Like on the stick,
# run.sh never runs the PSP load.
set -euo pipefail
VM_DIR="${VM_DIR:-$HOME/macos tahoe}"; HOST="${GUEST:-macos-vm}"
die() { echo "guest-run: $*" >&2; exit 1; }
[ $# -eq 1 ] || die "usage: $0 ARTIFACT_DIR"
ART="$(cd -- "$1" && pwd)"; RUN="$(readlink -f "$VM_DIR/navi33/runs/latest")"
[ -x "$ART/navi33/run.sh" ] || [ -f "$ART/navi33/run.sh" ] || die "no navi33/run.sh in $ART"
[ -d "$RUN" ] || die "no VM run directory; start vm/run-vm.sh first"
FW=/lib/firmware/amdgpu/psp_13_0_7_sos.bin.zst
WORK="$(mktemp -d)"; trap 'rm -rf "$WORK"' EXIT
cp -r "$ART/navi33" "$WORK/navi33"
# The kext accepts only the file whose SHA-256 it pins, so a wrong one fails the plan.
[ -f "$FW" ] && zstd -q -d -c "$FW" > "$WORK/navi33/psp_13_0_7_sos.bin"
chmod +x "$WORK/navi33/"n33* 2>/dev/null || true
# The eight decompressed linux-firmware files for n33psp --ip-fw (the SMU image
# is not among them). The Kext accepts only the pinned versions.
mkdir -p "$WORK/navi33/fw"
for n in gc_11_0_2_pfp gc_11_0_2_me gc_11_0_2_mec gc_11_0_2_rlc gc_11_0_2_imu \
         sdma_6_0_2 gc_11_0_2_mes_2 gc_11_0_2_mes1; do
  [ -f "/lib/firmware/amdgpu/$n.bin.zst" ] &&
    zstd -q -d -c "/lib/firmware/amdgpu/$n.bin.zst" > "$WORK/navi33/fw/$n.bin"
done
ssh -o BatchMode=yes "$HOST" 'rm -rf ~/navi33.new' || die "cannot reach $HOST over ssh"
scp -q -r "$WORK/navi33" "$HOST:navi33.new"
ssh -o BatchMode=yes "$HOST" 'mkdir -p ~/navi33 && cp -R ~/navi33.new/ ~/navi33/ && rm -rf ~/navi33.new'
out="$(ssh -o BatchMode=yes "$HOST" 'sudo -n sh ~/navi33/run.sh guest' | sed -n 's/^Saved to //p')" ||
  die "run.sh failed (the guest needs passwordless sudo for this; see AGENTS.md)"
[ -n "$out" ] || die "run.sh did not report a results file"
scp -q "$HOST:$out" "$RUN/"
echo "guest-run: $RUN/$(basename "$out")"
