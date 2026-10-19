// Navi33Probe - explicitly opted-in, PCI-configuration-only inventory at
// boot. BAR5 is read only on root-client request. The one GPU write is the
// PSP13 sOS load in Navi33DarwinPspLoad.cpp, gated by navi33-psp-load=1.
#include "Navi33Probe.hpp"
#include "Navi33DarwinPspLoad.h"
#include "Navi33NativeClient.hpp"
#include "Navi33NativePolicy.h"
#include <IOKit/IOLib.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <pexpert/pexpert.h>
#include "Navi33Identity.h"
#include "Navi33Discovery.h"
#include "Navi33HardwarePlan.h"
#include "Navi33Dcn321.h"
#include "Navi33FirmwareGuard.h"
#include "Navi33Sdma6Packet.h"
#include "Navi33Gmc11Gart.h"
#include "Navi33GpuVaArena.h"
#include "Navi33Sdma6Ring.h"
#include "Navi33DmaScatter.h"
#include "Navi33DarwinDma.h"
#include "Navi33NativeABI.h"
#include "Navi33Gfx11Command.h"
#include "Navi33LiveMmio.h"
#include "Navi33Psp13Status.h"
#include "Navi33Psp13Loader.h"
#include "Navi33Sdma6Status.h"
#include "Navi33Sdma6QueueCensus.h"
#include "Navi33HwProbeLogic.h"
#include "Navi33DarwinPciOwner.h"
#include "Navi33MmioPeek.h"
#include "Navi33Gmc11VramLive.h"
#include "Navi33VramSurvey.h"
#include "Navi33Autostart.h"
#include "Navi33EmbeddedFirmware.h"
#include <kern/thread_call.h>
#include <libkern/c++/OSData.h>
static bool bootArgOn(const char *name);
#include <libkern/libkern.h>




// Live READ-ONLY BAR5 probe, opt-in separately from navi33-probe.
// Never enables memory/BM, writes GPU registers, starts firmware or
// accesses VRAM. PCI resource is retained only during this one shot.
static bool navi33ReadStatusDword(void *ctx, uint64_t off, uint32_t *out) {
    IOMemoryMap *map = static_cast<IOMemoryMap *>(ctx);
    if (!map || !out || (off & 3u) || off > map->getLength() ||
        map->getLength() - off < sizeof(uint32_t)) return false;
    const uintptr_t base = static_cast<uintptr_t>(map->getVirtualAddress());
    if (!base || off > UINTPTR_MAX - base) return false;
    *out = *reinterpret_cast<const volatile uint32_t *>(base + static_cast<uintptr_t>(off));
    return true;
}

OSDefineMetaClassAndStructors(Navi33Probe, IOService)

IOService *Navi33Probe::probe(IOService *provider, SInt32 *score) {
    uint32_t enabled = 0;
    if (!PE_parse_boot_argn("navi33-probe", &enabled, sizeof(enabled)) || enabled != 1u)
        return nullptr;

    if (!super::probe(provider, score))
        return nullptr;

    auto *pci = OSDynamicCast(IOPCIDevice, provider);
    if (!pci)
        return nullptr;

    const navi33::Identity id = navi33::inspectPci(
        pci->configRead32(kIOPCIConfigVendorID),
        pci->configRead32(kIOPCIConfigRevisionID));
    if (!navi33::isSupported(id)) {
        IOLog("Navi33Probe: refusing PCI %04x:%04x class %02x (status %u)\n",
              id.vendorId, id.deviceId, id.baseClass,
              static_cast<unsigned>(id.status));
        return nullptr;
    }
    return this;
}

bool Navi33Probe::start(IOService *provider) {
    auto *pci = OSDynamicCast(IOPCIDevice, provider);
    if (!pci)
        return false;

    // Fail closed again if the provider changed between probe() and start().
    const navi33::Identity id = navi33::inspectPci(
        pci->configRead32(kIOPCIConfigVendorID),
        pci->configRead32(kIOPCIConfigRevisionID));
    if (!navi33::isSupported(id))
        return false;

    // Boot argument is deliberately checked at both probe and start.
    uint32_t enabled = 0;
    if (!PE_parse_boot_argn("navi33-probe", &enabled, sizeof(enabled)) || enabled != 1u)
        return false;
    if (!super::start(provider))
        return false;
    stageOwnerLock_=IOLockAlloc();
    pspLock_=IOLockAlloc();
    if(!stageOwnerLock_ || !pspLock_) {
        super::stop(provider);
        return false;
    }
    nativeFlags_ = N33_CAP_PCI_INVENTORY;

    setProperty("Navi33,Stage", "PCI inventory and supplied-ROM IP discovery only");
    setProperty("Navi33,NativeABIVersion", static_cast<uint64_t>(N33_ABI_VERSION), 32);
    setProperty("Navi33,NativeUserClientType", static_cast<uint64_t>(N33_UC_TYPE), 32);
    setProperty("Navi33,NativeCapabilities", static_cast<uint64_t>(N33_CAP_PCI_INVENTORY), 32);
    setProperty("Navi33,MetalHardwareReady", static_cast<uint64_t>(0), 8);
    setProperty("Navi33,VendorID", static_cast<uint64_t>(id.vendorId), 16);
    setProperty("Navi33,DeviceID", static_cast<uint64_t>(id.deviceId), 16);
    setProperty("Navi33,RevisionID", static_cast<uint64_t>(id.revisionId), 8);
    setProperty("Navi33,PCIClass", static_cast<uint64_t>(
        (static_cast<uint32_t>(id.baseClass) << 16) |
        (static_cast<uint32_t>(id.subClass) << 8) | id.progIf), 32);
    setProperty("Navi33,SubsystemPair", static_cast<uint64_t>(pci->configRead32(0x2Cu)), 32);
    // Owner probe: default off, read-only (config reads and one open/close).
    // Nothing here grants GPU access; see Navi33PciOwner.h.
    uint32_t ownerOn=0u, ownerOff=0u, recoveryOn=0u;
    const bool ownerGate=PE_parse_boot_argn("navi33-owner-probe",&ownerOn,sizeof(ownerOn)) && ownerOn==1u;
    const bool ownerKill=PE_parse_boot_argn("navi33-owner-off",&ownerOff,sizeof(ownerOff)) && ownerOff!=0u;
    const bool recovery=PE_parse_boot_argn("navi33-owner-recovery",&recoveryOn,sizeof(recoveryOn)) && recoveryOn==1u;
    // Unmapped DMA: only when the card has no IOMapper, and only with this
    // explicit opt-in. Recorded as Navi33,OwnerDmaMode; it grants no writes.
    uint32_t physicalOn=0u;
    const bool physicalOptIn=PE_parse_boot_argn("navi33-owner-physical",&physicalOn,sizeof(physicalOn)) && physicalOn==1u;
    // Hold: keeps the PCI open for one owner epoch, only when the decision is
    // Owned. Off by default; it blocks other clients from the card while held.
    uint32_t holdOn=0u;
    const bool holdGate=PE_parse_boot_argn("navi33-owner-hold",&holdOn,sizeof(holdOn)) && holdOn==1u;
    if(holdGate) {
        navi33::darwin_owner::ProbeReport report{};
        const bool held=navi33::darwin_owner::holdOwner(pci,this,ownerKill,recovery,physicalOptIn,pciOwner_,pciOwnerTicket_,&report);
        publishOwnerReport(report);
        setProperty("Navi33,OwnerHeld", static_cast<uint64_t>(held ? 1 : 0), 8);
        setProperty("Navi33,OwnerEpoch", static_cast<uint64_t>(pciOwnerTicket_.epoch), 32);
        setProperty("Navi33,OwnerPhase", static_cast<uint64_t>(pciOwner_.phase()), 8);
    } else if(ownerGate) {
        publishOwnerReport(navi33::darwin_owner::runOwnerProbe(pci,this,true,ownerKill,recovery,physicalOptIn));
    }
    // Report any explicitly enabled PSP or SMU write stage.
    uint32_t pspLoadOn=0u, pspRingOn=0u, pspFwOn=0u, pspOff=0u, smuOn=0u, smuOff=0u, mesOn=0u, mesOff=0u,
             mecOn=0u, mecOff=0u, gradOn=0u, gradOff=0u;
    const bool pspKilled=PE_parse_boot_argn("navi33-psp-off", &pspOff,
                                            sizeof(pspOff)) && pspOff!=0u;
    const bool pspWrites=!pspKilled &&
        ((PE_parse_boot_argn("navi33-psp-load", &pspLoadOn,
                             sizeof(pspLoadOn)) && pspLoadOn==1u) ||
         (PE_parse_boot_argn("navi33-psp-ring", &pspRingOn,
                             sizeof(pspRingOn)) && pspRingOn==1u) ||
         (PE_parse_boot_argn("navi33-psp-fw", &pspFwOn,
                             sizeof(pspFwOn)) && pspFwOn==1u));
    const bool smuWrites=!pspKilled &&
        !(PE_parse_boot_argn("navi33-smu-off",&smuOff,sizeof(smuOff)) && smuOff!=0u) &&
        PE_parse_boot_argn("navi33-smu-enable",&smuOn,sizeof(smuOn)) && smuOn==1u;
    const bool mesWrites=!pspKilled &&
        !(PE_parse_boot_argn("navi33-mes-off",&mesOff,sizeof(mesOff)) && mesOff!=0u) &&
        PE_parse_boot_argn("navi33-mes-start",&mesOn,sizeof(mesOn)) && mesOn==1u;
    const bool mecWrites=!pspKilled &&
        !(PE_parse_boot_argn("navi33-mec-off",&mecOff,sizeof(mecOff)) && mecOff!=0u) &&
        PE_parse_boot_argn("navi33-mec-queue",&mecOn,sizeof(mecOn)) && mecOn==1u;
    const bool gradWrites=!pspKilled &&
        !(PE_parse_boot_argn("navi33-gradient-off",&gradOff,sizeof(gradOff)) && gradOff!=0u) &&
        PE_parse_boot_argn("navi33-gradient",&gradOn,sizeof(gradOn)) && gradOn==1u;
    setProperty("Navi33,GpuWritesEnabled",
                static_cast<uint64_t>(pspWrites || smuWrites || mesWrites || mecWrites || gradWrites), 32);
    setProperty("Navi33,FirmwareLoadingImplemented",
                "PSP13 sOS bootloader, KM ring (TOC, TMR) and IP firmware records only");
    setProperty("Navi33,GpuQueuesImplemented", static_cast<uint64_t>(0), 8);
    setProperty("Navi33,DcnModeSettingImplemented", static_cast<uint64_t>(0), 8);
    // Software page-table and ring models compile into this Kext for shared
    // type-checking, but they do not map DMA or submit to a physical GPU.
    setProperty("Navi33,Gmc11GartHardwareReady", static_cast<uint64_t>(0), 8);
    setProperty("Navi33,SDMA6HardwareRingReady", static_cast<uint64_t>(0), 8);
    setProperty("Navi33,TLBInvalidationEnabled", static_cast<uint64_t>(0), 8);
    setProperty("Navi33,SDMA6DoorbellEnabled", static_cast<uint64_t>(0), 8);
    setProperty("Navi33,DmaPreflightExecuted", static_cast<uint64_t>(0), 8);
    // Separate explicit one-shot address preflight. Default boot allocates no
    // pinned DMA/physical buffer. The navi33-dma-off kill switch always wins.
    uint32_t dmaOptIn=0u, dmaOff=0u;
    const bool disabled=PE_parse_boot_argn("navi33-dma-off", &dmaOff,
                                           sizeof(dmaOff)) && dmaOff != 0u;
    if (!disabled && PE_parse_boot_argn("navi33-dma-preflight", &dmaOptIn,
                                        sizeof(dmaOptIn)) && dmaOptIn == 1u) {
        const navi33::darwin_dma::Report dmaReport =
            navi33::darwin_dma::runOnce(pci);
        setProperty("Navi33,DmaPreflightExecuted", static_cast<uint64_t>(1), 8);
        setProperty("Navi33,DmaPreflightStatus",
                    static_cast<uint64_t>(dmaReport.code), 8);
        setProperty("Navi33,DmaPreflightSegments",
                    static_cast<uint64_t>(dmaReport.segmentCount), 8);
        setProperty("Navi33,DmaPreflightPages",
                    static_cast<uint64_t>(dmaReport.pageCount), 8);
        setProperty("Navi33,DmaPreflightAddressKind",
                    dmaReport.code==navi33::darwin_dma::Code::PhysicalAddressOnly
                        ? "CPU-physical candidate only; GPU reachability unproven"
                        : "device IOVA or failure");
        IOLog("Navi33Probe: optional DMA address preflight returned %u (%u segments, %u pages), GPU DMA=NO\n",
              static_cast<unsigned>(dmaReport.code),
              static_cast<unsigned>(dmaReport.segmentCount),
              static_cast<unsigned>(dmaReport.pageCount));
    }

    // Inspect IOPCIFamily's *assigned resource metadata* only. In particular,
    // do not call setMemoryEnable(), mapDeviceMemoryWithRegister(), or touch
    // VRAM, doorbells, BAR5 MMIO registers, display timing, or firmware.
    struct Bar {
        UInt8 registerOffset;
        const char *sizeKey;
    };
    static const Bar kBars[] = {
        {kIOPCIConfigBaseAddress0, "Navi33,BAR0Bytes"},
        {kIOPCIConfigBaseAddress2, "Navi33,BAR2Bytes"},
        {kIOPCIConfigBaseAddress5, "Navi33,BAR5Bytes"}
    };
    for (const Bar &bar : kBars) {
        IODeviceMemory *range = pci->getDeviceMemoryWithRegister(bar.registerOffset);
        if (range)
            setProperty(bar.sizeKey, static_cast<uint64_t>(range->getLength()), 64);
    }

    // Optional bootloader-supplied full flash dump. Parsing OSData is a
    // CPU-only operation: no GPU resource is mapped and no MMIO is read.
    auto *snapshot = OSDynamicCast(OSData, pci->getProperty("ATY,bin_image"));
    if (!snapshot) {
        setProperty("Navi33,DiscoveryStatus", "No ATY,bin_image supplied");
    } else if (snapshot->getLength() > navi33::IPDiscovery::kMaxBlob ||
               snapshot->getLength() < 12u) {
        setProperty("Navi33,DiscoveryStatus", "Snapshot size refused");
    } else {
        const uint8_t *bytes = static_cast<const uint8_t *>(snapshot->getBytesNoCopy());
        navi33::IPDiscovery disc;
        if (!disc.parse(bytes, snapshot->getLength())) {
            setProperty("Navi33,DiscoveryStatus", "No checksum-valid IP Discovery table");
        } else {
            setProperty("Navi33,DiscoveryStatus", "Validated supplied ROM snapshot");
            setProperty("Navi33,DiscoveryOffset", static_cast<uint64_t>(disc.binaryOffset()), 64);
            setProperty("Navi33,IPCount", static_cast<uint64_t>(disc.count()), 16);
            setProperty("Navi33,IPBases64", static_cast<uint64_t>(disc.is64BitBase()), 8);
            const navi33::InventoryReport generation = navi33::inventory(disc);
            const bool candidate = generation.code == navi33::InventoryCode::Pass;
            if (candidate) {
                nativeFlags_ |= N33_CAP_ROM_DISCOVERY;
                setProperty("Navi33,NativeCapabilities", static_cast<uint64_t>(nativeFlags_), 32);
            }
            setProperty("Navi33,VersionsMatch", static_cast<uint64_t>(candidate), 8);
            setProperty("Navi33,HardwarePlanStatus",
                        static_cast<uint64_t>(generation.code), 8);
            setProperty("Navi33,HardwarePlanMatchedEngines",
                        static_cast<uint64_t>(generation.matchedEngines), 8);
            setProperty("Navi33,HardwarePlanRejectedEngine",
                        static_cast<uint64_t>(generation.failingEngine), 8);

            // Assigned BAR5 resource metadata only. No MMIO mapping or
            // device access: the plan lists hypothetical read locations,
            // strictly bounded to the aperture. It is NOT executed here.
            IODeviceMemory *regs = pci->getDeviceMemoryWithRegister(
                kIOPCIConfigBaseAddress5);
            const uint64_t bar5Bytes = regs ?
                static_cast<uint64_t>(regs->getLength()) : 0u;
            const navi33::dcn321::ReadPlan display =
                navi33::dcn321::buildReadPlan(disc, bar5Bytes);
            setProperty("Navi33,DCNReadPlanStatus",
                        static_cast<uint64_t>(display.code), 8);
            setProperty("Navi33,DCNReadPlanCount",
                        static_cast<uint64_t>(display.resolved), 8);
            if (display.code == navi33::AddressCode::Pass) {
                const uint64_t otg0 = display.offsets[
                  static_cast<size_t>(navi33::dcn321::Register::Pipe0FrameCount)];
                const uint64_t hubp0 = display.offsets[
                  static_cast<size_t>(navi33::dcn321::Register::Pipe0SurfaceLo)];
                setProperty("Navi33,DCN321OTG0FrameOffset", otg0, 64);
                setProperty("Navi33,DCN321HUBP0SurfaceOffset", hubp0, 64);
            }
            // This is deliberately NOT a GPU bring-up: read three status
            // registers on each of four OTGs only after two boot flags and
            // checksum-valid Navi33 IP Discovery. Never write a GPU register.
            uint32_t mmioOptIn = 0u, mmioOff = 0u;
            const bool liveOff = PE_parse_boot_argn("navi33-mmio-off",
                &mmioOff, sizeof(mmioOff)) && mmioOff != 0u;
            const bool liveOptIn = PE_parse_boot_argn("navi33-mmio-read",
                &mmioOptIn, sizeof(mmioOptIn)) && mmioOptIn == 1u;
            if (!liveOff && liveOptIn && candidate) {
                const navi33::live_mmio::Plan plan =
                    navi33::live_mmio::buildPlan(disc, bar5Bytes);
                setProperty("Navi33,LiveMMIOPlanStatus",
                    static_cast<uint64_t>(plan.code), 8);
                if (plan.code == navi33::live_mmio::Code::Pass && regs) {
                    IOMemoryMap *mmio = regs->map();
                    if (mmio) {
                        const navi33::live_mmio::Sample s =
                            navi33::live_mmio::readSnapshot(plan, true,
                                navi33ReadStatusDword, mmio);
                        setProperty("Navi33,LiveMMIOStatus",
                            static_cast<uint64_t>(s.code), 8);
                        setProperty("Navi33,LiveMMIOReads",
                            static_cast<uint64_t>(s.reads), 8);
                        if (s.code == navi33::live_mmio::Code::Pass) {
                            setProperty("Navi33,OTGActiveMask",
                                static_cast<uint64_t>(s.activeMask), 8);
                            setProperty("Navi33,OTGClockMask",
                                static_cast<uint64_t>(s.clockMask), 8);
                            for (unsigned pipe=0; pipe<4u; ++pipe) {
                                const char *names[4] = {
                                    "Navi33,OTG0Frame", "Navi33,OTG1Frame",
                                    "Navi33,OTG2Frame", "Navi33,OTG3Frame"
                                };
                                setProperty(names[pipe],
                                    static_cast<uint64_t>(s.frame[pipe]), 32);
                            }
                        }
                        mmio->release();
                    } else {
                        setProperty("Navi33,LiveMMIOStatus", "BAR5 mapping failed");
                    }
                }
            }
            struct Query { uint16_t hwid; const char *key; };
            static const Query queries[] = {
                {navi33::ipid::GC, "Navi33,GCVersion"},
                {navi33::ipid::SDMA0, "Navi33,SDMAVersion"},
                {navi33::ipid::MP0, "Navi33,PSPVersion"},
                {navi33::ipid::MP1, "Navi33,SMUVersion"},
                {navi33::ipid::DMU, "Navi33,DCNVersion"},
                {navi33::ipid::MMHUB, "Navi33,MMHUBVersion"}
            };
            for (const Query &q : queries) {
                navi33::IPEntry e{};
                if (!disc.find(q.hwid, 0u, e)) continue;
                const uint32_t version = (static_cast<uint32_t>(e.major) << 16u) |
                                         (static_cast<uint32_t>(e.minor) << 8u) | e.revision;
                setProperty(q.key, static_cast<uint64_t>(version), 32);
            }
            IOLog("Navi33Probe: supplied IP snapshot %u blocks, GFX11 gate=%s, DCN read-plan=%u; no GPU access\n",
                  disc.count(), candidate ? "MATCH" : "MISMATCH",
                  static_cast<unsigned>(display.code));
        }
    }

    IOLog("Navi33Probe: RX 7600 %04x:%04x rev %02x, inventory only; GPU untouched\n",
          id.vendorId, id.deviceId, id.revisionId);
    registerService();
    // Phase B1 boot-time bring-up: off unless navi33-autostart=1, and any kill
    // switch of a stage it runs (or of the hardware probe) closes it.
    navi33::autostart::Gates gates{};
    gates.on=bootArgOn("navi33-autostart");gates.autostartOff=bootArgOn("navi33-autostart-off");
    gates.pspOff=bootArgOn("navi33-psp-off");gates.smuOff=bootArgOn("navi33-smu-off");
    gates.mesOff=bootArgOn("navi33-mes-off");gates.mecOff=bootArgOn("navi33-mec-off");
    gates.hwProbe=bootArgOn("navi33-hw-probe");gates.hwOff=bootArgOn("navi33-hw-off");
    if(!navi33::autostart::allowed(gates)) {
        setProperty("Navi33,Autostart",gates.on ? "refused by a kill switch or a closed hw probe" : "off");
    } else if((autostartCall_=thread_call_allocate(&Navi33Probe::autostartThunk,this))) {
        setProperty("Navi33,GpuWritesEnabled",static_cast<uint64_t>(1),32);
        setProperty("Navi33,Autostart","running");
        thread_call_enter(autostartCall_);
    } else {
        setProperty("Navi33,Autostart","no thread call");
    }
    return true;
}

// PSP13 status does not load firmware, start queues, write a mailbox,
// or change power states. Called synchronously from root-gated N33N query.
// A non-zero mailbox indicates an existing sOS reported life, NOT gfx ready.
n33_psp_status Navi33Probe::pspStatus() {
    navi33::psp13::Sample sample{};
    uint32_t enabled=0u, disabled=0u;
    const bool off = PE_parse_boot_argn("navi33-psp-off", &disabled,
                                       sizeof(disabled)) && disabled != 0u;
    const bool on = PE_parse_boot_argn("navi33-psp-read", &enabled,
                                      sizeof(enabled)) && enabled == 1u;
    if (!on || off) return navi33::psp13::report(sample);
    auto *pci = OSDynamicCast(IOPCIDevice, getProvider());
    if (!pci) {
        sample.state = N33_PSP_NO_INVENTORY;
        return navi33::psp13::report(sample);
    }
    const navi33::Identity id = navi33::inspectPci(
        pci->configRead32(kIOPCIConfigVendorID),
        pci->configRead32(kIOPCIConfigRevisionID));
    if (!navi33::isSupported(id)) {
        sample.state = N33_PSP_NO_INVENTORY;
        return navi33::psp13::report(sample);
    }
    auto *snapshot = OSDynamicCast(OSData, pci->getProperty("ATY,bin_image"));
    if (!snapshot || snapshot->getLength() < 32u ||
        snapshot->getLength() > navi33::IPDiscovery::kMaxBlob) {
        sample.state = N33_PSP_NO_INVENTORY;
        return navi33::psp13::report(sample);
    }
    navi33::IPDiscovery disc;
    if (!disc.parse(static_cast<const uint8_t *>(snapshot->getBytesNoCopy()),
                    snapshot->getLength()) ||
        navi33::inventory(disc).code != navi33::InventoryCode::Pass) {
        sample.state = N33_PSP_NO_INVENTORY;
        return navi33::psp13::report(sample);
    }
    IODeviceMemory *bar = pci->getDeviceMemoryWithRegister(kIOPCIConfigBaseAddress5);
    const uint64_t barLength = bar ? static_cast<uint64_t>(bar->getLength()) : 0u;
    const navi33::psp13::Plan plan = navi33::psp13::buildPlan(disc, barLength);
    if (plan.state != N33_PSP_STOPPED) {
        sample.state = plan.state;
        return navi33::psp13::report(sample);
    }
    if (!bar) {
        sample.state = N33_PSP_BAD_MMIO_PLAN;
        return navi33::psp13::report(sample);
    }
    IOMemoryMap *mapping = bar->map();
    if (!mapping) {
        sample.state = N33_PSP_READ_ERROR;
        return navi33::psp13::report(sample);
    }
    // Mapping may be shorter than IOPCIFamily's assigned BAR resource.
    if (mapping->getLength() < barLength) {
        sample.state = N33_PSP_BAD_MMIO_PLAN;
    } else {
        sample = navi33::psp13::readOnce(plan, true,
                                         navi33ReadStatusDword, mapping);
    }
    mapping->release();
    return navi33::psp13::report(sample);
}

// SDMA0 queue-0 readback.  Kernel cannot submit packets, map GPU VRAM,
// alter doorbells or start firmware from this path.  Five bounded reads.
n33_sdma_status Navi33Probe::sdmaStatus() {
    navi33::sdma6live::Sample sample{};
    uint32_t enabled=0u, disabled=0u;
    const bool on=PE_parse_boot_argn("navi33-sdma-read", &enabled,
                                     sizeof(enabled)) && enabled==1u;
    const bool off=PE_parse_boot_argn("navi33-sdma-off", &disabled,
                                      sizeof(disabled)) && disabled!=0u;
    if(!on || off) return navi33::sdma6live::report(sample);
    auto *pci=OSDynamicCast(IOPCIDevice, getProvider());
    if(!pci) {
        sample.state=N33_SDMA_NO_INVENTORY;
        return navi33::sdma6live::report(sample);
    }
    const navi33::Identity id=navi33::inspectPci(
        pci->configRead32(kIOPCIConfigVendorID),
        pci->configRead32(kIOPCIConfigRevisionID));
    if(!navi33::isSupported(id)) {
        sample.state=N33_SDMA_NO_INVENTORY;
        return navi33::sdma6live::report(sample);
    }
    auto *snapshot=OSDynamicCast(OSData,pci->getProperty("ATY,bin_image"));
    if(!snapshot || snapshot->getLength()<32u ||
       snapshot->getLength()>navi33::IPDiscovery::kMaxBlob) {
        sample.state=N33_SDMA_NO_INVENTORY;
        return navi33::sdma6live::report(sample);
    }
    navi33::IPDiscovery disc;
    if(!disc.parse(static_cast<const uint8_t *>(snapshot->getBytesNoCopy()),
                   snapshot->getLength()) ||
       navi33::inventory(disc).code!=navi33::InventoryCode::Pass) {
        sample.state=N33_SDMA_NO_INVENTORY;
        return navi33::sdma6live::report(sample);
    }
    IODeviceMemory *bar=pci->getDeviceMemoryWithRegister(kIOPCIConfigBaseAddress5);
    const uint64_t bytes=bar?static_cast<uint64_t>(bar->getLength()):0u;
    const navi33::sdma6live::Plan plan =
        navi33::sdma6live::buildPlan(disc,bytes);
    if(plan.state!=N33_SDMA_OBSERVED) {
        sample.state=plan.state;
        return navi33::sdma6live::report(sample);
    }
    if(!bar) {
        sample.state=N33_SDMA_BAD_MMIO_PLAN;
        return navi33::sdma6live::report(sample);
    }
    IOMemoryMap *mapping=bar->map();
    if(!mapping) {
        sample.state=N33_SDMA_READ_ERROR;
    } else {
        if(mapping->getLength()<bytes) {
            sample.state=N33_SDMA_BAD_MMIO_PLAN;
        } else {
            sample=navi33::sdma6live::readOnce(plan,true,
                                               navi33ReadStatusDword,mapping);
        }
        mapping->release();
    }
    return navi33::sdma6live::report(sample);
}

// Root-gated READ-ONLY Queue0 register census. Two snapshots of
// physical BAR5 are checked for stability; an idle register snapshot
// is NOT proof of any PCI, firmware or queue ownership. NO writes.
n33_queue_census_status Navi33Probe::queueCensus() {
    navi33::queue_census::Sample sample{};
    uint32_t enabled=0u,disabled=0u;
    const bool on=PE_parse_boot_argn("navi33-sdma-read",&enabled,
                                     sizeof(enabled)) && enabled==1u;
    const bool off=PE_parse_boot_argn("navi33-sdma-off",&disabled,
                                      sizeof(disabled)) && disabled!=0u;
    if(!on || off)return navi33::queue_census::report(sample);
    auto *pci=OSDynamicCast(IOPCIDevice,getProvider());
    if(!pci) {
        sample.state=N33_QUEUE_NO_INVENTORY;
        return navi33::queue_census::report(sample);
    }
    const auto id=navi33::inspectPci(
        pci->configRead32(kIOPCIConfigVendorID),
        pci->configRead32(kIOPCIConfigRevisionID));
    if(!navi33::isSupported(id)) {
        sample.state=N33_QUEUE_NO_INVENTORY;
        return navi33::queue_census::report(sample);
    }
    auto *snapshot=OSDynamicCast(OSData,pci->getProperty("ATY,bin_image"));
    if(!snapshot || snapshot->getLength()<64u ||
       snapshot->getLength()>navi33::IPDiscovery::kMaxBlob) {
        sample.state=N33_QUEUE_NO_INVENTORY;
        return navi33::queue_census::report(sample);
    }
    navi33::IPDiscovery disc;
    if(!disc.parse(static_cast<const uint8_t *>(snapshot->getBytesNoCopy()),
                   snapshot->getLength()) ||
       navi33::inventory(disc).code!=navi33::InventoryCode::Pass) {
        sample.state=N33_QUEUE_NO_INVENTORY;
        return navi33::queue_census::report(sample);
    }
    IODeviceMemory *bar=pci->getDeviceMemoryWithRegister(
        kIOPCIConfigBaseAddress5);
    const uint64_t bytes=bar?static_cast<uint64_t>(bar->getLength()):0u;
    const auto plan=navi33::queue_census::fromDiscovery(disc,bytes);
    if(plan.state!=N33_QUEUE_OBSERVED) {
        sample.state=plan.state;
        return navi33::queue_census::report(sample);
    }
    if(!bar) {
        sample.state=N33_QUEUE_BAD_MMIO_PLAN;
        return navi33::queue_census::report(sample);
    }
    IOMemoryMap *mapping=bar->map();
    if(!mapping) {
        sample.state=N33_QUEUE_READ_ERROR;
    } else {
        if(static_cast<uint64_t>(mapping->getLength())<bytes) {
            sample.state=N33_QUEUE_BAD_MMIO_PLAN;
        } else {
            sample=navi33::queue_census::readStable(
                plan,true,navi33ReadStatusDword,mapping);
        }
        mapping->release();
    }
    return navi33::queue_census::report(sample);
}

// This is an ACTUAL physical RX7600 status read, not a mock test.
// It never changes PCI command state, VRAM, SDMA rings, page tables,
// MMHUB contexts, or doorbells. Results never advertise HW_VM/GFX/Metal.
n33_hw_preflight Navi33Probe::hardwarePreflight(
    const uint8_t *romBytes,size_t romLength) {
    uint32_t enabled=0u,off=0u;
    const bool allowed=PE_parse_boot_argn("navi33-hw-probe",
                                         &enabled,sizeof(enabled)) && enabled==1u;
    const bool stopped=PE_parse_boot_argn("navi33-hw-off",
                                         &off,sizeof(off)) && off!=0u;
    if(!allowed || stopped)
        return navi33::hwprobe::blank(N33_HW_DISABLED);

    auto *pci=OSDynamicCast(IOPCIDevice,getProvider());
    if(!pci)return navi33::hwprobe::blank(N33_HW_NO_PROVIDER);
    const navi33::Identity id=navi33::inspectPci(
        pci->configRead32(kIOPCIConfigVendorID),
        pci->configRead32(kIOPCIConfigRevisionID));
    if(!navi33::isSupported(id))
        return navi33::hwprobe::blank(N33_HW_NO_PROVIDER);

    // Either read the bootloader-provided ROM or take the exact bounded
    // caller-supplied bytes for this *one* synchronous root-client request.
    // No firmware loading, persistent storage or PCI ROM-decode writes.
    if((romBytes && (romLength<64u ||
        romLength>navi33::IPDiscovery::kMaxBlob)) ||
       (!romBytes && romLength!=0u))
        return navi33::hwprobe::blank(N33_HW_BAD_ROM);
    const auto *property=romBytes ? nullptr :
        OSDynamicCast(OSData,pci->getProperty("ATY,bin_image"));
    const uint8_t *rom=romBytes ? romBytes :
        property ? static_cast<const uint8_t *>(property->getBytesNoCopy()) :
        nullptr;
    const size_t bytesInRom=romBytes ? romLength :
        property ? static_cast<size_t>(property->getLength()) : 0u;
    if(!rom || bytesInRom<64u)
        return navi33::hwprobe::blank(N33_HW_NO_ROM);
    if(bytesInRom>navi33::IPDiscovery::kMaxBlob)
        return navi33::hwprobe::blank(N33_HW_BAD_ROM);
    navi33::IPDiscovery disc;
    if(!disc.parse(rom,bytesInRom) ||
       navi33::inventory(disc).code!=navi33::InventoryCode::Pass)
        return navi33::hwprobe::blank(N33_HW_BAD_ROM);

    IODeviceMemory *bar=pci->getDeviceMemoryWithRegister(
        kIOPCIConfigBaseAddress5);
    if(!bar || bar->getLength()<4u)
        return navi33::hwprobe::blank(N33_HW_NO_BAR5);
    const uint64_t bytes=static_cast<uint64_t>(bar->getLength());
    const navi33::psp13::Plan pspPlan=navi33::psp13::buildPlan(disc,bytes);
    const navi33::sdma6live::Plan sdmaPlan=
        navi33::sdma6live::buildPlan(disc,bytes);
    const navi33::hwprobe::HubOffsets hubs=
        navi33::hwprobe::planHubs(disc,bytes);
    if(pspPlan.state!=N33_PSP_STOPPED ||
       sdmaPlan.state!=N33_SDMA_OBSERVED || !hubs.valid)
        return navi33::hwprobe::blank(N33_HW_BAD_OFFSETS);

    IOMemoryMap *mapped=bar->map();
    if(!mapped)return navi33::hwprobe::blank(N33_HW_MAP_FAILED);
    if(static_cast<uint64_t>(mapped->getLength())<bytes) {
        mapped->release();
        return navi33::hwprobe::blank(N33_HW_MAP_FAILED);
    }
    const navi33::psp13::Sample psp=navi33::psp13::readOnce(
        pspPlan,true,navi33ReadStatusDword,mapped);
    const navi33::sdma6live::Sample sdma=navi33::sdma6live::readOnce(
        sdmaPlan,true,navi33ReadStatusDword,mapped);
    uint32_t bootloader=0u;
    const bool bootRead=navi33::psp13::readBootloader(
        pspPlan,navi33ReadStatusDword,mapped,&bootloader);
    uint32_t mmContext=UINT32_MAX,gfxContext=UINT32_MAX;
    const bool mmOk=navi33ReadStatusDword(mapped,hubs.mmhub,&mmContext);
    const bool gfxOk=navi33ReadStatusDword(mapped,hubs.gfxhub,&gfxContext);
    mapped->release();
    if(!mmOk || !gfxOk)
        return navi33::hwprobe::blank(N33_HW_MMIO_READ_FAILED);

    const n33_sdma_status sdmaStatus=navi33::sdma6live::report(sdma);
    n33_hw_preflight result=navi33::hwprobe::classify(
        navi33::psp13::report(psp),sdmaStatus,mmContext,gfxContext,bytes);
    if(bootRead)++result.mmio_reads;
    // The ABI struct has no room for these raw words, so they go to the
    // kernel log and to ioreg, which the physical results script captures.
    char boot[12],raw[192];
    if(bootRead)snprintf(boot,sizeof(boot),"%08x",bootloader);
    else snprintf(boot,sizeof(boot),"unread");
    snprintf(raw,sizeof(raw),
             "reason %u reads %u PSP %u SDMA %u C2PMSG_35 %s "
             "SDMA0_STATUS %08x UCODE_CHECKSUM %08x "
             "MMHUB_CTX0 %08x GFXHUB_CTX0 %08x",
             result.reason,result.mmio_reads,result.psp_state,
             result.sdma_state,boot,sdmaStatus.status_register,
             sdmaStatus.ucode_checksum,mmContext,gfxContext);
    setProperty("Navi33,LastHwPreflight",raw);
    // These two facts are not optional: a status read doesn't demonstrate
    // exclusive queue ownership or any working compute command path.
    IOLog("Navi33Probe: RX7600 physical HW preflight %s; no GPU copy submitted\n",
          raw);
    return result;
}

// Root-only allowlisted BAR5 window read for new diagnostics that should
// not need a new Kext. Same gate and kill switch as hardwarePreflight;
// the window table, not the caller, decides which registers are readable.
n33_peek_result Navi33Probe::peekRegisters(uint32_t window,uint32_t first,
    uint32_t count,const uint8_t *romBytes,size_t romLength) {
    uint32_t enabled=0u,off=0u;
    const bool allowed=PE_parse_boot_argn("navi33-hw-probe",
                                         &enabled,sizeof(enabled)) && enabled==1u;
    const bool stopped=PE_parse_boot_argn("navi33-hw-off",
                                         &off,sizeof(off)) && off!=0u;
    if(!allowed || stopped)
        return navi33::peek::blank(N33_PEEK_DISABLED,window,first,count);
    auto *pci=OSDynamicCast(IOPCIDevice,getProvider());
    if(!pci || !navi33::isSupported(navi33::inspectPci(
           pci->configRead32(kIOPCIConfigVendorID),
           pci->configRead32(kIOPCIConfigRevisionID))))
        return navi33::peek::blank(N33_PEEK_NO_PROVIDER,window,first,count);
    navi33::IPDiscovery disc;
    if(!romBytes || romLength<64u ||
       romLength>navi33::IPDiscovery::kMaxBlob ||
       !disc.parse(romBytes,romLength))
        return navi33::peek::blank(N33_PEEK_BAD_ROM,window,first,count);
    IODeviceMemory *bar=pci->getDeviceMemoryWithRegister(
        kIOPCIConfigBaseAddress5);
    if(!bar || bar->getLength()<4u)
        return navi33::peek::blank(N33_PEEK_NO_BAR5,window,first,count);
    const uint64_t bytes=static_cast<uint64_t>(bar->getLength());
    const navi33::peek::Plan plan=navi33::peek::plan(disc,window,first,
                                                      count,bytes);
    if(plan.state!=N33_PEEK_DONE)
        return navi33::peek::blank(plan.state,window,first,count);
    IOMemoryMap *mapped=bar->map();
    if(!mapped)
        return navi33::peek::blank(N33_PEEK_MAP_FAILED,window,first,count);
    if(static_cast<uint64_t>(mapped->getLength())<bytes) {
        mapped->release();
        return navi33::peek::blank(N33_PEEK_MAP_FAILED,window,first,count);
    }
    const n33_peek_result result=navi33::peek::read(
        plan,window,first,count,navi33ReadStatusDword,mapped);
    mapped->release();
    return result;
}

// The GOP console is the region fw_pri must stay clear of, by physical address,
// so from boot_args (see psp13load::bootConsole). A guest with no GOP gets the
// host's view of the firmware console (vm/prepare-card.sh); the executors use it
// only when boot_args has none.
static void fillConsole(navi33::darwin_psp::Request &req) {
    if(const auto *ba=static_cast<const boot_args *>(PE_state.bootArgs)) {
        const navi33::psp13load::Range c=navi33::psp13load::bootConsole(
            ba->Video.v_baseAddr,ba->Video.v_rowBytes,ba->Video.v_height);
        req.consolePhys=c.phys;req.consoleBytes=c.bytes;
    }
    uint64_t hostOffset=0u,hostBytes=0u;
    req.hostConsole=PE_parse_boot_argn("navi33-hostfb-offset",&hostOffset,
                                       sizeof(hostOffset)) &&
        PE_parse_boot_argn("navi33-hostfb-bytes",&hostBytes,sizeof(hostBytes));
    req.hostConsoleOffset=hostOffset;req.hostConsoleBytes=hostBytes;
}

static bool bootArgOn(const char *name) {
    uint32_t v=0u;
    return PE_parse_boot_argn(name,&v,sizeof(v)) && v!=0u;
}

// Phase B1: the whole bring-up from start(), on a thread call, with the
// embedded firmware. navi33-autostart=1 opens it; every kill switch of a stage
// it runs closes it. Each stage goes through its own executor, plan check and
// one-shot latch, so the N33N tools cannot repeat a stage it ran.
void Navi33Probe::autostartThunk(thread_call_param_t self,thread_call_param_t) {
    static_cast<Navi33Probe *>(self)->runAutostart();
}

void Navi33Probe::runAutostart() {
    namespace as=navi33::autostart;
    namespace em=navi33::embedded;
    auto *pci=OSDynamicCast(IOPCIDevice,getProvider());
    if(!pci || !pspLock_)return;
    struct Ctx { Navi33Probe *self; IOPCIDevice *pci; bool fwLatch2=false, fwLatch3=false; unsigned fwRuns=0; };
    Ctx ctx{this,pci};
    IOLog("Navi33Probe: autostart begins (embedded firmware: IPD %lu B, sOS %lu B, IP %lu B)\n",
          static_cast<unsigned long>(em::kIpdBytes),static_cast<unsigned long>(em::kSosBytes),
          static_cast<unsigned long>(em::kIpFwBytes));
    auto step=[](void *c,const as::Step &s,uint32_t *writes)->uint32_t {
        Ctx &x=*static_cast<Ctx *>(c);
        Navi33Probe *p=x.self;
        navi33::darwin_psp::Request req{};
        req.mode=s.mode;req.rom=em::kIpd;req.romBytes=em::kIpdBytes;
        static const uint8_t zero=0u;
        req.fw=s.payload==as::Payload::Sos ? em::kSos : s.payload==as::Payload::IpFw ? em::kIpFw : &zero;
        req.fwBytes=s.payload==as::Payload::Sos ? em::kSosBytes : s.payload==as::Payload::IpFw ?
            em::kIpFwBytes : 1u;
        req.writeGate=true;req.autoloadGate=s.autoload;
        req.stopAfter=s.stopAfter;req.startAt=s.startAt;
        fillConsole(req);
        switch(s.mode) {
        case N33_PSP_LOAD_RUN: req.attempted=&p->pspLoadAttempted_;break;
        case N33_PSP_LOAD_RING_RUN: req.attempted=&p->pspRingAttempted_;break;
        case N33_PSP_LOAD_FW_RUN:   // three record ranges: one provider latch, then two local ones
            req.attempted=x.fwRuns==0 ? &p->pspFwAttempted_ : x.fwRuns==1 ? &x.fwLatch2 : &x.fwLatch3;
            break;
        case N33_SMU_SCPM_RUN: req.attempted=&p->smuAttempted_;break;
        case N33_MES_START_RUN: req.attempted=&p->mesAttempted_;break;
        case N33_MEC_QUEUE_RUN: req.attempted=&p->mecAttempted_;break;
        default: return N33_PSPL_BAD_REQUEST;
        }
        IOLockLock(p->pspLock_);
        const bool before=*req.attempted;
        const n33_psp_load_result r=navi33::darwin_psp::execute(x.pci,req);
        IOLockUnlock(p->pspLock_);
        if(s.mode==N33_PSP_LOAD_FW_RUN && !before && *req.attempted)++x.fwRuns;
        *writes=r.register_writes;
        IOLog("Navi33Probe: autostart %s: state %u, %u register writes\n",s.name,r.state,r.register_writes);
        return r.state;
    };
    auto sleep=[](void *,uint32_t ms){IOSleep(ms);};
    const as::Report rep=as::run(&ctx,step,sleep);
    char text[256];int n=snprintf(text,sizeof(text),"%s reached %u/%u:",rep.done ? "DONE" : "STOPPED",
                                  rep.reached,as::kStepCount);
    for(unsigned i=0;i<as::kStepCount && n>0 && static_cast<size_t>(n)<sizeof(text);++i)
        if(rep.tries[i])n+=snprintf(text+n,sizeof(text)-n," %s=%u/%uw%s",as::kSteps[i].name,
                                    rep.state[i],rep.writes[i],rep.tries[i]>1u ? "+retry" : "");
    setProperty("Navi33,Autostart",text);
    IOLog("Navi33Probe: autostart %s\n",text);
}

// PLAN shares the hardware-preflight gate and only reads. RUN additionally
// needs navi33-psp-load=1 with no navi33-psp-off, and runs once per boot:
// a half-loaded PSP keeps its state until the card loses power.
n33_psp_load_result Navi33Probe::pspLoad(uint32_t mode,const uint8_t *romBytes,
    size_t romLength,const uint8_t *fwBytes,size_t fwLength,uint32_t stopAfter,
    uint32_t startAt) {
    n33_psp_load_result result{};
    result.abi_version=N33_ABI_VERSION;result.size=sizeof(result);
    uint32_t enabled=0u,off=0u,load=0u,ring=0u,fwOn=0u,autoOn=0u,pspOff=0u,smuOn=0u,smuOff=0u,
             mesOn=0u,mesOff=0u,mecOn=0u,mecOff=0u,gradOn=0u,gradOff=0u;
    const bool allowed=PE_parse_boot_argn("navi33-hw-probe",
                                         &enabled,sizeof(enabled)) && enabled==1u;
    const bool stopped=PE_parse_boot_argn("navi33-hw-off",
                                         &off,sizeof(off)) && off!=0u;
    if(!allowed || stopped || !pspLock_) {
        result.state=N33_PSPL_DISABLED;return result;
    }
    auto *pci=OSDynamicCast(IOPCIDevice,getProvider());
    if(!pci || !navi33::isSupported(navi33::inspectPci(
           pci->configRead32(kIOPCIConfigVendorID),
           pci->configRead32(kIOPCIConfigRevisionID)))) {
        result.state=N33_PSPL_NO_PROVIDER;return result;
    }
    navi33::darwin_psp::Request req{};
    req.mode=mode;req.rom=romBytes;req.romBytes=romLength;
    req.fw=fwBytes;req.fwBytes=fwLength;
    const bool ringMode=mode==N33_PSP_LOAD_RING_PLAN || mode==N33_PSP_LOAD_RING_RUN;
    const bool fwMode=mode==N33_PSP_LOAD_FW_PLAN || mode==N33_PSP_LOAD_FW_RUN;
    const bool smuMode=mode==N33_SMU_ENABLE_PLAN || mode==N33_SMU_ENABLE_RUN ||
        mode==N33_SMU_SCPM_PLAN || mode==N33_SMU_SCPM_RUN;
    const bool smuKilled=PE_parse_boot_argn("navi33-smu-off",&smuOff,sizeof(smuOff)) && smuOff!=0u;
    const bool mesMode=mode==N33_MES_START_PLAN || mode==N33_MES_START_RUN;
    const bool mesKilled=PE_parse_boot_argn("navi33-mes-off",&mesOff,sizeof(mesOff)) && mesOff!=0u;
    const bool mecMode=mode==N33_MEC_QUEUE_PLAN || mode==N33_MEC_QUEUE_RUN;
    const bool mecKilled=PE_parse_boot_argn("navi33-mec-off",&mecOff,sizeof(mecOff)) && mecOff!=0u;
    const bool gradMode=mode==N33_GRADIENT_PLAN || mode==N33_GRADIENT_RUN;
    const bool gradKilled=PE_parse_boot_argn("navi33-gradient-off",&gradOff,sizeof(gradOff)) && gradOff!=0u;
    const bool killed=PE_parse_boot_argn("navi33-psp-off",&pspOff,
                                         sizeof(pspOff)) && pspOff!=0u;
    // Each write stage has its own boot-arg; navi33-psp-off kills all of them.
    req.writeGate=!killed && (ringMode ?
        (PE_parse_boot_argn("navi33-psp-ring",&ring,sizeof(ring)) && ring==1u) :
        fwMode ?
        (PE_parse_boot_argn("navi33-psp-fw",&fwOn,sizeof(fwOn)) && fwOn==1u) :
        (PE_parse_boot_argn("navi33-psp-load",&load,sizeof(load)) && load==1u));
    if(smuMode)req.writeGate=!killed && !smuKilled &&
        PE_parse_boot_argn("navi33-smu-enable",&smuOn,sizeof(smuOn)) && smuOn==1u;
    if(smuMode) {
        uint32_t scpm=0u;
        const bool scpmGate=PE_parse_boot_argn("navi33-smu-scpm",&scpm,sizeof(scpm)) && scpm==1u;
        const bool scpmMode=mode==N33_SMU_SCPM_PLAN || mode==N33_SMU_SCPM_RUN;
        // Opening the SCPM experiment must never open the legacy mask sequence.
        req.writeGate=req.writeGate && (scpmMode ? scpmGate : !scpmGate);
    }
    // MES start: its own boot-arg; navi33-mes-off and navi33-psp-off both stop it.
    if(mesMode)req.writeGate=!killed && !mesKilled &&
        PE_parse_boot_argn("navi33-mes-start",&mesOn,sizeof(mesOn)) && mesOn==1u;
    // MEC queue: its own boot-arg; navi33-mec-off and navi33-psp-off both stop it.
    if(mecMode)req.writeGate=!killed && !mecKilled &&
        PE_parse_boot_argn("navi33-mec-queue",&mecOn,sizeof(mecOn)) && mecOn==1u;
    // Gradient: its own boot-arg; navi33-gradient-off and navi33-psp-off both stop it.
    if(gradMode)req.writeGate=!killed && !gradKilled &&
        PE_parse_boot_argn("navi33-gradient",&gradOn,sizeof(gradOn)) && gradOn==1u;
    // AUTOLOAD_RLC powers up the graphics engine: a second, separate boot-arg.
    req.autoloadGate=!killed && fwMode &&
        PE_parse_boot_argn("navi33-psp-autoload",&autoOn,sizeof(autoOn)) && autoOn==1u;
    req.stopAfter=stopAfter;req.startAt=startAt;
    fillConsole(req);
    IOLockLock(pspLock_);
    req.attempted=ringMode ? &pspRingAttempted_ : fwMode ? &pspFwAttempted_ :
                             &pspLoadAttempted_;
    if(smuMode)req.attempted=&smuAttempted_;
    if(mesMode)req.attempted=&mesAttempted_;
    if(mecMode)req.attempted=&mecAttempted_;
    if(gradMode)req.attempted=&gradAttempted_;
    result=navi33::darwin_psp::execute(pci,req);
    IOLockUnlock(pspLock_);
    char raw[320];
    snprintf(raw,sizeof(raw),
             "mode %u state %u step %u steps %u writes %u flags 0x%x "
             "C2PMSG_35 %08x->%08x C2PMSG_81 %08x->%08x sOS %08x fw_pri MC %llx "
             "console %s ring 0x%llx tmr 0x%llx/0x%x toc %u cmd 0x%x fence %u rflags 0x%x "
             "fw last %u/%u addr 0x%llx",
             mode,result.state,result.step,result.steps_done,
             result.register_writes,result.flags,result.c2pmsg35_before,
             result.c2pmsg35_last,result.c2pmsg81_before,result.c2pmsg81_after,
             result.sos_version_after,result.fw_pri_mc,
             req.consolePhys ? "boot" : (result.console_phys ? "host" : "none"),
             result.ring_mc,result.tmr_mc,result.tmr_bytes,result.toc_bytes,
             result.cmd_status,result.fence_seen,result.ring_flags,
             result.fw_last_record,result.fw_record_count,result.fw_tmr_addr);
    if(smuMode)snprintf(raw,sizeof(raw),
        "mode %u state %u mask %08x:%08x steps %u writes %u replies %x/%x/%x "
        "core %x->%x reset %x->%x RLC %x->%x",mode,result.state,
        result.smu_mask_high,result.smu_mask_low,result.steps_done,result.register_writes,
        result.smu_responses[0],result.smu_responses[1],result.smu_responses[2],
        result.smu_core_before,result.smu_core_after,result.smu_reset_before,
        result.smu_reset_after,result.smu_rlc_before,result.smu_rlc_after);
    if(mesMode)snprintf(raw,sizeof(raw),
        "mode %u state %u steps %u writes %u cntl %x->%x grbm %x RLC %x CP_STAT %x "
        "version %x/%x pc %x/%x samples %u",mode,result.state,result.steps_done,
        result.register_writes,result.mes_cntl_before,result.mes_cntl_after,
        result.mes_grbm_before,result.mes_rlc,result.mes_cp_stat,result.mes_version[0],
        result.mes_version[1],result.mes_pc[0],result.mes_pc[1],result.mes_samples);
    if(mecMode)snprintf(raw,sizeof(raw),
        "mode %u state %u steps %u writes %u cntl %x->%x RLC %x scratch %x fence %x rptr %x "
        "active %x->%x window 0x%llx",mode,result.state,result.steps_done,result.register_writes,
        result.mec_cntl_before,result.mec_cntl_after,result.mec_rlc,result.mec_scratch,
        result.mec_fence,result.mec_rptr,result.mec_hqd_active_before,
        result.mec_hqd_active_after,result.mec_window_mc);
    if(gradMode)snprintf(raw,sizeof(raw),
        "mode %u state %u steps %u writes %u SH_MEM %x rptr %u->%u fences %x/%x bad %u/%u "
        "fallbacks %u pixel(1,1) %08x screen %x",mode,result.state,result.steps_done,
        result.register_writes,result.grad_shmem_before,result.grad_rptr_before,
        result.grad_rptr_after,result.grad_fence_a,result.grad_fence_b,result.grad_bad_pixels,
        result.grad_screen_bad,result.grad_fallbacks,result.grad_first_pixel,result.grad_screen);
    setProperty(mode==N33_GRADIENT_RUN ? "Navi33,LastGradient" :
                mode==N33_GRADIENT_PLAN ? "Navi33,LastGradientPlan" :
                mode==N33_MEC_QUEUE_RUN ? "Navi33,LastMecQueue" :
                mode==N33_MEC_QUEUE_PLAN ? "Navi33,LastMecPlan" :
                mode==N33_MES_START_RUN ? "Navi33,LastMesStart" :
                mode==N33_MES_START_PLAN ? "Navi33,LastMesPlan" :
                (mode==N33_SMU_ENABLE_RUN || mode==N33_SMU_SCPM_RUN) ? "Navi33,LastSmuEnable" :
                (mode==N33_SMU_ENABLE_PLAN || mode==N33_SMU_SCPM_PLAN) ? "Navi33,LastSmuPlan" :
                mode==N33_PSP_LOAD_RUN ? "Navi33,LastPspLoad" :
                mode==N33_PSP_LOAD_RING_RUN ? "Navi33,LastPspRing" :
                mode==N33_PSP_LOAD_RING_PLAN ? "Navi33,LastPspRingPlan" :
                mode==N33_PSP_LOAD_FW_RUN ? "Navi33,LastPspFw" :
                mode==N33_PSP_LOAD_FW_PLAN ? "Navi33,LastPspFwPlan" :
                                               "Navi33,LastPspPlan",raw);
    return result;
}

// Live read-only MMHUB/GFXHUB FB registers; this does not claim VRAM,
// change PCI configuration, program GART/VMID, or write the graphics card.
n33_vram_geometry_status Navi33Probe::vramGeometryStatus() {
    navi33::vram_live::Sample sample{};
    uint32_t enabled=0u,kill=0u;
    const bool on=PE_parse_boot_argn("navi33-vram-read",
                                    &enabled,sizeof(enabled)) &&
                  enabled==1u;
    const bool off=PE_parse_boot_argn("navi33-vram-off",
                                     &kill,sizeof(kill)) && kill!=0u;
    if(!on || off)
        return navi33::vram_live::report(sample,0u,0u,0xffffffffu);
    auto *pci=OSDynamicCast(IOPCIDevice,getProvider());
    if(!pci) {
        sample.state=N33_VRAM_NO_BAR5;
        return navi33::vram_live::report(sample,0u,0u,0xffffffffu);
    }
    const auto id=navi33::inspectPci(
        pci->configRead32(kIOPCIConfigVendorID),
        pci->configRead32(kIOPCIConfigRevisionID));
    if(!navi33::isSupported(id)) {
        sample.state=N33_VRAM_BAD_DISCOVERY;
        return navi33::vram_live::report(sample,0u,0u,0xffffffffu);
    }
    auto *blob=OSDynamicCast(OSData,pci->getProperty("ATY,bin_image"));
    if(!blob || blob->getLength()<64u ||
       blob->getLength()>navi33::IPDiscovery::kMaxBlob) {
        sample.state=N33_VRAM_NO_ROM;
        return navi33::vram_live::report(sample,0u,0u,0xffffffffu);
    }
    navi33::IPDiscovery disc;
    if(!disc.parse(static_cast<const uint8_t *>(blob->getBytesNoCopy()),
                   blob->getLength()) ||
       navi33::inventory(disc).code!=navi33::InventoryCode::Pass) {
        sample.state=N33_VRAM_BAD_DISCOVERY;
        return navi33::vram_live::report(sample,0u,0u,0xffffffffu);
    }
    IODeviceMemory *bar5=pci->getDeviceMemoryWithRegister(
        kIOPCIConfigBaseAddress5);
    IODeviceMemory *bar0=pci->getDeviceMemoryWithRegister(
        kIOPCIConfigBaseAddress0);
    const uint64_t bar5Bytes=bar5?static_cast<uint64_t>(bar5->getLength()):0u;
    const uint64_t bar0Bytes=bar0?static_cast<uint64_t>(bar0->getLength()):0u;
    if(!bar5 || bar5Bytes<4u) {
        sample.state=N33_VRAM_NO_BAR5;
        return navi33::vram_live::report(sample,bar0Bytes,bar5Bytes,0xffffffffu);
    }
    const auto fbPlan=navi33::vram_live::fromDiscovery(disc,bar5Bytes);
    const auto memPlan=navi33::nbio4::fromDiscovery(disc,bar5Bytes);
    navi33::nbio4::Sample memory{};
    memory.code=memPlan.code;
    memory.version=memPlan.nbioRevision;
    if(fbPlan.code!=navi33::vram_live::PlanCode::Pass)
        sample.state=N33_VRAM_BAD_OFFSETS;

    // One opt-in BAR5 mapping permits independent, bounded FB and NBIO
    // snapshots. It is never used to write registers or VRAM.
    IOMemoryMap *mapped=bar5->map();
    if(!mapped) {
        if(fbPlan.code==navi33::vram_live::PlanCode::Pass)
            sample.state=N33_VRAM_MAP_FAILED;
        if(memPlan.code==navi33::nbio4::Code::Pass)
            memory.code=navi33::nbio4::Code::ReadFailed;
    } else {
        if(static_cast<uint64_t>(mapped->getLength())<bar5Bytes) {
            if(fbPlan.code==navi33::vram_live::PlanCode::Pass)
                sample.state=N33_VRAM_MAP_FAILED;
            if(memPlan.code==navi33::nbio4::Code::Pass)
                memory.code=navi33::nbio4::Code::ReadFailed;
        } else {
            if(fbPlan.code==navi33::vram_live::PlanCode::Pass)
                sample=navi33::vram_live::inspect(
                    fbPlan,navi33ReadStatusDword,mapped);
            memory=navi33::nbio4::inspect(
                memPlan,navi33ReadStatusDword,mapped);
        }
        mapped->release();
    }
    const auto output=navi33::vram_survey::combine(
        navi33::vram_live::report(
            sample,bar0Bytes,bar5Bytes,fbPlan.mmRevision),memory);
    IOLog("Navi33Probe: read-only FB state %u; NBIO mem state %u, capacity %u MiB; no GPU writes\n",
          output.state,output.nbio_state,output.nbio_memsize_mib);
    return output;
}

uint64_t Navi33Probe::claimStageOwner(const void *client) {
    if(!stageOwnerLock_ || !client)return 0u;
    IOLockLock(stageOwnerLock_);
    const auto token=stageOwner_.claim(client);
    IOLockUnlock(stageOwnerLock_);
    return token.epoch;
}

void Navi33Probe::releaseStageOwner(const void *client,uint64_t epoch) {
    if(!stageOwnerLock_ || !client || !epoch)return;
    IOLockLock(stageOwnerLock_);
    (void)stageOwner_.release(client,{epoch});
    IOLockUnlock(stageOwnerLock_);
}

// The owner probe's inputs and verdict as ioreg properties: a results file keeps
// them even after the kernel message buffer has rolled past the boot IOLog line.
void Navi33Probe::publishOwnerReport(const navi33::darwin_owner::ProbeReport &r) {
    const uint32_t bits=navi33::pci_owner::evidenceBits(r.evidence);
    setProperty("Navi33,OwnerDecision", static_cast<uint64_t>(r.decision.code), 8);
    setProperty("Navi33,OwnerRefusalMask", static_cast<uint64_t>(r.decision.refusalMask), 32);
    setProperty("Navi33,OwnerEvidence", static_cast<uint64_t>(bits), 32);
    setProperty("Navi33,OwnerDmaDomain", r.dmaDomain, 64);
    setProperty("Navi33,OwnerDmaMode", static_cast<uint64_t>(r.decision.dmaMode), 8);
    char text[160];
    snprintf(text,sizeof(text),
             "code %u mask 0x%x gate %d kill %d identity %d open %d display %d mapper %d recovery %d domain 0x%llx",
             static_cast<unsigned>(r.decision.code), r.decision.refusalMask,
             int(r.evidence.gateOn), int(r.evidence.killOn), int(r.evidence.identityOk),
             int(r.evidence.openSucceeded), int(r.evidence.displayClientAttached),
             int(r.evidence.dmaMapperPresent), int(r.evidence.recoveryAttested),
             static_cast<unsigned long long>(r.dmaDomain));
    setProperty("Navi33,OwnerProbe", text);
}

void Navi33Probe::releaseHeldOwner(IOService *provider) {
    if(!pciOwnerTicket_)
        return;
    navi33::darwin_owner::releaseOwner(OSDynamicCast(IOPCIDevice, provider),this,pciOwner_,pciOwnerTicket_);
    setProperty("Navi33,OwnerHeld", static_cast<uint64_t>(0), 8);
}

void Navi33Probe::systemWillShutdown(IOOptionBits specifier) {
    releaseHeldOwner(getProvider());
    super::systemWillShutdown(specifier);
}

bool Navi33Probe::willTerminate(IOService *provider, IOOptionBits options) {
    releaseHeldOwner(provider);
    return super::willTerminate(provider, options);
}

void Navi33Probe::stop(IOService *provider) {
    if(autostartCall_) {   // never tear down under a running bring-up
        thread_call_cancel_wait(autostartCall_);
        thread_call_free(autostartCall_);
        autostartCall_=nullptr;
    }
    releaseHeldOwner(provider);
    if(stageOwnerLock_) {
        IOLockLock(stageOwnerLock_);
        stageOwner_.shutdown();
        IOLockUnlock(stageOwnerLock_);
    }
    super::stop(provider);
}

void Navi33Probe::free() {
    if(stageOwnerLock_) {
        IOLockFree(stageOwnerLock_);
        stageOwnerLock_=nullptr;
    }
    if(pspLock_) {
        IOLockFree(pspLock_);
        pspLock_=nullptr;
    }
    super::free();
}

// Diagnostic N33N path is explicitly gated and root-only, independent of the
// display/MMIO boot arguments. Disabled by default; never arms a GPU.
IOReturn Navi33Probe::newUserClient(task_t owningTask, void *securityID,
                                    UInt32 type, OSDictionary *properties,
                                    IOUserClient **handler) {
    if (!handler) return kIOReturnBadArgument;
    *handler = nullptr;
    uint32_t enabled = 0u, off = 0u, probeEnabled = 0u;
    const bool probeOn = PE_parse_boot_argn("navi33-probe",
                         &probeEnabled, sizeof(probeEnabled)) && probeEnabled == 1u;
    const bool clientOn = PE_parse_boot_argn("navi33-client",
                         &enabled, sizeof(enabled)) && enabled == 1u;
    const bool kill = PE_parse_boot_argn("navi33-client-off",
                         &off, sizeof(off)) && off != 0u;
    if (navi33::native_policy::openAllowed(type, probeOn, clientOn, kill, true)
            != navi33::native_policy::Access::Allow)
        return kIOReturnUnsupported;

    auto *client = OSTypeAlloc(Navi33NativeClient);
    if (!client) return kIOReturnNoMemory;
    if (!client->initWithTask(owningTask, securityID, type, properties)) {
        client->release();
        return kIOReturnNotPrivileged;
    }
    if (!client->attach(this)) {
        client->release();
        return kIOReturnInternalError;
    }
    if (!client->start(this)) {
        client->detach(this);
        client->release();
        return kIOReturnInternalError;
    }
    *handler = client;
    return kIOReturnSuccess;
}
