#!/usr/bin/env python3
"""Manifest and read-only-source regression checks; no hardware access."""
import pathlib
import plistlib

root = pathlib.Path(__file__).resolve().parents[1]
plist = plistlib.loads((root / "Info.plist").read_bytes())
assert plist["CFBundleIdentifier"] == "com.cobalt64.navi33probe"
assert plist["CFBundleExecutable"] == "Navi33Probe"
personalities = plist["IOKitPersonalities"]
assert len(personalities) == 1
p = personalities["Navi33Probe"]
assert p["IOPCIPrimaryMatch"] == "0x74801002"
assert p["IOMatchCategory"] == "Navi33ReadOnlyProbe"
assert p["IOClass"] == "Navi33Probe"
assert p["IOProviderClass"] == "IOPCIDevice"
source = (root / "src" / "Navi33Probe.cpp").read_text()
assert 'PE_parse_boot_argn("navi33-probe"' in source
for forbidden in (
    "->configWrite", "->setMemoryEnable", "->setBusMasterEnable",
    "->mapDeviceMemory", "->setDeviceMemory", "WREG32(",
    "IOConnectCallMethod(", "LoadAccelerator",
):
    assert forbidden not in source, f"read-only source gained {forbidden}"
assert "pci->configRead32" in source
assert "getDeviceMemoryWithRegister" in source
assert '"ATY,bin_image"' in source
assert 'Navi33Discovery.h' in source
assert 'Navi33HardwarePlan.h' in source
assert 'Navi33Dcn321.h' in source
assert 'Navi33FirmwareGuard.h' in source
assert 'Navi33,DCNReadPlanStatus' in source
assert 'Navi33,GpuQueuesImplemented' in source
parser = (root / "src" / "Navi33Discovery.h").read_text()
assert "struct IPEntry" in parser and "isNavi33Candidate" in parser
for forbidden in ("<IOKit/", "WREG32", "configWrite", "mapDeviceMemory"):
    assert forbidden not in parser, f"IP parser gained {forbidden}"
for filename in ("Navi33HardwarePlan.h", "Navi33Dcn321.h", "Navi33FirmwareGuard.h"):
    code = (root / "src" / filename).read_text()
    for forbidden in ("<IOKit/", "->configWrite", "WREG32(", "RREG32(",
                      "mapDeviceMemory", "setMemoryEnable", "IOConnectCallMethod("):
        assert forbidden not in code, f"{filename} gained hardware I/O: {forbidden}"
print("Navi33 metadata-only hardware/firmware/DCN plans: static checks passed")
