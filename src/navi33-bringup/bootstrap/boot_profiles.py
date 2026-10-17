#!/usr/bin/env python3
"""Generate *separate* OpenCore boot-profile plists; never modify the EFI.

No UEFI or macOS interaction: stdlib-only, safe to run on Linux/macOS.
Every generated profile is a candidate configuration, NOT a selectable
OpenCore picker entry. Deployment requires a separately recoverable EFI.
"""
from __future__ import annotations

import argparse
import copy
import hashlib
import json
from pathlib import Path
import plistlib
import sys

APPLE_BOOT_GUID = "7C436110-AB2A-4BBB-A880-FE41995C9F82"
CONTROLLED_KEYS = ("navi33-fb=", "navi33-fb-off=", "navi33-probe=",
                   "navi33-dma-preflight=", "navi33-dma-off=")
OUTPUTS = ("recovery", "vesa-baseline", "fb-trial",
           "tahoe-firmware", "tahoe-fb-trial")


class ProfileError(ValueError):
    pass


def _tokens(s: str) -> list[str]:
    if not isinstance(s, str):
        raise ProfileError("OpenCore boot-args must be a plist string")
    # OpenCore boot-args are whitespace separated. Quoting is not rewritten:
    # accepting it here could silently corrupt unrelated third-party flags.
    if '"' in s or "'" in s or "\n" in s or "\r" in s:
        raise ProfileError("quoted/multiline boot-args require manual review")
    return s.split()


def _config_bootargs(config: dict) -> str:
    try:
        nvram = config["NVRAM"]
        args = nvram["Add"][APPLE_BOOT_GUID]["boot-args"]
    except (KeyError, TypeError) as exc:
        raise ProfileError("missing NVRAM/Add Apple GUID boot-args") from exc
    _tokens(args)
    if not isinstance(config.get("Kernel", {}).get("Add"), list):
        raise ProfileError("missing or malformed Kernel/Add")
    for item in config["Kernel"]["Add"]:
        if not isinstance(item, dict):
            raise ProfileError("Kernel/Add contains a non-dictionary entry")
        if "navi33gopfb" in str(item.get("BundlePath", "")).lower():
            raise ProfileError("Navi33GOPFB must NOT be injected from OpenCore Kernel/Add (IOGraphicsFamily dependency)")
    return args


def make_profile(config: dict, name: str) -> dict:
    if name not in OUTPUTS:
        raise ProfileError(f"unknown profile {name!r}")
    existing = _tokens(_config_bootargs(config))
    tokens = [x for x in existing if not any(x.startswith(prefix) for prefix in CONTROLLED_KEYS)]
    # Tahoe 26 has reported WhateverGreen AMD connector-hook regressions.
    # The firmware profiles avoid invoking WEG entirely; -radvesa is not
    # meaningful without WEG and is removed ONLY in these named profiles.
    # We do not block Lilu or any other unrelated kernel extensions.
    tahoe = name in ("tahoe-firmware", "tahoe-fb-trial")
    if tahoe:
        tokens = [x for x in tokens if x != "-radvesa"]
    # Never modify other boot arguments, SMBIOS, kernel patches, DeviceProperties
    # or SIP. The only NVRAM non-boot-args edit is deleting stale boot-args
    # *before* Add, to avoid an already set firmware variable defeating the test.
    # No profile ever opts in to kernel DMA diagnostics. The kill switch
    # is added to each variant to prevent stale boot arguments activating it.
    tokens.append("navi33-dma-off=1")
    if name == "recovery":
        tokens.append("navi33-fb-off=1")
    elif name == "vesa-baseline":
        if "-radvesa" not in tokens:
            tokens.append("-radvesa")
        tokens.append("navi33-fb-off=1")
    elif name == "fb-trial":
        if "-radvesa" not in tokens:
            tokens.append("-radvesa")
        tokens.append("navi33-fb=1")
    elif name == "tahoe-firmware":
        tokens.append("navi33-fb-off=1")
    elif name == "tahoe-fb-trial":
        tokens.append("navi33-fb=1")

    output = copy.deepcopy(config)
    if tahoe:
        # Disable only WEG in the ALTERNATE output config. On Tahoe 26,
        # WEG's AMD connector patch can itself cause a panic; do not enable
        # it merely to request -radvesa. Never delete its .kext from EFI.
        for entry in output["Kernel"]["Add"]:
            if str(entry.get("BundlePath", "")).lower().endswith("whatevergreen.kext"):
                entry["Enabled"] = False
    output["NVRAM"]["Add"][APPLE_BOOT_GUID]["boot-args"] = " ".join(tokens)
    delete = output["NVRAM"].setdefault("Delete", {})
    if not isinstance(delete, dict):
        raise ProfileError("NVRAM/Delete must be a dictionary")
    fields = delete.setdefault(APPLE_BOOT_GUID, [])
    if not isinstance(fields, list) or any(not isinstance(x, str) for x in fields):
        raise ProfileError("NVRAM/Delete Apple GUID must be an array of strings")
    if "boot-args" not in fields:
        fields.append("boot-args")
    return output


def has_whatevergreen(config: dict) -> bool:
    return any(
        x.get("Enabled") is True
        and str(x.get("BundlePath", "")).lower().endswith("whatevergreen.kext")
        for x in config["Kernel"]["Add"]
    )


def generate(source: Path, destination: Path) -> dict:
    source = source.resolve()
    destination = destination.resolve()
    if not source.is_file():
        raise ProfileError(f"source not found: {source}")
    if destination == source.parent:
        raise ProfileError("output directory must NOT be the directory containing the source config.plist")
    with source.open("rb") as fd:
        original_bytes = fd.read()
    if len(original_bytes) > 4 * 1024 * 1024:
        raise ProfileError("unreasonably large config.plist")
    try:
        config = plistlib.loads(original_bytes)
    except (ValueError, TypeError, OSError, plistlib.InvalidFileException) as exc:
        raise ProfileError("invalid OpenCore plist") from exc
    if not isinstance(config, dict):
        raise ProfileError("OpenCore plist root must be a dictionary")
    _config_bootargs(config)

    # Construct and validate every output *before* creating any files.
    outputs = {
        name: plistlib.dumps(make_profile(config, name), fmt=plistlib.FMT_XML)
        for name in OUTPUTS
    }
    files = {name: destination / (name + ".plist") for name in OUTPUTS}
    manifest = destination / "profiles.json"
    for path in (*files.values(), manifest):
        if path.exists():
            raise ProfileError(f"refusing to overwrite existing file: {path}")
    destination.mkdir(parents=True, exist_ok=True)
    report = {
        "source": str(source),
        "source_sha256": hashlib.sha256(original_bytes).hexdigest(),
        "whatevergreen_enabled": has_whatevergreen(config),
        "warning": "Output files are NOT independently selectable OpenCore boot entries. Do not overwrite your only working EFI.",
        "profiles": {},
    }
    for name, data in outputs.items():
        # Exclusive creation prevents silent overwrite even if another
        # process creates a file between our checks and the write.
        with files[name].open("xb") as fd:
            fd.write(data)
        report["profiles"][name] = {
            "path": str(files[name]),
            "sha256": hashlib.sha256(data).hexdigest(),
            "boot_args": make_profile(config, name)["NVRAM"]["Add"][APPLE_BOOT_GUID]["boot-args"],
        }
    with manifest.open("x", encoding="utf-8") as fd:
        json.dump(report, fd, indent=2, sort_keys=True)
        fd.write("\n")
    return report


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", type=Path, required=True, help="source OpenCore config.plist (never modified)")
    parser.add_argument("--output-dir", type=Path, required=True, help="new folder for 5 profiles and checksums")
    args = parser.parse_args(argv)
    try:
        report = generate(args.config, args.output_dir)
    except (ProfileError, FileExistsError, PermissionError, OSError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1
    print(json.dumps(report, indent=2, sort_keys=True))
    if not report["whatevergreen_enabled"]:
        print("WARNING: no enabled WhateverGreen.kext in Kernel/Add; -radvesa may have no effect.", file=sys.stderr)
    print("All changes were written to NEW files only; original config.plist unchanged.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
