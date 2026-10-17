// Navi33Probe - explicitly opted-in, PCI-configuration-only inventory.
// No BAR mapping, MMIO, framebuffer takeover, PSP/SMU commands or GPU writes.
#include "Navi33Probe.hpp"
#include "Navi33NativeClient.hpp"
#include "Navi33NativePolicy.h"
#include <IOKit/IOLib.h>
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
#include "Navi33Sdma6Status.h"
#include "Navi33HwProbeLogic.h"
#include <libkern/c++/OSData.h>




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
    setProperty("Navi33,GpuWritesEnabled", static_cast<uint64_t>(0), 32);
    setProperty("Navi33,FirmwareLoadingImplemented", static_cast<uint64_t>(0), 8);
    setProperty("Navi33,GpuQueuesImplemented", static_cast<uint64_t>(0), 8);
    setProperty("Navi33,DcnModeSettingImplemented", static_cast<uint64_t>(0), 8);
    // Software page-table and ring models compile into this Kext for shared
    // type-checking, but they do not map DMA or submit to a physical GPU.
    setProperty("Navi33,Gmc11GartHardwareReady", static_cast<uint64_t>(0), 8);
    setProperty("Navi33,SDMA6HardwareRingReady", static_cast<uint64_t>(0), 8);
    setProperty("Navi33,TLBInvalidationEnabled", static_cast<uint64_t>(0), 8);
    setProperty("Navi33,SDMA6DoorbellEnabled", static_cast<uint64_t>(0), 8);
    setProperty("Navi33,DmaPreflightExecuted", static_cast<uint64_t>(0), 8);
    // Separate explicit one-shot preflight. Default boot has no IOMMU mapping
    // allocations. The navi33-dma-off kill switch always wins.
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
        IOLog("Navi33Probe: optional IOMMU staging returned %u (%u segments, %u pages), no GPU DMA\n",
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
    uint32_t mmContext=UINT32_MAX,gfxContext=UINT32_MAX;
    const bool mmOk=navi33ReadStatusDword(mapped,hubs.mmhub,&mmContext);
    const bool gfxOk=navi33ReadStatusDword(mapped,hubs.gfxhub,&gfxContext);
    mapped->release();
    if(!mmOk || !gfxOk)
        return navi33::hwprobe::blank(N33_HW_MMIO_READ_FAILED);

    const n33_hw_preflight result=navi33::hwprobe::classify(
        navi33::psp13::report(psp),navi33::sdma6live::report(sdma),
        mmContext,gfxContext,bytes);
    // These two facts are not optional: a status read doesn't demonstrate
    // exclusive queue ownership or any working compute command path.
    IOLog("Navi33Probe: RX7600 physical HW preflight reason %u, reads %u, PSP %u, SDMA %u; no GPU copy submitted\n",
          result.reason,result.mmio_reads,result.psp_state,result.sdma_state);
    return result;
}

void Navi33Probe::stop(IOService *provider) {
    super::stop(provider);
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
