// SPDX-License-Identifier: MIT
// Read-only PSP13 firmware-container verification, no payload redistribution.
// Layout: Linux amdgpu_ucode.h, psp_firmware_header_v2_0/v2_1. Component
// offsets are relative to ucode_array_offset_bytes, as in amdgpu_psp.c
// parse_sos_bin_descriptor(). The header crc32 is reported, not checked:
// Linux never checks it, and in psp_13_0_7_sos.bin it is not a CRC-32 of
// the payload. Integrity comes from the SHA-256 pin in Navi33Psp13Loader.h.
// Nothing in this file loads firmware, maps DMA memory or touches PSP mailbox.
#pragma once
#include "Navi33FirmwareGuard.h"
#include <stddef.h>
#include <stdint.h>

namespace navi33 { namespace psp13fw {
enum class Code : uint8_t {
    Pass, CommonHeader, UnsupportedVersion, DescriptorCount,
    OutOfBounds, DuplicateType, OverlappingComponents
};
enum Type : uint32_t { // amdgpu_psp.h enum psp_fw_type
    kSos=1u, kSysDrv=2u, kKdb=3u, kToc=4u, kSpl=5u
};
constexpr size_t kMaximumParts=32u;
constexpr uint32_t kMaximumType=13u; // PSP_FW_TYPE_PSP_SPDM_DRV
constexpr bool kMayBootPsp=false;
struct Component {
    uint32_t type=0u, version=0u, offset=0u, size=0u; // offset: from file start
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
    // header_size_bytes covers the fixed v2.x header only (36 in
    // psp_13_0_7_sos.bin); the descriptor array follows it, before ucode.
    const size_t descEnd=descBase + static_cast<size_t>(count)*16u;
    if(descEnd > hdr.ucodeOffset || descEnd > size) {
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
        const uint32_t relative=read32(image,pos+8u);
        Component component={
            read32(image,pos), read32(image,pos+4u),
            0u, read32(image,pos+12u)
        };
        // Each part must lie inside the ucode array the header describes.
        if(component.type==0u || component.type>kMaximumType ||
           component.size==0u || relative>hdr.ucodeSize ||
           component.size>hdr.ucodeSize-relative) {
            result.code=Code::OutOfBounds;return result;
        }
        component.offset=hdr.ucodeOffset+relative;
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
    result.code=Code::Pass;
    return result;
}
inline const Component *find(const Inventory &inv,uint32_t type) {
    if(inv.code!=Code::Pass)return nullptr;
    for(uint32_t i=0;i<inv.descriptorCount && i<kMaximumParts;++i)
        if(inv.component[i].type==type)return &inv.component[i];
    return nullptr;
}
static_assert(!kMayBootPsp,"PSP container parser must remain offline-only");
} } // namespace navi33::psp13fw
