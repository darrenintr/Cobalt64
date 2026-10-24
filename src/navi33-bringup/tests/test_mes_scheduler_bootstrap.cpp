// SPDX-License-Identifier: MIT
// Host-only, no device writes: Navi33 MES KIQ -> Scheduler bootstrap.
#include "../src/Navi33MesSchedulerBootstrap.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

namespace b=navi33::mes_routeb_bootstrap;
namespace t=navi33::mes_routeb_transport;
namespace mes=navi33::gfx11_mes;

static mes::QueueInput queue(uint64_t base,uint32_t db) {
    mes::QueueInput q{};
    q.q.mqdGpuVa=base+0x1000u;
    q.q.pqGpuVa=base+0x2000u;
    q.q.eopGpuVa=base+0x4000u;
    q.q.rptrGpuVa=base+0x6000u;
    q.q.wptrGpuVa=base+0x6010u;
    q.q.queueBytes=4096u;
    q.q.useDoorbell=true;
    q.q.doorbellIndex=db;
    return q;
}
static b::Input good() {
    b::Input in{};
    in.owner.exactNavi33=in.owner.mesFirmwareLive=in.owner.freshEpoch=true;
    in.owner.kiqAndGfxUnowned=in.owner.g42Disabled=true;
    in.owner.mesVersion=0x92u;
    in.kiq=queue(0x8000100000ull,t::kKiqDoorbellDword);
    in.scheduler=queue(0x8000200000ull,t::kSchedulerDoorbellDword);
    return in;
}
static void golden() {
    const b::Input in=good();
    const auto p=b::prepare(in);
    assert(p.valid());
    assert(p.kiqRegisterPlan.count==15u);
    assert(p.schedulerRegisterPlan.count==15u);
    assert(p.kiqMqd.code==mes::Code::PlanOnly);
    assert(p.schedulerMqd.code==mes::Code::PlanOnly);
    for(unsigned i=0;i<15u;i++) {
        auto k=p.kiqRegisterPlan.writes[i];
        auto s=p.schedulerRegisterPlan.writes[i];
        assert(k.grbmMe==3 && k.grbmPipe==1);
        assert(s.grbmMe==3 && s.grbmPipe==0);
        assert(k.dword==s.dword && k.seg==s.seg);
    }
    assert(p.kiqRegisterPlan.writes[14].dword==navi33::gfx11_kiq::kCpHqdActive);
    assert(p.schedulerRegisterPlan.writes[14].dword==navi33::gfx11_kiq::kCpHqdActive);
    const auto &m=p.mapScheduler;
    assert(m.ok);
    assert(m.dw[0]==0xc005a200u); // gfx11 KIQ MAP_QUEUES packet
    assert(m.dw[1]==0x34080000u); // ME2 + ENGINE5 + one queue, pipe0 q0
    assert(m.dw[2]==(t::kSchedulerDoorbellDword<<2u));
    assert(m.dw[3]==static_cast<uint32_t>(in.scheduler.q.mqdGpuVa));
    assert(m.dw[4]==static_cast<uint32_t>(in.scheduler.q.mqdGpuVa>>32u));
    assert(m.dw[5]==static_cast<uint32_t>(in.scheduler.q.wptrGpuVa));
    assert(m.dw[6]==static_cast<uint32_t>(in.scheduler.q.wptrGpuVa>>32u));
    assert(p.kiqMqd.image.dw[navi33::gfx11_mqd::kCpHqdPqDoorbellControl] ==
        ((t::kKiqDoorbellDword<<2u)|(1u<<30u)));
    assert(p.schedulerMqd.image.dw[navi33::gfx11_mqd::kCpHqdPqDoorbellControl] ==
        ((t::kSchedulerDoorbellDword<<2u)|(1u<<30u)));
}
static void refusal() {
    auto in=good();in.owner.exactNavi33=false;
    assert(b::prepare(in).code==b::Code::WrongSilicon);
    in=good();in.owner.freshEpoch=false;
    assert(b::prepare(in).code==b::Code::NotFresh);
    in=good();in.owner.g42Disabled=false;
    assert(b::prepare(in).code==b::Code::OwnershipConflict);
    in=good();in.owner.schedulerQueueLive=true;
    assert(b::prepare(in).code==b::Code::OwnershipConflict);
    in=good();in.owner.mesFirmwareLive=false;
    assert(b::prepare(in).code==b::Code::FirmwareNotReady);
    in=good();in.owner.mesVersion=0x46u;
    assert(b::prepare(in).code==b::Code::UnsupportedMes);
    in=good();in.kiq.q.doorbellIndex=0u;
    assert(b::prepare(in).code==b::Code::BadQueue);
    in=good();in.scheduler.q.queueBytes=8192u;
    assert(b::prepare(in).code==b::Code::BadQueue);
    in=good();in.scheduler.q.hqdActiveAtStart=true;
    assert(b::prepare(in).code==b::Code::BadQueue);
    in=good();in.scheduler.q.mqdGpuVa+=256u;
    assert(b::prepare(in).code==b::Code::BadAddress);
    in=good();in.scheduler.q.wptrGpuVa=in.scheduler.q.rptrGpuVa;
    assert(b::prepare(in).code==b::Code::MemoryOverlap);
    in=good();in.scheduler.q.pqGpuVa=in.kiq.q.pqGpuVa;
    assert(b::prepare(in).code==b::Code::MemoryOverlap);
    in=good();in.scheduler.q.eopGpuVa=0xffffffffffffull-127u;
    assert(b::prepare(in).code==b::Code::BadAddress);
    in=good();in.scheduler.q.pqGpuVa+=0x80u; // not 256-byte aligned
    assert(b::prepare(in).code==b::Code::BadAddress);
}
int main() {
    golden();refusal();
    puts("Navi33 MES Route B dual-pipe bootstrap plan: PASS (offline)");
}
