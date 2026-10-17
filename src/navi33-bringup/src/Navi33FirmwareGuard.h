// Navi33FirmwareGuard - Linux AMD common_firmware_header offline metadata.
// Reference: drivers/gpu/drm/amd/amdgpu/amdgpu_ucode.h (MIT).
// No microcode execution, command submission, PSP mailbox or hardware writes.
// CRC field is REPORTED but NOT cryptographic proof of trust or authenticity.
#pragma once
#include <stdint.h>
#include <stddef.h>

namespace navi33 {
namespace firmware {

enum class Family : uint8_t {Gfx11, Psp13, Smu13, Sdma6};

struct FamilySpec {
    Family family;
    const char *linuxPrefix; // Expected AMD linux-firmware naming family
    uint16_t ipMajor, ipMinor;
};

constexpr FamilySpec kFamilies[] = {
    {Family::Gfx11, "gc_11_0_2", 11u, 0u},
    {Family::Psp13, "psp_13_0_7", 13u, 0u},
    {Family::Smu13, "smu_13_0_7", 13u, 0u},
    {Family::Sdma6, "sdma_6_0_2", 6u, 0u}
};

enum class Check : uint8_t {
    Pass = 0, MissingBytes, UnknownFamily, BadSize, BadHeader,
    WrongEngine, EmptyPayload, OutOfRange
};

struct Header {
    Check status = Check::MissingBytes;
    uint16_t headerMajor = 0, headerMinor = 0;
    uint16_t ipMajor = 0, ipMinor = 0;
    uint32_t ucodeVersion = 0, ucodeOffset = 0, ucodeSize = 0;
    uint32_t advertisedCrc32 = 0;
};

inline uint16_t read16(const uint8_t *p, size_t at) {
    return static_cast<uint16_t>(p[at]) |
           static_cast<uint16_t>(static_cast<uint16_t>(p[at+1]) << 8u);
}
inline uint32_t read32(const uint8_t *p, size_t at) {
    return static_cast<uint32_t>(read16(p,at)) |
           (static_cast<uint32_t>(read16(p,at+2u)) << 16u);
}

inline Header inspect(const uint8_t *bytes, size_t len, Family family) {
    Header out{};
    if (!bytes || len < 32u) return out;
    const size_t index = static_cast<size_t>(family);
    if (index >= sizeof(kFamilies)/sizeof(kFamilies[0])) {
        out.status = Check::UnknownFamily; return out;
    }
    if (len > 32u*1024u*1024u || read32(bytes, 0u) != len) {
        out.status = Check::BadSize; return out;
    }
    const uint32_t headerSize = read32(bytes, 4u);
    out.headerMajor = read16(bytes, 8u);
    out.headerMinor = read16(bytes, 10u);
    out.ipMajor = read16(bytes, 12u);
    out.ipMinor = read16(bytes, 14u);
    out.ucodeVersion = read32(bytes, 16u);
    out.ucodeSize = read32(bytes, 20u);
    out.ucodeOffset = read32(bytes, 24u);
    out.advertisedCrc32 = read32(bytes, 28u);
    if (headerSize < 32u || headerSize > len || !out.headerMajor) {
        out.status = Check::BadHeader; return out;
    }
    if (out.ipMajor != kFamilies[index].ipMajor ||
        out.ipMinor != kFamilies[index].ipMinor) {
        out.status = Check::WrongEngine; return out;
    }
    if (!out.ucodeSize) {
        out.status = Check::EmptyPayload; return out;
    }
    if (out.ucodeOffset < headerSize || out.ucodeOffset > len ||
        out.ucodeSize > len - out.ucodeOffset) {
        out.status = Check::OutOfRange; return out;
    }
    out.status = Check::Pass;
    return out;
}

constexpr bool kMayLoadFirmware = false;
static_assert(!kMayLoadFirmware, "Firmware metadata cannot trigger loading");
} // namespace firmware
} // namespace navi33
