#!/usr/bin/env bash
# vm/vk-run.sh: RADV (Mesa + mesa-patches/) on the RX 7600 in the guest. Boots like
# client-gpu.sh (autostart + client GPU), copies libvulkan_radeon.dylib, the Vulkan headers
# and tools/vkfill, builds vkfill in the guest and runs it as root, then shuts down.
# ARTIFACT=kext artifact dir, RADV=dir with libvulkan_radeon.dylib, VKINC=Vulkan include dir.
set -euo pipefail
V=/home/darren/Cobalt64/src/navi33-bringup/vm; T=/home/darren/Cobalt64/src/navi33-bringup/tools/vkfill
N="/home/darren/macos tahoe/navi33"
ART="${ARTIFACT:-$N/cache/artifact-0.0.50}"; OC="$N/cache/OpenCore-1.0.8-RELEASE"
RADV="${RADV:-$N/cache/radv-0010}"; VKINC="${VKINC:?set VKINC to a directory holding vulkan/vulkan.h}"
pgrep -f "^qemu-system-x86_64" >/dev/null && { echo "a VM is already running" >&2; exit 1; }
"$V/make-opencore.sh" "$ART" --oc "$OC" --hostfb /var/lib/cobalt64-navi33/hostfb.txt --autostart --client-gpu ${VK_EXTRA:-} | grep -E "UUID|boot-args"
tmux kill-session -t n33-vk 2>/dev/null || true
tmux new-session -d -s n33-vk "cd '$V' && ./run-vm.sh; sleep 300"
sleep 3; R="$(readlink -f "$N/runs/latest")"
for i in $(seq 1 60); do
  ssh -o ConnectTimeout=3 -o BatchMode=yes macos-vm true 2>/dev/null && break
  pgrep -f "^qemu-system-x86_64" >/dev/null || { echo "QEMU EXITED"; exit 1; }
  sleep 5
done
ssh macos-vm 'rm -rf ~/vk && mkdir -p ~/vk/include'
scp -q -r "$VKINC/vulkan" "$VKINC/vk_video" macos-vm:vk/include/ 2>/dev/null || scp -q -r "$VKINC/vulkan" macos-vm:vk/include/
scp -q "$RADV/libvulkan_radeon.dylib" "$T/vkfill.c" "$T/vkfill_spv.h" macos-vm:vk/
ssh macos-vm 'cd ~/vk && clang -O2 -Wall -Iinclude -I. vkfill.c -o vkfill && codesign -s - -f libvulkan_radeon.dylib 2>/dev/null; sudo ./vkfill ./libvulkan_radeon.dylib 65536 2>&1; echo rc=$?; ioreg -l -w0 -c Navi33Probe | grep -E "Navi33,(ClientGpu|ClientGpuStall|ClientGpuFault|N48n)"; sudo dmesg | grep -E "Navi33Probe: (n48n|client GPU)" | tail -40' > "$R/vkfill.txt" 2>&1 || true
cat "$R/vkfill.txt"
ssh macos-vm 'sudo shutdown -h now' >/dev/null 2>&1 || true
for i in $(seq 1 60); do pgrep -f "^qemu-system-x86_64" >/dev/null || break; sleep 2; done
tmux kill-session -t n33-vk 2>/dev/null || true
echo "run dir $R"
