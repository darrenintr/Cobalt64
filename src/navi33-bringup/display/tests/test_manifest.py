#!/usr/bin/env python3
"""Static fail-closed GPU display bootstrap gates; no hardware access."""
from pathlib import Path
import plistlib
root = Path(__file__).resolve().parents[1]
p=plistlib.loads((root/"Info.plist").read_bytes())
assert p["CFBundleIdentifier"]=="com.cobalt64.navi33gopfb"
assert p["CFBundleExecutable"]=="Navi33GOPFB"
personality=p["IOKitPersonalities"]
assert list(personality)==["Navi33GOPFB"]
pp=personality["Navi33GOPFB"]
assert pp["IOPCIPrimaryMatch"]=="0x74801002"
assert pp["IOMatchCategory"]=="IOFramebuffer"
assert p["OSBundleLibraries"]["com.apple.iokit.IOGraphicsFamily"]=="1.0"
source=(root/"src/Navi33GOPFB.cpp").read_text()
for required in (
    'PE_parse_boot_argn("navi33-fb"', 'PE_parse_boot_argn("navi33-fb-off"',
    "getConsoleInfo(&video)", "IODeviceMemory::withRange",
    "getDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0)",
    "checkBar0(d, barBase, barSize)",
    "isConsoleDevice()", "inspectGop(", "configRead16(0x04u)"
):
    assert required in source, required
for forbidden in (
    "configWrite", "setMemoryEnable", "setBusMasterEnable", "mapDeviceMemory",
    "regWrite", "WREG32", "MM_INDEX", "LOAD_IP_FW", "setPixelClock",
    "LoadAccelerator", "IOConnectCallMethod"
):
    assert forbidden not in source, forbidden
bar_guard = (root/"src/Navi33BarGuard.h").read_text()
assert "checkBar0" in bar_guard and "layout.length > barLength - offset" in bar_guard
print("Navi33 GOP framebuffer manifest / BAR0 ownership / no-MMIO-write checks: PASS")
