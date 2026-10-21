#!/usr/bin/env python3
"""Build independent RX7600 *read-only probe* and recovery OpenCore EFI copies.

This never modifies an existing EFI, disk, NVRAM, or macOS Kext collection.
It does not grant GPU command submission or override Apple Kext policy.
"""
from __future__ import annotations
import argparse
import copy
import json
import pathlib
import plistlib
import shutil
import struct
import sys

from boot_profiles import APPLE_BOOT_GUID, ProfileError, _tokens

CONTROLLED = ("navi33-probe=", "navi33-client=", "navi33-client-off=",
              "navi33-hw-probe=", "navi33-hw-off=", "navi33-dma-",
              "navi33-fb=", "navi33-fb-off=", "navi33-psp-",
              "navi33-sdma-", "navi33-mmio-")
LIMIT = 512 * 1024 * 1024

def _contained(a: pathlib.Path, b: pathlib.Path) -> bool:
    return a == b or a in b.parents

def _validate_tree(directory: pathlib.Path) -> None:
    total = 0
    for item in directory.rglob("*"):
        if item.is_symlink():
            raise ProfileError(f"symbolic link refused: {item}")
        if item.is_file():
            total += item.stat().st_size
            if total > LIMIT:
                raise ProfileError("EFI or Kext exceeds 512 MiB limit")

def _validate_kext(kext: pathlib.Path) -> None:
    info = kext / "Contents" / "Info.plist"
    binary = kext / "Contents" / "MacOS" / "Navi33Probe"
    if kext.name != "Navi33Probe.kext" or not info.is_file() or not binary.is_file():
        raise ProfileError("requires built Navi33Probe.kext with executable")
    try:
        pl = plistlib.loads(info.read_bytes())
        p = pl["IOKitPersonalities"]["Navi33Probe"]
        if (pl.get("CFBundleVersion") != "0.0.66" or
                pl["CFBundleIdentifier"] != "com.cobalt64.navi33probe" or
                pl["CFBundleExecutable"] != "Navi33Probe" or
                p["IOPCIPrimaryMatch"] != "0x74801002" or
                p["IOMatchCategory"] != "Navi33ReadOnlyProbe"):
            raise ProfileError("incorrect Kext personality or PCI ID")
    except (KeyError, TypeError, plistlib.InvalidFileException) as exc:
        raise ProfileError("malformed Navi33Probe Kext plist") from exc
    # Prevent accidentally injecting an arm64-only bundle into x86_64 Tahoe.
    h = binary.read_bytes()[:8]
    if len(h) != 8 or h[:4] != b"\xcf\xfa\xed\xfe" or struct.unpack("<I", h[4:8])[0] != 0x01000007:
        raise ProfileError("Navi33Probe binary is not x86_64 Mach-O")

def _config(config: dict, probe: bool) -> dict:
    if not isinstance(config, dict) or not isinstance(config.get("Kernel",{}).get("Add"), list):
        raise ProfileError("missing Kernel/Add in OpenCore config")
    nv = config.get("NVRAM",{})
    try:
        boot = nv["Add"][APPLE_BOOT_GUID]["boot-args"]
    except (KeyError, TypeError) as exc:
        raise ProfileError("missing OpenCore boot-args") from exc
    tokens = [x for x in _tokens(boot) if not any(x.startswith(k) for k in CONTROLLED)]
    args = ["navi33-dma-off=1", "navi33-dma-stage-off=1",
            "navi33-fb-off=1", "navi33-psp-off=1", "navi33-sdma-off=1"]
    if probe:
        args.extend(["navi33-probe=1", "navi33-client=1",
                     "navi33-hw-probe=1"])
    else:
        args.extend(["navi33-hw-off=1", "navi33-client-off=1"])
    copied = copy.deepcopy(config)
    adds = copied["Kernel"]["Add"]
    if any("navi33probe" in str(x.get("BundlePath","")).lower() for x in adds):
        raise ProfileError("source EFI already injects Navi33Probe; refusing duplicate")
    if probe:
        adds.append({
            "Arch":"x86_64", "BundlePath":"Navi33Probe.kext",
            "Comment":"Cobalt64 RX7600 read-only MMIO diagnostics only",
            "Enabled":True, "ExecutablePath":"Contents/MacOS/Navi33Probe",
            "PlistPath":"Contents/Info.plist", "MinKernel":"", "MaxKernel":""
        })
    copied["NVRAM"]["Add"][APPLE_BOOT_GUID]["boot-args"] = " ".join(tokens+args)
    delete = copied["NVRAM"].setdefault("Delete",{})
    if not isinstance(delete,dict):
        raise ProfileError("malformed NVRAM/Delete")
    fields = delete.setdefault(APPLE_BOOT_GUID,[])
    if not isinstance(fields,list) or not all(isinstance(x,str) for x in fields):
        raise ProfileError("malformed NVRAM/Delete boot-args list")
    if "boot-args" not in fields: fields.append("boot-args")
    return copied

def build(source: pathlib.Path, kext: pathlib.Path,
          out: pathlib.Path) -> dict:
    source, kext, out = source.resolve(), kext.resolve(), out.resolve()
    if not source.is_dir() or source.name != "EFI":
        raise ProfileError("source must be a copied directory named EFI")
    if not kext.is_dir():
        raise ProfileError("compiled Kext directory missing")
    for root in (source,kext):
        if _contained(root,out) or _contained(out,root):
            raise ProfileError("output must be separate from source and Kext")
    if out.exists(): raise ProfileError("output already exists; never overwrite")
    _validate_tree(source)
    _validate_tree(kext)
    _validate_kext(kext)
    if not (source/"BOOT"/"BOOTx64.efi").is_file() or not (source/"OC"/"OpenCore.efi").is_file():
        raise ProfileError("missing OpenCore or BOOTx64.efi")
    cfg = source/"OC"/"config.plist"
    if not cfg.is_file(): raise ProfileError("missing OC/config.plist")
    try:
        original = plistlib.loads(cfg.read_bytes())
    except (TypeError,ValueError,plistlib.InvalidFileException) as exc:
        raise ProfileError("invalid OpenCore config.plist") from exc
    configs = {"probe":_config(original,True),"recovery":_config(original,False)}
    out.mkdir(parents=True,exist_ok=False)
    try:
        for name,config in configs.items():
            dest = out/name/"EFI"
            shutil.copytree(source,dest,symlinks=False)
            if name == "probe":
                shutil.copytree(kext,dest/"OC"/"Kexts"/"Navi33Probe.kext",
                                symlinks=False)
            (dest/"OC"/"config.plist").write_bytes(
                plistlib.dumps(config,fmt=plistlib.FMT_XML))
        report = {
            "profiles": ["probe","recovery"],
            "note": "NOT boot-tested. Each profile is a different EFI tree; copy one to a DIFFERENT USB and keep your original working EFI.",
            "probe_boot_args": configs["probe"]["NVRAM"]["Add"][APPLE_BOOT_GUID]["boot-args"],
            "recovery_boot_args": configs["recovery"]["NVRAM"]["Add"][APPLE_BOOT_GUID]["boot-args"],
        }
        (out/"README.txt").write_text(
            "SAFE: no bootable media was written or mounted.\n"
            "probe/EFI injects only Navi33Probe Kext, read-only BAR5 polling.\n"
            "recovery/EFI does not inject that Kext and explicitly disables its client.\n"
            "Keep an unchanged, separately bootable known-good EFI.\n"
            "Do not inject or enable GPU MMIO write backends.\n"
            "The Kext may be rejected by macOS security/signature policy.\n"
            "No GPU compute is performed by n33compute --probe.\n",
            encoding="utf-8")
        (out/"report.json").write_text(json.dumps(report,indent=2)+"\n",
                                       encoding="utf-8")
        return report
    except BaseException:
        shutil.rmtree(out,ignore_errors=True)
        raise

def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--source-efi",type=pathlib.Path,required=True)
    ap.add_argument("--kext",type=pathlib.Path,required=True)
    ap.add_argument("--output-dir",type=pathlib.Path,required=True)
    options=ap.parse_args(argv)
    try:
        print(json.dumps(build(options.source_efi,options.kext,
                               options.output_dir),indent=2))
        return 0
    except (ProfileError,OSError) as exc:
        print(f"ERROR: {exc}",file=sys.stderr)
        return 1
if __name__=="__main__":
    raise SystemExit(main())
