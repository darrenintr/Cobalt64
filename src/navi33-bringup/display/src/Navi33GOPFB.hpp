// Navi33GOPFB - conservative software scanout adoption for Navi 33.
// No display mode-setting, hardware reset, BAR mapping, MMIO, firmware or GPU commands.
// Based conceptually on RDNA4FB's IOFramebuffer adoption (BSD-3-Clause).
#pragma once
#include <IOKit/graphics/IOFramebuffer.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <IOKit/IOPlatformExpert.h>
#include <pexpert/pexpert.h>
#include "Navi33GopLayout.h"
#include "Navi33EDID.h"

class Navi33GOPFB final : public IOFramebuffer {
    OSDeclareDefaultStructors(Navi33GOPFB)
    using super = IOFramebuffer;
public:
    IOService *probe(IOService *provider, SInt32 *score) override;
    bool start(IOService *provider) override;
    void stop(IOService *provider) override;
    IOReturn enableController() override;
    bool isConsoleDevice() override;

    IODeviceMemory *getApertureRange(IOPixelAperture aperture) override;
    IODeviceMemory *getVRAMRange() override;

    const char *getPixelFormats() override;
    IOItemCount getDisplayModeCount() override;
    IOReturn getDisplayModes(IODisplayModeID *allModes) override;
    IOReturn getInformationForDisplayMode(IODisplayModeID mode,
                                           IODisplayModeInformation *info) override;
    UInt64 getPixelFormatsForDisplayMode(IODisplayModeID mode, IOIndex depth) override;
    IOReturn getPixelInformation(IODisplayModeID mode, IOIndex depth,
                                  IOPixelAperture aperture,
                                  IOPixelInformation *info) override;
    IOReturn getCurrentDisplayMode(IODisplayModeID *mode, IOIndex *depth) override;
    IOReturn setDisplayMode(IODisplayModeID mode, IOIndex depth) override;
    IOReturn setStartupDisplayMode(IODisplayModeID mode, IOIndex depth) override;

    IOReturn getAttribute(IOSelect attribute, uintptr_t *value) override;
    IOReturn setAttribute(IOSelect attribute, uintptr_t value) override;
    bool hasDDCConnect(IOIndex connectIndex) override;
    IOReturn getDDCBlock(IOIndex connectIndex, UInt32 blockNumber,
                         IOSelect blockType, IOOptionBits options,
                         UInt8 *data, IOByteCount *length) override;
    IOItemCount getConnectionCount() override;
    IOReturn getAttributeForConnection(IOIndex index, IOSelect attribute,
                                        uintptr_t *value) override;
    IOReturn setAttributeForConnection(IOIndex index, IOSelect attribute,
                                        uintptr_t value) override;
private:
    bool captureConsole();
    bool hostConsole(navi33fb::Layout *out);
    bool readInjectedEDID();
    bool drawTestPattern();
    void initializePowerManagement();
    static constexpr IODisplayModeID kFirmwareMode = static_cast<IODisplayModeID>(1);
    IOPCIDevice *pci_ { nullptr }; // borrowed provider, no retain needed while attached
    navi33fb::Layout console_ {};
    uint32_t colorMap_ { 0 };
    uint8_t edid_[256]{};
    size_t edidBytes_{0u};
    bool pmInitialized_ { false };
};
