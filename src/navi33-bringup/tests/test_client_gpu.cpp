// SPDX-License-Identifier: MIT
#include "../src/Navi33ClientGpu.h"
#include "../src/Navi33N48nFront.h"
#include "rx7600_ipd_fixture.h"
#include <cassert>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
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
// G40's register writes on a fresh pipe: PFP counters (pipe 0 is select 0, already current: 2; pipe 1:
// select + 2), select 0, reset and release; the same for ME; RB0 (9, then 7 after the delay: base, base hi,
// active, doorbell control, RB doorbell range); MAX_CONTEXT, DEVICE_ID, unhalt. The kick is a doorbell.
constexpr size_t kGfxStartWrites=(2u+3u+1u+2u)*2u+9u+7u+3u;
// The HQD of the client with VMID k (queue 0/1 of MEC pipe 1..3).
constexpr uint32_t qsOf(uint32_t k){return mq::grbmSelect(1,cg::pipeOf(k-cg::kFirstVmid),cg::queueOf(k-cg::kFirstVmid));}
// A fake GC after the autostart: banked HQDs (GRBM select), contexts 2..7 that walk
// four levels from their own roots, a TLB per VMID that keeps translations until
// invalidation engine 17 flushes that VMID, and a CP that runs each queue's ring:
// INDIRECT_BUFFER in the queue's VMID, the gradient dispatch, RELEASE_MEM.
struct Fake {
    bool g42=false,mapStuck=false;unsigned stageWrites=0,hdpWrites=0,mapTriggers=0;
    bool hdpFlushSeen=false,hdpMemsizeReadAfterFlush=false,corruptRemapReadback=false,failGfxSnapshotRead=false;
    bool failHdpRegWrite=false,failHdpFlushWrite=false;
    std::vector<unsigned> hdpEvents;
    cg::Regs r; vh::Heap heap; cg::Device dev;
    std::map<uint64_t,uint32_t> mem; std::map<std::pair<uint32_t,uint64_t>,uint32_t> bank;
    std::unordered_map<uint64_t,uint32_t> vram;
    std::map<std::pair<uint32_t,uint64_t>,uint64_t> tlb;          // (vmid, va page) -> vram page
    std::map<uint32_t,uint64_t> rptr;                               // per queue, dwords consumed
    std::vector<std::pair<uint64_t,uint32_t>> writes;
    std::set<uint32_t> paused;                                      // queues that do not run
    // An HQD that processed an EOP event and was dequeued keeps its EOP fetcher state until a
    // load writes CP_HQD_EOP_RPTR.INIT_FETCHER (bit 31); until then RELEASE_MEM never lands
    // (the card, 2026-10-20 13:19-13:58, with bit 30 written instead).
    std::set<uint32_t> eopRan, eopStale;
    unsigned dispatches=0,flushes=0; bool noAck=false,stuck=false,noFlushModel=false;
    explicit Fake(cg::Regs regs):r(regs) {
        const v1::Regs &v=r.g.v;
        mem={{v.q.sos,42},{v.q.rlc,0xc000001fu},{v.q.grbm,0},{v.q.cntl,0x3c000000u},{v.q.scratch,0xdeadbeefu},
             {v.q.poll1,0},{v.l1,v1::kL1Default},{v.l2,v1::kL2Default},{v.l2c2,0},{v.l2c3,v1::kL2Cntl3Default},
             {v.l2c4,v1::kL2Cntl4Default},{v.l2c5,v1::kL2Cntl5Default},{v.faultStatus,0},{v.faultAddr,0},
             {r.invReq,0x02f80000u},{r.invAck,0},{r.l2Status,0},{r.mecDbLower,0x1234u},{r.mecDbUpper,0u},{r.nbioAperEn,0x100u},{r.pqStatus,0x8u},{r.pollCntl,0x80000123u},
             {r.hdp.remapMem,0u},{r.hdp.remapReg,0u},{r.hdp.memsize,8192u}};
        for(auto o:v.ident)mem[o]=0;
        const navi33::gfx_ring::Regs &g=r.gfx;
        for(uint64_t o:{g.pfpStart,g.pfpStartHi,g.meStart,g.meStartHi,g.cpStat,g.maxContext,g.deviceId,g.wptrDelay,g.rbVmid,
                        g.rb0Cntl,g.rb0Wptr,g.rb0WptrHi,g.rb0Rptr,g.rb0RptrAddr,g.rb0RptrAddrHi,g.wptrPollLo,g.wptrPollHi,
                        g.rb0Base,g.rb0BaseHi,g.rbActive,g.rbDoorbellCntl,g.rbDbLower,g.rbDbUpper})mem[o]=0;
        for(uint64_t o:{r.gfxHqdActive,r.gfxHqdVmid,r.gfxHqdBase,r.gfxHqdBaseHi,r.gfxHqdRptr,
                        r.gfxHqdCntl,r.gfxHqdWptr,r.gfxHqdWptrHi})mem[o]=0;
        mem[g.meCntl]=navi33::gfx_ring::kMeCntlDefault;
        for(auto &c:r.ctx){mem[c[0]]=v1::kCtxDefault;for(int j=1;j<7;++j)mem[c[j]]=0;}
        assert(heap.configure(8176ull<<20,256ull<<20)==vh::Code::Ok && heap.reserve(0,16ull<<20,0x424f4f54u)==vh::Code::Ok &&
               heap.reserve(240ull<<20,16ull<<20,0x53544753u)==vh::Code::Ok);
    }
    uint32_t get(uint64_t off){auto i=vram.find(off);return i==vram.end() ? 0u : i->second;}
    uint64_t q(uint64_t off){return get(off)|(uint64_t(get(off+4))<<32);}
    bool banked(uint64_t off) const {
        if(off==r.g.shMem || off==r.g.shBases || off==r.dequeue)return true;
        for(uint32_t dw=mq::kHqdFirst;dw<=mq::kHqdLast;++dw)if(off==mq::hqd(r.g.v.q,dw))return true;
        const uint64_t b0=mq::hqd(r.g.v.q,mq::kHqdFirst);                       // the diagnostic dump's tail
        if(off>b0 && off<=b0+4u*(cg::Device::kHqdDumpLast-mq::kHqdFirst))return true;
        return false;
    }
    uint32_t sel(){return mem[r.g.v.q.grbm];}
    bool faultNow=false;      // this queue's own fault (the status register is sticky and shared)
    void fault(uint32_t vmid,uint64_t va){
        faultNow=true;
        if(!mem[r.g.v.faultStatus]){mem[r.g.v.faultStatus]=0x100u|(vmid<<20);mem[r.g.v.faultAddr]=uint32_t(va>>12);}
    }
    // VA in VMID k -> VRAM offset, through the TLB first.
    bool translate(uint32_t vmid,uint64_t va,uint64_t &pa,bool write=false,bool exec=false) {
        const auto &c=r.ctx[vmid-cg::kFirstVmid];
        const v1::Regs &v=r.g.v;
        if((mem[v.l1]&0x58u)!=0x58u || !(mem[v.l2]&1u) || !(mem[c[0]]&1u) || ((mem[c[0]]>>1)&3u)!=3u){fault(vmid,va);return false;}
        auto t=tlb.find({vmid,va>>12});
        if(t!=tlb.end() && !exec){pa=t->second+(va&0xfffu);return true;}
        uint64_t table=(mem[c[1]]|(uint64_t(mem[c[2]])<<32))&gv::kAddrMask;
        for(uint32_t lv=3;;--lv) {
            const uint64_t e=q(table+8u*gv::index(va,lv));
            if(!(e&1u) || (!lv && write && !(e&gv::kPteWrite)) || (!lv && exec && !(e&gv::kPteExec))){fault(vmid,va);return false;}
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
            uint64_t hp=0;
            const uint32_t h=ring ? rd(k,at(i)) : (translate(k,at(i),hp,false,true) ? get(hp) : 0xdeadu);   // IBs: execute
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
            } else if(op==gd::kReleaseMem) {
                if(eopStale.count(k)){i+=1u+n;continue;}
                eopRan.insert(k);
                const uint64_t to=b[2]|(uint64_t(b[3])<<32);wr(k,to,b[4]);
                if((b[1]>>29)==2u)wr(k,to+4u,b[5]);
            }
            else assert(!"unexpected packet");
            i+=1u+n;
        }
    }
    // The gfx pipe (G40): runs RB0 in VMID 0 from its RPTR to the WPTR just written, once PFP/ME are
    // unhalted, RB_ACTIVE is set and both pipes got their program counters.
    unsigned gfxCsb=0, gfxPcs=0, gfxCntx=0, gfxFrames=0, gfxIbs=0; bool gfxBroken=false, gfxHold=false, gfxDoorbellLost=false;
    void runGfx() {
        namespace gr=navi33::gfx_ring;
        const gr::Regs &g=r.gfx;
        if(gfxBroken || gfxHold || (g42 && !mem[r.gfxMapped]) || (mem[g.meCntl]&(gr::kPfpHalt|gr::kMeHalt|gr::kPfpReset|gr::kMeReset)) || !mem[g.rbActive] || gfxPcs<4u)return;
        const uint64_t ring=((uint64_t(mem[g.rb0BaseHi])<<32)|mem[g.rb0Base])<<8;
        assert(ring>=kFb && (mem[g.rb0Cntl]&~0x00a00000u)==gr::kRb0CntlValue);
        // Kicked by its doorbell (DOORBELL_EN, offset 0x116, the RB range covering it, the MEC range below it).
        assert(!(mem[g.rbDoorbellCntl]&gr::kDoorbellEn) || ((mem[g.rbDoorbellCntl]&gr::kDoorbellOffsetMask)>>2)==gr::kDoorbellDword);
        assert(mem[g.rbDbLower]<=gr::kDoorbellByte && mem[g.rbDbUpper]>=gr::kDoorbellByte && mem[r.mecDbUpper]<gr::kDoorbellByte);
        auto at=[&](uint64_t d){return ring-kFb+4u*(d%gr::kRingDwords);};
        for(uint64_t i=gfxRptr;i<gfxWptr;) {
            const uint32_t h=get(at(i));
            if(h==mq::kNop){++i;continue;}
            const uint32_t op=(h>>8)&0xffu,n=((h>>16)&0x3fffu)+1u;
            std::vector<uint32_t> b(n);for(uint32_t j=0;j<n;++j)b[j]=get(at(i+1u+j));
            if(op==gr::kSetContextReg || op==gr::kPreambleCntl || op==gr::kClearState)++gfxCsb;
            else if(op==gr::kContextControl){++gfxCsb;++gfxCntx;}
            else if(op==gr::kFrameControl)++gfxFrames;
            else if(op==0x3fu) {                                   // a client IB in its VMID (no VALID bit on gfx)
                const uint32_t k=b[2]>>24;assert(!(b[2]&(1u<<23)) && k>=cg::kFirstVmid && k<cg::kFirstVmid+cg::kClients);
                uint64_t pgm=0,karg=0;uint32_t masks=0;faultNow=false;
                exec(k,b[0]|(uint64_t(b[1]&0xffffu)<<32),0,b[2]&0xfffffu,false,pgm,karg,masks);++gfxIbs;
            }
            else if(op==gd::kReleaseMem){const uint64_t to=b[2]|(uint64_t(b[3])<<32);vram[to-kFb]=b[4];
                                          if((b[1]>>29)==2u)vram[to-kFb+4u]=b[5];}
            else assert(!"unexpected gfx packet");
            i+=1u+n;
        }
        gfxRptr=gfxWptr;mem[g.rb0Rptr]=uint32_t(gfxWptr%gr::kRingDwords);
    }
    uint64_t gfxWptr=0, gfxRptr=0;
    void runQueue(uint32_t k) {
        const uint32_t qs=qsOf(k);
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
        if(f.failGfxSnapshotRead && off==f.r.gfxHqdActive)return false;
        if(f.banked(off)){*v=f.bank[{f.sel(),off}];return true;}
        assert(f.mem.count(off));
        if(off==f.r.hdp.remapMem && f.corruptRemapReadback && f.hdpWrites>=2u)*v=0u;
        else *v=f.mem[off];
        if(off==f.r.hdp.memsize && f.hdpFlushSeen){f.hdpMemsizeReadAfterFlush=true;f.hdpEvents.push_back(5u);}
        return true;
    }
    static bool write(void *c,uint64_t off,uint32_t v) {
        auto &f=*static_cast<Fake *>(c);
        assert(cg::allowed(f.r,off) || (f.g42 && cg::mapWriteAllowed(f.r,off,v)) ||
               (f.g42 && cg::mapPollDisableAllowed(f.r,off,f.mem[off],v)));
        if((off==f.r.hdp.remapReg && f.failHdpRegWrite) || (off==f.r.hdp.flush && f.failHdpFlushWrite))return false;
        f.writes.emplace_back(off,v);
        if(f.banked(off))f.bank[{f.sel(),off}]=v; else f.mem[off]=v;
        if(off==f.r.hdp.remapMem){++f.hdpWrites;f.hdpEvents.push_back(1u);}
        if(off==f.r.hdp.remapReg){++f.hdpWrites;f.hdpEvents.push_back(2u);}
        if(off==f.r.hdp.flush){++f.hdpWrites;f.hdpFlushSeen=true;f.hdpEvents.push_back(4u);}
        // GCVM_L2_CNTL2 with the invalidate bits latched keeps the hub L2 busy (the card,
        // 2026-10-20 13:58), and a busy L2 serves no invalidation engine.
        if(off==f.r.gfx.rb0Wptr && !(f.mem[f.r.gfx.rbDoorbellCntl]&navi33::gfx_ring::kDoorbellEn)) {
            f.gfxWptr=v|(uint64_t(f.mem[f.r.gfx.rb0WptrHi])<<32);f.runGfx();
        }
        if(off==f.r.g.v.l2c2)f.mem[f.r.l2Status]=(v&3u) ? 1u : 0u;
        if(off==f.r.invReq) {
            uint32_t ack=0;
            for(uint32_t k=0;k<16;++k)if(v&(1u<<k)){
                const uint64_t last=f.mem[f.r.invRangeLo]|(uint64_t(f.mem[f.r.invRangeHi]&0x1fu)<<32);
                if(!f.noFlushModel)for(auto i=f.tlb.begin();i!=f.tlb.end();)
                    i=i->first.first==k && i->first.second<=last ? f.tlb.erase(i) : std::next(i);
                ack|=1u<<k;
            }
            f.mem[f.r.invAck]=f.noAck || f.mem[f.r.l2Status] ? 0u : ack;++f.flushes;
        }
        if(off==f.r.dequeue && v==cg::kDrainPipe && !f.stuck) {
            f.bank[{f.sel(),mq::hqd(f.r.g.v.q,kiq::kCpHqdActive)}]=0u;
            for(uint32_t k=cg::kFirstVmid;k<cg::kFirstVmid+cg::kClients;++k)if(qsOf(k)==f.sel()) {
                f.rptr[k]=0;                                                     // a reload starts the ring at 0
                if(f.eopRan.count(k))f.eopStale.insert(k);
            }
        }
        // GRBM select: 0, a VMID (SH_MEM), a gfx pipe (ME 0, pipe 0/1, queue 0) or a client HQD: never MES's
        // queues 2+, never pipe 0 queue 0 (G34's).
        if(off==f.r.g.v.q.grbm && v && !(v&0xf0u)) {
            if(!((v>>2)&3u))assert((v&3u)<=1u && !((v>>8)&7u));
            else assert(((v>>2)&3u)==1u && ((v>>8)&7u)<=1u && (f.g42 || (v&3u) || ((v>>8)&7u)==1u));
        }
        {   namespace gr=navi33::gfx_ring;
            if((off==f.r.gfx.pfpStart || off==f.r.gfx.meStart) && v==gr::kPcStart)++f.gfxPcs;
        }
        if(off==mq::hqd(f.r.g.v.q,mq::kEopRptr) && (v&0x80000000u))
            for(uint32_t k=cg::kFirstVmid;k<cg::kFirstVmid+cg::kClients;++k)if(qsOf(k)==f.sel())f.eopStale.erase(k);
        // Activation fetches the ring (G36): only once ACTIVE is set.
        if(off==mq::hqd(f.r.g.v.q,kiq::kCpHqdActive) && (v&1u))
            for(uint32_t k=cg::kFirstVmid;k<cg::kFirstVmid+cg::kClients;++k)if(qsOf(k)==f.sel())f.runQueue(k);
        if(off==f.r.g.v.q.poll1)for(uint32_t i=0;i<cg::kClients;++i)if(v&cg::pollBit(i))f.runQueue(cg::kFirstVmid+i);

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
    // A doorbell rings the queue whose HQD carries that offset with DOORBELL_EN, if the NBIO
    // aperture is on and the MEC range covers it; the value is the new WPTR.
    unsigned rings=0;
    static bool doorbell(void *c,uint32_t byteOff,uint64_t v) {
        auto &f=*static_cast<Fake *>(c);
        if(f.g42 && byteOff==navi33::gfx_map::kKiqDoorbellByte) {
            if(f.mapStuck)return true;               // the KIQ never fetches: no fence, no mapping
            ++f.mapTriggers;f.hdpEvents.push_back(6u);
            namespace gm=navi33::gfx_map;
            const uint64_t p=gm::kG34Vram+gm::kAppendOff;
            assert(v==gm::kEndWptr && !(f.mem[f.r.pollCntl]&gm::kPqWptrPollEn) &&
                   (f.mem[f.r.pqStatus]&gm::kPqStatusDoorbellEnable));
            assert((f.mem[f.r.nbioAperEn]&1u) && f.mem[f.r.mecDbLower]<=gm::kKiqDoorbellByte &&
                   f.mem[f.r.mecDbUpper]>=gm::kKiqDoorbellByte+8u);
            assert(f.mem[f.r.g.v.q.grbm]==0u && f.mem[f.r.hdp.remapMem]==f.r.hdp.remapBytes &&
                   f.mem[f.r.hdp.remapReg]==f.r.hdp.remapBytes+4u && f.hdpFlushSeen && f.hdpMemsizeReadAfterFlush);
            assert(f.mem[f.r.scheduler]==0x30a0u && f.get(p)==0xc005a200u && f.get(p+4)==0x30000000u);
            assert(f.get(p+28)==0xc0033700u && f.get(p+32)==0x100500u && f.get(p+44)==gm::kMapFence);
            for(uint32_t i=16u;i<gm::kAppendDwords;++i)assert(f.get(p+4u*i)==mq::kNop);
            assert(f.q(p+36)==kFb+gm::kG34Vram+gm::kMapFenceOff);
            const uint64_t mqd=f.q(p+12)-kFb;
            assert(f.get(mqd+162u*4u)==0x100u && f.get(mqd+130u*4u)==1u &&
                   f.get(mqd+143u*4u)==(navi33::gfx_ring::kDoorbellEn|(navi33::gfx_ring::kDoorbellDword<<2)));
            const uint64_t base=f.q(mqd+136u*4u);
            f.mem[f.r.gfxHqdActive]=f.get(mqd+130u*4u);f.mem[f.r.gfxHqdVmid]=f.get(mqd+131u*4u);
            f.mem[f.r.gfxHqdBase]=f.get(mqd+136u*4u);f.mem[f.r.gfxHqdBaseHi]=f.get(mqd+137u*4u);
            f.mem[f.r.gfxHqdRptr]=f.get(mqd+138u*4u);f.mem[f.r.gfxHqdCntl]=f.get(mqd+145u*4u);
            f.mem[f.r.gfxHqdWptr]=f.get(mqd+149u*4u);f.mem[f.r.gfxHqdWptrHi]=f.get(mqd+150u*4u);
            f.mem[f.r.gfx.rb0Base]=uint32_t(base);f.mem[f.r.gfx.rb0BaseHi]=uint32_t(base>>32);
            f.mem[f.r.gfx.rb0Cntl]=f.get(mqd+145u*4u);f.mem[f.r.gfx.rbActive]=1u;f.mem[f.r.gfxMapped]=1u;
            f.gfxWptr=f.q(f.q(p+20)-kFb);f.runGfx();
            f.vram[gm::kG34Vram+gm::kMapFenceOff]=gm::kMapFence;
            f.bank[{f.sel(),mq::hqd(f.r.g.v.q,kiq::kCpHqdPqRptr)}]=gm::kEndWptr;
            return true;
        }
        if(byteOff==navi33::gfx_ring::kDoorbellByte) {           // the gfx ring (RB range, not MEC's)
            if(!f.gfxDoorbellLost && (f.mem[f.r.nbioAperEn]&1u) && byteOff>=f.mem[f.r.gfx.rbDbLower] && byteOff<=f.mem[f.r.gfx.rbDbUpper]){f.gfxWptr=v;f.runGfx();}
            return true;
        }
        ++f.rings;                                                  // client queue doorbells
        if(!(f.mem[f.r.nbioAperEn]&1u) || !(f.mem[f.r.pqStatus]&cg::kPqStatusDoorbellEnable) ||
           byteOff<f.mem[f.r.mecDbLower] || byteOff>=f.mem[f.r.mecDbUpper])return true;
        for(uint32_t i=0;i<cg::kClients;++i) {
            const uint32_t k=cg::kFirstVmid+i, dc=f.bank[{qsOf(k),mq::hqd(f.r.g.v.q,kiq::kCpHqdPqDoorbellControl)}];
            if((dc&cg::kDbEn) && (dc&cg::kDbOffsetMask)==4u*byteOff/4u) {
                const uint64_t poll=f.bank[{qsOf(k),mq::hqd(f.r.g.v.q,kiq::kCpHqdPqWptrPollAddr)}]|
                    (uint64_t(f.bank[{qsOf(k),mq::hqd(f.r.g.v.q,kiq::kCpHqdPqWptrPollAddrHi)}])<<32);
                uint64_t pa=0;assert(f.translate(k,poll,pa) && f.get(pa)==uint32_t(v));   // memory and doorbell agree
                f.runQueue(k);
            }
        }
        return true;
    }
    static bool stageWrite(void *c,uint32_t off,const uint32_t *p,uint32_t n) {
        auto &f=*static_cast<Fake *>(c);assert(f.g42 && gfx_map::stageRange(off,n,true));++f.stageWrites;
        f.hdpEvents.push_back(3u);
        for(uint32_t i=0;i<n;++i)f.vram[gfx_map::kG34Vram+off+4u*i]=p[i];
        return true;
    }
    static bool stageRead(void *c,uint32_t off,uint32_t *p,uint32_t n) {
        auto &f=*static_cast<Fake *>(c);assert(f.g42 && gfx_map::stageRange(off,n,false));
        for(uint32_t i=0;i<n;++i)p[i]=f.get(gfx_map::kG34Vram+off+4u*i);
        return true;
    }
    void g34() {
        g42=true;mem[r.scheduler]=0x30a0u;mem[r.gfxMapped]=0u;
        const uint32_t qs=mq::grbmSelect(1,0,0);
        const uint64_t base=(kFb+gfx_map::kG34Vram+mq::kRingOff)>>8;
        bank[{qs,mq::hqd(r.g.v.q,kiq::kCpHqdActive)}]=1u;
        bank[{qs,mq::hqd(r.g.v.q,kiq::kCpHqdVmid)}]=0u;
        bank[{qs,mq::hqd(r.g.v.q,kiq::kCpHqdPqBase)}]=uint32_t(base);
        bank[{qs,mq::hqd(r.g.v.q,kiq::kCpHqdPqBaseHi)}]=uint32_t(base>>32);
        bank[{qs,mq::hqd(r.g.v.q,kiq::kCpHqdPqControl)}]=0xd0308509u;
        // G34's tail writes doorbell control = MQD (offset 0, EN) | EN; with the gate on that is EN alone.
        bank[{qs,mq::hqd(r.g.v.q,kiq::kCpHqdPqDoorbellControl)}]=kiq::kDoorbellEnMask;
        bank[{qs,mq::hqd(r.g.v.q,kiq::kCpHqdPqRptr)}]=16u;
        vram[gfx_map::kG34Vram+mq::kWptrOff]=16u;
        vram[gfx_map::kG34Vram+mq::kFenceOff]=mq::kFenceValue;
    }
    cg::Hw hw(){return {this,read,write,vw,vr,delay,doorbell,stageWrite,stageRead};}
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
    assert(f.dev.vaMap(a.slot,a.ib,vaBase+0x30000,false,true)==cg::Code::Ok);
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
    assert(r.valid && r.dequeue==(0x1260u+0x1fc1u)*4u && cg::allowed(r,r.g.v.l2c2));
    assert(r.gfxHqdActive==(0x1260u+gfx_map::kGfxActiveDw)*4u &&
           r.gfxHqdVmid==(0x1260u+gfx_map::kGfxVmidDw)*4u &&
           r.gfxHqdBase==(0x1260u+gfx_map::kGfxBaseDw)*4u &&
           r.gfxHqdBaseHi==(0x1260u+gfx_map::kGfxBaseHiDw)*4u &&
           r.gfxHqdRptr==(0x1260u+gfx_map::kGfxRptrDw)*4u &&
           r.gfxHqdCntl==(0x1260u+gfx_map::kGfxCntlDw)*4u &&
           r.gfxHqdWptr==(0x1260u+gfx_map::kGfxWptrDw)*4u &&
           r.gfxHqdWptrHi==(0x1260u+gfx_map::kGfxWptrHiDw)*4u &&
           r.pollCntl==(0x1260u+gfx_map::kPqWptrPollCntlDw)*4u);
    for(uint64_t o:{r.gfxHqdActive,r.gfxHqdVmid,r.gfxHqdBase,r.gfxHqdBaseHi,r.gfxHqdRptr,
                    r.gfxHqdCntl,r.gfxHqdWptr,r.gfxHqdWptrHi})
        assert(!cg::allowed(r,o) && !cg::mapAllowed(r,o));
    assert(r.hdp.valid() && r.hdp.remapMem==0x3934u && r.hdp.remapReg==0x3938u &&
           r.hdp.flush==0x7f000u && r.hdp.memsize==0x378cu && r.hdp.remapBytes==0x7f000u);
    assert(!cg::allowed(r,r.hdp.remapMem) && cg::mapAllowed(r,r.hdp.remapMem) &&
           cg::mapWriteAllowed(r,r.hdp.remapMem,r.hdp.remapBytes) &&
           !cg::mapWriteAllowed(r,r.hdp.remapMem,r.hdp.remapBytes+4u) &&
           cg::mapWriteAllowed(r,r.hdp.flush,0u) && !cg::mapWriteAllowed(r,r.hdp.flush,1u));
    assert(!cg::allowed(r,r.pollCntl) && cg::mapPollDisableAllowed(r,r.pollCntl,0x80000123u,0x123u) &&
           !cg::mapPollDisableAllowed(r,r.pollCntl,0x123u,0x123u) &&
           !cg::mapPollDisableAllowed(r,r.pollCntl,0x80000123u,0x80000123u) &&
           !cg::mapPollDisableAllowed(r,r.pollCntl+4u,0x80000123u,0x123u));
    assert(!gfx_map::resolveHdp(d,rx7600_fixture::kBar5,8192u).valid());
    assert(!gfx_map::resolveHdp(d,0x7f000u,4096u).valid());
    {auto bad=rx7600_fixture::blob(2u,0u,1u);IPDiscovery x;assert(x.parse(bad.data(),bad.size()));
     assert(gfx_map::resolveHdp(x,rx7600_fixture::kBar5,4096u).code==gfx_map::HdpCode::UnsupportedNbif);}
    {auto bad=rx7600_fixture::blob(2u,1u,0u);IPDiscovery x;assert(x.parse(bad.data(),bad.size()));
     assert(gfx_map::resolveHdp(x,rx7600_fixture::kBar5,4096u).code==gfx_map::HdpCode::UnsupportedHdp);}
    // Of the invalidation engines only engine 17's REQ and address range are written (never a SEM or ACK).
    assert(r.invReq==(0x1260u+0x16bcu)*4u && r.invAck==(0x1260u+0x16ceu)*4u && r.l2Status==(0x1260u+0x15bfu)*4u);
    for(uint32_t dw=0x1699u;dw<=0x16f2u;++dw)assert(cg::allowed(r,(0x1260u+dw)*4u)==(dw==0x16bcu || dw==0x16f1u || dw==0x16f2u));
    assert(r.ctx[0][0]==(0x1260u+0x1690u)*4u && r.ctx[5][6]==(0x1260u+0x174eu)*4u && r.gbAddrConfig==0x263eu*4u);
    assert(!cg::allowed(r,r.gbAddrConfig));
    // Never context 0/1, SCRATCH, the MEC controls or the fault registers.
    for(uint64_t o:{r.g.v.ctx,r.g.v.baseLo,r.g.v.endHi,r.g.v.q.scratch,r.g.v.q.cntl,r.g.v.q.pc,r.g.v.faultStatus})
        assert(!cg::allowed(r,o));
    // 1. Two clients, each its own VMID, queue, buffers and image.
    {   Fake f(r);
        {uint32_t none=0;assert(f.dev.open(&f,none)==cg::Code::NotReady);}
        assert(f.dev.start(r,f.hw(),f.heap,kFb)==cg::Code::Ok);
        assert(f.mem[r.g.v.l1]==0x1859u && f.writes.size()==17u+12u+14u+kGfxStartWrites);   // hub, HQD ACTIVE checks, HQD dumps (selects), G40
        assert(f.mem[r.invRangeLo]==0xffffffffu && f.mem[r.invRangeHi]==0x1fu);
        // Doorbells: MEC range 0..0x450 bytes, NBIO aperture bit 0 set, the rest of the word kept.
        assert(f.mem[r.mecDbLower]==0u && f.mem[r.mecDbUpper]==0x450u && f.mem[r.nbioAperEn]==0x101u);
        {   const cg::DoorbellSetup db=f.dev.stats().db;
            assert(db.aperBefore==0x100u && db.lowerBefore==0x1234u && db.upperBefore==0u && db.aper==0x101u && db.upper==0x450u);}
        assert(cg::allowed(r,r.nbioAperEn) && r.nbioAperEn==(0xd20u+0xc0u)*4u && r.mecDbLower==(0x1260u+0x1dfcu)*4u);
        App a=setup(f,co,k,7,0x100000000ull), c=setup(f,co,k,99,0x100000000ull);   // same VAs, different VMIDs
        assert(a.slot==0 && c.slot==1 && f.dev.client(0)->vmid==8u && f.dev.client(1)->vmid==9u);
        uint64_t sa=0,sc=0,ret=0;
        const cg::Device::Ib ia{0x100030000ull,ibDwords(k)};
        assert(f.dev.submit(a.slot,&ia,1,sa)==cg::Code::Ok && sa==1u);
        assert(f.dev.submit(c.slot,&ia,1,sc)==cg::Code::Ok && sc==1u);
        assert(f.dev.wait(a.slot,sa,1000,ret)==cg::Code::Ok && ret==1u && f.dev.wait(c.slot,sc,1000,ret)==cg::Code::Ok);
        assert(imageOk(f,a,7) && imageOk(f,c,99) && f.dispatches==2);
        assert(f.dev.client(0)->loaded && (f.mem[r.pqStatus]&cg::kPqStatusDoorbellEnable) && f.rings==0u);   // loaded with work, no kick yet
        // VMID k's context: depth 3 at the client's root; SH_MEM for VMID k only.
        assert(f.mem[r.ctx[0][0]]==0x01fffe07u && (f.mem[r.ctx[0][1]]&~0xfffu)==f.dev.client(0)->space.rootVram());
        assert((f.bank[{8u<<4,r.g.shMem}]==gr::kShMemDefault) && (f.bank[{0u,r.g.shMem}]==0u));
        // An IB must sit in an executable mapping (the CP fetches it as code).
        {   uint32_t ro=0;uint64_t vro=0,sq=0;
            assert(f.dev.boCreate(c.slot,4096,ro,vro)==cg::Code::Ok && f.dev.vaMap(c.slot,ro,0x300000000ull,false,false)==cg::Code::Ok);
            const cg::Device::Ib nx{0x300000000ull,8};assert(f.dev.submit(c.slot,&nx,1,sq)==cg::Code::BadArgs);}
        // Isolation: client c cannot submit an IB at a VA it never mapped.
        const cg::Device::Ib far{0x200000000ull,4};
        uint64_t s=0;assert(f.dev.submit(c.slot,&far,1,s)==cg::Code::BadArgs);
        // Many submissions: the ring wraps, sequence numbers keep counting.
        for(int i=0;i<300;++i){assert(f.dev.submit(a.slot,&ia,1,s)==cg::Code::Ok);assert(f.dev.wait(a.slot,s,1000,ret)==cg::Code::Ok);}
        assert(s==301u && f.dev.client(0)->wptr>cg::kRingDwords && imageOk(f,a,7));
        // Back-pressure: a paused queue fills its ring and the next submission is Busy, not an overwrite.
        f.paused.insert(cg::kFirstVmid+1u);
        cg::Code code=cg::Code::Ok;int n=0;
        while((code=f.dev.submit(c.slot,&ia,1,s))==cg::Code::Ok)++n;
        assert(code==cg::Code::Busy && n>0 && n<64);
        assert(f.dev.wait(c.slot,s,500,ret)==cg::Code::Timeout);
        {   const cg::QueueSnap &sn=f.dev.client(c.slot)->snap;     // the timeout snapshot
            assert(sn.taken && sn.reg[0]==1u && sn.wptrMem==uint32_t(f.dev.client(c.slot)->wptr) && sn.fence<s);
            assert(f.mem[r.g.v.q.grbm]==0u);}
        f.paused.clear();f.runQueue(cg::kFirstVmid+1u);              // the queue catches up
        assert(f.dev.wait(c.slot,s,1000,ret)==cg::Code::Ok && ret==s);
        // Close: drain, dequeue, disable context, flush, give back every page.
        assert(f.dev.close(c.slot)==cg::Code::Ok && f.dev.close(a.slot)==cg::Code::Ok);
        assert((f.bank[{qsOf(cg::kFirstVmid),mq::hqd(r.g.v.q,kiq::kCpHqdActive)}]==0u && !(f.mem[r.ctx[0][0]]&1u)));
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
        assert(f.dev.vaMap(slot,x,0x500000,true,false)==cg::Code::Ok && f.dev.vaMap(slot,ib,0x600000,false,true)==cg::Code::Ok);
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
        assert(f.dev.vaMap(slot,x,0x500000,true,false)==cg::Code::Ok && f.dev.vaMap(slot,ib,0x600000,false,true)==cg::Code::Ok);
        f.put(slot,ib,0,{gd::packet3(gd::kReleaseMem,6u),gd::kReleaseMemControl,gd::kReleaseMemDataSel32,0x500000u,0u,0x1111u,0u,0u});
        const cg::Device::Ib i1{0x600000,8};
        assert(f.dev.submit(slot,&i1,1,s)==cg::Code::Ok && f.dev.wait(slot,s,1000,ret)==cg::Code::Ok && f.get(vx)==0x1111u);
        assert((viaFree ? f.dev.boFree(slot,x) : f.dev.vaUnmap(slot,x))==cg::Code::Ok);
        f.put(slot,ib,0,{gd::packet3(gd::kReleaseMem,6u),gd::kReleaseMemControl,gd::kReleaseMemDataSel32,0x500000u,0u,0x2222u,0u,0u});
        assert(f.dev.submit(slot,&i1,1,s)==cg::Code::Ok && f.dev.wait(slot,s,1000,ret)==cg::Code::Faulted);
        assert(f.get(vx)==0x1111u);
    }
    // 3d. A buffer the CPU mapped is never given back to the heap (a user mapping may outlive it).
    {   Fake f(r);assert(f.dev.start(r,f.hw(),f.heap,kFb)==cg::Code::Ok);
        uint32_t slot=0,x=0,y=0;uint64_t vx=0,vy=0;
        assert(f.dev.open(&f,slot)==cg::Code::Ok && f.dev.boCreate(slot,8192,x,vx)==cg::Code::Ok &&
               f.dev.boCreate(slot,4096,y,vy)==cg::Code::Ok && f.dev.markCpuMapped(slot,x));
        assert(f.dev.boFree(slot,x)==cg::Code::Ok && f.heap.covers(vx,8192) && f.dev.stats().retiredBytes==8192u);
        assert(f.dev.boFree(slot,y)==cg::Code::Ok && !f.heap.covers(vy,4096));
        uint32_t z=0;uint64_t vz=0;assert(f.dev.boCreate(slot,8192,z,vz)==cg::Code::Ok && vz!=vx);
        assert(f.dev.markCpuMapped(slot,z) && f.dev.close(slot)==cg::Code::Ok && f.heap.covers(vz,8192));
    }
    // 3e. Once the user client revoked the CPU mappings the pages are reusable again.
    {   Fake f(r);assert(f.dev.start(r,f.hw(),f.heap,kFb)==cg::Code::Ok);
        uint32_t slot=0,x=0,y=0;uint64_t vx=0,vy=0;
        assert(f.dev.open(&f,slot)==cg::Code::Ok && f.dev.boCreate(slot,8192,x,vx)==cg::Code::Ok &&
               f.dev.boCreate(slot,4096,y,vy)==cg::Code::Ok && f.dev.markCpuMapped(slot,x) && f.dev.markCpuMapped(slot,y));
        assert(f.dev.clearCpuMapped(slot,x) && f.dev.boFree(slot,x)==cg::Code::Ok && !f.heap.covers(vx,8192));
        assert(f.dev.stats().retiredBytes==0u && !f.dev.clearCpuMapped(slot,x));
        const uint64_t before=f.heap.usedBytes(cg::kTagClient);
        assert(f.dev.clearCpuMapped(slot,0) && f.dev.close(slot)==cg::Code::Ok && !f.heap.covers(vy,4096) && before>0);
        assert(f.heap.usedBytes(cg::kTagClient)==0u && f.dev.stats().retiredBytes==0u);
    }
    // 3c. Close with work still queued waits; if the queue never drains, nothing is freed.
    {   Fake f(r);assert(f.dev.start(r,f.hw(),f.heap,kFb)==cg::Code::Ok);
        App a=setup(f,co,k,7,0x100000000ull);uint64_t s=0;
        const cg::Device::Ib ia{0x100030000ull,ibDwords(k)};
        f.paused.insert(cg::kFirstVmid);
        assert(f.dev.submit(a.slot,&ia,1,s)==cg::Code::Ok);
        const uint64_t used=f.heap.usedBytes(cg::kTagClient);
        assert(f.dev.close(a.slot)==cg::Code::Quarantined && f.heap.usedBytes(cg::kTagClient)==used);
        assert((f.bank[{qsOf(cg::kFirstVmid),mq::hqd(r.g.v.q,kiq::kCpHqdActive)}]==1u));   // never dequeued under work
    }
    // 3f. The user fence (cs_chunk_fence): the 64-bit sequence lands in the user's buffer;
    //     a fence outside a writeable mapping is refused; a GPU null pointer faults.
    {   Fake f(r);assert(f.dev.start(r,f.hw(),f.heap,kFb)==cg::Code::Ok);
        App a=setup(f,co,k,7,0x7ffff0000000ull);uint64_t s=0,ret=0,va=0,bytes=0;
        const cg::Device::Ib ia{0x7ffff0030000ull,ibDwords(k)};
        assert(f.dev.boMapping(a.slot,a.args,va,bytes) && va==0x7ffff0010000ull);
        for(int i=0;i<3;++i)assert(f.dev.submit(a.slot,&ia,1,s,va+0x800u)==cg::Code::Ok);
        assert(f.dev.wait(a.slot,s,1000,ret)==cg::Code::Ok && s==3u);
        uint64_t base=0;assert(f.dev.boRange(a.slot,a.args,base,bytes) && f.q(base+0x800u)==3u);
        assert(f.dev.submit(a.slot,&ia,1,s,va+0x804u)==cg::Code::BadArgs);                  // alignment
        assert(f.dev.submit(a.slot,&ia,1,s,0x7ffff0030000ull)==cg::Code::BadArgs);           // read-only IB buffer
        uint64_t em=0,re=0;assert(f.dev.sequences(a.slot,em,re) && em==3u && re==3u);
        uint32_t nul=0;uint64_t vn=0;
        assert(f.dev.boCreate(a.slot,4096,nul,vn)==cg::Code::Ok && f.dev.vaMap(a.slot,nul,0x10000,false,true)==cg::Code::Ok);
        f.put(a.slot,nul,0,{gd::packet3(gd::kReleaseMem,6u),gd::kReleaseMemControl,gd::kReleaseMemDataSel32,0u,0u,1u,0u,0u});
        const cg::Device::Ib in{0x10000,8};
        assert(f.dev.submit(a.slot,&in,1,s)==cg::Code::Ok && f.dev.wait(a.slot,s,1000,ret)==cg::Code::Faulted);
    }
    // 4. A fault in the client's VMID ends that client (not the others); it can still be torn down.
    {   Fake f(r);assert(f.dev.start(r,f.hw(),f.heap,kFb)==cg::Code::Ok);
        App good=setup(f,co,k,5,0x100000000ull);
        uint32_t slot=0,ib=0;uint64_t vi=0,s=0,ret=0;
        assert(f.dev.open(&f,slot)==cg::Code::Ok && f.dev.boCreate(slot,4096,ib,vi)==cg::Code::Ok &&
               f.dev.vaMap(slot,ib,0x700000,false,true)==cg::Code::Ok);
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
        uint64_t s=0,ret=0;const cg::Device::Ib ia{0x100030000ull,ibDwords(k)};
        assert(f.dev.submit(a.slot,&ia,1,s)==cg::Code::Ok && f.dev.wait(a.slot,s,1000,ret)==cg::Code::Ok);   // loads the HQD
        const uint64_t used=f.heap.usedBytes(cg::kTagClient);
        f.stuck=true;
        assert(f.dev.close(a.slot)==cg::Code::Quarantined && f.heap.usedBytes(cg::kTagClient)==used);
        assert(f.dev.submit(a.slot,&ia,1,s)==cg::Code::Quarantined);
        uint32_t slot=0;assert(f.dev.open(&f,slot)==cg::Code::Ok && slot==1u);   // the slot stays taken
    }
    // 5a. Close issues no invalidation (nothing runs in the VMID once dequeued); the next
    //     open of the slot invalidates before enabling the context, and the new client's work
    //     lands in its own buffers even at the same VAs. Unmap invalidates once.
    {   Fake f(r);assert(f.dev.start(r,f.hw(),f.heap,kFb)==cg::Code::Ok);
        const unsigned f0=f.flushes;                                               // the hub setup's own
        App a=setup(f,co,k,7,0x100000000ull);uint64_t s=0,ret=0;
        assert(f.flushes==f0+1u);                                                  // open
        const cg::Device::Ib ia{0x100030000ull,ibDwords(k)};
        assert(f.dev.submit(a.slot,&ia,1,s)==cg::Code::Ok && f.dev.wait(a.slot,s,1000,ret)==cg::Code::Ok && imageOk(f,a,7));
        assert(f.dev.close(a.slot)==cg::Code::Ok && f.flushes==f0+1u);
        App b=setup(f,co,k,9,0x100000000ull);assert(b.slot==a.slot && f.flushes==f0+2u);
        assert(f.dev.submit(b.slot,&ia,1,s)==cg::Code::Ok && f.dev.wait(b.slot,s,1000,ret)==cg::Code::Ok && imageOk(f,b,9));
        uint32_t x=0;uint64_t vx=0;
        assert(f.dev.boCreate(b.slot,4096,x,vx)==cg::Code::Ok && f.dev.vaMap(b.slot,x,0x500000,true,false)==cg::Code::Ok);
        assert(f.flushes==f0+2u && f.dev.boFree(b.slot,x)==cg::Code::Ok && f.flushes==f0+3u && f.dev.stats().tlbFlushes==3u);
    }
    // 5c'. The HQD dump reads the whole block of every client-capable HQD but G34's, writing nothing but GRBM selects.
    {   Fake f(r);assert(f.dev.start(r,f.hw(),f.heap,kFb)==cg::Code::Ok);
        f.bank[{qsOf(cg::kFirstVmid),mq::hqd(r.g.v.q,0x1fd2u)}]=0x1f70000u;
        uint32_t w[cg::Device::kHqdDumpCount];const size_t before=f.writes.size();
        assert(!f.dev.hqdDump(0,w) && f.dev.hqdAtStart(0)==nullptr);
        assert(f.dev.hqdDump(1,w) && w[0x1fd2u-0x1fa9u]==0x1f70000u && f.dev.hqdAtStart(1) && f.dev.hqdAtStart(1)[0x1fd2u-0x1fa9u]==0u);
        for(size_t i=before;i<f.writes.size();++i)assert(f.writes[i].first==r.g.v.q.grbm);
        assert(f.mem[r.g.v.q.grbm]==0u);
    }
    // 5d. A hub left with GCVM_L2_CNTL2's invalidate bits latched (L2 busy, no ACK) is released at
    //     start; a client then opens with a VMID flush that ACKs.
    {   Fake f(r);f.mem[r.g.v.l1]=0x1859u;f.mem[r.g.v.l2]=0x00080e01u;f.mem[r.g.v.l2c2]=3u;f.mem[r.l2Status]=1u;
        f.mem[r.mecDbLower]=0x110u;f.mem[r.mecDbUpper]=0xffcu;f.mem[r.nbioAperEn]=1u;
        assert(f.dev.start(r,f.hw(),f.heap,kFb)==cg::Code::Ok);
        const cg::HubL2 l2=f.dev.stats().l2;
        assert(l2.cntl2Before==3u && l2.statusBefore==1u && l2.cntl2==0u && l2.status==0u && f.mem[r.g.v.l2c2]==0u);
        uint32_t slot=0;assert(f.dev.open(&f,slot)==cg::Code::Ok && f.dev.stats().tlbFlushes==1u && f.dev.stats().miss.misses==0u);
        // With the bits still latched every flush misses and the open fails closed.
        Fake g(r);assert(g.dev.start(r,g.hw(),g.heap,kFb)==cg::Code::Ok);g.mem[r.g.v.l2c2]=3u;g.mem[r.l2Status]=1u;
        assert(g.dev.open(&g,slot)!=cg::Code::Ok && g.dev.stats().miss.misses==1u && g.dev.stats().miss.vmid==8u);
        // The diagnostic switch records the miss and goes on.
        Fake u(r);assert(u.dev.start(r,u.hw(),u.heap,kFb)==cg::Code::Ok);u.mem[r.g.v.l2c2]=3u;u.mem[r.l2Status]=1u;
        u.dev.setTlbUnsafe(true);
        assert(u.dev.open(&u,slot)==cg::Code::Ok && u.dev.stats().miss.misses==1u && u.dev.stats().tlbFlushes==0u);
    }
    // 5e. The gfx pipe (G40): PFP/ME program counters on both gfx pipes, RB0 in VMID 0, unhalt, the
    //     clear-state preamble and a ring-test fence that lands.
    {   namespace gr=navi33::gfx_ring;
        Fake f(r);assert(f.dev.start(r,f.hw(),f.heap,kFb)==cg::Code::Ok);
        const cg::GfxReport g=f.dev.stats().gfx;
        assert(g.state==cg::GfxState::Ok && g.fence==gr::kFenceValue && g.meBefore==gr::kMeCntlDefault);
        assert(!(g.me&(gr::kPfpHalt|gr::kMeHalt)) && g.rptr==g.wptr && g.wptr>=navi33::gfx11_clear_state::kCsbDwords && !(g.wptr%8u));
        assert(f.gfxCsb==1u+1u+navi33::gfx11_clear_state::kExtentCount+1u+1u+1u && f.gfxPcs==4u);   // preamble, context control, extents, steering, preamble end, clear state
        assert(f.mem[r.gfx.maxContext]==7u && f.mem[r.gfx.deviceId]==1u && f.mem[r.gfx.rbActive]==1u && f.mem[r.g.v.q.grbm]==0u);
        assert(f.heap.usedBytes(cg::kTagGfxRing)==gr::kMemBytes);
        // The card's power-on CP_ME_CNTL (0x153c0000: halted, all four pipe resets set): released and running.
        Fake c(r);c.mem[r.gfx.meCntl]=0x153c0000u;assert(c.dev.start(r,c.hw(),c.heap,kFb)==cg::Code::Ok);
        assert(c.dev.stats().gfx.state==cg::GfxState::Ok && !(c.mem[r.gfx.meCntl]&(gr::kPfpReset|gr::kMeReset|gr::kPfpHalt|gr::kMeHalt)));
        // A pipe that never runs leaves the state at Timeout and compute untouched.
        auto lost=std::make_unique<Fake>(r);lost->gfxDoorbellLost=true;
        assert(lost->dev.start(r,lost->hw(),lost->heap,kFb)==cg::Code::Ok);
        assert(lost->dev.gfxReady() && lost->dev.stats().gfx.mmioKick && lost->gfxCsb>0u);
        lost=std::make_unique<Fake>(r);lost->gfxBroken=true;lost->mem[r.gfx.rb0Rptr]=4u;
        assert(lost->dev.start(r,lost->hw(),lost->heap,kFb)==cg::Code::Ok);
        assert(lost->dev.stats().gfx.state==cg::GfxState::Timeout && !lost->dev.stats().gfx.mmioKick);
        Fake h(r);h.gfxBroken=true;assert(h.dev.start(r,h.hw(),h.heap,kFb)==cg::Code::Ok && h.dev.stats().gfx.state==cg::GfxState::Timeout);
        App a=setup(h,co,k,7,0x100000000ull);uint64_t s=0,ret=0;const cg::Device::Ib ia{0x100030000ull,ibDwords(k)};
        assert(h.dev.submit(a.slot,&ia,1,s)==cg::Code::Ok && h.dev.wait(a.slot,s,1000,ret)==cg::Code::Ok && imageOk(h,a,7));
    }
    // G42: unmapped gfx cannot run. A validated G34 queue maps it once; malformed
    // ownership/pointers refuse before any stage-window or scheduler write.
    {
        assert(!gfx_map::stageRange(mq::kFenceOff,1u,true));
        assert(gfx_map::stageRange(gfx_map::kAppendOff,gfx_map::kAppendDwords,true));
        assert(!gfx_map::stageRange(gfx_map::kAppendOff,16u,true));
        assert(!gfx_map::stageRange(gfx_map::kAppendOff,gfx_map::kAppendDwords+1u,true));
        assert(!gfx_map::stageRange(gfx_map::kAppendOff-4u,gfx_map::kAppendDwords,true));
        assert(!cg::allowed(r,r.scheduler) && !cg::mapAllowed(r,r.scheduler) && !cg::mapAllowed(r,r.gfxMapped));
        auto f=std::make_unique<Fake>(r);f->g34();
        assert(f->dev.start(r,f->hw(),f->heap,kFb)==cg::Code::Ok && !f->dev.gfxReady() && !f->stageWrites);
        assert(f->mem[r.scheduler]==0x30a0u);
        f=std::make_unique<Fake>(r);f->g34();f->dev.setGfxMap(true);
        const auto candidateStart=f->dev.start(r,f->hw(),f->heap,kFb);
        if(candidateStart!=cg::Code::Ok || !f->dev.gfxReady())std::fprintf(stderr,"G42 debug start=%u state=%u poll=%x/%x pq=%x/%x hdp=%u map=%x fence=%x trigger=%u stages=%u\n",unsigned(candidateStart),unsigned(f->dev.gfxMapReport().state),f->dev.gfxMapReport().pollCntlBefore,f->dev.gfxMapReport().pollCntlAfter,f->dev.gfxMapReport().pqStatusBefore,f->dev.gfxMapReport().pqStatusAfter,unsigned(f->dev.gfxMapReport().hdp.state),f->dev.gfxMapReport().mapped,f->dev.gfxMapReport().fence,f->mapTriggers,f->stageWrites);
        assert(candidateStart==cg::Code::Ok && f->dev.gfxReady());
        assert(f->dev.gfxMapReport().state==gfx_map::State::Mapped && f->stageWrites==3u && f->hdpWrites==3u);
        assert(f->dev.gfxMapReport().hdp.state==gfx_map::HdpState::Complete && f->dev.gfxMapReport().hdp.writes==3u &&
               f->dev.gfxMapReport().hdp.remapMemAfter==0x7f000u && f->dev.gfxMapReport().hdp.remapRegAfter==0x7f004u &&
               f->dev.gfxMapReport().hdp.memsize==8192u);
        assert(f->dev.gfxMapReport().gfxAfter.valid && f->dev.gfxMapReport().gfxAfter.active==1u &&
               f->dev.gfxMapReport().gfxAfter.vmid==0u && f->dev.gfxMapReport().gfxAfter.cntl==0x20a0090bu);
        assert((f->hdpEvents==std::vector<unsigned>{1u,2u,3u,3u,3u,4u,5u,5u,6u}));
        const auto count=f->writes.size();assert(f->dev.start(r,f->hw(),f->heap,kFb)==cg::Code::Ok && f->writes.size()==count);
        f=std::make_unique<Fake>(r);f->g34();f->mem[r.hdp.remapMem]=0x7f000u;f->mem[r.hdp.remapReg]=0x7f004u;f->dev.setGfxMap(true);
        assert(f->dev.start(r,f->hw(),f->heap,kFb)==cg::Code::Ok && f->dev.gfxReady() && f->hdpWrites==1u);
        assert(f->dev.gfxMapReport().hdp.state==gfx_map::HdpState::Complete && f->dev.gfxMapReport().hdp.writes==1u);
        assert((f->hdpEvents==std::vector<unsigned>{3u,3u,3u,4u,5u,5u,6u}));
        f=std::make_unique<Fake>(r);f->g34();f->mem[r.hdp.remapMem]=0x7f000u;f->mem[r.hdp.remapReg]=0x111u;f->dev.setGfxMap(true);
        assert(f->dev.start(r,f->hw(),f->heap,kFb)==cg::Code::Ok && !f->dev.gfxReady());
        assert(f->dev.gfxMapReport().hdp.state==gfx_map::HdpState::UnexpectedRemap && f->hdpWrites==0u && f->stageWrites==0u);
        // The exact power-on pair is overwritten like a reset pair; a near miss still refuses.
        f=std::make_unique<Fake>(r);f->g34();f->mem[r.hdp.remapMem]=0x385cu;f->mem[r.hdp.remapReg]=0x3858u;f->dev.setGfxMap(true);
        assert(f->dev.start(r,f->hw(),f->heap,kFb)==cg::Code::Ok && f->dev.gfxReady() && f->hdpWrites==3u);
        assert(f->dev.gfxMapReport().hdp.state==gfx_map::HdpState::Complete && f->dev.gfxMapReport().hdp.writes==3u &&
               f->dev.gfxMapReport().hdp.remapMemBefore==0x385cu && f->dev.gfxMapReport().hdp.remapRegBefore==0x3858u &&
               f->dev.gfxMapReport().hdp.remapMemAfter==0x7f000u && f->dev.gfxMapReport().hdp.remapRegAfter==0x7f004u);
        f=std::make_unique<Fake>(r);f->g34();f->mem[r.hdp.remapMem]=0x385cu;f->mem[r.hdp.remapReg]=0x3854u;f->dev.setGfxMap(true);
        assert(f->dev.start(r,f->hw(),f->heap,kFb)==cg::Code::Ok && !f->dev.gfxReady());
        assert(f->dev.gfxMapReport().hdp.state==gfx_map::HdpState::UnexpectedRemap && f->hdpWrites==0u && f->stageWrites==0u);
        f=std::make_unique<Fake>(r);f->g34();f->dev.setGfxMap(true);f->corruptRemapReadback=true;
        assert(f->dev.start(r,f->hw(),f->heap,kFb)==cg::Code::Ok && !f->dev.gfxReady());
        assert(f->dev.gfxMapReport().state==gfx_map::State::HdpFailed &&
               f->dev.gfxMapReport().hdp.state==gfx_map::HdpState::RemapReadbackFailed && f->hdpWrites==2u && !f->stageWrites && !f->mapTriggers);
        f=std::make_unique<Fake>(r);f->g34();f->dev.setGfxMap(true);f->failHdpRegWrite=true;
        assert(f->dev.start(r,f->hw(),f->heap,kFb)==cg::Code::Ok && !f->dev.gfxReady());
        assert(f->dev.gfxMapReport().state==gfx_map::State::HdpFailed &&
               f->dev.gfxMapReport().hdp.state==gfx_map::HdpState::RemapWriteFailed &&
               f->dev.gfxMapReport().hdp.writes==1u && f->hdpWrites==1u && !f->stageWrites && !f->mapTriggers);
        f=std::make_unique<Fake>(r);f->g34();f->dev.setGfxMap(true);f->mem[r.hdp.memsize]=0xffffffffu;
        assert(f->dev.start(r,f->hw(),f->heap,kFb)==cg::Code::Ok && !f->dev.gfxReady());
        assert(f->dev.gfxMapReport().state==gfx_map::State::HdpFailed &&
               f->dev.gfxMapReport().hdp.state==gfx_map::HdpState::FlushReadbackFailed && f->hdpWrites==3u && !f->mapTriggers);
        f=std::make_unique<Fake>(r);f->g34();f->dev.setGfxMap(true);f->failHdpFlushWrite=true;
        assert(f->dev.start(r,f->hw(),f->heap,kFb)==cg::Code::Ok && !f->dev.gfxReady());
        assert(f->dev.gfxMapReport().state==gfx_map::State::HdpFailed &&
               f->dev.gfxMapReport().hdp.state==gfx_map::HdpState::FlushWriteFailed && f->hdpWrites==2u && !f->mapTriggers);
        f=std::make_unique<Fake>(r);f->g34();f->dev.setGfxMap(true);
        f->vram[gfx_map::kG34Vram+mq::kWptrOff]=32u;
        assert(f->dev.start(r,f->hw(),f->heap,kFb)==cg::Code::Ok && !f->dev.gfxReady());
        assert(f->dev.gfxMapReport().state==gfx_map::State::Refused && f->stageWrites==0u && f->mem[r.scheduler]==0x30a0u);
        f=std::make_unique<Fake>(r);f->g34();f->mapStuck=true;f->dev.setGfxMap(true);
        assert(f->dev.start(r,f->hw(),f->heap,kFb)==cg::Code::Ok && !f->dev.gfxReady());
        assert(f->dev.gfxMapReport().state==gfx_map::State::MapTimeout && f->stageWrites==3u);
        const size_t mapStuckWrites=f->writes.size();
        f=std::make_unique<Fake>(r);f->g34();f->mapStuck=true;f->failGfxSnapshotRead=true;f->dev.setGfxMap(true);
        assert(f->dev.start(r,f->hw(),f->heap,kFb)==cg::Code::Ok && !f->dev.gfxReady());
        assert(f->dev.gfxMapReport().state==gfx_map::State::MapTimeout && !f->dev.gfxMapReport().gfxAfter.valid &&
               f->writes.size()==mapStuckWrites);
    }
    // 5b. A queue left active by an earlier boot is skipped and never reused.
    {   Fake f(r);assert(f.dev.start(r,f.hw(),f.heap,kFb)==cg::Code::Ok);
        f.bank[{qsOf(cg::kFirstVmid),mq::hqd(r.g.v.q,kiq::kCpHqdActive)}]=1u;
        uint32_t slot=9;assert(f.dev.open(&f,slot)==cg::Code::Ok && slot==1u && f.dev.client(1)->vmid==9u);
        assert(f.dev.client(0)->state==cg::State::Quarantined && f.mem[r.g.v.q.grbm]==0u);
    }
    // 5c. An earlier boot's stuck client queues are drained and dequeued at start; one that
    //     will not go is retired. DOORBELL_ENABLE is set once a queue is loaded.
    {   Fake f(r);
        f.bank[{qsOf(cg::kFirstVmid),mq::hqd(r.g.v.q,kiq::kCpHqdActive)}]=1u;f.bank[{qsOf(cg::kFirstVmid+3u),mq::hqd(r.g.v.q,kiq::kCpHqdActive)}]=1u;
        assert(f.dev.start(r,f.hw(),f.heap,kFb)==cg::Code::Ok && f.dev.stats().db.reclaimed==2u);
        assert((f.bank[{qsOf(cg::kFirstVmid),mq::hqd(r.g.v.q,kiq::kCpHqdActive)}]==0u && f.mem[r.g.v.q.grbm]==0u));
        uint32_t slot=9;assert(f.dev.open(&f,slot)==cg::Code::Ok && slot==0u && !f.dev.client(0)->loaded);
        Fake g(r);g.stuck=true;g.bank[{qsOf(cg::kFirstVmid+1u),mq::hqd(r.g.v.q,kiq::kCpHqdActive)}]=1u;
        assert(g.dev.start(r,g.hw(),g.heap,kFb)==cg::Code::Ok && g.dev.stats().db.reclaimed==0u &&
               g.dev.client(1)->state==cg::State::Quarantined);
    }
    // 6. Arguments and a hub this code does not recognise.
    {   Fake f(r);assert(f.dev.start(r,f.hw(),f.heap,kFb)==cg::Code::Ok);
        uint32_t slot=0,x=0;uint64_t vx=0;assert(f.dev.open(&f,slot)==cg::Code::Ok);
        assert(f.dev.boCreate(slot,0,x,vx)==cg::Code::BadArgs && f.dev.boCreate(slot,(64ull<<20)+1,x,vx)==cg::Code::BadArgs);
        assert(f.dev.boCreate(slot,5000,x,vx)==cg::Code::Ok);
        for(uint64_t va:std::vector<uint64_t>{0x0ull,0x1000ull,0x500001ull,cg::kUserVaMax,0xf000ull,cg::kUserVaMax-4096u})assert(f.dev.vaMap(slot,x,va,true,false)==cg::Code::BadArgs);
        assert(f.dev.vaMap(slot,x,0x500000,true,false)==cg::Code::Ok && f.dev.vaMap(slot,x,0x900000,true,false)==cg::Code::BadArgs);
        uint32_t y=0;assert(f.dev.boCreate(slot,4096,y,vx)==cg::Code::Ok && f.dev.vaMap(slot,y,0x501000,true,false)==cg::Code::BadArgs);
        {   const cg::Client *cl=f.dev.client(slot);                     // the queue sits at its own MC address
            assert(cl->queue.va==kFb+cl->queue.vram && f.dev.vaMap(slot,y,cl->queue.va+0x1000u,true,false)==cg::Code::BadArgs);}
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
        h.mem[r.mecDbLower]=0x110u;h.mem[r.mecDbUpper]=0xffcu;h.mem[r.nbioAperEn]=1u;   // MES's range covers ours
        assert(h.dev.start(r,h.hw(),h.heap,kFb)==cg::Code::Ok && h.writes.size()==16u+12u+kGfxStartWrites+2u);   // + the MEC range moved below the gfx doorbell
        for(auto &w:h.writes)assert(w.first==r.g.v.q.grbm || navi33::gfx_ring::allowed(r.gfx,w.first) || w.first==r.mecDbLower || w.first==r.mecDbUpper || w.first==r.invRangeLo || w.first==r.invRangeHi);          // only the HQD ACTIVE checks
    }
    // 7. The N48N front end (what RADV's Darwin backend calls), on the same fake GC.
    {   namespace nf=n48n_front;
        Fake f(r);assert(f.dev.start(r,f.hw(),f.heap,kFb)==cg::Code::Ok);
        nf::Session ss{};
        uint64_t o4[4]={0,0,0,0};
        assert(nf::boCreate(f.dev,ss,n48n_gem_create_in{4096,0,N48N_GEM_DOMAIN_VRAM,0},o4)==nf::R::NotReady);
        assert(f.dev.open(&ss,ss.slot)==cg::Code::Ok);ss.open=true;
        n48n_info inf{};nf::info(f.dev,ss,f.heap,0x26010042u,29u,inf);
        assert(inf.abi_version==1u && inf.vmid==8u && inf.gc_version==0x0b0002u && inf.gb_addr_config==0x26010042u);
        assert(inf.va_low_first==0x10000u && inf.va_low_last==0x7fffffffffffull && inf.max_ibs==16u && inf.ring_size_dw==1024u);
        assert(inf.vram_vis_total==(256ull-32ull)<<20 && inf.vram_vis_free<inf.vram_vis_total && inf.gtt_cap>0u && !inf.flags);
        // Buffers: GTT is served from BAR0 VRAM; refused flags and domains.
        uint64_t sh[4],ag[4],im[4],ib[4],fe[4];
        assert(nf::boCreate(f.dev,ss,n48n_gem_create_in{gr::kShaderMax,0x10000,N48N_GEM_DOMAIN_VRAM,N48N_GEM_CPU_ACCESS_REQUIRED},sh)==nf::R::Ok);
        assert(sh[2]==N48N_GEM_DOMAIN_VRAM && (sh[3]&N48N_PLACED_CPU_MAPPABLE));
        uint64_t vr=0,by=0;assert(f.dev.boRange(ss.slot,uint32_t(sh[0]),vr,by) && !(vr&0xffffu));   // alignment kept
        assert(nf::boCreate(f.dev,ss,n48n_gem_create_in{4096,0,N48N_GEM_DOMAIN_GTT,N48N_GEM_CPU_GTT_USWC},ag)==nf::R::Ok);
        assert(nf::boCreate(f.dev,ss,n48n_gem_create_in{gr::kImageBytes,0,N48N_GEM_DOMAIN_VRAM|N48N_GEM_DOMAIN_GTT,0},im)==nf::R::Ok);
        assert(nf::boCreate(f.dev,ss,n48n_gem_create_in{4096,0,N48N_GEM_DOMAIN_GTT,0},ib)==nf::R::Ok);
        assert(nf::boCreate(f.dev,ss,n48n_gem_create_in{4096,0,N48N_GEM_DOMAIN_GTT,0},fe)==nf::R::Ok);
        uint64_t x[4]={999u,0,0,0};       // a handle nobody was given
        assert(nf::boCreate(f.dev,ss,n48n_gem_create_in{4096,0,N48N_GEM_DOMAIN_VRAM,N48N_GEM_CP_MQD_GFX9},x)==nf::R::BadArgument);
        assert(nf::boCreate(f.dev,ss,n48n_gem_create_in{4096,0,N48N_GEM_DOMAIN_CPU,0},x)==nf::R::BadArgument);
        assert(nf::boCreate(f.dev,ss,n48n_gem_create_in{4096,3,N48N_GEM_DOMAIN_VRAM,0},x)==nf::R::BadArgument);
        // GEM VA, as RADV maps: whole buffers at its own VAs.
        auto va=[&](uint64_t h,uint64_t addr,uint32_t flags,uint32_t op=N48N_VA_OP_MAP,uint64_t size=0,uint64_t off=0){
            n48n_gem_va g{};g.handle=uint32_t(h);g.operation=op;g.flags=flags;g.va_address=addr;g.map_size=size;g.offset_in_bo=off;
            return nf::gemVa(f.dev,ss,g);
        };
        const uint32_t RW=N48N_VM_PAGE_READABLE|N48N_VM_PAGE_WRITEABLE, RX=N48N_VM_PAGE_READABLE|N48N_VM_PAGE_EXECUTABLE;
        assert(va(sh[0],0x800000000ull,RX,N48N_VA_OP_MAP,gr::kShaderMax)==nf::R::Ok);
        assert(va(ag[0],0x800100000ull,RW)==nf::R::Ok && va(im[0],0x800200000ull,RW)==nf::R::Ok);
        assert(va(ib[0],0x800300000ull,RX)==nf::R::Ok && va(fe[0],0x800400000ull,RW)==nf::R::Ok);
        assert(va(999u,0x800500000ull,RW)==nf::R::NotFound);
        assert(va(ib[0],0x800600000ull,RW,N48N_VA_OP_MAP,0,4096)==nf::R::BadArgument);              // offset in BO
        assert(va(im[0],0x800600000ull,RW|N48N_VM_PAGE_PRT)==nf::R::Unsupported);
        assert(va(im[0],0x800600000ull,RW,3u)==nf::R::Unsupported);
        assert(va(ib[0],0x800999000ull,0,N48N_VA_OP_UNMAP)==nf::R::BadArgument);                     // wrong VA
        {   n48n_gem_va g{};g.handle=uint32_t(ib[0]);g.operation=N48N_VA_OP_MAP;g.va_address=0x800700000ull;g.vm_timeline_point=1;
            assert(nf::gemVa(f.dev,ss,g)==nf::R::BadArgument);}
        // Contexts.
        n48n_ctx c1{},c2{},q{};
        assert(nf::ctx(f.dev,ss,n48n_ctx{N48N_CTX_OP_ALLOC,0,0,0},c1)==nf::R::Ok && c1.op==1u);
        assert(nf::ctx(f.dev,ss,n48n_ctx{N48N_CTX_OP_ALLOC,0,0,0},c2)==nf::R::Ok && c2.op==2u);
        assert(nf::ctx(f.dev,ss,n48n_ctx{N48N_CTX_OP_QUERY_STATE2,0,1,0},q)==nf::R::Ok && q.op==0u);
        assert(nf::ctx(f.dev,ss,n48n_ctx{N48N_CTX_OP_FREE,0,2,0},q)==nf::R::Ok && nf::ctx(f.dev,ss,n48n_ctx{N48N_CTX_OP_FREE,0,2,0},q)==nf::R::BadArgument);
        // The gradient through CS: shader, args, a compute IB, a fence chunk.
        std::vector<uint32_t> code(gr::kShaderMax/4u,0u);
        for(uint64_t i=0;i<k.codeBytes;++i)code[i/4u]|=uint32_t(co[k.codeOffset+i])<<(8u*(i%4u));
        f.put(ss.slot,uint32_t(sh[0]),0,code);
        uint32_t argv[6];gr::args(argv,0x800200000ull,64,64,64,42);f.put(ss.slot,uint32_t(ag[0]),0,std::vector<uint32_t>(argv,argv+6));
        const std::vector<uint32_t> pm4=gradientIb(k,0x800000000ull,0x800100000ull,64,64);
        f.put(ss.slot,uint32_t(ib[0]),0,pm4);
        n48n_cs_in h{N48N_ABI_VERSION,1,1,N48N_CS_HAS_FENCE,uint32_t(fe[0]),16,0};
        n48n_cs_ib one{0,N48N_IB_FLAG_PREAMBLE,0x800300000ull,uint32_t(4u*pm4.size()),N48N_HW_IP_COMPUTE,0,0};
        uint64_t seq=0,w[3];
        assert(nf::submit(f.dev,ss,h,&one,seq)==nf::R::Ok && seq==1u);
        assert(nf::waitOnce(f.dev,ss,seq,w)==nf::R::Ok && !w[0] && w[1]==1u && w[2]==1u);
        assert(nf::waitOnce(f.dev,ss,N48N_SEQ_LAST,w)==nf::R::Ok && !w[0]);
        App view{};view.slot=ss.slot;view.image=uint32_t(im[0]);assert(imageOk(f,view,42));
        uint64_t fb=0;assert(f.dev.boRange(ss.slot,uint32_t(fe[0]),fb,by) && f.q(fb+16u)==1u);     // the user fence
        // A graphics submission (G40): the same IB on the kernel's gfx ring in this client's VMID, with
        // CONTEXT_CONTROL, FRAME_CONTROL around it, the user fence at its MC address.
        n48n_cs_ib gfx=one;gfx.ip_type=N48N_HW_IP_GFX;
        const unsigned cntx0=f.gfxCntx, ibs0=f.gfxIbs;
        assert(nf::submit(f.dev,ss,h,&gfx,seq)==nf::R::Ok && seq==2u && nf::waitOnce(f.dev,ss,seq,w)==nf::R::Ok && w[1]==2u);
        assert(f.gfxCntx==cntx0+1u && f.gfxIbs==ibs0+1u && f.q(fb+16u)==2u && imageOk(f,view,42));
        // In-order retire across the two rings: a held gfx ring keeps seq 3 open, so the compute seq 4
        // that finishes first does not retire until the gfx work lands.
        f.gfxHold=true;
        uint64_t s3=0,s4=0;
        assert(nf::submit(f.dev,ss,h,&gfx,s3)==nf::R::Ok && nf::submit(f.dev,ss,h,&one,s4)==nf::R::Ok && s3==3u && s4==4u);
        assert(nf::waitOnce(f.dev,ss,s4,w)==nf::R::Ok && w[0] && w[1]==2u);              // busy: retired stays 2
        f.gfxHold=false;f.runGfx();
        assert(nf::waitOnce(f.dev,ss,s4,w)==nf::R::Ok && !w[0] && w[1]==4u);
        n48n_cs_in two=h;two.num_ibs=2;n48n_cs_ib mixed[2]={one,gfx};
        assert(nf::submit(f.dev,ss,two,mixed,seq)==nf::R::BadArgument);                   // one ring per submission
        // Refusals: unknown ctx, bad fence, bad IB flags.
        n48n_cs_in badCtx=h;badCtx.ctx_id=2;assert(nf::submit(f.dev,ss,badCtx,&one,seq)==nf::R::BadArgument);
        n48n_cs_in badFence=h;badFence.fence_offset=4092;assert(nf::submit(f.dev,ss,badFence,&one,seq)==nf::R::BadArgument);
        n48n_cs_in roFence=h;roFence.fence_handle=uint32_t(ib[0]);roFence.fence_offset=0;
        assert(nf::submit(f.dev,ss,roFence,&one,seq)==nf::R::BadArgument);                         // read-only mapping
        n48n_cs_ib ce=one;ce.flags=N48N_IB_FLAG_CE;assert(nf::submit(f.dev,ss,h,&ce,seq)==nf::R::BadArgument);
        assert(nf::waitOnce(f.dev,ss,99,w)==nf::R::BadArgument);
        uint32_t reg=0;
        assert(nf::readRegs(0x263e,1,0x26010042u,&reg)==nf::R::Ok && reg==0x26010042u);
        assert(nf::readRegs(0x263f,1,0,&reg)==nf::R::Unsupported && nf::readRegs(0x263e,2,0,&reg)==nf::R::Unsupported);
        // A faulting session reports itself through QUERY_STATE2 and info.flags.
        f.put(ss.slot,uint32_t(ib[0]),0,{gd::packet3(gd::kReleaseMem,6u),gd::kReleaseMemControl,gd::kReleaseMemDataSel32,0x0u,0x9u,1u,0u,0u});
        n48n_cs_ib bad=one;bad.ib_bytes=32;
        assert(nf::submit(f.dev,ss,h,&bad,seq)==nf::R::Ok && nf::waitOnce(f.dev,ss,seq,w)==nf::R::Aborted);
        assert(nf::ctx(f.dev,ss,n48n_ctx{N48N_CTX_OP_QUERY_STATE2,0,1,0},q)==nf::R::Ok && (q.op&N48N_CTX_QUERY2_GUILTY));
        nf::info(f.dev,ss,f.heap,0,29u,inf);assert(inf.flags&N48N_INFO_HUNG);
    }
    puts("Client GPU contexts: per-client VMID/queue/address space, user IBs through INDIRECT_BUFFER, fences, "
         "ring wrap and back-pressure, TLB flush on remap, fault isolation, drain/dequeue teardown, quarantine: passed");
}
