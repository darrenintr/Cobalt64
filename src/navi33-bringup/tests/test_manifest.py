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
assert "Navi33Probe::newUserClient" in source
assert '"navi33-client"' in source and '"navi33-client-off"' in source
assert 'N33_CAP_ROM_DISCOVERY' in source
client=(root/"src/Navi33NativeClient.cpp").read_text()
assert "Navi33NativeClient::externalMethod" in client
assert "Navi33NativeClient::clientClose" in client
assert "N33_SEL_PSP_STATUS" in client
assert "N33_SEL_SDMA_STATUS" in client
assert "owner_->sdmaStatus()" in client
assert "owner_->pspStatus()" in client
assert "clientHasPrivilege" in client
assert "navi33::native_policy::validate" in client
assert "kIOReturnUnsupported" in client
for forbidden in ("WREG32(", "->configWrite", "->setMemoryEnable",
                  "->setBusMasterEnable", "mapDeviceMemory", "IOConnectCallMethod("):
    assert forbidden not in client, f"N33N client gained forbidden GPU access: {forbidden}"
assert "pci->configRead32" in source
assert "getDeviceMemoryWithRegister" in source
assert '"ATY,bin_image"' in source
assert 'Navi33Discovery.h' in source
assert 'Navi33HardwarePlan.h' in source
assert 'Navi33Dcn321.h' in source
assert 'Navi33FirmwareGuard.h' in source
assert 'Navi33,DCNReadPlanStatus' in source
assert 'PE_parse_boot_argn("navi33-psp-read"' in source
assert 'PE_parse_boot_argn("navi33-psp-off"' in source
assert "navi33::psp13::buildPlan" in source
assert 'PE_parse_boot_argn("navi33-sdma-read"' in source
assert 'PE_parse_boot_argn("navi33-sdma-off"' in source
assert "navi33::sdma6live::buildPlan" in source
assert 'Navi33,GpuQueuesImplemented' in source
assert 'Navi33,Gmc11GartHardwareReady' in source
assert 'Navi33,SDMA6HardwareRingReady' in source
assert 'Navi33,TLBInvalidationEnabled' in source
assert 'Navi33,SDMA6DoorbellEnabled' in source
assert 'Navi33,DmaPreflightExecuted' in source
assert 'PE_parse_boot_argn("navi33-dma-preflight"' in source
assert 'PE_parse_boot_argn("navi33-dma-off"' in source
assert 'navi33::darwin_dma::runOnce(pci)' in source
parser = (root / "src" / "Navi33Discovery.h").read_text()
assert "struct IPEntry" in parser and "isNavi33Candidate" in parser
for forbidden in ("<IOKit/", "WREG32", "configWrite", "mapDeviceMemory"):
    assert forbidden not in parser, f"IP parser gained {forbidden}"
for filename in ("Navi33HardwarePlan.h", "Navi33Dcn321.h", "Navi33FirmwareGuard.h", "Navi33Sdma6Packet.h", "Navi33Gmc11Gart.h",
                 "Navi33GpuVaArena.h", "Navi33Sdma6Ring.h", "Navi33DmaScatter.h",
                 "Navi33Psp13Status.h", "Navi33Psp13Firmware.h",
                 "Navi33Sdma6Status.h"):
    code = (root / "src" / filename).read_text()
    for forbidden in ("<IOKit/", "->configWrite", "WREG32(", "RREG32(",
                      "mapDeviceMemory", "setMemoryEnable", "IOConnectCallMethod("):
        assert forbidden not in code, f"{filename} gained hardware I/O: {forbidden}"
assert "kCanProgramGfxhub = false" in (root/"src/Navi33Gmc11Gart.h").read_text()
assert "kCanSubmitToHardware = false" in (root/"src/Navi33Sdma6Ring.h").read_text()
assert "kCanRingDoorbell = false" in (root/"src/Navi33Sdma6Ring.h").read_text()
kernel_dma=(root/"src/Navi33DarwinDma.cpp").read_text()
assert "IOMapper::copyMapperForDevice(pci)" in kernel_dma
assert "IODMACommand::withSpecification(" in kernel_dma
assert "gen64IOVMSegments(" in kernel_dma
assert "command->complete()" in kernel_dma
assert "clearMemoryDescriptor(false)" in kernel_dma
assert "memory->complete()" in kernel_dma
assert "mapIntoShadow(" in kernel_dma
for forbidden in ("configWrite", "setBusMasterEnable", "setMemoryEnable",
                  "mapDeviceMemory", "WREG32", "RREG32", "setPowerState",
                  "mmio", "doorbell"):
    # References in source comments are permitted; detect executable patterns
    if forbidden=="configWrite":
        assert "->configWrite" not in kernel_dma
    elif forbidden=="setBusMasterEnable":
        assert "->setBusMasterEnable" not in kernel_dma
    elif forbidden=="setMemoryEnable":
        assert "->setMemoryEnable" not in kernel_dma
    elif forbidden in ("WREG32","RREG32"):
        assert forbidden+"(" not in kernel_dma
print("Navi33 DMA mapping preflight and GPU access boundaries: static checks passed")
