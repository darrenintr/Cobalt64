#!/usr/bin/env bash
# Build the Navi33 OpenCore disk for the Tahoe VM from a CI usb artifact.
#   vm/make-opencore.sh ARTIFACT_DIR [--oc OPENCORE_RELEASE_DIR] [--hostfb FILE] [--psp-load|--psp-ring|--psp-fw|--smu-enable|--smu-scpm]
# ARTIFACT_DIR is an unpacked navi33-usb-macos-26-intel artifact (EFI/ + navi33/).
# The VM's own boot/OpenCore.qcow2 is only read: the result is a separate
# $VM_DIR/navi33/OpenCore-navi33.qcow2, so the VM's normal boots never see
# Navi33Probe. --oc takes an unpacked OpenCorePkg release (with X64/) and
# swaps in its OpenCore.efi, BOOTx64.efi and every driver of the same name:
# the VM ships 1.0.4, which fails to inject kexts on macOS 26 ("Invalid
# Parameter"; fixed in 1.0.5). --psp-load opens the PSP write gate
# (navi33-psp-load=1, no navi33-psp-off); like on the stick, only for an
# approved, hand-run attempt. --hostfb takes the hostfb.txt that
# vm/prepare-card.sh records and passes its console as navi33-hostfb-offset
# and -bytes (kext 0.0.8+): the guest has no GOP console of its own.
# --smu-scpm opens only the separately approved enable-only SMU experiment.
set -euo pipefail
VM_DIR="${VM_DIR:-$HOME/macos tahoe}"
die() { echo "make-opencore: $*" >&2; exit 1; }
[ $# -ge 1 ] || die "usage: $0 ARTIFACT_DIR [--oc OPENCORE_RELEASE_DIR] [--hostfb FILE] [--psp-load]"
ART="$(cd -- "$1" && pwd)"; shift
PSP_LOAD=0; PSP_RING=0; PSP_FW=0; PSP_AUTOLOAD=0; SMU_ENABLE=0; SMU_SCPM=0; OCREL=""; HOSTFB=""
while [ $# -gt 0 ]; do
  case "$1" in
    --smu-enable) SMU_ENABLE=1 ;;
    --smu-scpm) SMU_ENABLE=1; SMU_SCPM=1 ;;
    --psp-load) PSP_LOAD=1 ;;
    --psp-ring) PSP_RING=1 ;;
    --psp-fw) PSP_FW=1 ;;
    --psp-autoload) PSP_AUTOLOAD=1 ;;
    --oc) OCREL="$(cd -- "${2:?--oc needs a directory}" && pwd)"; shift ;;
    --hostfb) HOSTFB="${2:?--hostfb needs a file}"; shift ;;
    *) die "unknown option $1" ;;
  esac
  shift
done
[ -z "$OCREL" ] || [ -f "$OCREL/X64/EFI/OC/OpenCore.efi" ] || die "no X64/EFI/OC/OpenCore.efi in $OCREL"
[ -z "$HOSTFB" ] || [ -r "$HOSTFB" ] || die "cannot read $HOSTFB"
[ $((PSP_LOAD + PSP_RING + PSP_FW + SMU_ENABLE)) -le 1 ] || die "open one write gate at a time: --psp-load, --psp-ring, --psp-fw or --smu-enable"
[ "$PSP_AUTOLOAD" = 0 ] || [ "$PSP_FW" = 1 ] || die "--psp-autoload (AUTOLOAD_RLC) only together with --psp-fw"
KEXT="$ART/EFI/OC/Kexts/Navi33Probe.kext"
SRC="$VM_DIR/boot/OpenCore.qcow2"
OUT_DIR="$VM_DIR/navi33"; OUT="$OUT_DIR/OpenCore-navi33.qcow2"
[ -f "$KEXT/Contents/MacOS/Navi33Probe" ] || die "no Navi33Probe.kext in $ART"
[ -f "$SRC" ] || die "no VM OpenCore disk at $SRC"
for t in qemu-img sgdisk mcopy mdeltree mdir python3; do
  command -v "$t" >/dev/null || die "$t not found"
done
mkdir -p "$OUT_DIR"
WORK="$(mktemp -d)"; trap 'rm -rf "$WORK"' EXIT
# qemu-img takes the image lock, so this also refuses while the VM runs.
qemu-img convert -O raw "$SRC" "$WORK/oc.raw"
first="$(sgdisk -i 1 "$WORK/oc.raw" | sed -n 's/^First sector: \([0-9]*\).*/\1/p')"
[ -n "$first" ] || die "no partition 1 in $SRC"
IMG="$WORK/oc.raw@@$((first * 512))"
export MTOOLS_SKIP_CHECK=1
mcopy -n -i "$IMG" ::/EFI/OC/config.plist "$WORK/config.plist"
mdeltree -i "$IMG" ::/EFI/OC/Kexts/Navi33Probe.kext 2>/dev/null || true
mcopy -s -i "$IMG" "$KEXT" ::/EFI/OC/Kexts/
OCVER="as shipped with the VM"
if [ -n "$OCREL" ]; then
  # OpenRuntime and the other OC drivers must come from the same release as OpenCore.efi.
  mcopy -o -i "$IMG" "$OCREL/X64/EFI/OC/OpenCore.efi" ::/EFI/OC/OpenCore.efi
  mcopy -o -i "$IMG" "$OCREL/X64/EFI/BOOT/BOOTx64.efi" ::/EFI/BOOT/BOOTx64.efi
  for drv in $(mdir -b -i "$IMG" ::/EFI/OC/Drivers); do
    f="$OCREL/X64/EFI/OC/Drivers/$(basename "$drv")"
    [ -f "$f" ] && mcopy -o -i "$IMG" "$f" "::/EFI/OC/Drivers/$(basename "$drv")"
  done
  OCVER="$(basename "$OCREL"), OpenCore.efi sha256 $(sha256sum "$OCREL/X64/EFI/OC/OpenCore.efi" | cut -c1-16)…"
fi
python3 -I - "$WORK/config.plist" "$PSP_LOAD" "$KEXT/Contents/MacOS/Navi33Probe" \
  "$OUT_DIR/OpenCore-navi33.txt" "$OCVER" "$HOSTFB" "$PSP_RING" "$PSP_FW" "$PSP_AUTOLOAD" "$SMU_ENABLE" "$SMU_SCPM" <<'EOF'
import plistlib,struct,sys,uuid
path,psp_load,binary,manifest,ocver,hostfb=sys.argv[1],sys.argv[2]=="1",sys.argv[3],sys.argv[4],sys.argv[5],sys.argv[6]
psp_ring=sys.argv[7]=="1"
psp_fw=sys.argv[8]=="1"; psp_autoload=sys.argv[9]=="1"
smu_enable=sys.argv[10]=="1"
smu_scpm=sys.argv[11]=="1"
c=plistlib.load(open(path,"rb"))
add=[k for k in c["Kernel"]["Add"] if k.get("BundlePath")!="Navi33Probe.kext"]
add.append({"Arch":"x86_64","BundlePath":"Navi33Probe.kext",
    "Comment":"Cobalt64 RX7600 diagnostics (VM)","Enabled":True,
    "ExecutablePath":"Contents/MacOS/Navi33Probe","MaxKernel":"","MinKernel":"",
    "PlistPath":"Contents/Info.plist"})
c["Kernel"]["Add"]=add
nv=c["NVRAM"]["Add"]["7C436110-AB2A-4BBB-A880-FE41995C9F82"]
# Keep the VM's own args. serial=3 plus kprintf (debug 0x8) sends the kernel
# console, kprintf and panics to COM1, which the launcher writes to a file.
args=[a for a in nv.get("boot-args","").split()
      if not a.startswith(("navi33-","debug=","serial=","msgbuf=")) and a!="-v"]
args+=["-v","debug=0x108","serial=3","msgbuf=1048576",
       "navi33-probe=1","navi33-client=1","navi33-hw-probe=1",
       "navi33-dma-off=1","navi33-dma-stage-off=1","navi33-fb-off=1",
       "navi33-sdma-off=1",
       "navi33-psp-load=1" if psp_load else "navi33-psp-ring=1" if psp_ring else
       "navi33-psp-fw=1" if psp_fw else "navi33-smu-enable=1" if smu_enable else "navi33-psp-off=1"]
if not smu_enable: args.append("navi33-smu-off=1")
if smu_scpm: args.append("navi33-smu-scpm=1")
if psp_autoload: args.append("navi33-psp-autoload=1")   # AUTOLOAD_RLC: its own gate
fb={}
if hostfb:
    fb=dict(l.split("=",1) for l in open(hostfb).read().split("\n") if l[:1].isalpha() and "=" in l)
    off,size=int(fb["offset"],16),int(fb["bytes"],16)
    if off&0xfff or not size: sys.exit(f"bad host console in {hostfb}")
    args+=[f"navi33-hostfb-offset={off:#x}",f"navi33-hostfb-bytes={size:#x}"]
nv["boot-args"]=" ".join(dict.fromkeys(args))
if "boot-args" not in c["NVRAM"]["Delete"]["7C436110-AB2A-4BBB-A880-FE41995C9F82"]:
    sys.exit("config does not delete boot-args, so NVRAM would override them")
c["Misc"]["Boot"]["Timeout"]=5    # 45 s on the VM's own disk, and nobody is at the picker
# OpenCore's own log to COM1 too: with a GOP its errors otherwise only reach the screen.
c["Misc"]["Debug"]["Target"]|=0x08
plistlib.dump(c,open(path,"wb"))
d=open(binary,"rb").read(); off=32; kuuid="?"
for _ in range(struct.unpack_from("<I",d,16)[0]):
    cmd,size=struct.unpack_from("<II",d,off)
    if cmd==0x1b: kuuid=str(uuid.UUID(bytes=d[off+8:off+24])).upper()
    off+=size
info=f"kext UUID {kuuid}\nOpenCore {ocver}\nboot-args {nv['boot-args']}\n"
if fb: info+=f"hostfb boot_id={fb['boot_id']} offset={fb['offset']} bytes={fb['bytes']}\n"
open(manifest,"w").write(info); print(info,end="")
EOF
mcopy -o -i "$IMG" "$WORK/config.plist" ::/EFI/OC/config.plist
qemu-img convert -O qcow2 "$WORK/oc.raw" "$OUT.new"
mv -f "$OUT.new" "$OUT"
echo "wrote $OUT"
