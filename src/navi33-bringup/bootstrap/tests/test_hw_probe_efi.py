#!/usr/bin/env python3
from __future__ import annotations
import pathlib
import plistlib
import struct
import sys
import tempfile
import unittest
sys.path.insert(0,str(pathlib.Path(__file__).resolve().parents[1]))
from boot_profiles import APPLE_BOOT_GUID,ProfileError
from build_hw_probe_efi import build

def fixture(root):
    source=root/"original"/"EFI"
    (source/"BOOT").mkdir(parents=True)
    (source/"OC"/"Kexts").mkdir(parents=True)
    (source/"BOOT"/"BOOTx64.efi").write_bytes(b"mock")
    (source/"OC"/"OpenCore.efi").write_bytes(b"mock")
    config={"Kernel":{"Add":[{"Enabled":True,"BundlePath":"Lilu.kext"}],
                      "Patch":[{"Comment":"AMD kernel patch"}]},
            "NVRAM":{"Add":{APPLE_BOOT_GUID:{
                "boot-args":"-v keepsyms=1 navi33-hw-probe=0 navi33-dma-stage=1",
                "csr-active-config":b"\0\0\0\0"}}},
            "DeviceProperties":{"Add":{"PciRoot(0)":{"device-id":b"\x80\x74\0\0"}}}}
    (source/"OC"/"config.plist").write_bytes(plistlib.dumps(config))
    kext=root/"compiled"/"Navi33Probe.kext"
    (kext/"Contents"/"MacOS").mkdir(parents=True)
    info={"CFBundleVersion":"0.0.22",
          "CFBundleIdentifier":"com.cobalt64.navi33probe",
          "CFBundleExecutable":"Navi33Probe",
          "IOKitPersonalities":{"Navi33Probe":{
              "IOPCIPrimaryMatch":"0x74801002",
              "IOMatchCategory":"Navi33ReadOnlyProbe"}}}
    (kext/"Contents"/"Info.plist").write_bytes(plistlib.dumps(info))
    (kext/"Contents"/"MacOS"/"Navi33Probe").write_bytes(
        b"\xcf\xfa\xed\xfe"+struct.pack("<I",0x01000007)+b"\0"*80)
    return source,kext

class ProbeEFI(unittest.TestCase):
    def test_separate_recoverable_profiles(self):
        with tempfile.TemporaryDirectory() as td:
            root=pathlib.Path(td);source,kext=fixture(root)
            original=(source/"OC"/"config.plist").read_bytes()
            out=root/"packaged"
            report=build(source,kext,out)
            self.assertEqual(set(report["profiles"]),{"probe","recovery"})
            self.assertEqual((source/"OC"/"config.plist").read_bytes(),original)
            for profile in ("probe","recovery"):
                path=out/profile/"EFI"
                c=plistlib.loads((path/"OC"/"config.plist").read_bytes())
                boot=c["NVRAM"]["Add"][APPLE_BOOT_GUID]["boot-args"]
                self.assertIn("keepsyms=1",boot)
                self.assertNotIn("navi33-dma-stage=1",boot)
                self.assertIn("navi33-dma-stage-off=1",boot)
                self.assertIn("boot-args",c["NVRAM"]["Delete"][APPLE_BOOT_GUID])
                self.assertEqual(c["Kernel"]["Patch"][0]["Comment"],
                                 "AMD kernel patch")
                self.assertEqual(c["DeviceProperties"],
                                 plistlib.loads(original)["DeviceProperties"])
                injected=(path/"OC"/"Kexts"/"Navi33Probe.kext").exists()
                self.assertEqual(injected,profile=="probe")
                if profile=="probe":
                    self.assertIn("navi33-hw-probe=1",boot)
                    self.assertIn("navi33-client=1",boot)
                    self.assertEqual(c["Kernel"]["Add"][-1]["Arch"],"x86_64")
                    self.assertEqual(c["Kernel"]["Add"][-1]["BundlePath"],
                                     "Navi33Probe.kext")
                else:
                    self.assertIn("navi33-hw-off=1",boot)
                    self.assertIn("navi33-client-off=1",boot)
            with self.assertRaises(ProfileError):
                build(source,kext,out)
            with self.assertRaises(ProfileError):
                build(source,kext,source/"EFI"/"nested")
    def test_mismatch_symlink_and_wrong_binary_rejected(self):
        with tempfile.TemporaryDirectory() as td:
            root=pathlib.Path(td);source,kext=fixture(root)
            binary=kext/"Contents"/"MacOS"/"Navi33Probe"
            binary.write_bytes(b"\xcf\xfa\xed\xfe"+struct.pack("<I",0x0100000c))
            with self.assertRaises(ProfileError):
                build(source,kext,root/"out")
            self.assertFalse((root/"out").exists())
            binary.write_bytes(b"\xcf\xfa\xed\xfe"+struct.pack("<I",0x01000007))
            (source/"OC"/"unexpected").symlink_to(source/"OC"/"config.plist")
            with self.assertRaises(ProfileError):
                build(source,kext,root/"out")
            self.assertFalse((root/"out").exists())

if __name__=="__main__":
    unittest.main()
