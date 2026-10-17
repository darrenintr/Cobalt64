#!/bin/sh
# SPDX-License-Identifier: MIT
# Print the IP Discovery table that the Linux amdgpu driver parsed from a
# running Navi33 card. Read-only: it reads sysfs text files only, needs no
# root, and never touches PCI config space, BARs, debugfs or the VBIOS.
#
# Usage: tools/capture-linux-ipd.sh [card]      (default: first AMD card)
# Output feeds tests/fixtures/rx7600-linux-ipd.txt and the constants in
# tests/test_rx7600_silicon_bases.cpp.
set -eu

card=${1:-}
if [ -z "$card" ]; then
    for c in /sys/class/drm/card[0-9]*; do
        [ -d "$c/device/ip_discovery" ] || continue
        [ "$(cat "$c/device/vendor")" = "0x1002" ] || continue
        card=$(basename "$c"); break
    done
fi
[ -n "$card" ] || { echo "no amdgpu card with ip_discovery in sysfs" >&2; exit 1; }
dev=/sys/class/drm/$card/device
die=$dev/ip_discovery/die/0

echo "# pci $(cat "$dev/vendor"):$(cat "$dev/device") rev $(cat "$dev/revision")"
i=0
while read -r start end flags; do
    if [ "$i" -eq 5 ]; then
        printf '# bar5 0x%x\n' $((end - start + 1))
    fi
    i=$((i + 1))
done < "$dev/resource"
echo "# ip instance hw_id major.minor.revision bases..."
for ip in GC MP0 MP1 SDMA0 SDMA1 DMU MMHUB NBIF HDP OSSSYS ATHUB; do
    for inst in "$die/$ip"/*/; do
        [ -d "$inst" ] || continue
        printf '%s %s %s %s.%s.%s %s\n' "$ip" "$(basename "$inst")" \
            "$(cat "$inst/hw_id")" "$(cat "$inst/major")" \
            "$(cat "$inst/minor")" "$(cat "$inst/revision")" \
            "$(tr '\n' ' ' < "$inst/base_addr" | sed 's/ *$//')"
    done
done
