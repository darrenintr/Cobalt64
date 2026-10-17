#!/usr/bin/env python3
"""No mutating operations run in CI: staging is explicitly gated."""
from __future__ import annotations
import pathlib
import plistlib
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
from auxkc_stage import StageError, stage_live, stage_plan, ACK


def fake(folder: pathlib.Path):
    p = folder / "Contents"
    (p / "MacOS").mkdir(parents=True)
    (p / "Info.plist").write_bytes(plistlib.dumps({
        "CFBundleIdentifier": "com.cobalt64.navi33gopfb",
        "CFBundleExecutable": "Navi33GOPFB",
        "IOKitPersonalities": {"Navi33GOPFB": {
            "IOClass": "Navi33GOPFB", "IOMatchCategory": "IOFramebuffer",
            "IOPCIPrimaryMatch": "0x74801002"}},
        "OSBundleLibraries": {"com.apple.iokit.IOGraphicsFamily": "1.0"},
    }))
    (p / "MacOS/Navi33GOPFB").write_bytes(b"\xcf\xfa\xed\xfe" + struct.pack("<I", 0x01000007))


class StageTests(unittest.TestCase):
    def test_plan_does_not_modify_source_or_os(self):
        with tempfile.TemporaryDirectory() as t:
            folder = pathlib.Path(t) / "Navi33GOPFB.kext"
            fake(folder)
            before = (folder / "Contents/Info.plist").read_bytes()
            report = stage_plan(folder)
            self.assertTrue(report["ok"], report["errors"])
            self.assertEqual(report["mode"], "PLAN_ONLY")
            self.assertFalse(report["will_modify_files"])
            self.assertEqual(report["security_changes"], "NONE")
            self.assertEqual((folder / "Contents/Info.plist").read_bytes(), before)

    def test_apply_requires_explicit_acknowledgment(self):
        with tempfile.TemporaryDirectory() as t:
            folder = pathlib.Path(t) / "Navi33GOPFB.kext"
            fake(folder)
            with self.assertRaises(StageError):
                stage_live(folder, "no")
            self.assertTrue(folder.exists())

    def test_live_apply_not_supported_on_linux_ci(self):
        if sys.platform == "darwin" and __import__("platform").machine() == "x86_64":
            self.skipTest("CI test never calls stage_live with authorization on macOS")
        with tempfile.TemporaryDirectory() as t:
            folder = pathlib.Path(t) / "Navi33GOPFB.kext"
            fake(folder)
            with self.assertRaises(StageError):
                stage_live(folder, ACK)
            self.assertTrue(folder.exists())


if __name__ == "__main__":
    unittest.main()
