// SPDX-License-Identifier: MIT
// G39 (Phase B2): G38's VMID 1 gradient with real memory management. The kext's
// VRAM heap (Navi33VramHeap.h) covers all 8176 MiB; the boot console, the
// bring-up windows at 240..256 MiB (the PSP's ring still lives there, as does the
// MEC queue 0) and the firmware-owned top of VRAM are reserved for good. The
// page tables come from that heap through the address-space manager
// (Navi33Gmc11Vm.h), and so do the MQD, the queue, the shader, the kernel
// arguments and the test image, each its own buffer at its own VA. The console
// framebuffer is mapped into VMID 1 without being allocated (it stays reserved).
// Register writes are G38's, through its allowlist and writer. Buffers are never
// freed once the queue may have seen them: a live GPU can still reach them.
// No hardware I/O except callbacks.
#pragma once
#include "Navi33Gfx11VmGradient.h"
#include "Navi33Gmc11Vm.h"
namespace navi33 { namespace vm_alloc {
namespace vg=vm_gradient;
namespace vh=vram_heap;
namespace gv=gmc11_vm;
namespace mq=mec_queue;
namespace kiq=gfx11_kiq;
namespace gd=gfx11_dispatch;
namespace gr=gradient;
namespace v1=vmid1;
constexpr uint64_t MiB=1ull<<20;
// RCC_CONFIG_MEMSIZE 0x1ff0 on this card (findings log, 2026-10-17).
constexpr uint64_t kVramBytes=8176u*MiB;
// Reservations: the boot console and the VGA area (at least 16 MiB), the
// bring-up windows, and the top 112 MiB (the 19 MiB TMR at 8128 MiB, the IP
// discovery table and VBIOS-reserved memory that amdgpu keeps at the top).
constexpr uint64_t kBootMin=16u*MiB, kStagesAt=240u*MiB, kStagesBytes=16u*MiB, kTopAt=8064u*MiB;
constexpr uint32_t kTagBoot=0x424f4f54u, kTagStages=0x53544753u, kTagTop=0x544f5020u, kTagBo=0x564d4131u;
// VA layout (G38's base; all four directory indices non-zero).
constexpr uint64_t kQueueVa=vg::kVaBase, kShaderVa=vg::kVaBase+0x10000u, kArgsVa=vg::kVaBase+0x20000u;
constexpr uint64_t kImageVa=vg::kVaBase+0x30000u, kScreenVa=vg::kVaBase+(2u*MiB);
constexpr uint32_t kQueueBytes=0x5000u;        // mec_queue layout: EOP, ring, RPTR/WPTR, fences
constexpr uint32_t kFenceA=mq::kFenceOff+0x10u, kFenceB=mq::kFenceOff+0x20u;
constexpr uint32_t kArgsB=0x100u;
constexpr uint32_t kFenceValue=0x0c0b1640u, kFenceValueA=0x0c0b1641u, kFenceValueB=0x0c0b1642u;
constexpr uint32_t kFrameA=7u, kFrameB=64u;
constexpr uint32_t kTimeoutMs=1000u, kFallbackMs=200u;

// The heap as the kext configures it once per boot: everything that is not ours
// is pinned before the first allocation.
inline bool reservations(vh::Heap &heap,uint64_t visibleBytes,uint64_t consoleOffset,uint64_t consoleBytes) {
    if(heap.total())return heap.total()==kVramBytes;      // already configured this boot
    uint64_t boot=consoleOffset+consoleBytes;
    if(consoleOffset>kStagesAt || consoleBytes>kStagesAt || boot>kStagesAt)return false;
    boot=(boot+MiB-1u)&~(MiB-1u);
    if(boot<kBootMin)boot=kBootMin;
    return visibleBytes>=kStagesAt+kStagesBytes && visibleBytes<=kTopAt &&
        heap.configure(kVramBytes,visibleBytes)==vh::Code::Ok &&
        heap.reserve(0u,boot,kTagBoot)==vh::Code::Ok &&
        heap.reserve(kStagesAt,kStagesBytes,kTagStages)==vh::Code::Ok &&
        heap.reserve(kTopAt,kVramBytes-kTopAt,kTagTop)==vh::Code::Ok;
}
struct Layout { uint64_t fbBaseMc=0, visibleBytes=0; };
struct Hw {
    void *ctx=nullptr;
    bool (*read)(void *,uint64_t,uint32_t *)=nullptr;
    bool (*write)(void *,uint64_t,uint32_t)=nullptr;
    // CPU access to VRAM by offset; the IOKit side refuses anything outside a
    // live, unpinned heap block.
    bool (*vramWrite)(void *,uint64_t off,const uint32_t *src,uint32_t dwords)=nullptr;
    bool (*vramRead)(void *,uint64_t off,uint32_t *dst,uint32_t dwords)=nullptr;
    bool (*screenRead)(void *,uint32_t byteOff,uint32_t *dst)=nullptr;
    void (*sleep)(void *,uint32_t)=nullptr;
};
inline n33_psp_load_result blank() {
    n33_psp_load_result o{};o.abi_version=N33_ABI_VERSION;o.size=sizeof(o);
    o.vm_marker=N33_VMA_MARKER;o.grad_marker=N33_VMA_MARKER;return o;
}
// vg::Hw view for G38's sample()/Writer (register access only).
inline vg::Hw regsOnly(const Hw &h) { vg::Hw v{};v.ctx=h.ctx;v.read=h.read;v.write=h.write;v.sleep=h.sleep;return v; }
inline bool vram(const Hw &h,uint64_t off,const uint32_t *src,uint32_t n) {
    if(!h.vramWrite(h.ctx,off,src,n))return false;
    for(uint32_t i=0;i<n;++i) {
        uint32_t v=0;
        if(!h.vramRead(h.ctx,off+4u*i,&v,1u) || v!=src[i])return false;
    }
    return true;
}
// Page-table writes for the manager, verified like every other CPU write.
struct TableIo { const Hw *h; };
inline bool zeroPage(void *c,uint64_t off) {
    static const uint32_t zero[1024]={};
    const Hw &h=*static_cast<TableIo *>(c)->h;
    return h.vramWrite(h.ctx,off,zero,1024u);
}
inline bool putEntry(void *c,uint64_t off,uint64_t value) {
    const uint32_t dw[2]={static_cast<uint32_t>(value),static_cast<uint32_t>(value>>32)};
    return vram(*static_cast<TableIo *>(c)->h,off,dw,2u);
}
struct Bo { vh::Handle h{}; uint64_t vram=0, va=0; uint32_t bytes=0; };
// One packet run: thread masks, the G32 dispatch, NOPs to a multiple of 8.
inline gr::Packets packets(const gfx11_shader::Kernel &k,uint64_t args,uint32_t gx,uint32_t gy,uint64_t fence,
                           uint32_t value) {
    gr::Packets p{};
    gd::Request rq{};rq.kernel=&k;rq.shaderMc=kShaderVa;rq.kernargMc=args;rq.groupsX=gx;rq.groupsY=gy;
    rq.fenceMc=fence;rq.fenceValue=value;
    const gd::Stream s=gd::encode(rq);
    if(s.code!=gd::Code::Ok)return p;
    const uint32_t masks[]={gd::packet3(gd::kSetShReg,2u),gd::shOffset(gr::kThreadSe0),~0u,~0u,
                            gd::packet3(gd::kSetShReg,2u),gd::shOffset(gr::kThreadSe2),~0u,~0u};
    for(uint32_t v:masks)p.dw[p.count++]=v;
    for(size_t i=0;i<s.count;++i)p.dw[p.count++]=s.dwords[i];
    while(p.count%8u)p.dw[p.count++]=mq::kNop;
    p.ok=true;return p;
}
inline uint32_t submit(vg::Writer &w,const Hw &h,const Bo &q,const gr::Packets &p,uint32_t &atDw,uint32_t fenceOff,
                       uint32_t value,uint32_t &seen) {
    const vg::Regs &r=w.r; n33_psp_load_result &o=w.o;
    const uint32_t zero=0u,end=atDw+p.count,wptr[2]={end,0u};
    if(end>mq::kRingBytes/4u || !vram(h,q.vram+fenceOff,&zero,1u) ||
       !vram(h,q.vram+mq::kRingOff+4u*atDw,p.dw,p.count) || !vram(h,q.vram+mq::kWptrOff,wptr,2u))
        return N33_PSPL_STAGE_VERIFY_FAILED;
    if(!w.sel(vg::kQueueSel) || !w.put(r.v.q.poll1,1u<<1))return N33_PSPL_VMG_WRITE_FAILED;
    for(uint32_t ms=0;;++ms) {
        if(!h.vramRead(h.ctx,q.vram+fenceOff,&seen,1u))return N33_PSPL_READ_FAILED;
        if(seen==value)break;
        if(ms==kFallbackMs) {
            uint32_t rptr=0;
            if(!vg::read(w.h,mq::hqd(r.v.q,kiq::kCpHqdPqRptr),rptr))return N33_PSPL_READ_FAILED;
            if(rptr==atDw) {
                if(!w.put(mq::hqd(r.v.q,kiq::kCpHqdPqWptrLo),end) ||
                   !w.put(mq::hqd(r.v.q,kiq::kCpHqdPqWptrHi),0u))return N33_PSPL_VMG_WRITE_FAILED;
                ++o.grad_fallbacks;
            }
        }
        if(ms==kTimeoutMs)break;
        h.sleep(h.ctx,1u);++o.elapsed_ms;
    }
    if(!vg::read(w.h,mq::hqd(r.v.q,kiq::kCpHqdPqRptr),o.vm_rptr))return N33_PSPL_READ_FAILED;
    if(!w.sel(0u))return N33_PSPL_VMG_WRITE_FAILED;
    if(vg::faulted(r,w.h,o))return N33_PSPL_VMG_FAULT;
    atDw=end;
    return seen==value ? N33_PSPL_PLAN_OK : N33_PSPL_VMG_NO_FENCE;
}
// heap/space live in the provider for the whole boot. Plan calls get no write
// callback and never touch the heap. The run consumes the latch before the
// first allocation.
inline n33_psp_load_result execute(const vg::Regs &r,const Layout &l,const vg::Screen &screen,const uint8_t *co,
                                  size_t coBytes,vh::Heap &heap,gv::Space &space,const Hw &h,bool run,bool gate,
                                  bool &attempted) {
    auto o=blank();
    if(gate)o.flags|=N33_PSPL_FLAG_WRITE_GATE_OPEN;
    if(!r.valid){o.state=N33_PSPL_BAD_ROM;return o;}
    if(!l.fbBaseMc || (l.fbBaseMc&(MiB-1u)) || !l.visibleBytes){o.state=N33_PSPL_NO_WINDOW;return o;}
    const gfx11_shader::Kernel k=gr::kernelOf(co,coBytes);
    if(k.code!=gfx11_shader::Code::Ok){o.state=N33_PSPL_BAD_FIRMWARE;return o;}
    o.flags|=N33_PSPL_FLAG_FW_PINNED;
    const bool onScreen=vg::screenOk(screen);
    const gr::Packets a=packets(k,kArgsVa,gr::kTestSize/8u,gr::kTestSize/8u,kQueueVa+kFenceA,kFenceValueA);
    const gr::Packets b=packets(k,kArgsVa+kArgsB,screen.width/8u,screen.height/8u,kQueueVa+kFenceB,kFenceValueB);
    if(!a.ok || (onScreen && !b.ok)){o.state=N33_PSPL_BAD_REQUEST;return o;}
    o.grad_screen=onScreen ? (screen.width<<16)|screen.height : 0u;
    const vg::Hw rh=regsOnly(h);
    o.state=vg::sample(r,rh,o);
    if(o.state==N33_PSPL_VMG_BAD_PRECONDITION)o.state=N33_PSPL_VMA_BAD_PRECONDITION;
    if(o.state!=N33_PSPL_PLAN_OK || !run)return o;
    if(!gate){o.state=N33_PSPL_WRITE_DISABLED;return o;}
    if(attempted){o.state=N33_PSPL_ALREADY_ATTEMPTED;return o;}
    if(!h.write || !h.sleep || !h.vramWrite || !h.vramRead || (onScreen && !h.screenRead))
        {o.state=N33_PSPL_BAD_REQUEST;return o;}
    attempted=true;
    // 1. Memory: the heap (once per boot), five buffers, the address space.
    if(!reservations(heap,l.visibleBytes,screen.vram,onScreen ? screen.bytes : 0u)){o.state=N33_PSPL_VMA_NO_MEMORY;return o;}
    Bo q{},mqd{},sh{},args{},img{};
    struct Want { Bo *bo; uint32_t bytes; uint64_t va; };
    const Want want[]={{&q,kQueueBytes,kQueueVa},{&mqd,4096u,0u},{&sh,gr::kShaderMax,kShaderVa},
                       {&args,4096u,kArgsVa},{&img,gr::kImageBytes,kImageVa}};
    for(const Want &x:want) {
        const vh::Alloc al=heap.alloc(x.bytes,4096u,vh::Place::Visible,kTagBo);
        if(al.code!=vh::Code::Ok){o.state=N33_PSPL_VMA_NO_MEMORY;return o;}
        *x.bo={al.h,al.offset,x.va,x.bytes};
    }
    static TableIo tio;tio.h=&h;
    const gv::Io io{&tio,zeroPage,putEntry};
    if(space.init(heap,io)!=gv::Code::Ok){o.state=N33_PSPL_VMA_NO_MEMORY;return o;}
    const Bo *mapped[]={&q,&sh,&args,&img};
    for(const Bo *bo:mapped)
        if(space.map(bo->va,bo->vram,bo->bytes,gv::kPteVram)!=gv::Code::Ok){o.state=N33_PSPL_VMA_NO_MEMORY;return o;}
    if(onScreen && space.map(kScreenVa,screen.vram,screen.bytes,gv::kPteVram)!=gv::Code::Ok)
        {o.state=N33_PSPL_VMA_NO_MEMORY;return o;}
    o.vm_window=static_cast<uint32_t>(space.rootVram()>>12);
    o.vm_reserved=static_cast<uint32_t>(space.tables())|(5u<<16);
    o.flags|=N33_PSPL_FLAG_WROTE_VRAM;
    // 2. Contents: MQD (fetched in VMID 0 at its MC address), ring test, WPTR,
    //    shader, test arguments; zeroed buffers first, everything read back.
    static const uint32_t zero[1024]={};
    const Bo *all[]={&q,&mqd,&sh,&args,&img};
    for(const Bo *bo:all)
        for(uint32_t p=0;p<bo->bytes;p+=4096u)
            if(!h.vramWrite(h.ctx,bo->vram+p,zero,1024u)){o.state=N33_PSPL_STAGE_VERIFY_FAILED;return o;}
    const gfx11_mqd::Result m=gfx11_mqd::buildKiqMqd(mq::queueFor(mq::Layout{kQueueVa}));
    if(m.code!=kiq::Code::PlanOnly){o.state=N33_PSPL_BAD_REQUEST;return o;}
    gfx11_mqd::Image img0=m.image;
    const uint64_t mqdMc=l.fbBaseMc+mqd.vram;
    img0.dw[gfx11_mqd::kCpMqdBaseAddrLo]=static_cast<uint32_t>(mqdMc&0xfffffffcu);
    img0.dw[gfx11_mqd::kCpMqdBaseAddrHi]=static_cast<uint32_t>(mqdMc>>32);
    img0.dw[gfx11_mqd::kCpHqdVmid]=1u;
    uint32_t ring[mq::kRingDwords];mq::ringPackets(mq::Layout{kQueueVa},ring);
    ring[8]=kFenceValue;
    const uint32_t wptr[2]={mq::kRingDwords,0u};
    static uint32_t code[gr::kShaderMax/4u];
    for(uint32_t i=0;i<gr::kShaderMax/4u;++i)code[i]=0u;
    for(uint64_t i=0;i<k.codeBytes;++i)code[i/4u]|=uint32_t(co[k.codeOffset+i])<<(8u*(i%4u));
    uint32_t argA[6];gr::args(argA,kImageVa,gr::kTestSize,gr::kTestSize,gr::kTestSize,kFrameA);
    if(!vram(h,mqd.vram,img0.dw,gfx11_mqd::kDwords) || !vram(h,q.vram+mq::kRingOff,ring,mq::kRingDwords) ||
       !vram(h,q.vram+mq::kWptrOff,wptr,2u) ||
       !vram(h,sh.vram,code,static_cast<uint32_t>((k.codeBytes+255u)/256u*64u)) || !vram(h,args.vram,argA,6u))
        {o.state=N33_PSPL_STAGE_VERIFY_FAILED;return o;}
    // 3. G38's registers: hub, context 1 at the manager's root, SH_MEM for VMID 1, queue 1.
    vg::Writer w{r,rh,o};
    const v1::Regs &v=r.v;
    uint32_t c2=0;
    if(!vg::read(rh,v.l2c2,c2))return w.stop(N33_PSPL_READ_FAILED);
    const struct { uint64_t off; uint32_t val; } hub[]={
        {v.l1,v1::l1Word(o.vm_l1_before)},{v.l2,v1::l2Word(o.vm_l2_before)},{v.l2c2,v1::l2Cntl2Word(c2)},
        {v.l2c3,v1::kL2Cntl3Value},{v.l2c4,v1::kL2Cntl4Value},{v.l2c5,v1::kL2Cntl5Value},
        {v.ident[0],v1::kIdentity[0]},{v.ident[1],v1::kIdentity[1]},{v.ident[2],v1::kIdentity[2]},
        {v.ident[3],v1::kIdentity[3]},{v.ident[4],v1::kIdentity[4]},{v.ident[5],v1::kIdentity[5]}};
    for(const auto &x:hub)if(!w.put(x.off,x.val))return w.stop(N33_PSPL_VMG_WRITE_FAILED);
    ++o.steps_done;
    const uint64_t base=space.rootVram()|gv::kPdeValid;
    const struct { uint64_t off; uint32_t val; } ctx[]={
        {v.baseLo,static_cast<uint32_t>(base)},{v.baseHi,static_cast<uint32_t>(base>>32)},
        {v.startLo,0u},{v.startHi,0u},{v.endLo,vg::kEndLo},{v.endHi,vg::kEndHi},{v.ctx,vg::ctx1Word(o.vm_ctx_before)}};
    for(const auto &x:ctx)if(!w.put(x.off,x.val))return w.stop(N33_PSPL_VMG_WRITE_FAILED);
    if(!w.sel(vg::kGrbmVmid1) || !w.put(r.shMem,gr::kShMemDefault) || !w.put(r.shBases,vg::kShMemBasesValue) ||
       !w.sel(0u))return w.stop(N33_PSPL_VMG_WRITE_FAILED);
    ++o.steps_done;
    if(!w.put(v.q.scratch,mq::kScratchPreset) || !w.sel(vg::kQueueSel))return w.stop(N33_PSPL_VMG_WRITE_FAILED);
    uint32_t active=0;
    if(!vg::read(rh,mq::hqd(v.q,kiq::kCpHqdActive),active))return w.stop(N33_PSPL_READ_FAILED);
    if(active&1u)return w.stop(N33_PSPL_VMA_BAD_PRECONDITION);
    for(uint32_t dw=mq::kHqdFirst;dw<=mq::kHqdLast;++dw)
        if(dw!=mq::kHqdHole && !w.put(mq::hqd(v.q,dw),img0.dw[gfx11_mqd::kCpMqdBaseAddrLo+(dw-mq::kHqdFirst)]))
            return w.stop(N33_PSPL_VMG_WRITE_FAILED);
    const uint64_t poll=kQueueVa+mq::kWptrOff;
    const struct { uint32_t dw; uint32_t val; } tail[]={
        {kiq::kCpHqdPqDoorbellControl,img0.dw[gfx11_mqd::kCpHqdPqDoorbellControl]|mq::kDoorbellEn},
        {kiq::kCpHqdPqWptrLo,0u},{kiq::kCpHqdPqWptrHi,0u},
        {kiq::kCpHqdPqWptrPollAddr,static_cast<uint32_t>(poll)},{kiq::kCpHqdPqWptrPollAddrHi,static_cast<uint32_t>(poll>>32)}};
    for(const auto &x:tail)if(!w.put(mq::hqd(v.q,x.dw),x.val))return w.stop(N33_PSPL_VMG_WRITE_FAILED);
    if(!w.put(v.q.poll1,1u<<1) ||
       !w.put(mq::hqd(v.q,mq::kEopRptr),img0.dw[gfx11_mqd::kCpMqdBaseAddrLo+(mq::kEopRptr-mq::kHqdFirst)]|
              mq::kEopInitFetcher) ||
       !w.put(mq::hqd(v.q,kiq::kCpHqdActive),img0.dw[gfx11_mqd::kCpHqdActive]|1u) || !w.sel(0u))
        return w.stop(N33_PSPL_VMG_WRITE_FAILED);
    // 4. Ring test and fence through the manager's tables.
    for(uint32_t ms=0;;++ms) {
        if(!vg::read(rh,v.q.scratch,o.vm_scratch) || !h.vramRead(h.ctx,q.vram+mq::kFenceOff,&o.vm_fence,1u))
            return w.stop(N33_PSPL_READ_FAILED);
        if(o.vm_scratch==mq::kScratchDone && o.vm_fence==kFenceValue)break;
        if(ms==kTimeoutMs)break;
        h.sleep(h.ctx,1u);++o.elapsed_ms;
    }
    if(!w.sel(vg::kQueueSel))return w.stop(N33_PSPL_VMG_WRITE_FAILED);
    if(!vg::read(rh,mq::hqd(v.q,kiq::kCpHqdPqRptr),o.vm_rptr) || !vg::read(rh,mq::hqd(v.q,kiq::kCpHqdActive),o.vm_active))
        return w.stop(N33_PSPL_READ_FAILED);
    if(!w.sel(0u))return w.stop(N33_PSPL_VMG_WRITE_FAILED);
    o.grad_rptr_before=o.vm_rptr;
    if(vg::faulted(r,rh,o)){o.state=N33_PSPL_VMG_FAULT;return o;}
    if(o.vm_scratch!=mq::kScratchDone){o.state=N33_PSPL_VMG_NO_FETCH;return o;}
    if(o.vm_fence!=kFenceValue){o.state=N33_PSPL_VMG_NO_FENCE;return o;}
    ++o.steps_done;
    // 5. The 64x64 test image.
    uint32_t atDw=mq::kRingDwords;
    uint32_t st=submit(w,h,q,a,atDw,kFenceA,kFenceValueA,o.grad_fence_a);
    o.grad_rptr_after=o.vm_rptr;
    if(st!=N33_PSPL_PLAN_OK)return w.stop(st);
    static uint32_t image[gr::kImageBytes/4u];
    if(!h.vramRead(h.ctx,img.vram,image,gr::kImageBytes/4u)){o.state=N33_PSPL_READ_FAILED;return o;}
    o.grad_first_pixel=image[gr::kTestSize+1u];
    for(uint32_t y=0;y<gr::kTestSize;++y)for(uint32_t x=0;x<gr::kTestSize;++x)
        if(image[y*gr::kTestSize+x]!=gfx11_shader::gradientPixel(x,y,gr::kTestSize,gr::kTestSize,kFrameA))
            ++o.grad_bad_pixels;
    if(o.grad_bad_pixels){o.state=N33_PSPL_VMG_BAD_PIXELS;return o;}
    ++o.steps_done;
    if(!onScreen){o.state=N33_PSPL_VMA_OK;return o;}
    // 6. The screen through its VA range.
    uint32_t argB[6];gr::args(argB,kScreenVa,screen.pitch,screen.width,screen.height,kFrameB);
    if(!vram(h,args.vram+kArgsB,argB,6u)){o.state=N33_PSPL_STAGE_VERIFY_FAILED;return o;}
    st=submit(w,h,q,b,atDw,kFenceB,kFenceValueB,o.grad_fence_b);
    o.grad_rptr_after=o.vm_rptr;
    if(st!=N33_PSPL_PLAN_OK)return w.stop(st);
    for(uint32_t i=0;i<gr::kScreenSamples;++i) {
        const uint32_t x=(i*977u+13u)%screen.width, y=(i*389u+5u)%screen.height;
        uint32_t px=0;
        if(!h.screenRead(h.ctx,4u*(y*screen.pitch+x),&px)){o.state=N33_PSPL_READ_FAILED;return o;}
        if(px!=gfx11_shader::gradientPixel(x,y,screen.width,screen.height,kFrameB))++o.grad_screen_bad;
    }
    ++o.steps_done;
    o.state=o.grad_screen_bad ? N33_PSPL_VMG_BAD_PIXELS : N33_PSPL_VMA_ON_SCREEN;
    return o;
}
} }
