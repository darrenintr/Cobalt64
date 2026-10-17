// SPDX-License-Identifier: MIT
// Read-only PSP13 firmware-container verification, no payload redistribution.
// Layout: Linux amdgpu_ucode.h, psp_firmware_header_v2_0/v2_1.
// CRC32 is accidental-corruption detection, NOT signature/authenticity check.
// Nothing in this file loads firmware, maps DMA memory or touches PSP mailbox.
#pragma once
#include "Navi33FirmwareGuard.h"
#include <stddef.h>
#include <stdint.h>

namespace navi33 { namespace psp13fw {
enum class Code : uint8_t {
    Pass, CommonHeader, UnsupportedVersion, DescriptorCount,
    OutOfBounds, DuplicateType, OverlappingComponents, CrcMismatch
};
constexpr size_t kMaximumParts=32u;
constexpr uint32_t kMaximumType=13u; // PSP_FW_TYPE_PSP_SPDM_DRV
constexpr bool kMayBootPsp=false;
struct Component {
    uint32_t type=0u, version=0u, offset=0u, size=0u;
};
struct Inventory {
    Code code=Code::CommonHeader;
    uint32_t commonVersion=0u;
    uint32_t descriptorCount=0u;
    uint32_t auxiliaryIndex=UINT32_MAX;
    uint32_t computedCrc32=0u;
    uint32_t advertisedCrc32=0u;
    Component component[kMaximumParts]{};
};
inline uint32_t read32(const uint8_t *b,size_t off) {
    return firmware::read32(b,off);
}
inline uint32_t crc32(const uint8_t *data,size_t size) {
    // ISO/IEEE reflected polynomial. Same common FW payload CRC convention.
    uint32_t crc=0xffffffffu;
    for(size_t i=0;i<size;++i) {
        crc ^= data[i];
        for(unsigned bit=0;bit<8u;++bit)
            crc = (crc>>1u) ^ ((crc & 1u) ? 0xedb88320u : 0u);
    }
    return crc ^ 0xffffffffu;
}
inline Inventory inspect(const uint8_t *image,size_t size) {
    Inventory result{};
    const firmware::Header hdr=firmware::inspect(image,size,firmware::Family::Psp13);
    if(hdr.status != firmware::Check::Pass) return result;
    if(hdr.headerMajor!=2u || hdr.headerMinor>1u) {
        result.code=Code::UnsupportedVersion;return result;
    }
    const size_t descBase=(hdr.headerMinor==0u ? 36u : 40u);
    if(size<descBase || hdr.ucodeOffset<descBase ||
       hdr.ucodeOffset>size || hdr.ucodeSize>size-hdr.ucodeOffset) {
        result.code=Code::OutOfBounds;return result;
    }
    const uint32_t count=read32(image,32u);
    if(count==0u || count>kMaximumParts) {
        result.code=Code::DescriptorCount;return result;
    }
    const size_t descEnd=descBase + static_cast<size_t>(count)*16u;
    if(descEnd > hdr.ucodeOffset || descEnd > size ||
       descEnd > read32(image,4u)) {
        result.code=Code::OutOfBounds;return result;
    }
    if(hdr.headerMinor==1u) {
        result.auxiliaryIndex=read32(image,36u);
        if(result.auxiliaryIndex != UINT32_MAX &&
           result.auxiliaryIndex >= count) {
            result.code=Code::OutOfBounds;return result;
        }
    }
    for(unsigned i=0;i<count;++i) {
        const size_t pos=descBase+static_cast<size_t>(i)*16u;
        const Component component={
            read32(image,pos), read32(image,pos+4u),
            read32(image,pos+8u), read32(image,pos+12u)
        };
        if(component.type==0u || component.type>kMaximumType ||
           component.size==0u ||
           component.offset<hdr.ucodeOffset ||
           component.offset>size ||
           component.size>size-component.offset) {
            result.code=Code::OutOfBounds;return result;
        }
        for(unsigned j=0;j<i;++j) {
            const Component &old=result.component[j];
            if(old.type==component.type) {
                result.code=Code::DuplicateType;return result;
            }
            // Compare using subtraction after well-formed non-overflow bounds.
            if(component.offset<old.offset+old.size &&
               old.offset<component.offset+component.size) {
                result.code=Code::OverlappingComponents;return result;
            }
        }
        result.component[i]=component;
    }
    const uint32_t actual=crc32(image+hdr.ucodeOffset,hdr.ucodeSize);
    result.advertisedCrc32=hdr.advertisedCrc32;
    result.computedCrc32=actual;
    result.commonVersion=(static_cast<uint32_t>(hdr.headerMajor)<<16u)|
                         hdr.headerMinor;
    result.descriptorCount=count;
    if(actual != hdr.advertisedCrc32) {
        result.code=Code::CrcMismatch;return result;
    }
    result.code=Code::Pass;
    return result;
}
static_assert(!kMayBootPsp,"PSP container parser must remain offline-only");
} } // namespace navi33::psp13fw
