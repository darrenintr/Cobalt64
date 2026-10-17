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
assert "Navi33Probe::hardwarePreflight" in source
assert '"navi33-hw-probe"' in source and '"navi33-hw-off"' in source
assert "navi33::hwprobe::planHubs" in source
assert '"navi33-client"' in source and '"navi33-client-off"' in source
assert 'N33_CAP_ROM_DISCOVERY' in source
client=(root/"src/Navi33NativeClient.cpp").read_text()
assert "Navi33NativeClient::externalMethod" in client
assert "Navi33NativeClient::clientClose" in client
# A DMA client MUST return the provider-scoped pinned-memory lease BEFORE
# losing the provider pointer; otherwise the next root process stays BUSY.
for method, ending in (("Navi33NativeClient::stop", "IOReturn Navi33NativeClient::clientClose"),
                       ("Navi33NativeClient::clientClose", "IOReturn Navi33NativeClient::clientDied")):
    segment=client[client.index(method):client.index(ending)]
    assert segment.index("releaseStageLocked();") < segment.index("owner_ = nullptr;"), method
assert 'Navi33Probe::vramGeometryStatus' in source
assert 'navi33::nbio4::fromDiscovery' in source
assert 'navi33::nbio4::inspect' in source
assert 'navi33::vram_survey::combine' in source
assert 'N33_SEL_VRAM_GEOMETRY' in client

assert "N33_SEL_PSP_STATUS" in client
assert "N33_SEL_SDMA_STATUS" in client
assert "N33_SEL_HW_PREFLIGHT" in client
assert "owner_->hardwarePreflight(rom,romLen)" in client
assert "N33_SEL_HW_ROM_PREFLIGHT" in client
assert "structureInputDescriptor->readBytes(" in client
assert "IOFree(copied,romLen)" in client
assert "Navi33Probe::hardwarePreflight" in source
assert "N33_SEL_DMA_STAGE" in client
assert "N33_SEL_DMA_STATUS" in client
assert "N33_SEL_DMA_RELEASE" in client
assert '"navi33-dma-stage"' in client
assert '"navi33-dma-stage-off"' in client
assert "dmaStage_.release()" in client
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
                 "Navi33Sdma6Status.h", "Navi33Sdma6Staging.h"):
    code = (root / "src" / filename).read_text()
    for forbidden in ("<IOKit/", "->configWrite", "WREG32(", "RREG32(",
                      "mapDeviceMemory", "setMemoryEnable", "IOConnectCallMethod("):
        assert forbidden not in code, f"{filename} gained hardware I/O: {forbidden}"
adapter_code=(root/"src/Navi33Sdma6BarAdapter.h").read_text()
assert "sdma6_mmio::kRbCntl" in adapter_code
assert "#ifndef COBALT_NAVI33_ENABLE_UNSAFE_MMIO" in adapter_code
assert "#include \"Navi33Sdma6BarAdapter.h\"" not in source
assert "#include \"Navi33Sdma6BarAdapter.h\"" not in client
queue_code=(root/"src/Navi33Sdma6MmioQueue.h").read_text()
assert "#ifdef COBALT_NAVI33_ENABLE_UNSAFE_MMIO" in queue_code
assert "kDeviceSubmissionImplemented=false" in (root/"src/Navi33Sdma6HardwareGate.h").read_text()
mmhub=(root/"src/Navi33Gmc11Mmhub.h").read_text()
assert "#ifdef COBALT_NAVI33_ENABLE_UNSAFE_GMC11_MMIO" in mmhub
assert "planFromDiscovery" in mmhub and "InvalidateAck" in mmhub
assert '#include "Navi33Gmc11Mmhub.h"' not in source
assert '#include "Navi33Gmc11Mmhub.h"' not in client
assert "kGpuRingSubmissionEnabled=false" in (root/"src/Navi33Sdma6Staging.h").read_text()
assert "#include \"Navi33Sdma6MmioQueue.h\"" not in source
assert "#include \"Navi33Sdma6MmioQueue.h\"" not in client
assert "N33_CAP_HW_SDMA" not in client
# BAR0 write prototype must NEVER be linked into the live diagnostic Kext.
for caller in (source,client):
    assert '#include "Navi33Gmc11VramUpload.h"' not in caller
assert "N33_SEL_VRAM_GEOMETRY" in client
assert "N33_SEL_QUEUE_CENSUS" in client
assert "owner_->queueCensus()" in client
assert 'Navi33Probe::queueCensus' in source
assert 'navi33::queue_census::readStable' in source
assert '#include "Navi33Sdma6QueueCensus.h"' in source
assert "Navi33Sdma6QueueCensus.h" in (root / "Makefile").read_text()
assert "N33_SEL_QUEUE_CENSUS" in (root / "tools/n33compute.c").read_text()
assert "--queue" in (root / "tools/n33compute.c").read_text()
census=(root / "src/Navi33Sdma6QueueCensus.h").read_text()
for forbidden in ("->configWrite", "WREG32(", "->setBusMasterEnable",
                  "mapDeviceMemory", "->setMemoryEnable", "setPowerState(",
                  "IOConnectCallMethod(", "writeMmio", "writeRegister"):
    assert forbidden not in census, f"Queue0 census gained GPU write: {forbidden}"
assert "N33_QUEUE_FLAG_APPARENTLY_PARKED" in census
assert "N33_QUEUE_UNSTABLE" in census
assert "kCanWriteRegisters=false" in census
# Hardware writer MUST remain isolated from both production entrypoints.
for user_code in (source,client):
    assert '#include "Navi33Sdma6MmioQueue.h"' not in user_code
    assert '#include "Navi33Sdma6BarAdapter.h"' not in user_code

assert '"navi33-vram-read"' in source and '"navi33-vram-off"' in source
assert "N33_CAP_ACCELERATION" not in source

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

stager = (root/"src/Navi33DarwinStage.cpp").read_text()
assert "IOMapper::copyMapperForDevice(pci)" in stager
assert "IODMACommand::withSpecification(" in stager
assert "gen64IOVMSegments(" in stager
assert "command_->complete()" in stager
assert "clearMemoryDescriptor(false)" in stager
assert "memory_->complete()" in stager
assert "staging::prepare(" in stager
for forbidden in ("->configWrite", "->setMemoryEnable",
                  "->setBusMasterEnable", "WREG32(",
                  "IOConnectCallMethod(", "setPowerState("):
    assert forbidden not in stager, f"DMA stager gained GPU I/O: {forbidden}"
assert "kGpuRingSubmissionEnabled=false" in (root/"src/Navi33Sdma6Staging.h").read_text()
assert "kGartHardwareEnabled=false" in (root/"src/Navi33Sdma6Staging.h").read_text()
# These helper headers may only compute/read a supplied MMIO snapshot;
# NEVER introduce register writes, VBIOS flashing, PCI takeover or DMA.
for filename in ("Navi33Nbio43Capacity.h","Navi33VramSurvey.h"):
    code=(root/"src"/filename).read_text()
    for forbidden in ("->configWrite", "WREG32(", "->setBusMasterEnable",
                      "mapDeviceMemory", "->setMemoryEnable", "setPowerState(",
                      "IOConnectCallMethod("):
        assert forbidden not in code, f"{filename} gained GPU IO: {forbidden}"
print("Navi33 device IOMMU staging and GPU submission boundaries: static checks passed")
