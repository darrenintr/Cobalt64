// SPDX-License-Identifier: MIT
// PSP13 KM ring stage, IOKit side (protocol and plan: Navi33Psp13Ring.h).
// Writes only C2PMSG_64/67/69/70/71 and a 2 MiB sub-range of BAR0 chosen by
// the plan (fw_pri for the TOC copy, then the ring, command and fence pages).
// The sOS load keeps its own writer (Navi33DarwinPspLoad.cpp); this file does
// not widen what that one may touch. Every step is logged.
#include "Navi33DarwinPspLoad.h"
#include "Navi33Discovery.h"
#include "Navi33Psp13Ring.h"
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/pci/IOPCIDevice.h>

namespace navi33 { namespace darwin_psp {
namespace {
namespace pl=psp13load;
namespace rg=psp13ring;
struct Io {
    IOMemoryMap *regs=nullptr, *win=nullptr;
    uint64_t allowed[5]{};
};
bool read32(void *ctx,uint64_t off,uint32_t *out) {
    IOMemoryMap *map=static_cast<Io *>(ctx)->regs;
    if(!map || !out || (off&3u) || off>map->getLength() ||
       map->getLength()-off<4u) return false;
    *out=*reinterpret_cast<const volatile uint32_t *>(
        static_cast<uintptr_t>(map->getVirtualAddress())+off);
    return true;
}
bool write32(void *ctx,uint64_t off,uint32_t value) {
    Io *io=static_cast<Io *>(ctx);
    bool ok=false;
    for(uint64_t a:io->allowed)ok=ok || off==a;
    if(!io->regs || !ok || (off&3u) || off>io->regs->getLength() ||
       io->regs->getLength()-off<4u) return false;
    *reinterpret_cast<volatile uint32_t *>(
        static_cast<uintptr_t>(io->regs->getVirtualAddress())+off)=value;
    __asm__ __volatile__("mfence" ::: "memory");
    return true;
}
volatile uint8_t *window(Io *io,uint32_t off,uint32_t len) {
    if(!io->win || io->win->getLength()!=rg::kWindowBytes ||
       off>rg::kWindowBytes || len>rg::kWindowBytes-off)return nullptr;
    return reinterpret_cast<volatile uint8_t *>(
        static_cast<uintptr_t>(io->win->getVirtualAddress()))+off;
}
bool vramZero(void *ctx,uint32_t off,uint32_t len) {
    volatile uint8_t *w=window(static_cast<Io *>(ctx),off,len);
    if(!w || (off&3u) || (len&3u))return false;
    volatile uint32_t *d=reinterpret_cast<volatile uint32_t *>(const_cast<uint8_t *>(w));
    for(uint32_t i=0;i<len/4u;++i)d[i]=0u;
    __asm__ __volatile__("sfence; mfence" ::: "memory");
    return true;
}
bool vramWrite(void *ctx,uint32_t off,const uint8_t *src,uint32_t len) {
    volatile uint8_t *w=window(static_cast<Io *>(ctx),off,len);
    if(!w || !src)return false;
    for(uint32_t i=0;i<len;++i)w[i]=src[i];
    __asm__ __volatile__("sfence; mfence" ::: "memory");
    return true;
}
// The read-back (and every fence poll) goes through the uncached BAR0 map. If
// the HDP read cache ever returned a stale fence, the symptom is a CMD_TIMEOUT
// after the PSP did run the command; the plan then refuses a rerun by itself
// because the ring registers are no longer zero (it resumes instead).
bool vramRead(void *ctx,uint32_t off,uint8_t *dst,uint32_t len) {
    volatile uint8_t *w=window(static_cast<Io *>(ctx),off,len);
    if(!w || !dst)return false;
    for(uint32_t i=0;i<len;++i)dst[i]=w[i];
    return true;
}
void sleepMs(void *,uint32_t ms) { IOSleep(ms); }
const char *stepName(uint32_t s) {
    static const char *names[]={"none","KDB","SPL","SYS_DRV","sOS","RING_CREATE",
                                "LOAD_TOC","SETUP_TMR"};
    return s<8u ? names[s] : "?";
}
} // namespace

n33_psp_load_result executeRing(IOPCIDevice *pci,const Request &req) {
    if(!pci || (req.mode!=N33_PSP_LOAD_RING_PLAN && req.mode!=N33_PSP_LOAD_RING_RUN) ||
       !req.attempted)
        return pl::blank(N33_PSPL_BAD_REQUEST);
    IPDiscovery disc;
    if(!req.rom || req.romBytes<64u || req.romBytes>IPDiscovery::kMaxBlob ||
       !disc.parse(req.rom,req.romBytes))
        return pl::blank(N33_PSPL_BAD_ROM);
    n33_psp_load_result out=pl::blank(N33_PSPL_BAD_FIRMWARE);
    if(req.writeGate)out.ring_flags|=N33_PSPR_FLAG_GATE_OPEN;
    pl::Firmware fw{};
    if(!pl::fromContainer(req.fw,req.fwBytes,fw))return out;
    out.fw_sos_version=fw.sos.version;out.fw_kdb_bytes=fw.kdb.size;
    out.fw_spl_bytes=fw.spl.size;out.fw_sys_bytes=fw.sys.size;
    out.fw_sos_bytes=fw.sos.size;out.toc_bytes=fw.toc.size;
    if(!pl::pinned(req.fw,req.fwBytes))return out;
    out.flags|=N33_PSPL_FLAG_FW_PINNED;
    // LOAD_TOC and SETUP_TMR need the TOC; the ring alone does not.
    if(req.stopAfter>=N33_PSP_RING_STOP_TOC && (!fw.toc.data || !fw.toc.size))
        return out;

    IODeviceMemory *bar5=pci->getDeviceMemoryWithRegister(kIOPCIConfigBaseAddress5);
    IODeviceMemory *bar0=pci->getDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0);
    if(!bar5 || !bar0 || bar5->getLength()<4u) {
        out.state=N33_PSPL_NO_BARS;return out;
    }
    const uint64_t bar5Bytes=static_cast<uint64_t>(bar5->getLength());
    const pl::Range bar0Range{static_cast<uint64_t>(bar0->getPhysicalAddress()),
                              static_cast<uint64_t>(bar0->getLength())};
    out.bar0_phys=bar0Range.phys;out.bar0_bytes=bar0Range.bytes;
    // Same console rule as the sOS load: the boot console wins; a guest with
    // none may name the host's (navi33-hostfb-*, Navi33Psp13Loader.h).
    const pl::Range boot{req.consolePhys,req.consoleBytes};
    const pl::Range console=boot.phys ? boot :
        pl::hostConsole(boot,req.hostConsole,req.hostConsoleOffset,
                        req.hostConsoleBytes,bar0Range);
    out.console_phys=console.phys;out.console_bytes=console.bytes;
    const pl::Registers pregs=pl::resolve(disc,bar5Bytes);
    const rg::Regs rregs=rg::resolve(disc,bar5Bytes);
    if(!pregs.valid || !rregs.valid) {out.state=N33_PSPL_BAD_ROM;return out;}

    Io io{};
    io.allowed[0]=rregs.c64;io.allowed[1]=rregs.c67;io.allowed[2]=rregs.c69;
    io.allowed[3]=rregs.c70;io.allowed[4]=rregs.c71;
    io.regs=bar5->map();
    if(!io.regs || static_cast<uint64_t>(io.regs->getLength())<bar5Bytes) {
        if(io.regs)io.regs->release();
        out.state=N33_PSPL_MAP_FAILED;return out;
    }
    rg::Snapshot snap{};
    if(!rg::readSnapshot(pregs,rregs,read32,&io,snap)) {
        io.regs->release();
        out.state=N33_PSPL_READ_FAILED;return out;
    }
    const rg::Plan plan=rg::plan(snap,console,bar0Range,req.stopAfter);
    rg::report(out,snap,plan);
    out.state=plan.state;
    IOLog("Navi33Probe: psp ring: plan state %u sOS %08x C2PMSG_64 %08x _67 %08x "
          "_69 %08x _70 %08x _71 %08x %s console %s vram+0x%llx fw_pri MC 0x%llx "
          "ring 0x%llx cmd 0x%llx fence 0x%llx tmr 0x%llx stop %u\n",
          out.state,snap.base.c2p81,snap.c64,snap.c67,snap.c69,snap.c70,snap.c71,
          plan.resume ? "RESUME" : "fresh",
          boot.phys ? "boot" : (console.phys ? "host" : "none"),
          plan.win.consoleOffset,plan.win.fwPriMc,plan.ringMc,plan.cmdMc,
          plan.fenceMc,plan.tmrMc,req.stopAfter);
    if(plan.state!=N33_PSPL_PLAN_OK || req.mode==N33_PSP_LOAD_RING_PLAN) {
        io.regs->release();return out;
    }
    if(!req.writeGate) {
        io.regs->release();out.state=N33_PSPL_WRITE_DISABLED;return out;
    }
    if(*req.attempted) {
        io.regs->release();out.state=N33_PSPL_ALREADY_ATTEMPTED;return out;
    }
    IODeviceMemory *sub=IODeviceMemory::withSubRange(bar0,plan.win.fwPriOffset,
                                                     rg::kWindowBytes);
    if(sub) {
        io.win=sub->map(kIOMapInhibitCache);
        sub->release();
    }
    if(!io.win || io.win->getLength()!=rg::kWindowBytes) {
        if(io.win)io.win->release();
        io.regs->release();
        out.state=N33_PSPL_MAP_FAILED;return out;
    }
    // From here on the PSP may hold a ring until a full power-off.
    *req.attempted=true;
    IOLog("Navi33Probe: psp ring: RUN starts (%s, stop after %u)\n",
          plan.resume ? "resuming the ring" : "creating the ring",req.stopAfter);
    rg::Hw hw{};
    hw.ctx=&io;hw.read32=read32;hw.write32=write32;hw.vramZero=vramZero;
    hw.vramWrite=vramWrite;hw.vramRead=vramRead;hw.sleepMs=sleepMs;
    const rg::Outcome o=rg::run(plan,rregs,fw.toc,hw);
    rg::report(out,o);
    io.win->release();
    io.regs->release();
    IOLog("Navi33Probe: psp ring: RUN state %u at %s, %u steps, %u register writes, "
          "C2PMSG_64 %08x _67 %08x cmd status %08x fence %u TOC %u TMR %u B, %u ms\n",
          out.state,stepName(out.step),out.steps_done,out.register_writes,
          out.c2pmsg64_last,out.c2pmsg67_last,out.cmd_status,out.fence_seen,
          out.toc_bytes,out.tmr_bytes,out.elapsed_ms);
    return out;
}
} } // namespace navi33::darwin_psp
