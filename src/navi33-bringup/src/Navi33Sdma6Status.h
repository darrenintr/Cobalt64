// SPDX-License-Identifier: MIT
// RX 7600 SDMA6 live GPU register observer; DOES NOT submit packets.
// Source: Linux amdgpu gc_11_0_0_offset.h, GC BASE_IDX=0.
// Use with Navi33 IP Discovery only, never with Navi48 gfx1201.
// Observing a configured ring does NOT prove GPU execution or Metal support.
#pragma once
#include "Navi33HardwarePlan.h"
#include "Navi33NativeABI.h"
#include <stddef.h>
#include <stdint.h>

namespace navi33 { namespace sdma6live {
constexpr size_t kRegisterCount=5u;
constexpr uint32_t kDwords[kRegisterCount] = {
    0x0025u, // SDMA0_STATUS_REG
    0x0029u, // SDMA0_UCODE_CHECKSUM
    0x0080u, // SDMA0_QUEUE0_RB_CNTL
    0x0083u, // SDMA0_QUEUE0_RB_RPTR
    0x0085u  // SDMA0_QUEUE0_RB_WPTR
};
constexpr bool kMaySubmitSdma=false;
constexpr bool kMayRingDoorbell=false;
static_assert(!kMaySubmitSdma && !kMayRingDoorbell,
              "Read-only queue observation must not submit GPU work");

struct Plan {
    n33_sdma_state state=N33_SDMA_NO_INVENTORY;
    uint64_t offsets[kRegisterCount]{};
};
using ReadDword=bool (*)(void *ctx,uint64_t offset,uint32_t *out);
struct Sample {
    n33_sdma_state state=N33_SDMA_DISABLED;
    uint32_t values[kRegisterCount]{};
    uint32_t readCount=0u;
};
inline Plan buildPlan(const IPDiscovery &disc,uint64_t barLength) {
    Plan plan{};
    if(inventory(disc).code!=InventoryCode::Pass)return plan;
    for(size_t i=0;i<kRegisterCount;++i) {
        const RegisterAddress a=resolveDword(disc,Engine::GFX11,0u,
                                               kDwords[i],barLength);
        if(a.code!=AddressCode::Pass || a.bar5ByteOffset==0u ||
           (a.bar5ByteOffset&3u) || a.bar5ByteOffset>barLength ||
           barLength-a.bar5ByteOffset<4u) {
            plan.state=N33_SDMA_BAD_MMIO_PLAN;
            return plan;
        }
        for(size_t j=0;j<i;++j)
            if(plan.offsets[j]==a.bar5ByteOffset) {
                plan.state=N33_SDMA_BAD_MMIO_PLAN;return plan;
            }
        plan.offsets[i]=a.bar5ByteOffset;
    }
    plan.state=N33_SDMA_OBSERVED; // plan only, not actual engine status
    return plan;
}
inline Sample readOnce(const Plan &plan,bool optedIn,
                       ReadDword reader,void *ctx) {
    Sample s{};
    if(!optedIn)return s;
    if(plan.state!=N33_SDMA_OBSERVED) {s.state=plan.state;return s;}
    if(!reader) {s.state=N33_SDMA_READ_ERROR;return s;}
    for(size_t i=0;i<kRegisterCount;++i) {
        if(!plan.offsets[i] || (plan.offsets[i]&3u)) {
            s.state=N33_SDMA_BAD_MMIO_PLAN;return s;
        }
        for(size_t j=0;j<i;++j)
            if(plan.offsets[i]==plan.offsets[j]) {
                s.state=N33_SDMA_BAD_MMIO_PLAN;return s;
            }
    }
    uint32_t registers[kRegisterCount]{};
    for(size_t i=0;i<kRegisterCount;++i) {
        if(!reader(ctx,plan.offsets[i],&registers[i])) {
            s.state=N33_SDMA_READ_ERROR;return s;
        }
        ++s.readCount;
        if(registers[i]==UINT32_MAX) {
            s.state=N33_SDMA_INVALID_READ;return s;
        }
    }
    for(size_t i=0;i<kRegisterCount;++i) s.values[i]=registers[i];
    s.state=N33_SDMA_OBSERVED;
    return s;
}
inline n33_sdma_status report(const Sample &s) {
    n33_sdma_status out{};
    out.abi_version=N33_ABI_VERSION;out.size=sizeof(out);
    out.state=s.state;out.read_count=s.readCount;
    if(s.state==N33_SDMA_OBSERVED) {
        out.status_register=s.values[0];out.ucode_checksum=s.values[1];
        out.ring_control=s.values[2];
        out.ring_rptr=s.values[3];out.ring_wptr=s.values[4];
    }
    return out;
}
} } // namespace navi33::sdma6live
