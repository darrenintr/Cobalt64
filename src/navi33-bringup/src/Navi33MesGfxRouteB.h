// SPDX-License-Identifier: MIT
// RX 7600 / GC 11.0.2: offline-only MES v11 legacy GFX queue protocol.
// Source of truth: Linux mes_v11_api_def.h, mes_v11_0.c and gfx_v11_0.c.
// This header performs NO MMIO/doorbell/GPU operations. Do not install it
// into a live kext until the separate MES scheduler/ownership path is proven.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace navi33 { namespace mes_gfx_routeb {

constexpr uint32_t kFrameDwords=64, kFrameBytes=256;
constexpr uint32_t kMesSchedType=1, kSetResources=0, kAddQueue=2;
constexpr uint32_t kQueryStatus=11, kSetResources1=19;
constexpr uint32_t kGfxType=0, kGfxDoorbellDword=0x116;
constexpr uint32_t kGfxDoorbellByte=kGfxDoorbellDword*4;
constexpr uint32_t kMinLegacyMesVersion=0x47;
constexpr uint32_t kResources1MesVersion=0x52;
constexpr uint32_t kVersionMask=0xfff;
constexpr uint32_t kMaxMcBits=48;
constexpr uint64_t kMaxMc=(1ull<<kMaxMcBits)-1ull;
constexpr uint32_t kLegacyKqBit=1u<<12;
constexpr uint32_t kResourcesFlags=(1u<<0)|(1u<<1)|(1u<<2)|(1u<<6)|(1u<<10);
// gc11 mes_v11_api_def.h: 4-byte aligned API packet layout on x86_64.
// We use named structures and offsetof: no guessed array indexes.
#pragma pack(push,4)
struct Status { uint64_t addr; uint64_t value; };
struct Header { uint32_t bits; };
struct AddQueueFields {
    Header header;
    uint32_t process_id;
    uint64_t page_table_base_addr, process_va_start, process_va_end;
    uint64_t process_quantum, process_context_addr, gang_quantum, gang_context_addr;
    uint32_t inprocess_priority, global_priority, doorbell_offset;
    uint64_t mqd_addr, wptr_addr, h_context, h_queue;
    uint32_t queue_type, gds_base, gds_size, gws_base, gws_size, oa_mask;
    uint64_t trap_handler_addr;
    uint32_t vm_context_cntl, flags;
    Status status;
    uint64_t tma_addr;
    uint32_t sch_id;
    uint64_t timestamp;
    uint32_t process_context_array_index, gang_context_array_index;
    uint32_t pipe_id, queue_id, alignment_mode_setting, full_sh_mem_config_data;
    uint64_t unmap_flag_addr;
};
struct ResourcesFields {
    Header header;
    uint32_t vmid_mmhub, vmid_gfxhub, gds_size, paging_vmid;
    uint32_t compute_hqd[8], gfx_hqd[2], sdma_hqd[2], aggregated_db[5];
    uint64_t sched_context, query_status_fence;
    uint32_t gc_base[8], mmhub_base[8], osssys_base[8];
    Status status;
    uint32_t flags, oversubscription_timer;
    uint64_t doorbell_info, event_history, timestamp;
    uint32_t os_tdr_timeout;
};
struct Resources1Fields {
    Header header;
    Status status;
    uint64_t timestamp;
    uint32_t flags;
    uint64_t info_context;
    uint32_t info_context_bytes;
    uint64_t reserved;
    uint64_t cleaner_fence;
};
struct QueryFields {
    Header header;
    uint32_t subopcode;
    Status status;
    uint64_t timestamp;
    uint32_t data[20];
};
#pragma pack(pop)

static_assert(sizeof(Status)==16 && sizeof(Header)==4,"MES ABI status/header");
static_assert(sizeof(AddQueueFields)<=kFrameBytes,"MES ADD_QUEUE fields fit");
static_assert(sizeof(ResourcesFields)<=kFrameBytes,"MES SET_HW_RESOURCES fields fit");
static_assert(sizeof(Resources1Fields)<=kFrameBytes,"MES SET_HW_RESOURCES_1 fields fit");
static_assert(sizeof(QueryFields)<=kFrameBytes,"MES QUERY fields fit");
static_assert(offsetof(AddQueueFields, flags)==136,"MES legacy flag offset");
static_assert(offsetof(AddQueueFields, status)==140,"MES ADD_QUEUE status offset");
static_assert(offsetof(AddQueueFields, pipe_id)==188,"MES ADD_QUEUE pipe offset");

struct Packet { uint32_t dw[kFrameDwords]{}; };
enum class Code : uint8_t { Ready, BadFirmware, BadOwnership, BadAddress, BadRing, BadScheduler, BadResources };
struct Validation { Code code=Code::BadScheduler; bool ok() const { return code==Code::Ready; } };
inline uint32_t header(uint32_t opcode) {
    return kMesSchedType | (opcode<<4u) | (kFrameDwords<<12u);
}
inline void put(Packet &out,const void *data,size_t size) {
    memset(out.dw,0,sizeof(out.dw));
    if(size<=sizeof(out.dw))memcpy(out.dw,data,size);
}
inline void copy(Packet &out,const void *data,size_t size) { put(out,data,size); }

struct Owner {
    bool exactNavi33=false, mesFirmwareLive=false, schedulerQueueLive=false;
    bool schedulerResourcesAck=false, schedulerResources1Ack=false;
    bool kiqAndGfxUnowned=true, g42Disabled=false, freshEpoch=false;
    uint32_t mesVersion=0;
};
inline Validation readiness(const Owner &s) {
    if(!s.exactNavi33 || !s.mesFirmwareLive || !s.freshEpoch)
        return {Code::BadFirmware};
    if(!s.kiqAndGfxUnowned || !s.g42Disabled)
        return {Code::BadOwnership};
    if((s.mesVersion&kVersionMask)<kMinLegacyMesVersion || !s.schedulerQueueLive)
        return {Code::BadScheduler};
    if(!s.schedulerResourcesAck ||
       ((s.mesVersion&kVersionMask)>=kResources1MesVersion && !s.schedulerResources1Ack))
        return {Code::BadResources};
    return {Code::Ready};
}
inline bool validMc(uint64_t addr,uint64_t align) {
    return addr && addr<=kMaxMc && (addr&(align-1))==0;
}
struct AddInput {
    Owner owner{};
    uint64_t mqd=0,wptr=0,completionFence=0;
    uint64_t completionValue=0;
    uint32_t pipe=0,queue=0,doorbellDword=kGfxDoorbellDword;
};
struct Build { Packet packet{}; Validation validation{}; size_t statusOffset=0; };
inline Build addGfx(const AddInput &in) {
    Build out{};out.validation=readiness(in.owner);
    if(!out.validation.ok())return out;
    if(!validMc(in.mqd,4)||!validMc(in.wptr,8)||
       !validMc(in.completionFence,8)||!in.completionValue) {
        out.validation={Code::BadAddress};return out;
    }
    if(in.pipe!=0||in.queue!=0||in.doorbellDword!=kGfxDoorbellDword) {
        out.validation={Code::BadRing};return out;
    }
    AddQueueFields v{};
    v.header.bits=header(kAddQueue);
    v.doorbell_offset=in.doorbellDword;
    v.mqd_addr=in.mqd;v.wptr_addr=in.wptr;
    v.queue_type=kGfxType;v.flags=kLegacyKqBit;
    v.pipe_id=in.pipe;v.queue_id=in.queue;
    v.status.addr=in.completionFence;v.status.value=in.completionValue;
    copy(out.packet,&v,sizeof(v));
    out.statusOffset=offsetof(AddQueueFields,status);
    out.validation={Code::Ready};
    return out;
}
struct ResourceInput {
    uint32_t vmidGfx=0,vmidMmhub=0,gdsSize=0;
    uint32_t computeHqd[8]{},gfxHqd[2]{},sdmaHqd[2]{},aggDoorbells[5]{};
    uint32_t gcBase[8]{},mmhubBase[8]{},osssysBase[8]{};
    uint64_t schedulerContext=0,queryFence=0,apiFence=0,apiValue=0;
};
inline Build resources(const ResourceInput &in) {
    Build out{};
    if(!validMc(in.schedulerContext,8)||!validMc(in.queryFence,8)||
       !validMc(in.apiFence,8)||!in.apiValue ||
       !(in.gfxHqd[0]&1u)) {out.validation={Code::BadResources};return out;}
    ResourcesFields v{};v.header.bits=header(kSetResources);
    v.vmid_gfxhub=in.vmidGfx;v.vmid_mmhub=in.vmidMmhub;v.gds_size=in.gdsSize;
    for(unsigned i=0;i<8;i++) {
        v.compute_hqd[i]=in.computeHqd[i];v.gc_base[i]=in.gcBase[i];
        v.mmhub_base[i]=in.mmhubBase[i];v.osssys_base[i]=in.osssysBase[i];
    }
    for(unsigned i=0;i<2;i++){v.gfx_hqd[i]=in.gfxHqd[i];v.sdma_hqd[i]=in.sdmaHqd[i];}
    for(unsigned i=0;i<5;i++)v.aggregated_db[i]=in.aggDoorbells[i];
    v.sched_context=in.schedulerContext;v.query_status_fence=in.queryFence;
    v.flags=kResourcesFlags;v.status={in.apiFence,in.apiValue};
    copy(out.packet,&v,sizeof(v));out.statusOffset=offsetof(ResourcesFields,status);
    out.validation={Code::Ready};return out;
}
struct Resources1Input { uint64_t cleanerFence=0,apiFence=0,apiValue=0; };
inline Build resources1(const Resources1Input &in,uint32_t mesVersion) {
    Build out{};
    if((mesVersion&kVersionMask)<kResources1MesVersion ||
       !validMc(in.cleanerFence,8)||!validMc(in.apiFence,8)||!in.apiValue) {
        out.validation={Code::BadResources};return out;
    }
    Resources1Fields v{};v.header.bits=header(kSetResources1);
    v.flags=1u;v.cleaner_fence=in.cleanerFence;
    v.status={in.apiFence,in.apiValue};
    copy(out.packet,&v,sizeof(v));
    out.statusOffset=offsetof(Resources1Fields,status);
    out.validation={Code::Ready};return out;
}
inline Packet query(uint64_t fence,uint64_t value) {
    Packet out{}; if(!validMc(fence,8)||!value)return out;
    QueryFields v{};v.header.bits=header(kQueryStatus);
    v.status={fence,value};
    copy(out,&v,sizeof(v));return out;
}
// Host-only acknowledgement. Real hardware must poll both the scheduler
// sequence fence and the API completion fence, as mes_v11_0.c does.
struct Completion { uint64_t schedulerObserved=0,apiObserved=0; };
inline bool completed(Completion c,uint64_t schedulerExpected,uint64_t apiExpected) {
    return schedulerExpected && apiExpected &&
        c.schedulerObserved==schedulerExpected && c.apiObserved==apiExpected;
}
} } // namespace navi33::mes_gfx_routeb
