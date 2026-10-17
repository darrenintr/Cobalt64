// SPDX-License-Identifier: MIT
// Navi33 GMC11 MMHUB VMID0 GART setup + invalidate engine 17.
// Sources: Linux amdgpu gmc_v11_0.c, mmhub_v3_0.c, mmhub_v3_0_2.c,
// and mmhub_{3_0_0,3_0_2}_{offset,sh_mask}.h (AMD MIT).
//
// REAL MMIO writes are implemented but only compiled when
// COBALT_NAVI33_ENABLE_UNSAFE_GMC11_MMIO is explicitly defined.
// Navi33Probe.kext DOES NOT include or instantiate this code.
// MMHUB VMID0 is shared by GPU engines and potentially display: NEVER
// take it over on a single active display GPU. There is no automatic reset.
#pragma once
#include "Navi33HardwarePlan.h"
#include "Navi33Gmc11Gart.h"
#include "Navi33Sdma6Staging.h"
#include <stddef.h>
#include <stdint.h>

namespace navi33 { namespace mmhub11 {
enum class Register : uint8_t {
    Context=0, TableLo, TableHi, StartLo, StartHi, EndLo, EndHi,
    InvalidateSem, InvalidateReq, InvalidateAck,
    InvalidateRangeLo, InvalidateRangeHi, PrivateInvalidate, Count
};
constexpr size_t kCount=static_cast<size_t>(Register::Count);
constexpr uint32_t kMmhub300[kCount] = {
    0x0740u,0x07abu,0x07acu,0x07cbu,0x07ccu,0x07ebu,0x07ecu,
    0x0762u,0x0774u,0x0786u,0x07a9u,0x07aau,0x071bu
};
constexpr uint32_t kMmhub302[kCount] = {
    0x06c0u,0x072bu,0x072cu,0x074bu,0x074cu,0x076bu,0x076cu,
    0x06e2u,0x06f4u,0x0706u,0x0729u,0x072au,0x069bu
};
constexpr uint32_t kGfxhubGfx1102[kCount] = {
    0x1688u,0x16f3u,0x16f4u,0x1713u,0x1714u,0x1733u,0x1734u,
    0x16aau,0x16bcu,0x16ceu,0x16f1u,0x16f2u,0x15d7u
};
// GC11 GFXHUB uses the same VMID0 Context0/PT-base bit layout but does
// NOT acquire the MMHUB invalidate semaphore or private cache bit.
enum class Hub : uint8_t { Mmhub=0, Gfxhub=1 };
constexpr uint32_t kContextEnable=0x01u;
constexpr uint32_t kContextDepth=0x06u; // depth=0 for flat GART
constexpr uint32_t kInvalidateVmid0=1u;
constexpr uint32_t kInvalidatePtePde=0x00f80000u;
constexpr uint32_t kInvalidateRequest=kInvalidateVmid0|kInvalidatePtePde;
constexpr uint32_t kPrivateInvalidateBit=1u<<25u;
constexpr uint32_t kRangeLoAll=0xffffffffu;
constexpr uint32_t kRangeHiAll=0x1fu;
constexpr uint32_t kGartPtes=static_cast<uint32_t>(staging::kPages);
static_assert(kGartPtes==16u && kInvalidateRequest==0x00f80001u,
              "GMC11 GART/invalidation contract drift");

#ifdef COBALT_NAVI33_ENABLE_UNSAFE_GMC11_MMIO
constexpr bool kWritePathCompiled=true;
#else
constexpr bool kWritePathCompiled=false;
#endif
enum class Code : uint8_t {
    Disabled=0, NoDiscovery, UnsupportedVersion, InvalidBar, InvalidPlan,
    Denied, InvalidTable, Busy, ReadFailure, WriteFailure, RecoveryRequired,
    Prepared, Flushed, Timeout, Aborted
};
struct Plan {
    Code code=Code::NoDiscovery;
    Hub hub=Hub::Mmhub;
    uint8_t revision=0xffu;
    uint64_t barBytes=0u;
    uint64_t offsets[kCount]{};
};
inline Plan planFromEntry(const IPEntry &entry,bool identifiedNavi33,
                          uint64_t barBytes) {
    Plan p{};
    p.barBytes=barBytes;
    if(!identifiedNavi33 || entry.hwId!=ipid::MMHUB ||
       entry.instance!=0u || entry.numBases<1u) return p;
    if(entry.major!=3u || entry.minor!=0u ||
       (entry.revision!=0u && entry.revision!=2u)) {
        p.code=Code::UnsupportedVersion;
        return p;
    }
    p.revision=entry.revision;
    const uint64_t base=entry.bases[0u];
    if(!base || base==UINT64_MAX || base==UINT32_MAX ||
       barBytes<4u) {p.code=Code::InvalidBar;return p;}
    const uint32_t *regs=(entry.revision==2u)?kMmhub302:kMmhub300;
    const uint64_t last=(barBytes-4u)/4u;
    for(size_t i=0;i<kCount;++i) {
        if(base>last || regs[i]>last-base) {
            p.code=Code::InvalidBar;return p;
        }
        p.offsets[i]=(base+regs[i])*4u;
        for(size_t j=0;j<i;++j)
            if(p.offsets[j]==p.offsets[i]) {
                p.code=Code::InvalidPlan;return p;
            }
    }
    p.code=Code::Prepared; // address plan only, NO MMIO writes
    return p;
}
inline Plan planGfxFromEntry(const IPEntry &entry,bool identifiedNavi33,
                             uint64_t barBytes) {
    Plan p{};
    p.hub=Hub::Gfxhub;
    p.barBytes=barBytes;
    if(!identifiedNavi33 || entry.hwId!=ipid::GC || entry.instance!=0u ||
       entry.numBases<1u)return p;
    if(entry.major!=11u || entry.minor!=0u || entry.revision!=2u) {
        p.code=Code::UnsupportedVersion;return p;
    }
    p.revision=2u;
    const uint64_t base=entry.bases[0u];
    if(!base || base==UINT64_MAX || base==UINT32_MAX ||
       barBytes<4u){p.code=Code::InvalidBar;return p;}
    const uint64_t last=(barBytes-4u)/4u;
    for(size_t i=0;i<kCount;++i) {
        if(base>last || kGfxhubGfx1102[i]>last-base) {
            p.code=Code::InvalidBar;return p;
        }
        p.offsets[i]=(base+kGfxhubGfx1102[i])*4u;
        for(size_t j=0;j<i;++j)
            if(p.offsets[i]==p.offsets[j]) {
                p.code=Code::InvalidPlan;return p;
            }
    }
    p.code=Code::Prepared;
    return p;
}
inline Plan planGfxFromDiscovery(const IPDiscovery &disc,uint64_t barBytes) {
    Plan p{};
    p.hub=Hub::Gfxhub;
    if(inventory(disc).code!=InventoryCode::Pass ||
       !disc.isNavi33Candidate())return p;
    IPEntry gc{};
    if(!disc.find(ipid::GC,0u,gc))return p;
    return planGfxFromEntry(gc,true,barBytes);
}
inline Plan planFromDiscovery(const IPDiscovery &disc,uint64_t barBytes) {
    Plan p{};
    if(inventory(disc).code!=InventoryCode::Pass ||
       !disc.isNavi33Candidate())return p;
    IPEntry mmhub{};
    if(!disc.find(ipid::MMHUB,0u,mmhub))return p;
    return planFromEntry(mmhub,true,barBytes);
}
inline bool validPlan(const Plan &p) {
    if(p.code!=Code::Prepared || p.barBytes<4u ||
       (p.hub!=Hub::Mmhub && p.hub!=Hub::Gfxhub) ||
       (p.revision!=0u && p.revision!=2u) ||
       (p.hub==Hub::Gfxhub && p.revision!=2u))return false;
    const uint32_t *regs=p.hub==Hub::Gfxhub ? kGfxhubGfx1102 :
        p.revision==2u ? kMmhub302 : kMmhub300;
    if(p.offsets[0u]<(static_cast<uint64_t>(regs[0u])*4u))
        return false;
    const uint64_t baseByteOffset=p.offsets[0u]-
                                  static_cast<uint64_t>(regs[0u])*4u;
    if(!baseByteOffset || (baseByteOffset&3u))return false;
    for(size_t i=0;i<kCount;++i) {
        const uint64_t off=p.offsets[i];
        if(!off || (off&3u) || off>p.barBytes ||
           p.barBytes-off<4u ||
           off!=baseByteOffset+static_cast<uint64_t>(regs[i])*4u)
            return false;
        for(size_t j=0;j<i;++j)
            if(p.offsets[j]==off)return false;
    }
    return true;
}
struct Gate {
    bool dedicatedGpuOwnership=false;
    bool noActiveDisplayConsumers=false;
    bool vmhubExclusivelyOwned=false;
    bool noOutstandingGpuJobs=false;
    bool firmwareAndHdpVerified=false;
    bool mmhubCacheAlreadyConfigured=false;
    bool gpuTableMemoryPinnedAndTrusted=false;
    bool isolatedPowerCycleAvailable=false;
};
constexpr bool eligible(Gate g) {
    return g.dedicatedGpuOwnership && g.noActiveDisplayConsumers &&
           g.vmhubExclusivelyOwned && g.noOutstandingGpuJobs &&
           g.firmwareAndHdpVerified && g.mmhubCacheAlreadyConfigured &&
           g.gpuTableMemoryPinnedAndTrusted &&
           g.isolatedPowerCycleAvailable;
}
struct Table {
    uint64_t gpuPhysicalPageTable=0u; // verified GPU MC address, NOT CPU pointer
    uint64_t gpuVirtualBase=staging::kGuestGpuVa;
    uint64_t mappedBytes=staging::kBytes;
    uint64_t entries[staging::kPages]{};
};
inline bool validTable(const Table &t) {
    if(!t.gpuPhysicalPageTable ||
       (t.gpuPhysicalPageTable&(gmc11::kPageBytes-1u)) ||
       t.gpuPhysicalPageTable>=gmc11::kMaxConservativeDmaAddress ||
       t.gpuVirtualBase!=staging::kGuestGpuVa ||
       t.mappedBytes!=staging::kBytes)return false;
    // Require all 16 entries to describe ordinary pinned 4KiB DMA pages,
    // with source write-protected. Metadata is not proof of the page table
    // actually being installed or coherent with GPU caches.
    for(size_t i=0;i<staging::kPages;++i) {
        if(gmc11::parseSystemPte(t.entries[i]).code!=gmc11::Code::Pass ||
           (((t.entries[i]&gmc11::kPteWriteable)!=0u) !=
            (i!=staging::kSourcePage)))return false;
    }
    return true;
}
using Read=bool (*)(void*,uint64_t,uint32_t*);
using Write=bool (*)(void*,uint64_t,uint32_t);
using Sync=bool (*)(void*);
using Delay=void (*)(void*,unsigned);
struct IO {
    void *ctx=nullptr;
    Read read=nullptr;
    Write write=nullptr;
    Sync syncTableForDevice=nullptr;
    Sync flushHdp=nullptr;
    Delay delayUs=nullptr;
};
struct Result {
    Code code=Code::Disabled;
    uint32_t writes=0u;
    uint32_t polls=0u;
    uint32_t acknowledgement=0u;
};
class Controller {
public:
    Result prepare(const Plan &p, Gate g, const Table &table, IO io) {
#ifndef COBALT_NAVI33_ENABLE_UNSAFE_GMC11_MMIO
        (void)p;(void)g;(void)table;(void)io;
        return {Code::Disabled,0u,0u,0u};
#else
        Result r{};
        if(state_!=Code::Disabled) {r.code=Code::Busy;return r;}
        if(!eligible(g)) {r.code=Code::Denied;return r;}
        if(!validPlan(p)) {r.code=Code::InvalidPlan;return r;}
        if(!validTable(table)) {r.code=Code::InvalidTable;return r;}
        if(!io.read || !io.write || !io.syncTableForDevice ||
           !io.flushHdp || !io.delayUs) {
            r.code=Code::Denied;return r;
        }
        const size_t indices[9u]={0u,1u,2u,3u,4u,5u,6u,10u,11u};
        uint32_t old[9u]{};
        // Read every register before changing anything. Do not read
        // semaphore register here: Linux's RREG can acquire its lease.
        for(size_t i=0;i<9u;++i)
            if(!io.read(io.ctx,p.offsets[indices[i]],&old[i])) {
                r.code=Code::ReadFailure;return r;
            }
        uint32_t ack=0u,request=0u,privateValue=0u;
        if(!io.read(io.ctx,p.offsets[static_cast<size_t>(Register::InvalidateAck)],&ack) ||
           !io.read(io.ctx,p.offsets[static_cast<size_t>(Register::InvalidateReq)],&request) ||
           (p.hub==Hub::Mmhub &&
            !io.read(io.ctx,p.offsets[static_cast<size_t>(Register::PrivateInvalidate)],&privateValue))) {
            r.code=Code::ReadFailure;return r;
        }
        // Never replace a running/GOP-owned MMHUB or invalidate an
        // engine 17 transaction that belongs to another driver.
        if((old[0u]&kContextEnable) || (ack&kInvalidateVmid0) ||
           request!=0u || (p.hub==Hub::Mmhub && (privateValue&kPrivateInvalidateBit))) {
            r.code=Code::Busy;return r;
        }
        if(!io.syncTableForDevice(io.ctx)) {
            r.code=Code::Denied;return r;
        }
        const uint64_t start=table.gpuVirtualBase>>12u;
        const uint64_t end=(table.gpuVirtualBase+table.mappedBytes-1u)>>12u;
        const uint32_t next[9u]={
            static_cast<uint32_t>(old[0u]&~(kContextEnable|kContextDepth)),
            static_cast<uint32_t>(table.gpuPhysicalPageTable),
            static_cast<uint32_t>(table.gpuPhysicalPageTable>>32u),
            static_cast<uint32_t>(start),static_cast<uint32_t>(start>>32u),
            static_cast<uint32_t>(end),static_cast<uint32_t>(end>>32u),
            kRangeLoAll,kRangeHiAll
        };
        // Leave context 0 disabled until the full shadow image has been
        // installed and read back.
        unsigned written=0u;
        for(size_t i=1u;i<9u;++i) {
            if(!io.write(io.ctx,p.offsets[indices[i]],next[i])) {
                // Compensate even the failed write (may have partially landed).
                bool restored=true;
                for(size_t j=1u;j<=i;++j)
                    if(!io.write(io.ctx,p.offsets[indices[j]],old[j]))
                        restored=false;
                // A successful callback may still have failed to restore the
                // MMIO value. Read back the exact preflight state.
                for(size_t j=1u;j<=i;++j) {
                    uint32_t verified=0u;
                    if(!io.read(io.ctx,p.offsets[indices[j]],&verified) ||
                       verified!=old[j]) restored=false;
                }
                r.writes=written;
                r.code=restored?Code::WriteFailure:Code::RecoveryRequired;
                if(!restored)state_=Code::RecoveryRequired;
                return r;
            }
            ++written;
        }
        for(size_t i=1u;i<9u;++i) {
            uint32_t v=0u;
            if(!io.read(io.ctx,p.offsets[indices[i]],&v) || v!=next[i]) {
                state_=Code::RecoveryRequired;
                r.code=Code::RecoveryRequired;r.writes=written;
                return r;
            }
        }
        for(size_t i=0u;i<9u;++i)saved_[i]=old[i];
        plan_=p;io_=io;
        gpuTable_=table.gpuPhysicalPageTable;
        state_=Code::Prepared;
        r.code=Code::Prepared;r.writes=written;
        return r;
#endif
    }

    Result invalidateAndActivate(unsigned maxPolls=1000u,
                                 unsigned delayUs=10u) {
#ifndef COBALT_NAVI33_ENABLE_UNSAFE_GMC11_MMIO
        (void)maxPolls;(void)delayUs;
        return {Code::Disabled,0u,0u,0u};
#else
        Result r{};
        if(state_!=Code::Prepared) {r.code=Code::Denied;return r;}
        if(!maxPolls || maxPolls>100000u || !delayUs || delayUs>10000u) {
            r.code=Code::InvalidPlan;return r;
        }
        // A failed enable is ambiguous, even if the MMIO callback reports
        // false. Do not unpin memory until independent GPU power reset.
        const uint32_t ctrl=(saved_[0u]&~kContextDepth)|kContextEnable;
        if(!io_.write(io_.ctx,plan_.offsets[0u],ctrl)) {
            state_=Code::RecoveryRequired;r.code=Code::RecoveryRequired;return r;
        }
        ++r.writes;
        uint32_t v=0u;
        if(!io_.read(io_.ctx,plan_.offsets[0u],&v) || v!=ctrl ||
           !io_.flushHdp(io_.ctx)) {
            state_=Code::RecoveryRequired;r.code=Code::RecoveryRequired;return r;
        }
        // MMHUB3 requires a side-effecting SEM acquire on bare metal;
        // GFXHUB3 invalidation does NOT use this semaphore in Linux GMC11.
        if(plan_.hub==Hub::Mmhub) {
            bool acquired=false;
            for(unsigned i=0u;i<maxPolls;++i) {
                ++r.polls;
                if(!io_.read(io_.ctx,plan_.offsets[
                           static_cast<size_t>(Register::InvalidateSem)],&v)) {
                    state_=Code::RecoveryRequired;
                    r.code=Code::RecoveryRequired;return r;
                }
                if(v&1u){acquired=true;break;}
                io_.delayUs(io_.ctx,delayUs);
            }
            if(!acquired) {
                state_=Code::RecoveryRequired;r.code=Code::Timeout;return r;
            }
        }
        if(!io_.write(io_.ctx,plan_.offsets[
                  static_cast<size_t>(Register::InvalidateReq)],
                  kInvalidateRequest)) {
            state_=Code::RecoveryRequired;r.code=Code::RecoveryRequired;return r;
        }
        ++r.writes;
        bool acknowledged=false;
        for(unsigned i=0u;i<maxPolls;++i) {
            ++r.polls;
            if(!io_.read(io_.ctx,plan_.offsets[
                      static_cast<size_t>(Register::InvalidateAck)],&v)) {
                state_=Code::RecoveryRequired;r.code=Code::RecoveryRequired;return r;
            }
            r.acknowledgement=v;
            if(v&kInvalidateVmid0){acknowledged=true;break;}
            io_.delayUs(io_.ctx,delayUs);
        }
        // Linux also releases the MMHUB semaphore on ACK timeout.
        if(plan_.hub==Hub::Mmhub) {
            if(!io_.write(io_.ctx,plan_.offsets[
                      static_cast<size_t>(Register::InvalidateSem)],0u)) {
                state_=Code::RecoveryRequired;r.code=Code::RecoveryRequired;return r;
            }
            ++r.writes;
        }
        if(!acknowledged) {
            state_=Code::RecoveryRequired;r.code=Code::Timeout;return r;
        }
        // Only MMHUB requires the separate private cache invalidation.
        if(plan_.hub==Hub::Mmhub) {
            if(!io_.read(io_.ctx,plan_.offsets[
                          static_cast<size_t>(Register::PrivateInvalidate)],&v) ||
               !io_.write(io_.ctx,plan_.offsets[
                          static_cast<size_t>(Register::PrivateInvalidate)],
                          v|kPrivateInvalidateBit)) {
                state_=Code::RecoveryRequired;r.code=Code::RecoveryRequired;return r;
            }
            ++r.writes;
            if(!io_.read(io_.ctx,plan_.offsets[
                          static_cast<size_t>(Register::PrivateInvalidate)],&v)) {
                state_=Code::RecoveryRequired;r.code=Code::RecoveryRequired;return r;
            }
        }
        state_=Code::Flushed;
        r.code=Code::Flushed;
        return r;
#endif
    }

    Result abortBeforeActivation() {
#ifndef COBALT_NAVI33_ENABLE_UNSAFE_GMC11_MMIO
        return {Code::Disabled,0u,0u,0u};
#else
        Result r{};
        if(state_!=Code::Prepared) {r.code=Code::Denied;return r;}
        const size_t indices[9u]={0u,1u,2u,3u,4u,5u,6u,10u,11u};
        uint32_t ctl=0u;
        if(!io_.read(io_.ctx,plan_.offsets[0u],&ctl) ||
           (ctl&kContextEnable)) {
            state_=Code::RecoveryRequired;r.code=Code::RecoveryRequired;return r;
        }
        for(size_t i=1u;i<9u;++i) {
            if(!io_.write(io_.ctx,plan_.offsets[indices[i]],saved_[i])) {
                state_=Code::RecoveryRequired;r.code=Code::RecoveryRequired;return r;
            }
            ++r.writes;
            uint32_t verify=0u;
            if(!io_.read(io_.ctx,plan_.offsets[indices[i]],&verify) ||
               verify!=saved_[i]) {
                state_=Code::RecoveryRequired;r.code=Code::RecoveryRequired;return r;
            }
        }
        state_=Code::Disabled;r.code=Code::Aborted;return r;
#endif
    }

    bool canUseGpuPageTable() const {return state_==Code::Flushed;}
    Hub currentHub() const {return plan_.hub;}
    uint64_t verifiedTableAddress() const {
        return state_==Code::Flushed ? gpuTable_ : 0u;
    }
    // No MMHUB teardown has yet been validated on silicon. A mapped table
    // must remain pinned until a separate reset/recovery path is verified.
    bool mayReleaseMappedMemory() const {return state_==Code::Disabled;}
    bool requiresRecovery() const {
        return state_==Code::RecoveryRequired || state_==Code::Flushed;
    }
private:
    Code state_=Code::Disabled;
    Plan plan_{};
    IO io_{};
    [[maybe_unused]] uint32_t saved_[9u]{};
    uint64_t gpuTable_=0u;
};
// A verified single hub must NOT unlock SDMA6. Both hubs need to report
// completed invalidations for the exact same pinned root table.
inline bool dualHubReady(const Controller &mmhub,const Controller &gfxhub) {
    const uint64_t root=mmhub.verifiedTableAddress();
    return mmhub.canUseGpuPageTable() && gfxhub.canUseGpuPageTable() &&
           mmhub.currentHub()==Hub::Mmhub &&
           gfxhub.currentHub()==Hub::Gfxhub &&
           root!=0u && gfxhub.verifiedTableAddress()==root;
}
} } // namespace navi33::mmhub11
