#!/usr/bin/env python3
"""OpenCore profile generator unit tests; no firmware/EFI touched."""
from __future__ import annotations
import copy
import pathlib
import plistlib
import sys
import tempfile
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
from boot_profiles import (APPLE_BOOT_GUID, ProfileError, generate, make_profile,
                           has_whatevergreen)


def config(args="-v keepsyms=1 agdpmod=pikera navi33-fb=1 navi33-probe=1"):
    return {
        "ACPI": {"Add": [{"Enabled": True, "Path": "SSDT-EC.aml"}]},
        "DeviceProperties": {"Add": {"PciRoot(0x0)": {"model": "AMD Radeon RX 7600"}}},
        "Kernel": {"Add": [{"Enabled": True, "BundlePath": "WhateverGreen.kext"}],
                   "Patch": [{"Enabled": True, "Comment": "AMD Kernel patch"}]},
        "NVRAM": {
            "Add": {APPLE_BOOT_GUID: {"boot-args": args, "csr-active-config": b"\0\0\0\0"}},
            "Delete": {APPLE_BOOT_GUID: ["SystemAudioVolume"]},
        },
        "Misc": {"Boot": {"PickerMode": "External"}},
    }


class BootProfileTests(unittest.TestCase):
    def test_three_profiles_preserve_all_unrelated_config(self):
        original = config()
        frozen = copy.deepcopy(original)
        self.assertTrue(has_whatevergreen(original))
        for name in ("recovery", "vesa-baseline", "fb-trial"):
            with self.subTest(name=name):
                result = make_profile(original, name)
                tokens = result["NVRAM"]["Add"][APPLE_BOOT_GUID]["boot-args"].split()
                self.assertIn("-v", tokens)
                self.assertIn("keepsyms=1", tokens)
                self.assertIn("agdpmod=pikera", tokens)
                self.assertEqual(tokens.count("navi33-probe=1"), 0)
                self.assertNotIn("navi33-dma-preflight=1", tokens)
                self.assertEqual(tokens.count("navi33-dma-off=1"), 1)
                self.assertEqual(result["ACPI"], frozen["ACPI"])
                self.assertEqual(result["Kernel"], frozen["Kernel"])
                self.assertEqual(result["DeviceProperties"], frozen["DeviceProperties"])
                self.assertEqual(result["NVRAM"]["Add"][APPLE_BOOT_GUID]["csr-active-config"],
                                 frozen["NVRAM"]["Add"][APPLE_BOOT_GUID]["csr-active-config"])
                self.assertEqual(result["NVRAM"]["Delete"][APPLE_BOOT_GUID],
                                 ["SystemAudioVolume", "boot-args"])
                if name == "fb-trial":
                    self.assertIn("navi33-fb=1", tokens)
                    self.assertNotIn("navi33-fb-off=1", tokens)
                else:
                    self.assertNotIn("navi33-fb=1", tokens)
                    self.assertIn("navi33-fb-off=1", tokens)
                if name != "recovery":
                    self.assertEqual(tokens.count("-radvesa"), 1)
        self.assertEqual(original, frozen)

    def test_rescue_preserves_original_radvesa(self):
        original = config("-v -radvesa navi33-fb-off=0 navi33-fb=1")
        result = make_profile(original, "recovery")
        tokens = result["NVRAM"]["Add"][APPLE_BOOT_GUID]["boot-args"].split()
        self.assertEqual(tokens.count("-radvesa"), 1)
        self.assertEqual(tokens.count("navi33-fb-off=1"), 1)

    def test_fails_closed_on_incompatible_kext_injection(self):
        bad = config()
        bad["Kernel"]["Add"].append({"Enabled": True, "BundlePath": "Navi33GOPFB.kext"})
        with self.assertRaises(ProfileError):
            make_profile(bad, "fb-trial")

    def test_bad_structure_refused(self):
        for bad in ({"NVRAM": {}}, config('"not simple"'), config()):
            if bad.get("NVRAM") and isinstance(bad["NVRAM"]["Add"][APPLE_BOOT_GUID]["boot-args"], str) and \
                    '"' not in bad["NVRAM"]["Add"][APPLE_BOOT_GUID]["boot-args"]:
                bad["NVRAM"]["Delete"] = {APPLE_BOOT_GUID: "boot-args"}
            with self.assertRaises(ProfileError):
                make_profile(bad, "fb-trial")

    def test_generate_never_modifies_source_and_refuses_overwrite(self):
        with tempfile.TemporaryDirectory() as base:
            root = pathlib.Path(base)
            src = root / "config.plist"
            data = plistlib.dumps(config(), fmt=plistlib.FMT_BINARY)
            src.write_bytes(data)
            dest = root / "profiles"
            report = generate(src, dest)
            self.assertEqual(src.read_bytes(), data)
            self.assertEqual(len(report["profiles"]), 3)
            for name in ("recovery", "vesa-baseline", "fb-trial"):
                output = dest / (name + ".plist")
                self.assertTrue(output.is_file())
                parsed = plistlib.loads(output.read_bytes())
                self.assertEqual(parsed["Misc"], config()["Misc"])
            with self.assertRaises(ProfileError):
                generate(src, dest)
            with self.assertRaises(ProfileError):
                generate(src, root)


if __name__ == "__main__":
    unittest.main()
