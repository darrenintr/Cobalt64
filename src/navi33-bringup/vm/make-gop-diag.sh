#!/usr/bin/env bash
# Build a UEFI-shell boot disk that reports why the guest firmware gives the
# passed-through RX 7600 no GOP, then powers the VM off. Boot it with
#   BOOT_DISK="$VM_DIR/navi33/gop-diag.qcow2" vm/run-vm.sh --snapshot
# and read serial.log.
#   vm/make-gop-diag.sh OPENCORE_RELEASE_DIR [ROM]
# ROM defaults to the copy vm/prepare-card.sh saves; it is put on the disk
# for loadpcirom by hand. The script itself does not load it: a second AMD
# GOP instance hung the guest's connect -r on 2026-10-18.
set -euo pipefail
VM_DIR="${VM_DIR:-$HOME/macos tahoe}"
die() { echo "make-gop-diag: $*" >&2; exit 1; }
[ $# -ge 1 ] || die "usage: $0 OPENCORE_RELEASE_DIR [ROM]"
SHELL_EFI="$1/X64/EFI/OC/Tools/OpenShell.efi"; ROM="${2:-/var/lib/cobalt64-navi33/rx7600-rom.bin}"
[ -f "$SHELL_EFI" ] || die "no $SHELL_EFI"
[ -r "$ROM" ] || die "cannot read $ROM"
OUT="$VM_DIR/navi33/gop-diag.qcow2"
WORK="$(mktemp -d)"; trap 'rm -rf "$WORK"' EXIT
truncate -s 64M "$WORK/d.raw"
# -q would make sgdisk skip the write on a blank image.
sgdisk -o -n1:2048:0 -t1:ef00 "$WORK/d.raw" >/dev/null
IMG="$WORK/d.raw@@1048576"; export MTOOLS_SKIP_CHECK=1
mformat -i "$IMG" -F -v GOPDIAG ::
mmd -i "$IMG" ::/EFI ::/EFI/BOOT
mcopy -i "$IMG" "$SHELL_EFI" ::/EFI/BOOT/BOOTX64.EFI
mcopy -i "$IMG" "$ROM" ::/rx7600.rom
cat > "$WORK/startup.nsh" <<'EOF'
@echo -off
echo "=== GOPDIAG: drivers the firmware loaded on its own"
drivers
echo "=== GOPDIAG: GOP handles before connect"
dh -p GraphicsOutput
echo "=== GOPDIAG: pci"
pci
echo "=== GOPDIAG: connect -r"
connect -r
echo "=== GOPDIAG: GOP handles after connect"
dh -p GraphicsOutput
echo "=== GOPDIAG: device tree"
devtree
echo "=== GOPDIAG: driver handles"
dh -d -p DriverBinding
echo "=== GOPDIAG: done"
reset -s
EOF
mcopy -i "$IMG" "$WORK/startup.nsh" ::/startup.nsh
mcopy -i "$IMG" "$WORK/startup.nsh" ::/EFI/BOOT/startup.nsh
qemu-img convert -O qcow2 "$WORK/d.raw" "$OUT"
echo "wrote $OUT"
