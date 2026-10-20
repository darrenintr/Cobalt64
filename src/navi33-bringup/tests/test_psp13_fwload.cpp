// PSP13 IP firmware stage: planner and runner against a fake PSP, with synthetic
// images (requirePins=false; the real pins are tested in test_psp13_ipfw).
// The fake follows psp_ring_cmd_submit(): a frame is consumed when C2PMSG_67
// moves, LOAD_IP_FW gets a TMR address back, the fence is written last.
#include "../src/Navi33Psp13FwLoad.h"
#include "rx7600_ipd_fixture.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>
using namespace navi33;
namespace pl=navi33::psp13load;
namespace rg=navi33::psp13ring;
namespace fw=navi33::psp13ipfw;
namespace fl=navi33::psp13fwload;

constexpr uint64_t kBar0Phys=0x7000000000ull, kBar0=256ull<<20;
constexpr uint64_t kConsole=2560ull*1440u*4u;

// ---- synthetic images (same shapes as test_psp13_ipfw) ----
static void w32(std::vector<uint8_t> &v,size_t p,uint32_t x) {
    for(unsigned i=0;i<4u;++i)v[p+i]=static_cast<uint8_t>(x>>(8u*i));
}
static std::vector<uint8_t> image(size_t total,uint32_t hdrSize,uint16_t maj,uint16_t min,
                                  uint16_t ipMaj,uint32_t ucodeSize,uint8_t seed) {
    std::vector<uint8_t> v(total,0u);
    w32(v,0,static_cast<uint32_t>(total));w32(v,4,hdrSize);
    w32(v,8,static_cast<uint32_t>(maj)|static_cast<uint32_t>(min)<<16u);
    w32(v,12,static_cast<uint32_t>(ipMaj));
    w32(v,16,0x11u);w32(v,20,ucodeSize);w32(v,24,256u);
    for(size_t i=256;i<total;++i)v[i]=static_cast<uint8_t>(i*7u+seed);
    return v;
}
struct Set { std::vector<uint8_t> f[fw::kFileCount]; };
static Set synthetic() {
    Set s;
    auto &sd=s.f[static_cast<size_t>(fw::File::Sdma)];
    sd=image(416,64,2,0,6,160,1);
    w32(sd,36,100);w32(sd,48,356);w32(sd,52,60);
    uint8_t seed=2;
    for(fw::File f:{fw::File::Pfp,fw::File::Me,fw::File::Mec}) {
        auto &c=s.f[static_cast<size_t>(f)];
        c=image(536,60,2,0,11,280,seed++);
        w32(c,32,0x77u);w32(c,36,200);w32(c,40,256);w32(c,44,80);w32(c,48,456);
    }
    for(fw::File f:{fw::File::Mes,fw::File::Mes1}) {
        auto &m=s.f[static_cast<size_t>(f)];
        m=image(476,72,1,0,11,220,seed++);
        w32(m,36,150);w32(m,40,256);w32(m,48,70);w32(m,52,406);
    }
    auto &u=s.f[static_cast<size_t>(fw::File::Smu)];
    u=image(416,44,2,1,13,100,seed++);
    auto &i=s.f[static_cast<size_t>(fw::File::Imu)];
    i=image(352,48,1,0,11,96,seed++);
    w32(i,32,64);w32(i,36,256);w32(i,40,32);w32(i,44,320);
    auto &r=s.f[static_cast<size_t>(fw::File::Rlc)];
    r=image(580,204,2,3,11,100,seed++);
    w32(r,116,0);w32(r,120,0);w32(r,132,40);w32(r,136,356);w32(r,148,48);w32(r,152,396);
    w32(r,156,64);w32(r,160,444);w32(r,164,32);w32(r,168,508);
    w32(r,180,16);w32(r,184,540);w32(r,196,24);w32(r,200,556);
    return s;
}
static fw::Images view(const Set &s) {
    fw::Images in;
    for(size_t k=0;k<fw::kFileCount;++k)in.f[k]={s.f[k].data(),s.f[k].size()};
    return in;
}

// ---- fake PSP ----
struct Cmd { uint32_t id, type, size; uint64_t addr; std::vector<uint8_t> bytes; };
struct Fake {
    rg::Regs r;
    uint32_t c64=0x80020000u, c67=48u, c69=0x0f100000u, c70=0x80u, c71=0x1000u;
    std::vector<uint8_t> vram=std::vector<uint8_t>(8u*1024u*1024u,0u);
    uint64_t windowMc=0u;
    uint32_t status=0u, failAtCommand=0u;     // nth command (1-based) answers with `status`
    bool noFence=false, stuckStage=false;
    std::vector<std::pair<uint64_t,uint32_t>> writes;
    std::vector<Cmd> cmds;
    uint32_t sleeps=0u;
};
static uint32_t rd(const Fake &f,uint32_t off) { return rg::get32(f.vram.data(),off); }
static bool fRead(void *c,uint64_t off,uint32_t *out) {
    Fake &f=*static_cast<Fake *>(c);
    if(off==f.r.c64)*out=f.c64;
    else if(off==f.r.c67)*out=f.c67;
    else if(off==f.r.c69)*out=f.c69;
    else if(off==f.r.c70)*out=f.c70;
    else if(off==f.r.c71)*out=f.c71;
    else return false;
    return true;
}
static void consume(Fake &f,uint32_t newWptr) {
    const uint32_t slot=((newWptr+1024u-16u)%1024u)*4u;
    const uint32_t base=rg::kRingOff+slot;
    const uint64_t cmdMc=(static_cast<uint64_t>(rd(f,base+4u))<<32u)|rd(f,base);
    const uint64_t fenceMc=(static_cast<uint64_t>(rd(f,base+16u))<<32u)|rd(f,base+12u);
    const uint32_t fenceVal=rd(f,base+20u);
    assert(cmdMc>=f.windowMc && fenceMc>=f.windowMc);
    const uint32_t cmdOff=static_cast<uint32_t>(cmdMc-f.windowMc);
    const uint32_t fenceOff=static_cast<uint32_t>(fenceMc-f.windowMc);
    Cmd c{};c.id=rd(f,cmdOff+rg::kCmdIdOff);
    if(c.id==rg::kCmdLoadIpFw) {
        c.addr=(static_cast<uint64_t>(rd(f,cmdOff+32u))<<32u)|rd(f,cmdOff+28u);
        c.size=rd(f,cmdOff+36u);c.type=rd(f,cmdOff+40u);
        assert(c.addr>=f.windowMc+fl::kStageOff &&
               c.addr+c.size<=f.windowMc+fl::kStageOff+fl::kStageBytes);
        const uint32_t at=static_cast<uint32_t>(c.addr-f.windowMc);
        c.bytes.assign(f.vram.begin()+at,f.vram.begin()+at+c.size);
        // the PSP answers with the image's address inside the TMR
        rg::put32(f.vram.data(),cmdOff+rg::kRespFwAddrLoOff,0xfc100000u+static_cast<uint32_t>(f.cmds.size())*0x10000u);
        rg::put32(f.vram.data(),cmdOff+rg::kRespFwAddrLoOff+4u,0x81u);
    } else {
        assert(c.id==rg::kCmdAutoloadRlc);
        for(unsigned i=28;i<28+16;++i)assert(f.vram[cmdOff+i]==0u);   // no body
    }
    f.cmds.push_back(c);
    const uint32_t n=static_cast<uint32_t>(f.cmds.size());
    rg::put32(f.vram.data(),cmdOff+rg::kRespStatusOff,
              (f.failAtCommand && n==f.failAtCommand) ? f.status : 0u);
    if(!f.noFence)rg::put32(f.vram.data(),fenceOff,fenceVal);
}
static bool fWrite(void *c,uint64_t off,uint32_t v) {
    Fake &f=*static_cast<Fake *>(c);
    if(off!=f.r.c67)return false;                        // only the write pointer in this stage
    f.writes.push_back({off,v});
    f.c67=v;consume(f,v);
    return true;
}
static bool inAllowed(uint32_t off,uint32_t len) {       // the writer's two zones
    const bool ring=off>=rg::kRingOff && off+len<=rg::kRingOff+rg::kPagesBytes;
    const bool stage=off>=fl::kStageOff && off+len<=fl::kStageOff+fl::kStageBytes;
    return ring || stage;
}
static bool fZero(void *c,uint32_t off,uint32_t len) {
    Fake &f=*static_cast<Fake *>(c);
    if(!inAllowed(off,len))return false;
    for(uint32_t i=0;i<len;++i)f.vram[off+i]=0u;
    return true;
}
static bool fWriteV(void *c,uint32_t off,const uint8_t *src,uint32_t len) {
    Fake &f=*static_cast<Fake *>(c);
    if(!inAllowed(off,len))return false;
    for(uint32_t i=0;i<len;++i)f.vram[off+i]=src[i];
    return true;
}
static bool fReadV(void *c,uint32_t off,uint8_t *dst,uint32_t len) {
    Fake &f=*static_cast<Fake *>(c);
    if(off+len>f.vram.size())return false;
    for(uint32_t i=0;i<len;++i)dst[i]=f.vram[off+i];
    if(f.stuckStage && off<=fl::kStageOff && off+len>fl::kStageOff)dst[fl::kStageOff-off]^=1u;
    return true;
}
static void fSleep(void *c,uint32_t ms) { static_cast<Fake *>(c)->sleeps+=ms; }
static rg::Hw hwFor(Fake &f) {
    rg::Hw hw{};
    hw.ctx=&f;hw.read32=fRead;hw.write32=fWrite;hw.vramZero=fZero;
    hw.vramWrite=fWriteV;hw.vramRead=fReadV;hw.sleepMs=fSleep;
    return hw;
}

static rg::Snapshot haveRing() {
    rg::Snapshot s{};
    s.base.c2p35=0x80000000u;s.base.c2p81=0x433bbc95u;s.base.mmFbBase=0x8000u;
    s.base.mmFbTop=0x81ffu;s.base.dcnFbBase=0x8000u;s.base.dcnFbTop=0x81ffu;
    s.base.surfLo[0]=0u;s.base.surfHi[0]=0x80u;
    s.c64=0x80020000u;s.c67=48u;s.c69=0x0f100000u;s.c70=0x80u;s.c71=0x1000u;
    return s;
}

int main() {
    const std::vector<uint8_t> ipd=rx7600_fixture::blob();
    IPDiscovery disc;
    assert(disc.parse(ipd.data(),ipd.size()));
    const rg::Regs regs=rg::resolve(disc,rx7600_fixture::kBar5);
    assert(regs.valid);
    static_assert(fl::kStageOff+fl::kStageBytes==fl::kWindowBytes,"staging fills the top of the window");
    static_assert(rg::kRingOff+rg::kPagesBytes<=fl::kStageOff,"ring pages stay below the staging area");
    static_assert(fw::kCmdLoadIpFw==rg::kCmdLoadIpFw && fw::kCmdAutoloadRlc==rg::kCmdAutoloadRlc,
                  "the two headers agree on the command ids");

    const Set set=synthetic();
    const fw::Images in=view(set);
    const pl::Range bar0{kBar0Phys,kBar0}, console{kBar0Phys,kConsole};

    // ---- command buffers ----
    uint8_t cmd[rg::kCmdBytes];
    assert(rg::prepLoadIpFw(cmd,0x800f400000ull,17408u,71u));
    assert(rg::get32(cmd,8)==6u && rg::get32(cmd,28)==0x0f400000u && rg::get32(cmd,32)==0x80u &&
           rg::get32(cmd,36)==17408u && rg::get32(cmd,40)==71u && rg::get32(cmd,44)==0u);
    assert(!rg::prepLoadIpFw(cmd,0x800f400800ull,100u,71u));      // not 4 KiB aligned
    assert(!rg::prepLoadIpFw(cmd,0x800f400000ull,0u,71u));
    assert(!rg::prepLoadIpFw(cmd,0x800f400000ull,100u,0u));
    rg::prepAutoloadRlc(cmd);
    assert(rg::get32(cmd,8)==0x21u);
    for(unsigned i=12;i<rg::kCmdBytes;++i)assert(cmd[i]==0u);

    // ---- planner ----
    fl::Plan p=fl::plan(haveRing(),console,bar0,in,0u,2u,false);
    assert(p.state==N33_PSPL_PLAN_OK && p.list.count==27u && !p.autoload);
    assert(fl::recordCount(p)==2u && fl::firstRecord(p)==0u && fl::endRecord(p)==2u);
    assert(p.stageMc==0x800f400000ull);                               // fw_pri + 4 MiB
    assert(p.ring.ringMc==0x800f100000ull && p.ring.cmdMc==0x800f101000ull);
    assert(p.stagingTotal==27u*4096u);                                // synthetic: 1 page each
    assert(fl::plan(haveRing(),console,bar0,in,0u,2u,true).state==N33_PSPL_FW_BAD_IMAGES); // not pinned
    {Set bad=synthetic();w32(bad.f[0],8,3);                           // header version wrong
     assert(fl::plan(haveRing(),console,bar0,view(bad),0u,2u,false).state==N33_PSPL_FW_BAD_IMAGES);}
    assert(fl::plan(haveRing(),console,bar0,in,0u,0u,false).state==N33_PSPL_BAD_REQUEST);
    assert(fl::plan(haveRing(),console,bar0,in,2u,2u,false).state==N33_PSPL_BAD_REQUEST);
    assert(fl::plan(haveRing(),console,bar0,in,3u,2u,false).state==N33_PSPL_BAD_REQUEST);
    assert(fl::plan(haveRing(),console,bar0,in,0u,29u,false).state==N33_PSPL_BAD_REQUEST);
    assert(fl::plan(haveRing(),console,bar0,in,28u,28u,false).state==N33_PSPL_BAD_REQUEST);
    assert(fl::plan(haveRing(),console,bar0,in,28u,29u,false).state==N33_PSPL_BAD_REQUEST);
    p=fl::plan(haveRing(),console,bar0,in,27u,28u,false);             // autoload alone
    assert(p.state==N33_PSPL_PLAN_OK && p.autoload && fl::recordCount(p)==0u);
    p=fl::plan(haveRing(),console,bar0,in,0u,28u,false);
    assert(p.state==N33_PSPL_PLAN_OK && p.autoload && fl::recordCount(p)==27u);
    assert(fl::plan(haveRing(),console,bar0,in,0u,27u,false).autoload==false);
    rg::Snapshot s=haveRing();s.base.c2p81=0u;
    assert(fl::plan(s,console,bar0,in,0u,1u,false).state==N33_PSPL_SOS_NOT_ALIVE);
    s=haveRing();s.c64=0u;
    assert(fl::plan(s,console,bar0,in,0u,1u,false).state==N33_PSPL_RING_NOT_READY);
    s=haveRing();s.c69=s.c70=s.c71=s.c67=0u;                          // no ring yet
    assert(fl::plan(s,console,bar0,in,0u,1u,false).state==N33_PSPL_RING_MISSING);
    s=haveRing();s.c69=0x1000u;                                       // someone else's ring
    assert(fl::plan(s,console,bar0,in,0u,1u,false).state==N33_PSPL_RING_MISSING);
    s=haveRing();s.c67=8u;                                            // misaligned wptr
    assert(fl::plan(s,console,bar0,in,0u,1u,false).state==N33_PSPL_RING_MISSING);
    assert(fl::plan(haveRing(),pl::Range{},bar0,in,0u,1u,false).state==N33_PSPL_NO_CONSOLE);
    // the window is 8 MiB wide here: a surface at 258 MiB clears the 2 MiB ring
    // window but not the staging area at 244..248 MiB
    s=haveRing();s.base.surfLo[1]=0x10200000u;s.base.surfHi[1]=0x80u;
    assert(rg::plan(s,console,bar0,1u).state==N33_PSPL_PLAN_OK);
    assert(fl::plan(s,console,bar0,in,0u,1u,false).state==N33_PSPL_NO_WINDOW);

    // ---- runner: the first two records (SDMA) ----
    p=fl::plan(haveRing(),console,bar0,in,0u,2u,false);
    Fake f;f.r=regs;f.windowMc=p.ring.win.fwPriMc;
    fl::Outcome o=fl::run(p,regs,in,hwFor(f));
    assert(o.r.state==N33_PSPL_FW_DONE && o.recordsDone==2u && !o.autoloadSent);
    assert(f.cmds.size()==2u && o.r.writes==2u);
    assert(f.writes[0]==std::make_pair(regs.c67,64u) && f.writes[1]==std::make_pair(regs.c67,80u));
    assert(f.cmds[0].id==6u && f.cmds[0].type==71u && f.cmds[0].size==100u);
    assert(f.cmds[1].id==6u && f.cmds[1].type==72u && f.cmds[1].size==60u);
    assert(f.cmds[0].addr==0x800f400000ull && f.cmds[1].addr==0x800f401000ull);   // own pages
    const fw::Record &r0=p.list.rec[0];
    assert(!memcmp(f.cmds[0].bytes.data(),set.f[static_cast<size_t>(r0.file)].data()+r0.offset,r0.size));
    const fw::Record &r1=p.list.rec[1];
    assert(!memcmp(f.cmds[1].bytes.data(),set.f[static_cast<size_t>(r1.file)].data()+r1.offset,r1.size));
    assert(o.lastRecord==1u);
    assert(o.lastFwAddr==((0x81ull<<32u)|(0xfc100000u+0x10000u)));
    assert(f.vram[fl::kStageOff+2u*4096u]==0u);                       // record 2 not staged
    assert(o.r.flags==N33_PSPR_FLAG_FW_LOADED && o.r.fenceSeen==2u);
    // only the write pointer register was written, and only the two zones in VRAM
    for(const auto &w:f.writes)assert(w.first==regs.c67);

    // ---- all 27 records in list order, then autoload ----
    p=fl::plan(haveRing(),console,bar0,in,0u,28u,false);
    Fake g;g.r=regs;g.windowMc=p.ring.win.fwPriMc;
    o=fl::run(p,regs,in,hwFor(g));
    assert(o.r.state==N33_PSPL_FW_DONE && o.recordsDone==27u && o.autoloadSent);
    assert(g.cmds.size()==28u && o.r.writes==28u);
    for(uint32_t i=0;i<27u;++i) {
        assert(g.cmds[i].id==6u && g.cmds[i].type==p.list.rec[i].type);
        assert(g.cmds[i].size==p.list.rec[i].size);
        assert(g.cmds[i].addr==0x800f400000ull+i*4096ull);            // list order, one page each
    }
    assert(g.cmds[27].id==0x21u && p.list.rec[25].type==8u && p.list.rec[26].type==18u); // RLC_G, then the SMU last in the list
    assert(o.r.flags==(N33_PSPR_FLAG_FW_LOADED|N33_PSPR_FLAG_AUTOLOAD_SENT));
    assert(o.r.stepsDone==28u && o.r.step==N33_PSPL_STEP_AUTOLOAD);
    assert(o.r.fenceIndex==28u);
    // wptr wrapped nothing yet: 48 + 27 frames * 16 dwords = 480 < 1024
    assert(g.c67==48u+28u*16u);

    // ---- autoload alone ----
    // ---- the SMU first, on its own: record 26 at its fixed slot ----
    p=fl::plan(haveRing(),console,bar0,in,26u,27u,false);
    {Fake h;h.r=regs;h.windowMc=p.ring.win.fwPriMc;
     o=fl::run(p,regs,in,hwFor(h));
     assert(o.r.state==N33_PSPL_FW_DONE && o.recordsDone==1u && !o.autoloadSent);
     assert(h.cmds.size()==1u && h.cmds[0].type==18u && h.cmds[0].size==100u);
     assert(h.cmds[0].addr==0x800f400000ull+26ull*4096ull && o.lastRecord==26u);}
    p=fl::plan(haveRing(),console,bar0,in,27u,28u,false);
    {Fake h;h.r=regs;h.windowMc=p.ring.win.fwPriMc;
     o=fl::run(p,regs,in,hwFor(h));
     assert(o.r.state==N33_PSPL_FW_DONE && o.recordsDone==0u && o.autoloadSent);
     assert(h.cmds.size()==1u && h.cmds[0].id==0x21u);}

    // ---- the ring wraps like the PSP's (frame slot arithmetic) ----
    {rg::Snapshot w=haveRing();w.c67=1008u;                           // last frame of the ring
     p=fl::plan(w,console,bar0,in,0u,3u,false);
     Fake h;h.r=regs;h.windowMc=p.ring.win.fwPriMc;h.c67=1008u;
     o=fl::run(p,regs,in,hwFor(h));
     assert(o.r.state==N33_PSPL_FW_DONE && h.writes[0].second==0u && h.writes[1].second==16u);}

    // ---- failures stop where they happen ----
    p=fl::plan(haveRing(),console,bar0,in,0u,28u,false);
    {Fake h;h.r=regs;h.windowMc=p.ring.win.fwPriMc;h.noFence=true;     // PSP never answers
     o=fl::run(p,regs,in,hwFor(h));
     assert(o.r.state==N33_PSPL_CMD_TIMEOUT && o.recordsDone==0u && h.cmds.size()==1u);
     assert(!o.autoloadSent && h.sleeps>=rg::kCmdWaitMs);}
    {Fake h;h.r=regs;h.windowMc=p.ring.win.fwPriMc;h.status=0xffff0003u;h.failAtCommand=3u;
     o=fl::run(p,regs,in,hwFor(h));
     assert(o.r.state==N33_PSPL_CMD_FAILED && o.r.cmdStatus==0xffff0003u);
     assert(o.recordsDone==2u && h.cmds.size()==3u && !o.autoloadSent);}    // nothing after the refusal
    {Fake h;h.r=regs;h.windowMc=p.ring.win.fwPriMc;h.stuckStage=true;   // staged bytes do not read back
     o=fl::run(p,regs,in,hwFor(h));
     assert(o.r.state==N33_PSPL_STAGE_VERIFY_FAILED && h.writes.empty() && o.recordsDone==0u);}
    {Fake h;h.r=regs;h.windowMc=p.ring.win.fwPriMc;                      // not an OK plan
     fl::Plan bad=p;bad.state=N33_PSPL_NO_CONSOLE;
     o=fl::run(bad,regs,in,hwFor(h));
     assert(o.r.state==N33_PSPL_BAD_REQUEST && h.writes.empty() && !o.r.wroteVram);}
    {Fake h;h.r=regs;h.windowMc=p.ring.win.fwPriMc;                      // a record outside its file
     Set badset=synthetic();fw::Images bi=view(badset);bi.f[static_cast<size_t>(fw::File::Sdma)].size=100;
     o=fl::run(p,regs,bi,hwFor(h));
     assert(o.r.state==N33_PSPL_STAGE_VERIFY_FAILED && h.writes.empty());}

    // ---- result reports ----
    p=fl::plan(haveRing(),console,bar0,in,0u,2u,false);
    n33_psp_load_result out=pl::blank(N33_PSPL_BAD_REQUEST);
    fl::report(out,haveRing(),p);
    assert(out.fw_record_count==27u && out.fw_last_record==0u && out.ring_mc==0x800f100000ull);
    {Fake h;h.r=regs;h.windowMc=p.ring.win.fwPriMc;
     o=fl::run(p,regs,in,hwFor(h));fl::report(out,o);
     assert(out.state==N33_PSPL_FW_DONE && out.steps_done==2u && out.fw_last_record==1u &&
            out.register_writes==2u && (out.ring_flags&N33_PSPR_FLAG_FW_LOADED) &&
            out.fw_tmr_addr==o.lastFwAddr && (out.flags&N33_PSPL_FLAG_WROTE_REGISTERS));}
    // Only exactly [26,27) is the SMU-only range with its own latch.
    {namespace fl=navi33::psp13fwload;
     static_assert(fl::kSmuRecord==26u,"SMU image is record 26");
     assert(fl::smuOnlyRange(26u,27u));
     assert(!fl::smuOnlyRange(26u,28u) && !fl::smuOnlyRange(0u,27u) && !fl::smuOnlyRange(25u,27u) &&
            !fl::smuOnlyRange(0u,26u) && !fl::smuOnlyRange(27u,28u));}
    std::puts("PSP13 IP firmware stage planner and runner tests passed");
    return 0;
}
