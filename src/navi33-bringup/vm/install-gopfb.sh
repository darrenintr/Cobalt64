#!/usr/bin/env bash
# vm/install-gopfb.sh KEXT_DIR: put Navi33GOPFB.kext into the running guest's
# /Library/Extensions and build it into the Aux KC (G37). Boot the guest from an
# --fb disk first (SIP 0x267: unapproved kexts allowed); Tahoe still answers a plain
# `kmutil load` with "not approved" even then (2026-10-20), so the load skips the
# approval check (-z) and the Aux KC is rebuilt; it takes effect at the next boot
# (vm/fb-trial.sh). No System Settings click is needed.
# Never kmutil install --update-all (it rebuilds the system collections too).
#
# The kext only starts with navi33-fb=1 (vm/make-opencore.sh --fb) and stops
# at navi33-fb-off=1, which every other disk sets.
set -euo pipefail
K="${1:?usage: $0 path/to/Navi33GOPFB.kext}"
[ -f "$K/Contents/MacOS/Navi33GOPFB" ] || { echo "no Navi33GOPFB.kext at $K" >&2; exit 1; }
scp -q -r "$K" macos-vm:/tmp/
ssh macos-vm bash -s <<'EOF'
set -u
sudo rm -rf /Library/Extensions/Navi33GOPFB.kext
sudo cp -R /tmp/Navi33GOPFB.kext /Library/Extensions/
sudo chown -R root:wheel /Library/Extensions/Navi33GOPFB.kext
sudo chmod -R 755 /Library/Extensions/Navi33GOPFB.kext
sudo xattr -cr /Library/Extensions/Navi33GOPFB.kext
echo "== Aux KC rebuild (expect: requires a reboot)"
sudo kmutil load -z -p /Library/Extensions/Navi33GOPFB.kext 2>&1 | tail -4 || true
kmutil inspect -A /Library/KernelCollections/AuxiliaryKernelExtensions.kc 2>&1 | grep -i navi33gopfb || true
EOF
echo "next: vm/fb-trial.sh (reboots the guest into the new Aux KC)"
