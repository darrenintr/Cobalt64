// SPDX-License-Identifier: MIT
// MP1 13.0.7 three-message experiment. No hardware I/O except callbacks.
// Sources and the deliberate omissions from Linux: SMU_ENABLE_DESIGN.md.
#pragma once
#include "Navi33HardwarePlan.h"
#include "Navi33NativeABI.h"
namespace navi33 { namespace smu13 {
constexpr uint32_t kMsg=0x282u,kArg=0x292u,kResp=0x29au; // mp_13_0_2_offset.h, MP1 seg0
// smu_v13_0_7_init_allowed_features: default pp_feature flags, soc21 Navi33 PG.
constexpr uint64_t kAllowed=0x0001a3a471fe7fffull;
constexpr uint32_t kMessages[3]={0x5u,0x4u,0x6u}; // HIGH, LOW, EnableAll
constexpr uint32_t kParams[3]={uint32_t(kAllowed>>32u),uint32_t(kAllowed),0u};
constexpr uint32_t kTimeoutMs=1000u;
struct Regs { uint64_t msg=0,arg=0,resp=0,sos=0,scratch0=0,scratch1=0,core=0,reset=0,rlc=0; bool valid=false; };
inline Regs resolve(const IPDiscovery &d,uint64_t bytes) {
    Regs r{};
    uint64_t *dst[]={&r.msg,&r.arg,&r.resp,&r.sos,&r.scratch0,&r.scratch1,&r.core,&r.reset,&r.rlc};
    const Engine eng[]={Engine::SMU13,Engine::SMU13,Engine::SMU13,Engine::PSP13,
                        Engine::GFX11,Engine::GFX11,Engine::GFX11,Engine::GFX11,Engine::GFX11};
    const uint32_t dw[]={kMsg,kArg,kResp,0x91u,0x4068u,0x4069u,0x40b7u,0x40bcu,0x4e82u};
    for(unsigned i=0;i<9u;++i) {
        const auto a=resolveDword(d,eng[i],i>=4u ? 1u : 0u,dw[i],bytes);
        if(a.code!=AddressCode::Pass)return r;
        *dst[i]=a.bar5ByteOffset;
    }
    r.valid=true;return r;
}
struct Hw {
    void *ctx=nullptr;
    bool (*read)(void *,uint64_t,uint32_t *)=nullptr;
    bool (*write)(void *,uint64_t,uint32_t)=nullptr;
    void (*sleep)(void *,uint32_t)=nullptr;
    void (*trace)(void *,uint32_t,uint32_t,uint32_t)=nullptr; // msg, param, reply (0 before send)
};
inline bool read(const Hw &h,uint64_t off,uint32_t &v) {
    return h.read && h.read(h.ctx,off,&v) && v!=0xffffffffu;
}
inline n33_psp_load_result blank() {
    n33_psp_load_result o{};o.abi_version=N33_ABI_VERSION;o.size=sizeof(o);
    o.smu_mask_low=uint32_t(kAllowed);o.smu_mask_high=uint32_t(kAllowed>>32u);
    o.smu_marker=N33_SMU_MARKER;return o;
}
inline uint32_t sample(const Regs &r,const Hw &h,n33_psp_load_result &o) {
    if(!read(h,r.sos,o.c2pmsg81_after) || !read(h,r.msg,o.smu_message_before) ||
       !read(h,r.arg,o.smu_argument_before) || !read(h,r.resp,o.smu_response_before) ||
       !read(h,r.scratch0,o.smu_scratch0) || !read(h,r.scratch1,o.smu_scratch1) ||
       !read(h,r.core,o.smu_core_before) || !read(h,r.reset,o.smu_reset_before) ||
       !read(h,r.rlc,o.smu_rlc_before))return N33_PSPL_READ_FAILED;
    o.smu_core_after=o.smu_core_before;o.smu_reset_after=o.smu_reset_before;
    o.smu_rlc_after=o.smu_rlc_before;
    if(!o.c2pmsg81_after)return N33_PSPL_SOS_NOT_ALIVE;
    if(o.smu_response_before!=1u)return N33_PSPL_SMU_NOT_READY;
    if(o.smu_scratch0!=0x0b214700u || o.smu_scratch1!=0xbabefaceu ||
       o.smu_core_before!=2u || o.smu_reset_before!=0x10u || o.smu_rlc_before!=0u)
        return N33_PSPL_SMU_BAD_PRECONDITION;
    return N33_PSPL_PLAN_OK;
}
// Plan calls never receive a write callback. Run rechecks the live state under
// the provider lock before consuming the one-shot latch, including on failure.
inline n33_psp_load_result execute(const Regs &r,const Hw &h,bool run,bool gate,
                                  bool &attempted) {
    auto o=blank();
    if(gate)o.flags|=N33_PSPL_FLAG_WRITE_GATE_OPEN;
    if(!r.valid){o.state=N33_PSPL_BAD_ROM;return o;}
    o.state=sample(r,h,o);
    if(o.state!=N33_PSPL_PLAN_OK)return o;
    if(!run)return o;
    if(!gate){o.state=N33_PSPL_WRITE_DISABLED;return o;}
    if(attempted){o.state=N33_PSPL_ALREADY_ATTEMPTED;return o;}
    if(!h.write || !h.sleep){o.state=N33_PSPL_BAD_REQUEST;return o;}
    attempted=true;
    for(unsigned i=0;i<3u;++i) {
        o.smu_last_message=kMessages[i];o.smu_last_argument=kParams[i];
        // Never clobber an in-flight or failed mailbox, even between messages.
        uint32_t ready=0;
        if(!read(h,r.resp,ready)){o.state=N33_PSPL_READ_FAILED;return o;}
        if(ready!=1u){o.state=N33_PSPL_SMU_NOT_READY;return o;}
        if(h.trace)h.trace(h.ctx,kMessages[i],kParams[i],0u);
        const uint64_t offsets[]={r.resp,r.arg,r.msg};
        const uint32_t values[]={0u,kParams[i],kMessages[i]};
        for(unsigned j=0;j<3u;++j) {
            if(!h.write(h.ctx,offsets[j],values[j])) {o.state=N33_PSPL_SMU_WRITE_FAILED;return o;}
            ++o.register_writes;o.flags|=N33_PSPL_FLAG_WROTE_REGISTERS;
        }
        uint32_t response=0;
        for(uint32_t ms=0;;++ms) {
            if(!read(h,r.resp,response)){o.state=N33_PSPL_READ_FAILED;return o;}
            o.smu_responses[i]=response;
            if(response)break;
            if(ms==kTimeoutMs){o.state=N33_PSPL_SMU_TIMEOUT;return o;}
            h.sleep(h.ctx,1u);++o.elapsed_ms;
        }
        if(h.trace)h.trace(h.ctx,kMessages[i],kParams[i],response);
        if(response!=1u){o.state=N33_PSPL_SMU_REJECTED;return o;}
        ++o.steps_done;
    }
    // Acknowledgement is distinct from GC boot completion. Observe for 1 s;
    // never send extra messages, retry, reset the GPU, or undo partial state.
    for(uint32_t ms=0;;++ms) {
        if(!read(h,r.core,o.smu_core_after) || !read(h,r.reset,o.smu_reset_after) ||
           !read(h,r.rlc,o.smu_rlc_after)){o.state=N33_PSPL_READ_FAILED;return o;}
        if(o.smu_rlc_after&0x80000000u){o.state=N33_PSPL_SMU_GC_READY;return o;}
        if(ms==kTimeoutMs){o.state=N33_PSPL_SMU_ACKED;return o;}
        h.sleep(h.ctx,1u);++o.elapsed_ms;
    }
}
} }
