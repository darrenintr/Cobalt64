// SPDX-License-Identifier: MIT
// Pure PM4 encoder for one gfx1102 compute dispatch: the shader program and
// resources from a parsed code object (Navi33Gfx11Shader.h), the kernarg
// pointer in COMPUTE_USER_DATA_0/1, DISPATCH_DIRECT, then Linux's
// end-of-pipe RELEASE_MEM fence. It produces dwords only: no queue, no
// submission, no hardware access.
//
// Sources (torvalds/linux): nvd.h (PACKET3, opcodes, RELEASE_MEM fields,
// SET_SH_REG_START), gc_11_0_0_offset.h/sh_mask.h (COMPUTE_* registers and
// COMPUTE_DISPATCH_INITIATOR fields), soc21_enum.h
// (CACHE_FLUSH_AND_INV_TS_EVENT), gfx_v9_0.c (SET_SH_REG + DISPATCH_DIRECT on
// a compute ring, plain PACKET3), gfx_v11_0.c gfx_v11_0_ring_emit_fence().
#pragma once
#include "Navi33Gfx11Shader.h"
#include <stddef.h>
#include <stdint.h>

namespace navi33 { namespace gfx11_dispatch {

constexpr bool kCanSubmit=false;
static_assert(!kCanSubmit,"the dispatch encoder only builds dwords");

constexpr uint32_t packet3(uint32_t op,uint32_t n) {
    return (3u<<30)|((op&0xffu)<<8)|((n&0x3fffu)<<16);
}
constexpr uint32_t kSetShReg=0x76u, kDispatchDirect=0x15u, kReleaseMem=0x49u;
constexpr uint32_t kShRegStart=0x2c00u;   // PACKET3_SET_SH_REG_START
constexpr uint32_t kGcBase=0x1260u;       // RX 7600 GC BASE_IDX 0 (IP Discovery)

// gc_11_0_0_offset.h, BASE_IDX 0
constexpr uint32_t kStartX=0x1ba4u, kNumThreadX=0x1ba7u, kPgmLo=0x1bacu,
    kPgmRsrc1=0x1bb2u, kResourceLimits=0x1bb5u, kTmpringSize=0x1bb8u,
    kPgmRsrc3=0x1bc8u, kUserData0=0x1be0u;
constexpr uint32_t shOffset(uint32_t reg) { return kGcBase+reg-kShRegStart; }
static_assert(shOffset(kPgmLo)==0x20cu && kGcBase+kUserData0+15u<0x3000u,
              "COMPUTE_* registers sit in the SH register range");

// COMPUTE_DISPATCH_INITIATOR (gc_11_0_0_sh_mask.h)
constexpr uint32_t kComputeShaderEn=1u<<0, kForceStartAt000=1u<<2,
    kCsW32En=1u<<15;
// RELEASE_MEM, as gfx_v11_0_ring_emit_fence() for a 32-bit fence, no IRQ
constexpr uint32_t kCacheFlushAndInvTsEvent=0x14u;
constexpr uint32_t kReleaseMemControl=(1u<<22)|(1u<<21)|(1u<<13)|(1u<<12)|
    (3u<<25)|kCacheFlushAndInvTsEvent|(5u<<8); // SEQ, GL2_WB, GLM_INV/WB, bypass
constexpr uint32_t kReleaseMemDataSel32=1u<<29;

enum class Code : uint8_t { Ok=0, BadKernel, BadAddress, BadGrid, Capacity };

struct Request {
    const gfx11_shader::Kernel *kernel=nullptr;
    uint64_t shaderMc=0u;    // copy of the kernel code, 256-byte aligned
    uint64_t kernargMc=0u;   // kernel->kernargBytes of arguments
    uint32_t groupsX=0u, groupsY=0u, groupsZ=1u;
    uint32_t threadsX=8u, threadsY=8u, threadsZ=1u;
    uint64_t fenceMc=0u; uint32_t fenceValue=0u;
};

constexpr size_t kMaxDwords=96u;
struct Stream { Code code=Code::BadKernel; uint32_t dwords[kMaxDwords]{}; size_t count=0u; };

inline bool push(Stream &s,uint32_t v) {
    if(s.count>=kMaxDwords) { s.code=Code::Capacity; return false; }
    s.dwords[s.count++]=v; return true;
}
inline bool setSh(Stream &s,uint32_t reg,const uint32_t *values,uint32_t n) {
    if(!push(s,packet3(kSetShReg,n)) || !push(s,shOffset(reg))) return false;
    for(uint32_t i=0;i<n;++i) if(!push(s,values[i])) return false;
    return true;
}

inline Stream encode(const Request &r) {
    Stream s{};
    const gfx11_shader::Kernel *k=r.kernel;
    if(!k || k->code!=gfx11_shader::Code::Ok || k->userSgprs<2u || k->userSgprs>16u)
        return s;
    if(!r.shaderMc || (r.shaderMc&0xffu) || (r.shaderMc>>48) ||
       !r.kernargMc || (r.kernargMc&3u) || !r.fenceMc || (r.fenceMc&3u)) {
        s.code=Code::BadAddress; return s;
    }
    const uint64_t threads=static_cast<uint64_t>(r.threadsX)*r.threadsY*r.threadsZ;
    if(!r.groupsX || !r.groupsY || !r.groupsZ || !threads || threads>1024u ||
       r.threadsX>1023u || r.threadsY>1023u || r.threadsZ>1023u ||
       (r.threadsY>1u && !(k->tidigComponents>=1u)) || (r.groupsY>1u && !k->tgidY) ||
       (r.groupsZ>1u && !k->tgidZ) || (r.groupsX>1u && !k->tgidX)) {
        s.code=Code::BadGrid; return s;
    }
    const uint32_t pgm[2]={static_cast<uint32_t>(r.shaderMc>>8),
                           static_cast<uint32_t>(r.shaderMc>>40)&0xffu};
    const uint32_t rsrc[2]={k->rsrc1,k->rsrc2};
    const uint32_t zero[3]={0u,0u,0u};
    const uint32_t nthreads[3]={r.threadsX,r.threadsY,r.threadsZ};
    uint32_t user[16]{};
    user[0]=static_cast<uint32_t>(r.kernargMc);
    user[1]=static_cast<uint32_t>(r.kernargMc>>32);
    const bool ok=setSh(s,kPgmLo,pgm,2u) && setSh(s,kPgmRsrc1,rsrc,2u) &&
        setSh(s,kPgmRsrc3,&k->rsrc3,1u) && setSh(s,kResourceLimits,zero,1u) &&
        setSh(s,kTmpringSize,zero,1u) && setSh(s,kStartX,zero,3u) &&
        setSh(s,kNumThreadX,nthreads,3u) && setSh(s,kUserData0,user,k->userSgprs) &&
        push(s,packet3(kDispatchDirect,3u)) && push(s,r.groupsX) &&
        push(s,r.groupsY) && push(s,r.groupsZ) &&
        push(s,kComputeShaderEn|kForceStartAt000|kCsW32En) &&
        push(s,packet3(kReleaseMem,6u)) && push(s,kReleaseMemControl) &&
        push(s,kReleaseMemDataSel32) && push(s,static_cast<uint32_t>(r.fenceMc)) &&
        push(s,static_cast<uint32_t>(r.fenceMc>>32)) && push(s,r.fenceValue) &&
        push(s,0u) && push(s,0u);
    if(ok) s.code=Code::Ok;
    return s;
}

// The kernel arguments of shaders/gradient.cl, little-endian, 24 bytes.
inline void gradientKernargs(uint8_t out[24],uint64_t pixelsMc,uint32_t pitchPixels,
                             uint32_t width,uint32_t height,uint32_t frame) {
    const uint32_t w[6]={static_cast<uint32_t>(pixelsMc),static_cast<uint32_t>(pixelsMc>>32),
                         pitchPixels,width,height,frame};
    for(int i=0;i<6;++i)
        for(int b=0;b<4;++b) out[4*i+b]=static_cast<uint8_t>(w[i]>>(8*b));
}

} } // namespace navi33::gfx11_dispatch
