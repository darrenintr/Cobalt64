// SPDX-License-Identifier: MIT
// The N48N v1.0 front end over the per-client GPU contexts (Phase B4): what RADV's Darwin
// backend calls (mesa-patches/0002), translated onto client_gpu::Device. Pure: no IOKit,
// no sleeping; the IOKit side owns the session, the gate, CPU mappings and blocking waits.
// What the RX 7600 can do today decides the answers: buffers are CPU-visible VRAM (a GTT
// request is served from BAR0 VRAM and reported as VRAM), IBs run on the client's compute
// queue (ip_type COMPUTE), the graphics ring does not exist yet (ip_type GFX: Unsupported),
// ReadRegs answers GB_ADDR_CONFIG only.
#pragma once
#include "Navi33ClientGpu.h"
#include "Navi33N48nAbi.h"
namespace navi33 { namespace n48n_front {
namespace cg=client_gpu;
enum class R : uint8_t { Ok=0, BadArgument, Unsupported, NoMemory, NotReady, Busy, Timeout, NotFound, Aborted, NoResources };
inline R from(cg::Code c) {
    switch(c) {
    case cg::Code::Ok: return R::Ok;
    case cg::Code::BadArgs: return R::BadArgument;
    case cg::Code::NotReady: return R::NotReady;
    case cg::Code::NoSlot: return R::NoResources;
    case cg::Code::NoMemory: return R::NoMemory;
    case cg::Code::Busy: return R::Busy;
    case cg::Code::Timeout: return R::Timeout;
    case cg::Code::NotFound: return R::NotFound;
    default: return R::Aborted;          // Faulted, Quarantined, HwFailed, Hub
    }
}
constexpr uint32_t kGbAddrConfigDword=0x263eu;          // GC seg0 0x1260 + regGB_ADDR_CONFIG 0x13de
constexpr uint32_t kGcVersion=(11u<<16)|(0u<<8)|2u;
constexpr uint32_t kMaxCtx=64u;
constexpr uint64_t kAllowedDomainFlags=N48N_GEM_CPU_ACCESS_REQUIRED|N48N_GEM_NO_CPU_ACCESS|N48N_GEM_CPU_GTT_USWC|
    N48N_GEM_VRAM_CLEARED|N48N_GEM_VRAM_CONTIGUOUS|N48N_GEM_VM_ALWAYS_VALID|N48N_GEM_EXPLICIT_SYNC|
    N48N_GEM_DISCARDABLE|N48N_GEM_UNCACHED|N48N_GEM_GFX12_DCC|N48N_GEM_VIRTIO_SHARED;
constexpr uint32_t kAllowedVmFlags=N48N_VM_DELAY_UPDATE|N48N_VM_PAGE_READABLE|N48N_VM_PAGE_WRITEABLE|
    N48N_VM_PAGE_EXECUTABLE|N48N_VM_MTYPE_MASK|N48N_VM_PAGE_NOALLOC;
constexpr uint32_t kAllowedIbFlags=N48N_IB_FLAG_PREAMBLE|N48N_IB_FLAG_PREEMPT|N48N_IB_FLAG_TC_WB_NOT_INVALIDATE;
// One N48N connection: its device slot (opened by HELLO) and its context ids.
struct Session { uint32_t slot=0; bool open=false; uint64_t ctxIds=0; };   // bit i = id i + 1

inline void info(cg::Device &d,const Session &s,const vram_heap::Heap &heap,uint32_t gbAddrConfig,uint32_t build,
                 n48n_info &o) {
    o=n48n_info{};
    o.abi_version=N48N_ABI_VERSION;o.kext_build=build;o.gb_addr_config=gbAddrConfig;o.gc_version=kGcVersion;
    o.ring_size_dw=cg::kRingDwords;o.max_ibs=cg::kMaxIbs;
    const cg::Client *c=s.open ? d.client(s.slot) : nullptr;
    o.vmid=c ? c->vmid : 0u;
    if(c && c->state!=cg::State::Open)o.flags|=N48N_INFO_HUNG;
    // Only CPU-visible VRAM is handed out today: the BAR0 pool, minus what the kernel pinned.
    // The boot console and the bring-up windows (vm_alloc::kTagBoot / kTagStages).
    const uint64_t pinned=heap.usedBytes(0x424f4f54u)+heap.usedBytes(0x53544753u);
    o.vram_vis_total=heap.visible()>pinned ? heap.visible()-pinned : 0u;
    uint64_t usedVis=heap.usedBytes(cg::kTagClient)+heap.usedBytes(gmc11_vm::kTagTables);
    o.vram_vis_free=o.vram_vis_total>usedVis ? o.vram_vis_total-usedVis : 0u;
    o.gtt_cap=o.vram_vis_total/4u;o.gtt_max_bo=cg::kMaxBoBytes;     // served from the same BAR0 pool
    uint64_t em=0,re=0;
    if(s.open && d.sequences(s.slot,em,re)){o.seq_emitted=em;o.seq_retired=re;}
    o.va_low_first=cg::kUserVaMin;o.va_low_last=cg::kUserVaMax-1u;
    o.max_bos=cg::kMaxBos;o.fence_slots=1u;
}
inline R boCreate(cg::Device &d,const Session &s,const n48n_gem_create_in &in,uint64_t out[4]) {
    if(!s.open)return R::NotReady;
    const uint64_t dom=in.domains&(N48N_GEM_DOMAIN_VRAM|N48N_GEM_DOMAIN_GTT);
    if(!dom || (in.domains&~(N48N_GEM_DOMAIN_VRAM|N48N_GEM_DOMAIN_GTT)) || (in.domain_flags&~kAllowedDomainFlags) ||
       !in.bo_size || in.bo_size>cg::kMaxBoBytes || (in.alignment&(in.alignment-1u)) || in.alignment>(2u<<20))
        return R::BadArgument;
    uint32_t h=0;uint64_t vram=0,bytes=0;
    const R r=from(d.boCreate(s.slot,in.bo_size,h,vram,in.alignment ? in.alignment : 4096u));
    if(r!=R::Ok)return r;
    d.boRange(s.slot,h,vram,bytes);
    out[0]=h;out[1]=bytes;out[2]=N48N_GEM_DOMAIN_VRAM;out[3]=N48N_PLACED_CPU_MAPPABLE|N48N_PLACED_ZEROED;
    return R::Ok;
}
inline R gemVa(cg::Device &d,const Session &s,const n48n_gem_va &in) {
    if(!s.open)return R::NotReady;
    if(in._pad || in.vm_timeline_point || in.vm_timeline_syncobj_out || in.num_syncobj_handles ||
       in.input_fence_syncobj_handles)return R::BadArgument;
    if(in.operation!=N48N_VA_OP_MAP && in.operation!=N48N_VA_OP_UNMAP)return R::Unsupported;
    if(in.flags&N48N_VM_PAGE_PRT)return R::Unsupported;
    if(in.flags&~kAllowedVmFlags)return R::BadArgument;
    uint64_t va=0,bytes=0;
    if(!d.boMapping(s.slot,in.handle,va,bytes))return R::NotFound;
    // Whole buffers only (what RADV does): offset 0, the buffer's page-rounded size.
    if(in.offset_in_bo || (in.map_size && (in.map_size+4095u)/4096u*4096u!=bytes))return R::BadArgument;
    if(in.operation==N48N_VA_OP_UNMAP)
        return va==in.va_address ? from(d.vaUnmap(s.slot,in.handle)) : R::BadArgument;
    return from(d.vaMap(s.slot,in.handle,in.va_address,in.flags&N48N_VM_PAGE_WRITEABLE,in.flags&N48N_VM_PAGE_EXECUTABLE));
}
inline R ctx(cg::Device &d,Session &s,const n48n_ctx &in,n48n_ctx &out) {
    if(!s.open)return R::NotReady;
    out=n48n_ctx{};
    if(in.op==N48N_CTX_OP_ALLOC) {
        if(in.flags || in.ctx_id)return R::BadArgument;
        for(uint32_t i=0;i<kMaxCtx;++i)if(!(s.ctxIds&(1ull<<i))){s.ctxIds|=1ull<<i;out.op=i+1u;return R::Ok;}
        return R::NoResources;
    }
    if(!in.ctx_id || in.ctx_id>kMaxCtx || !(s.ctxIds&(1ull<<(in.ctx_id-1u))))return R::BadArgument;
    if(in.op==N48N_CTX_OP_FREE){s.ctxIds&=~(1ull<<(in.ctx_id-1u));return R::Ok;}
    if(in.op==N48N_CTX_OP_QUERY_STATE2) {
        const cg::Client *c=d.client(s.slot);
        const uint64_t f=(c && c->state!=cg::State::Open) ? (N48N_CTX_QUERY2_RESET|N48N_CTX_QUERY2_GUILTY) : 0u;
        out.op=static_cast<uint32_t>(f);out.flags=static_cast<uint32_t>(f>>32);
        return R::Ok;
    }
    return R::Unsupported;
}
inline R submit(cg::Device &d,const Session &s,const n48n_cs_in &h,const n48n_cs_ib *ibs,uint64_t &seq) {
    if(!s.open)return R::NotReady;
    if(h.abi!=N48N_ABI_VERSION || h.reserved || (h.flags&~N48N_CS_HAS_FENCE) || !h.num_ibs || h.num_ibs>cg::kMaxIbs ||
       !h.ctx_id || h.ctx_id>kMaxCtx || !(s.ctxIds&(1ull<<(h.ctx_id-1u))))return R::BadArgument;
    cg::Device::Ib list[cg::kMaxIbs];
    const uint32_t ip=ibs[0].ip_type;                     // one submission, one ring (as amdgpu's CS)
    for(uint32_t i=0;i<h.num_ibs;++i) {
        const n48n_cs_ib &b=ibs[i];
        if(b._pad || (b.flags&~kAllowedIbFlags) || b.ip_instance || b.ring || !b.ib_bytes || (b.ib_bytes&3u) ||
           b.ip_type!=ip)
            return R::BadArgument;
        if(b.ip_type!=N48N_HW_IP_COMPUTE && b.ip_type!=N48N_HW_IP_GFX)return R::BadArgument;
        list[i]={b.va_start,b.ib_bytes/4u};
    }
    if(ip==N48N_HW_IP_GFX && !d.gfxReady())return R::Unsupported;   // the gfx pipe did not come up (G40)
    uint64_t fence=0;
    if(h.flags&N48N_CS_HAS_FENCE) {
        uint64_t va=0,bytes=0;
        if(!d.boMapping(s.slot,h.fence_handle,va,bytes) || !va || (h.fence_offset&7u) ||
           uint64_t(h.fence_offset)+8u>bytes)return R::BadArgument;
        fence=va+h.fence_offset;
    } else if(h.fence_handle || h.fence_offset) return R::BadArgument;
    return from(ip==N48N_HW_IP_GFX ? d.submitGfx(s.slot,list,h.num_ibs,seq,fence) : d.submit(s.slot,list,h.num_ibs,seq,fence));
}
// One non-blocking look; the IOKit side loops with sleeps up to the timeout.
inline R waitOnce(cg::Device &d,const Session &s,uint64_t seq,uint64_t out[3]) {
    if(!s.open)return R::NotReady;
    uint64_t em=0,re=0;
    if(!d.sequences(s.slot,em,re))return R::NotReady;
    if(seq==N48N_SEQ_LAST)seq=em;
    if(seq>em)return R::BadArgument;
    out[0]=re<seq;out[1]=re;out[2]=em;
    const cg::Client *c=d.client(s.slot);
    if(out[0] && c && c->state!=cg::State::Open)return R::Aborted;   // will never retire
    return R::Ok;
}
inline R readRegs(uint32_t dword,uint32_t count,uint32_t gbAddrConfig,uint32_t *out) {
    if(count!=1u || dword!=kGbAddrConfigDword)return R::Unsupported;   // GB_ADDR_CONFIG only
    out[0]=gbAddrConfig;return R::Ok;
}
} }
