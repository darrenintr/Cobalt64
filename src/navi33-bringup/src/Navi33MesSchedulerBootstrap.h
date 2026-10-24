// SPDX-License-Identifier: MIT
// Navi33 MES Route B: independent preflight for the MES KIQ -> Scheduler
// bootstrap (Linux mes_v11_0_queue_init and mes_v11_0_kiq_enable_queue).
// Reuses Cobalt64's tested GC11 MQD builders. No MMIO/VRAM/doorbell I/O.
#pragma once

#include "Navi33GfxMesMqd.h"
#include "Navi33MesSchedulerTransport.h"
#include <stdint.h>

namespace navi33 { namespace mes_routeb_bootstrap {
namespace mes = gfx11_mes;
namespace mqd = gfx11_mes_mqd;
namespace t = mes_routeb_transport;
namespace policy = mes_gfx_routeb;

enum class Code : uint8_t {
    Ready, WrongSilicon, NotFresh, OwnershipConflict, FirmwareNotReady,
    UnsupportedMes, BadQueue, BadAddress, MemoryOverlap, InternalError
};
struct Buffer {
    uint64_t address = 0u;
    uint32_t bytes = 0u;
};
constexpr uint32_t kMesMqdBytes=2048u, kMesEopBytes=2048u, kWbSlotBytes=8u;

struct Input {
    policy::Owner owner{};
    mes::QueueInput kiq{};       // MES pipe 1, doorbell dword 0x18
    mes::QueueInput scheduler{}; // MES pipe 0, doorbell dword 0x16
};
struct Result {
    Code code = Code::FirmwareNotReady;
    mes::Plan kiqRegisterPlan{};
    mes::Plan schedulerRegisterPlan{};
    mqd::Result kiqMqd{};
    mqd::Result schedulerMqd{};
    mes::MapPacket mapScheduler{}; // KIQ MAP_QUEUES (me=2, eng_sel=5)
    bool valid() const { return code==Code::Ready; }
};

inline bool validBuffer(Buffer b,uint32_t alignment) {
    return b.bytes && b.address && !(b.address & (alignment-1u)) &&
           b.address<=policy::kMaxMc &&
           (uint64_t(b.bytes)-1u)<=policy::kMaxMc-b.address;
}
inline bool overlaps(Buffer a,Buffer b) {
    return a.address < b.address+b.bytes && b.address < a.address+a.bytes;
}
inline bool validateBuffers(const mes::QueueInput &k,const mes::QueueInput &s,
                            Code &code) {
    const mes::QueueInput a[2]={k,s};
    Buffer b[10]{};
    for(unsigned i=0;i<2;i++) {
        const mes::gfx11_kiq::KiqQueue &q=a[i].q;
        const unsigned o=i*5u;
        b[o+0]={q.mqdGpuVa,kMesMqdBytes};
        b[o+1]={q.pqGpuVa,q.queueBytes};
        b[o+2]={q.eopGpuVa,kMesEopBytes};
        b[o+3]={q.rptrGpuVa,kWbSlotBytes};
        b[o+4]={q.wptrGpuVa,kWbSlotBytes};
        const uint32_t align[5]={4096u,256u,256u,8u,8u};
        for(unsigned j=0;j<5;j++) {
            if(!validBuffer(b[o+j],align[j])) {
                code=Code::BadAddress;
                return false;
            }
        }
    }
    for(unsigned i=0;i<10;i++)
        for(unsigned j=i+1;j<10;j++)
            if(overlaps(b[i],b[j])) {
                code=Code::MemoryOverlap;
                return false;
            }
    return true;
}

inline Result prepare(const Input &in) {
    Result out{};
    const auto &owner=in.owner;
    if(!owner.exactNavi33) {out.code=Code::WrongSilicon;return out;}
    if(!owner.freshEpoch) {out.code=Code::NotFresh;return out;}
    // Direct G34/G42 KIQ and MES Scheduler may not own the same hardware.
    if(!owner.kiqAndGfxUnowned || !owner.g42Disabled ||
       owner.schedulerQueueLive || owner.schedulerResourcesAck ||
       owner.schedulerResources1Ack) {
        out.code=Code::OwnershipConflict;return out;
    }
    if(!owner.mesFirmwareLive) {out.code=Code::FirmwareNotReady;return out;}
    if((owner.mesVersion & policy::kVersionMask)<policy::kMinLegacyMesVersion) {
        out.code=Code::UnsupportedMes;return out;
    }
    if(in.kiq.q.queueBytes != t::kDefaultRingDwords*4u ||
       in.scheduler.q.queueBytes != t::kDefaultRingDwords*4u ||
       !in.kiq.q.useDoorbell || !in.scheduler.q.useDoorbell ||
       in.kiq.q.doorbellIndex != t::kKiqDoorbellDword ||
       in.scheduler.q.doorbellIndex != t::kSchedulerDoorbellDword ||
       in.kiq.q.hqdActiveAtStart || in.scheduler.q.hqdActiveAtStart) {
        out.code=Code::BadQueue;return out;
    }
    if(!validateBuffers(in.kiq,in.scheduler,out.code))return out;
    out.kiqMqd=mqd::buildMesKiqMqd(in.kiq);
    out.schedulerMqd=mqd::buildMesKiqMqd(in.scheduler);
    out.kiqRegisterPlan=mes::buildKiqQueueRegs(in.kiq);
    out.schedulerRegisterPlan=mes::buildKiqQueueRegs(in.scheduler);
    if(out.kiqMqd.code!=mes::Code::PlanOnly ||
       out.schedulerMqd.code!=mes::Code::PlanOnly ||
       out.kiqRegisterPlan.code!=mes::Code::PlanOnly ||
       out.schedulerRegisterPlan.code!=mes::Code::PlanOnly) {
        out.code=Code::BadQueue;return out;
    }
    // mes_v11_0_queue_init_register() takes a ring->pipe argument and
    // programs the same MQD fields on MES pipe 0 or 1. Existing Cobalt64
    // KIQ planner pins pipe 1, so retarget its register destinations to 0.
    for(unsigned i=0;i<out.schedulerRegisterPlan.count;i++) {
        auto &w=out.schedulerRegisterPlan.writes[i];
        if(w.grbmMe!=mes::kMesGrbmMe || w.grbmPipe!=mes::kKiqPipe) {
            out.code=Code::InternalError;return out;
        }
        w.grbmPipe=mes::kSchedPipe;
    }
    mes::MapInput map{};
    map.mqdGpuVa=in.scheduler.q.mqdGpuVa;
    map.wptrGpuVa=in.scheduler.q.wptrGpuVa;
    map.doorbellIndex=t::kSchedulerDoorbellDword;
    map.queue=0u;map.pipe=mes::kSchedPipe;
    out.mapScheduler=mes::buildMapMesRing(map);
    if(!out.mapScheduler.ok) {out.code=Code::InternalError;return out;}
    out.code=Code::Ready;
    return out;
}

} } // namespace navi33::mes_routeb_bootstrap
