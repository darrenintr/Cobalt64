// SPDX-License-Identifier: MIT
// PSP13 IP firmware stage, IOKit side (protocol, plan and runner:
// Navi33Psp13FwLoad.h; design: FIRMWARE_LOAD_DESIGN.md). Sends LOAD_IP_FW
// for a range of the 26 records through the existing KM ring, and optionally
// AUTOLOAD_RLC. The only register it may write is C2PMSG_67, the ring write
// pointer; the only VRAM it may touch is the ring pages and the staging area
// of the plan's 8 MiB window. The SMU image is record 26 and Linux sends it first, so it goes alone. Every step is logged.
#include "Navi33DarwinPspLoad.h"
#include "Navi33Discovery.h"
#include "Navi33Psp13FwLoad.h"
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/pci/IOPCIDevice.h>

namespace navi33 { namespace darwin_psp {
namespace {
namespace pl=psp13load;
namespace rg=psp13ring;
namespace fw=psp13ipfw;
namespace fl=psp13fwload;
struct Io {
    IOMemoryMap *regs=nullptr, *win=nullptr;
    uint64_t wptr=0u;              // BAR5 offset of C2PMSG_67, the only writable register
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
    if(!io->regs || !io->wptr || off!=io->wptr || (off&3u) ||
       off>io->regs->getLength() || io->regs->getLength()-off<4u) return false;
    *reinterpret_cast<volatile uint32_t *>(
        static_cast<uintptr_t>(io->regs->getVirtualAddress())+off)=value;
    __asm__ __volatile__("mfence" ::: "memory");
    return true;
}
// Two zones only: the ring pages and the staging area.
bool inZone(uint32_t off,uint32_t len) {
    if(!len)return false;
    const uint64_t end=static_cast<uint64_t>(off)+len;
    return (off>=rg::kRingOff && end<=rg::kRingOff+rg::kPagesBytes) ||
           (off>=fl::kStageOff && end<=fl::kStageOff+fl::kStageBytes);
}
volatile uint8_t *window(Io *io,uint32_t off,uint32_t len) {
    if(!io->win || io->win->getLength()!=fl::kWindowBytes || !inZone(off,len))
        return nullptr;
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
bool vramRead(void *ctx,uint32_t off,uint8_t *dst,uint32_t len) {
    volatile uint8_t *w=window(static_cast<Io *>(ctx),off,len);
    if(!w || !dst)return false;
    for(uint32_t i=0;i<len;++i)dst[i]=w[i];
    return true;
}
void sleepMs(void *,uint32_t ms) { IOSleep(ms); }
const char *stepName(uint32_t s) {
    static const char *names[]={"none","KDB","SPL","SYS_DRV","sOS","RING_CREATE",
                                "LOAD_TOC","SETUP_TMR","FW_LOAD","AUTOLOAD_RLC"};
    return s<10u ? names[s] : "?";
}
} // namespace

n33_psp_load_result executeFw(IOPCIDevice *pci,const Request &req) {
    if(!pci || (req.mode!=N33_PSP_LOAD_FW_PLAN && req.mode!=N33_PSP_LOAD_FW_RUN) ||
       !req.attempted)
        return pl::blank(N33_PSPL_BAD_REQUEST);
    IPDiscovery disc;
    if(!req.rom || req.romBytes<64u || req.romBytes>IPDiscovery::kMaxBlob ||
       !disc.parse(req.rom,req.romBytes))
        return pl::blank(N33_PSPL_BAD_ROM);
    n33_psp_load_result out=pl::blank(N33_PSPL_FW_BAD_IMAGES);
    if(req.writeGate)out.ring_flags|=N33_PSPR_FLAG_GATE_OPEN;
    if(req.autoloadGate)out.ring_flags|=N33_PSPR_FLAG_AUTOLOAD_GATE;
    out.fw_record_count=N33_PSP_FW_RECORDS;
    // The input is the eight pinned files, concatenated in File order; their
    // sizes are pinned, so the split needs no lengths from the caller.
    uint64_t total=0u;
    for(size_t k=0;k<fw::kFileCount;++k)total+=fw::kPinnedBytes[k];
    if(!req.fw || req.fwBytes!=total)return out;
    fw::Images in;
    size_t at=0u;
    for(size_t k=0;k<fw::kFileCount;++k) {
        in.f[k]={req.fw+at,fw::kPinnedBytes[k]};
        at+=fw::kPinnedBytes[k];
    }
    if(!fw::allPinned(in))return out;
    out.flags|=N33_PSPL_FLAG_FW_PINNED;

    IODeviceMemory *bar5=pci->getDeviceMemoryWithRegister(kIOPCIConfigBaseAddress5);
    IODeviceMemory *bar0=pci->getDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0);
    if(!bar5 || !bar0 || bar5->getLength()<4u) {
        out.state=N33_PSPL_NO_BARS;return out;
    }
    const uint64_t bar5Bytes=static_cast<uint64_t>(bar5->getLength());
    const pl::Range bar0Range{static_cast<uint64_t>(bar0->getPhysicalAddress()),
                              static_cast<uint64_t>(bar0->getLength())};
    out.bar0_phys=bar0Range.phys;out.bar0_bytes=bar0Range.bytes;
    // Same console rule as the other stages: boot console first, else the host's.
    const pl::Range boot{req.consolePhys,req.consoleBytes};
    const pl::Range console=boot.phys ? boot :
        pl::hostConsole(boot,req.hostConsole,req.hostConsoleOffset,
                        req.hostConsoleBytes,bar0Range);
    out.console_phys=console.phys;out.console_bytes=console.bytes;
    const pl::Registers pregs=pl::resolve(disc,bar5Bytes);
    const rg::Regs rregs=rg::resolve(disc,bar5Bytes);
    if(!pregs.valid || !rregs.valid) {out.state=N33_PSPL_BAD_ROM;return out;}

    Io io{};
    io.wptr=rregs.c67;
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
    const fl::Plan plan=fl::plan(snap,console,bar0Range,in,req.startAt,req.stopAfter);
    fl::report(out,snap,plan);
    out.state=plan.state;
    IOLog("Navi33Probe: psp fw: plan state %u sOS %08x C2PMSG_64 %08x _67 %08x "
          "ring %08x:%08x/%08x console %s records [%u,%u) of %u autoload %u "
          "staging MC 0x%llx (%llu B)\n",
          out.state,snap.base.c2p81,snap.c64,snap.c67,snap.c70,snap.c69,snap.c71,
          boot.phys ? "boot" : (console.phys ? "host" : "none"),
          plan.start,plan.stop,plan.list.count,plan.autoload ? 1u : 0u,
          plan.stageMc,plan.stagingTotal);
    if(plan.state!=N33_PSPL_PLAN_OK || req.mode==N33_PSP_LOAD_FW_PLAN) {
        io.regs->release();return out;
    }
    if(!req.writeGate) {
        io.regs->release();out.state=N33_PSPL_WRITE_DISABLED;return out;
    }
    // AUTOLOAD_RLC is refused before anything is written: a run that asks for it
    // without its own boot-arg sends no record either.
    if(plan.autoload && !req.autoloadGate) {
        io.regs->release();out.state=N33_PSPL_AUTOLOAD_DISABLED;return out;
    }
    if(*req.attempted) {
        io.regs->release();out.state=N33_PSPL_ALREADY_ATTEMPTED;return out;
    }
    IODeviceMemory *sub=IODeviceMemory::withSubRange(bar0,plan.ring.win.fwPriOffset,
                                                     fl::kWindowBytes);
    if(sub) {
        io.win=sub->map(kIOMapInhibitCache);
        sub->release();
    }
    if(!io.win || io.win->getLength()!=fl::kWindowBytes) {
        if(io.win)io.win->release();
        io.regs->release();
        out.state=N33_PSPL_MAP_FAILED;return out;
    }
    // From here on the PSP may hold state until a full power-off.
    *req.attempted=true;
    IOLog("Navi33Probe: psp fw: RUN starts, records [%u,%u)%s\n",plan.start,
          plan.stop<plan.list.count ? plan.stop : plan.list.count,
          plan.autoload ? " then AUTOLOAD_RLC" : "");
    rg::Hw hw{};
    hw.ctx=&io;hw.read32=read32;hw.write32=write32;hw.vramZero=vramZero;
    hw.vramWrite=vramWrite;hw.vramRead=vramRead;hw.sleepMs=sleepMs;
    const fl::Outcome o=fl::run(plan,rregs,in,hw);
    fl::report(out,o);
    io.win->release();
    io.regs->release();
    IOLog("Navi33Probe: psp fw: RUN state %u at %s, %u done (last record %u, TMR addr 0x%llx), "
          "%u register writes, cmd status %08x fence %u, %u ms%s\n",
          out.state,stepName(out.step),out.steps_done,out.fw_last_record,out.fw_tmr_addr,
          out.register_writes,out.cmd_status,out.fence_seen,out.elapsed_ms,
          o.autoloadSent ? ", AUTOLOAD_RLC sent" : "");
    return out;
}
} } // namespace navi33::darwin_psp
