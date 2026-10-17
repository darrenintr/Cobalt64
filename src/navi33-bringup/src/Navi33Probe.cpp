// Navi33Probe - explicitly opted-in, PCI-configuration-only inventory.
// No BAR mapping, MMIO, framebuffer takeover, PSP/SMU commands or GPU writes.
#include <IOKit/IOService.h>
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
#include <libkern/c++/OSData.h>

class Navi33Probe : public IOService {
    OSDeclareDefaultStructors(Navi33Probe)
    using super = IOService;

public:
    IOService *probe(IOService *provider, SInt32 *score) override;
    bool start(IOService *provider) override;
    void stop(IOService *provider) override;
};

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

    setProperty("Navi33,Stage", "PCI inventory and supplied-ROM IP discovery only");
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

void Navi33Probe::stop(IOService *provider) {
    super::stop(provider);
}
