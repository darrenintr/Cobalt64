// Navi33Identity.h - portable, side-effect-free RX 7600 PCI identity gate.
// No macOS SDK, MMIO, firmware, or IOKit dependencies.
#pragma once
#include <stdint.h>

namespace navi33 {

constexpr uint16_t kAmdVendorId = 0x1002u;
constexpr uint16_t kRx7600DeviceId = 0x7480u;
constexpr uint8_t kPciDisplayBaseClass = 0x03u;

enum class IdentityStatus : uint8_t {
    Accepted = 0,
    WrongVendor,
    UnsupportedDevice,
    NotDisplayController
};

struct Identity {
    IdentityStatus status;
    uint16_t vendorId;
    uint16_t deviceId;
    uint8_t revisionId;
    uint8_t baseClass;
    uint8_t subClass;
    uint8_t progIf;
};

constexpr Identity inspectPci(uint32_t vendorAndDevice, uint32_t classAndRevision) {
    const uint16_t vendor = static_cast<uint16_t>(vendorAndDevice);
    const uint16_t device = static_cast<uint16_t>(vendorAndDevice >> 16);
    const uint8_t revision = static_cast<uint8_t>(classAndRevision);
    const uint8_t progIf = static_cast<uint8_t>(classAndRevision >> 8);
    const uint8_t subclass = static_cast<uint8_t>(classAndRevision >> 16);
    const uint8_t base = static_cast<uint8_t>(classAndRevision >> 24);
    const IdentityStatus status =
        vendor != kAmdVendorId ? IdentityStatus::WrongVendor :
        device != kRx7600DeviceId ? IdentityStatus::UnsupportedDevice :
        base != kPciDisplayBaseClass ? IdentityStatus::NotDisplayController :
        IdentityStatus::Accepted;
    return {status, vendor, device, revision, base, subclass, progIf};
}

constexpr bool isSupported(const Identity &identity) {
    return identity.status == IdentityStatus::Accepted;
}

} // namespace navi33
