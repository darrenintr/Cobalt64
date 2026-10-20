// SPDX-License-Identifier: MIT
// PSP13 sOS load, IOKit side. Maps BAR5 for the handshake and ONLY the
// planned 1 MiB fw_pri sub-range of BAR0, so a bad offset cannot reach the
// GOP console. Every step is logged; the plan runs first in both modes.
#include "Navi33DarwinPspLoad.h"
#include "Navi33Discovery.h"
#include "Navi33Psp13Loader.h"
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/pci/IOPCIDevice.h>

namespace navi33 { namespace darwin_psp {
namespace {
namespace pl=psp13load;
struct Io {
    IOMemoryMap *regs=nullptr, *win=nullptr;
    uint64_t allowed[2]{};
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
    if(!io->regs || (off!=io->allowed[0] && off!=io->allowed[1]) ||
       (off&3u) || off>io->regs->getLength() ||
       io->regs->getLength()-off<4u) return false;
    *reinterpret_cast<volatile uint32_t *>(
        static_cast<uintptr_t>(io->regs->getVirtualAddress())+off)=value;
    __asm__ __volatile__("mfence" ::: "memory");
    return true;
}
volatile uint32_t *window(Io *io) {
    if(!io->win || io->win->getLength()!=pl::kFwPriBytes)return nullptr;
    return reinterpret_cast<volatile uint32_t *>(
        static_cast<uintptr_t>(io->win->getVirtualAddress()));
}
// psp_copy_fw(): memset the whole 1 MiB, then copy the part.
bool stage(void *ctx,const uint8_t *src,uint32_t len) {
    volatile uint32_t *w=window(static_cast<Io *>(ctx));
    if(!w || !src || !len || len>pl::kFwPriBytes)return false;
    for(uint32_t i=0;i<pl::kFwPriBytes/4u;++i)w[i]=0u;
    for(uint32_t i=0;i<len;i+=4u) {
        uint32_t v=0u;
        for(uint32_t b=0;b<4u && i+b<len;++b)
            v|=static_cast<uint32_t>(src[i+b])<<(8u*b);
        w[i/4u]=v;
    }
    __asm__ __volatile__("sfence; mfence" ::: "memory");
    return true;
}
// The read-back also forces every posted BAR0 write to complete before
// the bootloader command goes out.
bool verify(void *ctx,const uint8_t *src,uint32_t len) {
    volatile uint32_t *w=window(static_cast<Io *>(ctx));
    if(!w || !src || !len || len>pl::kFwPriBytes)return false;
    for(uint32_t i=0;i<pl::kFwPriBytes;i+=4u) {
        uint32_t want=0u;
        for(uint32_t b=0;b<4u && i+b<len;++b)
            want|=static_cast<uint32_t>(src[i+b])<<(8u*b);
        if(w[i/4u]!=want) {
            IOLog("Navi33Probe: psp: fw_pri read-back differs at +0x%x\n",i);
            return false;
        }
    }
    return true;
}
void sleepMs(void *,uint32_t ms) { IOSleep(ms); }
const char *stepName(uint32_t s) {
    static const char *names[]={"none","KDB","SPL","SYS_DRV","sOS"};
    return s<5u ? names[s] : "?";
}
} // namespace

n33_psp_load_result execute(IOPCIDevice *pci,const Request &req) {
    if(req.mode==N33_PSP_LOAD_RING_PLAN || req.mode==N33_PSP_LOAD_RING_RUN)
        return executeRing(pci,req);
    if(req.mode==N33_PSP_LOAD_FW_PLAN || req.mode==N33_PSP_LOAD_FW_RUN)
        return executeFw(pci,req);
    if(req.mode==N33_SMU_ENABLE_PLAN || req.mode==N33_SMU_ENABLE_RUN ||
       req.mode==N33_SMU_SCPM_PLAN || req.mode==N33_SMU_SCPM_RUN)
        return executeSmu(pci,req);
    if(req.mode==N33_MES_START_PLAN || req.mode==N33_MES_START_RUN)
        return executeMes(pci,req);
    if(req.mode==N33_MEC_QUEUE_PLAN || req.mode==N33_MEC_QUEUE_RUN)
        return executeMec(pci,req);
    if(req.mode==N33_GRADIENT_PLAN || req.mode==N33_GRADIENT_RUN)
        return executeGradient(pci,req);
    if(req.mode==N33_VM_QUEUE_PLAN || req.mode==N33_VM_QUEUE_RUN)
        return executeVm(pci,req);
    if(req.mode==N33_VM_GRADIENT_PLAN || req.mode==N33_VM_GRADIENT_RUN)
        return executeVmGradient(pci,req);
    if(!pci || (req.mode!=N33_PSP_LOAD_PLAN && req.mode!=N33_PSP_LOAD_RUN) ||
       !req.attempted)
        return pl::blank(N33_PSPL_BAD_REQUEST);
    IPDiscovery disc;
    if(!req.rom || req.romBytes<64u || req.romBytes>IPDiscovery::kMaxBlob ||
       !disc.parse(req.rom,req.romBytes))
        return pl::blank(N33_PSPL_BAD_ROM);
    n33_psp_load_result out=pl::blank(N33_PSPL_BAD_FIRMWARE);
    if(req.writeGate)out.flags|=N33_PSPL_FLAG_WRITE_GATE_OPEN;
    pl::Firmware fw{};
    if(!pl::fromContainer(req.fw,req.fwBytes,fw))return out;
    out.fw_sos_version=fw.sos.version;out.fw_kdb_bytes=fw.kdb.size;
    out.fw_spl_bytes=fw.spl.size;out.fw_sys_bytes=fw.sys.size;
    out.fw_sos_bytes=fw.sos.size;
    if(!pl::pinned(req.fw,req.fwBytes))return out;
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
    const pl::Range boot{req.consolePhys,req.consoleBytes};
    const pl::Range console=boot.phys ? boot :
        pl::hostConsole(boot,req.hostConsole,req.hostConsoleOffset,
                        req.hostConsoleBytes,bar0Range);
    out.console_phys=console.phys;out.console_bytes=console.bytes;
    const pl::Registers regs=pl::resolve(disc,bar5Bytes);
    if(!regs.valid) {out.state=N33_PSPL_BAD_ROM;return out;}

    Io io{};
    io.allowed[0]=regs.c2p35;io.allowed[1]=regs.c2p36;
    io.regs=bar5->map();
    if(!io.regs || static_cast<uint64_t>(io.regs->getLength())<bar5Bytes) {
        if(io.regs)io.regs->release();
        out.state=N33_PSPL_MAP_FAILED;return out;
    }
    pl::Snapshot snap{};
    if(!pl::readSnapshot(regs,read32,&io,snap)) {
        io.regs->release();
        out.state=N33_PSPL_READ_FAILED;return out;
    }
    const pl::Plan plan=pl::plan(snap,console,bar0Range);
    pl::report(out,snap,plan);
    out.state=plan.state;
    IOLog("Navi33Probe: psp: plan state %u C2PMSG_35 %08x C2PMSG_81 %08x "
          "FB %08x-%08x DCN FB %08x pipes 0x%x console %s vram+0x%llx "
          "(%llu KiB) fw_pri vram+0x%llx MC 0x%llx\n",
          out.state,snap.c2p35,snap.c2p81,snap.mmFbBase,snap.mmFbTop,
          snap.dcnFbBase,plan.activePipes,
          boot.phys ? "boot" : (console.phys ? "host" : "none"),
          plan.consoleOffset,console.bytes>>10,plan.fwPriOffset,plan.fwPriMc);
    if(plan.state!=N33_PSPL_PLAN_OK || req.mode==N33_PSP_LOAD_PLAN) {
        io.regs->release();return out;
    }
    if(!req.writeGate) {
        io.regs->release();out.state=N33_PSPL_WRITE_DISABLED;return out;
    }
    if(*req.attempted) {
        io.regs->release();out.state=N33_PSPL_ALREADY_ATTEMPTED;return out;
    }
    IODeviceMemory *sub=IODeviceMemory::withSubRange(bar0,plan.fwPriOffset,
                                                     pl::kFwPriBytes);
    if(sub) {
        io.win=sub->map(kIOMapInhibitCache);
        sub->release();
    }
    if(!io.win || io.win->getLength()!=pl::kFwPriBytes) {
        if(io.win)io.win->release();
        io.regs->release();
        out.state=N33_PSPL_MAP_FAILED;return out;
    }
    // From here on the PSP may hold state until a full power-off.
    *req.attempted=true;
    IOLog("Navi33Probe: psp: LOAD starts: KDB, SPL, SYS_DRV, sOS via fw_pri MC 0x%llx\n",
          plan.fwPriMc);
    pl::Hw hw{};
    hw.ctx=&io;hw.read32=read32;hw.write32=write32;
    hw.stage=stage;hw.verify=verify;hw.sleepMs=sleepMs;
    const pl::Outcome o=pl::run(plan,regs,fw,hw);
    pl::report(out,o);
    io.win->release();
    io.regs->release();
    IOLog("Navi33Probe: psp: LOAD state %u at %s, %u steps, %u register writes, "
          "C2PMSG_35 %08x C2PMSG_81 %08x sOS version %08x, %u ms\n",
          out.state,stepName(out.step),out.steps_done,out.register_writes,
          out.c2pmsg35_last,out.c2pmsg81_after,out.sos_version_after,
          out.elapsed_ms);
    return out;
}
} } // namespace navi33::darwin_psp
