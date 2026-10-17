// Navi33Probe - explicitly opted-in, PCI-configuration-only inventory.
// No BAR mapping, MMIO, framebuffer takeover, PSP/SMU commands or GPU writes.
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <pexpert/pexpert.h>
#include "Navi33Identity.h"
#include "Navi33Discovery.h"
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
            const bool candidate = disc.isNavi33Candidate();
            setProperty("Navi33,VersionsMatch", static_cast<uint64_t>(candidate), 8);
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
            IOLog("Navi33Probe: supplied IP snapshot count %u, Navi33 versions %s; no HW writes\n",
                  disc.count(), candidate ? "MATCH" : "MISMATCH");
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
