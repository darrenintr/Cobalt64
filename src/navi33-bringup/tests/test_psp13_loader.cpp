// PSP13 sOS load: firmware pin, read-only plan, and the bootloader chain
// against a fake PSP. Optional argv[1]: a real psp_13_0_7_sos.bin.
#include "../src/Navi33Psp13Loader.h"
#include "rx7600_ipd_fixture.h"
#include <cassert>
#include <cstdio>
#include <utility>
#include <vector>
using namespace navi33;
namespace pl=navi33::psp13load;

static void w16(std::vector<uint8_t> &v,size_t p,uint16_t x) {
    v[p]=static_cast<uint8_t>(x);v[p+1]=static_cast<uint8_t>(x>>8u);
}
static void w32(std::vector<uint8_t> &v,size_t p,uint32_t x) {
    w16(v,p,static_cast<uint16_t>(x));w16(v,p+2u,static_cast<uint16_t>(x>>16u));
}
struct Spec { uint32_t type, version, size; uint8_t fill; };
// Same shape as linux-firmware psp_13_0_7_sos.bin: v2.0, header_size 36,
// descriptors after it, ucode array at 256.
static std::vector<uint8_t> container(const std::vector<Spec> &parts) {
    uint32_t total=0u;
    for(const Spec &s:parts)total+=s.size;
    std::vector<uint8_t> v(256u+total,0u);
    w32(v,0u,static_cast<uint32_t>(v.size()));w32(v,4u,36u);
    w16(v,8u,2u);w16(v,10u,0u);w16(v,12u,13u);w16(v,14u,0u);
    w32(v,16u,0x00330024u);w32(v,20u,total);w32(v,24u,256u);
    w32(v,28u,0xe28dd196u);
    w32(v,32u,static_cast<uint32_t>(parts.size()));
    uint32_t rel=0u;
    for(size_t i=0;i<parts.size();++i) {
        const size_t d=36u+16u*i;
        w32(v,d,parts[i].type);w32(v,d+4u,parts[i].version);
        w32(v,d+8u,rel);w32(v,d+12u,parts[i].size);
        for(uint32_t b=0;b<parts[i].size;++b)v[256u+rel+b]=parts[i].fill;
        rel+=parts[i].size;
    }
    return v;
}
static std::vector<Spec> rx7600Parts() {
    return {{2u,0x00330024u,64u,0x22u},{1u,0x00330024u,48u,0x11u},
            {4u,0xcu,16u,0x44u},{3u,0x04003300u,32u,0x33u},
            {5u,0x010033c0u,8u,0x55u}};
}

struct Fake {
    pl::Registers r;
    uint32_t c2p35=pl::kBootloaderReady, c2p81=0u, pending=0u, busyPolls=3u;
    uint32_t settle=pl::kBootloaderReady, sosCountdown=0u, sosPolls=5u;
    uint32_t aliveAfterWrites=0u;
    bool neverReady=false, sosNeverAlive=false, failVerify=false;
    bool onesOn81=false;
    std::vector<std::pair<uint64_t,uint32_t>> writes;
    std::vector<std::vector<uint8_t>> staged;
    uint32_t sleeps=0u, verifies=0u, reads=0u;
};
static bool fRead(void *c,uint64_t off,uint32_t *out) {
    Fake &f=*static_cast<Fake *>(c);++f.reads;
    if(off==f.r.c2p35) {
        if(f.pending) {--f.pending;*out=0u;return true;}
        *out=f.c2p35;return true;
    }
    if(off==f.r.c2p81) {
        if(f.onesOn81) {*out=UINT32_MAX;return true;}
        if(f.aliveAfterWrites && f.writes.size()>=f.aliveAfterWrites)f.c2p81=0x1u;
        if(f.sosCountdown && !--f.sosCountdown && !f.sosNeverAlive)
            f.c2p81=0x00000a5au;
        *out=f.c2p81;return true;
    }
    if(off==f.r.c2p58) {*out=f.c2p81 ? 0x00330024u : 0u;return true;}
    return false;
}
static bool fWrite(void *c,uint64_t off,uint32_t v) {
    Fake &f=*static_cast<Fake *>(c);
    if(off!=f.r.c2p35 && off!=f.r.c2p36)return false;
    f.writes.push_back({off,v});
    if(off==f.r.c2p35) {
        if(v==pl::kLoadSos) {f.c2p35=0u;f.sosCountdown=f.sosPolls;}
        else {f.pending=f.busyPolls;f.c2p35=f.neverReady ? 0u : f.settle;}
    }
    return true;
}
static bool fStage(void *c,const uint8_t *s,uint32_t n) {
    static_cast<Fake *>(c)->staged.emplace_back(s,s+n);return true;
}
static bool fVerify(void *c,const uint8_t *,uint32_t) {
    Fake &f=*static_cast<Fake *>(c);++f.verifies;return !f.failVerify;
}
static void fSleep(void *c,uint32_t ms) { static_cast<Fake *>(c)->sleeps+=ms; }
static pl::Hw hwFor(Fake &f) {
    pl::Hw hw{};
    hw.ctx=&f;hw.read32=fRead;hw.write32=fWrite;hw.stage=fStage;
    hw.verify=fVerify;hw.sleepMs=fSleep;
    return hw;
}

// What the 0.0.5 run read on the RX 7600, plus a GOP console at vram+0.
static pl::Snapshot measured() {
    pl::Snapshot s{};
    s.c2p35=0x80000000u;s.c2p81=0u;s.mmFbBase=0x8000u;s.mmFbTop=0x81ffu;
    s.dcnFbBase=0x8000u;s.dcnFbTop=0x81ffu;
    s.surfLo[0]=0x00000000u;s.surfHi[0]=0x80u; // MC 0x80_0000_0000
    return s;
}
constexpr uint64_t kBar0Phys=0x7000000000ull, kBar0=256ull<<20;
constexpr uint64_t kConsole=2560ull*1440u*4u;

int main(int argc,char **argv) {
    // --- register map through the physical RX 7600 IP Discovery table ---
    const std::vector<uint8_t> ipd=rx7600_fixture::blob();
    IPDiscovery disc;
    assert(disc.parse(ipd.data(),ipd.size()));
    const pl::Registers regs=pl::resolve(disc,rx7600_fixture::kBar5);
    assert(regs.valid);
    assert(regs.c2p35==0x5818cu && regs.c2p36==0x58190u); // (0x16000+reg)*4
    assert(regs.c2p58==0x581e8u && regs.c2p81==0x58244u);
    assert(regs.mmFbBase==(0x1a000u+0x86cu)*4u && regs.mmFbTop==regs.mmFbBase+4u);
    assert(regs.dcnFbBase==(0x34c0u+0x475u)*4u);           // DMU seg2
    assert(regs.surfLo[0]==(0x34c0u+0x60au)*4u && regs.surfHi[3]==(0x34c0u+0x89fu)*4u);
    assert(!pl::resolve(IPDiscovery{},rx7600_fixture::kBar5).valid);
    assert(!pl::resolve(disc,0x58000u).valid);              // BAR5 too short
    const std::vector<uint8_t> mm300=rx7600_fixture::blob(0u);
    IPDiscovery other;
    assert(other.parse(mm300.data(),mm300.size()));
    assert(!pl::resolve(other,rx7600_fixture::kBar5).valid); // MMHUB 3.0.0
    static_assert(pl::kWritableRegisters[0]==0x64u && pl::kWritableRegisters[1]==0x63u,
                  "the writer may only touch C2PMSG_36 and C2PMSG_35");

    // --- firmware container and pin ---
    const std::vector<uint8_t> image=container(rx7600Parts());
    pl::Firmware fw{};
    assert(pl::fromContainer(image.data(),image.size(),fw));
    assert(fw.kdb.size==32u && fw.kdb.data[0]==0x33u && fw.spl.data[0]==0x55u);
    assert(fw.sys.size==64u && fw.sos.size==48u && fw.sos.version==0x00330024u);
    assert(!pl::pinned(image.data(),image.size()));        // not the real file
    auto parts=rx7600Parts();parts.pop_back();             // no SPL
    auto noSpl=container(parts);
    assert(!pl::fromContainer(noSpl.data(),noSpl.size(),fw));
    parts=rx7600Parts();parts.push_back({8u,1u,8u,0x66u}); // INTF_DRV: no step
    auto extra=container(parts);
    assert(!pl::fromContainer(extra.data(),extra.size(),fw));
    parts=rx7600Parts();parts[1].size=static_cast<uint32_t>(pl::kFwPriBytes+4u);
    auto huge=container(parts);
    assert(!pl::fromContainer(huge.data(),huge.size(),fw));
    assert(pl::fromContainer(image.data(),image.size(),fw));
    if(argc>1) {
        FILE *fp=std::fopen(argv[1],"rb");
        assert(fp);
        std::vector<uint8_t> real(pl::kPinnedBytes+1u);
        const size_t n=std::fread(real.data(),1u,real.size(),fp);
        std::fclose(fp);
        real.resize(n);
        pl::Firmware rf{};
        assert(pl::pinned(real.data(),real.size()));
        assert(pl::fromContainer(real.data(),real.size(),rf));
        assert(rf.sys.size==141200u && rf.sos.size==99184u);
        assert(rf.kdb.size==4880u && rf.spl.size==672u);
        assert(rf.sos.version==0x00330024u);
        real[1000]^=1u;
        assert(!pl::pinned(real.data(),real.size()));
        std::puts("real psp_13_0_7_sos.bin matches the pin");
    }

    // --- read-only plan ---
    const pl::Range bar0{kBar0Phys,kBar0}, console{kBar0Phys,kConsole};
    pl::Plan p=pl::plan(measured(),console,bar0);
    assert(p.state==N33_PSPL_PLAN_OK);
    assert(p.fbBaseMc==0x8000000000ull && p.fbTopMc==0x81ffffffffull);
    assert(p.consoleOffset==0u && p.activePipes==1u);
    assert(p.fwPriOffset==240ull<<20 && p.fwPriMc==0x800f000000ull);
    assert((p.fwPriMc>>20u)==0x800f0u);                     // C2PMSG_36
    assert(p.flags==(N33_PSPL_FLAG_CONSOLE_FOUND|N33_PSPL_FLAG_APERTURE_MATCH|
                     N33_PSPL_FLAG_DCN_FB_MATCH));
    pl::Snapshot s=measured();s.c2p81=0x1u;
    assert(pl::plan(s,console,bar0).state==N33_PSPL_SOS_ALREADY_ALIVE);
    s=measured();s.c2p35=0x80000003u;                       // error bits
    assert(pl::plan(s,console,bar0).state==N33_PSPL_BOOTLOADER_BUSY);
    s=measured();s.c2p35=0u;
    assert(pl::plan(s,console,bar0).state==N33_PSPL_BOOTLOADER_BUSY);
    s=measured();s.mmFbBase=0u;
    assert(pl::plan(s,console,bar0).state==N33_PSPL_BAD_FB_LOCATION);
    s=measured();s.dcnFbBase=0x8100u;
    assert(pl::plan(s,console,bar0).state==N33_PSPL_BAD_FB_LOCATION);
    s=measured();s.dcnFbBase=0u;s.dcnFbTop=0u;              // DCN unset: allowed
    p=pl::plan(s,console,bar0);
    assert(p.state==N33_PSPL_PLAN_OK && !(p.flags&N33_PSPL_FLAG_DCN_FB_MATCH));
    s=measured();s.surfHi[0]=0x90u;                         // outside VRAM
    assert(pl::plan(s,console,bar0).state==N33_PSPL_BAD_FB_LOCATION);
    assert(pl::plan(measured(),pl::Range{},bar0).state==N33_PSPL_NO_CONSOLE);
    assert(pl::plan(measured(),{kBar0Phys-0x1000u,kConsole},bar0).state==
           N33_PSPL_NO_CONSOLE);
    assert(pl::plan(measured(),{kBar0Phys+kBar0-0x1000u,kConsole},bar0).state==
           N33_PSPL_NO_CONSOLE);
    // The scanout surface must sit where the console is in BAR0, or the
    // BAR0 offset to VRAM offset identity is not proven.
    assert(pl::plan(measured(),{kBar0Phys+0x100000u,kConsole},bar0).state==
           N33_PSPL_APERTURE_MISMATCH);
    s=measured();s.surfLo[0]=0u;s.surfHi[0]=0u;             // no active pipe
    assert(pl::plan(s,console,bar0).state==N33_PSPL_APERTURE_MISMATCH);
    s=measured();s.surfLo[0]=0x0e000000u;                   // console at 224 MiB
    assert(pl::plan(s,{kBar0Phys+(224ull<<20),kConsole},bar0).state==
           N33_PSPL_NO_WINDOW);
    s=measured();s.surfLo[1]=0x0e800000u;s.surfHi[1]=0x80u; // 2nd pipe near fw_pri
    p=pl::plan(s,console,bar0);
    assert(p.state==N33_PSPL_NO_WINDOW && p.activePipes==3u);
    assert(pl::plan(measured(),console,{kBar0Phys,128ull<<20}).state==
           N33_PSPL_NO_WINDOW);
    s=measured();s.mmFbTop=0x8000u;                         // 16 MiB of VRAM
    assert(pl::plan(s,console,bar0).state==N33_PSPL_BAD_FB_LOCATION); // DCN disagrees
    s.dcnFbTop=0x8000u;
    assert(pl::plan(s,console,bar0).state==N33_PSPL_NO_WINDOW);
    // 2026-10-18, kext 0.0.6: getConsoleInfo() gave the kernel's mapping of
    // the console, so the plan refused. Only a physical address may pass.
    pl::Range c=pl::bootConsole(0xffffffa06a800000ull,7680u,1080u);
    assert(!c.phys && !c.bytes);
    assert(pl::plan(measured(),c,bar0).state==N33_PSPL_NO_CONSOLE);
    assert(!pl::bootConsole(pl::kPhysLimit,7680u,1080u).phys);
    c=pl::bootConsole(0x830000001ull,7680u,1080u);           // flag bit
    assert(c.phys==0x830000000ull && c.bytes==8100ull*1024u);
    p=pl::plan(measured(),c,{0x830000000ull,kBar0});          // this Mac's BAR0
    assert(p.state==N33_PSPL_PLAN_OK && p.fwPriMc==0x800f000000ull);
    assert(pl::clearOf(240ull<<20,0u,kConsole));
    assert(!pl::clearOf(240ull<<20,(240ull<<20)+(1ull<<20)+(15ull<<20),1u));

    // --- the chain, exactly as psp_v13_0 runs it ---
    p=pl::plan(measured(),console,bar0);
    Fake f;f.r=regs;
    pl::Outcome o=pl::run(p,regs,fw,hwFor(f));
    assert(o.state==N33_PSPL_SOS_ALIVE && o.step==N33_PSPL_STEP_SOS);
    assert(o.stepsDone==4u && o.writes==8u && o.wroteVram);
    assert(o.c2p81After==0x00000a5au && o.sosVersion==0x00330024u);
    const std::vector<std::pair<uint64_t,uint32_t>> expect={
        {regs.c2p36,0x800f0u},{regs.c2p35,pl::kLoadKdb},
        {regs.c2p36,0x800f0u},{regs.c2p35,pl::kLoadSpl},
        {regs.c2p36,0x800f0u},{regs.c2p35,pl::kLoadSysDrv},
        {regs.c2p36,0x800f0u},{regs.c2p35,pl::kLoadSos}};
    assert(f.writes==expect);
    assert(pl::kLoadKdb==0x80000u && pl::kLoadSpl==0x10000000u &&
           pl::kLoadSysDrv==0x10000u && pl::kLoadSos==0x20000u);
    // Upstream load_spl() sends the KDB image; the SPL bytes never go out.
    assert(f.staged.size()==4u && f.verifies==4u);
    assert(f.staged[0].size()==32u && f.staged[0][0]==0x33u);
    assert(f.staged[1]==f.staged[0]);
    assert(f.staged[2].size()==64u && f.staged[2][0]==0x22u);
    assert(f.staged[3].size()==48u && f.staged[3][0]==0x11u);
    assert(f.sleeps>=pl::kSosSettleMs && o.elapsedMs==f.sleeps);
    n33_psp_load_result res=pl::blank(N33_PSPL_PLAN_OK);
    pl::report(res,measured(),p);pl::report(res,o);
    assert(res.state==N33_PSPL_SOS_ALIVE && res.register_writes==8u);
    assert(res.fw_pri_mc==0x800f000000ull && res.surface_mc[0]==0x8000000000ull);
    assert((res.flags&N33_PSPL_FLAG_WROTE_VRAM) &&
           (res.flags&N33_PSPL_FLAG_WROTE_REGISTERS));
    assert(!res.reserved0 && !res.reserved1 && !res.reserved2);

    // Bootloader never comes back after the KDB command.
    Fake t;t.r=regs;t.neverReady=true;
    o=pl::run(p,regs,fw,hwFor(t));
    assert(o.state==N33_PSPL_BOOTLOADER_TIMEOUT && o.step==N33_PSPL_STEP_KDB);
    assert(o.writes==2u && o.stepsDone==0u && t.sleeps==pl::kBootloaderWaitMs);
    // Ready bit with error bits [7:0] is not ready (exact match, as Linux).
    Fake e;e.r=regs;e.settle=0x80000005u;
    o=pl::run(p,regs,fw,hwFor(e));
    assert(o.state==N33_PSPL_BOOTLOADER_TIMEOUT && o.c2p35Last==0x80000005u);
    // A read-back mismatch stops before any register write.
    Fake v;v.r=regs;v.failVerify=true;
    o=pl::run(p,regs,fw,hwFor(v));
    assert(o.state==N33_PSPL_STAGE_VERIFY_FAILED && o.writes==0u && o.wroteVram);
    assert(v.writes.empty());
    // sOS never signs on.
    Fake q;q.r=regs;q.sosNeverAlive=true;
    o=pl::run(p,regs,fw,hwFor(q));
    assert(o.state==N33_PSPL_SOS_TIMEOUT && o.step==N33_PSPL_STEP_SOS);
    assert(o.writes==8u && o.stepsDone==3u);
    // A live sOS closes the bootloader: Linux skips the remaining parts.
    Fake a;a.r=regs;a.aliveAfterWrites=2u;
    o=pl::run(p,regs,fw,hwFor(a));
    assert(o.state==N33_PSPL_SOS_ALIVE && o.writes==2u && o.stepsDone==1u);
    assert(o.step==N33_PSPL_STEP_SPL);
    // All-ones means the PSP stopped answering.
    Fake z;z.r=regs;z.onesOn81=true;
    o=pl::run(p,regs,fw,hwFor(z));
    assert(o.state==N33_PSPL_READ_FAILED && z.writes.empty() && z.staged.empty());
    // Nothing happens without a good plan, firmware and every callback.
    Fake n;n.r=regs;
    pl::Plan bad=p;bad.state=N33_PSPL_WRITE_DISABLED;
    assert(pl::run(bad,regs,fw,hwFor(n)).state==N33_PSPL_BAD_REQUEST);
    bad=p;bad.fwPriMc+=0x1000u;
    assert(pl::run(bad,regs,fw,hwFor(n)).state==N33_PSPL_BAD_REQUEST);
    pl::Hw partial=hwFor(n);partial.verify=nullptr;
    assert(pl::run(p,regs,fw,partial).state==N33_PSPL_BAD_REQUEST);
    assert(pl::run(p,regs,pl::Firmware{},hwFor(n)).state==N33_PSPL_BAD_REQUEST);
    assert(n.reads==0u && n.writes.empty());

    // Snapshot reads refuse all-ones.
    Fake r;r.r=regs;
    pl::Snapshot snap{};
    assert(!pl::readSnapshot(regs,fRead,&r,snap)); // fake has no MMHUB/DCN
    std::puts("PSP13 sOS load plan, pin and bootloader chain tests passed");
    return 0;
}
