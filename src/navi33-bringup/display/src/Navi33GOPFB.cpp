// Navi33GOPFB - firmware scanout handoff to macOS IOGraphicsFamily.
// BSD-3-Clause concept attribution: see ../LICENSE-RDNA4FB.txt.
// Works only if the UEFI GOP display engine remains programmed and live.
#include "Navi33GOPFB.hpp"
#include "Navi33BarGuard.h"
#include "../../src/Navi33Identity.h"
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceMemory.h>

#define N33FB_LOG(fmt, ...) IOLog("Navi33GOPFB: " fmt "\n", ##__VA_ARGS__)

OSDefineMetaClassAndStructors(Navi33GOPFB, IOFramebuffer)

IOService *Navi33GOPFB::probe(IOService *provider, SInt32 *score) {
    uint32_t enable = 0, off = 0;
    if (PE_parse_boot_argn("navi33-fb-off", &off, sizeof(off)) && off != 0u)
        return nullptr;
    if (!PE_parse_boot_argn("navi33-fb", &enable, sizeof(enable)) || enable != 1u)
        return nullptr;
    if (!super::probe(provider, score))
        return nullptr;
    auto *pci = OSDynamicCast(IOPCIDevice, provider);
    if (!pci)
        return nullptr;
    const auto id = navi33::inspectPci(
        pci->configRead32(kIOPCIConfigVendorID),
        pci->configRead32(kIOPCIConfigRevisionID));
    if (!navi33::isSupported(id))
        return nullptr;
    if (score) *score += 20000;
    return this;
}

bool Navi33GOPFB::captureConsole() {
    IOPlatformExpert *platform = getPlatform();
    if (!platform) return false;
    PE_Video video {};
    if (platform->getConsoleInfo(&video) != kIOReturnSuccess) {
        N33FB_LOG("getConsoleInfo refused: no usable GOP handoff");
        return false;
    }
    auto d = navi33fb::inspectGop(static_cast<uint64_t>(video.v_baseAddr),
                                 static_cast<uint64_t>(video.v_length),
                                 static_cast<uint32_t>(video.v_width),
                                 static_cast<uint32_t>(video.v_height),
                                 static_cast<uint32_t>(video.v_rowBytes),
                                 static_cast<uint32_t>(video.v_depth));
    if (!navi33fb::valid(d)) {
        N33FB_LOG("GOP geometry refused: reason %u, raw base 0x%llx, %ux%u, row %u, depth %u",
                  static_cast<unsigned>(d.error),
                  static_cast<unsigned long long>(video.v_baseAddr),
                  d.width, d.height, d.rowBytes, d.depth);
        return false;
    }
    // Read IOPCIFamily's assigned BAR0 *resource metadata*. Do not map BAR0
    // or enable PCI memory decode. Reject any firmware address that points
    // outside this GPU's advertised framebuffer aperture.
    if (!pci_) return false;
    IODeviceMemory *bar0 = pci_->getDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0);
    const uint64_t barBase = bar0 ? static_cast<uint64_t>(bar0->getPhysicalAddress()) : 0u;
    const uint64_t barSize = bar0 ? static_cast<uint64_t>(bar0->getLength()) : 0u;
    const auto ownership = navi33fb::checkBar0(d, barBase, barSize);
    if (!navi33fb::ownedByBar0(ownership)) {
        N33FB_LOG("GOP scanout refused: BAR0 ownership check %u (base 0x%llx, span 0x%llx)",
                  static_cast<unsigned>(ownership.status),
                  static_cast<unsigned long long>(barBase),
                  static_cast<unsigned long long>(barSize));
        return false;
    }

    console_ = d;
    setProperty("Navi33FB,Stage", "GOP firmware scanout only");
    setProperty("Navi33FB,BAR0Base", barBase, 64);
    setProperty("Navi33FB,BAR0Bytes", barSize, 64);
    setProperty("Navi33FB,ScanoutBAR0Offset", ownership.offset, 64);
    setProperty("Navi33FB,Base", d.physicalBase, 64);
    setProperty("Navi33FB,Length", d.length, 64);
    setProperty("Navi33FB,Width", static_cast<uint64_t>(d.width), 32);
    setProperty("Navi33FB,Height", static_cast<uint64_t>(d.height), 32);
    setProperty("Navi33FB,RowBytes", static_cast<uint64_t>(d.rowBytes), 32);
    setProperty("Navi33FB,Depth", static_cast<uint64_t>(d.depth), 32);
    N33FB_LOG("firmware scanout physical base 0x%llx, %ux%u, pitch %u",
              static_cast<unsigned long long>(d.physicalBase),
              d.width, d.height, d.rowBytes);
    return true;
}

bool Navi33GOPFB::start(IOService *provider) {
    uint32_t enable = 0, off = 0;
    if (PE_parse_boot_argn("navi33-fb-off", &off, sizeof(off)) && off != 0u)
        return false;
    if (!PE_parse_boot_argn("navi33-fb", &enable, sizeof(enable)) || enable != 1u)
        return false;
    pci_ = OSDynamicCast(IOPCIDevice, provider);
    if (!pci_) return false;
    const auto id = navi33::inspectPci(
        pci_->configRead32(kIOPCIConfigVendorID),
        pci_->configRead32(kIOPCIConfigRevisionID));
    if (!navi33::isSupported(id)) return false;

    // The firmware's video memory decode must still be on. Do not change
    // PCI COMMAND, BAR assignments, power state or any GPU registers here.
    constexpr uint16_t kPciMemorySpaceEnable = 0x0002u;
    if ((pci_->configRead16(0x04u) & kPciMemorySpaceEnable) == 0u) {
        N33FB_LOG("PCI memory decoding off; refusing without reprogramming");
        return false;
    }
    if (!captureConsole()) return false;

    uint32_t cmap = 0;
    if (PE_parse_boot_argn("navi33-fb-cmap", &cmap, sizeof(cmap)) && cmap <= 5u)
        colorMap_ = cmap;

    // The superclass publishes the IOFramebuffer for IOGraphics.
    if (!super::start(provider)) return false;
    setProperty("Navi33FB,GpuCommandSubmission", static_cast<uint64_t>(0), 32);
    setProperty("Navi33FB,HardwareWrites", static_cast<uint64_t>(0), 32);
    N33FB_LOG("started (software pixels via GOP; no registers/modeset)");
    return true;
}

void Navi33GOPFB::stop(IOService *provider) {
    super::stop(provider);
}

IOReturn Navi33GOPFB::enableController() {
    if (!navi33fb::valid(console_) && !captureConsole())
        return kIOReturnNoResources;
    return kIOReturnSuccess;
}

bool Navi33GOPFB::isConsoleDevice() { return true; }

IODeviceMemory *Navi33GOPFB::getApertureRange(IOPixelAperture aperture) {
    // IOGraphics may ask for the aperture during super::start(): do not
    // gate it on a post-start flag when GOP geometry is already validated.
    if (aperture != kIOFBSystemAperture || !navi33fb::valid(console_))
        return nullptr;
    // This is the existing GOP pixel buffer, not an MMIO register mapping.
    return IODeviceMemory::withRange(console_.physicalBase, console_.length);
}

IODeviceMemory *Navi33GOPFB::getVRAMRange() {
    if (!navi33fb::valid(console_)) return nullptr;
    return IODeviceMemory::withRange(console_.physicalBase, console_.length);
}

const char *Navi33GOPFB::getPixelFormats() {
    static const char formats[] = IO32BitDirectPixels "\0";
    return formats;
}

IOItemCount Navi33GOPFB::getDisplayModeCount() { return 1; }
IOReturn Navi33GOPFB::getDisplayModes(IODisplayModeID *modes) {
    if (!modes) return kIOReturnBadArgument;
    modes[0] = kFirmwareMode;
    return kIOReturnSuccess;
}
IOReturn Navi33GOPFB::getInformationForDisplayMode(IODisplayModeID mode,
                                                  IODisplayModeInformation *info) {
    if (!info || mode != kFirmwareMode || !navi33fb::valid(console_))
        return kIOReturnBadArgument;
    bzero(info, sizeof(*info));
    info->nominalWidth = console_.width;
    info->nominalHeight = console_.height;
    info->refreshRate = 60u << 16u; // API hint only: actual GOP refresh unknown
    info->maxDepthIndex = 0;
    info->flags = kDisplayModeValidFlag | kDisplayModeSafeFlag | kDisplayModeDefaultFlag;
    return kIOReturnSuccess;
}
UInt64 Navi33GOPFB::getPixelFormatsForDisplayMode(IODisplayModeID, IOIndex) {
    return 0; // Obsolete API contract.
}
IOReturn Navi33GOPFB::getPixelInformation(IODisplayModeID mode, IOIndex depth,
                                         IOPixelAperture aperture, IOPixelInformation *info) {
    if (!info || mode != kFirmwareMode || depth != 0 || aperture != kIOFBSystemAperture ||
        !navi33fb::valid(console_))
        return kIOReturnBadArgument;
    bzero(info, sizeof(*info));
    info->bytesPerRow = console_.rowBytes;
    info->bitsPerPixel = 32;
    info->pixelType = kIORGBDirectPixels;
    info->componentCount = 3;
    info->bitsPerComponent = 8;
    const navi33fb::Channels c = navi33fb::channelMapping(colorMap_);
    info->componentMasks[0] = navi33fb::channelMask(c.red);
    info->componentMasks[1] = navi33fb::channelMask(c.green);
    info->componentMasks[2] = navi33fb::channelMask(c.blue);

    char letters[4] = {'-', '-', '-', '-'};
    letters[c.red] = 'R'; letters[c.green] = 'G'; letters[c.blue] = 'B';
    char *p = info->pixelFormat;
    for (int byte = 3; byte >= 0; --byte) {
        for (int bit = 0; bit < 8; ++bit) *p++ = letters[byte];
    }
    *p = '\0';
    info->activeWidth = console_.width;
    info->activeHeight = console_.height;
    return kIOReturnSuccess;
}

IOReturn Navi33GOPFB::getCurrentDisplayMode(IODisplayModeID *mode, IOIndex *depth) {
    if (mode) *mode = kFirmwareMode;
    if (depth) *depth = 0;
    return kIOReturnSuccess;
}
IOReturn Navi33GOPFB::setDisplayMode(IODisplayModeID mode, IOIndex depth) {
    return mode == kFirmwareMode && depth == 0 ? kIOReturnSuccess : kIOReturnUnsupported;
}
IOReturn Navi33GOPFB::getAttribute(IOSelect attribute, uintptr_t *value) {
    if (attribute == kIOHardwareCursorAttribute) {
        if (value) *value = 0; // software cursor, don't touch DCN
        return kIOReturnSuccess;
    }
    return super::getAttribute(attribute, value);
}
IOReturn Navi33GOPFB::setAttribute(IOSelect attribute, uintptr_t value) {
    if (attribute == kIOPowerAttribute) {
        // Leave the firmware output alone: this pilot cannot reinitialize
        // DCN3.2 after a power down. This is not full sleep/wake support.
        return kIOReturnSuccess;
    }
    return super::setAttribute(attribute, value);
}
IOItemCount Navi33GOPFB::getConnectionCount() { return 1; }
IOReturn Navi33GOPFB::getAttributeForConnection(IOIndex index, IOSelect attr,
                                                uintptr_t *value) {
    if (index != 0) return kIOReturnBadArgument;
    switch (attr) {
    case kConnectionEnable:
    case kConnectionCheckEnable:
        if (value) *value = 1; return kIOReturnSuccess;
    case kConnectionFlags:
        if (value) *value = 0; return kIOReturnSuccess;
    default: return super::getAttributeForConnection(index, attr, value);
    }
}
IOReturn Navi33GOPFB::setAttributeForConnection(IOIndex index, IOSelect attr,
                                                uintptr_t value) {
    if (index != 0) return kIOReturnBadArgument;
    if (attr == kConnectionPower)
        return kIOReturnSuccess; // no physical display power operations
    return super::setAttributeForConnection(index, attr, value);
}
