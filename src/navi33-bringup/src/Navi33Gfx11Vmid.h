// SPDX-License-Identifier: MIT
// First translated GPU address space on GC 11.0.2 (G36, Phase B2). GFXHUB only
// (MMHUB, which the display uses, is not touched). Upstream, torvalds/linux
// master fetched 2026-10-19: gfxhub_v3_0.c
//  - init_tlb_regs(): GCMC_VM_MX_L1_TLB_CNTL L1 on, SYSTEM_ACCESS_MODE 3,
//    ADVANCED_DRIVER_MODEL 1, UNMAPPED_ACCESS 0, ECO_BITS 0, MTYPE UC.
//  - init_cache_regs(): GCVM_L2_CNTL..CNTL5 (translate_further is never set for
//    GMC11, so BANK_SELECT 9 and BIGK fragment 6), with the L1/L2 invalidation.
//  - disable_identity_aperture().
//  - setup_vmid_config() for context 1 only, with Linux's fault defaults and
//    noretry (GC >= 10.1: RETRY 0). Deviation: PAGE_TABLE_DEPTH 0, one flat
//    16-entry table, instead of Linux's 3-level tree; START 0, END page 15.
// Left out: context 0 / GART (VMID 0 stays physical inside the system aperture,
// proven by G34/G35), the system aperture and fault default pages (already the
// VBIOS values), contexts 2..15 and the invalidation engines.
// Proof: me 1 pipe 0 queue 1 in VMID 1 (as KFD's hqd_load_v11 loads a user
// queue), its EOP, ring, RPTR/WPTR and fence at virtual addresses mapped to
// VRAM pages in reverse order, Linux's ring test plus a fence. The fence must
// appear on the reversed physical page. No hardware I/O except callbacks.
#pragma once
#include "Navi33Gfx11MecQueue.h"
namespace navi33 { namespace vmid1 {
namespace mq=mec_queue;
namespace kiq=gfx11_kiq;
// gc_11_0_0_offset.h, all GC segment 0.
constexpr uint32_t kL1TlbCntl=0x167fu, kL2Cntl=0x15bcu, kL2Cntl2=0x15bdu, kL2Cntl3=0x15beu;
constexpr uint32_t kL2Cntl4=0x15d4u, kL2Cntl5=0x15dau, kIdentityFirst=0x15ceu;   // 0x15ce..0x15d3
constexpr uint32_t kCtx1Cntl=0x1689u, kCtx1BaseLo=0x16f5u, kCtx1BaseHi=0x16f6u;
constexpr uint32_t kCtx1StartLo=0x1715u, kCtx1StartHi=0x1716u, kCtx1EndLo=0x1735u, kCtx1EndHi=0x1736u;
constexpr uint32_t kFaultStatus=0x15c8u, kFaultAddrLo=0x15c9u;
// gc_11_0_0_default.h, which the census matched on this card (2026-10-19).
constexpr uint32_t kL1Default=0x00000501u, kL2Default=0x00080602u, kL2Cntl3Default=0x80120007u;
constexpr uint32_t kL2Cntl4Default=0x000000c1u, kL2Cntl5Default=0x00003fe0u, kCtxDefault=0x01fffe00u;
constexpr uint32_t field(uint32_t v,uint32_t mask,uint32_t value) {
    uint32_t shift=0;while(!((mask>>shift)&1u))++shift;
    return (v&~mask)|((value<<shift)&mask);
}
// gc_11_0_0_sh_mask.h field masks; soc21 MTYPE_UC = 3.
constexpr uint32_t l1Word(uint32_t cur) {
    return field(field(field(field(field(field(cur,0x1u,1u),0x18u,3u),0x40u,1u),0x20u,0u),0x780u,0u),
                 0x3800u,3u);
}
constexpr uint32_t l2Word(uint32_t cur) {
    return field(field(field(field(field(field(field(cur,0x1u,1u),0x2u,0u),0x800u,1u),0x100u,0u),
                 0x40000u,0u),0x180000u,1u),0x3e00000u,0u);
}
constexpr uint32_t l2Cntl2Word(uint32_t cur) { return field(field(cur,0x1u,1u),0x2u,1u); }
constexpr uint32_t kL2Cntl3Value=field(field(kL2Cntl3Default,0x3fu,9u),0xf8000u,6u);
constexpr uint32_t kL2Cntl4Value=field(field(kL2Cntl4Default,0x40u,0u),0x80u,0u);
constexpr uint32_t kL2Cntl5Value=field(kL2Cntl5Default,0x1fu,0u);
// ENABLE_CONTEXT 1, DEPTH 0, BLOCK_SIZE 0, RETRY 0, the seven *_DEFAULT fault bits 1.
constexpr uint32_t ctx1Word(uint32_t cur) {
    // RANGE, DUMMY_PAGE, PDE0, VALID, READ, WRITE, EXECUTE _PROTECTION_FAULT_ENABLE_DEFAULT
    return field(field(field(field(cur,0x1u,1u),0x6u,0u),0x78u,0u),0x80u,0u)|0x00555400u;
}
static_assert(l1Word(kL1Default)==0x00001859u && l2Word(kL2Default)==0x00080e01u,"hub words");
static_assert(kL2Cntl3Value==0x80130009u && kL2Cntl4Value==0x01u && kL2Cntl5Value==0x3fe0u,"cache words");
static_assert(ctx1Word(kCtxDefault)==0x01fffe01u,"context 1 word");
constexpr uint32_t kIdentity[6]={0xffffffffu,0xfu,0u,0u,0u,0u};
// amdgpu_vm.h PTE bits for a VRAM page: VALID, EXECUTABLE, READABLE, WRITEABLE,
// SYSTEM 0, MTYPE_NV10 NC (0).
constexpr uint64_t kPteVram=(1ull<<0)|(1ull<<4)|(1ull<<5)|(1ull<<6);
constexpr uint32_t kPages=16u, kPtPage=16u, kMqdPage=17u;  // window pages
constexpr uint32_t physPageOf(uint32_t vaPage) { return kPages-1u-vaPage; }   // reversed
constexpr uint32_t kFenceValue=0x0c0b1636u, kWindowBytes=1u<<20, kTimeoutMs=1000u;
constexpr unsigned kMaxWrites=96u;

struct Regs {
    mq::Regs q{};
    uint64_t l1=0,l2=0,l2c2=0,l2c3=0,l2c4=0,l2c5=0,ident[6]{},ctx=0,baseLo=0,baseHi=0;
    uint64_t startLo=0,startHi=0,endLo=0,endHi=0,faultStatus=0,faultAddr=0;
    bool valid=false;
};
inline Regs resolve(const IPDiscovery &d,uint64_t bytes) {
    Regs r{};r.q=mq::resolve(d,bytes);
    if(!r.q.valid)return r;
    struct One { uint64_t *dst; uint32_t dw; };
    const One ones[]={{&r.l1,kL1TlbCntl},{&r.l2,kL2Cntl},{&r.l2c2,kL2Cntl2},{&r.l2c3,kL2Cntl3},
        {&r.l2c4,kL2Cntl4},{&r.l2c5,kL2Cntl5},{&r.ctx,kCtx1Cntl},{&r.baseLo,kCtx1BaseLo},
        {&r.baseHi,kCtx1BaseHi},{&r.startLo,kCtx1StartLo},{&r.startHi,kCtx1StartHi},
        {&r.endLo,kCtx1EndLo},{&r.endHi,kCtx1EndHi},{&r.faultStatus,kFaultStatus},
        {&r.faultAddr,kFaultAddrLo},{&r.ident[0],kIdentityFirst},{&r.ident[1],kIdentityFirst+1u},
        {&r.ident[2],kIdentityFirst+2u},{&r.ident[3],kIdentityFirst+3u},{&r.ident[4],kIdentityFirst+4u},
        {&r.ident[5],kIdentityFirst+5u}};
    for(const One &o:ones) {
        const auto a=resolveDword(d,Engine::GFX11,0,o.dw,bytes);
        if(a.code!=AddressCode::Pass)return r;
        *o.dst=a.bar5ByteOffset;
    }
    r.valid=true;return r;
}
// The hub registers above (not the fault status/address, which are only read),
// plus what a queue load needs: GRBM select, SCRATCH_REG0, the poll trigger and
// the HQD block (mec_queue's list without the MEC program counters/control).
inline bool allowed(const Regs &r,uint64_t off) {
    if(!r.valid || off==r.faultStatus || off==r.faultAddr || off==r.q.pc || off==r.q.pcHi || off==r.q.cntl)
        return false;
    const uint64_t hub[]={r.l1,r.l2,r.l2c2,r.l2c3,r.l2c4,r.l2c5,r.ctx,r.baseLo,r.baseHi,r.startLo,
                          r.startHi,r.endLo,r.endHi};
    for(uint64_t o:hub)if(off==o)return true;
    for(uint64_t o:r.ident)if(off==o)return true;
    return mq::allowed(r.q,off);
}
// The window's MC address and its VRAM offset (BAR0 == VRAM proven by the caller).
struct Layout { uint64_t windowMc=0, windowVram=0; };
struct Hw {
    void *ctx=nullptr;
    bool (*read)(void *,uint64_t,uint32_t *)=nullptr;
    bool (*write)(void *,uint64_t,uint32_t)=nullptr;
    bool (*vramWrite)(void *,uint32_t off,const uint32_t *src,uint32_t dwords)=nullptr;
    bool (*vramRead)(void *,uint32_t off,uint32_t *dst,uint32_t dwords)=nullptr;
    void (*sleep)(void *,uint32_t)=nullptr;
};
inline bool read(const Hw &h,uint64_t off,uint32_t &v) {
    return h.read && h.read(h.ctx,off,&v) && v!=0xffffffffu;
}
inline n33_psp_load_result blank() {
    n33_psp_load_result o{};o.abi_version=N33_ABI_VERSION;o.size=sizeof(o);
    o.vm_marker=N33_VM_MARKER;return o;
}
// The 16 PTEs: VA page 0 stays invalid, page p maps to window page 15-p.
inline void pageTable(const Layout &l,uint64_t pte[kPages]) {
    pte[0]=0u;
    for(uint32_t p=1;p<kPages;++p)pte[p]=(l.windowVram+4096ull*physPageOf(p))|kPteVram;
}
// The queue at virtual addresses (mec_queue's layout from VA 0), MQD in VMID 0
// at its physical MC address, CP_HQD_VMID 1.
inline bool mqdImage(const Layout &l,gfx11_mqd::Image &img) {
    const gfx11_mqd::Result m=gfx11_mqd::buildKiqMqd(mq::queueFor(mq::Layout{0u}));
    if(m.code!=kiq::Code::PlanOnly)return false;
    img=m.image;
    const uint64_t mqdMc=l.windowMc+4096ull*kMqdPage;
    img.dw[gfx11_mqd::kCpMqdBaseAddrLo]=static_cast<uint32_t>(mqdMc&0xfffffffcu);
    img.dw[gfx11_mqd::kCpMqdBaseAddrHi]=static_cast<uint32_t>(mqdMc>>32);
    img.dw[gfx11_mqd::kCpHqdVmid]=1u;
    return true;
}
inline uint32_t sample(const Regs &r,const Hw &h,n33_psp_load_result &o) {
    uint32_t sos=0,rlc=0,grbm=0,cntl=0,l2=0;
    if(!read(h,r.q.sos,sos) || !read(h,r.q.rlc,rlc) || !read(h,r.q.grbm,grbm) || !read(h,r.q.cntl,cntl) ||
       !read(h,r.q.scratch,o.vm_scratch) || !read(h,r.l1,o.vm_l1_before) || !read(h,r.l2,l2) ||
       !read(h,r.ctx,o.vm_ctx_before) || !read(h,r.faultStatus,o.vm_fault_status))
        return N33_PSPL_READ_FAILED;
    o.c2pmsg81_after=sos;o.vm_l2_before=l2;
    if(!sos)return N33_PSPL_SOS_NOT_ALIVE;
    // The autostart (or G34) ran: MEC up, ring test done; the GFXHUB still has
    // its reset values, context 1 off, and no fault recorded.
    if(!(rlc&0x80000000u) || grbm || cntl!=0x3c000000u || o.vm_scratch!=mq::kScratchDone ||
       o.vm_l1_before!=kL1Default || l2!=kL2Default || o.vm_ctx_before!=kCtxDefault || o.vm_fault_status)
        return N33_PSPL_VM_BAD_PRECONDITION;
    return N33_PSPL_PLAN_OK;
}
struct Writer {
    const Regs &r; const Hw &h; n33_psp_load_result &o; uint32_t select=0u;
    bool put(uint64_t off,uint32_t v) {
        if(!allowed(r,off) || o.register_writes>=kMaxWrites || !h.write(h.ctx,off,v))return false;
        ++o.register_writes;o.flags|=N33_PSPL_FLAG_WROTE_REGISTERS;
        if(off==r.q.grbm)select=v;
        return true;
    }
    bool sel(uint32_t v) { return select==v || put(r.q.grbm,v); }
    n33_psp_load_result &stop(uint32_t state) {
        if(select)put(r.q.grbm,0u);
        o.state=state;return o;
    }
};
inline bool vram(const Hw &h,uint32_t off,const uint32_t *src,uint32_t n) {
    if(!h.vramWrite(h.ctx,off,src,n))return false;
    for(uint32_t i=0;i<n;++i) {
        uint32_t v=0;
        if(!h.vramRead(h.ctx,off+4u*i,&v,1u) || v!=src[i])return false;
    }
    return true;
}
inline n33_psp_load_result execute(const Regs &r,const Layout &l,const Hw &h,bool run,bool gate,
                                  bool &attempted) {
    auto o=blank();
    if(gate)o.flags|=N33_PSPL_FLAG_WRITE_GATE_OPEN;
    if(!r.valid){o.state=N33_PSPL_BAD_ROM;return o;}
    o.vm_window=static_cast<uint32_t>(l.windowMc>>20);
    if(!l.windowMc || (l.windowMc&(kWindowBytes-1u)) || (l.windowVram&(kWindowBytes-1u)) ||
       l.windowVram>=(1ull<<40)){o.state=N33_PSPL_NO_WINDOW;return o;}
    gfx11_mqd::Image img{};
    if(!mqdImage(l,img)){o.state=N33_PSPL_BAD_REQUEST;return o;}
    o.state=sample(r,h,o);
    if(o.state!=N33_PSPL_PLAN_OK || !run)return o;
    if(!gate){o.state=N33_PSPL_WRITE_DISABLED;return o;}
    if(attempted){o.state=N33_PSPL_ALREADY_ATTEMPTED;return o;}
    if(!h.write || !h.sleep || !h.vramWrite || !h.vramRead){o.state=N33_PSPL_BAD_REQUEST;return o;}
    attempted=true;
    Writer w{r,h,o};
    // 1. VRAM: the 18 window pages zeroed, page table, MQD, ring, WPTR (VA 0x3008
    //    lives on physical page 12), all read back.
    static uint32_t zero[(kMqdPage+1u)*1024u];
    uint64_t pte[kPages];pageTable(l,pte);
    uint32_t ptw[2u*kPages];
    for(uint32_t i=0;i<kPages;++i){ptw[2*i]=static_cast<uint32_t>(pte[i]);ptw[2*i+1]=static_cast<uint32_t>(pte[i]>>32);}
    uint32_t ring[mq::kRingDwords];mq::ringPackets(mq::Layout{0u},ring);
    ring[8]=kFenceValue;   // the RELEASE_MEM data dword
    const uint32_t wptr[2]={mq::kRingDwords,0u};
    auto at=[](uint32_t va){return 4096u*physPageOf(va/4096u)+va%4096u;};
    if(!h.vramWrite(h.ctx,0u,zero,(kMqdPage+1u)*1024u) || !vram(h,4096u*kPtPage,ptw,2u*kPages) ||
       !vram(h,4096u*kMqdPage,img.dw,gfx11_mqd::kDwords) || !vram(h,at(mq::kRingOff),ring,mq::kRingDwords) ||
       !vram(h,at(mq::kWptrOff),wptr,2u))
        {o.state=N33_PSPL_STAGE_VERIFY_FAILED;return o;}
    o.flags|=N33_PSPL_FLAG_WROTE_VRAM;
    // 2. gfxhub_v3_0: TLB, caches (with the invalidation), identity aperture off.
    uint32_t c2=0;
    if(!read(h,r.l2c2,c2))return w.stop(N33_PSPL_READ_FAILED);
    const struct { uint64_t off; uint32_t v; } hub[]={
        {r.l1,l1Word(o.vm_l1_before)},{r.l2,l2Word(o.vm_l2_before)},{r.l2c2,l2Cntl2Word(c2)},
        {r.l2c3,kL2Cntl3Value},{r.l2c4,kL2Cntl4Value},{r.l2c5,kL2Cntl5Value},
        {r.ident[0],kIdentity[0]},{r.ident[1],kIdentity[1]},{r.ident[2],kIdentity[2]},
        {r.ident[3],kIdentity[3]},{r.ident[4],kIdentity[4]},{r.ident[5],kIdentity[5]}};
    for(const auto &x:hub)if(!w.put(x.off,x.v))return w.stop(N33_PSPL_VM_WRITE_FAILED);
    ++o.steps_done;
    // 3. Context 1: flat table, pages 0..15, then enable.
    const uint64_t base=(l.windowVram+4096ull*kPtPage)|1ull;
    const struct { uint64_t off; uint32_t v; } ctx[]={
        {r.baseLo,static_cast<uint32_t>(base)},{r.baseHi,static_cast<uint32_t>(base>>32)},
        {r.startLo,0u},{r.startHi,0u},{r.endLo,kPages-1u},{r.endHi,0u},{r.ctx,ctx1Word(o.vm_ctx_before)}};
    for(const auto &x:ctx)if(!w.put(x.off,x.v))return w.stop(N33_PSPL_VM_WRITE_FAILED);
    ++o.steps_done;
    // 4. hqd_load_v11() for me 1 pipe 0 queue 1, VMID 1, after the ring-test preset.
    if(!w.put(r.q.scratch,mq::kScratchPreset) || !w.sel(mq::grbmSelect(mq::kMeMec,0u,1u)))
        return w.stop(N33_PSPL_VM_WRITE_FAILED);
    uint32_t active=0;
    if(!read(h,mq::hqd(r.q,kiq::kCpHqdActive),active))return w.stop(N33_PSPL_READ_FAILED);
    if(active&1u)return w.stop(N33_PSPL_VM_BAD_PRECONDITION);
    for(uint32_t dw=mq::kHqdFirst;dw<=mq::kHqdLast;++dw)
        if(dw!=mq::kHqdHole && !w.put(mq::hqd(r.q,dw),img.dw[gfx11_mqd::kCpMqdBaseAddrLo+(dw-mq::kHqdFirst)]))
            return w.stop(N33_PSPL_VM_WRITE_FAILED);
    const struct { uint32_t dw; uint32_t v; } tail[]={
        {kiq::kCpHqdPqDoorbellControl,img.dw[gfx11_mqd::kCpHqdPqDoorbellControl]|mq::kDoorbellEn},
        {kiq::kCpHqdPqWptrLo,0u},{kiq::kCpHqdPqWptrHi,0u},
        {kiq::kCpHqdPqWptrPollAddr,mq::kWptrOff},{kiq::kCpHqdPqWptrPollAddrHi,0u}};
    for(const auto &t:tail)if(!w.put(mq::hqd(r.q,t.dw),t.v))return w.stop(N33_PSPL_VM_WRITE_FAILED);
    // get_queue_mask(pipe 0, queue 1): bit 1 (eight queues per pipe).
    if(!w.put(r.q.poll1,1u<<1) ||
       !w.put(mq::hqd(r.q,mq::kEopRptr),img.dw[gfx11_mqd::kCpMqdBaseAddrLo+(mq::kEopRptr-mq::kHqdFirst)]|
              mq::kEopInitFetcher) ||
       !w.put(mq::hqd(r.q,kiq::kCpHqdActive),img.dw[gfx11_mqd::kCpHqdActive]|1u) || !w.sel(0u))
        return w.stop(N33_PSPL_VM_WRITE_FAILED);
    ++o.steps_done;
    // 5. Wait for the ring test and the fence on the reversed page; then the
    //    queue's RPTR and any fault the hub recorded.
    for(uint32_t ms=0;;++ms) {
        if(!read(h,r.q.scratch,o.vm_scratch) || !h.vramRead(h.ctx,at(mq::kFenceOff),&o.vm_fence,1u))
            return w.stop(N33_PSPL_READ_FAILED);
        if(o.vm_scratch==mq::kScratchDone && o.vm_fence==kFenceValue)break;
        if(ms==kTimeoutMs)break;
        h.sleep(h.ctx,1u);++o.elapsed_ms;
    }
    if(!w.sel(mq::grbmSelect(mq::kMeMec,0u,1u)) || !read(h,mq::hqd(r.q,kiq::kCpHqdPqRptr),o.vm_rptr) ||
       !read(h,mq::hqd(r.q,kiq::kCpHqdActive),o.vm_active) || !w.sel(0u) ||
       !read(h,r.faultStatus,o.vm_fault_status) || !read(h,r.faultAddr,o.vm_fault_addr))
        return w.stop(N33_PSPL_READ_FAILED);
    if(o.vm_fault_status){o.state=N33_PSPL_VM_FAULT;return o;}
    if(o.vm_scratch!=mq::kScratchDone){o.state=N33_PSPL_VM_NO_FETCH;return o;}
    ++o.steps_done;
    if(o.vm_fence!=kFenceValue){o.state=N33_PSPL_VM_NO_FENCE;return o;}
    ++o.steps_done;
    o.state=N33_PSPL_VM_QUEUE_OK;return o;
}
} }
