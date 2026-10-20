// SPDX-License-Identifier: MIT
// Navi33 NBIO 4.3 VRAM capacity: READ-ONLY, two-stable-read observer.
// Linux: nbio_v4_3_get_memsize(), NBIO IP Discovery BASE_IDX 2,
// nbio_4_3_0_offset.h: RCC_CONFIG_MEMSIZE = 0x00c3 DWORD.
// The value is in MiB, NOT a free VRAM allocation or BAR0 aperture size.
// No NBIO, PCI, firmware or VRAM writes are present in this module.
#pragma once
#include "Navi33HardwarePlan.h"
#include <stdint.h>
#include <stddef.h>

namespace navi33 { namespace nbio4 {
constexpr uint32_t kMemsizeRegisterDword=0x00c3u;
constexpr uint32_t kMemsizeBaseIndex=2u;
constexpr uint32_t kMiB=1024u*1024u;
// Bound to plausible discrete Navi33 cards. A probe failure must NOT
// initialize GPU memory from a guessed capacity.
constexpr uint32_t kMinimumSizeMiB=512u;
constexpr uint32_t kMaximumSizeMiB=32768u;
constexpr uint32_t kGranularityMiB=16u;
enum class Code : uint8_t {
    NoInventory=0, UnsupportedRevision, MissingBase,
    OutsideBar5, ReadFailed, Unstable, InvalidCapacity, Pass
};
struct Plan {
    Code code=Code::NoInventory;
    uint64_t byteOffset=0u;
    uint32_t nbioRevision=0xffffffffu;
};
inline Plan fromEntry(const IPEntry &nbif,bool candidate,
                      uint64_t assignedBar5Bytes) {
    Plan p{};
    if(!candidate || nbif.hwId!=ipid::NBIF || nbif.instance!=0u)
        return p;
    if(nbif.major!=4u || nbif.minor!=3u ||
       (nbif.revision!=0u && nbif.revision!=1u)) {
        p.code=Code::UnsupportedRevision;return p;
    }
    if(nbif.numBases<=kMemsizeBaseIndex) {
        p.code=Code::MissingBase;return p;
    }
    const uint64_t base=nbif.bases[kMemsizeBaseIndex];
    if(assignedBar5Bytes<4u || !base || base==UINT32_MAX ||
       base==UINT64_MAX) {
        p.code=Code::OutsideBar5;return p;
    }
    const uint64_t last=(assignedBar5Bytes-4u)/4u;
    if(base>last || kMemsizeRegisterDword>last-base) {
        p.code=Code::OutsideBar5;return p;
    }
    p.code=Code::Pass;
    p.byteOffset=(base+kMemsizeRegisterDword)*4u;
    p.nbioRevision=(static_cast<uint32_t>(nbif.major)<<16u)|
                   (static_cast<uint32_t>(nbif.minor)<<8u)|
                   nbif.revision;
    return p;
}
inline Plan fromDiscovery(const IPDiscovery &disc,uint64_t bar5Bytes) {
    if(inventory(disc).code!=InventoryCode::Pass ||
       !disc.isNavi33Candidate())return {};
    IPEntry nbif{};
    if(!disc.find(ipid::NBIF,0u,nbif))return {};
    return fromEntry(nbif,true,bar5Bytes);
}
using Read=bool (*)(void*,uint64_t,uint32_t*);
struct Sample {
    Code code=Code::NoInventory;
    uint32_t capacityMiB=0u;
    uint32_t readCount=0u;
    uint32_t version=0xffffffffu;
};
inline Sample inspect(const Plan &plan,Read read,void *context) {
    Sample out{};
    out.version=plan.nbioRevision;
    out.code=plan.code;
    if(plan.code!=Code::Pass)return out;
    if(!read) {out.code=Code::ReadFailed;return out;}
    uint32_t first=0u,second=0u;
    if(!read(context,plan.byteOffset,&first)) {
        out.code=Code::ReadFailed;return out;
    }
    out.readCount=1u;
    if(!read(context,plan.byteOffset,&second)) {
        out.code=Code::ReadFailed;return out;
    }
    out.readCount=2u;
    if(first==UINT32_MAX || second==UINT32_MAX) {
        out.code=Code::ReadFailed;return out;
    }
    if(first!=second) {
        out.code=Code::Unstable;return out;
    }
    // The RX 7600 reports 8176 MiB (0x1ff0) = 8 GiB minus a 16 MiB reserve, so
    // the granularity is 16 MiB, not 256 MiB (the old rule refused the real card).
    if(first<kMinimumSizeMiB || first>kMaximumSizeMiB ||
       (first%kGranularityMiB)!=0u) {
        out.code=Code::InvalidCapacity;return out;
    }
    out.capacityMiB=first;
    out.code=Code::Pass;
    return out;
}
} } // namespace navi33::nbio4
