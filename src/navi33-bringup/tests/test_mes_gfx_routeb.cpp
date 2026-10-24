// SPDX-License-Identifier: MIT
// Host-only Route B tests. No IOKit, MMIO, GPU doorbell or firmware writes.
#include "../src/Navi33MesGfxRouteB.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
using namespace navi33::mes_gfx_routeb;
static Owner readyOwner() {
    Owner o{};o.exactNavi33=o.mesFirmwareLive=o.schedulerQueueLive=true;
    o.schedulerResourcesAck=o.schedulerResources1Ack=true;
    o.kiqAndGfxUnowned=o.g42Disabled=o.freshEpoch=true;
    o.mesVersion=0x92;return o;
}
int main() {
    static_assert(sizeof(Packet)==256,"packet frame length");
    static_assert(kGfxDoorbellByte==0x458,"Navi33 GFX BAR2 doorbell");
    assert(header(kAddQueue)==0x00040201u);
    Owner o=readyOwner();
    assert(readiness(o).ok());
    o.mesVersion=0x46;assert(readiness(o).code==Code::BadScheduler);
    o=readyOwner();o.g42Disabled=false;assert(readiness(o).code==Code::BadOwnership);
    o=readyOwner();o.schedulerResources1Ack=false;
    assert(readiness(o).code==Code::BadResources);
    o.mesVersion=0x51;assert(readiness(o).ok()); // SET_RESOURCES_1 version gated
    o=readyOwner();o.freshEpoch=false;assert(readiness(o).code==Code::BadFirmware);
    AddInput in{};in.owner=readyOwner();in.mqd=0x8000001000ull;
    in.wptr=0x8000004000ull;in.completionFence=0x8000005000ull;
    in.completionValue=1;
    Build b=addGfx(in);assert(b.validation.ok());
    assert(b.statusOffset==offsetof(AddQueueFields,status));
    AddQueueFields a{};memcpy(&a,b.packet.dw,sizeof(a));
    assert(a.header.bits==header(kAddQueue));
    assert(a.pipe_id==0 && a.queue_id==0 && a.queue_type==kGfxType);
    assert(a.flags==kLegacyKqBit && a.doorbell_offset==kGfxDoorbellDword);
    assert(a.mqd_addr==in.mqd && a.wptr_addr==in.wptr);
    assert(a.status.addr==in.completionFence && a.status.value==1);
    for(unsigned j=sizeof(a)/4;j<64;j++)assert(b.packet.dw[j]==0);
    in.wptr+=4;assert(addGfx(in).validation.code==Code::BadAddress);
    in.wptr-=4;in.doorbellDword=0x117;assert(addGfx(in).validation.code==Code::BadRing);
    in.doorbellDword=kGfxDoorbellDword;in.owner.g42Disabled=false;
    assert(addGfx(in).validation.code==Code::BadOwnership);
    ResourceInput r{};r.vmidGfx=0xff00;r.vmidMmhub=0xff00;
    r.gfxHqd[0]=1;r.schedulerContext=0x8000010000ull;
    r.queryFence=0x8000011000ull;r.apiFence=0x8000012000ull;r.apiValue=1;
    r.gcBase[0]=0x1260u;r.gcBase[1]=0xa000u;
    auto res=resources(r);assert(res.validation.ok());
    ResourcesFields v{};memcpy(&v,res.packet.dw,sizeof(v));
    assert(v.header.bits==header(kSetResources));
    assert(v.vmid_gfxhub==0xff00u && v.vmid_mmhub==0xff00u);
    assert(v.gc_base[0]==0x1260 && v.gc_base[1]==0xa000);
    assert(v.flags==kResourcesFlags && v.status.addr==r.apiFence);
    r.gfxHqd[0]=0;assert(resources(r).validation.code==Code::BadResources);
    Resources1Input r1{0x8000013000ull,0x8000014000ull,1};
    auto p=resources1(r1,0x92);assert(p.validation.ok());
    Resources1Fields v1{};memcpy(&v1,p.packet.dw,sizeof(v1));
    assert(v1.flags==1 && v1.cleaner_fence==r1.cleanerFence);
    assert(v1.header.bits==header(kSetResources1));
    assert(resources1(r1,0x51).validation.code==Code::BadResources);
    Packet q=query(0x8000015000ull,3);
    QueryFields qs{};memcpy(&qs,q.dw,sizeof(qs));
    assert(qs.header.bits==header(kQueryStatus));
    assert(qs.status.addr==0x8000015000ull && qs.status.value==3);
    assert(!completed({1,0},1,1));
    assert(!completed({0,1},1,1));
    assert(completed({1,1},1,1));
    puts("Navi33 MES Route B ABI/ownership/fence checks: PASS (offline)");
}
