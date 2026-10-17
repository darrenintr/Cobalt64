// SPDX-License-Identifier: MIT
// Read-only, hardware-derived Navi33 PSP/SDMA/MMHUB/GFXHUB status.
// No MMIO writes, GPU job submission or firmware loading.
#pragma once
#include "Navi33HardwarePlan.h"
#include "Navi33NativeABI.h"
#include <stdint.h>

namespace navi33 { namespace hwprobe {
struct HubOffsets {
    bool valid=false;
    uint64_t mmhub=0u, gfxhub=0u;
};
inline bool resolve(uint64_t base, uint32_t reg, uint64_t bytes,
                    uint64_t *out) {
    if(!out || !base || base==UINT64_MAX || base==0xffffffffull ||
       bytes<4u) return false;
    const uint64_t max=(bytes-4u)/4u;
    if(base>max || reg>max-base)return false;
    *out=(base+reg)*4u;
    return *out!=0u;
}
inline HubOffsets planHubs(const IPDiscovery &rom, uint64_t barBytes) {
    HubOffsets out{};
    if(inventory(rom).code!=InventoryCode::Pass ||
       !rom.isNavi33Candidate()) return out;
    IPEntry mm{},gc{};
    if(!rom.find(ipid::MMHUB,0u,mm) || !rom.find(ipid::GC,0u,gc))
        return out;
    if(!mm.numBases || mm.major!=3u || mm.minor!=0u ||
       (mm.revision!=0u && mm.revision!=2u) ||
       !gc.numBases || gc.major!=11u || gc.minor!=0u ||
       gc.revision!=2u) return out;
    const uint32_t mmreg=mm.revision==2u ? 0x06c0u : 0x0740u;
    if(!resolve(mm.bases[0],mmreg,barBytes,&out.mmhub) ||
       !resolve(gc.bases[0],0x1688u,barBytes,&out.gfxhub) ||
       out.mmhub==out.gfxhub)return out;
    out.valid=true;
    return out;
}
inline n33_hw_preflight blank(uint32_t reason=N33_HW_DISABLED) {
    n33_hw_preflight out{};
    out.abi_version=N33_ABI_VERSION;
    out.size=sizeof(out);
    out.reason=reason;
    return out;
}
inline n33_hw_preflight classify(const n33_psp_status &psp,
                                  const n33_sdma_status &sdma,
                                  uint32_t mmContext,uint32_t gfxContext,
                                  uint64_t barBytes) {
    n33_hw_preflight out=blank(N33_HW_MMIO_READ_FAILED);
    out.flags=N33_HW_FLAG_ROM_VERIFIED|N33_HW_FLAG_BAR5_MAPPED;
    out.psp_state=psp.state;
    out.sdma_state=sdma.state;
    out.psp_alive=psp.sign_of_life;out.sos_version=psp.sos_version;
    out.sdma_control=sdma.ring_control;out.sdma_rptr=sdma.ring_rptr;
    out.sdma_wptr=sdma.ring_wptr;
    out.mmhub_context0=mmContext;out.gfxhub_context0=gfxContext;
    out.bar5_kib=static_cast<uint32_t>(barBytes/1024u);
    out.mmio_reads=psp.read_count+sdma.read_count+2u;
    if(psp.state!=N33_PSP_RUNNING) {
        out.reason=N33_HW_PSP_NOT_RUNNING;
        return out;
    }
    out.flags|=N33_HW_FLAG_PSP_ALIVE;
    if(sdma.state!=N33_SDMA_OBSERVED) {
        out.reason=N33_HW_SDMA_READ_FAILED;
        return out;
    }
    out.flags|=N33_HW_FLAG_SDMA_READ;
    if(mmContext==UINT32_MAX || gfxContext==UINT32_MAX) {
        out.reason=N33_HW_MMIO_READ_FAILED;
        return out;
    }
    out.flags|=N33_HW_FLAG_HUBS_READ;
    if(sdma.ring_control&1u)out.flags|=N33_HW_FLAG_SDMA_BUSY;
    if((mmContext&1u) || (gfxContext&1u))
        out.flags|=N33_HW_FLAG_HUBS_ACTIVE;
    // Readable and inactive is NOT proof of exclusive ownership.
    out.reason=(out.flags & (N33_HW_FLAG_SDMA_BUSY|N33_HW_FLAG_HUBS_ACTIVE)) ?
        N33_HW_SHARED_GPU_STATE : N33_HW_NO_OWNED_GPU_VM;
    return out;
}
static_assert(sizeof(n33_hw_preflight)==64,"physical status ABI");
} }
