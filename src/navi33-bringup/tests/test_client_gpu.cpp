// SPDX-License-Identifier: MIT
#include "../src/Navi33ClientGpu.h"
#include "rx7600_ipd_fixture.h"
#include <cassert>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <unordered_map>
#include <utility>
#include <vector>
using namespace navi33;
namespace cg=client_gpu;
namespace vg=vm_gradient;
namespace v1=vmid1;
namespace vh=vram_heap;
namespace gv=gmc11_vm;
namespace gr=gradient;
namespace mq=mec_queue;
namespace gd=gfx11_dispatch;
namespace kiq=gfx11_kiq;
constexpr uint64_t kFb=0x8000000000ull;
// A fake GC after the autostart: banked HQDs (GRBM select), contexts 2..7 that walk
// four levels from their own roots, a TLB per VMID that keeps translations until
// invalidation engine 17 flushes that VMID, and a CP that runs each queue's ring:
// INDIRECT_BUFFER in the queue's VMID, the gradient dispatch, RELEASE_MEM.
struct Fake {
    cg::Regs r; vh::Heap heap; cg::Device dev;
    std::map<uint64_t,uint32_t> mem; std::map<std::pair<uint32_t,uint64_t>,uint32_t> bank;
    std::unordered_map<uint64_t,uint32_t> vram;
    std::map<std::pair<uint32_t,uint64_t>,uint64_t> tlb;          // (vmid, va page) -> vram page
    std::map<uint32_t,uint64_t> rptr;                               // per queue, dwords consumed
    std::vector<std::pair<uint64_t,uint32_t>> writes;
    std::set<uint32_t> paused;                                      // queues that do not run
    unsigned dispatches=0,flushes=0; bool noAck=false,stuck=false,noFlushModel=false;
    explicit Fake(cg::Regs regs):r(regs) {
        const v1::Regs &v=r.g.v;
        mem={{v.q.sos,42},{v.q.rlc,0xc000001fu},{v.q.grbm,0},{v.q.cntl,0x3c000000u},{v.q.scratch,0xdeadbeefu},
             {v.q.poll1,0},{v.l1,v1::kL1Default},{v.l2,v1::kL2Default},{v.l2c2,0},{v.l2c3,v1::kL2Cntl3Default},
             {v.l2c4,v1::kL2Cntl4Default},{v.l2c5,v1::kL2Cntl5Default},{v.faultStatus,0},{v.faultAddr,0},
             {r.invReq,0},{r.invAck,0}};
        for(auto o:v.ident)mem[o]=0;
        for(auto &c:r.ctx){mem[c[0]]=v1::kCtxDefault;for(int j=1;j<7;++j)mem[c[j]]=0;}
        assert(heap.configure(8176ull<<20,256ull<<20)==vh::Code::Ok && heap.reserve(0,16ull<<20,1)==vh::Code::Ok &&
               heap.reserve(240ull<<20,16ull<<20,2)==vh::Code::Ok);
    }
    uint32_t get(uint64_t off){auto i=vram.find(off);return i==vram.end() ? 0u : i->second;}
    uint64_t q(uint64_t off){return get(off)|(uint64_t(get(off+4))<<32);}
    bool banked(uint64_t off) const {
        if(off==r.g.shMem || off==r.g.shBases || off==r.dequeue)return true;
        for(uint32_t dw=mq::kHqdFirst;dw<=mq::kHqdLast;++dw)if(off==mq::hqd(r.g.v.q,dw))return true;
        return false;
    }
    uint32_t sel(){return mem[r.g.v.q.grbm];}
    bool faultNow=false;      // this queue's own fault (the status register is sticky and shared)
    void fault(uint32_t vmid,uint64_t va){
        faultNow=true;
        if(!mem[r.g.v.faultStatus]){mem[r.g.v.faultStatus]=0x100u|(vmid<<20);mem[r.g.v.faultAddr]=uint32_t(va>>12);}
    }
    // VA in VMID k -> VRAM offset, through the TLB first.
    bool translate(uint32_t vmid,uint64_t va,uint64_t &pa,bool write=false) {
        const auto &c=r.ctx[vmid-cg::kFirstVmid];
        const v1::Regs &v=r.g.v;
        if((mem[v.l1]&0x58u)!=0x58u || !(mem[v.l2]&1u) || !(mem[c[0]]&1u) || ((mem[c[0]]>>1)&3u)!=3u){fault(vmid,va);return false;}
        auto t=tlb.find({vmid,va>>12});
        if(t!=tlb.end()){pa=t->second+(va&0xfffu);return true;}
        uint64_t table=(mem[c[1]]|(uint64_t(mem[c[2]])<<32))&gv::kAddrMask;
        for(uint32_t lv=3;;--lv) {
            const uint64_t e=q(table+8u*gv::index(va,lv));
            if(!(e&1u) || (!lv && write && !(e&gv::kPteWrite))){fault(vmid,va);return false;}
            table=e&gv::kAddrMask;
            if(!lv)break;
        }
        tlb[{vmid,va>>12}]=table;pa=table+(va&0xfffu);return true;
    }
    uint32_t rd(uint32_t k,uint64_t va){uint64_t p=0;return translate(k,va,p) ? get(p) : 0xdeadu;}
    void wr(uint32_t k,uint64_t va,uint32_t v){uint64_t p=0;if(translate(k,va,p,true))vram[p]=v;}
    // Run PM4 [from, to) in VMID k (ring or IB); returns false on a fault.
    void exec(uint32_t k,uint64_t base,uint64_t from,uint64_t to,bool ring,uint64_t &pgm,uint64_t &karg,uint32_t &masks) {
        for(uint64_t i=from;i<to;) {
            auto at=[&](uint64_t d){return base+4u*(ring ? d%cg::kRingDwords : d);};
            const uint32_t h=rd(k,at(i));
            if(faultNow)return;
            if(h==mq::kNop){++i;continue;}
            const uint32_t op=(h>>8)&0xffu,n=((h>>16)&0x3fffu)+1u;
            std::vector<uint32_t> b(n);
            for(uint32_t j=0;j<n;++j)b[j]=rd(k,at(i+1u+j));
            if(op==0x3fu) {
                assert(ring && (b[2]&(1u<<23)) && (b[2]>>24)==k);
                exec(k,b[0]|(uint64_t(b[1]&0xffffu)<<32),0,b[2]&0xfffffu,false,pgm,karg,masks);
            } else if(op==gd::kSetShReg) {
                for(uint32_t j=0;j+1<n;++j) {
                    const uint32_t rg=gd::kShRegStart+b[0]+j-gd::kGcBase,v=b[1+j];
                    if(rg==gd::kPgmLo)pgm=(pgm&~0xffffffffull)|(uint64_t(v)<<8);
                    if(rg==gd::kPgmLo+1u)pgm|=uint64_t(v&0xffu)<<40;
                    if(rg==gd::kUserData0)karg=(karg&~0xffffffffull)|v;
                    if(rg==gd::kUserData0+1u)karg|=uint64_t(v)<<32;
                    if((rg==0x1bb6u||rg==0x1bb7u||rg==0x1bb9u||rg==0x1bbau) && v==~0u)++masks;
                }
            } else if(op==gd::kDispatchDirect) {
                ++dispatches;
                if(masks==4u && rd(k,pgm) && bank[{k<<4,r.g.shMem}]==gr::kShMemDefault) {
                    const uint64_t px=rd(k,karg)|(uint64_t(rd(k,karg+4))<<32);
                    const uint32_t pitch=rd(k,karg+8),w=rd(k,karg+12),hh=rd(k,karg+16),fr=rd(k,karg+20);
                    for(uint32_t y=0;y<b[1]*8u;++y)for(uint32_t x=0;x<b[0]*8u;++x)
                        if(x<w && y<hh)wr(k,px+4ull*(y*pitch+x),gfx11_shader::gradientPixel(x,y,w,hh,fr));
                }
                masks=0;
            } else if(op==gd::kReleaseMem) wr(k,b[2]|(uint64_t(b[3])<<32),b[4]);
            else assert(!"unexpected packet");
            i+=1u+n;
        }
    }
    void runQueue(uint32_t k) {
        const uint32_t qs=mq::grbmSelect(1,0,k);
        if(stuck || paused.count(k) || !(bank[{qs,mq::hqd(r.g.v.q,kiq::kCpHqdActive)}]&1u))return;
        assert((bank[{qs,mq::hqd(r.g.v.q,kiq::kCpHqdVmid)}]==k));
        const uint64_t mqd=bank[{qs,mq::hqd(r.g.v.q,0x1fa9u)}]|(uint64_t(bank[{qs,mq::hqd(r.g.v.q,0x1faau)}])<<32);
        assert(mqd>=kFb && heap.covers(mqd-kFb,4096));
        const uint64_t ring=(uint64_t(bank[{qs,mq::hqd(r.g.v.q,kiq::kCpHqdPqBase)}])|
                             (uint64_t(bank[{qs,mq::hqd(r.g.v.q,kiq::kCpHqdPqBaseHi)}])<<32))<<8;
        const uint64_t poll=bank[{qs,mq::hqd(r.g.v.q,kiq::kCpHqdPqWptrPollAddr)}]|
            (uint64_t(bank[{qs,mq::hqd(r.g.v.q,kiq::kCpHqdPqWptrPollAddrHi)}])<<32);
        const uint64_t wptr=rd(k,poll)|(uint64_t(rd(k,poll+4))<<32);
        uint64_t pgm=0,karg=0;uint32_t masks=0;faultNow=false;
        exec(k,ring,rptr[k],wptr,true,pgm,karg,masks);
        rptr[k]=wptr;bank[{qs,mq::hqd(r.g.v.q,kiq::kCpHqdPqRptr)}]=uint32_t(wptr%cg::kRingDwords);
    }
    static bool read(void *c,uint64_t off,uint32_t *v) {
        auto &f=*static_cast<Fake *>(c);
        if(f.banked(off)){*v=f.bank[{f.sel(),off}];return true;}
        assert(f.mem.count(off));*v=f.mem[off];return true;
    }
    static bool write(void *c,uint64_t off,uint32_t v) {
        auto &f=*static_cast<Fake *>(c);
        assert(cg::allowed(f.r,off));
        f.writes.emplace_back(off,v);
        if(f.banked(off))f.bank[{f.sel(),off}]=v; else f.mem[off]=v;
        if(off==f.r.invReq) {
            uint32_t ack=0;
            for(uint32_t k=0;k<16;++k)if(v&(1u<<k)){
                if(!f.noFlushModel)for(auto i=f.tlb.begin();i!=f.tlb.end();)i=i->first.first==k ? f.tlb.erase(i) : std::next(i);
                ack|=1u<<k;
            }
            f.mem[f.r.invAck]=f.noAck ? 0u : ack;++f.flushes;
        }
        if(off==f.r.dequeue && v==cg::kDrainPipe && !f.stuck)f.bank[{f.sel(),mq::hqd(f.r.g.v.q,kiq::kCpHqdActive)}]=0u;
        if(off==f.r.g.v.q.poll1)for(uint32_t k=cg::kFirstVmid;k<cg::kFirstVmid+cg::kClients;++k)if(v&(1u<<k))f.runQueue(k);
        return true;
    }
    static bool vw(void *c,uint64_t off,const uint32_t *src,uint32_t n) {
        auto &f=*static_cast<Fake *>(c);assert(f.heap.covers(off,4ull*n));
        for(uint32_t i=0;i<n;++i)f.vram[off+4u*i]=src[i];
        return true;
    }
    static bool vr(void *c,uint64_t off,uint32_t *dst,uint32_t n) {
        auto &f=*static_cast<Fake *>(c);assert(f.heap.covers(off,4ull*n));
        for(uint32_t i=0;i<n;++i)dst[i]=f.get(off+4u*i);
        return true;
    }
    static void delay(void *,uint32_t){}
    cg::Hw hw(){return {this,read,write,vw,vr,delay};}
    // CPU write into a client's buffer (what the user's CPU mapping does).
    void put(uint32_t slot,uint32_t handle,uint32_t off,const std::vector<uint32_t> &v) {
        uint64_t base=0,bytes=0;assert(dev.boRange(slot,handle,base,bytes) && off+4u*v.size()<=bytes);
        for(size_t i=0;i<v.size();++i)vram[base+off+4u*i]=v[i];
    }
};
// A user-built IB: thread masks, the dispatch, nothing else (the kernel adds the fence).
std::vector<uint32_t> gradientIb(const gfx11_shader::Kernel &k,uint64_t shader,uint64_t args,uint32_t w,uint32_t h) {
    std::vector<uint32_t> ib={gd::packet3(gd::kSetShReg,2u),gd::shOffset(gr::kThreadSe0),~0u,~0u,
                              gd::packet3(gd::kSetShReg,2u),gd::shOffset(gr::kThreadSe2),~0u,~0u};
    gd::Request rq{};rq.kernel=&k;rq.shaderMc=shader;rq.kernargMc=args;rq.groupsX=w/8u;rq.groupsY=h/8u;
    rq.fenceMc=args+0x800u;rq.fenceValue=1u;
    const gd::Stream s=gd::encode(rq);assert(s.code==gd::Code::Ok);
    for(size_t i=0;i+8<s.count;++i)ib.push_back(s.dwords[i]);      // drop the encoder's own RELEASE_MEM
    return ib;
}
struct App { uint32_t slot=0, shader=0, args=0, image=0, ib=0; };
App setup(Fake &f,const std::vector<uint8_t> &co,const gfx11_shader::Kernel &k,uint32_t frame,uint64_t vaBase) {
    App a{};
    assert(f.dev.open(&f+frame,a.slot)==cg::Code::Ok);
    uint64_t vr=0;
    assert(f.dev.boCreate(a.slot,gr::kShaderMax,a.shader,vr)==cg::Code::Ok);
    assert(f.dev.boCreate(a.slot,4096,a.args,vr)==cg::Code::Ok);
    assert(f.dev.boCreate(a.slot,gr::kImageBytes,a.image,vr)==cg::Code::Ok);
    assert(f.dev.boCreate(a.slot,4096,a.ib,vr)==cg::Code::Ok);
    assert(f.dev.vaMap(a.slot,a.shader,vaBase,false,true)==cg::Code::Ok);
    assert(f.dev.vaMap(a.slot,a.args,vaBase+0x10000,true,false)==cg::Code::Ok);
    assert(f.dev.vaMap(a.slot,a.image,vaBase+0x20000,true,false)==cg::Code::Ok);
    assert(f.dev.vaMap(a.slot,a.ib,vaBase+0x30000,false,false)==cg::Code::Ok);
    std::vector<uint32_t> code(gr::kShaderMax/4u,0u);
    for(uint64_t i=0;i<k.codeBytes;++i)code[i/4u]|=uint32_t(co[k.codeOffset+i])<<(8u*(i%4u));
    f.put(a.slot,a.shader,0,code);
    uint32_t argv[6];gr::args(argv,vaBase+0x20000,64,64,64,frame);
    f.put(a.slot,a.args,0,std::vector<uint32_t>(argv,argv+6));
    f.put(a.slot,a.ib,0,gradientIb(k,vaBase,vaBase+0x10000,64,64));
    return a;
}
bool imageOk(Fake &f,const App &a,uint32_t frame) {
    uint64_t base=0,bytes=0;assert(f.dev.boRange(a.slot,a.image,base,bytes));
    for(uint32_t y=0;y<64;++y)for(uint32_t x=0;x<64;++x)
        if(f.get(base+4u*(y*64u+x))!=gfx11_shader::gradientPixel(x,y,64,64,frame))return false;
    return true;
}
uint32_t ibDwords(const gfx11_shader::Kernel &k){return static_cast<uint32_t>(gradientIb(k,0x10000,0x20000,64,64).size());}
int main() {
    std::ifstream in("shaders/gradient.co",std::ios::binary);
    const std::vector<uint8_t> co((std::istreambuf_iterator<char>(in)),std::istreambuf_iterator<char>());
    const gfx11_shader::Kernel k=gr::kernelOf(co.data(),co.size());assert(k.code==gfx11_shader::Code::Ok);
    auto b=rx7600_fixture::blob();IPDiscovery d;assert(d.parse(b.data(),b.size()));
    const auto r=cg::resolve(d,rx7600_fixture::kBar5);
    assert(r.valid && r.invReq==(0x1260u+0x16bcu)*4u && r.invAck==(0x1260u+0x16ceu)*4u && r.dequeue==(0x1260u+0x1fc1u)*4u);
    assert(r.ctx[0][0]==(0x1260u+0x168au)*4u && r.ctx[5][6]==(0x1260u+0x1742u)*4u);
    // Never context 0/1, SCRATCH, the MEC controls or the fault registers.
    for(uint64_t o:{r.g.v.ctx,r.g.v.baseLo,r.g.v.endHi,r.g.v.q.scratch,r.g.v.q.cntl,r.g.v.q.pc,r.g.v.faultStatus,r.invAck})
        assert(!cg::allowed(r,o));
    // 1. Two clients, each its own VMID, queue, buffers and image.
    {   Fake f(r);
        {uint32_t none=0;assert(f.dev.open(&f,none)==cg::Code::NotReady);}
        assert(f.dev.start(r,f.hw(),f.heap,kFb)==cg::Code::Ok && f.mem[r.g.v.l1]==0x1859u && f.writes.size()==12u);
        App a=setup(f,co,k,7,0x100000000ull), c=setup(f,co,k,99,0x100000000ull);   // same VAs, different VMIDs
        assert(a.slot==0 && c.slot==1 && f.dev.client(0)->vmid==2u && f.dev.client(1)->vmid==3u);
        uint64_t sa=0,sc=0,ret=0;
        const cg::Device::Ib ia{0x100030000ull,ibDwords(k)};
        assert(f.dev.submit(a.slot,&ia,1,sa)==cg::Code::Ok && sa==1u);
        assert(f.dev.submit(c.slot,&ia,1,sc)==cg::Code::Ok && sc==1u);
        assert(f.dev.wait(a.slot,sa,1000,ret)==cg::Code::Ok && ret==1u && f.dev.wait(c.slot,sc,1000,ret)==cg::Code::Ok);
        assert(imageOk(f,a,7) && imageOk(f,c,99) && f.dispatches==2);
        // VMID k's context: depth 3 at the client's root; SH_MEM for VMID k only.
        assert(f.mem[r.ctx[0][0]]==0x01fffe07u && (f.mem[r.ctx[0][1]]&~0xfffu)==f.dev.client(0)->space.rootVram());
        assert((f.bank[{2u<<4,r.g.shMem}]==gr::kShMemDefault) && (f.bank[{0u,r.g.shMem}]==0u));
        // Isolation: client c cannot submit an IB at a VA it never mapped.
        const cg::Device::Ib far{0x200000000ull,4};
        uint64_t s=0;assert(f.dev.submit(c.slot,&far,1,s)==cg::Code::BadArgs);
        // Many submissions: the ring wraps, sequence numbers keep counting.
        for(int i=0;i<300;++i){assert(f.dev.submit(a.slot,&ia,1,s)==cg::Code::Ok);assert(f.dev.wait(a.slot,s,1000,ret)==cg::Code::Ok);}
        assert(s==301u && f.dev.client(0)->wptr>cg::kRingDwords && imageOk(f,a,7));
        // Back-pressure: a paused queue fills its ring and the next submission is Busy, not an overwrite.
        f.paused.insert(3u);
        cg::Code code=cg::Code::Ok;int n=0;
        while((code=f.dev.submit(c.slot,&ia,1,s))==cg::Code::Ok)++n;
        assert(code==cg::Code::Busy && n>0 && n<64);
        assert(f.dev.wait(c.slot,s,500,ret)==cg::Code::Timeout);
        f.paused.clear();f.runQueue(3);              // the queue catches up
        assert(f.dev.wait(c.slot,s,1000,ret)==cg::Code::Ok && ret==s);
        // Close: drain, dequeue, disable context, flush, give back every page.
        assert(f.dev.close(c.slot)==cg::Code::Ok && f.dev.close(a.slot)==cg::Code::Ok);
        assert(f.bank[{mq::grbmSelect(1,0,2),mq::hqd(r.g.v.q,kiq::kCpHqdActive)}]==0u && !(f.mem[r.ctx[0][0]]&1u));
        assert(f.heap.usedBytes(cg::kTagClient)==0u && f.heap.usedBytes(gv::kTagTables)==0u && f.heap.consistent());
    }
    // 2. Teardown returns every page; reopening reuses the slot.
    {   Fake f(r);assert(f.dev.start(r,f.hw(),f.heap,kFb)==cg::Code::Ok);
        const uint64_t before=f.heap.freeBytes();
        App a=setup(f,co,k,7,0x100000000ull);uint64_t s=0,ret=0;
        const cg::Device::Ib ia{0x100030000ull,ibDwords(k)};
        assert(f.dev.submit(a.slot,&ia,1,s)==cg::Code::Ok && f.dev.wait(a.slot,s,1000,ret)==cg::Code::Ok);
        uint32_t slot=9;assert(f.dev.slotOf(&f+7,slot) && slot==a.slot);
        assert(f.dev.close(a.slot)==cg::Code::Ok && f.heap.freeBytes()==before && f.heap.consistent());
        assert(!f.dev.slotOf(&f+7,slot) && f.dev.open(&f,slot)==cg::Code::Ok && slot==0u);
    }
    // 3. Remapping needs the TLB flush: the second buffer receives the write, not the first.
    for(bool model:{true,false}) {
        Fake f(r);f.noFlushModel=!model;assert(f.dev.start(r,f.hw(),f.heap,kFb)==cg::Code::Ok);
        uint32_t slot=0,x=0,y=0,ib=0;uint64_t vx=0,vy=0,vi=0,s=0,ret=0;
        assert(f.dev.open(&f,slot)==cg::Code::Ok);
        assert(f.dev.boCreate(slot,4096,x,vx)==cg::Code::Ok && f.dev.boCreate(slot,4096,y,vy)==cg::Code::Ok &&
               f.dev.boCreate(slot,4096,ib,vi)==cg::Code::Ok);
        assert(f.dev.vaMap(slot,x,0x500000,true,false)==cg::Code::Ok && f.dev.vaMap(slot,ib,0x600000,false,false)==cg::Code::Ok);
        const std::vector<uint32_t> w={gd::packet3(gd::kReleaseMem,6u),gd::kReleaseMemControl,gd::kReleaseMemDataSel32,
                                       0x500000u,0u,0x1234u,0u,0u};
        f.put(slot,ib,0,w);
        const cg::Device::Ib i1{0x600000,8};
        assert(f.dev.submit(slot,&i1,1,s)==cg::Code::Ok && f.dev.wait(slot,s,1000,ret)==cg::Code::Ok && f.get(vx)==0x1234u);
        assert(f.dev.vaUnmap(slot,x)==cg::Code::Ok && f.dev.vaMap(slot,y,0x500000,true,false)==cg::Code::Ok);
        f.put(slot,ib,0,{gd::packet3(gd::kReleaseMem,6u),gd::kReleaseMemControl,gd::kReleaseMemDataSel32,0x500000u,0u,0x5678u,0u,0u});
        assert(f.dev.submit(slot,&i1,1,s)==cg::Code::Ok && f.dev.wait(slot,s,1000,ret)==cg::Code::Ok);
        // With the flush modelled the new page gets it; a hub that ignored the flush would hit the old one.
        assert(model ? (f.get(vy)==0x5678u && f.get(vx)==0x1234u) : f.get(vx)==0x5678u);
        // An unmapped buffer is unreachable: freeing it is safe.
        assert(f.dev.boFree(slot,x)==cg::Code::Ok && f.dev.boFree(slot,x)==cg::Code::NotFound);
    }
    // 3b. After unmap (or a free of a mapped buffer) the GPU cannot reach the old pages:
    //     a write there faults instead of landing in memory the heap may hand out again.
    for(bool viaFree:{false,true}) {
        Fake f(r);assert(f.dev.start(r,f.hw(),f.heap,kFb)==cg::Code::Ok);
        uint32_t slot=0,x=0,ib=0;uint64_t vx=0,vi=0,s=0,ret=0;
        assert(f.dev.open(&f,slot)==cg::Code::Ok && f.dev.boCreate(slot,4096,x,vx)==cg::Code::Ok &&
               f.dev.boCreate(slot,4096,ib,vi)==cg::Code::Ok);
        assert(f.dev.vaMap(slot,x,0x500000,true,false)==cg::Code::Ok && f.dev.vaMap(slot,ib,0x600000,false,false)==cg::Code::Ok);
        f.put(slot,ib,0,{gd::packet3(gd::kReleaseMem,6u),gd::kReleaseMemControl,gd::kReleaseMemDataSel32,0x500000u,0u,0x1111u,0u,0u});
        const cg::Device::Ib i1{0x600000,8};
        assert(f.dev.submit(slot,&i1,1,s)==cg::Code::Ok && f.dev.wait(slot,s,1000,ret)==cg::Code::Ok && f.get(vx)==0x1111u);
        assert((viaFree ? f.dev.boFree(slot,x) : f.dev.vaUnmap(slot,x))==cg::Code::Ok);
        f.put(slot,ib,0,{gd::packet3(gd::kReleaseMem,6u),gd::kReleaseMemControl,gd::kReleaseMemDataSel32,0x500000u,0u,0x2222u,0u,0u});
        assert(f.dev.submit(slot,&i1,1,s)==cg::Code::Ok && f.dev.wait(slot,s,1000,ret)==cg::Code::Faulted);
        assert(f.get(vx)==0x1111u);
    }
    // 3c. Close with work still queued waits; if the queue never drains, nothing is freed.
    {   Fake f(r);assert(f.dev.start(r,f.hw(),f.heap,kFb)==cg::Code::Ok);
        App a=setup(f,co,k,7,0x100000000ull);uint64_t s=0;
        const cg::Device::Ib ia{0x100030000ull,ibDwords(k)};
        f.paused.insert(2u);
        assert(f.dev.submit(a.slot,&ia,1,s)==cg::Code::Ok);
        const uint64_t used=f.heap.usedBytes(cg::kTagClient);
        assert(f.dev.close(a.slot)==cg::Code::Quarantined && f.heap.usedBytes(cg::kTagClient)==used);
        assert(f.bank[{mq::grbmSelect(1,0,2),mq::hqd(r.g.v.q,kiq::kCpHqdActive)}]==1u);   // never dequeued under work
    }
    // 4. A fault in the client's VMID ends that client (not the others); it can still be torn down.
    {   Fake f(r);assert(f.dev.start(r,f.hw(),f.heap,kFb)==cg::Code::Ok);
        App good=setup(f,co,k,5,0x100000000ull);
        uint32_t slot=0,ib=0;uint64_t vi=0,s=0,ret=0;
        assert(f.dev.open(&f,slot)==cg::Code::Ok && f.dev.boCreate(slot,4096,ib,vi)==cg::Code::Ok &&
               f.dev.vaMap(slot,ib,0x700000,false,false)==cg::Code::Ok);
        f.put(slot,ib,0,{gd::packet3(gd::kReleaseMem,6u),gd::kReleaseMemControl,gd::kReleaseMemDataSel32,0x900000u,0u,1u,0u,0u});
        const cg::Device::Ib bad{0x700000,8};
        assert(f.dev.submit(slot,&bad,1,s)==cg::Code::Ok);
        assert(f.dev.wait(slot,s,1000,ret)==cg::Code::Faulted && f.dev.client(slot)->faultStatus);
        assert(f.dev.submit(slot,&bad,1,s)==cg::Code::Faulted);
        const cg::Device::Ib ia{0x100030000ull,ibDwords(k)};
        assert(f.dev.submit(good.slot,&ia,1,s)==cg::Code::Ok && f.dev.wait(good.slot,s,1000,ret)==cg::Code::Ok && imageOk(f,good,5));
        assert(f.dev.close(slot)==cg::Code::Ok);
    }
    // 5. A queue that will not dequeue, or a flush without ACK, quarantines: nothing is freed.
    {   Fake f(r);assert(f.dev.start(r,f.hw(),f.heap,kFb)==cg::Code::Ok);
        App a=setup(f,co,k,7,0x100000000ull);
        const uint64_t used=f.heap.usedBytes(cg::kTagClient);
        f.stuck=true;
        assert(f.dev.close(a.slot)==cg::Code::Quarantined && f.heap.usedBytes(cg::kTagClient)==used);
        uint64_t s=0;const cg::Device::Ib ia{0x100030000ull,ibDwords(k)};
        assert(f.dev.submit(a.slot,&ia,1,s)==cg::Code::Quarantined);
        uint32_t slot=0;assert(f.dev.open(&f,slot)==cg::Code::Ok && slot==1u);   // the slot stays taken
    }
    {   Fake f(r);assert(f.dev.start(r,f.hw(),f.heap,kFb)==cg::Code::Ok);
        uint32_t slot=0,x=0;uint64_t vx=0;
        assert(f.dev.open(&f,slot)==cg::Code::Ok && f.dev.boCreate(slot,4096,x,vx)==cg::Code::Ok);
        f.noAck=true;
        assert(f.dev.vaMap(slot,x,0x500000,true,false)==cg::Code::Quarantined);
        assert(f.dev.boFree(slot,x)==cg::Code::Quarantined && f.heap.covers(vx,4096));
    }
    // 6. Arguments and a hub this code does not recognise.
    {   Fake f(r);assert(f.dev.start(r,f.hw(),f.heap,kFb)==cg::Code::Ok);
        uint32_t slot=0,x=0;uint64_t vx=0;assert(f.dev.open(&f,slot)==cg::Code::Ok);
        assert(f.dev.boCreate(slot,0,x,vx)==cg::Code::BadArgs && f.dev.boCreate(slot,(64ull<<20)+1,x,vx)==cg::Code::BadArgs);
        assert(f.dev.boCreate(slot,5000,x,vx)==cg::Code::Ok);
        for(uint64_t va:std::vector<uint64_t>{0x0ull,0x1000ull,0x500001ull,cg::kUserVaMax,cg::kKernelVa})assert(f.dev.vaMap(slot,x,va,true,false)==cg::Code::BadArgs);
        assert(f.dev.vaMap(slot,x,0x500000,true,false)==cg::Code::Ok && f.dev.vaMap(slot,x,0x900000,true,false)==cg::Code::BadArgs);
        uint32_t y=0;assert(f.dev.boCreate(slot,4096,y,vx)==cg::Code::Ok && f.dev.vaMap(slot,y,0x501000,true,false)==cg::Code::BadArgs);
        uint64_t s=0;const cg::Device::Ib ib{0x500000,0};assert(f.dev.submit(slot,&ib,1,s)==cg::Code::BadArgs);
        assert(f.dev.wait(slot,5,0,s)==cg::Code::BadArgs && f.dev.boFree(7,1)==cg::Code::NotFound);
        for(uint32_t i=1;i<cg::kClients;++i){uint32_t o=0;assert(f.dev.open(&f+i,o)==cg::Code::Ok);}
        uint32_t o=0;assert(f.dev.open(&f,o)==cg::Code::NoSlot);
    }
    {   Fake f(r);f.mem[r.g.v.l1]=0x1234u;
        assert(f.dev.start(r,f.hw(),f.heap,kFb)==cg::Code::Hub && f.writes.empty());
        Fake g(r);g.mem[r.g.v.q.cntl]=0x400f0000u;
        assert(g.dev.start(r,g.hw(),g.heap,kFb)==cg::Code::NotReady && g.writes.empty());
        Fake h(r);h.mem[r.g.v.l1]=0x1859u;h.mem[r.g.v.l2]=0x00080e01u;     // G36-G39 already set the hub
        assert(h.dev.start(r,h.hw(),h.heap,kFb)==cg::Code::Ok && h.writes.empty());
    }
    puts("Client GPU contexts: per-client VMID/queue/address space, user IBs through INDIRECT_BUFFER, fences, "
         "ring wrap and back-pressure, TLB flush on remap, fault isolation, drain/dequeue teardown, quarantine: passed");
}
