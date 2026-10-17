// SPDX-License-Identifier: MIT
// Navi33/GFX11 SDMA6 Queue0 MMIO *write-capable transport*.
// Ported from Linux amdgpu sdma_v6_0_gfx_resume_instance / ring_set_wptr
// and GC11 register definitions. NOT linked into Navi33Probe.kext.
//
// This executes a REAL MMIO sequence IF supplied an independently owned,
// initialized GPU, fully installed GPU VM, a GPU-visible 4KiB ring, working
// cache maintenance and a recoverable exclusive queue. No caller in Cobalt64
// can currently attest those properties. Do not run on a display-only GPU.
//
// Build production code only after a real isolated test device exists by
// defining COBALT_NAVI33_ENABLE_UNSAFE_MMIO. Default build has no write path.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "Navi33Sdma6Staging.h"

namespace navi33 { namespace sdma6_mmio {
constexpr uint32_t kRbCntl=0x0080u;
constexpr uint32_t kRbBase=0x0081u;
constexpr uint32_t kRbBaseHi=0x0082u;
constexpr uint32_t kRptr=0x0083u;
constexpr uint32_t kRptrHi=0x0084u;
constexpr uint32_t kWptr=0x0085u;
constexpr uint32_t kWptrHi=0x0086u;
constexpr uint32_t kDoorbell=0x0092u;
constexpr uint32_t kMinorPtr=0x00b5u;
constexpr uint32_t kEnable=1u;
constexpr uint32_t kRbSizeMask=0x3eu; // log2(dwords) << 1, Linux GC11
constexpr uint32_t kRbPriv=1u<<23u;
constexpr uint32_t kVmidMask=0x0fu<<24u;
constexpr uint32_t kWptrPoll=0x100u;
constexpr uint32_t kF32Poll=0x800u;
constexpr uint32_t kRptrWb=0x1000u;
constexpr uint32_t kCtlUpdateMask=kRbSizeMask|kEnable|kVmidMask|
     kWptrPoll|kF32Poll|kRptrWb|kRbPriv;
constexpr uint32_t kHwRingDwords=1024u; // 4KiB mapped ring, not 16 DW
constexpr uint32_t kCommandsBytes=
    static_cast<uint32_t>(staging::kRingWords*4u);
static_assert(kCommandsBytes==64u,"16 DWORDs => 64 bytes");
static_assert(kHwRingDwords*4u==staging::kBytePage,
              "hardware ring must fit exactly in mapped ring page");
#ifdef COBALT_NAVI33_ENABLE_UNSAFE_MMIO
constexpr bool kWriteBackendCompiled=true;
#else
constexpr bool kWriteBackendCompiled=false;
#endif

struct Preconditions {
    bool firmwareVerified=false;
    bool queueExclusivelyOwned=false;
    bool vmid0MappedByThisDriver=false;
    bool tlbFlushedAfterMap=false;
    bool ringGpuVaResolved=false;
    bool dmaOwnershipHeld=false;
    bool independentGpuRecovery=false;
    bool cpuGpuCacheMaintenance=false;
    // VMID1 requires independent evidence that BOTH MMHUB and GFXHUB
    // context1 are owned, page-table bases installed and TLB flushed.
    // A synthetic boolean is not hardware ownership evidence.
    bool vmid1MappedByThisDriver=false;
    bool vmid1ExclusivelyOwned=false;
};
constexpr bool eligible(const Preconditions &p) {
    return p.firmwareVerified && p.queueExclusivelyOwned &&
           p.tlbFlushedAfterMap && p.ringGpuVaResolved &&
           p.dmaOwnershipHeld && p.independentGpuRecovery &&
           p.cpuGpuCacheMaintenance;
}
enum class Code : uint8_t {
    NotBuilt, Denied, Invalid, Busy, ReadFailed, WriteFailed,
    VerifyFailed, Ready, Submitted, Completed, Timeout, RecoveryRequired,
    Unstable
};
struct Request {
    uint64_t ringGpuVa=0u;  // actual device-mapped GPU VA, not OS IOVA
    uint64_t sourceGpuVa=0u;
    uint64_t targetGpuVa=0u;
    uint64_t fenceGpuVa=0u;
    uint32_t cookie=0u;
    uint32_t ringDwords=kHwRingDwords;
    uint32_t vmid=0u; // opt-in isolated Queue1 may use VMID1
};
using ReadRegister=bool (*)(void*,uint32_t,uint32_t*);
using WriteRegister=bool (*)(void*,uint32_t,uint32_t);
using Sync=bool (*)(void*); // IODMACommand/IOBuffer cache sync hook
using ReadByte=bool (*)(void*,size_t,uint8_t*); // pinned CPU staging buffer
using Delay=void (*)(void*,uint32_t);
struct IO {
    void *ctx=nullptr;
    ReadRegister read=nullptr;
    WriteRegister write=nullptr;
    Sync beforeGpu=nullptr;
    Sync afterGpu=nullptr;
    ReadByte readByte=nullptr;
    Delay delayUs=nullptr;
};
// Two complete snapshots are a *conflict detector*, NOT an ownership
// certificate. An independent PCI/firmware owner must already have
// excluded all other agents before any register write is considered.
constexpr uint32_t kSnapshotRegisters[8u]={
    kRbCntl,kDoorbell,kRptr,kRptrHi,kWptr,kWptrHi,kRbBase,kRbBaseHi
};
struct QueueSnapshot {
    uint32_t values[8u]{};
};
inline bool readQueueSnapshot(const IO &io,QueueSnapshot &out) {
    if(!io.read)return false;
    for(size_t i=0u;i<8u;++i) {
        if(!io.read(io.ctx,kSnapshotRegisters[i],&out.values[i]) ||
           out.values[i]==UINT32_MAX)return false;
    }
    return true;
}
inline bool sameQueueSnapshot(const QueueSnapshot &a,
                              const QueueSnapshot &b) {
    for(size_t i=0u;i<8u;++i)
        if(a.values[i]!=b.values[i])return false;
    return true;
}
struct Observation {
    Code status=Code::NotBuilt;
    uint32_t mmioWrites=0u;
    uint32_t polls=0u;
    uint32_t finalRptr=0u;
    uint32_t finalWptr=0u;
    uint32_t fence=0u;
};
inline bool validRequest(const Request &r) {
    constexpr uint64_t maxAddress=(1ull<<48u);
    const uint64_t source=staging::kSourcePage*staging::kBytePage;
    const uint64_t target=staging::kTargetPage*staging::kBytePage;
    const uint64_t fence=staging::kFencePage*staging::kBytePage;
    if(!r.ringGpuVa || (r.ringGpuVa&255ull)!=0ull ||
       !r.sourceGpuVa || (r.sourceGpuVa&4095ull)!=0ull ||
       !r.targetGpuVa || (r.targetGpuVa&4095ull)!=0ull ||
       !r.fenceGpuVa || (r.fenceGpuVa&4095ull)!=0ull ||
       r.ringDwords!=kHwRingDwords || r.cookie!=staging::kFenceCookie ||
       r.vmid>1u)
        return false;
    if(r.ringGpuVa>=maxAddress || r.sourceGpuVa>=maxAddress ||
       r.targetGpuVa>=maxAddress || r.fenceGpuVa>=maxAddress)
        return false;
    const uint64_t ringBase=r.sourceGpuVa+
                         staging::kRingPage*staging::kBytePage;
    return r.sourceGpuVa<=maxAddress-staging::kBytes &&
           r.targetGpuVa==r.sourceGpuVa+target-source &&
           r.ringGpuVa==ringBase &&
           r.fenceGpuVa==r.sourceGpuVa+fence-source;
}

class Queue {
public:
    // Explicit sequence is the only possible register-write entrypoint.
    Observation configure(const Preconditions &gate,Request req,IO io) {
#ifndef COBALT_NAVI33_ENABLE_UNSAFE_MMIO
        (void)gate;(void)req;(void)io;
        return {Code::NotBuilt,0u,0u,0u,0u,0u};
#else
        Observation result{};
        if(state_!=Code::Denied || touchedHardware_) {
            result.status=Code::Busy;return result;
        }
        if(!eligible(gate) ||
           (req.vmid==0u && !gate.vmid0MappedByThisDriver) ||
           (req.vmid==1u && (!gate.vmid1MappedByThisDriver ||
                             !gate.vmid1ExclusivelyOwned))) {
            result.status=Code::Denied;return result;
        }
        if(!validRequest(req) || !io.read || !io.write ||
           !io.beforeGpu || !io.afterGpu || !io.readByte ||
           !io.delayUs) {result.status=Code::Invalid;return result;}
        // Never infer a free queue merely from RB_ENABLE=0: stale
        // pointers, firmware setup and a changing owner are all blockers.
        QueueSnapshot first{},second{};
        if(!readQueueSnapshot(io,first) ||
           !readQueueSnapshot(io,second)) {
            result.status=Code::ReadFailed;return result;
        }
        if(!sameQueueSnapshot(first,second)) {
            result.status=Code::Unstable;return result;
        }
        const uint32_t ctl=first.values[0u];
        if((ctl&kEnable) || first.values[1u] || first.values[2u] ||
           first.values[3u] || first.values[4u] || first.values[5u] ||
           first.values[6u] || first.values[7u]) {
            result.status=Code::Busy;return result;
        }
        // Validate the exact pinned source, poisoned destination, COPY/FENCE
        // packet and clean fence. An arbitrary ring must not reach hardware.
        const sdma6::CopyPacket expected=sdma6::makeCopy(
            req.sourceGpuVa,req.targetGpuVa,
            static_cast<uint32_t>(staging::kBytePage));
        if(expected.status!=sdma6::CopyCode::Pass) {
            result.status=Code::Invalid;return result;
        }
        for(size_t i=0;i<staging::kBytePage;++i) {
            uint8_t a=0u,b=0u;
            if(!io.readByte(io.ctx,staging::kSourcePage*staging::kBytePage+i,&a) ||
               !io.readByte(io.ctx,staging::kTargetPage*staging::kBytePage+i,&b)) {
                result.status=Code::ReadFailed;return result;
            }
            if(a!=staging::pattern(i) || b!=0xcdu) {
                result.status=Code::Invalid;return result;
            }
        }
        const uint32_t fencePacket[4u]={
            5u|(3u<<16u),static_cast<uint32_t>(req.fenceGpuVa),
            static_cast<uint32_t>(req.fenceGpuVa>>32u),req.cookie
        };
        for(size_t i=0;i<staging::kRingWords;++i) {
            uint32_t word=0u;
            for(unsigned j=0;j<4u;++j) {
                uint8_t v=0u;
                if(!io.readByte(io.ctx,staging::kRingPage*
                         staging::kBytePage+i*4u+j,&v)) {
                    result.status=Code::ReadFailed;return result;
                }
                word|=static_cast<uint32_t>(v)<<(8u*j);
            }
            const uint32_t expectedWord=i<sdma6::kCopyPacketDwords ?
                expected.words[i] :
                i<sdma6::kCopyPacketDwords+4u ?
                fencePacket[i-sdma6::kCopyPacketDwords] : 0u;
            if(word!=expectedWord) {
                result.status=Code::Invalid;return result;
            }
        }
        uint8_t fenceByte=0u;
        for(size_t i=0;i<4u;++i) {
            if(!io.readByte(io.ctx,staging::kFencePage*
                           staging::kBytePage+i,&fenceByte)) {
                result.status=Code::ReadFailed;return result;
            }
            if(fenceByte!=0u) {result.status=Code::Busy;return result;}
        }
        // Ring base is encoded in 256-byte units (low 32, then high).
        const uint64_t encoded=req.ringGpuVa>>8u;
        const uint32_t rbSizeLog2=10u; // 1024 DWORDs
        const uint32_t encodedCtl=
            (ctl & ~kCtlUpdateMask) |
            ((rbSizeLog2<<1u)&kRbSizeMask) | kRbPriv |
            ((req.vmid<<24u)&kVmidMask);
        const uint32_t offsets[]={kRbBase,kRbBaseHi,kRptr,kRptrHi,
                                  kWptr,kWptrHi,kRbCntl};
        const uint32_t values[]={static_cast<uint32_t>(encoded),
                                 static_cast<uint32_t>(encoded>>32u),
                                 0u,0u,0u,0u,encodedCtl};
        const uint32_t snapshotOld[7u]={
            first.values[6u],first.values[7u],first.values[2u],
            first.values[3u],first.values[4u],first.values[5u],
            first.values[0u]
        };
        uint32_t old[7]{};
        for(unsigned i=0;i<7u;++i) {
            if(!io.read(io.ctx,offsets[i],&old[i])) {
                result.status=Code::ReadFailed;return result;
            }
            if(old[i]!=snapshotOld[i]) {
                result.status=Code::Unstable;return result;
            }
        }
        // After validating packet contents and all saved registers, check
        // the entire queue (including doorbell) again. This does not
        // eliminate races; only independent PCI ownership can do that.
        QueueSnapshot finalCheck{};
        if(!readQueueSnapshot(io,finalCheck)) {
            result.status=Code::ReadFailed;return result;
        }
        if(!sameQueueSnapshot(second,finalCheck)) {
            result.status=Code::Unstable;return result;
        }
        // An MMIO callback returning "false" could still have written
        // silicon. From the first attempt onward the ring and its DMA
        // backing are quarantined until INDEPENDENT hardware reset.
        // Never try speculative software rollback on an unknown device.
        touchedHardware_=true;
        state_=Code::RecoveryRequired;
        unsigned written=0u;
        for(unsigned i=0;i<7u;++i) {
            ++written; // count possibly-live attempts, not only successes
            if(!io.write(io.ctx,offsets[i],values[i])) {
                result.mmioWrites=written;
                result.status=Code::RecoveryRequired;
                return result;
            }
        }
        uint32_t verify=0u;
        for(unsigned i=0;i<7u;++i) {
            if(!io.read(io.ctx,offsets[i],&verify) ||
               verify!=values[i]) {
                // Hardware register mismatch: DO NOT release pinned memory.
                state_=Code::RecoveryRequired;
                result.status=Code::RecoveryRequired;
                result.mmioWrites=written;
                return result;
            }
        }
        req_=req;
        io_=io;
        oldCtl_=ctl;
        for(unsigned i=0;i<7u;++i) savedRegisters_[i]=old[i];
        state_=Code::Ready;
        result.status=Code::Ready;
        result.mmioWrites=written;
        return result;
#endif
    }
    Observation submit() {
#ifndef COBALT_NAVI33_ENABLE_UNSAFE_MMIO
        return {Code::NotBuilt,0u,0u,0u,0u,0u};
#else
        Observation o{};
        if(state_!=Code::Ready) {o.status=Code::Denied;return o;}
        // Actual device-specific cache maintenance MUST occur before RB enable.
        if(!io_.beforeGpu(io_.ctx)) {
            o.status=Code::ReadFailed;return o;
        }
        const uint32_t ctl=(oldCtl_ & ~kCtlUpdateMask) |
                           (10u<<1u) | kRbPriv | kEnable |
                           ((req_.vmid<<24u)&kVmidMask);
        // Enabling and publishing an SDMA ring can be irreversible without
        // a GPU reset; once enabled no blind register rollback is attempted.
        if(!io_.write(io_.ctx,kRbCntl,ctl)) {
            state_=Code::RecoveryRequired;
            o.status=Code::RecoveryRequired;return o;
        }
        ++o.mmioWrites;
        if(!io_.write(io_.ctx,kWptrHi,0u) ||
           !io_.write(io_.ctx,kWptr,kCommandsBytes)) {
            state_=Code::RecoveryRequired;
            o.status=Code::RecoveryRequired;return o;
        }
        o.mmioWrites+=2u;
        state_=Code::Submitted;
        o.status=Code::Submitted;
        return o;
#endif
    }
    Observation waitAndVerify(uint32_t polls,uint32_t delayUs=100u) {
#ifndef COBALT_NAVI33_ENABLE_UNSAFE_MMIO
        (void)polls;(void)delayUs;
        return {Code::NotBuilt,0u,0u,0u,0u,0u};
#else
        Observation o{};
        if(state_!=Code::Submitted) {o.status=Code::Denied;return o;}
        if(!polls || polls>100000u || !delayUs || delayUs>100000u) {
            o.status=Code::Invalid;return o;
        }
        for(uint32_t attempt=0;attempt<polls;++attempt) {
            ++o.polls;
            // Explicit invalidate or sync before *every* CPU fence read.
            if(!io_.afterGpu(io_.ctx)) {
                state_=Code::RecoveryRequired;
                o.status=Code::RecoveryRequired;return o;
            }
            uint32_t fence=0u;
            for(unsigned i=0;i<4u;++i) {
                uint8_t b=0u;
                if(!io_.readByte(io_.ctx,staging::kFencePage*
                         staging::kBytePage+i,&b)) {
                    state_=Code::RecoveryRequired;
                    o.status=Code::RecoveryRequired;return o;
                }
                fence|=static_cast<uint32_t>(b)<<(i*8u);
            }
            o.fence=fence;
            if(fence==req_.cookie) {
                // Fence packet follows COPY; after cache sync, compare the
                // entire 4096-byte destination, not just a sentinel.
                for(size_t i=0;i<staging::kBytePage;++i) {
                    uint8_t v=0u,source=0u;
                    if(!io_.readByte(io_.ctx,
                           staging::kTargetPage*staging::kBytePage+i,&v) ||
                       !io_.readByte(io_.ctx,
                           staging::kSourcePage*staging::kBytePage+i,&source) ||
                       source!=staging::pattern(i) || v!=source) {
                        state_=Code::RecoveryRequired;
                        o.status=Code::VerifyFailed;return o;
                    }
                }
                if(!io_.read(io_.ctx,kRptr,&o.finalRptr) ||
                   !io_.read(io_.ctx,kWptr,&o.finalWptr)) {
                    state_=Code::RecoveryRequired;
                    o.status=Code::RecoveryRequired;return o;
                }
                // MMIO pointers count bytes, not ring DWORDs.
                if(o.finalRptr!=kCommandsBytes ||
                   o.finalWptr!=kCommandsBytes) {
                    state_=Code::RecoveryRequired;
                    o.status=Code::VerifyFailed;return o;
                }
                state_=Code::Completed;
                o.status=Code::Completed;return o;
            }
            io_.delayUs(io_.ctx,delayUs);
        }
        // May still be DMAing! Caller MUST NOT free its IOMMU buffers.
        state_=Code::RecoveryRequired;
        o.status=Code::Timeout;return o;
#endif
    }
    // No command was ever published. Only an exclusively owned, still
    // disabled ring may be disarmed without GPU fence proof.
    Observation abortBeforeSubmit() {
#ifndef COBALT_NAVI33_ENABLE_UNSAFE_MMIO
        return {Code::NotBuilt,0u,0u,0u,0u,0u};
#else
        Observation o{};
        if(state_!=Code::Ready) {o.status=Code::Denied;return o;}
        uint32_t ctl=0u,rptr=0u,wptr=0u;
        if(!io_.read(io_.ctx,kRbCntl,&ctl) ||
           !io_.read(io_.ctx,kRptr,&rptr) ||
           !io_.read(io_.ctx,kWptr,&wptr) ||
           (ctl&kEnable) || rptr!=0u || wptr!=0u) {
            state_=Code::RecoveryRequired;
            o.status=Code::RecoveryRequired;
            return o;
        }
        if(!restoreParked(o.mmioWrites)) {
            state_=Code::RecoveryRequired;
            o.status=Code::RecoveryRequired;
            return o;
        }
        state_=Code::Denied;
        o.status=Code::Completed;
        return o;
#endif
    }
    Observation stopAfterCompleted() {
#ifndef COBALT_NAVI33_ENABLE_UNSAFE_MMIO
        return {Code::NotBuilt,0u,0u,0u,0u,0u};
#else
        Observation o{};
        if(state_!=Code::Completed) {
            o.status=Code::Denied;return o;
        }
        if(!restoreParked(o.mmioWrites)) {
            state_=Code::RecoveryRequired;
            o.status=Code::RecoveryRequired;return o;
        }
        state_=Code::Denied;
        o.status=Code::Completed;return o;
#endif
    }
    bool requiresRecovery() const {return state_==Code::RecoveryRequired;}
    // Even a completed queue or successfully restored register image does
    // not prove that the GPU stopped using its GART/IOVA backing. Only an
    // independent device reset plus VM-hub teardown can release memory.
    bool requiresIndependentResetBeforeUnmap() const {
        return touchedHardware_;
    }
    bool mayUnmapGpuMemory() const {
        return state_==Code::Denied && !touchedHardware_;
    }
private:
#ifdef COBALT_NAVI33_ENABLE_UNSAFE_MMIO
    bool restoreParked(uint32_t &writeCount) {
        const uint32_t registers[6u]={kRbBase,kRbBaseHi,kRptr,
                                       kRptrHi,kWptr,kWptrHi};
        // First disable ring and verify it is parked. Do not touch the
        // mapped memory's base while GPU is still allowed to fetch packets.
        if(!io_.write(io_.ctx,kRbCntl,oldCtl_ & ~kEnable))
            return false;
        ++writeCount;
        uint32_t ctl=0u;
        if(!io_.read(io_.ctx,kRbCntl,&ctl) || (ctl&kEnable))
            return false;
        for(unsigned i=0;i<6u;++i) {
            if(!io_.write(io_.ctx,registers[i],savedRegisters_[i]))
                return false;
            ++writeCount;
            uint32_t verify=0u;
            if(!io_.read(io_.ctx,registers[i],&verify) ||
               verify!=savedRegisters_[i])return false;
        }
        return true;
    }
#endif
    Code state_=Code::Denied;
    bool touchedHardware_=false;
    Request req_{};
    IO io_{};
    [[maybe_unused]] uint32_t oldCtl_=0u;
    [[maybe_unused]] uint32_t savedRegisters_[7u]{};
};
} } // namespace navi33::sdma6_mmio
