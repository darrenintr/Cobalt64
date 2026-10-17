// SPDX-License-Identifier: MIT
// BAR5 adapter for the isolated SDMA6 Queue0 / Queue1 transport.
// Converts GC11 IP Discovery base + hardware DWORD indexes into checked
// BAR5 byte offsets. The default Navi33Probe.kext NEVER imports this file.
// MMIO writes compile only under the same explicit experimental macro.
#pragma once
#include "Navi33HardwarePlan.h"
#include "Navi33Sdma6MmioQueue.h"
#include <stddef.h>
#include <stdint.h>
namespace navi33 { namespace sdma6_bar {
constexpr uint32_t kRegs[] = {
    sdma6_mmio::kRbCntl,sdma6_mmio::kRbBase,
    sdma6_mmio::kRbBaseHi,sdma6_mmio::kRptr,
    sdma6_mmio::kRptrHi,sdma6_mmio::kWptr,
    sdma6_mmio::kWptrHi,sdma6_mmio::kDoorbell,
    sdma6_mmio::kMinorPtr
};
constexpr size_t kCount=sizeof(kRegs)/sizeof(kRegs[0]);
struct Plan {
    AddressCode code=AddressCode::InventoryRejected;
    uint64_t bar5Length=0u;
    uint32_t queueIndex=0u;
    uint64_t byteOffset[kCount]{};
};
inline Plan buildPlan(const IPDiscovery &disc,uint64_t assignedBar5Length,
                      uint32_t queueIndex=0u) {
    Plan p{};
    p.bar5Length=assignedBar5Length;
    p.queueIndex=queueIndex;
    if(queueIndex>1u || inventory(disc).code!=InventoryCode::Pass)
        return p;
    const uint32_t delta=queueIndex*sdma6_mmio::kQueue1DwordDelta;
    for(size_t i=0;i<kCount;++i) {
        const RegisterAddress a=resolveDword(
            disc,Engine::GFX11,0u,kRegs[i]+delta,assignedBar5Length);
        if(a.code!=AddressCode::Pass || !a.bar5ByteOffset ||
           (a.bar5ByteOffset&3u)!=0u ||
           a.bar5ByteOffset>assignedBar5Length ||
           assignedBar5Length-a.bar5ByteOffset<4u) {
            p.code=a.code==AddressCode::Pass
                  ? AddressCode::OutsideBar5 : a.code;
            return p;
        }
        for(size_t j=0;j<i;++j) {
            if(p.byteOffset[j]==a.bar5ByteOffset) {
                p.code=AddressCode::OutsideBar5;return p;
            }
        }
        p.byteOffset[i]=a.bar5ByteOffset;
    }
    p.code=AddressCode::Pass;
    return p;
}
using ReadMmio=bool (*)(void*,uint64_t,uint32_t*);
using WriteMmio=bool (*)(void*,uint64_t,uint32_t);
struct Adapter {
    Plan plan{};
    void *device=nullptr;
    ReadMmio read=nullptr;
    WriteMmio write=nullptr;
    bool exclusiveOwned=false;
};
inline bool translate(const Plan &p,uint32_t reg,uint64_t *byteOffset) {
    if(!byteOffset || p.code!=AddressCode::Pass ||
       p.queueIndex>1u)return false;
    const uint32_t delta=p.queueIndex*sdma6_mmio::kQueue1DwordDelta;
    for(size_t i=0;i<kCount;++i)
        if(kRegs[i]+delta==reg) {
            const uint64_t off=p.byteOffset[i];
            if((off&3ull) || off==0ull || off>p.bar5Length ||
               p.bar5Length-off<4ull) return false;
            *byteOffset=off;
            return true;
        }
    return false;
}
inline bool read(void *opaque,uint32_t reg,uint32_t *value) {
    Adapter *a=static_cast<Adapter *>(opaque);
    if(!a || !a->read || !value)return false;
    uint64_t off=0u;
    if(!translate(a->plan,reg,&off))return false;
    return a->read(a->device,off,value);
}
inline bool write(void *opaque,uint32_t reg,uint32_t value) {
#ifndef COBALT_NAVI33_ENABLE_UNSAFE_MMIO
    (void)opaque;(void)reg;(void)value;
    return false;
#else
    Adapter *a=static_cast<Adapter *>(opaque);
    if(!a || !a->exclusiveOwned || !a->write)return false;
    // Never expose an MMIO write surface for firmware, resets, doorbells,
    // other PCI registers or unowned SDMA engines.
    if(a->plan.queueIndex>1u)return false;
    const uint32_t delta=a->plan.queueIndex*sdma6_mmio::kQueue1DwordDelta;
    if(reg!=sdma6_mmio::kRbCntl+delta &&
       reg!=sdma6_mmio::kRbBase+delta &&
       reg!=sdma6_mmio::kRbBaseHi+delta &&
       reg!=sdma6_mmio::kRptr+delta &&
       reg!=sdma6_mmio::kRptrHi+delta &&
       reg!=sdma6_mmio::kWptr+delta &&
       reg!=sdma6_mmio::kWptrHi+delta)return false;
    uint64_t off=0u;
    if(!translate(a->plan,reg,&off))return false;
    return a->write(a->device,off,value);
#endif
}
} } // navi33::sdma6_bar
