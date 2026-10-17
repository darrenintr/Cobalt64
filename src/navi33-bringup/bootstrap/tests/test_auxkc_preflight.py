#!/usr/bin/env python3
from __future__ import annotations
import pathlib
import plistlib
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
from auxkc_preflight import inspect_kext, CPU_TYPE_X86_64


def sample(folder: pathlib.Path, device="0x74801002", arch=CPU_TYPE_X86_64):
    content = folder / "Contents"
    macho = content / "MacOS"
    macho.mkdir(parents=True)
    p = {
        "CFBundleIdentifier": "com.cobalt64.navi33gopfb",
        "CFBundleExecutable": "Navi33GOPFB",
        "IOKitPersonalities": {"Navi33GOPFB": {
            "IOClass": "Navi33GOPFB", "IOMatchCategory": "IOFramebuffer",
            "IOPCIPrimaryMatch": device}},
        "OSBundleLibraries": {"com.apple.iokit.IOGraphicsFamily": "1.0"},
    }
    (content / "Info.plist").write_bytes(plistlib.dumps(p))
    (macho / "Navi33GOPFB").write_bytes(b"\xcf\xfa\xed\xfe" + struct.pack("<I", arch))


class AuxKCPreflightTests(unittest.TestCase):
    def test_good(self):
        with tempfile.TemporaryDirectory() as directory:
            folder = pathlib.Path(directory) / "Navi33GOPFB.kext"
            sample(folder)
            report = inspect_kext(folder)
            self.assertTrue(report["ok"], report["errors"])
            self.assertTrue(report["requires_auxkc"])
            self.assertFalse(report["open_core_kernel_add_supported"])
            self.assertTrue(report["has_not_installed_anything"])
            self.assertTrue(folder.is_dir())

    def test_wrong_gpu_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            folder = pathlib.Path(directory) / "Navi33GOPFB.kext"
            sample(folder, device="0x75501002")
            self.assertFalse(inspect_kext(folder)["ok"])

    def test_arm_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            folder = pathlib.Path(directory) / "Navi33GOPFB.kext"
            sample(folder, arch=0x0100000c)
            self.assertFalse(inspect_kext(folder)["ok"])

    def test_no_kext_refused(self):
        with tempfile.TemporaryDirectory() as directory:
            report = inspect_kext(pathlib.Path(directory) / "missing.kext")
            self.assertFalse(report["ok"])


if __name__ == "__main__":
    unittest.main()
