#!/bin/sh
# One-time Ubuntu host setup for the Navi33 VM loop:
#   sudo sh vm/host-setup.sh            set up
#   sudo sh vm/host-setup.sh --remove   undo (openssh-server stays installed)
# 1. An SSH server and tmux: while the VM owns the RX 7600 this PC has no
#    screen, so it is driven from the laptop, in a session that survives a
#    dropped connection.
# 2. A GRUB entry that boots headless and gives the RX 7600 (and its HDMI
#    audio, so VFIO can reset the pair) to vfio-pci. amdgpu is blacklisted
#    there: it would load the PSP sOS itself, and the card would no longer
#    be in the bootloader-ready state the experiments start from. The entry
#    is chosen for one boot only (vm/reboot-to-vm.sh); the default boot is
#    unchanged.
# 3. VFIO access without root: /dev/vfio/* for group kvm, and unlimited
#    memlock (VFIO pins all guest RAM).
# 4. cobalt64-navi33-card.service: in that boot only, vm/prepare-card.sh
#    shrinks BAR0 to 256 MiB and saves the card's ROM image.
# Rerunning it is safe; in the VM boot it also runs step 4 right away.
set -eu
[ "$(id -u)" = 0 ] || { echo "run it with sudo" >&2; exit 1; }
USER_NAME="${SUDO_USER:?run it through sudo from your own account}"
HERE=$(cd "$(dirname "$0")" && pwd)
ENTRY=/etc/grub.d/42_cobalt64_navi33_vm
LIMITS=/etc/security/limits.d/90-cobalt64-vfio.conf
UDEV=/etc/udev/rules.d/90-cobalt64-vfio.rules
CARD=/usr/local/libexec/cobalt64-navi33-card
UNIT=/etc/systemd/system/cobalt64-navi33-card.service
if [ "${1:-}" = --remove ]; then
    systemctl disable cobalt64-navi33-card.service 2>/dev/null || true
    rm -f "$ENTRY" "$LIMITS" "$UDEV" "$CARD" "$UNIT"
    systemctl daemon-reload
    update-grub
    udevadm control --reload
    echo "removed the VM boot entry, card service, memlock limit and vfio udev rule"
    exit 0
fi

apt-get install -y openssh-server tmux

root_uuid=$(findmnt -no UUID /)
[ -n "$root_uuid" ] && [ -e /boot/vmlinuz ] && [ -e /boot/initrd.img ] ||
    { echo "cannot find the root UUID or /boot/vmlinuz" >&2; exit 1; }
cat > "$ENTRY" <<EOF
#!/bin/sh
cat <<'MENU'
menuentry 'Ubuntu, RX 7600 to the macOS VM (headless)' --id cobalt64-navi33-vm {
	insmod gzio
	insmod part_gpt
	insmod ext2
	search --no-floppy --fs-uuid --set=root $root_uuid
	linux /boot/vmlinuz root=UUID=$root_uuid ro modprobe.blacklist=amdgpu,snd_hda_intel modules-load=vfio-pci vfio-pci.ids=1002:7480,1002:ab30 vfio-pci.disable_idle_d3=1 systemd.unit=multi-user.target
	initrd /boot/initrd.img
}
MENU
EOF
chmod 755 "$ENTRY"
update-grub

printf '%s - memlock unlimited\n' "$USER_NAME" > "$LIMITS"
echo 'SUBSYSTEM=="vfio", GROUP="kvm", MODE="0660"' > "$UDEV"
udevadm control --reload

install -m 755 "$HERE/prepare-card.sh" "$CARD"
cat > "$UNIT" <<EOF
[Unit]
Description=Cobalt64: prepare the RX 7600 for the macOS VM (BAR0 256 MiB, ROM copy)
ConditionKernelCommandLine=vfio-pci.ids=1002:7480,1002:ab30
After=systemd-modules-load.service

[Service]
Type=oneshot
RemainAfterExit=yes
ExecStart=$CARD

[Install]
WantedBy=multi-user.target
EOF
systemctl daemon-reload
systemctl enable cobalt64-navi33-card.service
# Skipped by its condition outside the VM boot; refuses while a VM holds the card.
systemctl restart cobalt64-navi33-card.service || true
journalctl -b -u cobalt64-navi33-card.service --no-pager -o cat | tail -3

echo
systemctl --no-pager --lines=0 status ssh.socket ssh.service 2>/dev/null | grep -E "^[●○ ] |Active:" || true
ip -4 -br addr show scope global | grep -v -E "docker|virbr|wg" || true
echo "Done. From the laptop: ssh $USER_NAME@<the address above>"
