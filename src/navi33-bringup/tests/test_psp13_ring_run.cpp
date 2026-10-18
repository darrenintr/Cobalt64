// PSP13 KM ring stage: planner and runner against a fake PSP.
// The fake follows psp_v13_0.c / psp_ring_cmd_submit(): a frame is consumed
// when C2PMSG_67 moves, the command buffer gets its response, then the
// fence is written. Nothing here touches hardware.
#include "../src/Navi33Psp13Ring.h"
#include "rx7600_ipd_fixture.h"
#include <cassert>
#include <cstdio>
#include <vector>
using namespace navi33;
namespace pl=navi33::psp13load;
namespace pr=navi33::psp13ring;

constexpr uint64_t kBar0Phys=0x7000000000ull, kBar0=256ull<<20;
constexpr uint64_t kConsole=2560ull*1440u*4u;

static pl::Snapshot sosAlive() {
    pl::Snapshot s{};
    s.c2p35=0x80000000u;s.c2p81=0x433bbc95u;s.mmFbBase=0x8000u;s.mmFbTop=0x81ffu;
    s.dcnFbBase=0x8000u;s.dcnFbTop=0x81ffu;
    s.surfLo[0]=0u;s.surfHi[0]=0x80u;
    return s;
}
static pr::Snapshot fresh() {
    pr::Snapshot s{};
    s.base=sosAlive();s.c64=0x80000000u;
    return s;
}

struct Fake {
    pr::Regs r;
    uint32_t c64=0x80000000u, c67=0u, c69=0u, c70=0u, c71=0u;
    uint32_t c64Pending=0u;
    std::vector<uint8_t> vram=std::vector<uint8_t>(2u*1024u*1024u,0u);
    uint64_t windowMc=0u, expectTmrMc=0u;
    uint32_t tmrReply=0x1300000u, status=0u;
    bool noCreateResp=false, noFence=false, stuckBit=false;
    std::vector<std::pair<uint64_t,uint32_t>> writes;
    std::vector<uint32_t> cmdIds;
    uint32_t sleeps=0u;
};
static uint32_t rd(const Fake &f,uint32_t off) { return pr::get32(f.vram.data(),off); }
static bool fRead(void *c,uint64_t off,uint32_t *out) {
    Fake &f=*static_cast<Fake *>(c);
    if(off==f.r.c64) {
        if(f.c64Pending) {--f.c64Pending;*out=0u;return true;}
        *out=f.c64;return true;
    }
    if(off==f.r.c67) {*out=f.c67;return true;}
    if(off==f.r.c69) {*out=f.c69;return true;}
    if(off==f.r.c70) {*out=f.c70;return true;}
    if(off==f.r.c71) {*out=f.c71;return true;}
    return false;
}
static void consume(Fake &f,uint32_t newWptr) {
    // The frame just before the new write pointer.
    const uint32_t slot=((newWptr+1024u-16u)%1024u)*4u;
    const uint32_t base=pr::kRingOff+slot;
    const uint64_t cmdMc=(static_cast<uint64_t>(rd(f,base+4u))<<32u)|rd(f,base);
    const uint64_t fenceMc=(static_cast<uint64_t>(rd(f,base+16u))<<32u)|rd(f,base+12u);
    const uint32_t fenceVal=rd(f,base+20u);
    assert(cmdMc>=f.windowMc && fenceMc>=f.windowMc);
    const uint32_t cmdOff=static_cast<uint32_t>(cmdMc-f.windowMc);
    const uint32_t fenceOff=static_cast<uint32_t>(fenceMc-f.windowMc);
    const uint32_t id=rd(f,cmdOff+pr::kCmdIdOff);
    f.cmdIds.push_back(id);
    pr::put32(f.vram.data(),cmdOff+pr::kRespStatusOff,f.status);
    if(id==pr::kCmdLoadToc) {
        const uint64_t toc=(static_cast<uint64_t>(rd(f,cmdOff+32u))<<32u)|rd(f,cmdOff+28u);
        assert(toc==f.windowMc);                      // fw_pri, window start
        assert(rd(f,cmdOff+36u)!=0u);
        pr::put32(f.vram.data(),cmdOff+pr::kRespTmrSizeOff,f.tmrReply);
    } else if(id==pr::kCmdSetupTmr) {
        const uint64_t tmr=(static_cast<uint64_t>(rd(f,cmdOff+32u))<<32u)|rd(f,cmdOff+28u);
        assert(tmr==f.expectTmrMc && rd(f,cmdOff+36u)==f.tmrReply);
        assert(rd(f,cmdOff+40u)==2u);                  // virt_phy_addr
        // "system physical" address = VRAM offset = MC - FB base (0x8000000000).
        const uint64_t pa=(static_cast<uint64_t>(rd(f,cmdOff+48u))<<32u)|rd(f,cmdOff+44u);
        assert(pa==f.expectTmrMc-0x8000000000ull);
    } else {
        assert(false && "unexpected command id");
    }
    if(!f.noFence)pr::put32(f.vram.data(),fenceOff,fenceVal);
}
static bool fWrite(void *c,uint64_t off,uint32_t v) {
    Fake &f=*static_cast<Fake *>(c);
    if(off!=f.r.c64 && off!=f.r.c67 && off!=f.r.c69 && off!=f.r.c70 &&
       off!=f.r.c71)return false;                     // the allowlist
    f.writes.push_back({off,v});
    if(off==f.r.c69)f.c69=v;
    else if(off==f.r.c70)f.c70=v;
    else if(off==f.r.c71)f.c71=v;
    else if(off==f.r.c64) {
        assert(v==(pr::kRingTypeKm<<16u) && f.c69 && f.c71==pr::kRingBytes);
        f.c64=f.noCreateResp ? 0u : 0x80000000u;f.c64Pending=3u;
    } else {
        f.c67=v;consume(f,v);
    }
    return true;
}
static bool fZero(void *c,uint32_t off,uint32_t len) {
    Fake &f=*static_cast<Fake *>(c);
    if(off+len>f.vram.size())return false;
    for(uint32_t i=0;i<len;++i)f.vram[off+i]=0u;
    return true;
}
static bool fWriteV(void *c,uint32_t off,const uint8_t *src,uint32_t len) {
    Fake &f=*static_cast<Fake *>(c);
    if(off+len>f.vram.size())return false;
    for(uint32_t i=0;i<len;++i)f.vram[off+i]=src[i];
    return true;
}
static bool fReadV(void *c,uint32_t off,uint8_t *dst,uint32_t len) {
    Fake &f=*static_cast<Fake *>(c);
    if(off+len>f.vram.size())return false;
    for(uint32_t i=0;i<len;++i)dst[i]=f.vram[off+i];
    if(f.stuckBit && off<=pr::kRingOff && off+len>pr::kRingOff)dst[pr::kRingOff-off]|=1u;
    return true;
}
static void fSleep(void *c,uint32_t ms) { static_cast<Fake *>(c)->sleeps+=ms; }
static pr::Hw hwFor(Fake &f) {
    pr::Hw hw{};
    hw.ctx=&f;hw.read32=fRead;hw.write32=fWrite;hw.vramZero=fZero;
    hw.vramWrite=fWriteV;hw.vramRead=fReadV;hw.sleepMs=fSleep;
    return hw;
}
static bool onlyRingRegs(const Fake &f) {
    for(const auto &w:f.writes)
        if(w.first!=f.r.c64 && w.first!=f.r.c67 && w.first!=f.r.c69 &&
           w.first!=f.r.c70 && w.first!=f.r.c71)return false;
    return true;
}

int main() {
    const std::vector<uint8_t> ipd=rx7600_fixture::blob();
    IPDiscovery disc;
    assert(disc.parse(ipd.data(),ipd.size()));
    const pr::Regs regs=pr::resolve(disc,rx7600_fixture::kBar5);
    assert(regs.valid);
    // BAR5 byte offset = (0x16000 + reg) * 4, reg from mp_13_0_2_offset.h.
    assert(regs.c64==0x58200u && regs.c67==0x5820cu && regs.c69==0x58214u);
    assert(regs.c70==0x58218u && regs.c71==0x5821cu);
    assert(!pr::resolve(IPDiscovery{},rx7600_fixture::kBar5).valid);
    assert(!pr::resolve(disc,0x58000u).valid);               // BAR5 too short
    for(uint32_t r:pr::kWritableRegisters)
        assert(r!=pl::kC2pmsg35 && r!=pl::kC2pmsg36);        // never the bootloader

    // --- planner ---
    const pl::Range bar0{kBar0Phys,kBar0}, console{kBar0Phys,kConsole};
    pr::Plan p=pr::plan(fresh(),console,bar0,3u);
    assert(p.state==N33_PSPL_PLAN_OK && !p.resume);
    assert(p.win.fwPriMc==0x800f000000ull);
    assert(p.ringMc==0x800f100000ull && p.cmdMc==0x800f101000ull &&
           p.fenceMc==0x800f102000ull);
    assert(p.tmrMc==0x81fc000000ull);                        // FB base + 8128 MiB
    assert(pr::plan(fresh(),console,bar0,0u).state==N33_PSPL_BAD_REQUEST);
    assert(pr::plan(fresh(),console,bar0,4u).state==N33_PSPL_BAD_REQUEST);
    pr::Snapshot s=fresh();s.base.c2p81=0u;
    assert(pr::plan(s,console,bar0,1u).state==N33_PSPL_SOS_NOT_ALIVE);
    s=fresh();s.c64=0u;
    assert(pr::plan(s,console,bar0,1u).state==N33_PSPL_RING_NOT_READY);
    s=fresh();s.c64=0x80000001u;                             // error status
    assert(pr::plan(s,console,bar0,1u).state==N33_PSPL_RING_NOT_READY);
    s=fresh();s.c69=0x1000u;
    assert(pr::plan(s,console,bar0,1u).state==N33_PSPL_RING_REGS_UNEXPECTED);
    s=fresh();s.c67=16u;                                     // wptr without a ring
    assert(pr::plan(s,console,bar0,1u).state==N33_PSPL_RING_REGS_UNEXPECTED);
    // An existing ring at our address is resumed (wptr frame aligned, in range).
    s=fresh();s.c69=0x0f100000u;s.c70=0x80u;s.c71=0x1000u;s.c67=32u;
    p=pr::plan(s,console,bar0,2u);
    assert(p.state==N33_PSPL_PLAN_OK && p.resume);
    s.c67=8u;
    assert(pr::plan(s,console,bar0,2u).state==N33_PSPL_RING_REGS_UNEXPECTED);
    s.c67=1024u;
    assert(pr::plan(s,console,bar0,2u).state==N33_PSPL_RING_REGS_UNEXPECTED);
    s=fresh();s.c69=0x0f100000u;s.c70=0x80u;s.c71=0x2000u;   // wrong size
    assert(pr::plan(s,console,bar0,2u).state==N33_PSPL_RING_REGS_UNEXPECTED);
    // The sOS-load checks still apply: console, aperture, FB, window.
    assert(pr::plan(fresh(),pl::Range{},bar0,1u).state==N33_PSPL_NO_CONSOLE);
    s=fresh();s.base.surfLo[0]=0x01000000u;                  // surface not at console
    assert(pr::plan(s,console,bar0,1u).state==N33_PSPL_APERTURE_MISMATCH);
    s=fresh();s.base.mmFbTop=0x8000u;s.base.dcnFbTop=0x8000u; // 16 MiB of VRAM
    assert(pr::plan(s,console,bar0,1u).state==N33_PSPL_NO_WINDOW);
    s=fresh();s.base.mmFbTop=0x81f0u;s.base.dcnFbTop=0x81f0u; // TMR would not fit
    assert(pr::plan(s,console,bar0,1u).state==N33_PSPL_NO_WINDOW);
    // The window is 2 MiB wide: a surface at 257 MiB (VRAM) clears a 1 MiB
    // fw_pri at 240 MiB but not the ring pages next to it.
    s=fresh();s.base.surfLo[1]=0x10100000u;s.base.surfHi[1]=0x80u;   // +257 MiB
    pl::Snapshot sl=s.base;sl.c2p81=0u;
    assert(pl::plan(sl,console,bar0).state==N33_PSPL_PLAN_OK);
    assert(pr::plan(s,console,bar0,1u).state==N33_PSPL_NO_WINDOW);

    // --- runner: the full chain ---
    const std::vector<uint8_t> toc(2304u,0x5au);
    const pl::Part tocPart{toc.data(),static_cast<uint32_t>(toc.size()),1u};
    p=pr::plan(fresh(),console,bar0,3u);
    Fake f;f.r=regs;f.windowMc=p.win.fwPriMc;f.expectTmrMc=p.tmrMc;
    pr::Outcome o=pr::run(p,regs,tocPart,hwFor(f));
    assert(o.state==N33_PSPL_RING_DONE && o.stepsDone==3u && o.step==N33_PSPL_STEP_SETUP_TMR);
    assert(o.writes==6u && onlyRingRegs(f));
    // Exact order of the register writes: 69, 70, 71, 64, then wptr twice.
    assert(f.writes[0]==std::make_pair(regs.c69,0x0f100000u));
    assert(f.writes[1]==std::make_pair(regs.c70,0x80u));
    assert(f.writes[2]==std::make_pair(regs.c71,0x1000u));
    assert(f.writes[3]==std::make_pair(regs.c64,0x00020000u));
    assert(f.writes[4]==std::make_pair(regs.c67,16u));
    assert(f.writes[5]==std::make_pair(regs.c67,32u));
    assert(f.cmdIds.size()==2u && f.cmdIds[0]==pr::kCmdLoadToc &&
           f.cmdIds[1]==pr::kCmdSetupTmr);
    assert(o.tmrBytes==0x1300000u && o.tocBytes==2304u && o.fenceIndex==2u);
    assert(o.flags==(N33_PSPR_FLAG_RING_CREATED|N33_PSPR_FLAG_TOC_LOADED|
                     N33_PSPR_FLAG_TMR_SET));
    assert(o.cmdStatus==0u && o.fenceSeen==2u && o.wroteVram);
    // The TOC sits at the window start, zero padded to 1 MiB.
    for(uint32_t i=0;i<2304u;++i)assert(f.vram[i]==0x5au);
    for(uint32_t i=2304u;i<1024u*1024u;i+=4093u)assert(f.vram[i]==0u);
    // Frame 0 (LOAD_TOC) points at the cmd and fence pages.
    assert(rd(f,pr::kRingOff+0u)==static_cast<uint32_t>(p.cmdMc) &&
           rd(f,pr::kRingOff+12u)==static_cast<uint32_t>(p.fenceMc));
    assert(rd(f,pr::kRingOff+8u)==pr::kCmdBytes && rd(f,pr::kRingOff+20u)==1u);
    assert(rd(f,pr::kRingOff+64u+20u)==2u);

    // --- stop points ---
    p=pr::plan(fresh(),console,bar0,1u);
    {Fake g;g.r=regs;g.windowMc=p.win.fwPriMc;
     o=pr::run(p,regs,pl::Part{},hwFor(g));                 // no TOC needed for 1
     assert(o.state==N33_PSPL_RING_DONE && o.writes==4u && o.stepsDone==1u);
     assert(g.cmdIds.empty() && g.c67==0u && g.vram[0]==0u);
     assert(o.flags==N33_PSPR_FLAG_RING_CREATED);}
    p=pr::plan(fresh(),console,bar0,2u);
    {Fake g;g.r=regs;g.windowMc=p.win.fwPriMc;g.expectTmrMc=p.tmrMc;
     o=pr::run(p,regs,tocPart,hwFor(g));
     assert(o.state==N33_PSPL_RING_DONE && o.writes==5u && o.stepsDone==2u);
     assert(g.cmdIds.size()==1u && o.tmrBytes==0x1300000u);
     assert(!(o.flags&N33_PSPR_FLAG_TMR_SET));}
    {Fake g;g.r=regs;g.windowMc=p.win.fwPriMc;                // stop 2 needs a TOC
     o=pr::run(p,regs,pl::Part{},hwFor(g));
     assert(o.state==N33_PSPL_BAD_REQUEST && g.writes.empty() && !o.wroteVram);}

    // --- resume: the ring already exists, wptr continues ---
    s=fresh();s.c69=0x0f100000u;s.c70=0x80u;s.c71=0x1000u;s.c67=32u;
    p=pr::plan(s,console,bar0,2u);
    {Fake g;g.r=regs;g.windowMc=p.win.fwPriMc;g.expectTmrMc=p.tmrMc;
     g.c67=32u;g.c69=s.c69;g.c70=s.c70;g.c71=s.c71;
     o=pr::run(p,regs,tocPart,hwFor(g));
     assert(o.state==N33_PSPL_RING_DONE && o.writes==1u && onlyRingRegs(g));
     assert(g.writes[0]==std::make_pair(regs.c67,48u));
     assert(o.flags==(N33_PSPR_FLAG_RESUMED|N33_PSPR_FLAG_TOC_LOADED));
     assert(rd(g,pr::kRingOff+32u*4u+8u)==pr::kCmdBytes);}   // frame at slot 2

    // --- failures stop where they happen ---
    p=pr::plan(fresh(),console,bar0,3u);
    {Fake g;g.r=regs;g.windowMc=p.win.fwPriMc;g.noCreateResp=true;
     o=pr::run(p,regs,tocPart,hwFor(g));
     assert(o.state==N33_PSPL_RING_CREATE_TIMEOUT && o.writes==4u);
     assert(g.cmdIds.empty() && g.c67==0u);}
    {Fake g;g.r=regs;g.windowMc=p.win.fwPriMc;g.noFence=true;  // PSP never answers
     o=pr::run(p,regs,tocPart,hwFor(g));
     assert(o.state==N33_PSPL_CMD_TIMEOUT && o.step==N33_PSPL_STEP_LOAD_TOC);
     assert(o.writes==5u && g.sleeps>=pr::kCmdWaitMs && o.stepsDone==1u);}
    {Fake g;g.r=regs;g.windowMc=p.win.fwPriMc;g.status=0xffff000cu;
     o=pr::run(p,regs,tocPart,hwFor(g));
     assert(o.state==N33_PSPL_CMD_FAILED && o.cmdStatus==0xffff000cu &&
            o.writes==5u && g.cmdIds.size()==1u);}              // never SETUP_TMR
    const uint32_t badTmr[]={0u,0x1300800u,0x2100000u};         // 0, unaligned, >32 MiB
    for(uint32_t bad:badTmr) {
        Fake g;g.r=regs;g.windowMc=p.win.fwPriMc;g.tmrReply=bad;
        o=pr::run(p,regs,tocPart,hwFor(g));
        assert(o.state==N33_PSPL_TOC_BAD && g.cmdIds.size()==1u && o.writes==5u);
    }
    // Pages that do not read back as zero stop the run before any register write.
    {Fake g;g.r=regs;g.windowMc=p.win.fwPriMc;g.stuckBit=true;
     o=pr::run(p,regs,tocPart,hwFor(g));
     assert(o.state==N33_PSPL_STAGE_VERIFY_FAILED && g.writes.empty() && o.writes==0u);}
    // A ring that is not ready to be created is not written to either.
    {Fake g;g.r=regs;g.windowMc=p.win.fwPriMc;g.c64=0u;
     o=pr::run(p,regs,tocPart,hwFor(g));
     assert(o.state==N33_PSPL_RING_NOT_READY && g.writes.empty());}
    // A plan that is not OK never runs.
    {Fake g;g.r=regs;pr::Plan bad=p;bad.state=N33_PSPL_NO_CONSOLE;
     o=pr::run(bad,regs,tocPart,hwFor(g));
     assert(o.state==N33_PSPL_BAD_REQUEST && g.writes.empty() && !o.wroteVram);}

    // --- result report ---
    n33_psp_load_result out=pl::blank(N33_PSPL_BAD_REQUEST);
    p=pr::plan(fresh(),console,bar0,3u);
    pr::report(out,fresh(),p);
    assert(out.ring_mc==0x800f100000ull && out.tmr_mc==0x81fc000000ull &&
           out.fw_pri_mc==0x800f000000ull && out.c2pmsg64_last==0x80000000u);
    {Fake g;g.r=regs;g.windowMc=p.win.fwPriMc;g.expectTmrMc=p.tmrMc;
     o=pr::run(p,regs,tocPart,hwFor(g));
     pr::report(out,o);
     assert(out.state==N33_PSPL_RING_DONE && out.register_writes==6u &&
            out.steps_done==3u && out.tmr_bytes==0x1300000u &&
            out.toc_bytes==2304u && out.fence_seen==2u);
     assert(out.flags&N33_PSPL_FLAG_WROTE_REGISTERS && out.flags&N33_PSPL_FLAG_WROTE_VRAM);
     assert(out.ring_flags&N33_PSPR_FLAG_TMR_SET);}
    std::puts("PSP13 KM ring planner and runner tests passed");
    return 0;
}
