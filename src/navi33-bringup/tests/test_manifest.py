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
# Real IOKit passes non-null scalar arrays for zero counts; pointer checks
# rejected every struct selector with kIOReturnBadArgument on hardware.
assert "args->scalarInput != nullptr" not in client
assert "args->scalarOutput != nullptr" not in client
assert "args->scalarInputCount != 0u, args->scalarOutputCount != 0u" in client
# Out-of-line input is wired for the copy and unwired after it.
assert "copyDescriptor(args->structureInputDescriptor," in client
copy=client[client.index("static bool copyDescriptor"):]
copy=copy[:copy.index("\n}\n")]
assert copy.index("md->prepare(kIODirectionOut)") < copy.index("md->readBytes(0,dst,len)") \
    < copy.index("md->complete(kIODirectionOut)")
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
assert "navi33::psp13::readBootloader(" in source
assert '"Navi33,LastHwPreflight"' in source
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
                 "Navi33DmaBackend.h", "Navi33DmaEngine.h",
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
# MMIO peek: allowlisted reads only, same gate as the hardware preflight.
assert "N33_SEL_MMIO_PEEK" in client
assert "owner_->peekRegisters(" in client
assert "Navi33Probe::peekRegisters" in source
assert "navi33::peek::plan(disc,window,first," in source
peek=(root / "src/Navi33MmioPeek.h").read_text()
for forbidden in ("->configWrite", "WREG32(", "writeMmio", "writeRegister",
                  "volatile uint32_t *)", "= value", "*out=0"):
    assert forbidden not in peek, f"MMIO peek gained a write: {forbidden}"
windows=(root / "src/Navi33PeekWindows.h").read_text()
for hazard in ("_SEM", "_ACK", "SCRATCH_RAM", "INT_STATUS", "EDC_COUNTER"):
    assert hazard not in windows.split("*/",1)[1], f"hazard window: {hazard}"
assert "N33_SEL_MMIO_PEEK" in (root / "tools/n33peek.c").read_text()
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
# The real BAR5 Queue1 and BAR0 root-read adapters deliberately compile
# as separate macOS objects only. Neither is linked into the live Kext.
queue_bridge=(root/"src/Navi33DarwinQueue1IO.cpp").read_text()
root_bridge=(root/"src/Navi33DarwinRootReadback.cpp").read_text()
stage_header=(root/"src/Navi33DarwinStage.h").read_text()
makefile=(root/"Makefile").read_text()
for required in ("Navi33DarwinQueue1IO.o", "Navi33DarwinRootReadback.o",
                 "physical-io-check"):
    assert required in makefile
assert "Navi33DarwinQueue1IO.o $(BUILD)/Navi33DarwinRootReadback.o" in makefile
for required in ("IOPCIDevice", "getDeviceMemoryWithRegister",
                 "kIOPCIConfigBaseAddress5", "sdma6_bar::buildPlan",
                 "stage_->synchronizeForDevice()",
                 "stage_->synchronizeForHost()", "stage_->readByte"):
    assert required in queue_bridge, required
assert "denyQueueRegisterWrite" in queue_bridge
for forbidden in ("configWrite", "setBusMasterEnable", "setMemoryEnable",
                  "writeBar5", "WREG32(", "->setPowerState("):
    assert forbidden not in queue_bridge
for required in ("kIOPCIConfigBaseAddress0", "readWord", "readPhysical",
                 "rootPhysical_", "rootBarOffset_", "vram_geometry::locate"):
    assert required in root_bridge
assert "void RootReadback::write" not in root_bridge
assert "kBar0WriteEnabled=false" in (root/"src/Navi33DarwinRootReadback.h").read_text()
assert 'Navi33DmaLifetime.h' in stage_header
lifetime_policy=(root/"src/Navi33DmaLifetime.h").read_text()
stage_cpp=(root/"src/Navi33DarwinStage.cpp").read_text()
assert 'quarantineBeforeFirstMmioAttempt' in lifetime_policy
assert 'requiresExternalHardwareReset' in lifetime_policy
assert 'lifetime_.quarantineBeforeFirstMmioAttempt()' in stage_cpp
assert 'if(!lifetime_.mayCompleteIommuMapping())' in stage_cpp
assert 'if(dmaStage_.requiresExternalReset())' in client
assert 'return state(N33_DMA_BUSY);' in stage_cpp
assert 'test-dma-lifetime' in makefile
assert "bool pinnedAndMapped() const" in stage_header
assert "bool synchronizeForHost()" in stage_header
assert "bool readByte(size_t offset,uint8_t *out) const" in stage_header
for forbidden_include in ('#include "Navi33DarwinQueue1IO.h"',
                          '#include "Navi33DarwinRootReadback.h"'):
    assert forbidden_include not in source
    assert forbidden_include not in client
bridge_workflow=(root.parents[1]/".github/workflows/navi33-host.yml").read_text()
assert "physical-io-check" in bridge_workflow
assert "Navi33DarwinRootReadback.o" in bridge_workflow
# Do not allow low-level BAR/IOVA callbacks to advertise hw acceleration.
assert "N33_CAP_HW_SDMA" not in queue_bridge
assert "N33_CAP_METAL" not in root_bridge
# PSP13 sOS load: the ONLY GPU writer (approved 2026-10-18). It is linked,
# gated by navi33-psp-load / navi33-psp-off, one attempt per boot, and may
# write only C2PMSG_35/36 and the 1 MiB fw_pri sub-range of BAR0.
assert "N33_SEL_PSP_LOAD" in client and "owner_->pspLoad(" in client
assert "Navi33Probe::pspLoad" in source
load_fn=source[source.index("Navi33Probe::pspLoad"):]
load_fn=load_fn[:load_fn.index("\n}\n")]
assert 'PE_parse_boot_argn("navi33-hw-probe"' in load_fn
assert 'PE_parse_boot_argn("navi33-psp-load"' in load_fn
assert 'PE_parse_boot_argn("navi33-psp-off"' in load_fn
# The sOS load keeps its own one-shot flag; only the ring modes use the other.
assert "req.attempted=ringMode ? &pspRingAttempted_ : fwMode ? &pspFwAttempted_ :" in load_fn
assert load_fn.count("&pspLoadAttempted_")==1
assert "IOLockLock(pspLock_)" in load_fn
assert "navi33::darwin_psp::execute(pci,req)" in load_fn
# getConsoleInfo() returned a kernel virtual address on hardware (0.0.6);
# the console must come from boot_args through the physical-only filter.
# pspLoad() and the autostart share fillConsole() (kext 0.0.25).
console_fn=source[source.index("static void fillConsole("):]
console_fn=console_fn[:console_fn.index("\n}\n")]
assert "fillConsole(req);" in load_fn
assert "PE_state.bootArgs" in console_fn and "psp13load::bootConsole(" in console_fn
assert "getConsoleInfo" not in load_fn and "getConsoleInfo" not in console_fn
# A guest without a GOP (Tahoe VM, 2026-10-18) may name the host's view of
# the firmware console, but both boot-args are needed and a boot console wins.
assert 'PE_parse_boot_argn("navi33-hostfb-offset"' in console_fn
assert 'PE_parse_boot_argn("navi33-hostfb-bytes"' in console_fn
writer=(root/"src/Navi33DarwinPspLoad.cpp").read_text()
assert "const pl::Range console=boot.phys ? boot :" in writer
assert "pl::hostConsole(boot,req.hostConsole," in writer
assert "pl::plan(snap,console,bar0Range)" in writer
assert "req.consolePhys,req.consoleBytes},bar0Range" not in writer
assert all(o in makefile.splitlines()[[i for i,l in enumerate(makefile.splitlines()) if l.rstrip().endswith("-o $(EXEC)")][0]] for o in ("$(BUILD)/Navi33DarwinPciOwner.o",))
# PCI owner probe: default off, gated, and read-only (config reads and one open/close).
owner=(root/"src/Navi33DarwinPciOwner.cpp").read_text()
for forbidden in ("configWrite", "WREG32(", "setBusMasterEnable", "setMemoryEnable",
                  "mapDeviceMemory", "RREG32(", "writeMmio"):
    assert forbidden not in owner, f"PCI owner probe gained {forbidden}"
assert 'PE_parse_boot_argn("navi33-owner-probe"' in source
assert 'PE_parse_boot_argn("navi33-owner-off"' in source
assert 'PE_parse_boot_argn("navi33-owner-recovery"' in source
assert 'PE_parse_boot_argn("navi33-owner-hold"' in source
assert 'releaseOwner(' in source, 'stop() must release a held owner'
assert 'Navi33Probe::systemWillShutdown' in source, 'shutdown must release a held owner'
assert 'Navi33Probe::willTerminate' in source, 'termination must release a held owner'
# The owner verdict must survive a rolled-over kernel log: ioreg properties.
assert '"Navi33,OwnerProbe"' in source and '"Navi33,OwnerEvidence"' in source
runsh=(root/"usb"/"run.sh").read_text()
assert 'Navi33,(Owner|DmaPreflight)' in runsh, 'run.sh must print the owner and DMA verdicts'
for forbidden in ("n33psp --load", "--i-have-ssh", "--ring ", "--ip-fw ", "--smu-"):
    assert forbidden not in runsh, f"run.sh must stay read-only: {forbidden}"
wfn=writer[writer.index("bool write32("):]
wfn=wfn[:wfn.index("\n}\n")]
assert "off!=io->allowed[0] && off!=io->allowed[1]" in wfn
assert "io.allowed[0]=regs.c2p35;io.allowed[1]=regs.c2p36;" in writer
assert "IODeviceMemory::withSubRange(bar0,plan.fwPriOffset," in writer
assert "bar0->map(" not in writer, "the whole of BAR0 must never be mapped"
order=[writer.index(s) for s in ("if(plan.state!=N33_PSPL_PLAN_OK || req.mode==N33_PSP_LOAD_PLAN)",
                                 "if(!req.writeGate)", "if(*req.attempted)",
                                 "withSubRange(", "*req.attempted=true;",
                                 "pl::run(plan,regs,fw,hw)")]
assert order==sorted(order), "PSP load gates are out of order"
for forbidden in ("->configWrite", "->setMemoryEnable", "->setBusMasterEnable",
                  "mapDeviceMemory", "IOConnectCallMethod(", "setPowerState(",
                  "WREG32("):
    assert forbidden not in writer, f"PSP writer gained {forbidden}"
loader=(root/"src/Navi33Psp13Loader.h").read_text()
assert "{N33_PSPL_STEP_SPL,&fw.kdb,kLoadSpl}" in loader  # upstream quirk kept
assert "kWritableRegisters[2]={kC2pmsg36,kC2pmsg35}" in loader
# PSP13 KM ring stage (kext 0.0.9+): its own boot-arg, its own one-shot flag and
# its own writer. Not approved to run until the user says so (AGENTS.md).
assert 'PE_parse_boot_argn("navi33-psp-ring"' in load_fn
assert "req.attempted=ringMode ? &pspRingAttempted_ : fwMode ? &pspFwAttempted_ :" in load_fn
assert "const bool killed=PE_parse_boot_argn(\"navi33-psp-off\"" in load_fn
assert "!killed && (ringMode ?" in load_fn     # psp-off kills both stages
assert "bool pspRingAttempted_ = false;" in (root/"src/Navi33Probe.hpp").read_text()
ring=(root/"src/Navi33DarwinPspRing.cpp").read_text()
assert "return executeRing(pci,req);" in writer
link_line=[l for l in makefile.splitlines() if l.rstrip().endswith("-o $(EXEC)")][0]
assert all(o in link_line for o in ("$(BUILD)/Navi33DarwinPspRing.o","$(BUILD)/Navi33DarwinPspFw.o","$(BUILD)/kmod_info.o"))
rwfn=ring[ring.index("bool write32("):]
rwfn=rwfn[:rwfn.index("\n}\n")]
assert "ok=ok || off==a" in rwfn and "uint64_t allowed[5]{}" in ring
assert ("io.allowed[0]=rregs.c64;io.allowed[1]=rregs.c67;io.allowed[2]=rregs.c69;"
        "\n    io.allowed[3]=rregs.c70;io.allowed[4]=rregs.c71;") in ring
assert "pl::pinned(req.fw,req.fwBytes)" in ring
assert "IODeviceMemory::withSubRange(bar0,plan.win.fwPriOffset," in ring
assert "bar0->map(" not in ring, "the whole of BAR0 must never be mapped"
rorder=[ring.index(x) for x in (
    "if(plan.state!=N33_PSPL_PLAN_OK || req.mode==N33_PSP_LOAD_RING_PLAN)",
    "if(!req.writeGate)", "if(*req.attempted)", "withSubRange(",
    "*req.attempted=true;", "rg::run(plan,rregs,fw.toc,hw)")]
assert rorder==sorted(rorder), "PSP ring gates are out of order"
for forbidden in ("->configWrite", "->setMemoryEnable", "->setBusMasterEnable",
                  "mapDeviceMemory", "IOConnectCallMethod(", "setPowerState(",
                  "WREG32("):
    assert forbidden not in ring, f"PSP ring writer gained {forbidden}"
ringh=(root/"src/Navi33Psp13Ring.h").read_text()
assert "kWritableRegisters[5]={kC2pmsg64,kC2pmsg67,kC2pmsg69," in ringh
assert "tmrValid" in ringh and "kTmrVramOffset=8128ull<<20u" in ringh
for forbidden in ("<IOKit/", "->configWrite", "WREG32(", "RREG32(",
                  "mapDeviceMemory", "setMemoryEnable", "IOConnectCallMethod("):
    assert forbidden not in ringh, f"Navi33Psp13Ring.h gained hardware I/O: {forbidden}"
for filename in ("Navi33Psp13Loader.h", "Navi33Sha256.h"):
    code=(root/"src"/filename).read_text()
    for forbidden in ("<IOKit/", "->configWrite", "WREG32(", "RREG32(",
                      "mapDeviceMemory", "setMemoryEnable", "IOConnectCallMethod("):
        assert forbidden not in code, f"{filename} gained hardware I/O: {forbidden}"
# The IP firmware record builder is pure: no I/O, no hardware, nothing wired in yet.
ipfw=(root/"src/Navi33Psp13IpFw.h").read_text()
for forbidden in ("<IOKit/", "->configWrite", "WREG32(", "RREG32(", "mapDeviceMemory",
                  "setMemoryEnable", "IOConnectCallMethod(", "IOLog(", "write32"):
    assert forbidden not in ipfw, f"Navi33Psp13IpFw.h gained hardware I/O: {forbidden}"
assert ipfw.count("kTypeSmu")>=2 and "File::Smu" in ipfw            # the SMU is record 26, last in the list
assert "Navi33Psp13IpFw.h" not in source and "Navi33Psp13IpFw.h" not in client
# PSP13 IP firmware stage (kext 0.0.10+): its own boot-arg, its own one-shot flag,
# a second boot-arg for AUTOLOAD_RLC, its own writer with a one-register allowlist.
assert 'PE_parse_boot_argn("navi33-psp-fw"' in load_fn
assert 'PE_parse_boot_argn("navi33-psp-autoload"' in load_fn
assert "req.attempted=ringMode ? &pspRingAttempted_ : fwMode ? &pspFwAttempted_ :" in load_fn
assert "req.autoloadGate=!killed && fwMode &&" in load_fn      # psp-off kills autoload too
assert "bool pspFwAttempted_ = false;" in (root/"src/Navi33Probe.hpp").read_text()
assert "return executeFw(pci,req);" in writer
assert all(o in link_line for o in ("$(BUILD)/Navi33DarwinPspFw.o","$(BUILD)/kmod_info.o"))
fwc=(root/"src/Navi33DarwinPspFw.cpp").read_text()
fwfn=fwc[fwc.index("bool write32("):]
fwfn=fwfn[:fwfn.index("\n}\n")]
assert "off!=io->wptr" in fwfn and "io.wptr=rregs.c67;" in fwc   # the ring write pointer only
assert "uint64_t allowed" not in fwc
# the call must use the default requirePins=true (no 7th argument)
assert "fl::plan(snap,console,bar0Range,in,req.startAt,req.stopAfter);" in fwc
assert "requirePins=true" in (root/"src/Navi33Psp13FwLoad.h").read_text()
assert "if(!fw::allPinned(in))return out;" in fwc
assert "IODeviceMemory::withSubRange(bar0,plan.ring.win.fwPriOffset," in fwc
assert "bar0->map(" not in fwc, "the whole of BAR0 must never be mapped"
forder=[fwc.index(x) for x in (
    "if(plan.state!=N33_PSPL_PLAN_OK || req.mode==N33_PSP_LOAD_FW_PLAN)",
    "if(!req.writeGate)", "if(plan.autoload && !req.autoloadGate)",
    "if(*req.attempted)", "withSubRange(", "*req.attempted=true;",
    "fl::run(plan,rregs,in,hw)")]
assert forder==sorted(forder), "PSP firmware gates are out of order"
for forbidden in ("->configWrite", "->setMemoryEnable", "->setBusMasterEnable",
                  "mapDeviceMemory", "IOConnectCallMethod(", "setPowerState(", "WREG32("):
    assert forbidden not in fwc, f"PSP firmware writer gained {forbidden}"
fwl=(root/"src/Navi33Psp13FwLoad.h").read_text()
for forbidden in ("<IOKit/", "->configWrite", "WREG32(", "RREG32(", "mapDeviceMemory",
                  "setMemoryEnable", "IOConnectCallMethod(", "IOLog("):
    assert forbidden not in fwl, f"Navi33Psp13FwLoad.h gained hardware I/O: {forbidden}"
# The SMU image is only ever a record of the pinned list (IpFw.h); the writer and runner never name it.
for text,name in ((fwl,"FwLoad.h"),(fwc,"DarwinPspFw.cpp")):
    assert "kTypeSmu" not in text and "GFX_FW_TYPE_SMU" not in text, f"{name} names the SMU type"
# Hard rule 5: our scripts and sources never modify the VBIOS (reading is allowed).
for rel in ("vm/prepare-card.sh","vm/run-vm.sh","vm/host-setup.sh","vm/reboot-to-vm.sh",
            "vm/guest-run.sh","vm/make-opencore.sh","vm/host-peek.py"):
    code=(root/rel).read_text()
    for forbidden in ("flashrom","amdvbflash","atiflash","amdgpu-flash","fwupd",
                      "of=/sys/bus/pci","> \"$D/rom\" <","dd of=/dev/mtd"):
        assert forbidden not in code, f"{rel} could modify the VBIOS: {forbidden}"
tool=(root/"tools/n33psp.c").read_text()
assert "N33_SEL_PSP_LOAD" in tool and "--i-have-ssh" in tool
assert "N33_PSP_LOAD_RING_RUN" in tool and "(writes && !ssh)" in tool
assert "N33_PSP_LOAD_FW_RUN" in tool and "navi33-psp-autoload=1" in tool
# Never write the GPU from anything that runs unattended.
for unattended in ("usb/run.sh", "usb/navi33-autorun.sh", "tools/n33autorun.c"):
    code=(root/unattended).read_text()
    assert "--load" not in code, f"{unattended} must never run the PSP load"
    assert "N33_PSP_LOAD_RUN" not in code
    assert "--ring" not in code, f"{unattended} must never run the ring stage"
    assert "N33_PSP_LOAD_RING_RUN" not in code
    assert "--ip-fw" not in code, f"{unattended} must never run the firmware stage"
    assert "N33_PSP_LOAD_FW_RUN" not in code
print("Navi33 device IOMMU staging and GPU submission boundaries: static checks passed")
# SMU stage: separate opt-in, independent latch, narrow register allowlist.
smu=(root/'src/Navi33DarwinSmu.cpp').read_text()
smuh=(root/'src/Navi33Smu13Enable.h').read_text()
assert 'if(smuMode)req.attempted=&smuAttempted_;' in load_fn
assert 'req.writeGate=!killed && !smuKilled &&' in load_fn
assert 'PE_parse_boot_argn("navi33-smu-enable"' in load_fn
assert 'PE_parse_boot_argn("navi33-smu-off"' in load_fn
assert 'bool smuAttempted_ = false;' in (root/'src/Navi33Probe.hpp').read_text()
assert 'return executeSmu(pci,req);' in writer
assert 'off!=io->regs.msg && off!=io->regs.arg && off!=io->regs.resp' in smu
assert 'run ? write32 : nullptr' in smu  # the plan has no write callback
assert 'smu13::execute(io.regs,hw,run,req.writeGate,*req.attempted,enableOnly)' in smu
assert 'Navi33DarwinSmu.o' in makefile
for forbidden in ('BaseAddress0','withSubRange','->configWrite','setMemoryEnable',
                  'setBusMasterEnable','setPowerState','WREG32(','vramWrite','vramZero'):
    assert forbidden not in smu, f'SMU stage gained {forbidden}'
for forbidden in ('<IOKit/','IOLog(','WREG32(','RREG32(','mapDeviceMemory'):
    assert forbidden not in smuh
for unattended in ('usb/run.sh','usb/navi33-autorun.sh','tools/n33autorun.c'):
    code=(root/unattended).read_text()
    assert '--smu-enable' not in code and 'N33_SMU_ENABLE_RUN' not in code
print('Navi33 SMU mailbox allowlist, independent gate and plan boundary: passed')

# SCPM mode is opt-in, mutually excludes mask writes, and shares the SMU latch.
assert 'scpmMode ? scpmGate : !scpmGate' in load_fn
assert 'PE_parse_boot_argn("navi33-smu-scpm"' in load_fn
assert 'const unsigned first=enableOnly ? 2u : 0u;' in smuh
for unattended in ('usb/run.sh','usb/navi33-autorun.sh','tools/n33autorun.c'):
    code=(root/unattended).read_text()
    assert '--smu-scpm-enable' not in code and 'N33_SMU_SCPM_RUN' not in code

# MES start: own gate, own latch, four-register allowlist enforced by the writer itself.
mes=(root/'src/Navi33DarwinMes.cpp').read_text()
mesh=(root/'src/Navi33Gfx11MesStart.h').read_text()
assert 'if(mesMode)req.attempted=&mesAttempted_;' in load_fn
assert 'if(mesMode)req.writeGate=!killed && !mesKilled &&' in load_fn
assert 'PE_parse_boot_argn("navi33-mes-start"' in load_fn
assert 'PE_parse_boot_argn("navi33-mes-off"' in load_fn
assert 'bool mesAttempted_ = false;' in (root/'src/Navi33Probe.hpp').read_text()
assert 'return executeMes(pci,req);' in writer
assert 'if(!mes_start::allowed(io->regs,off)' in mes
assert 'off==r.grbm || off==r.cntl || off==r.start || off==r.startHi' in mesh
assert 'run ? write32 : nullptr' in mes
assert 'Navi33DarwinMes.o' in makefile
for forbidden in ('BaseAddress0','withSubRange','->configWrite','setMemoryEnable',
                  'setBusMasterEnable','setPowerState','WREG32(','vramWrite','vramZero'):
    assert forbidden not in mes, f'MES stage gained {forbidden}'
for forbidden in ('<IOKit/','IOLog(','WREG32(','RREG32(','mapDeviceMemory'):
    assert forbidden not in mesh
for unattended in ('usb/run.sh','usb/navi33-autorun.sh','tools/n33autorun.c'):
    code=(root/unattended).read_text()
    assert '--mes-start' not in code and 'N33_MES_START_RUN' not in code
print('Navi33 MES start allowlist, independent gate and plan boundary: passed')

# MEC queue: own gate and latch; the writer enforces the runner's allowlist and
# maps BAR0 only through the planned 1 MiB sub-range.
mecq=(root/'src/Navi33DarwinMecQueue.cpp').read_text()
mecqh=(root/'src/Navi33Gfx11MecQueue.h').read_text()
assert 'if(mecMode)req.attempted=&mecAttempted_;' in load_fn
assert 'if(mecMode)req.writeGate=!killed && !mecKilled &&' in load_fn
assert 'PE_parse_boot_argn("navi33-mec-queue"' in load_fn
assert 'PE_parse_boot_argn("navi33-mec-off"' in load_fn
assert 'bool mecAttempted_ = false;' in (root/'src/Navi33Probe.hpp').read_text()
assert 'return executeMec(pci,req);' in writer
assert 'if(!mq::allowed(io->r,off,io->gfxMapGate)' in mecq
assert 'withSubRange(bar0,win.fwPriOffset+kWindowFromProof' in mecq
assert 'mq::Hw hw{&io,read32,nullptr,nullptr,vramRead,sleepMs};' in mecq   # the plan cannot write
assert 'Navi33DarwinMecQueue.o' in makefile
for forbidden in ('->configWrite','setMemoryEnable','setBusMasterEnable','setPowerState','WREG32('):
    assert forbidden not in mecq, f'MEC queue stage gained {forbidden}'
for forbidden in ('<IOKit/','IOLog(','WREG32(','RREG32(','mapDeviceMemory'):
    assert forbidden not in mecqh
for unattended in ('usb/run.sh','usb/navi33-autorun.sh','tools/n33autorun.c'):
    code=(root/unattended).read_text()
    assert '--mec-queue' not in code and 'N33_MEC_QUEUE_RUN' not in code
print('Navi33 MEC queue allowlist, window, independent gate and plan boundary: passed')

# Gradient: own gate and latch, pinned shader, writes only through the runner's allowlist.
grad=(root/'src/Navi33DarwinGradient.cpp').read_text()
gradh=(root/'src/Navi33Gfx11Gradient.h').read_text()
assert 'if(gradMode)req.attempted=&gradAttempted_;' in load_fn
assert 'if(gradMode)req.writeGate=!killed && !gradKilled &&' in load_fn
assert 'PE_parse_boot_argn("navi33-gradient"' in load_fn
assert 'bool gradAttempted_ = false;' in (root/'src/Navi33Probe.hpp').read_text()
assert 'return executeGradient(pci,req);' in writer
assert 'if(!gr::allowed(io->r,off)' in grad
assert 'kIOMapInhibitCache|kIOMapReadOnly' in grad          # the screen is only read by the CPU
assert 'gr::Hw hw{&io,read32,nullptr,nullptr,vramRead,nullptr,sleepMs};' in grad
assert 'sha256::equal(sha256::hash(co,bytes),kCodeObjectSha256)' in gradh
assert 'Navi33DarwinGradient.o' in makefile
for forbidden in ('->configWrite','setMemoryEnable','setBusMasterEnable','setPowerState','WREG32('):
    assert forbidden not in grad, f'gradient stage gained {forbidden}'
for forbidden in ('<IOKit/','IOLog(','WREG32(','RREG32(','mapDeviceMemory'):
    assert forbidden not in gradh
for unattended in ('usb/run.sh','usb/navi33-autorun.sh','tools/n33autorun.c'):
    code=(root/unattended).read_text()
    assert '--gradient' not in code and 'N33_GRADIENT_RUN' not in code
print('Navi33 gradient allowlist, pinned shader, independent gate and plan boundary: passed')

# Autostart (Phase B1): off by default, one boot-arg, every kill switch of its
# stages closes it, firmware only from the SHA-checked embed, never from scripts.
probe_src=(root/'src/Navi33Probe.cpp').read_text()
emb=(root/'tools/embed-firmware.py').read_text()
assert 'gates.on=bootArgOn("navi33-autostart")' in probe_src
for kill in ('navi33-autostart-off','navi33-psp-off','navi33-smu-off','navi33-mes-off','navi33-mec-off','navi33-hw-off'):
    assert f'bootArgOn("{kill}")' in probe_src, kill
assert 'if(!navi33::autostart::allowed(gates))' in probe_src
assert 'thread_call_cancel_wait(autostartCall_);' in probe_src
assert 'raise SystemExit(f"embed-firmware: {name} is' in emb and 'hashlib.sha256(data)' in emb
assert 'Navi33EmbeddedFirmware.o' in makefile
for unattended in ('usb/run.sh','usb/navi33-autorun.sh','tools/n33autorun.c'):
    assert 'navi33-autostart' not in (root/unattended).read_text()
print('Navi33 autostart gate, kill switches, embedded firmware pins and teardown: passed')

# VMID1 queue (G36): own gate and latch; GFXHUB only, never MMHUB (the display's hub).
vmw=(root/'src/Navi33DarwinVmid.cpp').read_text()
vmh=(root/'src/Navi33Gfx11Vmid.h').read_text()
assert 'if(vmMode)req.attempted=&vmAttempted_;' in load_fn
assert 'if(vmMode)req.writeGate=!killed && !vmKilled &&' in load_fn
assert 'PE_parse_boot_argn("navi33-vm-queue"' in load_fn
assert 'return executeVm(pci,req);' in writer
assert 'if(!vm::allowed(io->r,off)' in vmw
assert 'mmhub' not in vmh.lower().replace("mmhub, which the display uses, is not touched","")
assert 'Navi33DarwinVmid.o' in makefile
for forbidden in ('->configWrite','setMemoryEnable','setBusMasterEnable','setPowerState','WREG32('):
    assert forbidden not in vmw, f'VMID1 stage gained {forbidden}'
for forbidden in ('<IOKit/','IOLog(','WREG32(','RREG32(','mapDeviceMemory'):
    assert forbidden not in vmh
for unattended in ('usb/run.sh','usb/navi33-autorun.sh','tools/n33autorun.c'):
    code=(root/unattended).read_text()
    assert '--vm-queue' not in code and 'N33_VM_QUEUE_RUN' not in code
print('Navi33 VMID1 queue allowlist (GFXHUB only), independent gate and plan boundary: passed')

# VMID1 gradient (G38): own gate and latch; GFXHUB and SH_MEM only, never MMHUB.
vgw=(root/'src/Navi33DarwinVmGradient.cpp').read_text()
vgh=(root/'src/Navi33Gfx11VmGradient.h').read_text()
assert 'if(vmgMode)req.attempted=vmaMode ? &vmaAttempted_ : &vmgAttempted_;' in load_fn
assert 'if(vmgMode)req.writeGate=!killed && !vmKilled &&' in load_fn
assert 'PE_parse_boot_argn("navi33-vm-gradient"' in load_fn
assert 'return executeVmGradient(pci,req);' in writer
assert 'if(!vg::allowed(io->r,off)' in vgw
assert 'kIOMapInhibitCache|kIOMapReadOnly' in vgw          # the screen is only read by the CPU
assert 'vg::Hw hw{&io,read32,nullptr,nullptr,vramRead,nullptr,sleepMs};' in vgw
assert 'mmhub' not in vgh.lower()
assert 'Navi33DarwinVmGradient.o' in makefile
for forbidden in ('->configWrite','setMemoryEnable','setBusMasterEnable','setPowerState','WREG32('):
    assert forbidden not in vgw, f'VMID1 gradient stage gained {forbidden}'
for forbidden in ('<IOKit/','IOLog(','WREG32(','RREG32(','mapDeviceMemory'):
    assert forbidden not in vgh
for unattended in ('usb/run.sh','usb/navi33-autorun.sh','tools/n33autorun.c'):
    code=(root/unattended).read_text()
    assert '--vm-gradient' not in code and 'N33_VM_GRADIENT_RUN' not in code
print('Navi33 VMID1 gradient allowlist (GFXHUB + SH_MEM only), independent gate and plan boundary: passed')

# VRAM heap stage (G39): G38's registers and gate, its own latch; CPU VRAM access
# only inside live, unpinned heap blocks; the heap lives in the provider.
vaw=(root/'src/Navi33DarwinVmAlloc.cpp').read_text()
vah=(root/'src/Navi33Gfx11VmAlloc.h').read_text()
heap=(root/'src/Navi33VramHeap.h').read_text()+(root/'src/Navi33Gmc11Vm.h').read_text()
assert 'if(vmaMode){req.heap=&vramHeap_;req.vm1=&vm1Space_;}' in load_fn
assert 'return executeVmAlloc(pci,req);' in writer
assert 'if(!vg::allowed(io->r,off)' in vaw
assert '!io->heap->covers(off,len)' in vaw                      # pinned ranges are never CPU-accessible
assert 'kIOMapInhibitCache|kIOMapReadOnly' in vaw
assert 'va::Hw hw{&io,read32,nullptr,nullptr,vramRead,nullptr,sleepMs};' in vaw
assert 'Navi33DarwinVmAlloc.o' in makefile
assert '*this=' not in heap                                      # no large temporaries on a kernel stack
for forbidden in ('->configWrite','setMemoryEnable','setBusMasterEnable','setPowerState','WREG32('):
    assert forbidden not in vaw, f'VRAM heap stage gained {forbidden}'
for forbidden in ('<IOKit/','IOLog(','WREG32(','RREG32(','mapDeviceMemory','mmhub','MMHUB'):
    assert forbidden not in vah and forbidden not in heap
for unattended in ('usb/run.sh','usb/navi33-autorun.sh','tools/n33autorun.c'):
    code=(root/unattended).read_text()
    assert '--vm-alloc' not in code and 'N33_VM_ALLOC_RUN' not in code
print('Navi33 VRAM heap stage: G38 allowlist, heap-bounded CPU VRAM access, own latch: passed')

# Client GPU contexts (B4): own boot-arg, register writes only through client_gpu::allowed,
# CPU VRAM access only inside heap blocks, context closed with the user client.
cgw=(root/'src/Navi33DarwinClientGpu.cpp').read_text()
cgh=(root/'src/Navi33ClientGpu.h').read_text()
probe=(root/'src/Navi33Probe.cpp').read_text()
nclient=(root/'src/Navi33NativeClient.cpp').read_text()
assert 'PE_parse_boot_argn("navi33-client-gpu",&cgOn,sizeof(cgOn)) && cgOn==1u' in probe
assert 'clientGpuGate_=!pspKilled &&' in probe
assert 'if(!clientGpuGate_ || !pspLock_)return kIOReturnUnsupported;' in probe
assert 'if(!cg::allowed(h->r,off)' in cgw and '!h->heap->covers(off,len)' in cgw
assert 'if(md)gpuHost_.dev.markCpuMapped(slot,handle);' in probe   # CPU-mapped memory never reused
assert 'owner_->gpuContext(this, N33_CTX_OP_CLOSE, nullptr);' in nclient
assert 'Navi33DarwinClientGpu.o' in makefile
for forbidden in ('->configWrite','setMemoryEnable','setBusMasterEnable','setPowerState','WREG32('):
    assert forbidden not in cgw, f'client GPU host gained {forbidden}'
for forbidden in ('<IOKit/','IOLog(','WREG32(','RREG32(','mapDeviceMemory','mmhub','MMHUB'):
    assert forbidden not in cgh
for unattended in ('usb/run.sh','usb/navi33-autorun.sh','tools/n33autorun.c'):
    code=(root/unattended).read_text()
    assert 'n33gpu' not in code and 'navi33-client-gpu' not in code
assert 'h->gfxMapGate && cg::mapWriteAllowed(h->r,off,value)' in cgw
assert '!h->gfxMapGate || !gfx_map::stageRange(off,n,write)' in cgw
assert 'gfxMapAttempted_=true;' in cgh
assert 'r.hdp=gfx_map::resolveHdp(d,bytes,pageBytes);' in cgh
assert 'cg::resolve(disc,bar5Bytes,static_cast<uint32_t>(PAGE_SIZE))' in cgw
assert 'if(!r_->hdp.valid())' in cgh and 'HdpState::UnexpectedRemap' in cgh
assert 'nbio4::inspect(capacityPlan,h_.read,h_.ctx)' in cgh
print('Navi33 client GPU contexts: own gate, allowlist, heap-bounded CPU access, close with the client: passed')
# CPU mappings of client buffers are removed and unmapped before their pages can be reused.
assert 'while (IOMemoryMap *map = removeMappingForDescriptor(gpuBoMd_[i]))' in nclient
assert nclient.index('revokeCpuMappings(static_cast<uint32_t>(in[0]));') < nclient.index('return owner_->gpuBoFree(this, static_cast<uint32_t>(in[0]));')
assert nclient.index('revokeCpuMappings(0u);') < nclient.index('owner_->gpuContext(this, N33_CTX_OP_CLOSE, nullptr);')
print('Navi33 client GPU CPU mappings revoked before reuse: passed')

# The N48N front end (RADV's Darwin backend): only with the client GPU gate, root only,
# mappings revoked before BOFREE and close, HELLO opens the context.
n48c=(root/'src/Navi33N48nClient.cpp').read_text()
assert 'if (!probeOn || !clientOn || kill || !clientGpuGate_) return kIOReturnUnsupported;' in probe
assert 'clientHasPrivilege(securityID, kIOClientPrivilegeAdministrator)' in n48c
assert n48c.index('revokeCpuMappings(static_cast<uint32_t>(args->scalarInput[0]));') < n48c.index('r = owner_->n48nCall(this, session_, selector, args);')
assert n48c.index('revokeCpuMappings(0u);') < n48c.index('owner_->gpuContext(this, N33_CTX_OP_CLOSE, nullptr);')
assert 'Navi33N48nClient.o' in makefile
front=(root/'src/Navi33N48nFront.h').read_text()
# Graphics IBs only once the gfx pipe came up (G40), and one ring per submission.
assert 'if(ip==N48N_HW_IP_GFX && !d.gfxReady())return R::Unsupported;' in front
assert 'b.ip_type!=ip)' in front
for forbidden in ('<IOKit/','IOLog(','WREG32('):
    assert forbidden not in front
print('Navi33 N48N front end: gate, root, revoke-before-free, compute and (G40) graphics: passed')

# CI's glibc defines assert(expr) with ONE parameter: a comma outside parentheses in an
# assert (e.g. inside braces) breaks the build there while a variadic local glibc accepts it.
def _top_level_comma(expr):
    depth=0
    for ch in expr:
        if ch=='(': depth+=1
        elif ch==')': depth-=1
        elif ch==',' and depth==0: return True
    return False
_bad=[]
for _p in sorted((root/'tests').glob('*.cpp')):
    _s=_p.read_text();_i=0
    while True:
        _j=_s.find('assert(',_i)
        if _j<0: break
        if _j and (_s[_j-1].isalnum() or _s[_j-1]=='_'): _i=_j+7; continue
        _k=_j+7;_d=1
        while _d:
            _d+= {'(':1,')':-1}.get(_s[_k],0);_k+=1
        if _top_level_comma(_s[_j+7:_k-1]): _bad.append(f'{_p.name}:{_s.count(chr(10),0,_j)+1}')
        _i=_k
assert not _bad, f'assert with a top-level comma (wrap it in extra parentheses): {_bad}'
print('Host tests: no assert with a top-level comma (single-argument glibc assert): passed')
