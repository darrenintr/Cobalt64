// SPDX-License-Identifier: MIT
// Read-only silicon Queue0 census (GFX11/Navi33), independent of the
// isolated experimental MMIO *writer*. This code never writes PCI, BAR5,
// firmware, doorbells, VRAM or graphics memory.
// Source: AMD Linux GC 11 register offsets, SDMA0_QUEUE0 controls.
// A stable/parked Queue0 DOES NOT prove ownership or permit submission.
#pragma once
#include "Navi33HardwarePlan.h"
#include "Navi33NativeABI.h"
#include <stdint.h>
#include <stddef.h>

namespace navi33 { namespace queue_census {
constexpr size_t kCount=8u;
constexpr uint32_t kRegisters[kCount]={
    0x0080u, // RB_CNTL
    0x0092u, // DOORBELL
    0x0083u, // RB_RPTR
    0x0084u, // RB_RPTR_HI
    0x0085u, // RB_WPTR
    0x0086u, // RB_WPTR_HI
    0x0081u, // RB_BASE
    0x0082u  // RB_BASE_HI
};
constexpr bool kCanWriteRegisters=false;
constexpr bool kCanAcquireQueue=false;
static_assert(!kCanWriteRegisters && !kCanAcquireQueue,
              "Queue0 census cannot own or program hardware");

struct Plan {
    n33_queue_census_state state=N33_QUEUE_NO_INVENTORY;
    uint64_t bar5Bytes=0u;
    uint64_t offsets[kCount]{};
};
inline bool validPlan(const Plan &p) {
    if(p.state!=N33_QUEUE_OBSERVED || p.bar5Bytes<4u)return false;
    for(size_t i=0u;i<kCount;++i) {
        const uint64_t off=p.offsets[i];
        if(!off || (off&3u) || off>p.bar5Bytes ||
           p.bar5Bytes-off<4u)return false;
        for(size_t j=0u;j<i;++j)
            if(off==p.offsets[j])return false;
    }
    return true;
}
inline Plan fromDiscovery(const IPDiscovery &disc,uint64_t bar5Bytes) {
    Plan p{};
    p.bar5Bytes=bar5Bytes;
    if(inventory(disc).code!=InventoryCode::Pass ||
       !disc.isNavi33Candidate())return p;
    for(size_t i=0u;i<kCount;++i) {
        const auto pos=resolveDword(disc,Engine::GFX11,0u,
                                    kRegisters[i],bar5Bytes);
        if(pos.code!=AddressCode::Pass || !pos.bar5ByteOffset) {
            p.state=N33_QUEUE_BAD_MMIO_PLAN;return p;
        }
        p.offsets[i]=pos.bar5ByteOffset;
    }
    p.state=N33_QUEUE_OBSERVED; // plan only, no register has been read
    if(!validPlan(p))p.state=N33_QUEUE_BAD_MMIO_PLAN;
    return p;
}
using ReadDword=bool (*)(void*,uint64_t,uint32_t*);
struct Sample {
    n33_queue_census_state state=N33_QUEUE_DISABLED;
    uint32_t values[kCount]{};
    uint32_t flags=0u;
    uint32_t reads=0u;
};
inline Sample readStable(const Plan &p,bool optIn,
                         ReadDword read,void *context) {
    Sample s{};
    if(!optIn)return s;
    if(p.state!=N33_QUEUE_OBSERVED) {s.state=p.state;return s;}
    if(!validPlan(p)) {s.state=N33_QUEUE_BAD_MMIO_PLAN;return s;}
    if(!read) {s.state=N33_QUEUE_READ_ERROR;return s;}
    uint32_t first[kCount]{},second[kCount]{};
    for(unsigned pass=0u;pass<2u;++pass) {
        for(size_t i=0u;i<kCount;++i) {
            uint32_t x=0u;
            if(!read(context,p.offsets[i],&x)) {
                s.state=N33_QUEUE_READ_ERROR;return s;
            }
            ++s.reads;
            if(x==UINT32_MAX) {
                s.state=N33_QUEUE_INVALID_READ;return s;
            }
            (pass ? second : first)[i]=x;
        }
    }
    for(size_t i=0u;i<kCount;++i) {
        if(first[i]!=second[i]) {
            s.state=N33_QUEUE_UNSTABLE;return s;
        }
    }
    for(size_t i=0u;i<kCount;++i)s.values[i]=second[i];
    if(second[0u]&1u)s.flags|=N33_QUEUE_FLAG_ENABLED;
    if(second[1u])s.flags|=N33_QUEUE_FLAG_DOORBELL;
    if(second[2u] || second[3u] || second[4u] || second[5u])
        s.flags|=N33_QUEUE_FLAG_POINTERS;
    if(second[6u] || second[7u])s.flags|=N33_QUEUE_FLAG_RING_BASE;
    if((s.flags&(N33_QUEUE_FLAG_ENABLED|N33_QUEUE_FLAG_DOORBELL|
                 N33_QUEUE_FLAG_POINTERS|N33_QUEUE_FLAG_RING_BASE))==0u)
        s.flags|=N33_QUEUE_FLAG_APPARENTLY_PARKED;
    s.state=N33_QUEUE_OBSERVED;
    return s;
}
inline n33_queue_census_status report(const Sample &s) {
    n33_queue_census_status r{};
    r.abi_version=N33_ABI_VERSION;
    r.size=sizeof(r);
    r.state=s.state;
    r.read_count=s.reads;
    if(s.state==N33_QUEUE_OBSERVED) {
        r.flags=s.flags;
        r.ring_control=s.values[0u];
        r.doorbell_control=s.values[1u];
        r.ring_rptr_lo=s.values[2u];
        r.ring_rptr_hi=s.values[3u];
        r.ring_wptr_lo=s.values[4u];
        r.ring_wptr_hi=s.values[5u];
        r.ring_base_lo=s.values[6u];
        r.ring_base_hi=s.values[7u];
    }
    return r;
}
} } // namespace navi33::queue_census
