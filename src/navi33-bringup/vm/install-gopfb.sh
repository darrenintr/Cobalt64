#!/usr/bin/env bash
# vm/install-gopfb.sh KEXT_DIR: put Navi33GOPFB.kext into the running guest's
# /Library/Extensions and ask macOS to load it (G37). The first load is
# refused as "not approved"; approve it once in System Settings > Privacy &
# Security ("Allow"), which needs a screen: boot the guest with
# `run-vm.sh --no-gpu` (QXL on VNC 127.0.0.1:5901) for that one step, then
# restart. Later rebuilds keep the bundle ID, so they need no new click.
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
echo "== load request (the first one is refused until you click Allow)"
sudo kmutil load -p /Library/Extensions/Navi33GOPFB.kext 2>&1 | tail -4 || true
EOF
echo "next: System Settings > Privacy & Security > Allow (over VNC), restart, then vm/fb-trial.sh"
