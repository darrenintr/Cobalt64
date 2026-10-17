#!/usr/bin/env python3
"""Inspect a built Navi33GOPFB.kext and output an AuxKC deployment plan.

Does NOT copy Kexts, call kmutil, rebuild kernel collections, or change SIP.
This is only an auditable preparation step for an eventual, recoverable macOS
installation. Running inside a VM only inspects the Kext; it does NOT install
anything into a different bare-metal macOS volume.
"""
from __future__ import annotations
import argparse
import json
import pathlib
import plistlib
import struct
import sys

BUNDLE = "com.cobalt64.navi33gopfb"
EXPECTED_PCI = "0x74801002"
# Mach-O 64-bit magic (little-endian bytes), then x86_64 CPU type.
MH_MAGIC_64_BYTES = b"\xcf\xfa\xed\xfe"
CPU_TYPE_X86_64 = 0x01000007


def inspect_kext(folder: pathlib.Path) -> dict:
    folder = folder.resolve()
    errors = []
    plist_path = folder / "Contents" / "Info.plist"
    exe = folder / "Contents" / "MacOS" / "Navi33GOPFB"
    p = {}
    try:
        p = plistlib.loads(plist_path.read_bytes())
    except (OSError, ValueError, TypeError, plistlib.InvalidFileException) as exc:
        errors.append(f"Info.plist cannot be read: {exc}")
    if p:
        if p.get("CFBundleIdentifier") != BUNDLE:
            errors.append("unexpected CFBundleIdentifier")
        if p.get("CFBundleExecutable") != "Navi33GOPFB":
            errors.append("unexpected CFBundleExecutable")
        personality = p.get("IOKitPersonalities", {}).get("Navi33GOPFB", {})
        if personality.get("IOClass") != "Navi33GOPFB":
            errors.append("wrong IOKit class")
        if personality.get("IOMatchCategory") != "IOFramebuffer":
            errors.append("missing framebuffer matching category")
        if personality.get("IOPCIPrimaryMatch") != EXPECTED_PCI:
            errors.append("PCI target differs from RX 7600")
        if "com.apple.iokit.IOGraphicsFamily" not in p.get("OSBundleLibraries", {}):
            errors.append("missing IOGraphicsFamily dependency")
    try:
        with exe.open("rb") as binary:
            header = binary.read(8)
        if header[:4] != MH_MAGIC_64_BYTES:
            errors.append("expected 64-bit Mach-O x86_64 executable")
        elif len(header) < 8 or struct.unpack("<I", header[4:8])[0] != CPU_TYPE_X86_64:
            errors.append("expected x86_64, not an ARM/other GPU driver")
    except OSError as exc:
        errors.append(f"executable cannot be read: {exc}")
    return {
        "kext": str(folder),
        "bundle_id": p.get("CFBundleIdentifier"),
        "pci_match": p.get("IOKitPersonalities", {}).get("Navi33GOPFB", {}).get("IOPCIPrimaryMatch"),
        "checked_macho_x86_64": not any("Mach-O" in e or "x86_64" in e or "executable" in e for e in errors),
        "ok": not errors,
        "errors": errors,
        "install_location_if_approved": "/Library/Extensions/Navi33GOPFB.kext",
        "requires_auxkc": True,
        "open_core_kernel_add_supported": False,
        "requires_running_target_macos_and_reboot": True,
        "has_not_installed_anything": True,
        "notes": [
            "A compiled Kext does not prove macOS Tahoe runtime compatibility or a working display.",
            "Kext must be approved and added to the target macOS AuxKC; do not copy into OpenCore Kernel/Add.",
            "A Kext prepared in a macOS VM does not automatically become part of a different bare-metal installation.",
            "Do not disable SIP, reset NVRAM, or touch a sole bootable EFI just to complete this preflight.",
            "Only use an approved/recoverable macOS test volume; macOS may require reboot and user approval.",
            "Suggested future commands, for a running TEST installation only: sudo ditto <Kext> /Library/Extensions/Navi33GOPFB.kext; sudo kmutil load -p /Library/Extensions/Navi33GOPFB.kext; sudo kmutil rebuild (if required).",
        ],
    }


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--kext", type=pathlib.Path, required=True)
    args = parser.parse_args(argv)
    report = inspect_kext(args.kext)
    print(json.dumps(report, indent=2, sort_keys=True))
    return 0 if report["ok"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
