// Navi33GOPFB - firmware scanout handoff to macOS IOGraphicsFamily.
// BSD-3-Clause concept attribution: see ../LICENSE-RDNA4FB.txt.
// Works only if the UEFI GOP display engine remains programmed and live.
#include "Navi33GOPFB.hpp"
#include "Navi33BarGuard.h"
#include "Navi33EDID.h"
#include "Navi33Pattern.h"
#include <IOKit/IOMemoryDescriptor.h>
#include <libkern/c++/OSData.h>
#include "../../src/Navi33Identity.h"
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/pwr_mgt/IOPM.h>

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
    setProperty("Navi33FB,FirmwareDepth", static_cast<uint64_t>(d.firmwareDepth), 32);
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

    // Load optional OpenCore-injected EDID before IOGraphics attaches an
    // IODisplay. No DP AUX/I2C transactions or MMIO read occurs.
    readInjectedEDID();
    uint32_t draw=0u;
    if(PE_parse_boot_argn("navi33-fb-pattern",&draw,sizeof(draw)) &&
       draw==1u) {
        const bool drawn=drawTestPattern();
        setProperty("Navi33FB,PatternWriteSucceeded",
                    static_cast<uint64_t>(drawn),8);
    }
    pci_->setProperty("model", "AMD Radeon RX 7600 (GOP fallback)");
    pci_->setProperty("device_type", "display");

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

void Navi33GOPFB::initializePowerManagement() {
    if (pmInitialized_) return;
    // IOGraphicsFamily registers power states during enableController(),
    // not start(): calling this before IOFramebuffer has joined the power
    // tree can hang during driver startup. Use the RDNA4FB/IONDRV pattern.
    static IOPMPowerState powerStates[3] = {
        { 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
        { 1, 0, 0, kIOPMPowerOn, 0, 0, 0, 0, 0, 0, 0, 0 },
        { 1, kIOPMDeviceUsable, kIOPMPowerOn, kIOPMPowerOn, 0, 0, 0, 0, 0, 0, 0, 0 },
    };
    // A GOP-only driver cannot resume a reset/power-gated DCN3.2 display.
    // Refuse full system sleep rather than destroying the last image.
    for (auto &state : powerStates)
        state.capabilityFlags |= kIOPMPreventSystemSleep;
    pmInitialized_ = true;
    registerPowerDriver(this, powerStates, 3);
    temporaryPowerClampOn();
    changePowerStateTo(1);
    N33FB_LOG("power: registered doze-only policy; system sleep disallowed");
}

IOReturn Navi33GOPFB::enableController() {
    if (!navi33fb::valid(console_) && !captureConsole())
        return kIOReturnNoResources;
    initializePowerManagement();
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

IOReturn Navi33GOPFB::setStartupDisplayMode(IODisplayModeID mode, IOIndex depth) {
    // Never reprogram DCN 3.2.1 or ask the firmware for a different mode.
    return setDisplayMode(mode, depth);
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
    case kConnectionSupportsHLDDCSense:
        // OSDisplay can query the injected EDID through our DDC methods.
        return edidBytes_ != 0u ? kIOReturnSuccess : kIOReturnUnsupported;
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


// ---------------------------------------------------------------------------
// Optional passive DDC/EDID service (no GPU or physical monitor I/O)
// ---------------------------------------------------------------------------
bool Navi33GOPFB::readInjectedEDID() {
    edidBytes_=0u;
    if(!pci_)return false;
    // Dedicated property name avoids binding to undocumented Apple defaults.
    const OSData *blob=OSDynamicCast(OSData,pci_->getProperty("Navi33,EDID"));
    if(!blob){
        setProperty("Navi33FB,EDIDStatus","Not injected (generic display fallback)");
        return false;
    }
    const size_t length=blob->getLength();
    if(!navi33fb::edid::inspect(
          static_cast<const uint8_t*>(blob->getBytesNoCopy()),length).valid()){
        setProperty("Navi33FB,EDIDStatus","Injected EDID refused (checksum/size/header)");
        N33FB_LOG("EDID injection invalid: length %zu; continuing without EDID",length);
        return false;
    }
    memcpy(edid_,blob->getBytesNoCopy(),length);
    edidBytes_=length;
    setProperty("Navi33FB,EDIDStatus","Valid OpenCore-injected EDID");
    setProperty("Navi33FB,EDIDBytes",static_cast<uint64_t>(edidBytes_),32);
    N33FB_LOG("EDID injection accepted: %zu bytes; no DDC hardware touched",length);
    return true;
}

bool Navi33GOPFB::hasDDCConnect(IOIndex index) {
    return index==0 && edidBytes_>=128u;
}

IOReturn Navi33GOPFB::getDDCBlock(IOIndex index, UInt32 block,
                                    IOSelect kind, IOOptionBits options,
                                    UInt8 *data, IOByteCount *length) {
    (void)options;
    if(index!=0 || kind!=kIODDCBlockTypeEDID || !data || !length)
        return kIOReturnUnsupported;
    if(block<1u || block>edidBytes_/128u)
        return kIOReturnNotFound;
    const size_t offset=(static_cast<size_t>(block)-1u)*128u;
    const IOByteCount n=*length<128u?*length:128u;
    memcpy(data,edid_+offset,n);
    *length=n;
    return kIOReturnSuccess;
}


// Draw only the top 32 rows of the *already validated* GOP BAR0 pixel
// buffer. Never alter GPU display registers or scanout address.
// Opt-in visual cue for the handoff phase (can be overwritten by WindowServer).
bool Navi33GOPFB::drawTestPattern() {
    if(!navi33fb::valid(console_) || !pci_) return false;
    const uint32_t rows=(console_.height<navi33fb::pattern::kStripeRows)
                        ?console_.height:navi33fb::pattern::kStripeRows;
    if(!rows || console_.width==0u || console_.rowBytes<console_.width*4u)
        return false;
    const uint64_t bytes=static_cast<uint64_t>(console_.rowBytes)*rows;
    if(bytes>console_.length || bytes>1024u*1024u) return false;
    // Reconfirm BAR ownership immediately before a CPU write.
    IODeviceMemory *bar0=pci_->getDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0);
    if(!bar0 || !navi33fb::ownedByBar0(navi33fb::checkBar0(
        console_,static_cast<uint64_t>(bar0->getPhysicalAddress()),
        static_cast<uint64_t>(bar0->getLength()))))
        return false;
    IODeviceMemory *memory=IODeviceMemory::withRange(
        console_.physicalBase,static_cast<IOByteCount>(bytes));
    if(!memory)return false;
    IOMemoryMap *mapping=memory->map();
    memory->release();
    if(!mapping)return false;
    const uintptr_t address=static_cast<uintptr_t>(mapping->getVirtualAddress());
    if(!address || (address & 3u)){
        mapping->release();
        return false;
    }
    auto *pixels=reinterpret_cast<volatile uint32_t *>(address);
    const size_t stride=console_.rowBytes/4u;
    for(uint32_t row=0;row<rows;++row){
        for(uint32_t x=0;x<console_.width;++x)
            pixels[static_cast<size_t>(row)*stride+x]=
                navi33fb::pattern::at(x,console_.width);
    }
    mapping->release();
    N33FB_LOG("wrote 4-colour GOP test stripe (%u rows) to BAR0 pixels; screen not verified",rows);
    return true;
}
