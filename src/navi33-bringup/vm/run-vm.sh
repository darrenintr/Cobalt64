#!/usr/bin/env bash
# Boot the Tahoe VM for Navi33 work and keep everything it prints.
#   vm/run-vm.sh [--no-gpu] [--snapshot] [--check]
# Default: the real RX 7600 through VFIO (09:00.0, plus 09:00.1 when it is
# also on vfio-pci), no emulated display and its ROM hidden (rombar=0), so the
# scanout the PC firmware set up survives and the PSP plan can check it
# against the host's record of that console (navi33-hostfb-*). Needs the
# headless boot (vm/reboot-to-vm.sh).
#   --no-gpu    emulated QXL display on VNC 127.0.0.1:5901, no RX 7600; for
#               testing the VM itself from a normal desktop boot
#   --snapshot  throw away the guest disk's writes on exit
#   --check     print the QEMU command line and stop
# Every run gets $VM_DIR/navi33/runs/<stamp>/ (runs/latest points at it):
# serial.log (kernel console, kprintf, panics), qemu.log, cmdline.txt and the
# qmp.sock/hmp.sock monitors. gdb attaches on 127.0.0.1:1234 and the guest's
# SSH is 127.0.0.1:2222 (ssh macos-vm). A guest reboot stops the VM, so the
# card never goes through its firmware again without us asking.
set -euo pipefail
VM_DIR="${VM_DIR:-$HOME/macos tahoe}"
QEMU_BIN="${QEMU_BIN:-qemu-system-x86_64}"
GPU="${GPU:-0000:09:00.0}"; GPU_AUDIO="${GPU_AUDIO:-0000:09:00.1}"
# ROMBAR=1 lets the guest firmware run the card's AMD GOP from its ROM. Off by
# default: on 2026-10-18 that GOP switched DCN off and lit no output.
ROMBAR="${ROMBAR:-0}"
ROMFILE="${ROMFILE:-}"        # with ROMBAR=1, only if QEMU cannot read the card's own ROM
VFIO_OPTS="${VFIO_OPTS:-}"    # extra vfio-pci options for 09:00.0, e.g. ,x-vga=on
RAM="${RAM:-8G}"; SSH_PORT="${SSH_PORT:-2222}"; GDB_PORT="${GDB_PORT:-1234}"
USE_GPU=1; SNAPSHOT=0; CHECK=0
for a in "$@"; do
  case "$a" in
    --no-gpu) USE_GPU=0 ;; --snapshot) SNAPSHOT=1 ;; --check) CHECK=1 ;;
    -h|--help) sed -n '2,17p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "unknown option $a (try --help)" >&2; exit 1 ;;
  esac
done
die() { echo "run-vm: $*" >&2; exit 1; }
N="$VM_DIR/navi33"
# BOOT_DISK swaps the OpenCore disk for another qcow2 (vm/make-gop-diag.sh).
OC="${BOOT_DISK:-$N/OpenCore-navi33.qcow2}"; HDD="$VM_DIR/HDD.qcow2"
# OVMF_CODE/OVMF_VARS swap the firmware pair (OVMF_VARS: a writable copy).
OVMF_CODE="${OVMF_CODE:-$VM_DIR/ovmf/OVMF_CODE.fd}"; VARS="${OVMF_VARS:-$N/OVMF_VARS-navi33.fd}"
[ -f "$OC" ] || die "no $OC; build it with vm/make-opencore.sh ARTIFACT_DIR"
[ -f "$HDD" ] && [ -f "$OVMF_CODE" ] || die "no VM at $VM_DIR (set VM_DIR)"
[ -r /dev/kvm ] || die "/dev/kvm is not usable"
# The VM's NVRAM stays separate from the normal VM boots.
[ -f "$VARS" ] || cp "$VM_DIR/ovmf/OVMF_VARS.fd" "$VARS"
qemu-img info "$HDD" >/dev/null 2>&1 || die "$HDD is in use by another VM"

drv() { basename "$(readlink "/sys/bus/pci/devices/$1/driver" 2>/dev/null)" 2>/dev/null || true; }
GPU_ARGS=()
if [ "$USE_GPU" = 1 ]; then
  d="$(drv "$GPU")"
  # amdgpu loads the PSP sOS itself; once it has, the card is not in the state
  # the experiments start from, so never take it over from a running driver.
  [ "$d" = vfio-pci ] || die "$GPU is on '${d:-no driver}', not vfio-pci. Boot the
  headless entry first (vm/reboot-to-vm.sh), or use --no-gpu."
  group="$(basename "$(readlink "/sys/bus/pci/devices/$GPU/iommu_group")")"
  [ -r "/dev/vfio/$group" ] && [ -w "/dev/vfio/$group" ] ||
    die "/dev/vfio/$group is not accessible; run vm/host-setup.sh"
  lim="$(ulimit -l)"
  [ "$lim" = unlimited ] || die "memlock limit is ${lim} KiB; VFIO pins all ${RAM} of guest RAM.
  Run vm/host-setup.sh, then log in again."
  read -r s e _ < "/sys/bus/pci/devices/$GPU/resource"
  bar0=$(( (e - s + 1) >> 20 ))
  [ "$bar0" = 256 ] || echo "run-vm: BAR0 is $bar0 MiB; macOS cannot place it (gave it
  0x8012400000000000 at 8 GiB). sudo systemctl restart cobalt64-navi33-card" >&2
  rom=",rombar=0"
  if [ "$ROMBAR" = 1 ]; then rom=""; [ -n "$ROMFILE" ] && rom=",romfile=$ROMFILE"; fi
  # Kext 0.0.8's navi33-hostfb-* boot-args must be this boot's console record.
  fb="$(sed -n 's/^hostfb //p' "$N/OpenCore-navi33.txt" 2>/dev/null || true)"
  if [ -n "$fb" ]; then
    rec=/var/lib/cobalt64-navi33/hostfb.txt
    want="boot_id=$(cat /proc/sys/kernel/random/boot_id) offset=$(sed -n 's/^offset=//p' "$rec" 2>/dev/null) bytes=$(sed -n 's/^bytes=//p' "$rec" 2>/dev/null)"
    [ "$fb" = "$want" ] || die "OpenCore-navi33.qcow2 has hostfb '$fb', but this boot is '$want'.
  Rebuild it: vm/make-opencore.sh ARTIFACT_DIR --oc ... --hostfb $rec"
  fi
  GPU_ARGS=(-vga none -display none
    -device pcie-root-port,id=rp.gpu,bus=pcie.0,chassis=1,slot=1,x-speed=16,x-width=16
    -device "vfio-pci,host=$GPU,bus=rp.gpu,addr=00.0,multifunction=on$rom$VFIO_OPTS")
  if [ "$(drv "$GPU_AUDIO")" = vfio-pci ]; then
    GPU_ARGS+=(-device "vfio-pci,host=$GPU_AUDIO,bus=rp.gpu,addr=00.1")
  fi
  # After a VFIO reset the guest's GOP lit no output (2026-10-18); prepare-card.sh stops it.
  if [ -n "$(cat "/sys/bus/pci/devices/$GPU/reset_method" 2>/dev/null)" ] ||
     [ "$(drv "$GPU_AUDIO")" = vfio-pci ]; then
    echo "run-vm: VFIO can still reset the card before the guest starts" >&2
  else
    echo "run-vm: no VFIO reset possible; the guest gets the card as the PC firmware left it"
  fi
else
  GPU_ARGS=(-device qxl-vga,vgamem_mb=128,vram_size_mb=128 -display none -vnc 127.0.0.1:1)
fi

STAMP="$(date +%Y%m%d-%H%M%S)"; RUN="$N/runs/$STAMP"
HDD_OPT="id=MacHDD,if=none,format=qcow2,file=$HDD"
[ "$SNAPSHOT" = 1 ] && HDD_OPT+=",snapshot=on"
ARGS=(
  -name navi33-tahoe,debug-threads=on -pidfile "$RUN/qemu.pid"
  -global ICH9-LPC.acpi-pci-hotplug-with-bridge-support=off
  -enable-kvm -m "$RAM" -machine q35
  # Same CPU and SMBIOS shape as the VM's own boot.sh, so the guest is the one installed.
  -cpu Haswell-noTSX,kvm=on,vendor=GenuineIntel,+invtsc,vmware-cpuid-freq=on,+ssse3,+sse4.2,+popcnt,+avx,+aes,+xsave,+xsaveopt,check
  -smp threads=2,cores=4,sockets=1
  -device qemu-xhci,id=xhci -device usb-kbd,bus=xhci.0 -device usb-tablet,bus=xhci.0
  -device isa-applesmc,osk="ourhardworkbythesewordsguardedpleasedontsteal(c)AppleComputerInc"
  -drive "if=pflash,format=raw,readonly=on,file=$OVMF_CODE"
  -drive "if=pflash,format=raw,file=$VARS"
  -smbios type=2
  -device ich9-ahci,id=sata
  -drive "id=OpenCoreBoot,if=none,format=qcow2,file=$OC"
  -device ide-hd,bus=sata.2,drive=OpenCoreBoot,bootindex=1
  -drive "$HDD_OPT"
  -device ide-hd,bus=sata.3,drive=MacHDD,rotation_rate=7200
  -netdev "user,id=net0,ipv6=off,hostfwd=tcp:127.0.0.1:$SSH_PORT-:22"
  -device virtio-net-pci,netdev=net0,id=net0,mac=00:16:cb:00:21:09
  -chardev "file,id=serial0,path=$RUN/serial.log" -serial chardev:serial0
  -qmp "unix:$RUN/qmp.sock,server=on,wait=off"
  -monitor "unix:$RUN/hmp.sock,server=on,wait=off"
  -gdb "tcp:127.0.0.1:$GDB_PORT"
  -action reboot=shutdown -boot menu=off
  "${GPU_ARGS[@]}"
)
if [ "$CHECK" = 1 ]; then printf '%q ' "$QEMU_BIN" "${ARGS[@]}"; echo; exit 0; fi
mkdir -p "$RUN"; ln -sfn "$STAMP" "$N/runs/latest"
printf '%q ' "$QEMU_BIN" "${ARGS[@]}" > "$RUN/cmdline.txt"; echo >> "$RUN/cmdline.txt"
echo "run-vm: $RUN (gpu=$USE_GPU snapshot=$SNAPSHOT)"
echo "run-vm: ssh macos-vm | socat - UNIX-CONNECT:$RUN/hmp.sock | gdb :$GDB_PORT"
"$QEMU_BIN" "${ARGS[@]}" 2>&1 | tee "$RUN/qemu.log"
