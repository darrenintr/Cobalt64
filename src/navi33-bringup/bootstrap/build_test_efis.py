#!/usr/bin/env python3
"""Offline, non-destructive OpenCore EFI *directory* profile packager.

The output is NOT a disk/ISO, installer, signed driver or verified boot media.
A human must copy the desired output's EFI directory to a DIFFERENT,
recoverable FAT32 USB volume after inspecting generated OpenCore configs.
Never overwrite the only working EFI or write partitions in this tool.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import plistlib
import shutil
import sys

from boot_profiles import (ProfileError, APPLE_BOOT_GUID, make_profile)

PROFILE_NAMES=("recovery","tahoe-firmware","tahoe-fb-trial")
MAX_SOURCE_BYTES=512*1024*1024

def _inside(parent:Path,child:Path)->bool:
    return child==parent or parent in child.parents

def pack(source:Path,output:Path)->dict:
    source=source.resolve()
    output=output.resolve()
    if not source.is_dir() or source.name!="EFI":
        raise ProfileError("source must be an existing directory named EFI")
    if _inside(source,output) or _inside(output,source):
        raise ProfileError("output cannot be inside/above source EFI directory")
    if output.exists():
        raise ProfileError("output already exists; cannot overwrite existing boot entries")
    config_path=source/"OC"/"config.plist"
    if not (source/"BOOT"/"BOOTx64.efi").is_file() or not config_path.is_file() or not (source/"OC"/"OpenCore.efi").is_file():
        raise ProfileError("source must contain BOOT/BOOTx64.efi, OC/OpenCore.efi and OC/config.plist")
    total=0
    for f in source.rglob("*"):
        if f.is_symlink():
            raise ProfileError("source EFI contains symbolic links; refusing recursive copy")
        if f.is_file():
            total+=f.stat().st_size
            if total>MAX_SOURCE_BYTES:
                raise ProfileError("source EFI exceeds conservative 512MiB size limit")
    original=config_path.read_bytes()
    try:
        config=plistlib.loads(original)
    except (ValueError,TypeError,plistlib.InvalidFileException) as exc:
        raise ProfileError("invalid original OpenCore plist") from exc
    outputs={name:plistlib.dumps(make_profile(config,name),fmt=plistlib.FMT_XML)
             for name in PROFILE_NAMES}

    # Validate everything before writing; no EFI partition or NVRAM touched.
    output.mkdir(parents=True,exist_ok=False)
    report={
        "source_sha256": hashlib.sha256(original).hexdigest(),
        "source":str(source),
        "notice":"NOT BOOT-TESTED. Copy exactly one profile/EFI to an independent FAT32 USB after verifying recovery. Do not overwrite active EFI.",
        "profiles":{}
    }
    try:
        for name in PROFILE_NAMES:
            dest=output/name/"EFI"
            shutil.copytree(source,dest,symlinks=False)
            target=dest/"OC"/"config.plist"
            target.write_bytes(outputs[name])
            parsed=plistlib.loads(target.read_bytes())
            args=parsed["NVRAM"]["Add"][APPLE_BOOT_GUID]["boot-args"]
            report["profiles"][name]={
                "efi_directory":str(dest),
                "sha256":hashlib.sha256(outputs[name]).hexdigest(),
                "boot_args":args,
                "whatevergreen_enabled":any(
                    x.get("Enabled") is True and
                    str(x.get("BundlePath","")).lower().endswith("whatevergreen.kext")
                    for x in parsed["Kernel"]["Add"]),
            }
        (output/"README.txt").write_text(
            "These are ALTERNATIVE EFI directories, not OpenCore picker entries.\n"
            "For a separate test USB, copy ONLY <profile>/EFI to its FAT32 root.\n"
            "Always keep the original EFI and a separate independent boot device.\n"
            "No macOS target Kext/AuxKC has been modified; Tahoe basic display\n"
            "is possible only if the existing firmware framebuffer survives.\n"
            "tahoe-fb-trial additionally requires a Kext already approved in\n"
            "the actual TARGET installation's Auxiliary Kernel Collection.\n"
            "Do NOT put Navi33GOPFB.kext into OC/Kernel/Add.\n",
            encoding="utf-8")
        (output/"profiles.json").write_text(json.dumps(report,indent=2,sort_keys=True)+"\n",encoding="utf-8")
    except BaseException:
        # Remove only our newly created output tree, never the source.
        shutil.rmtree(output,ignore_errors=True)
        raise
    return report

def main(argv=None)->int:
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument("--source-efi",type=Path,required=True,
                   help="path to existing EFI directory; READ ONLY")
    p.add_argument("--output-dir",type=Path,required=True,
                   help="NEW output directory, outside original EFI")
    a=p.parse_args(argv)
    try:
        print(json.dumps(pack(a.source_efi,a.output_dir),indent=2,sort_keys=True))
        return 0
    except (ProfileError,OSError) as e:
        print("ERROR: "+str(e),file=sys.stderr)
        return 1

if __name__=="__main__":
    raise SystemExit(main())
