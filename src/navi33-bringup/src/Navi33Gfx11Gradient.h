// SPDX-License-Identifier: MIT
// First GPU-calculated pixels on GC 11.0.2 (G35): shaders/gradient.co on the
// compute queue G34 left running (me 1 pipe 0 queue 0, VMID 0, ring in the
// 1 MiB VRAM window at 250 MiB). Upstream, torvalds/linux master 2026-10-19:
//  - gfx_v11_0_constants_init(): SH_MEM_CONFIG = DEFAULT_SH_MEM_CONFIG for
//    VMID 0 (64-bit, unaligned, INITIAL_INST_PREFETCH 3), SH_MEM_BASES untouched.
//  - Mesa RADV's compute preamble: COMPUTE_STATIC_THREAD_MGMT_SE0..3 all ones,
//    sent on the ring as SET_SH_REG (gc_11_0_0_offset.h 0x1bb6/7, 0x1bb9/a).
//  - The dispatch itself is the G32 stream (Navi33Gfx11Dispatch.h).
// Kicked as G34 was: WPTR in VRAM, one-shot CP_PQ_WPTR_POLL_CNTL1. If the RPTR
// has not moved after 200 ms, the runner writes CP_HQD_PQ_WPTR_LO/HI once.
// First a 64x64 image inside the window, checked pixel by pixel against the
// CPU reference; only then, if a screen is given, the full gradient into it.
// No hardware I/O except callbacks.
#pragma once
#include "Navi33Gfx11MecQueue.h"
#include "Navi33Gfx11Shader.h"
#include "Navi33Sha256.h"
namespace navi33 { namespace gradient {
namespace mq=mec_queue;
namespace gd=gfx11_dispatch;
namespace kiq=gfx11_kiq;
constexpr uint32_t kShMemConfig=0x09e4u;            // GC seg 1
constexpr uint32_t kShMemDefault=0x0u|(3u<<2)|(3u<<14);
static_assert(kShMemDefault==0xc00cu,"DEFAULT_SH_MEM_CONFIG");
constexpr uint32_t kThreadSe0=0x1bb6u, kThreadSe2=0x1bb9u;
// shaders/gradient.co as committed (built by shaders/build.sh).
constexpr size_t kCodeObjectBytes=4824u;
constexpr sha256::Digest kCodeObjectSha256={{
    0x11,0x48,0x5a,0x75,0x76,0x20,0x41,0x9a,0x8f,0x08,0x0e,0xa5,0x3b,0x04,0x3e,0xb7,
    0x51,0xbe,0x71,0x99,0x0a,0xb2,0x5e,0xa6,0xa6,0x38,0x05,0x5a,0x9b,0x7f,0xd8,0x00}};
// Window layout on top of G34's (bytes from the window start).
constexpr uint32_t kFenceA=0x4010u, kFenceB=0x4020u, kShaderOff=0x10000u, kShaderMax=0x8000u;
constexpr uint32_t kArgsA=0x18000u, kArgsB=0x18100u, kImageOff=0x20000u;
constexpr uint32_t kTestSize=64u, kImageBytes=kTestSize*kTestSize*4u, kFrameA=7u, kFrameB=0u;
constexpr uint32_t kFenceValueA=0x0c0b1602u, kFenceValueB=0x0c0b1603u;
constexpr uint32_t kRingStart=mq::kRingDwords;      // G34 consumed 16 dwords
constexpr uint32_t kTimeoutMs=1000u, kFallbackMs=200u, kScreenSamples=64u;
constexpr unsigned kMaxWrites=16u;

struct Regs { mq::Regs q{}; uint64_t shMem=0; bool valid=false; };
inline Regs resolve(const IPDiscovery &d,uint64_t bytes) {
    Regs r{};r.q=mq::resolve(d,bytes);
    const auto a=resolveDword(d,Engine::GFX11,1,kShMemConfig,bytes);
    if(!r.q.valid || a.code!=AddressCode::Pass)return r;
    r.shMem=a.bar5ByteOffset;r.valid=true;return r;
}
// The run may write only these: GRBM select, SH_MEM_CONFIG, the poll trigger
// and (fallback) the queue's WPTR.
inline bool allowed(const Regs &r,uint64_t off) {
    return r.valid && (off==r.q.grbm || off==r.shMem || off==r.q.poll1 ||
        off==mq::hqd(r.q,kiq::kCpHqdPqWptrLo) || off==mq::hqd(r.q,kiq::kCpHqdPqWptrHi));
}
struct Screen { uint64_t mc=0; uint32_t width=0, height=0, pitch=0; };  // width 0: no screen pass
struct Hw {
    void *ctx=nullptr;
    bool (*read)(void *,uint64_t,uint32_t *)=nullptr;
    bool (*write)(void *,uint64_t,uint32_t)=nullptr;
    bool (*vramWrite)(void *,uint32_t off,const uint32_t *src,uint32_t dwords)=nullptr;
    bool (*vramRead)(void *,uint32_t off,uint32_t *dst,uint32_t dwords)=nullptr;
    bool (*screenRead)(void *,uint32_t byteOff,uint32_t *dst)=nullptr;   // one pixel
    void (*sleep)(void *,uint32_t)=nullptr;
};
inline bool read(const Hw &h,uint64_t off,uint32_t &v) {
    return h.read && h.read(h.ctx,off,&v) && v!=0xffffffffu;
}
inline n33_psp_load_result blank() {
    n33_psp_load_result o{};o.abi_version=N33_ABI_VERSION;o.size=sizeof(o);
    o.grad_marker=N33_GRAD_MARKER;return o;
}
inline gfx11_shader::Kernel kernelOf(const uint8_t *co,size_t bytes) {
    gfx11_shader::Kernel k{};
    if(!co || bytes!=kCodeObjectBytes ||
       !sha256::equal(sha256::hash(co,bytes),kCodeObjectSha256))return k;
    k=gfx11_shader::parse(co,bytes,"cobalt_gradient");
    if(k.code==gfx11_shader::Code::Ok && (k.codeBytes>kShaderMax || k.kernargBytes>24u))
        k.code=gfx11_shader::Code::BadDescriptor;
    return k;
}
// One submission: thread masks, the G32 dispatch, NOPs to a multiple of 8.
struct Packets { uint32_t dw[gd::kMaxDwords+16u]{}; uint32_t count=0; bool ok=false; };
inline Packets packets(const gfx11_shader::Kernel &k,uint64_t windowMc,uint32_t args,
                       uint32_t groupsX,uint32_t groupsY,uint32_t fenceOff,uint32_t fenceValue) {
    Packets p{};
    gd::Request rq{};rq.kernel=&k;rq.shaderMc=windowMc+kShaderOff;rq.kernargMc=windowMc+args;
    rq.groupsX=groupsX;rq.groupsY=groupsY;rq.fenceMc=windowMc+fenceOff;rq.fenceValue=fenceValue;
    const gd::Stream s=gd::encode(rq);
    if(s.code!=gd::Code::Ok)return p;
    const uint32_t masks[]={gd::packet3(gd::kSetShReg,2u),gd::shOffset(kThreadSe0),~0u,~0u,
                            gd::packet3(gd::kSetShReg,2u),gd::shOffset(kThreadSe2),~0u,~0u};
    for(uint32_t v:masks)p.dw[p.count++]=v;
    for(size_t i=0;i<s.count;++i)p.dw[p.count++]=s.dwords[i];
    while(p.count%8u)p.dw[p.count++]=mq::kNop;
    p.ok=true;return p;
}
inline void args(uint32_t out[6],uint64_t pixelsMc,uint32_t pitch,uint32_t w,uint32_t h,uint32_t frame) {
    uint8_t b[24];gd::gradientKernargs(b,pixelsMc,pitch,w,h,frame);
    for(int i=0;i<6;++i)out[i]=b[4*i]|(b[4*i+1]<<8)|(b[4*i+2]<<16)|(uint32_t(b[4*i+3])<<24);
}
inline uint32_t sample(const Regs &r,const Hw &h,n33_psp_load_result &o) {
    uint32_t sos=0,rlc=0,grbm=0,cntl=0,scratch=0,g34[2]={0,0};
    if(!read(h,r.q.sos,sos) || !read(h,r.q.rlc,rlc) || !read(h,r.q.grbm,grbm) ||
       !read(h,r.q.cntl,cntl) || !read(h,r.q.scratch,scratch) || !read(h,r.shMem,o.grad_shmem_before) ||
       !h.vramRead || !h.vramRead(h.ctx,mq::kFenceOff,&g34[0],1u) ||
       !h.vramRead(h.ctx,mq::kWptrOff,&g34[1],1u))return N33_PSPL_READ_FAILED;
    o.c2pmsg81_after=sos;o.grad_g34_fence=g34[0];
    if(!sos)return N33_PSPL_SOS_NOT_ALIVE;
    // G34 ran in this power cycle and nothing else touched its queue.
    if(!(rlc&0x80000000u) || grbm || cntl!=0x3c000000u || scratch!=mq::kScratchDone ||
       g34[0]!=mq::kFenceValue || g34[1]!=kRingStart)
        return N33_PSPL_GRAD_BAD_PRECONDITION;
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
// Submit one packet run at ring dword `at`, wait for its fence. Returns the
// state to stop with, or PLAN_OK.
inline uint32_t submit(Writer &w,const Packets &p,uint32_t &at,uint32_t fenceOff,uint32_t value,
                       uint32_t &fenceSeen) {
    const Regs &r=w.r; const Hw &h=w.h; n33_psp_load_result &o=w.o;
    const uint32_t zero=0u,end=at+p.count,wptr[2]={end,0u};
    if(end>mq::kRingBytes/4u || !vram(h,fenceOff,&zero,1u) ||
       !vram(h,mq::kRingOff+4u*at,p.dw,p.count) || !vram(h,mq::kWptrOff,wptr,2u))
        return N33_PSPL_STAGE_VERIFY_FAILED;
    o.flags|=N33_PSPL_FLAG_WROTE_VRAM;
    if(!w.put(r.q.grbm,mq::grbmSelect(mq::kMeMec,mq::kPipe,mq::kQueue)) || !w.put(r.q.poll1,1u))
        return N33_PSPL_GRAD_WRITE_FAILED;
    for(uint32_t ms=0;;++ms) {
        if(!h.vramRead(h.ctx,fenceOff,&fenceSeen,1u))return N33_PSPL_READ_FAILED;
        if(fenceSeen==value)break;
        if(ms==kFallbackMs) {
            uint32_t rptr=0;
            if(!read(h,mq::hqd(r.q,kiq::kCpHqdPqRptr),rptr))return N33_PSPL_READ_FAILED;
            if(rptr==at) {   // the poll was not taken: write the WPTR itself, once
                if(!w.put(mq::hqd(r.q,kiq::kCpHqdPqWptrLo),end) ||
                   !w.put(mq::hqd(r.q,kiq::kCpHqdPqWptrHi),0u))return N33_PSPL_GRAD_WRITE_FAILED;
                ++o.grad_fallbacks;
            }
        }
        if(ms==kTimeoutMs)break;
        h.sleep(h.ctx,1u);++o.elapsed_ms;
    }
    if(!read(h,mq::hqd(r.q,kiq::kCpHqdPqRptr),o.grad_rptr_after))return N33_PSPL_READ_FAILED;
    if(!w.put(r.q.grbm,0u))return N33_PSPL_GRAD_WRITE_FAILED;
    at=end;
    return fenceSeen==value ? N33_PSPL_PLAN_OK : N33_PSPL_GRAD_NO_FENCE;
}
// Plan calls get no write callback. The run consumes the latch before its first write.
inline n33_psp_load_result execute(const Regs &r,const mq::Layout &l,const Screen &screen,
                                  const uint8_t *co,size_t coBytes,const Hw &h,bool run,bool gate,
                                  bool &attempted) {
    auto o=blank();
    if(gate)o.flags|=N33_PSPL_FLAG_WRITE_GATE_OPEN;
    if(!r.valid){o.state=N33_PSPL_BAD_ROM;return o;}
    if(!l.windowMc || (l.windowMc&(mq::kWindowBytes-1u))){o.state=N33_PSPL_NO_WINDOW;return o;}
    const gfx11_shader::Kernel k=kernelOf(co,coBytes);
    if(k.code!=gfx11_shader::Code::Ok){o.state=N33_PSPL_BAD_FIRMWARE;return o;}
    o.flags|=N33_PSPL_FLAG_FW_PINNED;
    const bool onScreen=screen.width && screen.height && screen.pitch>=screen.width &&
        !(screen.width%8u) && !(screen.height%8u) && screen.mc && !(screen.mc&0xffu);
    const Packets a=packets(k,l.windowMc,kArgsA,kTestSize/8u,kTestSize/8u,kFenceA,kFenceValueA);
    const Packets b=packets(k,l.windowMc,kArgsB,screen.width/8u,screen.height/8u,kFenceB,kFenceValueB);
    if(!a.ok || (onScreen && !b.ok)){o.state=N33_PSPL_BAD_REQUEST;return o;}
    o.grad_screen=onScreen ? (screen.width<<16)|screen.height : 0u;
    o.state=sample(r,h,o);
    if(o.state!=N33_PSPL_PLAN_OK || !run)return o;
    if(!gate){o.state=N33_PSPL_WRITE_DISABLED;return o;}
    if(attempted){o.state=N33_PSPL_ALREADY_ATTEMPTED;return o;}
    if(!h.write || !h.sleep || !h.vramWrite || (onScreen && !h.screenRead)){o.state=N33_PSPL_BAD_REQUEST;return o;}
    attempted=true;
    Writer w{r,h,o};
    // 1. The G34 queue is still there: active, our ring, everything consumed.
    uint32_t active=0,base=0;
    if(!w.put(r.q.grbm,mq::grbmSelect(mq::kMeMec,mq::kPipe,mq::kQueue)))return w.stop(N33_PSPL_GRAD_WRITE_FAILED);
    if(!read(h,mq::hqd(r.q,kiq::kCpHqdActive),active) || !read(h,mq::hqd(r.q,kiq::kCpHqdPqBase),base) ||
       !read(h,mq::hqd(r.q,kiq::kCpHqdPqRptr),o.grad_rptr_before))return w.stop(N33_PSPL_READ_FAILED);
    if(!w.put(r.q.grbm,0u))return w.stop(N33_PSPL_GRAD_WRITE_FAILED);
    if(!(active&1u) || base!=static_cast<uint32_t>((l.windowMc+mq::kRingOff)>>8) || o.grad_rptr_before!=kRingStart)
        {o.state=N33_PSPL_GRAD_BAD_PRECONDITION;return o;}
    ++o.steps_done;
    // 2. constants_init for VMID 0 (GRBM select 0 is VMID 0).
    if(!w.put(r.shMem,kShMemDefault))return w.stop(N33_PSPL_GRAD_WRITE_FAILED);
    // 3. Shader code (256-byte aligned start, zero tail), test arguments, cleared image.
    static uint32_t code[kShaderMax/4u];
    for(uint32_t i=0;i<kShaderMax/4u;++i)code[i]=0u;
    for(uint64_t i=0;i<k.codeBytes;++i)code[i/4u]|=uint32_t(co[k.codeOffset+i])<<(8u*(i%4u));
    static uint32_t image[kImageBytes/4u];
    for(uint32_t i=0;i<kImageBytes/4u;++i)image[i]=0u;
    uint32_t argA[6];args(argA,l.windowMc+kImageOff,kTestSize,kTestSize,kTestSize,kFrameA);
    if(!vram(h,kShaderOff,code,static_cast<uint32_t>((k.codeBytes+255u)/256u*64u)) ||
       !vram(h,kArgsA,argA,6u) || !vram(h,kImageOff,image,kImageBytes/4u))
        {o.state=N33_PSPL_STAGE_VERIFY_FAILED;return o;}
    o.flags|=N33_PSPL_FLAG_WROTE_VRAM;
    ++o.steps_done;
    // 4. The 64x64 test dispatch, checked against the CPU reference.
    uint32_t at=kRingStart;
    uint32_t st=submit(w,a,at,kFenceA,kFenceValueA,o.grad_fence_a);
    if(st!=N33_PSPL_PLAN_OK)return w.stop(st);
    ++o.steps_done;
    if(!h.vramRead(h.ctx,kImageOff,image,kImageBytes/4u)){o.state=N33_PSPL_READ_FAILED;return o;}
    o.grad_first_pixel=image[kTestSize+1u];   // (1,1)
    for(uint32_t y=0;y<kTestSize;++y)for(uint32_t x=0;x<kTestSize;++x)
        if(image[y*kTestSize+x]!=gfx11_shader::gradientPixel(x,y,kTestSize,kTestSize,kFrameA))
            ++o.grad_bad_pixels;
    if(o.grad_bad_pixels){o.state=N33_PSPL_GRAD_BAD_PIXELS;return o;}
    ++o.steps_done;
    if(!onScreen){o.state=N33_PSPL_GRAD_OK;return o;}
    // 5. The whole screen, then a spread of samples read back.
    uint32_t argB[6];args(argB,screen.mc,screen.pitch,screen.width,screen.height,kFrameB);
    if(!vram(h,kArgsB,argB,6u)){o.state=N33_PSPL_STAGE_VERIFY_FAILED;return o;}
    st=submit(w,b,at,kFenceB,kFenceValueB,o.grad_fence_b);
    if(st!=N33_PSPL_PLAN_OK)return w.stop(st);
    for(uint32_t i=0;i<kScreenSamples;++i) {
        const uint32_t x=(i*977u+13u)%screen.width, y=(i*389u+5u)%screen.height;
        uint32_t px=0;
        if(!h.screenRead(h.ctx,4u*(y*screen.pitch+x),&px)){o.state=N33_PSPL_READ_FAILED;return o;}
        if(px!=gfx11_shader::gradientPixel(x,y,screen.width,screen.height,kFrameB))++o.grad_screen_bad;
    }
    ++o.steps_done;
    o.state=o.grad_screen_bad ? N33_PSPL_GRAD_BAD_PIXELS : N33_PSPL_GRAD_ON_SCREEN;
    return o;
}
} }
