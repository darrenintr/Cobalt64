// SPDX-License-Identifier: MIT
// Per-client GPU contexts on the RX 7600 (Phase B4): the core a user client calls
// for buffer create/free, GPU VA map/unmap, submit and wait, in the shape of
// amdgpu's DRM interface (and of the Navi48 N48N ABI that RADV's Darwin backend
// speaks). No hardware I/O except callbacks; the caller serializes.
//
// Each client owns VMID k (k = 8..13, the range amdgpu leaves to KFD compute; VMID 1 stays
// with the G36-G39 proofs, 2..7 were burnt on 2026-10-20 13:05 by un-ACKed flushes), a compute
// queue (queue 0/1 of MEC pipe 1..3, see pipeOf), its own four-level address space (Navi33Gmc11Vm.h)
// and its buffers, all from the kernel's VRAM heap. The queue is loaded as
// hqd_load_v11 does (KFD without a scheduler), its ring and fence live in kernel
// buffer mapped below the client's first user page,
// and a submission is amdgpu's compute IB emission: INDIRECT_BUFFER per IB with the
// VMID in its control word, then a RELEASE_MEM fence with the sequence number
// (gfx_v11_0_ring_emit_ib_compute / _emit_fence). The WPTR is 64-bit and monotonic
// (support_64bit_ptrs); the CP takes it modulo the ring.
//
// Upstream, torvalds master 2026-10-20: gmc_v11_0_emit_flush_gpu_tlb (the flush from the
// ring: WAIT_REG_MEM write-then-wait on the ring's own engine, no semaphore on GFXHUB),
// gfx_v11_0_ring_emit_reg_write_reg_wait, gfxhub_v3_0_get_invalidate_req,
// hqd_destroy_v11 (CP_HQD_DEQUEUE_REQUEST = DRAIN_PIPE, wait for CP_HQD_ACTIVE to clear),
// gfx_v11_0_constants_init (SH_MEM for VMID != 0), gfxhub_v3_0_setup_vmid_config.
//
// Memory is freed only once the GPU can no longer reach it: a buffer is unmapped
// and the TLB flushed first, and only after the client's last fence retired; a
// client is torn down only after its queue drained. Any step that cannot be
// confirmed (a TLB flush without ACK, a queue that will not dequeue, a hub fault in
// the client's VMID) quarantines the client: its memory is never freed again.
#pragma once
#include "Navi33Gfx11GfxRing.h"
#include "Navi33Gfx11VmGradient.h"
#include "Navi33Gmc11Vm.h"
#include "Navi33Nbio43Capacity.h"
namespace navi33 { namespace client_gpu {
namespace vg=vm_gradient;
namespace vh=vram_heap;
namespace gv=gmc11_vm;
namespace mq=mec_queue;
namespace kiq=gfx11_kiq;
namespace gd=gfx11_dispatch;
namespace v1=vmid1;
constexpr uint32_t kFirstVmid=8u, kClients=6u;                 // VMIDs 8..13
// Each client's HQD. Queues 2 and up of every pipe belong to MES (amdgpu_mes:
// compute_hqd_mask 0xc; MES runs since the autostart): on the card (2026-10-20 09:37)
// queue 3 never took its WPTR, queue 4 was active before use, queue 5 went inactive by
// itself. Pipes 1..3 (queues 0/1) never fetched either (09:43..10:01). The only queues
// that ever executed after load are pipe 0 queues 0/1 (G34-G36, G35's re-poll), so slot 0
// is pipe 0 queue 1 (G36-G39's queue, never used with them in one power cycle), the rest
// stay on pipes 1..3 until it is known why those do not run.
constexpr uint32_t pipeOf(uint32_t slot) { return slot ? 1u+(slot-1u)/2u : 0u; }
constexpr uint32_t queueOf(uint32_t slot) { return slot ? (slot-1u)%2u : 1u; }
constexpr uint32_t pollBit(uint32_t slot) { return 1u<<(8u*pipeOf(slot)+queueOf(slot)); }   // CP_PQ_WPTR_POLL_CNTL1 mask
static_assert(pipeOf(0)==0u && queueOf(0)==1u && pipeOf(5)==3u && queueOf(5)==0u && pollBit(0)==0x2u,"client HQDs");
constexpr uint32_t kMaxBos=512u, kMaxIbs=16u;
// The client's queue lives below the first user page (Mesa's amdgpu VA manager starts at
// 0x10000 and runs to the GMC hole), page 0 left unmapped so a null GPU pointer faults.
constexpr uint64_t kUserVaMin=0x10000u, kUserVaMax=gv::kVaLimit;   // [min, max)
// The client's queue structures are mapped at a VA equal to their MC address (identity),
// so they read the same whether the CP translates them through the queue's VMID or not:
// on the card a VMID >= 2 queue never re-read its WPTR after activation (2026-10-20
// 09:30..10:08), while VMID 0 (MC addresses) did (G35). That range is closed to user maps.
constexpr uint64_t kMaxBoBytes=64ull<<20;
constexpr uint32_t kTagClient=0x434c4e54u, kTagGfxRing=0x47465852u;   // 'CLNT', 'GFXR'
// gc_11_0_0_offset.h (GC segment 0): context k registers,
// CP_HQD_DEQUEUE_REQUEST (in the HQD block, banked by GRBM select).
constexpr uint32_t ctxCntl(uint32_t k) { return 0x1688u+k; }
constexpr uint32_t ctxBaseLo(uint32_t k) { return 0x16f3u+2u*k; }
constexpr uint32_t ctxStartLo(uint32_t k) { return 0x1713u+2u*k; }
constexpr uint32_t ctxEndLo(uint32_t k) { return 0x1733u+2u*k; }
static_assert(ctxCntl(1)==0x1689u && ctxBaseLo(2)==0x16f7u && ctxStartLo(2)==0x1717u && ctxEndLo(7)+1u==0x1742u,
              "context register strides (gfxhub_v3_0 ctx_distance 1, ctx_addr_distance 2)");
constexpr uint32_t kDequeueRequest=0x1fc1u;
// TLB invalidation: gmc_v11_0_flush_gpu_tlb's MMIO path, engine 17, per VMID, wait for the
// ACK. On the card no engine ever ACKed (2026-10-20 10:26..13:21) while GCVM_L2_STATUS read
// L2_BUSY 1 and GCVM_L2_CNTL2 kept INVALIDATE_ALL_L1_TLBS | INVALIDATE_L2_CACHE set (13:58):
// our hub setup wrote those bits with GC running, where gfxhub_v3_0_init_cache_regs writes them
// at GMC init, before the RLC brings GC out of reset; the hub then never left that
// invalidation. So they are never set here, and cleared at start if found set.
// A client's VMID is flushed before its context is used (open) and after an unmap; a miss
// quarantines (fail closed).
constexpr uint32_t kInvReq17=0x16bcu, kInvAck17=0x16ceu, kL2Status=0x15bfu, kL2Busy=1u;
constexpr uint32_t kL2Cntl2Invalidate=3u;            // INVALIDATE_ALL_L1_TLBS | INVALIDATE_L2_CACHE
// gfxhub_v3_0_get_invalidate_req(vmid, 0): PER_VMID bit, FLUSH_TYPE 0, L2 PTES/PDE0/1/2, L1 PTES.
constexpr uint32_t invalidateReq(uint32_t vmid) { return (1u<<vmid)|(1u<<19)|(1u<<20)|(1u<<21)|(1u<<22)|(1u<<23); }
static_assert(invalidateReq(2)==0x00f80004u,"GCVM_INVALIDATE_ENG0_REQ word");
constexpr uint32_t kDrainPipe=1u;
constexpr uint32_t kGbAddrConfig=0x13deu;                       // regGB_ADDR_CONFIG, read only
// Doorbells (the only way upstream gfx11 kicks a compute queue): gc_11_0_0_offset.h
// CP_MEC_DOORBELL_RANGE_LOWER/UPPER (byte offsets, gfx_v11_0_kiq_init_register:
// (kiq * 2) << 2 .. (userqueue_end * 2) << 2), nbio_4_3_0_offset.h
// RCC_DEV0_EPF0_RCC_DOORBELL_APER_EN (BASE_IDX 2, bit 0, nbio_v4_3_enable_doorbell_aperture),
// amdgpu_doorbell.h AMDGPU_NAVI10_DOORBELL_KIQ 0, USERQUEUE_END 0x8a (64-bit doorbells; a
// queue's DOORBELL_OFFSET is its dword index = index << 1), and CP_PQ_STATUS.DOORBELL_ENABLE
// (bit 1), which gfx_v11_0_kiq_init_register sets after loading a doorbell queue.
// The card came up with the MEC range at 0x110..0xffc (MES); the client doorbells sit
// inside it (indices 0x40..0x45), so the range is only written when it does not cover them.
constexpr uint32_t kMecDbLower=0x1dfcu, kMecDbUpper=0x1dfdu, kNbioAperEn=0x00c0u, kCpPqStatus=0x1e58u;
constexpr uint32_t kDbFirst=0x40u, kDbUserqueueEnd=0x8au, kPqStatusDoorbellEnable=1u<<1;
constexpr uint32_t kMecDbLowerValue=0u, kMecDbUpperValue=(kDbUserqueueEnd*2u)<<2;
constexpr uint32_t doorbellDword(uint32_t slot) { return (kDbFirst+slot)<<1; }
constexpr uint32_t doorbellByte(uint32_t slot) { return 4u*doorbellDword(slot); }
constexpr uint32_t kDbOffsetMask=0x0ffffffcu, kDbEn=1u<<30;      // CP_HQD_PQ_DOORBELL_CONTROL
static_assert(kMecDbUpperValue==0x450u && doorbellByte(5)+8u<=0x450u && doorbellByte(0)==0x200u,"doorbells");                               // enum hqd_dequeue_request_type
constexpr uint32_t kFaultVmidShift=20u, kFaultVmidMask=0xf00000u;
// The queue buffer at its own MC address, mec_queue's layout: page 0 unmapped, EOP 0x1000, ring
// 0x2000 (GPU read-only), RPTR/WPTR 0x3000/0x3008, fence 0x4000.
constexpr uint32_t kQueueBytes=0x5000u, kRingDwords=mq::kRingBytes/4u;
// The client's fence for its submissions on the kernel's gfx ring, next to the compute fence.
constexpr uint32_t kGfxFenceOff=mq::kFenceOff+0x10u;
constexpr uint32_t kGfxSubmitMaxDwords=3u+2u+4u*kMaxIbs+2u+8u+8u+8u+8u;   // cntxcntl, frames, IBs, 3 fences, pad
constexpr uint32_t kSubmitMaxDwords=4u*kMaxIbs+8u+8u+8u;       // IBs, user fence, fence, padding
constexpr uint32_t kTlbTimeoutUs=100000u, kDequeueTimeoutMs=1000u;

enum class Code : uint8_t {
    Ok=0, BadArgs, NotReady, NoSlot, NoMemory, Busy, Timeout, Faulted, Quarantined, NotFound, HwFailed, Hub
};
struct Regs {
    vg::Regs g{};          // G38's: hub, context 1, SH_MEM, GRBM, SCRATCH, poll trigger, HQD block
    uint64_t ctx[kClients][7]{};    // CNTL, BASE lo/hi, START lo/hi, END lo/hi
    uint64_t dequeue=0, invReq=0, invAck=0, l2Status=0;
    gfx_ring::Regs gfx{};           // the kernel's gfx ring 0 (G40)
    uint64_t gbAddrConfig=0;        // read only (QueryInfo)
    uint64_t mecDbLower=0, mecDbUpper=0, nbioAperEn=0, pqStatus=0;
    bool valid=false;
};
inline Regs resolve(const IPDiscovery &d,uint64_t bytes) {
    Regs r{};r.g=vg::resolve(d,bytes);
    if(!r.g.valid)return r;
    auto one=[&](uint32_t dw,uint64_t &out){
        const auto a=resolveDword(d,Engine::GFX11,0,dw,bytes);
        if(a.code!=AddressCode::Pass)return false;
        out=a.bar5ByteOffset;return true;
    };
    for(uint32_t i=0;i<kClients;++i) {
        const uint32_t k=kFirstVmid+i;
        const uint32_t dws[7]={ctxCntl(k),ctxBaseLo(k),ctxBaseLo(k)+1u,ctxStartLo(k),ctxStartLo(k)+1u,ctxEndLo(k),ctxEndLo(k)+1u};
        for(int j=0;j<7;++j)if(!one(dws[j],r.ctx[i][j]))return r;
    }
    r.gfx=gfx_ring::resolve(d,bytes);
    if(!r.gfx.valid || !one(kDequeueRequest,r.dequeue) || !one(kInvReq17,r.invReq) || !one(kInvAck17,r.invAck) ||
       !one(kL2Status,r.l2Status) ||
       !one(kGbAddrConfig,r.gbAddrConfig) || !one(kMecDbLower,r.mecDbLower) || !one(kMecDbUpper,r.mecDbUpper) ||
       !one(kCpPqStatus,r.pqStatus))return r;
    IPEntry nbif{};
    if(!d.find(ipid::NBIF,0u,nbif) || nbif.major!=4u || nbif.minor!=3u || nbif.numBases<=2u || !nbif.bases[2] ||
       (nbif.bases[2]+kNbioAperEn)*4u+4u>bytes)return r;
    r.nbioAperEn=(nbif.bases[2]+kNbioAperEn)*4u;
    r.valid=true;return r;
}
// Writable registers: the GFXHUB words, invalidation engine 17's request, the gfx ring's CP registers, contexts 8..13, SH_MEM_CONFIG/BASES, GRBM select, the poll trigger and the HQD block
// (CP_HQD_DEQUEUE_REQUEST lies inside it). Never the MEC program counters/control,
// SCRATCH_REG0, context 0/1, the fault registers or anything outside GC.
inline bool allowed(const Regs &r,uint64_t off) {
    if(!r.valid || off==r.g.v.q.scratch || off==r.g.v.ctx || off==r.g.v.baseLo || off==r.g.v.baseHi ||
       off==r.g.v.startLo || off==r.g.v.startHi || off==r.g.v.endLo || off==r.g.v.endHi)return false;
    if(gfx_ring::allowed(r.gfx,off))return true;
    if(off==r.invReq || off==r.mecDbLower || off==r.mecDbUpper || off==r.nbioAperEn || off==r.pqStatus)return true;
    for(const auto &c:r.ctx)for(uint64_t o:c)if(off==o)return true;
    return vg::allowed(r.g,off);
}
struct Hw {
    void *ctx=nullptr;
    bool (*read)(void *,uint64_t,uint32_t *)=nullptr;
    bool (*write)(void *,uint64_t,uint32_t)=nullptr;
    bool (*vramWrite)(void *,uint64_t off,const uint32_t *src,uint32_t dwords)=nullptr;   // heap blocks only
    bool (*vramRead)(void *,uint64_t off,uint32_t *dst,uint32_t dwords)=nullptr;
    void (*delayUs)(void *,uint32_t)=nullptr;
    bool (*doorbell)(void *,uint32_t byteOff,uint64_t value)=nullptr;    // 64-bit write into BAR2
};
struct Bo { vh::Handle h{}; uint64_t vram=0, bytes=0, va=0; uint64_t vaFlags=0; bool live=false, cpuMapped=false; };
struct Mapping { uint64_t va=0, bytes=0; };
enum class State : uint8_t { Free=0, Open, Faulted, Quarantined };
// What the queue looked like when a wait first timed out (gc_11_0_0_offset.h HQD block,
// read under GRBM select; then the RPTR report, WPTR and fence words in the queue buffer).
constexpr uint32_t kSnapRegs[]={0x1fab,0x1fb3,0x1fdf,0x1fbb,0x1fbd,0x1fbe,0x1fc9,0x1fdc,0x1fd1,0x1fd2};
constexpr uint32_t kSnapCount=sizeof(kSnapRegs)/4u;
struct QueueSnap { bool taken=false; uint32_t reg[kSnapCount]{}; uint32_t rptrReport=0, wptrMem=0, fence=0; };
struct Client {
    State state=State::Free;
    const void *owner=nullptr;
    gv::Space space;
    Bo bo[kMaxBos]{};                       // handle = index + 1
    Bo queue{}, mqd{};
    uint64_t wptr=0;                        // dwords written to the ring, monotonic
    uint64_t emitted=0, retired=0;          // sequence numbers (the fence holds the low 32 bits)
    uint64_t seqEnd[64]{};                  // compute ring position after seq (modulo 64)
    uint64_t gfxMask=0;                     // bit seq % 64: that submission went to the gfx ring
    uint32_t vmid=0, faultStatus=0, faultAddr=0;
    QueueSnap snap{};
    bool loaded=false;          // the HQD is loaded at the first submission (see submit)
};
// The doorbell setup found at start (before any write) and after it.
struct DoorbellSetup { uint32_t aperBefore=0, lowerBefore=0, upperBefore=0, aper=0, lower=0, upper=0;
                       uint32_t pqBefore=0, pq=0, reclaimed=0; };
// The last TLB flush that got no ACK: the VMID, the request written, what REQ and ACK read back.
struct FlushMiss { uint32_t vmid=0, req=0, reqBack=0, ack=0, misses=0; };
// GCVM_L2_CNTL2 and GCVM_L2_STATUS at start, and after the invalidate bits were cleared.
struct HubL2 { uint32_t cntl2Before=0, statusBefore=0, cntl2=0, status=0; };
// The kernel's gfx ring at start: CP_ME_CNTL before and after, CP_STAT after the unhalt, the
// ring's RPTR and the ring-test fence, and how far it got (GfxState).
enum class GfxState : uint8_t { Off=0, NoMemory, Halt, Config, Resume, Unhalt, Timeout, Ok };
struct GfxReport { GfxState state=GfxState::Off; uint32_t meBefore=0, me=0, stat=0, rptr=0, fence=0, wptr=0; uint64_t ringMc=0; };
struct Stats { uint32_t clients=0, bos=0; uint64_t emitted=0, retired=0, retiredBytes=0; uint32_t writes=0, tlbFlushes=0;
               FlushMiss miss{}; HubL2 l2{}; GfxReport gfx{};
               DoorbellSetup db{}; };

class Device {
public:
    // Called once the provider owns the card and the MEC is running (autostart).
    // Sets up the GFXHUB as gfxhub_v3_0 does unless G36-G39 already did.
    Code start(const Regs &r,const Hw &h,vh::Heap &heap,uint64_t fbBaseMc) {
        if(started_)return Code::Ok;
        if(!r.valid || !h.read || !h.write || !h.vramWrite || !h.vramRead || !h.delayUs || !h.doorbell || !heap.total() || !fbBaseMc)
            return Code::BadArgs;
        r_=&r;h_=h;heap_=&heap;fbBase_=fbBaseMc;
        const v1::Regs &v=r.g.v;
        uint32_t sos=0,rlc=0,grbm=0,cntl=0,l1=0,l2=0,fault=0;
        if(!rd(r.g.v.q.sos,sos) || !rd(v.q.rlc,rlc) || !rd(v.q.grbm,grbm) || !rd(v.q.cntl,cntl) ||
           !rd(v.l1,l1) || !rd(v.l2,l2) || !rd(v.faultStatus,fault))return Code::HwFailed;
        if(!sos || !(rlc&0x80000000u) || grbm || cntl!=0x3c000000u)return Code::NotReady;
        if(l1==v1::kL1Default && l2==v1::kL2Default) {
            uint32_t c2=0;
            if(!rd(v.l2c2,c2))return Code::HwFailed;
            const struct { uint64_t off; uint32_t val; } hub[]={
                {v.l1,v1::l1Word(l1)},{v.l2,v1::l2Word(l2)},{v.l2c2,c2&~kL2Cntl2Invalidate},
                {v.l2c3,v1::kL2Cntl3Value},{v.l2c4,v1::kL2Cntl4Value},{v.l2c5,v1::kL2Cntl5Value},
                {v.ident[0],v1::kIdentity[0]},{v.ident[1],v1::kIdentity[1]},{v.ident[2],v1::kIdentity[2]},
                {v.ident[3],v1::kIdentity[3]},{v.ident[4],v1::kIdentity[4]},{v.ident[5],v1::kIdentity[5]}};
            for(const auto &x:hub)if(!wr(x.off,x.val))return Code::HwFailed;
        } else if(l1!=v1::l1Word(v1::kL1Default) || l2!=v1::l2Word(v1::kL2Default)) {
            return Code::Hub;                // a hub we do not recognise: touch nothing
        }
        if(fault)faultSeen_=fault;
        // The hub L2: leave no invalidation latched (see kInvReq17's note), give it 10 ms to go idle.
        if(!rd(v.l2c2,l2_.cntl2Before) || !rd(r.l2Status,l2_.statusBefore))return Code::HwFailed;
        if((l2_.cntl2Before&kL2Cntl2Invalidate) && !wr(v.l2c2,l2_.cntl2Before&~kL2Cntl2Invalidate))return Code::HwFailed;
        for(uint32_t us=0;;us+=100u) {
            if(!rd(v.l2c2,l2_.cntl2) || !rd(r.l2Status,l2_.status))return Code::HwFailed;
            if(!(l2_.status&kL2Busy) || us>=10000u)break;
            h.delayUs(h.ctx,100u);
        }
        // Doorbells: the MEC range as gfx_v11_0_kiq_init_register, the NBIO aperture as
        // nbio_v4_3_enable_doorbell_aperture; only what is not already so is written.
        uint32_t aper=0,lower=0,upper=0,pq=0;
        if(!rd(r.nbioAperEn,aper) || !rd(r.mecDbLower,lower) || !rd(r.mecDbUpper,upper) || !rd(r.pqStatus,pq))
            return Code::HwFailed;
        db_={aper,lower,upper,aper,lower,upper,pq,pq,0u};
        const bool covered=lower<=doorbellByte(0) && upper>=doorbellByte(kClients-1u)+8u;
        if(!covered && (!wr(r.mecDbLower,kMecDbLowerValue) || !wr(r.mecDbUpper,kMecDbUpperValue)))return Code::HwFailed;
        if(!(aper&1u) && !wr(r.nbioAperEn,aper|1u))return Code::HwFailed;
        if(!rd(r.nbioAperEn,db_.aper) || !rd(r.mecDbLower,db_.lower) || !rd(r.mecDbUpper,db_.upper))return Code::HwFailed;
        if(!(db_.aper&1u) || db_.lower>doorbellByte(0) || db_.upper<doorbellByte(kClients-1u)+8u)return Code::HwFailed;
        for(uint32_t i=0;i<kHqdDumpQueues;++i)hqdStartOk_[i]=hqdDump(i,hqdStart_[i]);
        gfxStart();                       // the gfx pipe: compute does not depend on it
        // Client queues an earlier boot of the kext left active (stuck work) are drained
        // and dequeued now, as close() would have; one that will not go is retired.
        for(uint32_t i=0;i<kClients;++i) {
            uint32_t active=0;
            if(!sel(qsel(i)) || !rd(mq::hqd(r.g.v.q,kiq::kCpHqdActive),active) || !sel(0u))return Code::HwFailed;
            if(!(active&1u))continue;
            Client &c=c_[i];c.vmid=kFirstVmid+i;
            if(dequeue(c))++db_.reclaimed; else c.state=State::Quarantined;
        }
        started_=true;return Code::Ok;
    }
    bool started() const { return started_; }
    // Diagnostics only (boot-arg navi33-client-tlb-unsafe=1): a TLB flush that gets no ACK is
    // recorded but not fatal, so user space can be debugged on a hub that never ACKs. Stale
    // translations may then reach freed memory; never set by default or on the stick.
    void setTlbUnsafe(bool on) { tlbUnsafe_=on; }
    // Diagnostics (2026-10-20: a client HQD that already finished work stalls after a reload):
    // the whole HQD block CP_MQD_BASE_ADDR..CP_HQD_DDID_WPTR of MEC pipes 0..3 queues 0/1
    // except pipe 0 queue 0 (G34's), read under GRBM select. Reads only.
    static constexpr uint32_t kHqdDumpFirst=0x1fa9u, kHqdDumpLast=0x1fe5u,
                              kHqdDumpCount=kHqdDumpLast-kHqdDumpFirst+1u, kHqdDumpQueues=8u;
    static constexpr uint32_t hqdDumpSel(uint32_t i) { return mq::grbmSelect(mq::kMeMec,i/2u,i%2u); }
    bool hqdDump(uint32_t i,uint32_t out[kHqdDumpCount]) {
        if(!r_ || i>=kHqdDumpQueues || !i || !sel(hqdDumpSel(i)))return false;
        const uint64_t base=mq::hqd(r_->g.v.q,kHqdDumpFirst);
        bool ok=true;
        for(uint32_t k=0;k<kHqdDumpCount;++k)ok=h_.read(h_.ctx,base+4u*k,&out[k]) && ok;
        return sel(0u) && ok;
    }
    const uint32_t *hqdAtStart(uint32_t i) const { return i<kHqdDumpQueues && hqdStartOk_[i] ? hqdStart_[i] : nullptr; }
    // A new client: VMID k, address space, queue buffers mapped, context k, SH_MEM, queue k loaded.
    Code open(const void *owner,uint32_t &slot) {
        if(!started_)return Code::NotReady;
        if(!owner)return Code::BadArgs;
        slot=kClients;
        for(uint32_t i=0;i<kClients && slot==kClients;++i) {
            if(c_[i].state!=State::Free)continue;
            // A queue an earlier boot of the kext left active (stuck work) is never reused.
            uint32_t active=0;
            if(!sel(qsel(i)) || !rd(mq::hqd(r_->g.v.q,kiq::kCpHqdActive),active) || !sel(0u))
                return Code::HwFailed;
            if(active&1u){c_[i].state=State::Quarantined;c_[i].vmid=kFirstVmid+i;continue;}
            slot=i;
        }
        if(slot==kClients)return Code::NoSlot;
        Client &c=c_[slot];
        resetClient(c);
        c.vmid=kFirstVmid+slot;c.owner=owner;
        const vh::Alloc q=heap_->alloc(kQueueBytes,4096u,vh::Place::Visible,kTagClient);
        const vh::Alloc m=q.code==vh::Code::Ok ? heap_->alloc(4096u,4096u,vh::Place::Visible,kTagClient) : vh::Alloc{};
        if(q.code!=vh::Code::Ok || m.code!=vh::Code::Ok) {
            if(q.code==vh::Code::Ok)heap_->free(q.h);
            return Code::NoMemory;
        }
        c.queue={q.h,q.offset,kQueueBytes,fbBase_+q.offset,0,true};c.mqd={m.h,m.offset,4096u,0,0,true};
        tio_.dev=this;
        const gv::Io io{&tio_,zeroPage,putEntry};
        if(!zero(c.queue) || !zero(c.mqd) || c.space.init(*heap_,io)!=gv::Code::Ok ||
           // EOP, ring, RPTR/WPTR, fence as G36 mapped its queue (VRAM, RWX); page 0 unmapped.
           c.space.map(c.queue.va+mq::kEopOff,q.offset+mq::kEopOff,0x4000u,gv::kPteVram)!=gv::Code::Ok) {
            quarantineIfMapped(c);        // nothing is live on the GPU yet: free what we can
            return Code::NoMemory;
        }
        c.state=State::Open;
        // Context k (Linux's fault defaults, depth 3), SH_MEM for VMID k, then the queue.
        const uint64_t *cr=r_->ctx[slot];
        const uint64_t base=c.space.rootVram()|gv::kPdeValid;
        uint32_t cntl=0;
        if(!rd(cr[0],cntl))return fail(c,Code::HwFailed);
        const struct { uint64_t off; uint32_t val; } ctx[]={
            {cr[1],static_cast<uint32_t>(base)},{cr[2],static_cast<uint32_t>(base>>32)},{cr[3],0u},{cr[4],0u},
            {cr[5],vg::kEndLo},{cr[6],vg::kEndHi},{cr[0],vg::ctx1Word(cntl&~1u)}};
        for(const auto &x:ctx)if(!wr(x.off,x.val))return fail(c,Code::HwFailed);
        if(!sel(c.vmid<<4) || !wr(r_->g.shMem,gradient::kShMemDefault) || !wr(r_->g.shBases,vg::kShMemBasesValue) ||
           !sel(0u))return fail(c,Code::HwFailed);
        // A previous client of this VMID may have left translations behind.
        if(!flushTlb(c.vmid) && !tlbUnsafe_)return fail(c,Code::HwFailed);
        return Code::Ok;
    }
    // A zeroed, CPU-visible VRAM buffer. Returns handle 1..kMaxBos.
    Code boCreate(uint32_t slot,uint64_t bytes,uint32_t &handle,uint64_t &vram,uint64_t align=4096u) {
        Client *c=live(slot);
        if(!c)return Code::NotFound;
        if(c->state!=State::Open)return stateCode(*c);
        if(!bytes || bytes>kMaxBoBytes || align>(2u<<20) || (align&(align-1u)))return Code::BadArgs;
        if(align<4096u)align=4096u;
        bytes=(bytes+4095u)&~4095ull;
        uint32_t i=0;
        while(i<kMaxBos && c->bo[i].live)++i;
        if(i==kMaxBos)return Code::NoSlot;
        const vh::Alloc a=heap_->alloc(bytes,align,vh::Place::Visible,kTagClient);
        if(a.code!=vh::Code::Ok)return Code::NoMemory;
        c->bo[i]={a.h,a.offset,bytes,0,0,true};
        if(!zero(c->bo[i])){heap_->free(a.h);c->bo[i]=Bo{};return Code::HwFailed;}
        handle=i+1u;vram=a.offset;return Code::Ok;
    }
    // Map the whole buffer at va. Adding valid PTEs needs no TLB flush on GFX9+ (the
    // hub does not cache invalid entries; amdgpu_vm_update_range); unmapping does.
    Code vaMap(uint32_t slot,uint32_t handle,uint64_t va,bool writeable,bool executable) {
        Client *c=live(slot);Bo *b=c ? bo(*c,handle) : nullptr;
        if(!c || !b)return Code::NotFound;
        if(c->state!=State::Open)return stateCode(*c);
        if(b->va || (va%4096u) || va<kUserVaMin || va>=kUserVaMax || b->bytes>kUserVaMax-va)return Code::BadArgs;
        for(const Bo &o:c->bo)if(o.live && o.va && va<o.va+o.bytes && o.va<va+b->bytes)return Code::BadArgs;
        if(va<c->queue.va+kQueueBytes && c->queue.va<va+b->bytes)return Code::BadArgs;       // the kernel's queue
        const uint64_t flags=gv::kPteValid|gv::kPteRead|(writeable ? gv::kPteWrite : 0u)|(executable ? gv::kPteExec : 0u);
        if(c->space.map(va,b->vram,b->bytes,flags)!=gv::Code::Ok) {
            c->space.unmap(va,b->bytes);   // partial mappings point at our own buffer only
            return Code::NoMemory;
        }
        b->va=va;b->vaFlags=flags;
        return Code::Ok;
    }
    Code vaUnmap(uint32_t slot,uint32_t handle) {
        Client *c=live(slot);Bo *b=c ? bo(*c,handle) : nullptr;
        if(!c || !b)return Code::NotFound;
        if(c->state==State::Quarantined)return Code::Quarantined;
        if(!b->va)return Code::BadArgs;
        const Code idle=waitIdle(*c);
        if(idle!=Code::Ok)return idle;
        if(c->space.unmap(b->va,b->bytes)!=gv::Code::Ok || (!flushTlb(c->vmid) && !tlbUnsafe_)){quarantine(*c);return Code::Quarantined;}
        b->va=0;b->vaFlags=0;return Code::Ok;
    }
    Code boFree(uint32_t slot,uint32_t handle) {
        Client *c=live(slot);Bo *b=c ? bo(*c,handle) : nullptr;
        if(!c || !b)return Code::NotFound;
        if(b->va){const Code u=vaUnmap(slot,handle);if(u!=Code::Ok)return u;}
        if(c->state==State::Quarantined)return Code::Quarantined;
        release(*b);return Code::Ok;
    }
    // A user CPU mapping may outlive BO_FREE and the context (it belongs to the task
    // until IOKit tears it down), so a buffer that is CPU-mapped is never given back to
    // the heap: freeing it retires it. The user client revokes its mappings first
    // (IOUserClient::removeMappingForDescriptor + unmap) and then calls
    // clearCpuMapped(), after which the pages are reusable again.
    bool markCpuMapped(uint32_t slot,uint32_t handle) {
        Client *c=live(slot);Bo *b=c ? bo(*c,handle) : nullptr;
        if(!c || !b || c->state==State::Quarantined)return false;
        b->cpuMapped=true;return true;
    }
    // handle 0: every buffer of the client.
    bool clearCpuMapped(uint32_t slot,uint32_t handle) {
        Client *c=live(slot);
        if(!c)return false;
        if(!handle){for(Bo &b:c->bo)b.cpuMapped=false;return true;}
        Bo *b=bo(*c,handle);
        if(!b)return false;
        b->cpuMapped=false;return true;
    }
    // The CPU-visible VRAM range of a buffer (for the user client's CPU mapping).
    bool boRange(uint32_t slot,uint32_t handle,uint64_t &vram,uint64_t &bytes) {
        Client *c=live(slot);Bo *b=c ? bo(*c,handle) : nullptr;
        if(!c || !b || c->state==State::Quarantined)return false;
        vram=b->vram;bytes=b->bytes;return true;
    }
    struct Ib { uint64_t va=0; uint32_t dwords=0; };
    // Append the IBs and a fence to the client's ring and kick the queue. userFence
    // (optional, 8-byte aligned, inside a writeable mapping) also receives the 64-bit
    // sequence number, as amdgpu's cs_chunk_fence.
    Code submit(uint32_t slot,const Ib *ibs,uint32_t n,uint64_t &seq,uint64_t userFence=0) {
        Client *c=live(slot);
        if(!c)return Code::NotFound;
        if(c->state!=State::Open)return stateCode(*c);
        if(!ibs || !n || n>kMaxIbs)return Code::BadArgs;
        for(uint32_t i=0;i<n;++i)
            if(!ibs[i].dwords || ibs[i].dwords>=(1u<<20) || (ibs[i].va&3u) ||
               !mapped(*c,ibs[i].va,4ull*ibs[i].dwords,gv::kPteRead|gv::kPteExec))return Code::BadArgs;   // the CP fetches IBs as code
        if(userFence && ((userFence&7u) || !mapped(*c,userFence,8u,gv::kPteWrite)))return Code::BadArgs;
        poll(*c);
        if(c->state!=State::Open)return stateCode(*c);
        if(c->emitted-c->retired>=63u || c->wptr+kSubmitMaxDwords-retiredWptr(*c)>kRingDwords)return Code::Busy;
        uint32_t p[kSubmitMaxDwords];uint32_t k=0;
        for(uint32_t i=0;i<n;++i) {
            p[k++]=gd::packet3(kIndirectBuffer,2u);
            p[k++]=static_cast<uint32_t>(ibs[i].va);p[k++]=static_cast<uint32_t>(ibs[i].va>>32)&0xffffu;
            p[k++]=kIbValid|ibs[i].dwords|(c->vmid<<24);
        }
        const uint64_t s=c->emitted+1u, fence=c->queue.va+mq::kFenceOff;
        if(userFence) {     // RELEASE_MEM DATA_SEL 2: the 64-bit sequence number
            const uint32_t u[8]={gd::packet3(gd::kReleaseMem,6u),gd::kReleaseMemControl,2u<<29,
                                 static_cast<uint32_t>(userFence),static_cast<uint32_t>(userFence>>32),
                                 static_cast<uint32_t>(s),static_cast<uint32_t>(s>>32),0u};
            for(uint32_t v:u)p[k++]=v;
        }
        const uint32_t rel[8]={gd::packet3(gd::kReleaseMem,6u),gd::kReleaseMemControl,gd::kReleaseMemDataSel32,
                               static_cast<uint32_t>(fence),static_cast<uint32_t>(fence>>32),static_cast<uint32_t>(s),0u,0u};
        for(uint32_t v:rel)p[k++]=v;
        c->gfxMask&=~(1ull<<(s%64u));
        if(!push(*c,p,k)){quarantine(*c);return Code::Quarantined;}
        seq=s;return Code::Ok;
    }
    bool gfxReady() const { return gfx_.state==GfxState::Ok; }
    // A graphics submission (G40): the IBs run on the kernel's gfx ring in the client's VMID, as
    // amdgpu_ib_schedule() emits them; the client's gfx fence (and its user fence) are written at MC
    // addresses, since the ring itself runs in VMID 0. Sequence numbers are shared with the client's
    // compute submissions and retire in order across both (poll).
    Code submitGfx(uint32_t slot,const Ib *ibs,uint32_t n,uint64_t &seq,uint64_t userFence=0) {
        namespace gr=gfx_ring;
        Client *c=live(slot);
        if(!c)return Code::NotFound;
        if(c->state!=State::Open)return stateCode(*c);
        if(!gfxReady())return Code::NotReady;
        if(!ibs || !n || n>kMaxIbs)return Code::BadArgs;
        for(uint32_t i=0;i<n;++i)
            if(!ibs[i].dwords || ibs[i].dwords>=(1u<<20) || (ibs[i].va&3u) ||
               !mapped(*c,ibs[i].va,4ull*ibs[i].dwords,gv::kPteRead|gv::kPteExec))return Code::BadArgs;
        uint64_t ufMc=0;
        if(userFence && ((userFence&7u) || !mapped(*c,userFence,8u,gv::kPteWrite) || !vaToMc(*c,userFence,ufMc)))
            return Code::BadArgs;
        poll(*c);gfxPoll();
        if(c->state!=State::Open)return stateCode(*c);
        if(c->emitted-c->retired>=63u || gfxSeq_-gfxRetired_>=63u ||
           gfxWptr_+kGfxSubmitMaxDwords-gfxEnd_[gfxRetired_%64u]>gr::kRingDwords)return Code::Busy;
        const uint64_t s=c->emitted+1u, ks=gfxSeq_+1u, mem=fbBase_+gfxMemVram_;
        uint32_t p[kGfxSubmitMaxDwords];uint32_t k=0;
        p[k++]=gd::packet3(gr::kContextControl,1u);p[k++]=gr::kCntxCntlSwitch;p[k++]=0u;
        p[k++]=gd::packet3(gr::kFrameControl,0u);p[k++]=gr::kFrameStart;
        for(uint32_t i=0;i<n;++i) {
            p[k++]=gd::packet3(kIndirectBuffer,2u);
            p[k++]=static_cast<uint32_t>(ibs[i].va);p[k++]=static_cast<uint32_t>(ibs[i].va>>32)&0xffffu;
            p[k++]=gr::ibControl(ibs[i].dwords,c->vmid);
        }
        p[k++]=gd::packet3(gr::kFrameControl,0u);p[k++]=gr::kFrameEnd;
        if(userFence) {
            const uint32_t u[8]={gd::packet3(gd::kReleaseMem,6u),gd::kReleaseMemControl,2u<<29,
                                 static_cast<uint32_t>(ufMc),static_cast<uint32_t>(ufMc>>32),
                                 static_cast<uint32_t>(s),static_cast<uint32_t>(s>>32),0u};
            for(uint32_t v:u)p[k++]=v;
        }
        k+=gr::buildFence(p+k,fbBase_+c->queue.vram+kGfxFenceOff,static_cast<uint32_t>(s));
        k+=gr::buildFence(p+k,mem+gr::kKernelFenceOff,static_cast<uint32_t>(ks));
        while(k%8u)p[k++]=mq::kNop;
        for(uint32_t i=0;i<k;++i) {
            const uint32_t at=static_cast<uint32_t>((gfxWptr_+i)%gr::kRingDwords);
            if(!vramPut(gfxMemVram_+gr::kRingOff+4u*at,&p[i],1u))return Code::HwFailed;
        }
        const uint64_t end=gfxWptr_+k;
        const uint32_t wp[2]={static_cast<uint32_t>(end),static_cast<uint32_t>(end>>32)};
        if(!vramPut(gfxMemVram_+gr::kWptrOff,wp,2u) || !h_.doorbell(h_.ctx,gr::kDoorbellByte,end))return Code::HwFailed;
        gfxWptr_=end;gfxSeq_=ks;gfxEnd_[ks%64u]=end;
        c->seqEnd[s%64u]=c->wptr;c->gfxMask|=1ull<<(s%64u);c->emitted=s;
        seq=s;return Code::Ok;
    }
    // Wait until seq retired (timeoutUs 0: just poll once).
    Code wait(uint32_t slot,uint64_t seq,uint32_t timeoutUs,uint64_t &retired) {
        Client *c=live(slot);
        if(!c)return Code::NotFound;
        if(seq>c->emitted)return Code::BadArgs;
        for(uint32_t us=0;;us+=100u) {
            poll(*c);
            retired=c->retired;
            if(c->retired>=seq)return Code::Ok;
            if(c->state!=State::Open)return stateCode(*c);
            if(us>=timeoutUs){if(timeoutUs && !c->snap.taken)snapshot(*c);return Code::Timeout;}
            h_.delayUs(h_.ctx,100u);
        }
    }
    // Drain, dequeue, disable context k, flush, free everything. A client that
    // cannot be drained or dequeued is quarantined (its memory stays reserved).
    Code close(uint32_t slot) {
        Client *c=live(slot);
        if(!c)return Code::NotFound;
        if(c->state==State::Quarantined){c->owner=nullptr;return Code::Quarantined;}
        if(c->state==State::Open && waitIdle(*c)!=Code::Ok){quarantine(*c);c->owner=nullptr;return Code::Quarantined;}
        if(c->loaded && !dequeue(*c)){quarantine(*c);c->owner=nullptr;return Code::Quarantined;}
        uint32_t cntl=0;
        if(!rd(r_->ctx[slot][0],cntl) || !wr(r_->ctx[slot][0],cntl&~1u))
            {quarantine(*c);c->owner=nullptr;return Code::Quarantined;}
        for(Bo &b:c->bo)if(b.live)release(b);
        heap_->free(c->queue.h);heap_->free(c->mqd.h);
        c->space.destroy();
        resetClient(*c);
        return Code::Ok;
    }
    // The slot owned by this user client, if any.
    bool slotOf(const void *owner,uint32_t &slot) const {
        for(uint32_t i=0;i<kClients;++i)if(c_[i].state!=State::Free && c_[i].owner==owner){slot=i;return true;}
        return false;
    }
    const Client *client(uint32_t slot) const { return slot<kClients ? &c_[slot] : nullptr; }
    // Refresh and report a client's sequence numbers (for WaitSeq / QueryInfo).
    bool sequences(uint32_t slot,uint64_t &emitted,uint64_t &retired) {
        Client *c=live(slot);
        if(!c)return false;
        poll(*c);emitted=c->emitted;retired=c->retired;return true;
    }
    // The GPU VA and size a buffer is mapped at (0 if unmapped).
    bool boMapping(uint32_t slot,uint32_t handle,uint64_t &va,uint64_t &bytes) {
        Client *c=live(slot);Bo *b=c ? bo(*c,handle) : nullptr;
        if(!b)return false;
        va=b->va;bytes=b->bytes;return true;
    }
    Stats stats() const {
        Stats s{};
        for(const Client &c:c_) {
            if(c.state==State::Free)continue;
            ++s.clients;s.emitted+=c.emitted;s.retired+=c.retired;
            for(const Bo &b:c.bo)if(b.live)++s.bos;
        }
        s.writes=writes_;s.tlbFlushes=flushes_;s.retiredBytes=retiredBytes_;s.db=db_;s.miss=miss_;s.l2=l2_;s.gfx=gfx_;return s;
    }
private:
    static constexpr uint32_t kIndirectBuffer=0x3fu, kIbValid=1u<<23;   // nvd.h
    static constexpr uint32_t qsel(uint32_t slot) { return mq::grbmSelect(mq::kMeMec,pipeOf(slot),queueOf(slot)); }
    uint32_t index(const Client &c) const { return static_cast<uint32_t>(&c-c_); }
    struct TableIo { Device *dev=nullptr; };
    bool rd(uint64_t off,uint32_t &v) { return h_.read(h_.ctx,off,&v) && v!=0xffffffffu; }
    bool wr(uint64_t off,uint32_t v) {
        if(!allowed(*r_,off) || !h_.write(h_.ctx,off,v))return false;
        ++writes_;if(off==r_->g.v.q.grbm)select_=v;
        return true;
    }
    bool sel(uint32_t v) { return select_==v || wr(r_->g.v.q.grbm,v); }
    bool vramPut(uint64_t off,const uint32_t *src,uint32_t n) {
        if(!h_.vramWrite(h_.ctx,off,src,n))return false;
        for(uint32_t i=0;i<n;++i){uint32_t v=0;if(!h_.vramRead(h_.ctx,off+4u*i,&v,1u) || v!=src[i])return false;}
        return true;
    }
    bool zero(const Bo &b) {
        static const uint32_t z[1024]={};
        for(uint64_t p=0;p<b.bytes;p+=4096u)if(!h_.vramWrite(h_.ctx,b.vram+p,z,1024u))return false;
        return true;
    }
    static bool zeroPage(void *c,uint64_t off) {
        static const uint32_t z[1024]={};
        Device *d=static_cast<TableIo *>(c)->dev;return d->h_.vramWrite(d->h_.ctx,off,z,1024u);
    }
    static bool putEntry(void *c,uint64_t off,uint64_t value) {
        const uint32_t dw[2]={static_cast<uint32_t>(value),static_cast<uint32_t>(value>>32)};
        return static_cast<TableIo *>(c)->dev->vramPut(off,dw,2u);
    }
    bool waitCpIdle() {
        for(uint32_t us=0;;us+=10u) {
            if(!rd(r_->gfx.cpStat,gfx_.stat))return false;
            if(!gfx_.stat)return true;
            if(us>=gfx_ring::kStatTimeoutUs)return false;
            h_.delayUs(h_.ctx,10u);
        }
    }
    // G40: gfx_v11_0_config_gfx_rs64 (PFP/ME), cp_gfx_resume (RB0, VMID 0), cp_gfx_start and a
    // RELEASE_MEM ring test, in that order; gfx_.state says how far it got.
    void gfxStart() {
        namespace gr=gfx_ring;
        const gr::Regs &g=r_->gfx;
        gfx_={};
        if(!rd(g.meCntl,gfx_.meBefore))return;
        const vh::Alloc a=heap_->alloc(gr::kMemBytes,4096u,vh::Place::Visible,kTagGfxRing);
        if(a.code!=vh::Code::Ok){gfx_.state=GfxState::NoMemory;return;}
        gfxMem_=a.h;gfxMemVram_=a.offset;
        const Bo m{a.h,a.offset,gr::kMemBytes,0,0,true,false};
        if(!zero(m)){gfx_.state=GfxState::NoMemory;return;}
        const uint64_t mc=fbBase_+a.offset;gfx_.ringMc=mc+gr::kRingOff;
        // A gfx pipe an earlier kext boot left running is halted first (gfx_v11_0_cp_gfx_enable(false)).
        gfx_.state=GfxState::Halt;
        uint32_t me=gfx_.meBefore;
        if((me&(gr::kPfpHalt|gr::kMeHalt))!=(gr::kPfpHalt|gr::kMeHalt)) {
            me|=gr::kPfpHalt|gr::kMeHalt;
            if(!wr(g.meCntl,me))return;
            waitCpIdle();                 // gfx_v11_0_cp_gfx_enable(false) only logs a CP_STAT timeout
        }
        gfx_.state=GfxState::Config;
        for(uint32_t pipe=0;pipe<gr::kGfxPipes;++pipe)
            if(!sel(mq::grbmSelect(0u,pipe,0u)) || !wr(g.pfpStart,gr::kPcStart) || !wr(g.pfpStartHi,gr::kPcStartHi))return;
        // REG_SET_FIELD(.., PIPEn_RESET, 1) then 0: the card came up with all four resets set
        // (CP_ME_CNTL 0x153c0000, 2026-10-20), so the release clears them explicitly.
        if(!sel(0u) || !wr(g.meCntl,me|gr::kPfpReset) || !wr(g.meCntl,me&=~gr::kPfpReset))return;
        for(uint32_t pipe=0;pipe<gr::kGfxPipes;++pipe)
            if(!sel(mq::grbmSelect(0u,pipe,0u)) || !wr(g.meStart,gr::kPcStart) || !wr(g.meStartHi,gr::kPcStartHi))return;
        if(!sel(0u) || !wr(g.meCntl,me|gr::kMeReset) || !wr(g.meCntl,me&=~gr::kMeReset))return;
        gfx_.state=GfxState::Resume;
        const uint64_t rptr=mc+gr::kRptrOff, wptr=mc+gr::kWptrOff, base=gfx_.ringMc>>8;
        uint32_t dbc=0;
        if(!wr(g.wptrDelay,0u) || !wr(g.rbVmid,0u) || !wr(g.rb0Cntl,gr::kRb0CntlValue) || !wr(g.rb0Wptr,0u) ||
           !wr(g.rb0WptrHi,0u) || !wr(g.rb0RptrAddr,static_cast<uint32_t>(rptr)) ||
           !wr(g.rb0RptrAddrHi,static_cast<uint32_t>(rptr>>32)&gr::kRptrAddrHiMask) ||
           !wr(g.wptrPollLo,static_cast<uint32_t>(wptr)) || !wr(g.wptrPollHi,static_cast<uint32_t>(wptr>>32)))return;
        h_.delayUs(h_.ctx,1000u);
        if(!wr(g.rb0Cntl,gr::kRb0CntlValue) || !wr(g.rb0Base,static_cast<uint32_t>(base)) ||
           !wr(g.rb0BaseHi,static_cast<uint32_t>(base>>32)) || !wr(g.rbActive,1u) || !rd(g.rbDoorbellCntl,dbc) ||
           !wr(g.rbDoorbellCntl,(dbc&~gr::kDoorbellOffsetMask)|(gr::kDoorbellDword<<2)|gr::kDoorbellEn) ||
           !wr(g.rbDbLower,gr::kRbRangeLower) || !wr(g.rbDbUpper,gr::kRbRangeUpper))return;
        // The MEC range must end below the gfx doorbell (the card had MES's 0x110..0xffc).
        uint32_t mecUpper=0;
        if(!rd(r_->mecDbUpper,mecUpper) ||
           (mecUpper>gr::kMecRangeUpper && (!wr(r_->mecDbLower,kMecDbLowerValue) || !wr(r_->mecDbUpper,gr::kMecRangeUpper))))return;
        gfx_.state=GfxState::Unhalt;
        if(!wr(g.maxContext,gr::kMaxHwContexts-1u) || !wr(g.deviceId,1u) ||
           !wr(g.meCntl,me&~(gr::kPfpHalt|gr::kMeHalt)))return;
        waitCpIdle();                     // as gfx_v11_0_cp_gfx_enable: a busy CP_STAT is only noted (MES keeps the CP busy)
        rd(g.meCntl,gfx_.me);
        // The clear-state preamble and the ring test, then WPTR by register.
        static uint32_t p[gfx11_clear_state::kCsbDwords+16u];
        uint32_t k=gr::buildCsb(p);
        k+=gr::buildFence(p+k,mc+gr::kFenceOff,gr::kFenceValue);
        while(k%8u)p[k++]=mq::kNop;
        const uint32_t wp[2]={k,0u};
        if(!vramPut(gfxMemVram_+gr::kRingOff,p,k) || !vramPut(gfxMemVram_+gr::kWptrOff,wp,2u) ||
           !h_.doorbell(h_.ctx,gr::kDoorbellByte,k))return;
        gfxWptr_=k;gfxSeq_=gfxRetired_=0;for(uint64_t &e:gfxEnd_)e=k;
        gfx_.wptr=k;gfx_.state=GfxState::Timeout;
        for(uint32_t us=0;us<gr::kFenceTimeoutUs;us+=100u) {
            h_.vramRead(h_.ctx,gfxMemVram_+gr::kFenceOff,&gfx_.fence,1u);
            if(gfx_.fence==gr::kFenceValue){gfx_.state=GfxState::Ok;break;}
            h_.delayUs(h_.ctx,100u);
        }
        rd(g.rb0Rptr,gfx_.rptr);
    }
    // Write p (whole packets, a multiple of 8 dwords, ending in the fence of seq emitted+1)
    // to the ring, publish the WPTR and start the queue.
    bool push(Client &c,const uint32_t *p,uint32_t k) {
        for(uint32_t i=0;i<k;++i) {           // the ring wraps per dword
            const uint32_t at=static_cast<uint32_t>((c.wptr+i)%kRingDwords);
            if(!vramPut(c.queue.vram+mq::kRingOff+4u*at,&p[i],1u))return false;
        }
        const uint64_t end=c.wptr+k, s=c.emitted+1u;
        const uint32_t wp[2]={static_cast<uint32_t>(end),static_cast<uint32_t>(end>>32)};
        if(!vramPut(c.queue.vram+mq::kWptrOff,wp,2u))return false;
        c.wptr=end;c.emitted=s;c.seqEnd[s%64u]=end;
        // The first submission loads the HQD with its WPTR already in memory: the fetch at
        // activation is the one path proven for a VMID != 0 queue (G36). Later ones kick.
        if(!c.loaded) {
            if(loadQueue(c)!=Code::Ok){sel(0u);return false;}
            c.loaded=true;return true;
        }
        // Kick: the queue's doorbell, as gfx_v11_0_ring_set_wptr_compute, and the one-shot
        // WPTR poll that moved G35's queue. On the card (2026-10-20 09:37..10:01) neither
        // alone moved a client queue on pipes 1..3 or pipe 0 queue >= 2.
        const uint32_t slot=index(c);
        return h_.doorbell(h_.ctx,doorbellByte(slot),end) && sel(qsel(slot)) && wr(r_->g.v.q.poll1,pollBit(slot)) && sel(0u);
    }
    // gmc_v11_0_flush_gpu_tlb, MMIO path: engine 17, request, wait for the VMID's ACK bit.
    bool flushTlb(uint32_t vmid) {
        if(!wr(r_->invReq,invalidateReq(vmid)))return false;
        uint32_t ack=0;
        for(uint32_t us=0;us<kTlbTimeoutUs;us+=10u) {
            if(!h_.read(h_.ctx,r_->invAck,&ack))break;
            if(ack!=0xffffffffu && (ack&(1u<<vmid))){++flushes_;return true;}
            h_.delayUs(h_.ctx,10u);
        }
        uint32_t back=0;
        h_.read(h_.ctx,r_->invReq,&back);
        miss_={vmid,invalidateReq(vmid),back,ack,miss_.misses+1u};
        return false;
    }
    // hqd_load_v11 for queue k in VMID k (as G38, with the poll bit of queue k).
    Code loadQueue(Client &c) {
        const gfx11_mqd::Result m=gfx11_mqd::buildKiqMqd(mq::queueFor(mq::Layout{c.queue.va}));
        if(m.code!=kiq::Code::PlanOnly)return Code::HwFailed;
        gfx11_mqd::Image img=m.image;
        const uint64_t mqdMc=fbBase_+c.mqd.vram;
        img.dw[gfx11_mqd::kCpMqdBaseAddrLo]=static_cast<uint32_t>(mqdMc&0xfffffffcu);
        img.dw[gfx11_mqd::kCpMqdBaseAddrHi]=static_cast<uint32_t>(mqdMc>>32);
        img.dw[gfx11_mqd::kCpHqdVmid]=c.vmid;
        if(!vramPut(c.mqd.vram,img.dw,gfx11_mqd::kDwords))return Code::HwFailed;
        const mq::Regs &q=r_->g.v.q;
        if(!sel(qsel(index(c))))return Code::HwFailed;
        uint32_t active=0;
        if(!rd(mq::hqd(q,kiq::kCpHqdActive),active) || (active&1u)){sel(0u);return Code::HwFailed;}
        // As hqd_load_v11: the block with ACTIVE still 0 (KFD's MQD leaves cp_hqd_active 0; ours,
        // from gfx_v11_0_compute_mqd_init, has 1, which made a reused HQD live while the rest of
        // its block still held the previous client's values), then the EOP fetcher, then ACTIVE.
        for(uint32_t dw=mq::kHqdFirst;dw<=mq::kHqdLast;++dw) {
            const uint32_t v=img.dw[gfx11_mqd::kCpMqdBaseAddrLo+(dw-mq::kHqdFirst)];
            if(dw!=mq::kHqdHole && !wr(mq::hqd(q,dw),dw==kiq::kCpHqdActive ? (v&~1u) : v)){sel(0u);return Code::HwFailed;}
        }
        const uint64_t poll=c.queue.va+mq::kWptrOff;
        const struct { uint32_t dw; uint32_t val; } tail[]={
            {kiq::kCpHqdPqDoorbellControl,(img.dw[gfx11_mqd::kCpHqdPqDoorbellControl]&~(kDbOffsetMask|kDbEn))|
                                          (doorbellDword(index(c))<<2)|kDbEn},
            {kiq::kCpHqdPqWptrLo,0u},{kiq::kCpHqdPqWptrHi,0u},
            {kiq::kCpHqdPqWptrPollAddr,static_cast<uint32_t>(poll)},
            {kiq::kCpHqdPqWptrPollAddrHi,static_cast<uint32_t>(poll>>32)}};
        for(const auto &x:tail)if(!wr(mq::hqd(q,x.dw),x.val)){sel(0u);return Code::HwFailed;}
        if(!wr(q.poll1,pollBit(index(c))) ||
           !wr(mq::hqd(q,mq::kEopRptr),img.dw[gfx11_mqd::kCpMqdBaseAddrLo+(mq::kEopRptr-mq::kHqdFirst)]|mq::kEopInitFetcher) ||
           !wr(mq::hqd(q,kiq::kCpHqdActive),img.dw[gfx11_mqd::kCpHqdActive]|1u) || !sel(0u))
            {sel(0u);return Code::HwFailed;}
        uint32_t pq=0;
        if(!rd(r_->pqStatus,pq) || (!(pq&kPqStatusDoorbellEnable) && !wr(r_->pqStatus,pq|kPqStatusDoorbellEnable)))
            return Code::HwFailed;
        rd(r_->pqStatus,db_.pq);
        return Code::Ok;
    }
    // hqd_destroy_v11 with DRAIN_PIPE.
    bool dequeue(Client &c) {
        const mq::Regs &q=r_->g.v.q;
        if(!sel(qsel(index(c))))return false;
        uint32_t active=0;
        if(!rd(mq::hqd(q,kiq::kCpHqdActive),active)){sel(0u);return false;}
        if(active&1u) {
            if(!wr(r_->dequeue,kDrainPipe)){sel(0u);return false;}
            for(uint32_t ms=0;;++ms) {
                if(!rd(mq::hqd(q,kiq::kCpHqdActive),active)){sel(0u);return false;}
                if(!(active&1u))break;
                if(ms==kDequeueTimeoutMs){sel(0u);return false;}
                h_.delayUs(h_.ctx,1000u);
            }
        }
        return sel(0u);
    }
public:
    // Record the queue state of a slot whose wait timed out (once per client).
    void noteStall(uint32_t slot) { Client *c=live(slot); if(c && !c->snap.taken)snapshot(*c); }
private:
    // Diagnostics only: GRBM select writes and reads, nothing else.
    void snapshot(Client &c) {
        QueueSnap s{};s.taken=true;
        if(sel(qsel(index(c)))) {
            for(uint32_t i=0;i<kSnapCount;++i)h_.read(h_.ctx,mq::hqd(r_->g.v.q,kSnapRegs[i]),&s.reg[i]);
            sel(0u);
        }
        h_.vramRead(h_.ctx,c.queue.vram+mq::kRptrOff,&s.rptrReport,1u);
        h_.vramRead(h_.ctx,c.queue.vram+mq::kWptrOff,&s.wptrMem,1u);
        h_.vramRead(h_.ctx,c.queue.vram+mq::kFenceOff,&s.fence,1u);
        c.snap=s;
    }
    uint64_t retiredWptr(const Client &c) const { return c.retired ? c.seqEnd[c.retired%64u] : 0u; }
    // The kernel's gfx ring: its own sequence (the last fence of every submission) for ring space.
    void gfxPoll() {
        uint32_t f=0;
        if(!h_.vramRead(h_.ctx,gfxMemVram_+gfx_ring::kKernelFenceOff,&f,1u))return;
        const uint64_t s=expand(gfxSeq_,f);
        if(s>gfxRetired_ && s<=gfxSeq_)gfxRetired_=s;
    }
    // The MC address of a client VA inside one of its live buffers.
    bool vaToMc(const Client &c,uint64_t va,uint64_t &mc) const {
        for(const Bo &b:c.bo)if(b.live && b.va && va>=b.va && va-b.va<b.bytes){mc=fbBase_+b.vram+(va-b.va);return true;}
        return false;
    }
    // Read the fence and the hub's fault status; a fault in this VMID ends the client.
    // A fence holds the low 32 bits of the last sequence number its ring retired.
    static uint64_t expand(uint64_t emitted,uint32_t f) {
        const uint64_t cand=(emitted&~0xffffffffull)|f;
        return cand>emitted ? cand-(1ull<<32) : cand;
    }
    void poll(Client &c) {
        if(c.state!=State::Open)return;
        uint32_t f=0,fg=0,st=0,addr=0;
        if(!h_.vramRead(h_.ctx,c.queue.vram+mq::kFenceOff,&f,1u) || !h_.vramRead(h_.ctx,c.queue.vram+kGfxFenceOff,&fg,1u) ||
           !rd(r_->g.v.faultStatus,st) || !rd(r_->g.v.faultAddr,addr)) {quarantine(c);return;}
        // Each ring retires in order and its fence carries the client's sequence numbers, so seq s
        // is done once the fence of the ring s went to has reached it; retire in order across both.
        const uint64_t cs=expand(c.emitted,f), gs=expand(c.emitted,fg);
        while(c.retired<c.emitted) {
            const uint64_t s=c.retired+1u, done=(c.gfxMask>>(s%64u))&1u ? gs : cs;
            if(done<s || done>c.emitted)break;
            c.retired=s;
        }
        if(st && st!=faultSeen_ && ((st&kFaultVmidMask)>>kFaultVmidShift)==c.vmid) {
            c.faultStatus=st;c.faultAddr=addr;c.state=State::Faulted;
        }
    }
    Code waitIdle(Client &c) {
        if(c.state==State::Quarantined)return Code::Quarantined;
        uint64_t r=0;
        if(c.emitted && wait(static_cast<uint32_t>(&c-c_),c.emitted,kDequeueTimeoutMs*1000u,r)!=Code::Ok)
            return c.state==State::Faulted ? Code::Faulted : Code::Timeout;
        return Code::Ok;
    }
    Client *live(uint32_t slot) { return slot<kClients && c_[slot].state!=State::Free ? &c_[slot] : nullptr; }
    Bo *bo(Client &c,uint32_t handle) { return handle && handle<=kMaxBos && c.bo[handle-1u].live ? &c.bo[handle-1u] : nullptr; }
    bool mapped(const Client &c,uint64_t va,uint64_t bytes,uint64_t need) const {
        for(const Bo &b:c.bo)
            if(b.live && b.va && va>=b.va && va-b.va<b.bytes && bytes<=b.bytes-(va-b.va))return (b.vaFlags&need)==need;
        return false;
    }
    static Code stateCode(const Client &c) {
        return c.state==State::Faulted ? Code::Faulted : c.state==State::Quarantined ? Code::Quarantined : Code::NotReady;
    }
    void quarantine(Client &c) { c.state=State::Quarantined; }
    void release(Bo &b) {
        if(b.cpuMapped)retiredBytes_+=b.bytes;      // stays allocated: a CPU mapping may remain
        else heap_->free(b.h);
        b=Bo{};
    }
    Code fail(Client &c,Code code) { quarantine(c);sel(0u);return code; }
    // Before the context exists nothing on the GPU references the client: free it all.
    void quarantineIfMapped(Client &c) {
        heap_->free(c.queue.h);heap_->free(c.mqd.h);
        c.space.destroy();resetClient(c);
    }
    void resetClient(Client &c) {
        c.state=State::Free;c.owner=nullptr;c.queue=Bo{};c.mqd=Bo{};
        for(Bo &b:c.bo)b=Bo{};
        for(uint64_t &e:c.seqEnd)e=0u;
        c.wptr=c.emitted=c.retired=0u;c.gfxMask=0u;c.vmid=c.faultStatus=c.faultAddr=0u;c.loaded=false;
    }
    const Regs *r_=nullptr; Hw h_{}; vh::Heap *heap_=nullptr; uint64_t fbBase_=0;
    Client c_[kClients]{};
    TableIo tio_{};
    uint32_t select_=0, writes_=0, flushes_=0, faultSeen_=0;
    uint64_t retiredBytes_=0;
    DoorbellSetup db_{};
    FlushMiss miss_{}; HubL2 l2_{}; bool tlbUnsafe_=false;
    GfxReport gfx_{}; vh::Handle gfxMem_{}; uint64_t gfxMemVram_=0;
    uint64_t gfxWptr_=0, gfxSeq_=0, gfxRetired_=0, gfxEnd_[64]{};
    uint32_t hqdStart_[kHqdDumpQueues][kHqdDumpCount]{}; bool hqdStartOk_[kHqdDumpQueues]{};
    bool started_=false;
};
} }
