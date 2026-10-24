// SPDX-License-Identifier: MIT
// Host-only Navi33 Route B scheduler transport tests; no device I/O.
#include "../src/Navi33MesSchedulerTransport.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

namespace r = navi33::mes_gfx_routeb;
namespace t = navi33::mes_routeb_transport;

static r::Owner owner() {
    r::Owner o{};
    o.exactNavi33=o.mesFirmwareLive=o.schedulerQueueLive=true;
    o.kiqAndGfxUnowned=o.g42Disabled=o.freshEpoch=true;
    o.mesVersion=0x92;
    return o;
}
static r::ResourceInput resourceData() {
    r::ResourceInput v{};
    v.vmidGfx=v.vmidMmhub=0xff00;
    v.gfxHqd[0]=1;
    v.schedulerContext=0x8000010000ull;
    v.queryFence=0x8000011000ull;
    v.apiFence=0x8000012000ull;
    v.apiValue=1;
    v.gcBase[0]=0x1260;
    v.gcBase[1]=0xa000;
    return v;
}
static t::Input stage(t::Step s,uint64_t wp=0,uint64_t rp=0) {
    t::Input x{};
    x.step=s;x.owner=owner();
    x.ring.gpuBase=0x8000020000ull;
    x.ring.rptr=rp;x.ring.wptr=wp;
    x.ring.exclusive=true;
    x.seq=5;
    switch(s) {
    case t::Step::SetResources:
        x.command=r::resources(resourceData());
        break;
    case t::Step::SetResources1: {
        x.owner.schedulerResourcesAck=true;
        x.command=r::resources1({0x8000013000ull,0x8000012000ull,1},x.owner.mesVersion);
        break;
    }
    case t::Step::MapGfx: {
        x.owner.schedulerResourcesAck=x.owner.schedulerResources1Ack=true;
        r::AddInput a{};
        a.owner=x.owner;
        a.mqd=0x8000030000ull;
        a.wptr=0x8000031000ull;
        a.completionFence=0x8000012000ull;
        a.completionValue=1;
        x.command=r::addGfx(a);
        break;
    }
    }
    x.query=r::query(0x8000014000ull,x.seq);
    return x;
}
static void happy(t::Step step) {
    auto x=stage(step);
    const auto p=t::prepare(x);
    assert(p.ready() && p.step==step && p.count==128);
    assert(p.nextWptr==128 && p.previousWptr==0);
    assert(p.doorbellByte==0x58);
    assert(p.apiFenceAddress==0x8000012000ull && p.expectedApiFence==1);
    assert(p.schedulerFenceAddress==0x8000014000ull && p.expectedSchedulerFence==5);
    uint32_t ring[1024]{};
    assert(t::applyHostOnly(p,ring,1024));
    for(unsigned i=0;i<64;i++) {
        assert(ring[i]==x.command.packet.dw[i]);
        assert(ring[64+i]==x.query.dw[i]);
    }
    assert(!t::complete(p,{0,0}));
    assert(!t::complete(p,{1,0}));
    assert(!t::complete(p,{0,5}));
    assert(!t::complete(p,{1,4}));
    assert(t::complete(p,{1,5}));
}
static void wrapAndCapacity() {
    auto x=stage(t::Step::MapGfx,1000,1000);
    const auto p=t::prepare(x);
    assert(p.ready() && p.previousWptr==1000 && p.nextWptr==1128);
    assert(p.writes[0].index==1000 && p.writes[23].index==1023);
    assert(p.writes[24].index==0 && p.writes[127].index==103);
    uint32_t ring[1024]{};
    assert(t::applyHostOnly(p,ring,1024));
    assert(ring[0]==x.command.packet.dw[24]);
    assert(ring[103]==x.query.dw[63]);
    x=stage(t::Step::MapGfx,900,0);
    assert(t::prepare(x).code==t::Code::InsufficientSpace);
    x=stage(t::Step::MapGfx,1000,100);
    assert(t::prepare(x).code==t::Code::InsufficientSpace);
    x=stage(t::Step::MapGfx,1024,0);
    assert(t::prepare(x).code==t::Code::PointerRefused);
    x=stage(t::Step::MapGfx,0,1);
    assert(t::prepare(x).code==t::Code::PointerRefused);
    x=stage(t::Step::MapGfx,UINT64_MAX-10,UINT64_MAX-10);
    assert(t::prepare(x).code==t::Code::PointerRefused);
}
static void refusalTests() {
    auto x=stage(t::Step::MapGfx);
    x.owner.g42Disabled=false;
    assert(t::prepare(x).code==t::Code::OwnershipRefused);
    x=stage(t::Step::MapGfx);
    x.owner.schedulerQueueLive=false;
    assert(t::prepare(x).code==t::Code::SchedulerRefused);
    x=stage(t::Step::MapGfx);
    x.owner.schedulerResources1Ack=false;
    assert(t::prepare(x).code==t::Code::ResourceOrderRefused);
    x=stage(t::Step::SetResources);
    x.owner.schedulerResourcesAck=true;
    assert(t::prepare(x).code==t::Code::ResourceOrderRefused);
    x=stage(t::Step::SetResources1);
    x.owner.mesVersion=0x51;
    assert(t::prepare(x).code==t::Code::ResourceOrderRefused);
    x=stage(t::Step::MapGfx);
    x.ring.doorbellByte=0x60;
    assert(t::prepare(x).code==t::Code::RingRefused);
    x=stage(t::Step::MapGfx);
    x.ring.exclusive=false;
    assert(t::prepare(x).code==t::Code::RingRefused);
    x=stage(t::Step::MapGfx);
    x.ring.dwords=300;
    assert(t::prepare(x).code==t::Code::RingRefused);
    x=stage(t::Step::MapGfx);
    x.command.packet.dw[0]^=1;
    assert(t::prepare(x).code==t::Code::PacketRefused);
    x=stage(t::Step::MapGfx);
    x.query.dw[0]^=1;
    assert(t::prepare(x).code==t::Code::PacketRefused);
    x=stage(t::Step::MapGfx);
    x.query=r::query(x.command.packet.dw[0],5);
    assert(t::prepare(x).code==t::Code::PacketRefused);
    x=stage(t::Step::MapGfx);
    x.query=r::query(0x8000012000ull,5);
    assert(t::prepare(x).code==t::Code::FenceRefused); // aliases API fence
    x=stage(t::Step::MapGfx);
    x.query=r::query(0x8000014000ull,4);
    assert(t::prepare(x).code==t::Code::FenceRefused);
    x=stage(t::Step::MapGfx);
    x.seq=0;
    assert(t::prepare(x).code==t::Code::FenceRefused);
    const auto bad=t::prepare(x);
    uint32_t ring[1024]{};
    assert(!t::applyHostOnly(bad,ring,1024));
    auto good=t::prepare(stage(t::Step::MapGfx));
    assert(!t::applyHostOnly(good,ring,100));
    for(uint32_t v:ring)assert(v==0);
}
int main() {
    static_assert(t::kSchedulerDoorbellDword==0x16,"Navi33 MES0 byte offset 0x58");
    static_assert(t::kKiqDoorbellDword==0x18,"Navi33 MES1 byte offset 0x60");
    happy(t::Step::SetResources);
    happy(t::Step::SetResources1);
    happy(t::Step::MapGfx);
    wrapAndCapacity();
    refusalTests();
    puts("Navi33 MES Route B scheduler ring transport: PASS (host-only)");
}
