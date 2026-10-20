// SPDX-License-Identifier: MIT
// G38 (Phase B2): the gradient computed from a VMID 1 queue whose whole address
// space goes through a Linux-shaped four-level page table. Builds on G36 (hub
// words, queue load) and G35 (pinned kernel, PM4 stream). Upstream, torvalds/linux
// master fetched 2026-10-20:
//  - gmc_v11_0.c: amdgpu_vm_adjust_size(adev, 256 * 1024, 9, 3, 48), so
//    amdgpu_vm.c gives max_pfn 1 << 36, num_level 3 (root PDB2), block_size 9.
//  - gfxhub_v3_0_setup_vmid_config(): PAGE_TABLE_DEPTH num_level,
//    PAGE_TABLE_BLOCK_SIZE block_size - 9, START 0, END max_pfn - 1.
//  - gmc_v11_0_get_vm_pde(): a VRAM directory entry is the VRAM offset of the
//    next level | AMDGPU_PTE_VALID; translate_further is never set for GMC11, so
//    no PDE_BFS / PTE_TF bits. vram_base_offset is the hub FB_OFFSET, 0 here.
//  - gfx_v11_0_constants_init(): for VMID i != 0, SH_MEM_CONFIG
//    DEFAULT_SH_MEM_CONFIG and SH_MEM_BASES from the private/shared apertures
//    (0x1000.../0x2000... >> 48), under soc21_grbm_select(0, 0, 0, i).
// Every queue buffer, the shader, its arguments, the test image and the screen
// are reached only through VMID 1 virtual addresses: the work pages map to window
// pages in reverse order, and the screen is the console framebuffer mapped at a
// second VA range. The MQD is fetched in VMID 0, as KFD does. No hardware I/O
// except callbacks.
#pragma once
#include "Navi33Gfx11Gradient.h"
#include "Navi33Gfx11Vmid.h"
namespace navi33 { namespace vm_gradient {
namespace mq=mec_queue;
namespace kiq=gfx11_kiq;
namespace gd=gfx11_dispatch;
namespace gr=gradient;
namespace v1=vmid1;
// gc_11_0_0_offset.h, GC segment 1; GRBM_GFX_CNTL.VMID [7:4] (sh_mask).
constexpr uint32_t kShMemBases=0x09e3u;
constexpr uint32_t kGrbmVmid1=1u<<4;
// gmc_v11_0_early_init() apertures >> 48 into PRIVATE_BASE [15:0], SHARED_BASE [31:16].
constexpr uint32_t kShMemBasesValue=(0x2000u<<16)|0x1000u;
// Four levels of 9 bits over 4 KiB pages: 48-bit VA, END = max_pfn - 1.
constexpr uint32_t kDepth=3u;
constexpr uint64_t kMaxPfn=1ull<<36;
constexpr uint32_t kEndLo=static_cast<uint32_t>(kMaxPfn-1u), kEndHi=static_cast<uint32_t>((kMaxPfn-1u)>>32);
// Context 1 as G36's word but PAGE_TABLE_DEPTH 3 (bits 2:1), BLOCK_SIZE 0 (6:3).
constexpr uint32_t ctx1Word(uint32_t cur) { return v1::field(v1::ctx1Word(cur),0x6u,kDepth); }
static_assert(ctx1Word(v1::kCtxDefault)==0x01fffe07u,"context 1 word, depth 3");
static_assert(kEndLo==0xffffffffu && kEndHi==0xfu,"END = max_pfn - 1");
// The VA layout: every directory index non-zero, below the GMC hole (1 << 47).
constexpr uint32_t kIdx2=0x0abu, kIdx1=0x155u, kIdx0=0x0f0u;
constexpr uint64_t kVaBase=(uint64_t(kIdx2)<<39)|(uint64_t(kIdx1)<<30)|(uint64_t(kIdx0)<<21);
constexpr uint64_t kScreenVa=kVaBase+(2ull<<20);       // PDB0 entries kIdx0+1.., one PTB per 2 MiB
static_assert(kVaBase==0x55d55e000000ull && kVaBase<(1ull<<47),"VA base");
constexpr uint32_t level(uint64_t va,uint32_t l) { return static_cast<uint32_t>(va>>(12u+9u*l))&511u; }
// Window pages: PDB2, PDB1, PDB0, the work PTB, up to eight screen PTBs, the
// MQD; work VA page v (1..63) lives on window page 255 - v.
constexpr uint32_t kWindowBytes=1u<<20, kPdb2=0u, kPdb1=1u, kPdb0=2u, kPtbWork=3u, kPtbScreen=4u;
constexpr uint32_t kMaxScreenPtbs=8u, kMqdPage=12u, kWorkPages=64u;
constexpr uint32_t physPageOf(uint32_t vaPage) { return 255u-vaPage; }
constexpr uint32_t kMaxScreenBytes=kMaxScreenPtbs*512u*4096u;
// Work offsets from kVaBase: G34's queue, G35's gradient layout.
static_assert(gr::kImageOff+gr::kImageBytes<=kWorkPages*4096u && mq::kEopOff>=4096u,"work range");
constexpr uint32_t kFenceValue=0x0c0b1638u, kFenceValueA=0x0c0b1639u, kFenceValueB=0x0c0b163au;
constexpr uint32_t kFrameA=7u, kFrameB=128u;            // B: blue shifted half a turn from G35's screen
constexpr uint32_t kTimeoutMs=1000u, kFallbackMs=200u;
constexpr unsigned kMaxWrites=128u;
constexpr uint64_t kPteVram=v1::kPteVram, kPdeValid=1u;

struct Regs { v1::Regs v{}; uint64_t shMem=0, shBases=0; bool valid=false; };
inline Regs resolve(const IPDiscovery &d,uint64_t bytes) {
    Regs r{};r.v=v1::resolve(d,bytes);
    const auto a=resolveDword(d,Engine::GFX11,1,gr::kShMemConfig,bytes);
    const auto b=resolveDword(d,Engine::GFX11,1,kShMemBases,bytes);
    if(!r.v.valid || a.code!=AddressCode::Pass || b.code!=AddressCode::Pass)return r;
    r.shMem=a.bar5ByteOffset;r.shBases=b.bar5ByteOffset;r.valid=true;return r;
}
// G36's allowlist (GFXHUB TLB/L2/identity/context 1, GRBM select, SCRATCH_REG0,
// the poll trigger, the HQD block) plus SH_MEM_CONFIG/SH_MEM_BASES.
inline bool allowed(const Regs &r,uint64_t off) {
    return r.valid && (off==r.shMem || off==r.shBases || v1::allowed(r.v,off));
}
struct Layout { uint64_t windowMc=0, windowVram=0; };
// The console framebuffer: VRAM offset, size, geometry (width 0: no screen pass).
struct Screen { uint64_t vram=0; uint32_t bytes=0, width=0, height=0, pitch=0; };
struct Hw {
    void *ctx=nullptr;
    bool (*read)(void *,uint64_t,uint32_t *)=nullptr;
    bool (*write)(void *,uint64_t,uint32_t)=nullptr;
    bool (*vramWrite)(void *,uint32_t off,const uint32_t *src,uint32_t dwords)=nullptr;
    bool (*vramRead)(void *,uint32_t off,uint32_t *dst,uint32_t dwords)=nullptr;
    bool (*screenRead)(void *,uint32_t byteOff,uint32_t *dst)=nullptr;
    void (*sleep)(void *,uint32_t)=nullptr;
};
inline bool read(const Hw &h,uint64_t off,uint32_t &v) {
    return h.read && h.read(h.ctx,off,&v) && v!=0xffffffffu;
}
inline n33_psp_load_result blank() {
    n33_psp_load_result o{};o.abi_version=N33_ABI_VERSION;o.size=sizeof(o);
    o.vm_marker=N33_VMG_MARKER;o.grad_marker=N33_VMG_MARKER;return o;
}
inline bool screenOk(const Screen &s) {
    return s.width && s.height && s.pitch>=s.width && !(s.width%8u) && !(s.height%8u) &&
        !(s.vram&0xfffu) && s.bytes && !(s.bytes&0xfffu) && s.bytes<=kMaxScreenBytes &&
        uint64_t(s.pitch)*s.height*4u<=s.bytes;
}
// The table pages as 64-bit entries, indexed [page][entry]; zero = invalid.
struct Tables { uint64_t e[kMqdPage][512]; };
inline void tables(const Layout &l,const Screen &s,Tables &t) {
    for(auto &p:t.e)for(auto &x:p)x=0u;
    auto pa=[&](uint32_t page){return l.windowVram+4096ull*page;};
    t.e[kPdb2][kIdx2]=pa(kPdb1)|kPdeValid;
    t.e[kPdb1][kIdx1]=pa(kPdb0)|kPdeValid;
    t.e[kPdb0][kIdx0]=pa(kPtbWork)|kPdeValid;
    for(uint32_t v=1;v<kWorkPages;++v)t.e[kPtbWork][v]=pa(physPageOf(v))|kPteVram;
    if(!screenOk(s))return;
    const uint32_t pages=s.bytes/4096u;
    for(uint32_t i=0;i<pages;++i) {
        const uint32_t ptb=kPtbScreen+i/512u;
        t.e[kPdb0][kIdx0+1u+i/512u]=pa(ptb)|kPdeValid;
        t.e[ptb][i%512u]=(s.vram+4096ull*i)|kPteVram;
    }
}
inline bool vram(const Hw &h,uint32_t off,const uint32_t *src,uint32_t n) {
    if(!h.vramWrite(h.ctx,off,src,n))return false;
    for(uint32_t i=0;i<n;++i) {
        uint32_t v=0;
        if(!h.vramRead(h.ctx,off+4u*i,&v,1u) || v!=src[i])return false;
    }
    return true;
}
// Work byte offset (from kVaBase) -> window byte offset.
constexpr uint32_t at(uint32_t off) { return 4096u*physPageOf(off/4096u)+off%4096u; }
// CPU access to a work range page by page (consecutive VA pages are not
// consecutive in VRAM).
inline bool workWrite(const Hw &h,uint32_t off,const uint32_t *src,uint32_t n,bool verify=true) {
    for(uint32_t i=0;i<n;) {
        const uint32_t o=off+4u*i, k=(4096u-o%4096u)/4u<n-i ? (4096u-o%4096u)/4u : n-i;
        if(o/4096u==0u || o/4096u>=kWorkPages || !h.vramWrite(h.ctx,at(o),src+i,k))return false;
        for(uint32_t j=0;verify && j<k;++j) {
            uint32_t x=0;
            if(!h.vramRead(h.ctx,at(o)+4u*j,&x,1u) || x!=src[i+j])return false;
        }
        i+=k;
    }
    return true;
}
inline bool workRead(const Hw &h,uint32_t off,uint32_t *dst,uint32_t n) {
    for(uint32_t i=0;i<n;) {
        const uint32_t o=off+4u*i, k=(4096u-o%4096u)/4u<n-i ? (4096u-o%4096u)/4u : n-i;
        if(o/4096u==0u || o/4096u>=kWorkPages || !h.vramRead(h.ctx,at(o),dst+i,k))return false;
        i+=k;
    }
    return true;
}
inline bool mqdImage(const Layout &l,gfx11_mqd::Image &img) {
    const gfx11_mqd::Result m=gfx11_mqd::buildKiqMqd(mq::queueFor(mq::Layout{kVaBase}));
    if(m.code!=kiq::Code::PlanOnly)return false;
    img=m.image;
    const uint64_t mqdMc=l.windowMc+4096ull*kMqdPage;
    img.dw[gfx11_mqd::kCpMqdBaseAddrLo]=static_cast<uint32_t>(mqdMc&0xfffffffcu);
    img.dw[gfx11_mqd::kCpMqdBaseAddrHi]=static_cast<uint32_t>(mqdMc>>32);
    img.dw[gfx11_mqd::kCpHqdVmid]=1u;
    return true;
}
// Same preconditions as G36: autostart state, GFXHUB untouched, queue 1 idle.
inline uint32_t sample(const Regs &r,const Hw &h,n33_psp_load_result &o) {
    uint32_t sos=0,rlc=0,grbm=0,cntl=0;
    const v1::Regs &v=r.v;
    if(!read(h,v.q.sos,sos) || !read(h,v.q.rlc,rlc) || !read(h,v.q.grbm,grbm) || !read(h,v.q.cntl,cntl) ||
       !read(h,v.q.scratch,o.vm_scratch) || !read(h,v.l1,o.vm_l1_before) || !read(h,v.l2,o.vm_l2_before) ||
       !read(h,v.ctx,o.vm_ctx_before) || !read(h,v.faultStatus,o.vm_fault_status) ||
       !read(h,r.shMem,o.grad_shmem_before))
        return N33_PSPL_READ_FAILED;
    o.c2pmsg81_after=sos;
    if(!sos)return N33_PSPL_SOS_NOT_ALIVE;
    if(!(rlc&0x80000000u) || grbm || cntl!=0x3c000000u || o.vm_scratch!=mq::kScratchDone ||
       o.vm_l1_before!=v1::kL1Default || o.vm_l2_before!=v1::kL2Default || o.vm_ctx_before!=v1::kCtxDefault ||
       o.vm_fault_status)
        return N33_PSPL_VMG_BAD_PRECONDITION;
    return N33_PSPL_PLAN_OK;
}
struct Writer {
    const Regs &r; const Hw &h; n33_psp_load_result &o; uint32_t select=0u;
    bool put(uint64_t off,uint32_t v) {
        if(!allowed(r,off) || o.register_writes>=kMaxWrites || !h.write(h.ctx,off,v))return false;
        ++o.register_writes;o.flags|=N33_PSPL_FLAG_WROTE_REGISTERS;
        if(off==r.v.q.grbm)select=v;
        return true;
    }
    bool sel(uint32_t v) { return select==v || put(r.v.q.grbm,v); }
    n33_psp_load_result &stop(uint32_t state) {
        if(select)put(r.v.q.grbm,0u);
        o.state=state;return o;
    }
};
constexpr uint32_t kQueueSel=mq::grbmSelect(mq::kMeMec,0u,1u);
// Any fault the hub latched since the run began ends it.
inline bool faulted(const Regs &r,const Hw &h,n33_psp_load_result &o) {
    return !read(h,r.v.faultStatus,o.vm_fault_status) || !read(h,r.v.faultAddr,o.vm_fault_addr) ||
        o.vm_fault_status!=0u;
}
// Append one packet run at ring dword `at`, kick the queue, wait for its fence.
inline uint32_t submit(Writer &w,const gr::Packets &p,uint32_t &atDw,uint32_t fenceOff,uint32_t value,
                       uint32_t &seen) {
    const Regs &r=w.r; const Hw &h=w.h; n33_psp_load_result &o=w.o;
    const uint32_t zero=0u,end=atDw+p.count,wptr[2]={end,0u};
    if(end>mq::kRingBytes/4u || !workWrite(h,fenceOff,&zero,1u) ||
       !workWrite(h,mq::kRingOff+4u*atDw,p.dw,p.count) || !workWrite(h,mq::kWptrOff,wptr,2u))
        return N33_PSPL_STAGE_VERIFY_FAILED;
    if(!w.sel(kQueueSel) || !w.put(r.v.q.poll1,1u<<1))return N33_PSPL_VMG_WRITE_FAILED;
    for(uint32_t ms=0;;++ms) {
        if(!workRead(h,fenceOff,&seen,1u))return N33_PSPL_READ_FAILED;
        if(seen==value)break;
        if(ms==kFallbackMs) {
            uint32_t rptr=0;
            if(!read(h,mq::hqd(r.v.q,kiq::kCpHqdPqRptr),rptr))return N33_PSPL_READ_FAILED;
            if(rptr==atDw) {   // the poll was not taken: write the WPTR itself, once
                if(!w.put(mq::hqd(r.v.q,kiq::kCpHqdPqWptrLo),end) ||
                   !w.put(mq::hqd(r.v.q,kiq::kCpHqdPqWptrHi),0u))return N33_PSPL_VMG_WRITE_FAILED;
                ++o.grad_fallbacks;
            }
        }
        if(ms==kTimeoutMs)break;
        h.sleep(h.ctx,1u);++o.elapsed_ms;
    }
    if(!read(h,mq::hqd(r.v.q,kiq::kCpHqdPqRptr),o.vm_rptr))return N33_PSPL_READ_FAILED;
    if(!w.sel(0u))return N33_PSPL_VMG_WRITE_FAILED;
    if(faulted(r,h,o))return N33_PSPL_VMG_FAULT;
    atDw=end;
    return seen==value ? N33_PSPL_PLAN_OK : N33_PSPL_VMG_NO_FENCE;
}
// Plan calls get no write callback. The run consumes the latch before its first write.
inline n33_psp_load_result execute(const Regs &r,const Layout &l,const Screen &screen,const uint8_t *co,
                                  size_t coBytes,const Hw &h,bool run,bool gate,bool &attempted) {
    auto o=blank();
    if(gate)o.flags|=N33_PSPL_FLAG_WRITE_GATE_OPEN;
    if(!r.valid){o.state=N33_PSPL_BAD_ROM;return o;}
    o.vm_window=static_cast<uint32_t>(l.windowMc>>20);
    if(!l.windowMc || (l.windowMc&(kWindowBytes-1u)) || (l.windowVram&(kWindowBytes-1u)) ||
       l.windowVram>=(1ull<<40)){o.state=N33_PSPL_NO_WINDOW;return o;}
    const gfx11_shader::Kernel k=gr::kernelOf(co,coBytes);
    if(k.code!=gfx11_shader::Code::Ok){o.state=N33_PSPL_BAD_FIRMWARE;return o;}
    o.flags|=N33_PSPL_FLAG_FW_PINNED;
    const bool onScreen=screenOk(screen);
    const gr::Packets a=gr::packets(k,kVaBase,gr::kArgsA,gr::kTestSize/8u,gr::kTestSize/8u,gr::kFenceA,kFenceValueA);
    const gr::Packets b=gr::packets(k,kVaBase,gr::kArgsB,screen.width/8u,screen.height/8u,gr::kFenceB,kFenceValueB);
    gfx11_mqd::Image img{};
    if(!a.ok || (onScreen && !b.ok) || !mqdImage(l,img)){o.state=N33_PSPL_BAD_REQUEST;return o;}
    o.grad_screen=onScreen ? (screen.width<<16)|screen.height : 0u;
    o.state=sample(r,h,o);
    if(o.state!=N33_PSPL_PLAN_OK || !run)return o;
    if(!gate){o.state=N33_PSPL_WRITE_DISABLED;return o;}
    if(attempted){o.state=N33_PSPL_ALREADY_ATTEMPTED;return o;}
    if(!h.write || !h.sleep || !h.vramWrite || !h.vramRead || (onScreen && !h.screenRead))
        {o.state=N33_PSPL_BAD_REQUEST;return o;}
    attempted=true;
    Writer w{r,h,o};
    const v1::Regs &v=r.v;
    // 1. VRAM: the whole window zeroed, the four-level tables, MQD, ring test,
    //    WPTR, shader, test arguments; everything but the zero fill read back.
    static uint32_t zero[1024];
    for(uint32_t p=0;p<kWindowBytes/4096u;++p)
        if(!h.vramWrite(h.ctx,4096u*p,zero,1024u)){o.state=N33_PSPL_STAGE_VERIFY_FAILED;return o;}
    static Tables t;tables(l,onScreen ? screen : Screen{},t);
    for(uint32_t p=0;p<kMqdPage;++p) {
        static uint32_t dw[1024];
        for(uint32_t i=0;i<512;++i){dw[2*i]=static_cast<uint32_t>(t.e[p][i]);dw[2*i+1]=static_cast<uint32_t>(t.e[p][i]>>32);}
        if(!vram(h,4096u*p,dw,1024u)){o.state=N33_PSPL_STAGE_VERIFY_FAILED;return o;}
    }
    uint32_t ring[mq::kRingDwords];mq::ringPackets(mq::Layout{kVaBase},ring);
    ring[8]=kFenceValue;   // the RELEASE_MEM data dword
    const uint32_t wptr[2]={mq::kRingDwords,0u};
    static uint32_t code[gr::kShaderMax/4u];
    for(uint32_t i=0;i<gr::kShaderMax/4u;++i)code[i]=0u;
    for(uint64_t i=0;i<k.codeBytes;++i)code[i/4u]|=uint32_t(co[k.codeOffset+i])<<(8u*(i%4u));
    uint32_t argA[6];gr::args(argA,kVaBase+gr::kImageOff,gr::kTestSize,gr::kTestSize,gr::kTestSize,kFrameA);
    if(!vram(h,4096u*kMqdPage,img.dw,gfx11_mqd::kDwords) || !workWrite(h,mq::kRingOff,ring,mq::kRingDwords) ||
       !workWrite(h,mq::kWptrOff,wptr,2u) ||
       !workWrite(h,gr::kShaderOff,code,static_cast<uint32_t>((k.codeBytes+255u)/256u*64u)) ||
       !workWrite(h,gr::kArgsA,argA,6u))
        {o.state=N33_PSPL_STAGE_VERIFY_FAILED;return o;}
    o.flags|=N33_PSPL_FLAG_WROTE_VRAM;
    // 2. gfxhub_v3_0 TLB, caches (with the invalidation), identity aperture off.
    uint32_t c2=0;
    if(!read(h,v.l2c2,c2))return w.stop(N33_PSPL_READ_FAILED);
    const struct { uint64_t off; uint32_t val; } hub[]={
        {v.l1,v1::l1Word(o.vm_l1_before)},{v.l2,v1::l2Word(o.vm_l2_before)},{v.l2c2,v1::l2Cntl2Word(c2)},
        {v.l2c3,v1::kL2Cntl3Value},{v.l2c4,v1::kL2Cntl4Value},{v.l2c5,v1::kL2Cntl5Value},
        {v.ident[0],v1::kIdentity[0]},{v.ident[1],v1::kIdentity[1]},{v.ident[2],v1::kIdentity[2]},
        {v.ident[3],v1::kIdentity[3]},{v.ident[4],v1::kIdentity[4]},{v.ident[5],v1::kIdentity[5]}};
    for(const auto &x:hub)if(!w.put(x.off,x.val))return w.stop(N33_PSPL_VMG_WRITE_FAILED);
    ++o.steps_done;
    // 3. Context 1: root PDB2, the whole 48-bit range, depth 3; enable last.
    const uint64_t base=(l.windowVram+4096ull*kPdb2)|kPdeValid;
    const struct { uint64_t off; uint32_t val; } ctx[]={
        {v.baseLo,static_cast<uint32_t>(base)},{v.baseHi,static_cast<uint32_t>(base>>32)},
        {v.startLo,0u},{v.startHi,0u},{v.endLo,kEndLo},{v.endHi,kEndHi},{v.ctx,ctx1Word(o.vm_ctx_before)}};
    for(const auto &x:ctx)if(!w.put(x.off,x.val))return w.stop(N33_PSPL_VMG_WRITE_FAILED);
    // 4. constants_init for VMID 1.
    if(!w.sel(kGrbmVmid1) || !w.put(r.shMem,gr::kShMemDefault) || !w.put(r.shBases,kShMemBasesValue) ||
       !w.sel(0u))return w.stop(N33_PSPL_VMG_WRITE_FAILED);
    ++o.steps_done;
    // 5. hqd_load_v11() for me 1 pipe 0 queue 1, VMID 1 (as G36), after the ring-test preset.
    if(!w.put(v.q.scratch,mq::kScratchPreset) || !w.sel(kQueueSel))return w.stop(N33_PSPL_VMG_WRITE_FAILED);
    uint32_t active=0;
    if(!read(h,mq::hqd(v.q,kiq::kCpHqdActive),active))return w.stop(N33_PSPL_READ_FAILED);
    if(active&1u)return w.stop(N33_PSPL_VMG_BAD_PRECONDITION);
    for(uint32_t dw=mq::kHqdFirst;dw<=mq::kHqdLast;++dw)
        if(dw!=mq::kHqdHole && !w.put(mq::hqd(v.q,dw),img.dw[gfx11_mqd::kCpMqdBaseAddrLo+(dw-mq::kHqdFirst)]))
            return w.stop(N33_PSPL_VMG_WRITE_FAILED);
    const uint64_t poll=kVaBase+mq::kWptrOff;
    const struct { uint32_t dw; uint32_t val; } tail[]={
        {kiq::kCpHqdPqDoorbellControl,img.dw[gfx11_mqd::kCpHqdPqDoorbellControl]|mq::kDoorbellEn},
        {kiq::kCpHqdPqWptrLo,0u},{kiq::kCpHqdPqWptrHi,0u},
        {kiq::kCpHqdPqWptrPollAddr,static_cast<uint32_t>(poll)},{kiq::kCpHqdPqWptrPollAddrHi,static_cast<uint32_t>(poll>>32)}};
    for(const auto &x:tail)if(!w.put(mq::hqd(v.q,x.dw),x.val))return w.stop(N33_PSPL_VMG_WRITE_FAILED);
    if(!w.put(v.q.poll1,1u<<1) ||
       !w.put(mq::hqd(v.q,mq::kEopRptr),img.dw[gfx11_mqd::kCpMqdBaseAddrLo+(mq::kEopRptr-mq::kHqdFirst)]|
              mq::kEopInitFetcher) ||
       !w.put(mq::hqd(v.q,kiq::kCpHqdActive),img.dw[gfx11_mqd::kCpHqdActive]|1u) || !w.sel(0u))
        return w.stop(N33_PSPL_VMG_WRITE_FAILED);
    // 6. The ring test and its fence, through four levels of translation.
    for(uint32_t ms=0;;++ms) {
        if(!read(h,v.q.scratch,o.vm_scratch) || !workRead(h,mq::kFenceOff,&o.vm_fence,1u))
            return w.stop(N33_PSPL_READ_FAILED);
        if(o.vm_scratch==mq::kScratchDone && o.vm_fence==kFenceValue)break;
        if(ms==kTimeoutMs)break;
        h.sleep(h.ctx,1u);++o.elapsed_ms;
    }
    if(!w.sel(kQueueSel))return w.stop(N33_PSPL_VMG_WRITE_FAILED);
    if(!read(h,mq::hqd(v.q,kiq::kCpHqdPqRptr),o.vm_rptr) || !read(h,mq::hqd(v.q,kiq::kCpHqdActive),o.vm_active))
        return w.stop(N33_PSPL_READ_FAILED);
    if(!w.sel(0u))return w.stop(N33_PSPL_VMG_WRITE_FAILED);
    o.grad_rptr_before=o.vm_rptr;
    if(faulted(r,h,o)){o.state=N33_PSPL_VMG_FAULT;return o;}
    if(o.vm_scratch!=mq::kScratchDone){o.state=N33_PSPL_VMG_NO_FETCH;return o;}
    if(o.vm_fence!=kFenceValue){o.state=N33_PSPL_VMG_NO_FENCE;return o;}
    ++o.steps_done;
    // 7. The 64x64 test dispatch in VMID 1, checked against the CPU reference.
    uint32_t atDw=mq::kRingDwords;
    uint32_t st=submit(w,a,atDw,gr::kFenceA,kFenceValueA,o.grad_fence_a);
    o.grad_rptr_after=o.vm_rptr;
    if(st!=N33_PSPL_PLAN_OK)return w.stop(st);
    static uint32_t image[gr::kImageBytes/4u];
    if(!workRead(h,gr::kImageOff,image,gr::kImageBytes/4u)){o.state=N33_PSPL_READ_FAILED;return o;}
    o.grad_first_pixel=image[gr::kTestSize+1u];
    for(uint32_t y=0;y<gr::kTestSize;++y)for(uint32_t x=0;x<gr::kTestSize;++x)
        if(image[y*gr::kTestSize+x]!=gfx11_shader::gradientPixel(x,y,gr::kTestSize,gr::kTestSize,kFrameA))
            ++o.grad_bad_pixels;
    if(o.grad_bad_pixels){o.state=N33_PSPL_VMG_BAD_PIXELS;return o;}
    ++o.steps_done;
    if(!onScreen){o.state=N33_PSPL_VMG_OK;return o;}
    // 8. The screen through its own VA range, then a spread of samples.
    uint32_t argB[6];gr::args(argB,kScreenVa,screen.pitch,screen.width,screen.height,kFrameB);
    if(!workWrite(h,gr::kArgsB,argB,6u)){o.state=N33_PSPL_STAGE_VERIFY_FAILED;return o;}
    st=submit(w,b,atDw,gr::kFenceB,kFenceValueB,o.grad_fence_b);
    o.grad_rptr_after=o.vm_rptr;
    if(st!=N33_PSPL_PLAN_OK)return w.stop(st);
    for(uint32_t i=0;i<gr::kScreenSamples;++i) {
        const uint32_t x=(i*977u+13u)%screen.width, y=(i*389u+5u)%screen.height;
        uint32_t px=0;
        if(!h.screenRead(h.ctx,4u*(y*screen.pitch+x),&px)){o.state=N33_PSPL_READ_FAILED;return o;}
        if(px!=gfx11_shader::gradientPixel(x,y,screen.width,screen.height,kFrameB))++o.grad_screen_bad;
    }
    ++o.steps_done;
    o.state=o.grad_screen_bad ? N33_PSPL_VMG_BAD_PIXELS : N33_PSPL_VMG_ON_SCREEN;
    return o;
}
} }
