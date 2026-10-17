#!/usr/bin/env python3
"""Safe-by-default AuxKC staging for an already booted, recoverable test macOS.

Default = PLAN. --stage-live only writes to the *currently running* macOS
/Library/Extensions, never a guest/host APFS volume that isn't mounted as /.
No SIP, OpenCore, EFI, Apple system volumes, kernelcache or NVRAM edits.
No automatic reboot or alternate-volume mount. A working installer is NOT
provided by this script.
"""
from __future__ import annotations

import argparse
import json
import os
import pathlib
import platform
import shutil
import subprocess
import sys
import tempfile

from auxkc_preflight import inspect_kext

DEST = pathlib.Path("/Library/Extensions/Navi33GOPFB.kext")
ACK = "THIS_RUNNING_TEST_MACOS"


class StageError(RuntimeError):
    pass


def stage_plan(source: pathlib.Path) -> dict:
    source = source.resolve()
    result = inspect_kext(source)
    result["mode"] = "PLAN_ONLY"
    result["destination"] = str(DEST)
    result["will_modify_files"] = False
    result["bootargs_recovery"] = "navi33-fb-off=1"
    result["security_changes"] = "NONE"
    result["requires"] = (
        "The TARGET test macOS must already boot, have a separate recovery "
        "path and accept third-party Kext approval; a VM stages only its own "
        "running volume, not a different bare-metal installation."
    )
    return result


def _run_checked(argv: list[str], **kwargs):
    try:
        return subprocess.run(argv, check=True, text=True, **kwargs)
    except (subprocess.CalledProcessError, OSError) as exc:
        raise StageError(f"command failed: {argv[0]}: {exc}") from exc


def stage_live(source: pathlib.Path, acknowledgement: str, allow_adhoc: bool = False) -> str:
    result = stage_plan(source)
    if not result["ok"]:
        raise StageError("built Kext failed preflight: " + "; ".join(result["errors"]))
    if acknowledgement != ACK:
        raise StageError("missing explicit --acknowledge-this-volume confirmation")
    if sys.platform != "darwin" or platform.machine() != "x86_64":
        raise StageError("live staging is only supported on a RUNNING x86_64 macOS test installation")
    if os.geteuid() != 0:
        raise StageError("run as root on your TEST macOS only; never on your sole recoverable install")
    if not DEST.parent.is_dir() or DEST.parent.is_symlink():
        raise StageError("/Library/Extensions missing or replaced by a symlink")
    if DEST.exists() or DEST.is_symlink():
        raise StageError("refusing to overwrite an installed Navi33GOPFB; remove it manually after backup")

    source = source.resolve()
    _run_checked(["/usr/bin/codesign", "--verify", "--verbose=2", str(source)])
    details = _run_checked(["/usr/bin/codesign", "-d", "--verbose=4", str(source)],
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    signature = (details.stdout or "") + (details.stderr or "")
    if "Signature=adhoc" in signature and not allow_adhoc:
        raise StageError("CI Kext has an ad-hoc signature; normal macOS policy may refuse it. "
                         "A properly authorized kext-signing identity is preferred. "
                         "For an isolated test volume, --allow-adhoc-test only bypasses "
                         "THIS script's check, NOT macOS signature policy or SIP.")

    # Stage into an isolated same-filesystem temporary directory first.
    # Only the final rename publishes the Kext at its live repository path.
    scratch = pathlib.Path(tempfile.mkdtemp(prefix=".navi33-stage-", dir=str(DEST.parent)))
    try:
        candidate = scratch / DEST.name
        _run_checked(["/usr/bin/ditto", str(source), str(candidate)])
        _run_checked(["/usr/sbin/chown", "-R", "root:wheel", str(candidate)])
        _run_checked(["/bin/chmod", "-R", "u+rwX,go+rX,go-w", str(candidate)])
        _run_checked(["/usr/bin/codesign", "--verify", "--verbose=2", str(candidate)])
        if DEST.exists() or DEST.is_symlink():
            raise StageError("destination appeared during staging; refusing to overwrite it")
        os.rename(candidate, DEST)
    finally:
        shutil.rmtree(scratch, ignore_errors=True)

    # kmutil invokes the running test macOS's own approval and AuxKC build
    # flow. An approval-required / reboot-required result is NOT an install
    # success and we intentionally leave that decision to the OS/user.
    attempt = subprocess.run(["/usr/bin/kmutil", "load", "-p", str(DEST)],
                             check=False, text=True,
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if attempt.returncode != 0:
        return (f"Kext copied to {DEST}, but kmutil returned {attempt.returncode}; "
                "additional OS approval/signing/reboot may be needed. "
                "This command did not bypass SIP or change the EFI. "
                f"kmutil: {(attempt.stderr or attempt.stdout).strip()[:500]}")
    return (f"Kext staged via kmutil on the currently running test macOS: {DEST}. "
            "Do not assume it is in the AuxKC until the OS finishes approval "
            "and required reboot. No reboot has been issued.")


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--kext", type=pathlib.Path, required=True)
    parser.add_argument("--stage-live", action="store_true", help="DANGEROUS: modify currently running macOS")
    parser.add_argument("--acknowledge-this-volume", default="", help=f"must equal {ACK} for live staging")
    parser.add_argument("--allow-adhoc-test", action="store_true",
                        help="allow ad-hoc test signature past this script's guard; does not bypass macOS policy")
    args = parser.parse_args(argv)
    try:
        if not args.stage_live:
            report = stage_plan(args.kext)
            print(json.dumps(report, indent=2, sort_keys=True))
            return 0 if report["ok"] else 1
        print(stage_live(args.kext, args.acknowledge_this_volume, args.allow_adhoc_test))
        return 0
    except (StageError, OSError) as exc:
        print("ERROR: " + str(exc), file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
