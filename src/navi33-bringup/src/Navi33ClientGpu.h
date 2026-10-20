// SPDX-License-Identifier: MIT
// Per-client GPU contexts on the RX 7600 (Phase B4): the core a user client calls
// for buffer create/free, GPU VA map/unmap, submit and wait, in the shape of
// amdgpu's DRM interface (and of the Navi48 N48N ABI that RADV's Darwin backend
// speaks). No hardware I/O except callbacks; the caller serializes.
//
// Each client owns VMID k and compute queue me 1 pipe 0 queue k (k = 2..7; VMID/queue
// 1 stay with the G36-G39 proofs), its own four-level address space (Navi33Gmc11Vm.h)
// and its buffers, all from the kernel's VRAM heap. The queue is loaded as
// hqd_load_v11 does (KFD without a scheduler), its ring and fence live in kernel
// buffers mapped at the top of the client's VA space (ring read-only for the GPU),
// and a submission is amdgpu's compute IB emission: INDIRECT_BUFFER per IB with the
// VMID in its control word, then a RELEASE_MEM fence with the sequence number
// (gfx_v11_0_ring_emit_ib_compute / _emit_fence). The WPTR is 64-bit and monotonic
// (support_64bit_ptrs); the CP takes it modulo the ring.
//
// Upstream, torvalds master 2026-10-20: gmc_v11_0_flush_gpu_tlb (invalidation engine 17,
// no semaphore on GFXHUB, wait for the ACK bit of the VMID), gfxhub_v3_0_get_invalidate_req,
// hqd_destroy_v11 (CP_HQD_DEQUEUE_REQUEST = DRAIN_PIPE, wait for CP_HQD_ACTIVE to clear),
// gfx_v11_0_constants_init (SH_MEM for VMID != 0), gfxhub_v3_0_setup_vmid_config.
//
// Memory is freed only once the GPU can no longer reach it: a buffer is unmapped
// and the TLB flushed first, and only after the client's last fence retired; a
// client is torn down only after its queue drained. Any step that cannot be
// confirmed (a TLB flush without ACK, a queue that will not dequeue, a hub fault in
// the client's VMID) quarantines the client: its memory is never freed again.
#pragma once
#include "Navi33Gfx11VmGradient.h"
#include "Navi33Gmc11Vm.h"
namespace navi33 { namespace client_gpu {
namespace vg=vm_gradient;
namespace vh=vram_heap;
namespace gv=gmc11_vm;
namespace mq=mec_queue;
namespace kiq=gfx11_kiq;
namespace gd=gfx11_dispatch;
namespace v1=vmid1;
constexpr uint32_t kFirstVmid=2u, kClients=6u;                 // VMIDs / queues 2..7
constexpr uint32_t kMaxBos=512u, kMaxIbs=16u;
// The client's queue lives below the first user page (Mesa's amdgpu VA manager starts at
// 0x10000 and runs to the GMC hole), page 0 left unmapped so a null GPU pointer faults.
constexpr uint64_t kUserVaMin=0x10000u, kUserVaMax=gv::kVaLimit;   // [min, max)
constexpr uint64_t kKernelVa=0u;
static_assert(kKernelVa+0x5000u<=kUserVaMin,"kernel VA range");
constexpr uint64_t kMaxBoBytes=64ull<<20;
constexpr uint32_t kTagClient=0x434c4e54u;
// gc_11_0_0_offset.h (GC segment 0): context k registers, invalidation engine 17,
// CP_HQD_DEQUEUE_REQUEST (in the HQD block, banked by GRBM select).
constexpr uint32_t ctxCntl(uint32_t k) { return 0x1688u+k; }
constexpr uint32_t ctxBaseLo(uint32_t k) { return 0x16f3u+2u*k; }
constexpr uint32_t ctxStartLo(uint32_t k) { return 0x1713u+2u*k; }
constexpr uint32_t ctxEndLo(uint32_t k) { return 0x1733u+2u*k; }
static_assert(ctxCntl(1)==0x1689u && ctxBaseLo(2)==0x16f7u && ctxStartLo(2)==0x1717u && ctxEndLo(7)+1u==0x1742u,
              "context register strides (gfxhub_v3_0 ctx_distance 1, ctx_addr_distance 2)");
constexpr uint32_t kInvReq17=0x16bcu, kInvAck17=0x16ceu, kDequeueRequest=0x1fc1u;
constexpr uint32_t kDrainPipe=1u;
constexpr uint32_t kGbAddrConfig=0x13deu;                       // regGB_ADDR_CONFIG, read only                               // enum hqd_dequeue_request_type
// gfxhub_v3_0_get_invalidate_req(vmid, 0): PER_VMID bit, FLUSH_TYPE 0, L2 PTES/PDE0/1/2, L1 PTES.
constexpr uint32_t invalidateReq(uint32_t vmid) { return (1u<<vmid)|(1u<<19)|(1u<<20)|(1u<<21)|(1u<<22)|(1u<<23); }
static_assert(invalidateReq(2)==0x00f80004u,"GCVM_INVALIDATE_ENG0_REQ word");
constexpr uint32_t kFaultVmidShift=20u, kFaultVmidMask=0xf00000u;
// The queue buffer at kKernelVa, mec_queue's layout: page 0 unmapped, EOP 0x1000, ring
// 0x2000 (GPU read-only), RPTR/WPTR 0x3000/0x3008, fence 0x4000.
constexpr uint32_t kQueueBytes=0x5000u, kRingDwords=mq::kRingBytes/4u;
constexpr uint32_t kSubmitMaxDwords=4u*kMaxIbs+8u+8u+8u;       // IBs, user fence, fence, padding
constexpr uint32_t kTlbTimeoutUs=100000u, kDequeueTimeoutMs=1000u;

enum class Code : uint8_t {
    Ok=0, BadArgs, NotReady, NoSlot, NoMemory, Busy, Timeout, Faulted, Quarantined, NotFound, HwFailed, Hub
};
struct Regs {
    vg::Regs g{};          // G38's: hub, context 1, SH_MEM, GRBM, SCRATCH, poll trigger, HQD block
    uint64_t ctx[kClients][7]{};    // CNTL, BASE lo/hi, START lo/hi, END lo/hi
    uint64_t invReq=0, invAck=0, dequeue=0;
    uint64_t gbAddrConfig=0;        // read only (QueryInfo)
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
    if(!one(kInvReq17,r.invReq) || !one(kInvAck17,r.invAck) || !one(kDequeueRequest,r.dequeue) ||
       !one(kGbAddrConfig,r.gbAddrConfig))return r;
    r.valid=true;return r;
}
// Writable registers: the GFXHUB words, contexts 2..7, invalidation engine 17's
// request, SH_MEM_CONFIG/BASES, GRBM select, the poll trigger and the HQD block
// (CP_HQD_DEQUEUE_REQUEST lies inside it). Never the MEC program counters/control,
// SCRATCH_REG0, context 0/1, the fault registers or anything outside GC.
inline bool allowed(const Regs &r,uint64_t off) {
    if(!r.valid || off==r.g.v.q.scratch || off==r.g.v.ctx || off==r.g.v.baseLo || off==r.g.v.baseHi ||
       off==r.g.v.startLo || off==r.g.v.startHi || off==r.g.v.endLo || off==r.g.v.endHi)return false;
    if(off==r.invReq)return true;
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
};
struct Bo { vh::Handle h{}; uint64_t vram=0, bytes=0, va=0; uint64_t vaFlags=0; bool live=false, cpuMapped=false; };
struct Mapping { uint64_t va=0, bytes=0; };
enum class State : uint8_t { Free=0, Open, Faulted, Quarantined };
struct Client {
    State state=State::Free;
    const void *owner=nullptr;
    gv::Space space;
    Bo bo[kMaxBos]{};                       // handle = index + 1
    Bo queue{}, mqd{};
    uint64_t wptr=0;                        // dwords written to the ring, monotonic
    uint64_t emitted=0, retired=0;          // sequence numbers (the fence holds the low 32 bits)
    uint64_t seqEnd[64]{};                  // ring position after seq (modulo 64)
    uint32_t vmid=0, faultStatus=0, faultAddr=0;
};
// The last TLB flush that got no ACK: the VMID, the request written, what REQ and ACK read back.
struct FlushMiss { uint32_t vmid=0, req=0, reqBack=0, ack=0, misses=0; };
struct Stats { uint32_t clients=0, bos=0; uint64_t emitted=0, retired=0, retiredBytes=0; uint32_t writes=0, tlbFlushes=0;
               FlushMiss miss{}; };

class Device {
public:
    // Called once the provider owns the card and the MEC is running (autostart).
    // Sets up the GFXHUB as gfxhub_v3_0 does unless G36-G39 already did.
    Code start(const Regs &r,const Hw &h,vh::Heap &heap,uint64_t fbBaseMc) {
        if(started_)return Code::Ok;
        if(!r.valid || !h.read || !h.write || !h.vramWrite || !h.vramRead || !h.delayUs || !heap.total() || !fbBaseMc)
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
                {v.l1,v1::l1Word(l1)},{v.l2,v1::l2Word(l2)},{v.l2c2,v1::l2Cntl2Word(c2)},
                {v.l2c3,v1::kL2Cntl3Value},{v.l2c4,v1::kL2Cntl4Value},{v.l2c5,v1::kL2Cntl5Value},
                {v.ident[0],v1::kIdentity[0]},{v.ident[1],v1::kIdentity[1]},{v.ident[2],v1::kIdentity[2]},
                {v.ident[3],v1::kIdentity[3]},{v.ident[4],v1::kIdentity[4]},{v.ident[5],v1::kIdentity[5]}};
            for(const auto &x:hub)if(!wr(x.off,x.val))return Code::HwFailed;
        } else if(l1!=v1::l1Word(v1::kL1Default) || l2!=v1::l2Word(v1::kL2Default)) {
            return Code::Hub;                // a hub we do not recognise: touch nothing
        }
        if(fault)faultSeen_=fault;
        started_=true;return Code::Ok;
    }
    bool started() const { return started_; }
    // A new client: VMID k, address space, queue buffers mapped, context k, SH_MEM, queue k loaded.
    Code open(const void *owner,uint32_t &slot) {
        if(!started_)return Code::NotReady;
        if(!owner)return Code::BadArgs;
        slot=kClients;
        for(uint32_t i=0;i<kClients;++i)if(c_[i].state==State::Free){slot=i;break;}
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
        c.queue={q.h,q.offset,kQueueBytes,kKernelVa,0,true};c.mqd={m.h,m.offset,4096u,0,0,true};
        tio_.dev=this;
        const gv::Io io{&tio_,zeroPage,putEntry};
        if(!zero(c.queue) || !zero(c.mqd) || c.space.init(*heap_,io)!=gv::Code::Ok ||
           c.space.map(kKernelVa+mq::kEopOff,q.offset+mq::kEopOff,0x1000u,
                       gv::kPteValid|gv::kPteRead|gv::kPteWrite)!=gv::Code::Ok ||
           c.space.map(kKernelVa+mq::kRingOff,q.offset+mq::kRingOff,0x1000u,gv::kPteValid|gv::kPteRead)!=gv::Code::Ok ||
           c.space.map(kKernelVa+mq::kRptrOff,q.offset+mq::kRptrOff,0x2000u,
                       gv::kPteValid|gv::kPteRead|gv::kPteWrite)!=gv::Code::Ok) {
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
        // No flush: a VMID is only reused after close() flushed it, and new valid
        // PTEs need none (amdgpu_vm_update_range flushes only on GC < 9 or invalidation).
        return loadQueue(c)==Code::Ok ? Code::Ok : fail(c,Code::HwFailed);
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
        if(c->space.unmap(b->va,b->bytes)!=gv::Code::Ok || !flushTlb(c->vmid)){quarantine(*c);return Code::Quarantined;}
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
               !mapped(*c,ibs[i].va,4ull*ibs[i].dwords,gv::kPteRead))return Code::BadArgs;
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
        const uint64_t s=c->emitted+1u, fence=kKernelVa+mq::kFenceOff;
        if(userFence) {     // RELEASE_MEM DATA_SEL 2: the 64-bit sequence number
            const uint32_t u[8]={gd::packet3(gd::kReleaseMem,6u),gd::kReleaseMemControl,2u<<29,
                                 static_cast<uint32_t>(userFence),static_cast<uint32_t>(userFence>>32),
                                 static_cast<uint32_t>(s),static_cast<uint32_t>(s>>32),0u};
            for(uint32_t v:u)p[k++]=v;
        }
        const uint32_t rel[8]={gd::packet3(gd::kReleaseMem,6u),gd::kReleaseMemControl,gd::kReleaseMemDataSel32,
                               static_cast<uint32_t>(fence),static_cast<uint32_t>(fence>>32),static_cast<uint32_t>(s),0u,0u};
        for(uint32_t v:rel)p[k++]=v;
        while(k%8u)p[k++]=mq::kNop;
        for(uint32_t i=0;i<k;++i) {           // the ring wraps per dword
            const uint32_t at=static_cast<uint32_t>((c->wptr+i)%kRingDwords);
            if(!vramPut(c->queue.vram+mq::kRingOff+4u*at,&p[i],1u)){quarantine(*c);return Code::Quarantined;}
        }
        const uint64_t end=c->wptr+k;
        const uint32_t wp[2]={static_cast<uint32_t>(end),static_cast<uint32_t>(end>>32)};
        if(!vramPut(c->queue.vram+mq::kWptrOff,wp,2u)){quarantine(*c);return Code::Quarantined;}
        c->wptr=end;c->emitted=s;c->seqEnd[s%64u]=end;
        if(!sel(mq::grbmSelect(mq::kMeMec,0u,c->vmid)) || !wr(r_->g.v.q.poll1,1u<<c->vmid) || !sel(0u)) {
            quarantine(*c);return Code::Quarantined;
        }
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
            if(us>=timeoutUs)return Code::Timeout;
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
        if(!dequeue(*c)){quarantine(*c);c->owner=nullptr;return Code::Quarantined;}
        uint32_t cntl=0;
        if(!rd(r_->ctx[slot][0],cntl) || !wr(r_->ctx[slot][0],cntl&~1u) || !flushTlb(c->vmid))
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
        s.writes=writes_;s.tlbFlushes=flushes_;s.retiredBytes=retiredBytes_;s.miss=miss_;return s;
    }
private:
    static constexpr uint32_t kIndirectBuffer=0x3fu, kIbValid=1u<<23;   // nvd.h
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
    // gmc_v11_0_flush_gpu_tlb, MMIO path: engine 17, request, wait for the VMID's ACK bit.
    bool flushTlb(uint32_t vmid) {
        if(!wr(r_->invReq,invalidateReq(vmid)))return false;
        uint32_t ack=0;
        for(uint32_t us=0;us<kTlbTimeoutUs;++us) {
            if(!h_.read(h_.ctx,r_->invAck,&ack))break;
            if(ack!=0xffffffffu && (ack&(1u<<vmid))){++flushes_;return true;}
            h_.delayUs(h_.ctx,1u);
        }
        uint32_t back=0;
        h_.read(h_.ctx,r_->invReq,&back);
        miss_={vmid,invalidateReq(vmid),back,ack,miss_.misses+1u};
        return false;
    }
    // hqd_load_v11 for queue k in VMID k (as G38, with the poll bit of queue k).
    Code loadQueue(Client &c) {
        const gfx11_mqd::Result m=gfx11_mqd::buildKiqMqd(mq::queueFor(mq::Layout{kKernelVa}));
        if(m.code!=kiq::Code::PlanOnly)return Code::HwFailed;
        gfx11_mqd::Image img=m.image;
        const uint64_t mqdMc=fbBase_+c.mqd.vram;
        img.dw[gfx11_mqd::kCpMqdBaseAddrLo]=static_cast<uint32_t>(mqdMc&0xfffffffcu);
        img.dw[gfx11_mqd::kCpMqdBaseAddrHi]=static_cast<uint32_t>(mqdMc>>32);
        img.dw[gfx11_mqd::kCpHqdVmid]=c.vmid;
        if(!vramPut(c.mqd.vram,img.dw,gfx11_mqd::kDwords))return Code::HwFailed;
        const mq::Regs &q=r_->g.v.q;
        if(!sel(mq::grbmSelect(mq::kMeMec,0u,c.vmid)))return Code::HwFailed;
        uint32_t active=0;
        if(!rd(mq::hqd(q,kiq::kCpHqdActive),active) || (active&1u)){sel(0u);return Code::HwFailed;}
        for(uint32_t dw=mq::kHqdFirst;dw<=mq::kHqdLast;++dw)
            if(dw!=mq::kHqdHole && !wr(mq::hqd(q,dw),img.dw[gfx11_mqd::kCpMqdBaseAddrLo+(dw-mq::kHqdFirst)]))
                {sel(0u);return Code::HwFailed;}
        const uint64_t poll=kKernelVa+mq::kWptrOff;
        const struct { uint32_t dw; uint32_t val; } tail[]={
            {kiq::kCpHqdPqDoorbellControl,img.dw[gfx11_mqd::kCpHqdPqDoorbellControl]|mq::kDoorbellEn},
            {kiq::kCpHqdPqWptrLo,0u},{kiq::kCpHqdPqWptrHi,0u},
            {kiq::kCpHqdPqWptrPollAddr,static_cast<uint32_t>(poll)},
            {kiq::kCpHqdPqWptrPollAddrHi,static_cast<uint32_t>(poll>>32)}};
        for(const auto &x:tail)if(!wr(mq::hqd(q,x.dw),x.val)){sel(0u);return Code::HwFailed;}
        if(!wr(q.poll1,1u<<c.vmid) ||
           !wr(mq::hqd(q,mq::kEopRptr),img.dw[gfx11_mqd::kCpMqdBaseAddrLo+(mq::kEopRptr-mq::kHqdFirst)]|mq::kEopInitFetcher) ||
           !wr(mq::hqd(q,kiq::kCpHqdActive),img.dw[gfx11_mqd::kCpHqdActive]|1u) || !sel(0u))
            {sel(0u);return Code::HwFailed;}
        return Code::Ok;
    }
    // hqd_destroy_v11 with DRAIN_PIPE.
    bool dequeue(Client &c) {
        const mq::Regs &q=r_->g.v.q;
        if(!sel(mq::grbmSelect(mq::kMeMec,0u,c.vmid)))return false;
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
    uint64_t retiredWptr(const Client &c) const { return c.retired ? c.seqEnd[c.retired%64u] : 0u; }
    // Read the fence and the hub's fault status; a fault in this VMID ends the client.
    void poll(Client &c) {
        if(c.state!=State::Open)return;
        uint32_t f=0,st=0,addr=0;
        if(!h_.vramRead(h_.ctx,c.queue.vram+mq::kFenceOff,&f,1u) || !rd(r_->g.v.faultStatus,st) ||
           !rd(r_->g.v.faultAddr,addr)) {quarantine(c);return;}
        // The fence holds the low 32 bits of the last retired sequence number.
        const uint64_t cand=(c.emitted&~0xffffffffull)|f;
        const uint64_t seq=cand>c.emitted ? cand-(1ull<<32) : cand;
        if(seq>c.retired && seq<=c.emitted)c.retired=seq;
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
        c.wptr=c.emitted=c.retired=0u;c.vmid=c.faultStatus=c.faultAddr=0u;
    }
    const Regs *r_=nullptr; Hw h_{}; vh::Heap *heap_=nullptr; uint64_t fbBase_=0;
    Client c_[kClients]{};
    TableIo tio_{};
    uint32_t select_=0, writes_=0, flushes_=0, faultSeen_=0;
    uint64_t retiredBytes_=0;
    FlushMiss miss_{};
    bool started_=false;
};
} }
