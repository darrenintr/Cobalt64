#!/bin/sh
# Root, VM boot only: cobalt64-navi33-card.service runs it at boot (installed
# by vm/host-setup.sh; rerun with: sudo systemctl restart cobalt64-navi33-card).
# Prepares the RX 7600 for the macOS guest:
# - BAR0 to 256 MiB. The firmware gives the card 8 GiB, which macOS cannot
#   place (on 2026-10-18 the guest moved it to 0x8012400000000000).
#   Bare-metal macOS saw 256 MiB as well.
# NEVER touches the VBIOS / expansion ROM (hard rule 5, AGENTS.md): no read of
# the `rom` file, no ROM-enable bit, nothing. An earlier version copied the ROM
# image; that code is gone on the user's instruction (2026-10-18).
# - Records the firmware console as a BAR0 offset in hostfb.txt there.
# - Stops VFIO resetting the card, so the guest gets it as the PC firmware
#   set it up. After VFIO's bus reset the guest's AMD GOP loaded but lit no
#   output (2026-10-18, two OVMF builds, with and without x-vga). Clearing
#   reset_method stops the function reset; taking 09:00.1 off vfio-pci stops
#   any bus reset, which needs every function of the slot on vfio-pci.
# Only touches the card while it is on vfio-pci and no VM holds it.
set -eu
DEV=0000:09:00.0; D=/sys/bus/pci/devices/$DEV; OUT=/var/lib/cobalt64-navi33
drv=$(basename "$(readlink "$D/driver" 2>/dev/null)" 2>/dev/null || true)
# Never from amdgpu: by then it has loaded the PSP sOS.
[ "$drv" = vfio-pci ] || { echo "$DEV is on '${drv:-no driver}', not vfio-pci: left alone"; exit 0; }
group=$(basename "$(readlink "$D/iommu_group")")
if fuser -s "/dev/vfio/$group" 2>/dev/null; then
    echo "a VM has $DEV open; stop it first" >&2; exit 1
fi
mkdir -p "$OUT"
# The firmware console as a BAR0 offset, for kext 0.0.8's navi33-hostfb-*
# boot-args: the guest boots without the AMD GOP (rombar=0; it switched DCN
# off), so it has no console of its own. Only while BAR0 is still where the
# firmware put it; after the resize below the old record stays.
python3 -I - "$D/resource" "$OUT/hostfb.txt" <<'EOF' || echo "no host console recorded" >&2
import struct, sys
bar0_start, bar0_end = (int(x, 16) for x in open(sys.argv[1]).readline().split()[:2])
d = open("/sys/kernel/boot_params/data", "rb").read(0x40)   # struct screen_info
w, h, depth, base, size = struct.unpack_from("<HHHII", d, 0x12)
line = struct.unpack_from("<H", d, 0x24)[0]
caps, ext = struct.unpack_from("<II", d, 0x36)
phys = base | (ext << 32 if caps & 2 else 0)               # VIDEO_CAPABILITY_64BIT_BASE
nbytes = line * h
if d[0x0f] != 0x70 or not bar0_start <= phys <= bar0_end - nbytes + 1:   # VIDEO_TYPE_EFI
    sys.exit(f"firmware framebuffer {phys:#x} is not in BAR0 {bar0_start:#x}-{bar0_end:#x}")
boot_id = open("/proc/sys/kernel/random/boot_id").read().strip()
open(sys.argv[2], "w").write(
    f"boot_id={boot_id}\noffset={phys - bar0_start:#x}\nbytes={nbytes:#x}\n"
    f"geometry={w}x{h}x{depth} line={line} fb_phys={phys:#x} "
    f"bar0_boot={bar0_start:#x}-{bar0_end:#x}\n")
print(f"host console: BAR0+{phys - bar0_start:#x}, {nbytes:#x} bytes, {w}x{h}x{depth}")
EOF
[ -f "$OUT/hostfb.txt" ] && chmod 644 "$OUT/hostfb.txt"
bar0() { set -- $(sed -n 1p "$D/resource"); echo $(( ($2 - $1 + 1) >> 20 )); }
size=$(bar0)
if [ "$size" -ne 256 ]; then
    # Resizing needs the device unbound; the override keeps any other driver off it meanwhile.
    echo vfio-pci > "$D/driver_override"
    echo "$DEV" > /sys/bus/pci/drivers/vfio-pci/unbind
    if echo 8 > "$D/resource0_resize"; then
        echo "BAR0 ${size} MiB -> $(bar0) MiB"
    else
        echo "BAR0 resize failed; it stays ${size} MiB" >&2
    fi
    echo "$DEV" > /sys/bus/pci/drivers/vfio-pci/bind
else
    echo "BAR0 is already 256 MiB"
fi
echo > "$D/reset_method" || echo "could not clear reset_method" >&2
AUDIO=0000:09:00.1; A=/sys/bus/pci/devices/$AUDIO
adrv=$(basename "$(readlink "$A/driver" 2>/dev/null)" 2>/dev/null || true)
if [ "$adrv" = vfio-pci ]; then
    echo none > "$A/driver_override"
    echo "$AUDIO" > /sys/bus/pci/drivers/vfio-pci/unbind
fi
echo "reset methods '$(cat "$D/reset_method")'; $AUDIO on '$(basename "$(readlink "$A/driver" 2>/dev/null)" 2>/dev/null || echo none)'"
