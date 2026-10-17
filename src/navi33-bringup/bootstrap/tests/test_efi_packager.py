#!/usr/bin/env python3
"""Offline boot-profile packager: all file writes occur in temp folders."""
from __future__ import annotations
import copy
import pathlib
import plistlib
import sys
import tempfile
import unittest
sys.path.insert(0,str(pathlib.Path(__file__).resolve().parents[1]))
from build_test_efis import pack,ProfileError,PROFILE_NAMES
from boot_profiles import APPLE_BOOT_GUID

def efi(root:pathlib.Path,include_weg:bool=True):
    source=root/"source"/"EFI"
    (source/"BOOT").mkdir(parents=True)
    (source/"OC").mkdir()
    (source/"OC"/"Kexts").mkdir()
    (source/"BOOT"/"BOOTx64.efi").write_bytes(b"MZboot-mock")
    (source/"OC"/"OpenCore.efi").write_bytes(b"MZopencore-mock")
    conf={"Kernel":{"Add":[
        {"BundlePath":"Lilu.kext","Enabled":True},
        {"BundlePath":"WhateverGreen.kext","Enabled":include_weg},
        {"BundlePath":"VirtualSMC.kext","Enabled":True}],
        "Patch":[{"Enabled":True,"Comment":"AMD kernel patch"}]},
        "NVRAM":{"Add":{APPLE_BOOT_GUID:{
            "boot-args":"-v keepsyms=1 -radvesa navi33-fb=1 navi33-dma-preflight=1",
            "csr-active-config":b"\0\0\0\0"}}},
        "DeviceProperties":{"Add":{"PciRoot(0)":{"device-id":b"\x80\x74\0\0"}}}}
    (source/"OC"/"config.plist").write_bytes(plistlib.dumps(conf))
    return source,conf

class PackTests(unittest.TestCase):
    def test_independent_outputs_without_overwrite(self):
        with tempfile.TemporaryDirectory() as name:
            root=pathlib.Path(name)
            source,conf=efi(root)
            original=(source/"OC"/"config.plist").read_bytes()
            out=root/"profiles"
            manifest=pack(source,out)
            self.assertEqual((source/"OC"/"config.plist").read_bytes(),original)
            self.assertEqual(set(manifest["profiles"]),set(PROFILE_NAMES))
            for profile in PROFILE_NAMES:
                copied=out/profile/"EFI"
                self.assertEqual((copied/"BOOT"/"BOOTx64.efi").read_bytes(),b"MZboot-mock")
                value=plistlib.loads((copied/"OC"/"config.plist").read_bytes())
                args=value["NVRAM"]["Add"][APPLE_BOOT_GUID]["boot-args"]
                self.assertNotIn("navi33-dma-preflight=1",args)
                self.assertIn("navi33-dma-off=1",args)
                self.assertEqual(value["Kernel"]["Patch"],conf["Kernel"]["Patch"])
                self.assertEqual(value["DeviceProperties"],conf["DeviceProperties"])
                if profile.startswith("tahoe-"):
                    self.assertFalse(value["Kernel"]["Add"][1]["Enabled"])
                    self.assertNotIn("-radvesa",args)
                if profile=="tahoe-firmware":
                    self.assertIn("navi33-fb-off=1",args)
                if profile=="tahoe-fb-trial":
                    self.assertIn("navi33-fb=1",args)
            with self.assertRaises(ProfileError):
                pack(source,out)
            with self.assertRaises(ProfileError):
                pack(source,source/"nested")
    def test_symlink_and_missing_executable_rejected(self):
        with tempfile.TemporaryDirectory() as name:
            root=pathlib.Path(name)
            source,_=efi(root)
            (source/"OC"/"OpenCore.efi").unlink()
            with self.assertRaises(ProfileError):
                pack(source,root/"out")
            (source/"OC"/"OpenCore.efi").write_bytes(b"MZ")
            (source/"OC"/"Kexts"/"bad").symlink_to(source/"OC"/"config.plist")
            with self.assertRaises(ProfileError):
                pack(source,root/"out")
            self.assertFalse((root/"out").exists())
if __name__=="__main__":
    unittest.main()
