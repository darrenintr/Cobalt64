// SPDX-License-Identifier: MIT
// VMID1 queue stage, IOKit side (runner: Navi33Gfx11Vmid.h). Writes only the
// runner's allowlist (GFXHUB TLB/L2/identity/context-1 registers, GRBM select,
// SCRATCH_REG0, CP_PQ_WPTR_POLL_CNTL1, the HQD block) and a 1 MiB sub-range of
// BAR0 at VRAM 251 MiB. BAR0 offset == VRAM offset is proven as for the PSP
// stages; the page table holds VRAM offsets computed from that proof.
#include "Navi33DarwinPspLoad.h"
#include "Navi33Discovery.h"
#include "Navi33Gfx11Vmid.h"
#include "Navi33Psp13Loader.h"
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/pci/IOPCIDevice.h>

namespace navi33 { namespace darwin_psp {
namespace {
namespace pl=psp13load;
namespace vm=vmid1;
// PSP 240..248 MiB, SDMA ring test 248, MEC queue 250; this window is 251 MiB.
constexpr uint64_t kProofBytes=16u*1024u*1024u, kWindowFromProof=11u*1024u*1024u;
struct Io { IOMemoryMap *regs=nullptr, *win=nullptr; vm::Regs r{}; };
bool read32(void *ctx,uint64_t off,uint32_t *out) {
    IOMemoryMap *map=static_cast<Io *>(ctx)->regs;
    if(!map || !out || (off&3u) || off>map->getLength() || map->getLength()-off<4u)return false;
    *out=*reinterpret_cast<const volatile uint32_t *>(
        static_cast<uintptr_t>(map->getVirtualAddress())+off);
    return true;
}
bool write32(void *ctx,uint64_t off,uint32_t value) {
    Io *io=static_cast<Io *>(ctx);
    if(!vm::allowed(io->r,off) || !io->regs || (off&3u) || off>io->regs->getLength() ||
       io->regs->getLength()-off<4u)return false;
    *reinterpret_cast<volatile uint32_t *>(
        static_cast<uintptr_t>(io->regs->getVirtualAddress())+off)=value;
    __asm__ __volatile__("mfence" ::: "memory");
    return true;
}
volatile uint32_t *window(Io *io,uint32_t off,uint32_t dwords) {
    const uint64_t len=4ull*dwords;
    if(!io->win || io->win->getLength()!=vm::kWindowBytes || (off&3u) ||
       off>vm::kWindowBytes || len>vm::kWindowBytes-off)return nullptr;
    return reinterpret_cast<volatile uint32_t *>(
        static_cast<uintptr_t>(io->win->getVirtualAddress())+off);
}
bool vramWrite(void *ctx,uint32_t off,const uint32_t *src,uint32_t dwords) {
    volatile uint32_t *w=window(static_cast<Io *>(ctx),off,dwords);
    if(!w || !src)return false;
    for(uint32_t i=0;i<dwords;++i)w[i]=src[i];
    __asm__ __volatile__("sfence; mfence" ::: "memory");
    return true;
}
bool vramRead(void *ctx,uint32_t off,uint32_t *dst,uint32_t dwords) {
    volatile uint32_t *w=window(static_cast<Io *>(ctx),off,dwords);
    if(!w || !dst)return false;
    for(uint32_t i=0;i<dwords;++i)dst[i]=w[i];
    return true;
}
void sleepMs(void *,uint32_t ms) { IOSleep(ms); }
} // namespace

n33_psp_load_result executeVm(IOPCIDevice *pci,const Request &req) {
    auto out=vm::blank();out.state=N33_PSPL_BAD_REQUEST;
    if(!pci || !req.attempted || (req.mode!=N33_VM_QUEUE_PLAN && req.mode!=N33_VM_QUEUE_RUN))
        return out;
    IPDiscovery disc;
    if(!req.rom || req.romBytes<64u || req.romBytes>IPDiscovery::kMaxBlob ||
       !disc.parse(req.rom,req.romBytes)) {out.state=N33_PSPL_BAD_ROM;return out;}
    IODeviceMemory *bar5=pci->getDeviceMemoryWithRegister(kIOPCIConfigBaseAddress5);
    IODeviceMemory *bar0=pci->getDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0);
    if(!bar5 || !bar0 || bar5->getLength()<4u) {out.state=N33_PSPL_NO_BARS;return out;}
    const uint64_t bar5Bytes=static_cast<uint64_t>(bar5->getLength());
    const pl::Range bar0Range{static_cast<uint64_t>(bar0->getPhysicalAddress()),
                              static_cast<uint64_t>(bar0->getLength())};
    const pl::Range boot{req.consolePhys,req.consoleBytes};
    const pl::Range console=boot.phys ? boot :
        pl::hostConsole(boot,req.hostConsole,req.hostConsoleOffset,req.hostConsoleBytes,bar0Range);
    const pl::Registers pregs=pl::resolve(disc,bar5Bytes);
    Io io{};io.r=vm::resolve(disc,bar5Bytes);
    if(!pregs.valid || !io.r.valid) {out.state=N33_PSPL_BAD_ROM;return out;}
    io.regs=bar5->map();
    if(!io.regs || static_cast<uint64_t>(io.regs->getLength())<bar5Bytes) {
        if(io.regs)io.regs->release();
        out.state=N33_PSPL_MAP_FAILED;return out;
    }
    pl::Snapshot snap{};
    if(!pl::readSnapshot(pregs,read32,&io,snap)) {
        io.regs->release();out.state=N33_PSPL_READ_FAILED;return out;
    }
    const pl::Plan win=pl::planWindow(snap,console,bar0Range,true,kProofBytes);
    if(win.state!=N33_PSPL_PLAN_OK) {
        io.regs->release();out.state=win.state;out.flags|=win.flags;return out;
    }
    const vm::Layout layout{win.fwPriMc+kWindowFromProof,win.fwPriOffset+kWindowFromProof};
    const bool run=req.mode==N33_VM_QUEUE_RUN;
    // The window is mapped only for a run that passed the plan and the gates.
    vm::Hw hw{&io,read32,nullptr,nullptr,vramRead,sleepMs};
    out=vm::execute(io.r,layout,hw,false,req.writeGate,*req.attempted);
    out.flags|=win.flags;
    if(run && out.state==N33_PSPL_PLAN_OK) {
        if(!req.writeGate) out.state=N33_PSPL_WRITE_DISABLED;
        else if(*req.attempted) out.state=N33_PSPL_ALREADY_ATTEMPTED;
        else {
            IODeviceMemory *sub=IODeviceMemory::withSubRange(bar0,win.fwPriOffset+kWindowFromProof,
                                                             vm::kWindowBytes);
            if(sub) {io.win=sub->map(kIOMapInhibitCache);sub->release();}
            if(!io.win || io.win->getLength()!=vm::kWindowBytes) {
                out.state=N33_PSPL_MAP_FAILED;
            } else {
                IOLog("Navi33Probe: VMID1 queue RUN starts, window MC 0x%llx VRAM 0x%llx\n",layout.windowMc,layout.windowVram);
                hw.write=write32;hw.vramWrite=vramWrite;
                out=vm::execute(io.r,layout,hw,true,true,*req.attempted);
                out.flags|=win.flags|N33_PSPL_FLAG_WRITE_GATE_OPEN;
            }
            if(io.win)io.win->release();
        }
    }
    io.regs->release();
    IOLog("Navi33Probe: VMID1 queue %s state %u steps %u writes %u L1 %x L2 %x ctx1 %x scratch %x "
          "fence %x rptr %x active %x fault %x/%x window 0x%x MiB ms %u\n",run ? "RUN" : "PLAN",out.state,
          out.steps_done,out.register_writes,out.vm_l1_before,out.vm_l2_before,out.vm_ctx_before,
          out.vm_scratch,out.vm_fence,out.vm_rptr,out.vm_active,out.vm_fault_status,out.vm_fault_addr,
          out.vm_window,out.elapsed_ms);
    return out;
}
} } // namespace navi33::darwin_psp
