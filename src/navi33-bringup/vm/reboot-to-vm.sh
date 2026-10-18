#!/bin/sh
# Restart into the headless VFIO entry for one boot (vm/host-setup.sh adds
# it). The boot after that is normal Ubuntu again. After a PSP write attempt
# power off instead (sudo poweroff): a warm restart may keep the PSP state.
set -eu
sudo grub-reboot cobalt64-navi33-vm
echo "next boot: 'Ubuntu, RX 7600 to the macOS VM (headless)'; reach it with ssh"
sudo systemctl reboot
